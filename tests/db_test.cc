#include "strata/db.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "log/log_writer.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"
#include "table/table.h"
#include "util/comparator.h"

namespace strata {
namespace {

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    static std::atomic<uint64_t> test_counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_db_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(env_->NowMicros()) + "_" + std::to_string(test_counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    Close();
    custom_env_.reset();
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  Status Open(Options options = Options()) {
    Close();
    options_ = options;
    return DB::Open(options_, test_dir_, &db_);
  }

  Status Reopen(Options options = Options()) {
    Close();
    options_ = options;
    return DB::Open(options_, test_dir_, &db_);
  }

  void Close() { db_.reset(); }

  Status Put(std::string_view key, std::string_view value, WriteOptions wo = WriteOptions()) {
    if (!db_) {
      return Status::InvalidArgument("db is closed");
    }
    return db_->Put(wo, key, value);
  }

  Status Delete(std::string_view key, WriteOptions wo = WriteOptions()) {
    if (!db_) {
      return Status::InvalidArgument("db is closed");
    }
    return db_->Delete(wo, key);
  }

  std::string Get(std::string_view key, Status* status = nullptr, ReadOptions ro = ReadOptions()) {
    if (!db_) {
      if (status != nullptr) {
        *status = Status::InvalidArgument("db is closed");
      }
      return "";
    }
    std::string val;
    Status s = db_->Get(ro, key, &val);
    if (status != nullptr) {
      *status = s;
    }
    return s.ok() ? val : "";
  }

  // Whole-file read and write through our own Env. (iostreams are avoided on
  // purpose: GCC 13 at -O2 reports a false -Wnull-dereference inside
  // libstdc++'s streambuf, which -Werror turns into a build failure.)
  std::string ReadFileToString(const std::string& path) {
    uint64_t size = 0;
    EXPECT_TRUE(env_->GetFileSize(path, &size).ok());
    std::unique_ptr<SequentialFile> file;
    EXPECT_TRUE(env_->NewSequentialFile(path, &file).ok());
    if (file == nullptr) {
      return "";
    }
    std::string scratch(static_cast<size_t>(size), '\0');
    std::string_view result;
    EXPECT_TRUE(file->Read(scratch.size(), &result, scratch.data()).ok());
    return std::string(result);
  }

  void WriteStringToFile(const std::string& path, std::string_view data) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(env_->NewWritableFile(path, &file).ok());
    ASSERT_TRUE(file->Append(data).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  std::string GetNewestLogFile() {
    std::vector<std::string> children;
    Status s = env_->GetChildren(test_dir_, &children);
    if (!s.ok()) {
      return "";
    }
    std::vector<uint64_t> logs;
    for (const auto& name : children) {
      uint64_t num;
      FileType type;
      if (ParseFileName(name, &num, &type) && type == FileType::kLogFile) {
        logs.push_back(num);
      }
    }
    if (logs.empty()) {
      return "";
    }
    std::sort(logs.begin(), logs.end());
    return LogFileName(test_dir_, logs.back());
  }

  int CountFiles(FileType target_type) {
    std::vector<std::string> children;
    Status s = env_->GetChildren(test_dir_, &children);
    if (!s.ok()) {
      return 0;
    }
    int count = 0;
    for (const auto& name : children) {
      uint64_t num;
      FileType type;
      if (ParseFileName(name, &num, &type) && type == target_type) {
        ++count;
      }
    }
    return count;
  }

  DBImpl* dbfull() { return static_cast<DBImpl*>(db_.get()); }

  std::vector<std::string> GetTableFiles() {
    std::vector<std::string> children;
    Status s = env_->GetChildren(test_dir_, &children);
    if (!s.ok()) {
      return {};
    }
    std::vector<std::string> tables;
    for (const auto& name : children) {
      uint64_t num;
      FileType type;
      if (ParseFileName(name, &num, &type) && type == FileType::kTableFile) {
        tables.push_back(test_dir_ + "/" + name);
      }
    }
    return tables;
  }

  int CountEntriesInTable(const std::string& path) {
    uint64_t size = 0;
    Status s = env_->GetFileSize(path, &size);
    if (!s.ok()) {
      return -1;
    }
    std::unique_ptr<RandomAccessFile> file;
    s = env_->NewRandomAccessFile(path, &file);
    if (!s.ok()) {
      return -1;
    }
    std::unique_ptr<Table> table;
    InternalKeyComparator icmp;
    s = Table::Open(options_, &icmp, std::move(file), size, &table);
    if (!s.ok()) {
      return -1;
    }
    int count = 0;
    std::unique_ptr<Iterator> it = table->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++count;
    }
    return count;
  }

  Env* env_ = nullptr;
  std::unique_ptr<Env> custom_env_;
  std::string test_dir_;
  Options options_;
  std::unique_ptr<DB> db_;
};

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------

TEST_F(DBTest, EmptyDatabaseReturnsNotFound) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  Status s;
  EXPECT_EQ(Get("non_existent_key", &s), "");
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, PutGetOverwriteDelete) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("key1", "val1").ok());
  EXPECT_EQ(Get("key1"), "val1");

  ASSERT_TRUE(Put("key1", "val2").ok());
  EXPECT_EQ(Get("key1"), "val2");

  ASSERT_TRUE(Delete("key1").ok());
  Status s;
  Get("key1", &s);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, BatchWithTwoPutsSameKeySecondWins) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  WriteBatch batch;
  batch.Put("k", "v1");
  batch.Put("k", "v2");
  ASSERT_TRUE(db_->Write(WriteOptions(), &batch).ok());

  EXPECT_EQ(Get("k"), "v2");
}

