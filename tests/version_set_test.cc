#include "db/version_set.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "log/log_writer.h"
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

  std::ifstream in(manifest_path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  // Flip a byte in the middle of data (at 1/3 of the length)
  ASSERT_GT(data.size(), 30u);
  data[data.size() / 3] ^= 0x5a;

  std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();

  VersionSet vset2(test_dir_, &options_, &icmp_);
  Status s = vset2.Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, CurrentWithoutNewlineReturnsCorruption) {
  ASSERT_TRUE(InitNewDB().ok());

  // Overwrite CURRENT without a trailing newline
  const std::string cur_path = CurrentFileName(test_dir_);
  std::ofstream out(cur_path, std::ios::binary | std::ios::trunc);
  out << "MANIFEST-000001";  // No newline!
  out.close();

  VersionSet vset(test_dir_, &options_, &icmp_);
  Status s = vset.Recover();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(VersionSetTest, CurrentNamingMissingManifestReturnsError) {
  ASSERT_TRUE(InitNewDB().ok());

  // Overwrite CURRENT to point to non-existent manifest
  const std::string cur_path = CurrentFileName(test_dir_);
  std::ofstream out(cur_path, std::ios::binary | std::ios::trunc);
  out << "MANIFEST-999999\n";
  out.close();

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

}  // namespace
}  // namespace strata
