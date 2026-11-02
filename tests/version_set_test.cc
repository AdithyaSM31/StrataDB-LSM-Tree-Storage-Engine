#include "db/version_set.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "db/builder.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {
namespace {

static Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  static constexpr size_t kBufferSize = 8192;
  char space[kBufferSize];
  while (true) {
    std::string_view fragment;
    s = file->Read(kBufferSize, &fragment, space);
    if (!s.ok()) {
      return s;
    }
    data->append(fragment.data(), fragment.size());
    if (fragment.empty()) {
      break;
    }
  }
  return Status::OK();
}

static Status WriteStringToFile(Env* env, const std::string& fname, std::string_view data) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  return s;
}

class VersionSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    static std::atomic<uint64_t> counter{0};
    test_dir_ = (std::filesystem::temp_directory_path() /
                 ("stratadb_vset_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(env_->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
                    .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
    options_.env = env_;
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  Status InitNewDB() {
    VersionEdit edit;
    edit.SetComparatorName(icmp_.Name());
    edit.SetLogNumber(0);
    edit.SetNextFile(2);
    edit.SetLastSequence(0);

    const std::string manifest = DescriptorFileName(test_dir_, 1);
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(manifest, &file);
    if (!s.ok()) {
      return s;
    }
    {
      log::Writer log(file.get());
      std::string record;
      edit.EncodeTo(&record);
      s = log.AddRecord(record);
      if (s.ok()) {
        s = file->Sync();
      }
    }
    if (s.ok()) {
      s = file->Close();
    }
    if (s.ok()) {
      s = SetCurrentFile(env_, test_dir_, 1);
    }
    return s;
  }

  FileMetaData BuildTableWithEntries(
      uint64_t file_num,
      const std::vector<std::tuple<std::string, SequenceNumber, ValueType, std::string>>& entries) {
    auto* mem = new MemTable(icmp_);
    mem->Ref();
    for (const auto& [key, seq, type, val] : entries) {
      mem->Add(seq, type, key, val);
    }
    FileMetaData meta;
    meta.number = file_num;
    std::unique_ptr<Iterator> iter = mem->NewIterator();
    Status s = BuildTable(test_dir_, env_, options_, icmp_, iter.get(), &meta);
    EXPECT_TRUE(s.ok()) << s.ToString();
    mem->Unref();
    return meta;
  }

  Env* env_ = nullptr;
  std::string test_dir_;
  Options options_;
  InternalKeyComparator icmp_;
};

TEST_F(VersionSetTest, EmptyDatabaseRecovery) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  for (int l = 0; l < kNumLevels; ++l) {
    EXPECT_EQ(vset.current()->NumFiles(l), 0);
  }
  EXPECT_EQ(vset.LogNumber(), 0u);
  EXPECT_EQ(vset.NextFileNumber(), 2u);
  EXPECT_EQ(vset.LastSequence(), 0u);
}

TEST_F(VersionSetTest, LogAndApplyAndReopen) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  VersionEdit edit;
  std::string s0 = MakeInternalKey("k0_small", 1, kTypeValue);
  std::string l0 = MakeInternalKey("k0_large", 5, kTypeValue);
  edit.AddFile(0, 10, 1000, s0, l0);

  std::string s1_1 = MakeInternalKey("a", 2, kTypeValue);
  std::string l1_1 = MakeInternalKey("m", 3, kTypeValue);
  edit.AddFile(1, 11, 2000, s1_1, l1_1);

  std::string s1_2 = MakeInternalKey("n", 4, kTypeValue);
  std::string l1_2 = MakeInternalKey("z", 6, kTypeValue);
  edit.AddFile(1, 12, 3000, s1_2, l1_2);

  edit.SetLastSequence(50);
  edit.SetLogNumber(4);

  ASSERT_TRUE(vset.LogAndApply(&edit).ok());

  EXPECT_EQ(vset.current()->NumFiles(0), 1);
  EXPECT_EQ(vset.current()->NumFiles(1), 2);
  EXPECT_EQ(vset.LastSequence(), 50u);
  EXPECT_EQ(vset.LogNumber(), 4u);

  // Fresh VersionSet recovers the same state
  VersionSet vset2(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset2.Recover().ok());

  EXPECT_EQ(vset2.current()->NumFiles(0), 1);
  EXPECT_EQ(vset2.current()->files(0)[0]->number, 10u);
  EXPECT_EQ(vset2.current()->files(0)[0]->file_size, 1000u);
  EXPECT_EQ(vset2.current()->files(0)[0]->smallest, s0);
  EXPECT_EQ(vset2.current()->files(0)[0]->largest, l0);

  EXPECT_EQ(vset2.current()->NumFiles(1), 2);
  EXPECT_EQ(vset2.current()->files(1)[0]->number, 11u);
  EXPECT_EQ(vset2.current()->files(1)[0]->smallest, s1_1);
  EXPECT_EQ(vset2.current()->files(1)[1]->number, 12u);
  EXPECT_EQ(vset2.current()->files(1)[1]->smallest, s1_2);

  EXPECT_EQ(vset2.NextFileNumber(), vset.NextFileNumber());
  EXPECT_EQ(vset2.LastSequence(), 50u);
  EXPECT_EQ(vset2.LogNumber(), 4u);
}