TEST_F(DBTest, LargeValueSpanningBlocks100KB) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  const std::string large(size_t{100} * 1024, 'x');
  ASSERT_TRUE(Put("large_key", large).ok());
  EXPECT_EQ(Get("large_key"), large);
}

// ---------------------------------------------------------------------------
// Restarts, the milestone itself
// ---------------------------------------------------------------------------

TEST_F(DBTest, SurviveRestart) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(Put("k2", "v2").ok());
  ASSERT_TRUE(Put("k3", "v3").ok());
  ASSERT_TRUE(Delete("k2").ok());

  Close();

  opts.create_if_missing = false;
  ASSERT_TRUE(Reopen(opts).ok());

  EXPECT_EQ(Get("k1"), "v1");
  EXPECT_EQ(Get("k3"), "v3");

  Status s;
  Get("k2", &s);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, RecoverLastSequenceNumberAcrossReopen) {
  // Test for mistake 1: write k=v1, reopen, write k=v2, reopen:
  // The answer must be v2. If last_sequence_ isn't recovered,
  // v2 gets a lower sequence number than v1, and the old value wins.
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "v1").ok());
  Close();

  ASSERT_TRUE(Reopen(opts).ok());
  ASSERT_TRUE(Put("k", "v2").ok());
  Close();

  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(Get("k"), "v2");
}

TEST_F(DBTest, FiveReopenCycles) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
    Close();
    ASSERT_TRUE(Reopen(opts).ok());
  }

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }

  // Verify that obsolete log files were deleted (the Week 1 pile-up ends).
  std::vector<std::string> children;
  ASSERT_TRUE(env_->GetChildren(test_dir_, &children).ok());
  int log_count = 0;
  for (const auto& name : children) {
    uint64_t num;
    FileType type;
    if (ParseFileName(name, &num, &type) && type == FileType::kLogFile) {
      log_count++;
    }
  }
  // With obsolete files deleted at the end of Open, only 1 active log file remains.
  EXPECT_EQ(log_count, 1);
}

// ---------------------------------------------------------------------------
// Crashes and corruption
// ---------------------------------------------------------------------------

TEST_F(DBTest, TornTailAtEndOfLastLog) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(Put("k2", "v2").ok());
  ASSERT_TRUE(Put("k3", "v3_longer_value_data").ok());

  std::string newest_log = GetNewestLogFile();
  ASSERT_FALSE(newest_log.empty());

  Close();

  uint64_t file_size = 0;
  ASSERT_TRUE(env_->GetFileSize(newest_log, &file_size).ok());
  ASSERT_GT(file_size, 5u);

  // Cut a few bytes off the end of the last log
  ASSERT_EQ(::truncate(newest_log.c_str(), static_cast<off_t>(file_size - 5)), 0);

  // Reopen should succeed and earlier writes must be there!
  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(Get("k1"), "v1");
  EXPECT_EQ(Get("k2"), "v2");
}

TEST_F(DBTest, DamageInMiddleReturnsCorruption) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(Put("k2", "v2").ok());
  ASSERT_TRUE(Put("k3", "v3").ok());

  std::string newest_log = GetNewestLogFile();
  ASSERT_FALSE(newest_log.empty());

  Close();

  // Read log file contents
  std::string log_bytes = ReadFileToString(newest_log);

  // Damage in the middle: flip a byte inside a record that has more records after it.
  // Record 1 is at 0, Record 2 starts after Record 1 (around byte 20-30).
  // Flipping a byte at offset 25 corrupts record 2 while record 3 follows it.
  ASSERT_GT(log_bytes.size(), 30u);
  log_bytes[25] ^= 0x42;

  WriteStringToFile(newest_log, log_bytes);

  // Open must return Corruption!
  opts.paranoid_checks = true;
  Status s = Reopen(opts);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, ZeroedMiddleBlockReturnsCorruption) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Write three distinct records, each ~30 KB, so the log spans across at least 3 blocks.
  const std::string chunk(size_t{30} * 1024, 'a');
  ASSERT_TRUE(Put("k1", chunk).ok());
  ASSERT_TRUE(Put("k2", chunk).ok());
  ASSERT_TRUE(Put("k3", chunk).ok());

  std::string newest_log = GetNewestLogFile();
  ASSERT_FALSE(newest_log.empty());

  Close();

  uint64_t file_size = 0;
  ASSERT_TRUE(env_->GetFileSize(newest_log, &file_size).ok());
  ASSERT_GT(file_size, 2u * 32768);

  // Read full log
  std::string log_bytes = ReadFileToString(newest_log);

  // Zero out the entire second block (bytes 32768 to 65535)
  for (size_t i = 32768; i < 65536 && i < log_bytes.size(); ++i) {
    log_bytes[i] = '\0';
  }

  WriteStringToFile(newest_log, log_bytes);

  // Open must return Corruption!
  opts.paranoid_checks = true;
  Status s = Reopen(opts);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, RealProcessCrashSurvivesUnsyncedWrites) {
  // A real process crash: fork a child process that opens the database,
  // makes 1,000 Puts with sync = false, and then exits with _exit without closing.
  // The parent reopens and all 1,000 must be there.
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);

  if (pid == 0) {
    Options opts;
    opts.create_if_missing = true;
    std::unique_ptr<DB> child_db;
    Status s = DB::Open(opts, test_dir_, &child_db);
    if (!s.ok()) {
      ::_exit(1);
    }
    WriteOptions wo;
    wo.sync = false;
    for (int i = 0; i < 1000; ++i) {
      std::string k = "k" + std::to_string(i);
      std::string v = "v" + std::to_string(i);
      s = child_db->Put(wo, k, v);
      if (!s.ok()) {
        ::_exit(2);
      }
    }
    // Ungraceful process crash: no destructors run, no clean Close.
    ::_exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  // Parent reopens the database.
  Options opts;
  opts.create_if_missing = false;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 1000; ++i) {
    std::string k = "k" + std::to_string(i);
    std::string expected_v = "v" + std::to_string(i);
    Status s;
    std::string v = Get(k, &s);
    ASSERT_TRUE(s.ok()) << "Missing key: " << k;
    ASSERT_EQ(v, expected_v);
  }
}

// ---------------------------------------------------------------------------
// Locks and errors
// ---------------------------------------------------------------------------

TEST_F(DBTest, SecondOpenFailsWhileFirstIsOpen) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  std::unique_ptr<DB> second_db;
  Status s = DB::Open(opts, test_dir_, &second_db);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError());

  Close();

  s = DB::Open(opts, test_dir_, &second_db);
  EXPECT_TRUE(s.ok()) << s.ToString();
}

TEST_F(DBTest, FailedOpenReleasesLock) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "v").ok());
  std::string newest_log = GetNewestLogFile();
  Close();

  // Read log
  const std::string orig_bytes = ReadFileToString(newest_log);

  // Corrupt log
  std::string corrupted = orig_bytes;
  corrupted[0] = static_cast<char>(static_cast<uint8_t>(corrupted[0]) ^ 0xFFU);
  WriteStringToFile(newest_log, corrupted);

  // Open should fail due to corruption
  opts.paranoid_checks = true;
  Status s = Reopen(opts);
  EXPECT_TRUE(s.IsCorruption());

  // Repair the log
  WriteStringToFile(newest_log, orig_bytes);

  // Next open must succeed and not report "lock held"
  s = Reopen(opts);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(Get("k"), "v");
}

namespace {

class FailingWritableFile : public WritableFileWrapper {
 public:
  FailingWritableFile(std::unique_ptr<WritableFile> target, int fail_after)
      : WritableFileWrapper(std::move(target)), fail_after_(fail_after) {}

  Status Append(std::string_view data) override {
    if (appends_++ >= fail_after_) {
      return Status::IOError("injected append failure");
    }
    return WritableFileWrapper::Append(data);
  }

 private:
  int fail_after_;
  int appends_ = 0;
};

class FailingEnv : public EnvWrapper {
 public:
  FailingEnv(Env* target, int fail_after) : EnvWrapper(target), fail_after_(fail_after) {}

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    std::unique_ptr<WritableFile> base_file;
    Status s = EnvWrapper::NewWritableFile(fname, &base_file);
    if (!s.ok()) {
      return s;
    }
    *result = std::make_unique<FailingWritableFile>(std::move(base_file), fail_after_);
    return Status::OK();
  }

 private:
  int fail_after_;
};

}  // namespace

TEST_F(DBTest, StickyErrorsAfterFailedLogWrite) {
  // Wrap Env::Default() and fail Append after 2 successful appends (one complete record).
  custom_env_ = std::make_unique<FailingEnv>(Env::Default(), /*fail_after=*/2);
  Options opts;
  opts.create_if_missing = true;
  opts.env = custom_env_.get();

  ASSERT_TRUE(Open(opts).ok());

  // First Put succeeds (uses appends 0 and 1)
  ASSERT_TRUE(Put("k1", "v1").ok());

  // Second Put fails due to injected error on append 2
  Status s2 = Put("k2", "v2");
  EXPECT_FALSE(s2.ok());
  EXPECT_TRUE(s2.IsIOError());

  // Every subsequent Put must return the same sticky error
  Status s3 = Put("k3", "v3");
  EXPECT_FALSE(s3.ok());
  EXPECT_TRUE(s3.IsIOError());
  EXPECT_EQ(s3.ToString(), s2.ToString());

  // The database stays readable: k1 is still retrievable
  Status gs;
  std::string v = Get("k1", &gs);
  EXPECT_TRUE(gs.ok());
  EXPECT_EQ(v, "v1");

  Close();
  custom_env_.reset();
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST_F(DBTest, ConcurrentFourThreadsPutsAndGets) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  constexpr int kNumThreads = 4;
  constexpr int kOpsPerThread = 250;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([this, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string k = "t" + std::to_string(t) + "_k" + std::to_string(i);
        std::string v = "v" + std::to_string(i);
        Status ps = Put(k, v);
        EXPECT_TRUE(ps.ok());

        Status gs;
        std::string gv = Get(k, &gs);
        EXPECT_TRUE(gs.ok());
        EXPECT_EQ(gv, v);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // All keys are intact
  for (int t = 0; t < kNumThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string k = "t" + std::to_string(t) + "_k" + std::to_string(i);
      std::string expected_v = "v" + std::to_string(i);
      EXPECT_EQ(Get(k), expected_v);
    }
  }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

TEST_F(DBTest, SnapshotIsolation) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "v1").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_NE(snap, nullptr);

  ASSERT_TRUE(Put("k", "v2").ok());

  ReadOptions ro;
  ro.snapshot = snap;
  EXPECT_EQ(Get("k", nullptr, ro), "v1");
  EXPECT_EQ(Get("k"), "v2");

  db_->ReleaseSnapshot(snap);
}

// ---------------------------------------------------------------------------
// Options flags: create_if_missing & error_if_exists
// ---------------------------------------------------------------------------

TEST_F(DBTest, CreateIfMissingFalseFailsOnNonExistentDir) {
  const std::string non_existent = (std::filesystem::path(test_dir_) / "missing_subdir").string();
  Options opts;
  opts.create_if_missing = false;
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, non_existent, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBTest, ErrorIfExistsTrueFailsOnExistingDB) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());
  Close();

  opts.error_if_exists = true;
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, test_dir_, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