TEST_F(VersionSetTest, L0OrderingAndFileDeletion) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  // Add files 5, 20, 10 to L0, and file 30 to L1
  VersionEdit edit1;
  std::string k_a = MakeInternalKey("a", 1, kTypeValue);
  std::string k_b = MakeInternalKey("b", 1, kTypeValue);
  edit1.AddFile(0, 5, 100, k_a, k_b);
  edit1.AddFile(0, 20, 200, k_a, k_b);
  edit1.AddFile(0, 10, 150, k_a, k_b);
  edit1.AddFile(1, 30, 300, k_a, k_b);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  // In L0: newest first (20, 10, 5)
  ASSERT_EQ(vset.current()->NumFiles(0), 3);
  EXPECT_EQ(vset.current()->files(0)[0]->number, 20u);
  EXPECT_EQ(vset.current()->files(0)[1]->number, 10u);
  EXPECT_EQ(vset.current()->files(0)[2]->number, 5u);

  // Now delete file 10 from L0 and file 30 from L1
  VersionEdit edit2;
  edit2.RemoveFile(0, 10);
  edit2.RemoveFile(1, 30);
  ASSERT_TRUE(vset.LogAndApply(&edit2).ok());

  ASSERT_EQ(vset.current()->NumFiles(0), 2);
  EXPECT_EQ(vset.current()->files(0)[0]->number, 20u);
  EXPECT_EQ(vset.current()->files(0)[1]->number, 5u);
  EXPECT_EQ(vset.current()->NumFiles(1), 0);

  // Reopen in fresh VersionSet: verify ordering and deleted files are gone
  VersionSet vset2(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset2.Recover().ok());
  ASSERT_EQ(vset2.current()->NumFiles(0), 2);
  EXPECT_EQ(vset2.current()->files(0)[0]->number, 20u);
  EXPECT_EQ(vset2.current()->files(0)[1]->number, 5u);
  EXPECT_EQ(vset2.current()->NumFiles(1), 0);
}

TEST_F(VersionSetTest, TornLastRecordDroppedSilently) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  std::string ka = MakeInternalKey("a", 1, kTypeValue);
  std::string kz = MakeInternalKey("z", 1, kTypeValue);

  VersionEdit edit1;
  edit1.AddFile(0, 10, 100, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  VersionEdit edit2;
  edit2.AddFile(0, 20, 200, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit2).ok());

  // Find the manifest file currently pointed by CURRENT
  std::string current_content;
  ASSERT_TRUE(ReadFileToString(env_, CurrentFileName(test_dir_), &current_content).ok());
  while (!current_content.empty() &&
         (current_content.back() == '\n' || current_content.back() == '\r')) {
    current_content.pop_back();
  }
  std::string manifest_path = test_dir_ + "/" + current_content;

  uint64_t fsize = 0;
  ASSERT_TRUE(env_->GetFileSize(manifest_path, &fsize).ok());
  ASSERT_GT(fsize, 10u);

  // Truncate a few bytes off the end (e.g. 5 bytes)
  ASSERT_EQ(::truncate(manifest_path.c_str(), static_cast<off_t>(fsize - 5)), 0);

  // Recovery must succeed and drop the torn last record
  VersionSet vset2(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset2.Recover().ok());
  // File 10 should exist, file 20 should be dropped
  ASSERT_EQ(vset2.current()->NumFiles(0), 1);
  EXPECT_EQ(vset2.current()->files(0)[0]->number, 10u);
}