// ---------------------------------------------------------------------------
// Unimplemented methods return NotSupported / defaults
// ---------------------------------------------------------------------------

TEST_F(DBTest, FutureMethodsReturnNotSupported) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  EXPECT_TRUE(db_->Flush().ok());
  EXPECT_TRUE(db_->CompactRange("a", "z").ok());
  std::unique_ptr<Iterator> it = db_->NewIterator(ReadOptions());
  ASSERT_NE(it, nullptr);
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());

  std::string prop;
  EXPECT_FALSE(db_->GetProperty("strata.stats", &prop));
}

// ---------------------------------------------------------------------------
// Additional edge cases: empty dirs, numeric replay, empty keys, overflow
// ---------------------------------------------------------------------------

TEST_F(DBTest, CreateIfMissingFalseOnExistingEmptyDirFails) {
  // test_dir_ exists as an empty directory (created in SetUp()).
  // With create_if_missing = false, it must fail because no DB exists.
  Options opts;
  opts.create_if_missing = false;
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, test_dir_, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBTest, ErrorIfExistsOnExistingEmptyDirSucceeds) {
  // test_dir_ exists as an empty directory with no database.
  // With error_if_exists = true and create_if_missing = true,
  // it should succeed because no database exists there yet.
  Options opts;
  opts.create_if_missing = true;
  opts.error_if_exists = true;
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, test_dir_, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_NE(db, nullptr);
}

TEST_F(DBTest, ReplayLogsInNumericOrderNotAlphabetical) {
  // Create log 2 with ("k", "v2") and log 10 with ("k", "v10").
  // Alphabetically, "000010.log" sorts before "000002.log".
  // Numerically, 2 < 10. Replay must happen in numeric order (2, then 10),
  // so the final value for "k" must be "v10".
  const std::string log2_path = LogFileName(test_dir_, 2);
  const std::string log10_path = LogFileName(test_dir_, 10);

  std::unique_ptr<WritableFile> f2;
  ASSERT_TRUE(env_->NewWritableFile(log2_path, &f2).ok());
  log::Writer w2(f2.get());
  WriteBatch b2;
  WriteBatchInternal::SetSequence(&b2, 100);
  b2.Put("k", "v2");
  ASSERT_TRUE(w2.AddRecord(WriteBatchInternal::Contents(&b2)).ok());
  ASSERT_TRUE(f2->Close().ok());

  std::unique_ptr<WritableFile> f10;
  ASSERT_TRUE(env_->NewWritableFile(log10_path, &f10).ok());
  log::Writer w10(f10.get());
  WriteBatch b10;
  WriteBatchInternal::SetSequence(&b10, 200);
  b10.Put("k", "v10");
  ASSERT_TRUE(w10.AddRecord(WriteBatchInternal::Contents(&b10)).ok());
  ASSERT_TRUE(f10->Close().ok());

  Options opts;
  opts.create_if_missing = false;
  ASSERT_TRUE(Open(opts).ok());
  EXPECT_EQ(Get("k"), "v10");

  // A subsequent Put gets sequence > 200:
  ASSERT_TRUE(Put("k", "v11").ok());
  Close();
  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(Get("k"), "v11");
}

TEST_F(DBTest, EmptyKeyAndEmptyValue) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("", "empty_key_val").ok());
  ASSERT_TRUE(Put("non_empty_key", "").ok());
  ASSERT_TRUE(Put("", "").ok());

  EXPECT_EQ(Get(""), "");
  EXPECT_EQ(Get("non_empty_key"), "");

  // Reopen and verify persistence
  Close();
  opts.create_if_missing = false;
  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(Get(""), "");
  EXPECT_EQ(Get("non_empty_key"), "");

  ASSERT_TRUE(Delete("").ok());
  Status s;
  Get("", &s);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, SequenceNumberOverflowInLogFailsWithCorruption) {
  // A corrupted log record with sequence number > kMaxSequenceNumber
  // must report corruption and fail Open cleanly without aborting.
  const std::string log1_path = LogFileName(test_dir_, 1);
  std::unique_ptr<WritableFile> f1;
  ASSERT_TRUE(env_->NewWritableFile(log1_path, &f1).ok());
  log::Writer w1(f1.get());
  WriteBatch b1;
  b1.Put("k", "v");
  // Set sequence to an invalid value > kMaxSequenceNumber
  WriteBatchInternal::SetSequence(&b1, kMaxSequenceNumber + 100);
  ASSERT_TRUE(w1.AddRecord(WriteBatchInternal::Contents(&b1)).ok());
  ASSERT_TRUE(f1->Close().ok());

  Options opts;
  opts.create_if_missing = false;
  opts.paranoid_checks = true;
  Status s = Open(opts);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, GetNullValuePointerReturnsInvalidArgument) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());
  Status s = db_->Get(ReadOptions(), "some_key", nullptr);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(DBTest, ReleaseSnapshotSafety) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Calling ReleaseSnapshot with nullptr is a safe no-op:
  db_->ReleaseSnapshot(nullptr);

  const Snapshot* snap1 = db_->GetSnapshot();
  ASSERT_NE(snap1, nullptr);

  const Snapshot* snap2 = db_->GetSnapshot();
  ASSERT_NE(snap2, nullptr);

  // Release snapshots in reverse and normal order safely
  db_->ReleaseSnapshot(snap1);
  db_->ReleaseSnapshot(snap2);
}

TEST_F(DBTest, OpenCreatesCurrentAndManifest) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Verify CURRENT exists
  const std::string cur_path = CurrentFileName(test_dir_);
  EXPECT_TRUE(env_->FileExists(cur_path));

  // Verify MANIFEST-000001 exists
  const std::string manifest_path = DescriptorFileName(test_dir_, 1);
  EXPECT_TRUE(env_->FileExists(manifest_path));

  // Read CURRENT and verify contents
  std::unique_ptr<SequentialFile> cur_file;
  ASSERT_TRUE(env_->NewSequentialFile(cur_path, &cur_file).ok());
  char scratch[100];
  std::string_view cur_content;
  ASSERT_TRUE(cur_file->Read(sizeof(scratch), &cur_content, scratch).ok());
  EXPECT_EQ(cur_content, "MANIFEST-000001\n");
}

// ---------------------------------------------------------------------------
// Week 2: Flush, L0 SSTables, Reading across MemTable/Imm/L0
// ---------------------------------------------------------------------------

TEST_F(DBTest, MilestoneFlushAcrossReopen) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;  // the minimum: ~200 KB of data flushes a few times
  ASSERT_TRUE(Open(opts).ok());

  // Write 2,000 keys of 100 bytes each
  const std::string val_100(100, 'x');
  for (int i = 0; i < 2000; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "k%05d", i);
    ASSERT_TRUE(Put(key_buf, val_100).ok());
  }

  // Several .sst files must appear
  EXPECT_GT(CountFiles(FileType::kTableFile), 1);

  // Every key reads back correctly
  for (int i = 0; i < 2000; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "k%05d", i);
    EXPECT_EQ(Get(key_buf), val_100);
  }

  // Close and reopen
  opts.create_if_missing = false;
  ASSERT_TRUE(Reopen(opts).ok());

  // Still reads back correctly after reopen
  for (int i = 0; i < 2000; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "k%05d", i);
    EXPECT_EQ(Get(key_buf), val_100);
  }
}

TEST_F(DBTest, FlushOperation) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("hello", "world").ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(Get("hello"), "world");

  // An .sst file exists
  EXPECT_GE(CountFiles(FileType::kTableFile), 1);

  // And old log is gone: exactly one log file remains (the new active log)
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
}

TEST_F(DBTest, NewerDataHidesOlderDataAcrossLayers) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "v1").ok());
  ASSERT_TRUE(db_->Flush().ok());

  ASSERT_TRUE(Put("k", "v2").ok());
  // v2 from active memtable
  EXPECT_EQ(Get("k"), "v2");

  ASSERT_TRUE(db_->Flush().ok());
  // still v2, from the newer L0 file
  EXPECT_EQ(Get("k"), "v2");
}

TEST_F(DBTest, TombstoneHidesAcrossFiles) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "value_to_delete").ok());
  ASSERT_TRUE(db_->Flush().ok());

  ASSERT_TRUE(Delete("k").ok());
  ASSERT_TRUE(db_->Flush().ok());

  Status s;
  Get("k", &s);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

TEST_F(DBTest, SnapshotsAcrossFlush) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k", "v1").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_NE(snap, nullptr);

  ASSERT_TRUE(Put("k", "v2").ok());
  ASSERT_TRUE(db_->Flush().ok());

  // Reading at snapshot returns v1
  ReadOptions ropts;
  ropts.snapshot = snap;
  Status s;
  EXPECT_EQ(Get("k", &s, ropts), "v1");
  EXPECT_TRUE(s.ok());

  // Reading without snapshot returns v2
  EXPECT_EQ(Get("k"), "v2");

  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, RecoveryFlush) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 50; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "rec_key_%02d", i);
    ASSERT_TRUE(Put(key_buf, "val").ok());
  }

  // Close WITHOUT calling Flush()
  Close();

  // Reopen
  opts.create_if_missing = false;
  ASSERT_TRUE(Reopen(opts).ok());

  // Data is now in L0, and only one log and one MANIFEST remain
  EXPECT_GE(CountFiles(FileType::kTableFile), 1);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
  EXPECT_EQ(CountFiles(FileType::kDescriptorFile), 1);

  for (int i = 0; i < 50; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "rec_key_%02d", i);
    EXPECT_EQ(Get(key_buf), "val");
  }
}

TEST_F(DBTest, Week1TrapFixedCorruptionPolicy) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(Put("k2", "v2").ok());
  ASSERT_TRUE(Put("k3", "v3").ok());

  std::string newest_log = GetNewestLogFile();
  ASSERT_FALSE(newest_log.empty());

  Close();

  std::string log_bytes = ReadFileToString(newest_log);
  ASSERT_GT(log_bytes.size(), 40u);
  // Damage record 2 in the middle (record 1 is [0..25], record 2 starts at 26)
  log_bytes[35] ^= 0x42;
  WriteStringToFile(newest_log, log_bytes);

  // With paranoid_checks = true: fail Open with Corruption
  Options paranoid_opts = opts;
  paranoid_opts.create_if_missing = false;
  paranoid_opts.paranoid_checks = true;
  Status s = Reopen(paranoid_opts);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  // With paranoid_checks = false (default): keep prefix and succeed Open
  Options non_paranoid_opts = opts;
  non_paranoid_opts.create_if_missing = false;
  non_paranoid_opts.paranoid_checks = false;
  ASSERT_TRUE(Reopen(non_paranoid_opts).ok());

  // Prefix (k1) is kept, and nothing from the damaged record on survives: a
  // prefix of the writes, never a state with a hole in it.
  EXPECT_EQ(Get("k1"), "v1");
  Status gs;
  Get("k2", &gs);
  EXPECT_TRUE(gs.IsNotFound()) << gs.ToString();
  Get("k3", &gs);
  EXPECT_TRUE(gs.IsNotFound()) << gs.ToString();

  // The early stop is visible, not silent.
  std::string recovery;
  ASSERT_TRUE(db_->GetProperty("strata.recovery", &recovery));
  EXPECT_NE(recovery.find("Corruption"), std::string::npos) << recovery;
  EXPECT_NE(recovery.find("replay stopped"), std::string::npos) << recovery;

  // Write new data
  ASSERT_TRUE(Put("k_new", "v_new").ok());

  // Reopen again and both must be there
  Close();
  ASSERT_TRUE(Reopen(non_paranoid_opts).ok());
  EXPECT_EQ(Get("k1"), "v1");
  EXPECT_EQ(Get("k_new"), "v_new");
  // The damaged log was retired, so this Open replayed cleanly.
  ASSERT_TRUE(db_->GetProperty("strata.recovery", &recovery));
  EXPECT_EQ(recovery, "OK");
}