TEST_F(VersionSetTest, DamageInMiddleReturnsCorruption) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  std::string ka = MakeInternalKey("a", 1, kTypeValue);
  std::string kz = MakeInternalKey("z", 1, kTypeValue);

  VersionEdit edit1;
  edit1.AddFile(0, 10, 100, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  VersionEdit edit2;
  edit2.AddFile(0, 20, 200, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit2).ok());

  VersionEdit edit3;
  edit3.AddFile(0, 30, 300, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit3).ok());

  std::string current_content;
  ASSERT_TRUE(ReadFileToString(env_, CurrentFileName(test_dir_), &current_content).ok());
  while (!current_content.empty() &&
         (current_content.back() == '\n' || current_content.back() == '\r')) {
    current_content.pop_back();
  }
  std::string manifest_path = test_dir_ + "/" + current_content;

  std::string data;
  ASSERT_TRUE(ReadFileToString(env_, manifest_path, &data).ok());

  // Flip a byte in the middle of data (at 1/3 of the length)
  ASSERT_GT(data.size(), 30u);
  data[data.size() / 3] ^= 0x5a;

  ASSERT_TRUE(WriteStringToFile(env_, manifest_path, data).ok());

  VersionSet vset2(test_dir_, &options_, &icmp_);
  Status s = vset2.Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, CurrentWithoutNewlineReturnsCorruption) {
  ASSERT_TRUE(InitNewDB().ok());

  // Overwrite CURRENT without a trailing newline
  const std::string cur_path = CurrentFileName(test_dir_);
  ASSERT_TRUE(WriteStringToFile(env_, cur_path, "MANIFEST-000001").ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  Status s = vset.Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, CurrentNamingMissingManifestReturnsError) {
  ASSERT_TRUE(InitNewDB().ok());

  // Overwrite CURRENT to point to non-existent manifest
  const std::string cur_path = CurrentFileName(test_dir_);
  ASSERT_TRUE(WriteStringToFile(env_, cur_path, "MANIFEST-999999\n").ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  Status s = vset.Recover();
  EXPECT_FALSE(s.ok());
}

TEST_F(VersionSetTest, ComparatorMismatchReturnsInvalidArgument) {
  VersionEdit edit;
  edit.SetComparatorName("wrong_comparator_name");
  edit.SetLogNumber(0);
  edit.SetNextFile(2);
  edit.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(test_dir_, 1);
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_->NewWritableFile(manifest, &file).ok());
  log::Writer log(file.get());
  std::string record;
  edit.EncodeTo(&record);
  ASSERT_TRUE(log.AddRecord(record).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  ASSERT_TRUE(SetCurrentFile(env_, test_dir_, 1).ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  Status s = vset.Recover();
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(VersionSetTest, CrashBetweenWritingManifestAndRenamingCurrent) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  std::string ka = MakeInternalKey("a", 1, kTypeValue);
  std::string kz = MakeInternalKey("z", 1, kTypeValue);
  VersionEdit edit1;
  edit1.AddFile(0, 10, 100, ka, kz);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  // CURRENT currently points to MANIFEST-000002.
  // Now simulate a crash during a subsequent manifest write:
  // Create MANIFEST-000005 with edit adding file 99
  const std::string manifest5 = DescriptorFileName(test_dir_, 5);
  std::unique_ptr<WritableFile> f5;
  ASSERT_TRUE(env_->NewWritableFile(manifest5, &f5).ok());
  log::Writer w5(f5.get());
  VersionEdit edit_crashed;
  edit_crashed.SetComparatorName(icmp_.Name());
  edit_crashed.AddFile(0, 99, 999, ka, kz);
  edit_crashed.SetLogNumber(10);
  edit_crashed.SetNextFile(100);
  edit_crashed.SetLastSequence(100);
  std::string rec;
  edit_crashed.EncodeTo(&rec);
  ASSERT_TRUE(w5.AddRecord(rec).ok());
  ASSERT_TRUE(f5->Sync().ok());
  ASSERT_TRUE(f5->Close().ok());

  // Leave a .dbtmp file lying around
  const std::string tmp_file = TempFileName(test_dir_, 5);
  std::unique_ptr<WritableFile> f_tmp;
  ASSERT_TRUE(env_->NewWritableFile(tmp_file, &f_tmp).ok());
  ASSERT_TRUE(f_tmp->Append("MANIFEST-000005\n").ok());
  ASSERT_TRUE(f_tmp->Close().ok());

  // CURRENT was NOT renamed! It still points to MANIFEST-000002.
  // Fresh VersionSet recovers old state (file 10 in L0, NOT file 99)!
  VersionSet vset2(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset2.Recover().ok());
  ASSERT_EQ(vset2.current()->NumFiles(0), 1);
  EXPECT_EQ(vset2.current()->files(0)[0]->number, 10u);
}

TEST_F(VersionSetTest, CrashBetweenWritingSnapshotAndWritingEdit) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  std::string ka = MakeInternalKey("a", 1, kTypeValue);
  std::string kz = MakeInternalKey("z", 1, kTypeValue);
  VersionEdit edit1;
  edit1.AddFile(0, 10, 100, ka, kz);
  edit1.SetLogNumber(4);
  edit1.SetLastSequence(40);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  // Simulate a restart: a new VersionSet is opened and recovered.
  // descriptor_log_ is null until the first LogAndApply.
  VersionSet vset2(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset2.Recover().ok());
  ASSERT_EQ(vset2.current()->NumFiles(0), 1);
  EXPECT_EQ(vset2.LogNumber(), 4u);
  EXPECT_EQ(vset2.LastSequence(), 40u);

  // Now simulate a crash in LogAndApply after writing snapshot and renaming CURRENT (step 3),
  // but BEFORE appending the delta edit (step 4).
  const uint64_t manifest3_num = vset2.NewFileNumber();
  const std::string manifest3_path = DescriptorFileName(test_dir_, manifest3_num);
  std::unique_ptr<WritableFile> f3;
  ASSERT_TRUE(env_->NewWritableFile(manifest3_path, &f3).ok());
  log::Writer w3(f3.get());

  VersionEdit snapshot;
  snapshot.SetComparatorName(icmp_.Name());
  snapshot.SetLogNumber(vset2.LogNumber());
  snapshot.SetNextFile(vset2.NextFileNumber());
  snapshot.SetLastSequence(vset2.LastSequence());
  snapshot.AddFile(0, 10, 100, ka, kz);

  std::string snapshot_rec;
  snapshot.EncodeTo(&snapshot_rec);
  ASSERT_TRUE(w3.AddRecord(snapshot_rec).ok());
  ASSERT_TRUE(f3->Sync().ok());
  ASSERT_TRUE(f3->Close().ok());

  // Point CURRENT to MANIFEST-000003 and crash before writing the delta edit!
  ASSERT_TRUE(SetCurrentFile(env_, test_dir_, manifest3_num).ok());

  // Fresh recovery must succeed and cleanly restore the snapshot state.
  VersionSet vset3(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset3.Recover().ok());
  ASSERT_EQ(vset3.current()->NumFiles(0), 1);
  EXPECT_EQ(vset3.current()->files(0)[0]->number, 10u);
  EXPECT_EQ(vset3.LogNumber(), 4u);
  EXPECT_EQ(vset3.LastSequence(), 40u);
  EXPECT_GE(vset3.NextFileNumber(), manifest3_num + 1);
}

TEST_F(VersionSetTest, EmptyOrNewlineOnlyCurrentReturnsCorruption) {
  ASSERT_TRUE(InitNewDB().ok());

  const std::string cur_path = CurrentFileName(test_dir_);

  // Case 1: Empty file
  ASSERT_TRUE(WriteStringToFile(env_, cur_path, "").ok());
  VersionSet vset1(test_dir_, &options_, &icmp_);
  EXPECT_TRUE(vset1.Recover().IsCorruption());

  // Case 2: Only newline
  ASSERT_TRUE(WriteStringToFile(env_, cur_path, "\n").ok());
  VersionSet vset2(test_dir_, &options_, &icmp_);
  EXPECT_TRUE(vset2.Recover().IsCorruption());

  // Case 3: Only CRLF
  ASSERT_TRUE(WriteStringToFile(env_, cur_path, "\r\n").ok());
  VersionSet vset3(test_dir_, &options_, &icmp_);
  EXPECT_TRUE(vset3.Recover().IsCorruption());
}

// An Env that can fail SyncDir, or one specific Append to a MANIFEST file.
class FlakyEnv : public EnvWrapper {
 public:
  explicit FlakyEnv(Env* target) : EnvWrapper(target) {}

  bool fail_sync_dir = false;
  // Fail the Nth (0-based) Append made to any MANIFEST file, once.
  int manifest_append_to_fail = -1;

  Status SyncDir(const std::string& dir) override {
    if (fail_sync_dir) {
      return Status::IOError("injected SyncDir failure");
    }
    return target()->SyncDir(dir);
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    Status s = target()->NewWritableFile(fname, result);
    if (s.ok() && fname.find("MANIFEST") != std::string::npos) {
      *result = std::make_unique<FlakyFile>(std::move(*result), this);
    }
    return s;
  }

 private:
  class FlakyFile : public WritableFileWrapper {
   public:
    FlakyFile(std::unique_ptr<WritableFile> target, FlakyEnv* env)
        : WritableFileWrapper(std::move(target)), env_(env) {}
    Status Append(std::string_view data) override {
      if (env_->manifest_appends_++ == env_->manifest_append_to_fail) {
        env_->manifest_append_to_fail = -1;
        return Status::IOError("injected MANIFEST append failure");
      }
      return WritableFileWrapper::Append(data);
    }

   private:
    FlakyEnv* env_;
  };

  int manifest_appends_ = 0;
};

VersionEdit AddFileEdit(uint64_t number, const std::string& smallest, const std::string& largest) {
  VersionEdit edit;
  edit.AddFile(0, number, 100, MakeInternalKey(smallest, number, kTypeValue),
               MakeInternalKey(largest, number, kTypeValue));
  return edit;
}

TEST_F(VersionSetTest, FailedDirectorySyncAfterRenameKeepsDatabaseOpenable) {
  // SetCurrentFile renames CURRENT to name the new MANIFEST, then syncs the
  // directory. If only the sync fails, the new MANIFEST must not be deleted:
  // CURRENT already names it.
  ASSERT_TRUE(InitNewDB().ok());
  FlakyEnv flaky(env_);
  options_.env = &flaky;
  {
    VersionSet vset(test_dir_, &options_, &icmp_);
    ASSERT_TRUE(vset.Recover().ok());
    flaky.fail_sync_dir = true;
    VersionEdit edit = AddFileEdit(10, "a", "b");
    EXPECT_TRUE(vset.LogAndApply(&edit).IsIOError());
    EXPECT_EQ(vset.current()->NumFiles(0), 0);  // not installed

    // Once the disk recovers, the next edit succeeds and is durable.
    flaky.fail_sync_dir = false;
    VersionEdit retry = AddFileEdit(11, "c", "d");
    ASSERT_TRUE(vset.LogAndApply(&retry).ok());
  }
  VersionSet reopened(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(reopened.Recover().ok());
  ASSERT_EQ(reopened.current()->NumFiles(0), 1);
  EXPECT_EQ(reopened.current()->files(0)[0]->number, 11U);
}

TEST_F(VersionSetTest, FailedDirectorySyncThenCrashStillOpens) {
  // As above, but the process "crashes" right after the failure: whichever
  // MANIFEST CURRENT names must exist and describe the state before the edit.
  ASSERT_TRUE(InitNewDB().ok());
  FlakyEnv flaky(env_);
  options_.env = &flaky;
  {
    VersionSet vset(test_dir_, &options_, &icmp_);
    ASSERT_TRUE(vset.Recover().ok());
    flaky.fail_sync_dir = true;
    VersionEdit edit = AddFileEdit(10, "a", "b");
    EXPECT_TRUE(vset.LogAndApply(&edit).IsIOError());
  }
  flaky.fail_sync_dir = false;
  VersionSet reopened(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(reopened.Recover().ok());
  EXPECT_EQ(reopened.current()->NumFiles(0), 0);
}

TEST_F(VersionSetTest, FailedEditAppendStartsAFreshManifest) {
  // The first LogAndApply creates a MANIFEST: Appends 0-1 are the snapshot
  // record's header and payload, 2-3 the edit's. Failing Append 3 leaves just
  // the edit's header in the file. The next edit must not be appended after
  // it, or recovery would find a valid record after garbage.
  ASSERT_TRUE(InitNewDB().ok());
  FlakyEnv flaky(env_);
  options_.env = &flaky;
  {
    VersionSet vset(test_dir_, &options_, &icmp_);
    ASSERT_TRUE(vset.Recover().ok());
    flaky.manifest_append_to_fail = 3;
    VersionEdit failed = AddFileEdit(10, "a", "b");
    EXPECT_TRUE(vset.LogAndApply(&failed).IsIOError());
    const uint64_t broken_manifest = vset.ManifestFileNumber();

    VersionEdit next = AddFileEdit(11, "c", "d");
    ASSERT_TRUE(vset.LogAndApply(&next).ok());
    EXPECT_NE(vset.ManifestFileNumber(), broken_manifest);  // a fresh MANIFEST
  }
  VersionSet reopened(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(reopened.Recover().ok());
  ASSERT_EQ(reopened.current()->NumFiles(0), 1);
  EXPECT_EQ(reopened.current()->files(0)[0]->number, 11U);
}

TEST_F(VersionSetTest, FailedEditAppendOnAnOpenManifest) {
  // The same failure on a MANIFEST already in use: Appends 0-3 belong to the
  // first (successful) LogAndApply, 4-5 to the second, whose payload fails.
  ASSERT_TRUE(InitNewDB().ok());
  FlakyEnv flaky(env_);
  options_.env = &flaky;
  {
    VersionSet vset(test_dir_, &options_, &icmp_);
    ASSERT_TRUE(vset.Recover().ok());
    VersionEdit first = AddFileEdit(10, "a", "b");
    ASSERT_TRUE(vset.LogAndApply(&first).ok());

    flaky.manifest_append_to_fail = 5;
    VersionEdit failed = AddFileEdit(11, "c", "d");
    EXPECT_TRUE(vset.LogAndApply(&failed).IsIOError());

    VersionEdit third = AddFileEdit(12, "e", "f");
    ASSERT_TRUE(vset.LogAndApply(&third).ok());
  }
  VersionSet reopened(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(reopened.Recover().ok());
  ASSERT_EQ(reopened.current()->NumFiles(0), 2);
  // L0 is newest first.
  EXPECT_EQ(reopened.current()->files(0)[0]->number, 12U);
  EXPECT_EQ(reopened.current()->files(0)[1]->number, 10U);
}

TEST_F(VersionSetTest, VersionGetL1BinarySearch) {
  ASSERT_TRUE(InitNewDB().ok());

  FileMetaData meta1 = BuildTableWithEntries(100, {{"a", 10, kTypeValue, "val_a"},
                                                   {"b", 10, kTypeValue, "val_b"},
                                                   {"c", 10, kTypeValue, "val_c"}});
  FileMetaData meta2 = BuildTableWithEntries(101, {{"e", 10, kTypeValue, "val_e"},
                                                   {"f", 10, kTypeValue, "val_f"},
                                                   {"g", 10, kTypeValue, "val_g"}});
  FileMetaData meta3 = BuildTableWithEntries(102, {{"p", 10, kTypeValue, "val_p"},
                                                   {"q", 10, kTypeDeletion, ""},
                                                   {"z", 10, kTypeValue, "val_z"}});

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  VersionEdit edit;
  edit.AddFile(1, meta1.number, meta1.file_size, meta1.smallest, meta1.largest);
  edit.AddFile(1, meta2.number, meta2.file_size, meta2.smallest, meta2.largest);
  edit.AddFile(1, meta3.number, meta3.file_size, meta3.smallest, meta3.largest);
  ASSERT_TRUE(vset.LogAndApply(&edit).ok());

  TableCache cache(test_dir_, options_, &icmp_);
  std::string val;

  // Key in first table
  Status s = vset.current()->Get(LookupKey("b", 10), &val, &cache);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(val, "val_b");

  // Key before first table
  s = vset.current()->Get(LookupKey("0", 10), &val, &cache);
  EXPECT_TRUE(s.IsNotFound());

  // Key in gap between table 1 and table 2
  s = vset.current()->Get(LookupKey("d", 10), &val, &cache);
  EXPECT_TRUE(s.IsNotFound());

  // Key in middle table
  s = vset.current()->Get(LookupKey("f", 10), &val, &cache);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(val, "val_f");

  // Deleted key (tombstone) in third table
  s = vset.current()->Get(LookupKey("q", 10), &val, &cache);
  EXPECT_TRUE(s.IsNotFound());

  // Key in third table
  s = vset.current()->Get(LookupKey("z", 10), &val, &cache);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(val, "val_z");

  // Key after third table
  s = vset.current()->Get(LookupKey("zzz", 10), &val, &cache);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(VersionSetTest, VersionGetMultiLevelHiding) {
  ASSERT_TRUE(InitNewDB().ok());

  // Level 2 table
  FileMetaData meta_l2 = BuildTableWithEntries(200, {{"k1", 5, kTypeValue, "v1_l2"},
                                                     {"k2", 5, kTypeValue, "v2_l2"},
                                                     {"k3", 5, kTypeValue, "v3_l2"}});

  // Level 1 table
  FileMetaData meta_l1 =
      BuildTableWithEntries(201, {{"k1", 10, kTypeValue, "v1_l1"}, {"k2", 10, kTypeDeletion, ""}});

  // Level 0 table
  FileMetaData meta_l0 = BuildTableWithEntries(202, {{"k3", 15, kTypeValue, "v3_l0"}});

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  VersionEdit edit;
  edit.AddFile(2, meta_l2.number, meta_l2.file_size, meta_l2.smallest, meta_l2.largest);
  edit.AddFile(1, meta_l1.number, meta_l1.file_size, meta_l1.smallest, meta_l1.largest);
  edit.AddFile(0, meta_l0.number, meta_l0.file_size, meta_l0.smallest, meta_l0.largest);
  ASSERT_TRUE(vset.LogAndApply(&edit).ok());

  TableCache cache(test_dir_, options_, &icmp_);
  std::string val;

  // k1: L1 value hides L2 value
  Status s = vset.current()->Get(LookupKey("k1", 20), &val, &cache);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(val, "v1_l1");

  // k2: L1 tombstone hides L2 value
  s = vset.current()->Get(LookupKey("k2", 20), &val, &cache);
  EXPECT_TRUE(s.IsNotFound());

  // k3: L0 value hides L2 value
  s = vset.current()->Get(LookupKey("k3", 20), &val, &cache);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(val, "v3_l0");
}

TEST_F(VersionSetTest, AddLiveFilesAndNumLevelFiles) {
  ASSERT_TRUE(InitNewDB().ok());

  VersionSet vset(test_dir_, &options_, &icmp_);
  ASSERT_TRUE(vset.Recover().ok());

  VersionEdit edit1;
  std::string s_a = MakeInternalKey("a", 1, kTypeValue);
  std::string l_b = MakeInternalKey("b", 1, kTypeValue);
  edit1.AddFile(0, 10, 100, s_a, l_b);
  edit1.AddFile(1, 20, 200, s_a, l_b);
  edit1.AddFile(2, 30, 300, s_a, l_b);
  ASSERT_TRUE(vset.LogAndApply(&edit1).ok());

  EXPECT_EQ(vset.NumLevelFiles(0), 1);
  EXPECT_EQ(vset.NumLevelFiles(1), 1);
  EXPECT_EQ(vset.NumLevelFiles(2), 1);
  EXPECT_EQ(vset.NumLevelFiles(3), 0);

  EXPECT_EQ(vset.current()->NumLevelFiles(0), 1);
  EXPECT_EQ(vset.current()->NumLevelFiles(1), 1);
  EXPECT_EQ(vset.current()->NumLevelFiles(2), 1);

  std::set<uint64_t> live;
  vset.AddLiveFiles(&live);
  EXPECT_EQ(live.size(), 3u);
  EXPECT_TRUE(live.count(10));
  EXPECT_TRUE(live.count(20));
  EXPECT_TRUE(live.count(30));

  // Pin current version
  Version* v1 = vset.current();
  v1->Ref();

  // Apply a new edit deleting file 10 and adding file 11
  VersionEdit edit2;
  edit2.RemoveFile(0, 10);
  edit2.AddFile(0, 11, 100, s_a, l_b);
  ASSERT_TRUE(vset.LogAndApply(&edit2).ok());

  // v1 is still pinned, so AddLiveFiles includes files from both versions: 10, 11, 20, 30
  live.clear();
  vset.AddLiveFiles(&live);
  EXPECT_EQ(live.size(), 4u);
  EXPECT_TRUE(live.count(10));
  EXPECT_TRUE(live.count(11));
  EXPECT_TRUE(live.count(20));
  EXPECT_TRUE(live.count(30));

  v1->Unref();
  // Now v1 is destroyed, so 10 is no longer live
  live.clear();
  vset.AddLiveFiles(&live);
  EXPECT_EQ(live.size(), 3u);
  EXPECT_FALSE(live.count(10));
  EXPECT_TRUE(live.count(11));
}

}  // namespace
}  // namespace strata