class FailingSstEnv : public EnvWrapper {
 public:
  explicit FailingSstEnv(Env* target) : EnvWrapper(target) {}

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    uint64_t number = 0;
    FileType type;
    std::string_view basename = fname;
    const auto slash = basename.rfind('/');
    if (slash != std::string_view::npos) {
      basename.remove_prefix(slash + 1);
    }
    if (fail_sst_.load() && ParseFileName(basename, &number, &type) &&
        type == FileType::kTableFile) {
      std::unique_ptr<WritableFile> base_file;
      Status s = EnvWrapper::NewWritableFile(fname, &base_file);
      if (!s.ok()) {
        return s;
      }
      *result = std::make_unique<FailingWritableFile>(std::move(base_file), /*fail_after=*/0);
      return Status::OK();
    }
    return EnvWrapper::NewWritableFile(fname, result);
  }

  void SetFailSst(bool fail) { fail_sst_.store(fail); }

 private:
  std::atomic<bool> fail_sst_{false};
};

TEST_F(DBTest, StickyBackgroundError) {
  auto flaky_env = std::make_unique<FailingSstEnv>(Env::Default());
  Options opts;
  opts.create_if_missing = true;
  opts.env = flaky_env.get();

  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k_initial", "v_initial").ok());

  // Cause future SST appends to fail
  flaky_env->SetFailSst(true);

  // Trigger flush
  Status s = db_->Flush();
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError()) << s.ToString();

  // Subsequent writes return IOError and keep returning it
  Status s2 = Put("k_after", "v_after");
  EXPECT_FALSE(s2.ok());
  EXPECT_TRUE(s2.IsIOError()) << s2.ToString();

  Status s3 = Put("k_another", "v_another");
  EXPECT_FALSE(s3.ok());
  EXPECT_TRUE(s3.IsIOError()) << s3.ToString();

  // Data written earlier still reads back
  EXPECT_EQ(Get("k_initial"), "v_initial");

  Close();
}

TEST_F(DBTest, ConcurrencyWritesReadsFlushes) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;  // ~500 KB of data: several flushes
  ASSERT_TRUE(Open(opts).ok());

  std::atomic<bool> stop{false};
  constexpr int kNumKeys = 500;
  // ~1 KB values, so each memtable holds many entries and flushes keep happening.
  auto value_for = [](int i) {
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "val_%04d_", i);
    return std::string(prefix) + std::string(1000, 'p');
  };

  // Writer thread
  std::thread writer([this, &stop, &value_for]() {
    for (int i = 0; i < kNumKeys && !stop.load(); ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "concur_%04d", i);
      (void)Put(key, value_for(i));
    }
  });

  // Reader thread
  std::thread reader([this, &stop, &value_for]() {
    while (!stop.load()) {
      for (int i = 0; i < 50; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "concur_%04d", i);
        Status s;
        std::string v = Get(key, &s);
        if (s.ok()) {
          EXPECT_EQ(v, value_for(i));
        }
      }
    }
  });

  writer.join();
  stop.store(true);
  reader.join();
  EXPECT_GE(CountFiles(FileType::kTableFile), 2) << "the test must flush while reading";

  // All written keys read back
  for (int i = 0; i < kNumKeys; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "concur_%04d", i);
    EXPECT_EQ(Get(key), value_for(i));
  }
}

TEST_F(DBTest, ProcessCrashDuringFlushes) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;
  // ~1 KB values: 300 of them (~300 KB) flush several times before the crash.
  const std::string padding(1000, 'p');

  // Open and close once to establish the DB
  ASSERT_TRUE(Open(opts).ok());
  Close();

  pid_t pid = ::fork();
  ASSERT_NE(pid, -1);

  if (pid == 0) {
    // Child process: open DB, write many keys with sync to ensure durability before crash
    std::unique_ptr<DB> child_db;
    if (!DB::Open(opts, test_dir_, &child_db).ok()) {
      ::_exit(1);
    }
    WriteOptions wo;
    wo.sync = true;
    for (int i = 0; i < 300; ++i) {
      char key[32];
      char val[64];
      std::snprintf(key, sizeof(key), "crash_k_%04d", i);
      std::snprintf(val, sizeof(val), "crash_v_%04d", i);
      if (!child_db->Put(wo, key, std::string(val) + padding).ok()) {
        ::_exit(2);
      }
    }
    // Hard crash without closing DB
    ::_exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  // The child flushed while running (the reopen below flushes the rest).
  EXPECT_GE(CountFiles(FileType::kTableFile), 2);

  // Parent reopens DB
  opts.create_if_missing = false;
  ASSERT_TRUE(Reopen(opts).ok());

  // Every written key must be there after reopening
  for (int i = 0; i < 300; ++i) {
    char key[32];
    char expected_val[64];
    std::snprintf(key, sizeof(key), "crash_k_%04d", i);
    std::snprintf(expected_val, sizeof(expected_val), "crash_v_%04d", i);
    EXPECT_EQ(Get(key), std::string(expected_val) + padding);
  }
}

TEST_F(DBTest, FlushOnEmptyDB) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Flush without any writes
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
  EXPECT_EQ(CountFiles(FileType::kLogFile), 1);
}

TEST_F(DBTest, MultipleConsecutiveFlushes) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(CountFiles(FileType::kTableFile), 1);

  // Subsequent flushes on empty memtable must succeed and not create spurious .sst files
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(CountFiles(FileType::kTableFile), 1);

  EXPECT_EQ(Get("k1"), "v1");
}

TEST_F(DBTest, DeleteNonExistentKeyThenFlush) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Delete("absent_key").ok());
  ASSERT_TRUE(db_->Flush().ok());

  Status s;
  Get("absent_key", &s);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, ConcurrentFlushes) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
  }

  std::vector<std::thread> flush_threads;
  flush_threads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    flush_threads.emplace_back([this]() { EXPECT_TRUE(db_->Flush().ok()); });
  }
  for (auto& t : flush_threads) {
    t.join();
  }

  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }
}

TEST_F(DBTest, TinyWriteBufferIsRaisedToTheMinimum) {
  // With a 1 KB buffer every write used to become its own flush and L0 file,
  // because an empty memtable already reports a 4 KB arena block. The buffer
  // is now raised to 64 KB, so ~30 KB of writes stay in the memtable.
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 1024;
  ASSERT_TRUE(Open(opts).ok());
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), std::string(100, 'v')).ok());
  }
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(CountFiles(FileType::kTableFile), 1);
}

// ---------------------------------------------------------------------------
// Week 3 Compaction Integration Tests
// ---------------------------------------------------------------------------

TEST_F(DBTest, DropRulesWithoutSnapshots) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Write key with multiple versions across flushes
  ASSERT_TRUE(Put("key1", "val1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(Put("key1", "val2").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(Put("key1", "val3").ok());
  ASSERT_TRUE(db_->Flush().ok());

  EXPECT_EQ(CountFiles(FileType::kTableFile), 3);

  // Compact the entire database
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  // Point lookup must return latest value
  EXPECT_EQ(Get("key1"), "val3");

  // Inspect table files: older versions val1 and val2 must be dropped
  auto tables = GetTableFiles();
  int total_entries = 0;
  for (const auto& t : tables) {
    total_entries += CountEntriesInTable(t);
  }
  EXPECT_EQ(total_entries, 1);
}

TEST_F(DBTest, DropRulesWithSnapshot) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("key1", "v0").ok());
  ASSERT_TRUE(Put("key1", "v1").ok());
  ASSERT_TRUE(db_->Flush().ok());

  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_NE(snap, nullptr);

  ASSERT_TRUE(Put("key1", "v2").ok());
  ASSERT_TRUE(Put("key1", "v3").ok());
  ASSERT_TRUE(db_->Flush().ok());

  ASSERT_TRUE(db_->CompactRange("", "").ok());

  // Snapshot reads v1
  ReadOptions ro_snap;
  ro_snap.snapshot = snap;
  EXPECT_EQ(Get("key1", nullptr, ro_snap), "v1");

  // Non-snapshot read returns newest v3
  ReadOptions ro_latest;
  EXPECT_EQ(Get("key1", nullptr, ro_latest), "v3");

  // Release the snapshot and write a new version to trigger merge compaction:
  // now older versions v1, v2, v3 are all dropped, only v4 survives.
  db_->ReleaseSnapshot(snap);
  ASSERT_TRUE(Put("key1", "v4").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  EXPECT_EQ(Get("key1"), "v4");
  auto tables = GetTableFiles();
  int total_entries = 0;
  for (const auto& t : tables) {
    total_entries += CountEntriesInTable(t);
  }
  EXPECT_EQ(total_entries, 1);
}

TEST_F(DBTest, TombstoneSurvivesUntilBaseLevel) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Step 1: Write "k1" -> "v1", flush to L0, compact to L1, then L2
  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(0), 0);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 1);

  dbfull()->TEST_CompactRange(1, nullptr, nullptr);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 0);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(2), 1);
  EXPECT_EQ(Get("k1"), "v1");

  // Step 2: Delete "k1" and flush to L0
  ASSERT_TRUE(Delete("k1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(0), 1);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(2), 1);

  // Step 3: Compact L0 to L1.
  // Because L2 has "k1", IsBaseLevelForKey is false: tombstone must survive in L1!
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(0), 0);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 1);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(2), 1);

  Status s;
  Get("k1", &s);
  EXPECT_TRUE(s.IsNotFound());

  // Step 4: Compact L1 down to L2.
  // Now tombstone in L1 meets value in L2, and IsBaseLevelForKey is true (nothing in L3+).
  // Both tombstone and value are eliminated!
  dbfull()->TEST_CompactRange(1, nullptr, nullptr);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 0);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(2), 0);
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);

  Get("k1", &s);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, DeleteEverythingThenCompactReclaimsSpace) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  EXPECT_GT(CountFiles(FileType::kTableFile), 0);

  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Delete("k" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  ASSERT_TRUE(db_->CompactRange("", "").ok());

  for (int i = 0; i < 1000; ++i) {
    Status s;
    Get("k" + std::to_string(i), &s);
    EXPECT_TRUE(s.IsNotFound());
  }

  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
}

TEST_F(DBTest, SustainedLoadAcrossLevels) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;
  opts.max_file_size = 64 * 1024;
  opts.max_bytes_for_level_base = 256 * 1024;
  ASSERT_TRUE(Open(opts).ok());

  constexpr int kNumKeys = 10000;
  for (int i = 0; i < kNumKeys; ++i) {
    std::string key = "key_" + std::to_string((i * 10007) % kNumKeys);
    std::string value(100, static_cast<char>('a' + (i % 26)));
    ASSERT_TRUE(Put(key, value).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  // L0 files must be under the stop trigger (12)
  EXPECT_LE(dbfull()->TEST_NumLevelFiles(0), opts.l0_stop_trigger);

  // Background compaction must have pushed data down to L2 or deeper
  int deeper_files = 0;
  for (int lvl = 2; lvl < kNumLevels; ++lvl) {
    deeper_files += dbfull()->TEST_NumLevelFiles(lvl);
  }
  EXPECT_GT(deeper_files, 0);

  // Reopen DB and verify data integrity
  ASSERT_TRUE(Reopen(opts).ok());

  for (int i = 0; i < kNumKeys; ++i) {
    std::string key = "key_" + std::to_string((i * 10007) % kNumKeys);
    std::string expected_value(100, static_cast<char>('a' + (i % 26)));
    Status s;
    std::string val = Get(key, &s);
    ASSERT_TRUE(s.ok()) << "Failed to get " << key << ": " << s.ToString() << " at index " << i;
    ASSERT_EQ(val, expected_value);
  }
}

TEST_F(DBTest, UserKeyBoundarySplitNeverSplitsSameKeyAcrossFiles) {
  Options opts;
  opts.create_if_missing = true;
  // Set small file size so files split frequently
  opts.max_file_size = 1024;
  ASSERT_TRUE(Open(opts).ok());

  // Write many versions of the same key
  const Snapshot* snap = db_->GetSnapshot();
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("hot_key", std::string(100, 'x')).ok());
  }
  // And a couple of other keys
  ASSERT_TRUE(Put("a_key", "val_a").ok());
  ASSERT_TRUE(Put("z_key", "val_z").ok());
  ASSERT_TRUE(db_->Flush().ok());

  // Compact L0 down to L1
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);

  // In L1, verify that every file's largest user key is strictly less than
  // the next file's smallest user key (STRATA_DCHECK in VersionSet checks this,
  // but let's explicitly verify it as well).
  EXPECT_EQ(Get("a_key"), "val_a");
  EXPECT_EQ(Get("hot_key"), std::string(100, 'x'));
  EXPECT_EQ(Get("z_key"), "val_z");

  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, IteratorAcrossMemTableAndSSTables) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // L1 data
  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(Put("k3", "v3").ok());
  ASSERT_TRUE(db_->Flush().ok());
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);

  // L0 data
  ASSERT_TRUE(Put("k2", "v2").ok());
  ASSERT_TRUE(Put("k5", "v5").ok());
  ASSERT_TRUE(db_->Flush().ok());

  // MemTable data
  ASSERT_TRUE(Put("k4", "v4").ok());

  auto iter = db_->NewIterator(ReadOptions());
  ASSERT_NE(iter, nullptr);

  std::vector<std::pair<std::string, std::string>> expected = {
      {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}, {"k4", "v4"}, {"k5", "v5"}};

  // Forward scan
  std::vector<std::pair<std::string, std::string>> forward_seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    forward_seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  EXPECT_EQ(forward_seen, expected);

  // Reverse scan
  std::vector<std::pair<std::string, std::string>> reverse_seen;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    reverse_seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  std::reverse(reverse_seen.begin(), reverse_seen.end());
  EXPECT_EQ(reverse_seen, expected);

  // Seek tests
  iter->Seek("k3");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k3");
  EXPECT_EQ(iter->value(), "v3");

  iter->Seek("k25");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k3");

  iter->Seek("k6");
  EXPECT_FALSE(iter->Valid());
}

TEST_F(DBTest, IteratorWithSnapshotIsolation) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("a", "v_a1").ok());
  ASSERT_TRUE(Put("b", "v_b1").ok());
  ASSERT_TRUE(Put("c", "v_c1").ok());

  const Snapshot* snap = db_->GetSnapshot();

  // Modify data after snapshot
  ASSERT_TRUE(Put("a", "v_a2").ok());
  ASSERT_TRUE(Delete("b").ok());
  ASSERT_TRUE(Put("d", "v_d1").ok());
  ASSERT_TRUE(db_->Flush().ok());

  // Read with snapshot
  ReadOptions snap_opts;
  snap_opts.snapshot = snap;
  auto snap_iter = db_->NewIterator(snap_opts);

  std::vector<std::pair<std::string, std::string>> snap_expected = {
      {"a", "v_a1"}, {"b", "v_b1"}, {"c", "v_c1"}};
  std::vector<std::pair<std::string, std::string>> snap_seen;
  for (snap_iter->SeekToFirst(); snap_iter->Valid(); snap_iter->Next()) {
    snap_seen.emplace_back(std::string(snap_iter->key()), std::string(snap_iter->value()));
  }
  EXPECT_EQ(snap_seen, snap_expected);

  // Read current
  auto cur_iter = db_->NewIterator(ReadOptions());
  std::vector<std::pair<std::string, std::string>> cur_expected = {
      {"a", "v_a2"}, {"c", "v_c1"}, {"d", "v_d1"}};
  std::vector<std::pair<std::string, std::string>> cur_seen;
  for (cur_iter->SeekToFirst(); cur_iter->Valid(); cur_iter->Next()) {
    cur_seen.emplace_back(std::string(cur_iter->key()), std::string(cur_iter->value()));
  }
  EXPECT_EQ(cur_seen, cur_expected);

  db_->ReleaseSnapshot(snap);
}

}  // namespace
}  // namespace strata
