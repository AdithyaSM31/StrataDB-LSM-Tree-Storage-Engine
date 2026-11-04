#include "strata/db.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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

  // Waits until n writers (the leader included) are in the write queue. Gives
  // up after 10 s, so a bug fails the test instead of hanging it.
  bool WaitForQueuedWriters(int n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (dbfull()->TEST_WritersCount() < n) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

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
    std::unique_ptr<Iterator> it = table->NewIterator(ReadOptions());
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

TEST_F(DBTest, MaxOpenFilesIsClamped) {
  Options opts;
  opts.create_if_missing = true;
  opts.max_open_files = 10;
  ASSERT_TRUE(Open(opts).ok());
  auto* impl = static_cast<DBImpl*>(db_.get());
  EXPECT_EQ(impl->TEST_options().max_open_files, 64);
  // Clamped to at least 64. TableCache capacity is 64 - 10 = 54, split across
  // 16 shards so that the shard capacities sum to exactly 54.
  for (int i = 0; i < 70; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v").ok());
    ASSERT_TRUE(db_->Flush().ok());
  }
  for (int i = 0; i < 70; ++i) {
    EXPECT_EQ(Get("k" + std::to_string(i)), "v");
  }
  EXPECT_LE(impl->TEST_table_cache()->TotalCharge(), 54u);

  // Upper clamp: max_open_files > 50000 is clamped to 50000.
  Close();
  opts.max_open_files = 100000;
  ASSERT_TRUE(Open(opts).ok());
  auto* impl2 = static_cast<DBImpl*>(db_.get());
  EXPECT_EQ(impl2->TEST_options().max_open_files, 50000);
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
  opts.write_buffer_size = size_t{64} * 1024;
  opts.max_file_size = size_t{64} * 1024;
  opts.max_bytes_for_level_base = uint64_t{256} * 1024;
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
  opts.max_file_size = 1024;  // tiny outputs: the compaction must cut many times
  ASSERT_TRUE(Open(opts).ok());

  // A snapshot older than every write keeps all 60 versions of "hot" alive, so
  // the compaction must write ~6 KB for that one user key.
  const Snapshot* snap = db_->GetSnapshot();
  for (int i = 10; i < 50; ++i) {
    ASSERT_TRUE(Put("a" + std::to_string(i), std::string(200, 'a')).ok());
  }
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(Put("hot", std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(Put("hot", std::string(100, 'y')).ok());
  }
  for (int i = 10; i < 50; ++i) {
    ASSERT_TRUE(Put("z" + std::to_string(i), std::string(200, 'z')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_EQ(dbfull()->TEST_NumLevelFiles(0), 2);

  // Two overlapping L0 files: a real merge, not a trivial move.
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);
  ASSERT_EQ(dbfull()->TEST_NumLevelFiles(0), 0);

  const std::vector<FileMetaData> files = dbfull()->TEST_LevelFiles(1);
  ASSERT_GE(files.size(), 3u) << "outputs were not cut by size";
  // "hot" (~6 KB of versions) must sit whole inside one file.
  int files_with_hot = 0;
  for (const auto& f : files) {
    if (ExtractUserKey(f.smallest) <= "hot" && "hot" <= ExtractUserKey(f.largest)) {
      ++files_with_hot;
    }
  }
  EXPECT_EQ(files_with_hot, 1);
  for (size_t i = 0; i + 1 < files.size(); ++i) {
    // Strict: no user key may straddle two files of the same level.
    EXPECT_LT(ExtractUserKey(files[i].largest), ExtractUserKey(files[i + 1].smallest))
        << "files " << files[i].number << " and " << files[i + 1].number;
  }

  EXPECT_EQ(Get("a10"), std::string(200, 'a'));
  EXPECT_EQ(Get("hot"), std::string(100, 'y'));
  EXPECT_EQ(Get("z49"), std::string(200, 'z'));
  ReadOptions at_snapshot;
  at_snapshot.snapshot = snap;
  Status s;
  Get("hot", &s, at_snapshot);
  EXPECT_TRUE(s.IsNotFound());
  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, CompactRangeRewritesASingleFileInsteadOfMovingIt) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Puts and deletes in one memtable become one L0 file full of tombstones.
  // Moving it to L1 unchanged would keep that dead data forever.
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v").ok());
  }
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(Delete("k" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_EQ(dbfull()->TEST_NumLevelFiles(0), 1);

  ASSERT_TRUE(db_->CompactRange("", "").ok());
  for (int level = 0; level < kNumLevels; ++level) {
    EXPECT_EQ(dbfull()->TEST_NumLevelFiles(level), 0) << "level " << level;
  }
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);
}

TEST_F(DBTest, CompactPointerIsRecordedAndSurvivesTwoReopens) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());
  EXPECT_EQ(dbfull()->TEST_CompactPointer(0), "");

  ASSERT_TRUE(Put("a", "1").ok());
  ASSERT_TRUE(Put("m", "1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(Put("m", "2").ok());
  ASSERT_TRUE(Put("q", "2").ok());
  ASSERT_TRUE(db_->Flush().ok());
  dbfull()->TEST_CompactRange(0, nullptr, nullptr);

  // The pointer is the largest key among the compaction's level-0 inputs.
  const std::string pointer = dbfull()->TEST_CompactPointer(0);
  ASSERT_FALSE(pointer.empty());
  EXPECT_EQ(ExtractUserKey(pointer), "q");

  // The first reopen replays the edit that set the pointer. The second reads
  // only the snapshot the first one wrote into its fresh MANIFEST.
  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(dbfull()->TEST_CompactPointer(0), pointer);
  ASSERT_TRUE(Reopen(opts).ok());
  EXPECT_EQ(dbfull()->TEST_CompactPointer(0), pointer);
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

TEST_F(DBTest, CompactRangeStopsAtMaxLevel) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("k1", "v1").ok());
  ASSERT_TRUE(db_->Flush().ok());

  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(0), 1);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 0);

  // CompactRange on database with only L0 data should compact L0 into L1,
  // and NOT cascade down into L2, L3, L4, L5, L6.
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(0), 0);
  EXPECT_EQ(dbfull()->TEST_NumLevelFiles(1), 1);
  for (int lvl = 2; lvl < kNumLevels; ++lvl) {
    EXPECT_EQ(dbfull()->TEST_NumLevelFiles(lvl), 0);
  }
}

TEST_F(DBTest, ConcurrentCompactRange) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "val" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  std::vector<std::thread> compacters;
  compacters.reserve(3);
  for (int t = 0; t < 3; ++t) {
    compacters.emplace_back([this] {
      for (int i = 0; i < 5; ++i) {
        Status s = db_->CompactRange("", "");
        EXPECT_TRUE(s.ok());
      }
    });
  }

  for (auto& t : compacters) {
    t.join();
  }

  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(Get("k" + std::to_string(i)), "val" + std::to_string(i));
  }
}

bool IsTableFileName(const std::string& fname) {
  std::string_view basename = fname;
  const auto slash = basename.rfind('/');
  if (slash != std::string_view::npos) {
    basename.remove_prefix(slash + 1);
  }
  uint64_t number = 0;
  FileType type;
  return ParseFileName(basename, &number, &type) && type == FileType::kTableFile;
}

// Counts the write path's slowdown sleeps (MakeRoomForWrite is the only caller).
class CountingSleepEnv : public EnvWrapper {
 public:
  explicit CountingSleepEnv(Env* target) : EnvWrapper(target) {}
  void SleepForMicroseconds(int micros) override {
    sleeps_.fetch_add(1);
    EnvWrapper::SleepForMicroseconds(micros);
  }
  int sleeps() const { return sleeps_.load(); }

 private:
  std::atomic<int> sleeps_{0};
};

TEST_F(DBTest, WritesSlowDownAtTheL0SlowdownTrigger) {
  auto env = std::make_unique<CountingSleepEnv>(Env::Default());
  CountingSleepEnv* counting = env.get();
  custom_env_ = std::move(env);
  Options opts;
  opts.create_if_missing = true;
  opts.env = counting;
  opts.l0_compaction_trigger = 100;  // nothing compacts the L0 files away
  opts.l0_slowdown_trigger = 2;
  opts.l0_stop_trigger = 100;
  ASSERT_TRUE(Open(opts).ok());

  ASSERT_TRUE(Put("a", "1").ok());
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_TRUE(Put("b", "1").ok());  // one L0 file: below the trigger
  ASSERT_TRUE(db_->Flush().ok());
  ASSERT_EQ(dbfull()->TEST_NumLevelFiles(0), 2);
  EXPECT_EQ(counting->sleeps(), 0);

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(Put("c" + std::to_string(i), "1").ok());
  }
  EXPECT_EQ(counting->sleeps(), 5);  // exactly one 1 ms delay per write
}

TEST_F(DBTest, WritesStopAtTheL0StopTriggerUntilCompactionCatchesUp) {
  Options opts;
  opts.create_if_missing = true;
  opts.l0_compaction_trigger = 100;  // only the manual compaction below drains L0
  opts.write_buffer_size = size_t{64} * 1024;
  opts.l0_slowdown_trigger = 100;
  opts.l0_stop_trigger = 3;
  ASSERT_TRUE(Open(opts).ok());

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(Put("k", std::to_string(i)).ok());
    ASSERT_TRUE(db_->Flush().ok());
  }
  ASSERT_EQ(dbfull()->TEST_NumLevelFiles(0), 3);

  // ~100 KB of writes: the memtable fills once (64 KB), and switching it would
  // make a fourth L0 file, so the writer must wait.
  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    for (int i = 0; i < 100; ++i) {
      EXPECT_TRUE(Put("w" + std::to_string(i), std::string(1000, 'w')).ok());
    }
    writer_done.store(true);
  });

  env_->SleepForMicroseconds(300 * 1000);
  EXPECT_FALSE(writer_done.load()) << "writes did not stop at the L0 stop trigger";

  dbfull()->TEST_CompactRange(0, nullptr, nullptr);  // drains L0, waking the writer
  writer.join();
  EXPECT_TRUE(writer_done.load());
  EXPECT_EQ(Get("k"), "2");
  EXPECT_EQ(Get("w99"), std::string(1000, 'w'));
}

// Holds back the first table file created after Arm() until Release(), and
// logs when each table file is created and closed from then on.
class GateEnv : public EnvWrapper {
 public:
  explicit GateEnv(Env* target) : EnvWrapper(target) {}

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    if (IsTableFileName(fname)) {
      std::unique_lock<std::mutex> lock(mu_);
      if (armed_) {
        events_.push_back("open " + fname);
        if (!gate_used_) {
          gate_used_ = true;
          blocked_ = true;
          cv_.notify_all();
          cv_.wait(lock, [this] { return released_; });
        }
      }
    }
    std::unique_ptr<WritableFile> base;
    Status s = EnvWrapper::NewWritableFile(fname, &base);
    if (s.ok() && IsTableFileName(fname)) {
      *result = std::make_unique<LoggedFile>(std::move(base), this, fname);
    } else if (s.ok()) {
      *result = std::move(base);
    }
    return s;
  }

  void Arm() {
    std::lock_guard<std::mutex> lock(mu_);
    armed_ = true;
  }
  void WaitUntilBlocked() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return blocked_; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = true;
    cv_.notify_all();
  }
  std::vector<std::string> events() {
    std::lock_guard<std::mutex> lock(mu_);
    return events_;
  }

 private:
  class LoggedFile : public WritableFileWrapper {
   public:
    LoggedFile(std::unique_ptr<WritableFile> target, GateEnv* env, std::string fname)
        : WritableFileWrapper(std::move(target)), env_(env), fname_(std::move(fname)) {}
    Status Close() override {
      env_->Record("close " + fname_);
      return WritableFileWrapper::Close();
    }

   private:
    GateEnv* const env_;
    const std::string fname_;
  };

  void Record(std::string event) {
    std::lock_guard<std::mutex> lock(mu_);
    if (armed_) {
      events_.push_back(std::move(event));
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  bool armed_ = false;
  bool gate_used_ = false;
  bool blocked_ = false;
  bool released_ = false;
  std::vector<std::string> events_;
};

TEST_F(DBTest, FlushRunsWhileACompactionIsInProgress) {
  auto env = std::make_unique<GateEnv>(Env::Default());
  GateEnv* gate = env.get();
  custom_env_ = std::move(env);
  Options opts;
  opts.create_if_missing = true;
  opts.env = gate;
  opts.l0_compaction_trigger = 100;  // only the manual compaction below runs
  opts.write_buffer_size = size_t{64} * 1024;
  ASSERT_TRUE(Open(opts).ok());

  for (char round : {'a', 'b'}) {  // two overlapping L0 files
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(Put("k" + std::to_string(1000 + i), std::string(100, round)).ok());
    }
    ASSERT_TRUE(db_->Flush().ok());
  }

  // The compaction stops while creating its output file.
  gate->Arm();
  std::thread compactor([&] { dbfull()->TEST_CompactRange(0, nullptr, nullptr); });
  gate->WaitUntilBlocked();

  // Meanwhile 60 writes of 1 KB fill the 64 KB memtable exactly once, making
  // an imm_ that only the (busy) background thread can flush. Keep the count
  // well below 90: around there the memtable fills a second time, and that
  // write would wait for the flush the paused compaction is holding up.
  for (int i = 0; i < 60; ++i) {
    ASSERT_TRUE(Put("m" + std::to_string(i), std::string(1000, 'm')).ok());
  }
  gate->Release();
  compactor.join();

  // events[0] opens the compaction's output. The flush must create and finish
  // its own table before the compaction closes that output.
  const std::vector<std::string> events = gate->events();
  ASSERT_GE(events.size(), 2u);
  ASSERT_EQ(events[0].rfind("open ", 0), 0u);
  const std::string compaction_output = events[0].substr(5);
  const auto output_closed = std::find(events.begin(), events.end(), "close " + compaction_output);
  const auto flush_opened =
      std::find_if(events.begin() + 1, events.end(),
                   [](const std::string& e) { return e.rfind("open ", 0) == 0; });
  ASSERT_NE(output_closed, events.end());
  ASSERT_NE(flush_opened, events.end());
  EXPECT_LT(flush_opened - events.begin(), output_closed - events.begin())
      << "the memtable was flushed only after the compaction finished";

  EXPECT_EQ(Get("k1000"), std::string(100, 'b'));
  EXPECT_EQ(Get("m59"), std::string(1000, 'm'));
}

class GroupCommitTestEnv : public EnvWrapper {
 public:
  explicit GroupCommitTestEnv(Env* target) : EnvWrapper(target) {}

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    std::unique_ptr<WritableFile> base_file;
    Status s = EnvWrapper::NewWritableFile(fname, &base_file);
    if (!s.ok()) {
      return s;
    }
    uint64_t num = 0;
    FileType type = FileType::kTableFile;
    size_t slash = fname.rfind('/');
    std::string_view basename =
        (slash == std::string::npos) ? fname : std::string_view(fname).substr(slash + 1);
    if (ParseFileName(basename, &num, &type) && type == FileType::kLogFile) {
      *result = std::make_unique<GroupCommitTestWritableFile>(std::move(base_file), this);
    } else {
      *result = std::move(base_file);
    }
    return Status::OK();
  }

  void HoldNextSync() {
    hold_sync_.store(true);
    in_sync_.store(false);
    release_sync_.store(false);
  }

  // Both waits give up after 10 s and return false, so a write path that
  // never syncs (or never appends) fails the test instead of hanging it.
  bool WaitForSyncToHold() { return WaitFor(in_sync_); }

  void ReleaseSync() {
    hold_sync_.store(false);
    release_sync_.store(true);
  }

  void HoldNextAppend() {
    hold_append_.store(true);
    in_append_.store(false);
    release_append_.store(false);
  }

  bool WaitForAppendToHold() { return WaitFor(in_append_); }

  void ReleaseAppend() {
    hold_append_.store(false);
    release_append_.store(true);
  }

  void SetFailSync(bool fail) { fail_sync_.store(fail); }

  void SetFailOnSyncNumber(int n) { fail_on_sync_num_.store(n); }

  int wal_sync_count() const { return wal_sync_count_.load(); }
  void reset_wal_sync_count() { wal_sync_count_.store(0); }

 private:
  static bool WaitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!flag.load()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

  class GroupCommitTestWritableFile : public WritableFileWrapper {
   public:
    GroupCommitTestWritableFile(std::unique_ptr<WritableFile> target, GroupCommitTestEnv* env)
        : WritableFileWrapper(std::move(target)), env_(env) {}

    Status Append(std::string_view data) override {
      if (env_->hold_append_.load()) {
        env_->in_append_.store(true);
        while (!env_->release_append_.load()) {
          std::this_thread::yield();
        }
      }
      return WritableFileWrapper::Append(data);
    }

    Status Sync() override {
      if (env_->hold_sync_.load()) {
        env_->in_sync_.store(true);
        while (!env_->release_sync_.load()) {
          std::this_thread::yield();
        }
      }
      int sync_idx = env_->wal_sync_count_.fetch_add(1) + 1;
      if (env_->fail_sync_.load() || env_->fail_on_sync_num_.load() == sync_idx) {
        return Status::IOError("injected WAL sync failure");
      }
      return WritableFileWrapper::Sync();
    }

   private:
    GroupCommitTestEnv* env_;
  };

  std::atomic<int> wal_sync_count_{0};
  std::atomic<bool> hold_sync_{false};
  std::atomic<bool> in_sync_{false};
  std::atomic<bool> release_sync_{false};
  std::atomic<bool> hold_append_{false};
  std::atomic<bool> in_append_{false};
  std::atomic<bool> release_append_{false};
  std::atomic<bool> fail_sync_{false};
  std::atomic<int> fail_on_sync_num_{0};
};

TEST_F(DBTest, GroupCommitHappens) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextSync();

  std::vector<std::thread> writers;
  std::vector<Status> statuses(10);

  // Writer 0 becomes leader and is held in WAL Sync()
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[0] = Put("k0", "v0", wo);
  });

  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Queue 9 more sync writers behind Writer 0
  for (int i = 1; i < 10; ++i) {
    writers.emplace_back([this, &statuses, i] {
      WriteOptions wo;
      wo.sync = true;
      statuses[i] = Put("k" + std::to_string(i), "v" + std::to_string(i), wo);
    });
  }

  // Wait until all 10 writers are in the queue
  EXPECT_TRUE(WaitForQueuedWriters(10));

  // Release the held sync. Writer 0 finishes sync #1.
  // Writer 1 becomes leader, batches writers 1..9, and executes sync #2.
  env_ptr->ReleaseSync();

  for (auto& t : writers) {
    t.join();
  }

  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(statuses[i].ok());
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }

  // Expect exactly 2 syncs for 10 writes!
  EXPECT_EQ(env_ptr->wal_sync_count(), 2);
}

TEST_F(DBTest, NoUnsyncedSyncWrites) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextAppend();

  std::thread non_sync_writer([this] {
    WriteOptions wo;
    wo.sync = false;
    EXPECT_TRUE(Put("k_async", "v_async", wo).ok());
  });

  EXPECT_TRUE(env_ptr->WaitForAppendToHold());

  std::thread sync_writer([this] {
    WriteOptions wo;
    wo.sync = true;
    EXPECT_TRUE(Put("k_sync", "v_sync", wo).ok());
  });

  EXPECT_TRUE(WaitForQueuedWriters(2));

  // Release non-sync writer. It must NOT batch the sync writer because
  // non-sync leader cannot acknowledge a sync follower without fsync.
  env_ptr->ReleaseAppend();

  non_sync_writer.join();
  sync_writer.join();

  EXPECT_EQ(Get("k_async"), "v_async");
  EXPECT_EQ(Get("k_sync"), "v_sync");

  // The sync writer's record must get its own Sync (1 sync total for the WAL).
  EXPECT_EQ(env_ptr->wal_sync_count(), 1);
}

TEST_F(DBTest, SyncFailureStickyAcrossGroup) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextSync();

  std::vector<std::thread> writers;
  std::vector<Status> statuses(4);

  // Writer 0 enters Sync and holds
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[0] = Put("k0", "v0", wo);
  });

  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Writers 1, 2, 3 queue up while Writer 0 is holding
  for (int i = 1; i < 4; ++i) {
    writers.emplace_back([this, &statuses, i] {
      WriteOptions wo;
      wo.sync = true;
      statuses[i] = Put("k" + std::to_string(i), "v" + std::to_string(i), wo);
    });
  }

  EXPECT_TRUE(WaitForQueuedWriters(4));

  // Make sync #2 (the group sync for writers 1, 2, 3) fail
  env_ptr->SetFailOnSyncNumber(2);
  env_ptr->ReleaseSync();

  for (auto& t : writers) {
    t.join();
  }

  // Writer 0 succeeded (sync #1 succeeded)
  EXPECT_TRUE(statuses[0].ok());
  EXPECT_EQ(Get("k0"), "v0");

  // Every writer in the queued group (writers 1, 2, 3) gets the sync failure error
  for (int i = 1; i < 4; ++i) {
    EXPECT_FALSE(statuses[i].ok());
    EXPECT_TRUE(statuses[i].IsIOError());
  }

  // The next Put also returns the sticky error
  Status next_s = Put("k_next", "v_next");
  EXPECT_FALSE(next_s.ok());
  EXPECT_TRUE(next_s.IsIOError());
}

TEST_F(DBTest, ConcurrentWritesNoSequenceGaps) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  constexpr int kThreads = 8;
  constexpr int kPutsPerThread = 2000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t] {
      for (int i = 0; i < kPutsPerThread; ++i) {
        std::string key = "c_th_" + std::to_string(t) + "_k_" + std::to_string(i);
        std::string val = "c_val_" + std::to_string(i);
        Status s = Put(key, val);
        EXPECT_TRUE(s.ok());
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPutsPerThread; ++i) {
      std::string key = "c_th_" + std::to_string(t) + "_k_" + std::to_string(i);
      std::string val = "c_val_" + std::to_string(i);
      EXPECT_EQ(Get(key), val);
    }
  }

  EXPECT_EQ(dbfull()->TEST_LastSequence(), static_cast<SequenceNumber>(kThreads * kPutsPerThread));
}

TEST_F(DBTest, FlushDuringConcurrentWrites) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;  // the minimum: flushes happen often
  ASSERT_TRUE(Open(opts).ok());

  std::atomic<bool> stop{false};
  constexpr int kWriters = 4;
  constexpr int kPutsPerWriter = 500;
  std::vector<std::thread> writers;
  writers.reserve(kWriters);

  for (int t = 0; t < kWriters; ++t) {
    writers.emplace_back([this, t] {
      for (int i = 0; i < kPutsPerWriter; ++i) {
        std::string key = "fl_th_" + std::to_string(t) + "_k_" + std::to_string(i);
        std::string val = "fl_val_" + std::to_string(i);
        Status s = Put(key, val);
        EXPECT_TRUE(s.ok());
      }
    });
  }

  std::thread flush_thread([this, &stop] {
    while (!stop.load(std::memory_order_relaxed)) {
      (void)db_->Flush();
      std::this_thread::yield();
    }
  });

  for (auto& t : writers) {
    t.join();
  }
  stop.store(true, std::memory_order_relaxed);
  flush_thread.join();

  for (int t = 0; t < kWriters; ++t) {
    for (int i = 0; i < kPutsPerWriter; ++i) {
      std::string key = "fl_th_" + std::to_string(t) + "_k_" + std::to_string(i);
      std::string val = "fl_val_" + std::to_string(i);
      EXPECT_EQ(Get(key), val);
    }
  }
}

TEST_F(DBTest, ConcurrentSyncWritesShareFsyncs) {
  // Throughput belongs in the benchmark; this only checks that concurrent
  // sync writers really share fsyncs. (On the review machine 2,000 sync writes
  // from 8 threads took about 455 fsyncs.)
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());
  env_ptr->reset_wal_sync_count();

  constexpr int kThreads = 8;
  constexpr int kPutsPerThread = 250;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t] {
      WriteOptions wo;
      wo.sync = true;
      for (int i = 0; i < kPutsPerThread; ++i) {
        EXPECT_TRUE(Put("th_" + std::to_string(t) + "_k_" + std::to_string(i), "v", wo).ok());
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  const int writes = kThreads * kPutsPerThread;
  EXPECT_GT(env_ptr->wal_sync_count(), 0);
  EXPECT_LT(env_ptr->wal_sync_count(), writes);
  EXPECT_EQ(dbfull()->TEST_LastSequence(), static_cast<SequenceNumber>(writes));
}

TEST_F(DBTest, WriteRejectsANullBatch) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());
  ASSERT_TRUE(Put("k", "v").ok());

  // nullptr is only the internal "switch the memtable" signal used by Flush.
  Status s = db_->Write(WriteOptions(), nullptr);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(CountFiles(FileType::kTableFile), 0);  // and it did not flush
  EXPECT_EQ(Get("k"), "v");
}

TEST_F(DBTest, ReadersNeverSeeAPartialBatch) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Every batch sets all keys to the same round number, so any snapshot must
  // see the same value for the first and the last key.
  constexpr int kKeys = 100;
  auto key = [](int k) { return "pb_" + std::to_string(1000 + k); };
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int round = 0; !stop.load(); ++round) {
      WriteBatch batch;
      for (int k = 0; k < kKeys; ++k) {
        batch.Put(key(k), std::to_string(round));
      }
      EXPECT_TRUE(db_->Write(WriteOptions(), &batch).ok());
    }
  });

  int mismatches = 0;
  int checks = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline || checks < 1000) {
    const Snapshot* snap = db_->GetSnapshot();
    ReadOptions ro;
    ro.snapshot = snap;
    // The last key first: a batch is inserted in order, so a half-inserted one
    // would show the old value here and the new value in the first key.
    Status s_last;
    Status s_first;
    const std::string last = Get(key(kKeys - 1), &s_last, ro);
    const std::string first = Get(key(0), &s_first, ro);
    if (s_last.ok() != s_first.ok() || last != first) {
      ++mismatches;
    }
    ++checks;
    db_->ReleaseSnapshot(snap);
  }
  stop.store(true);
  writer.join();
  EXPECT_EQ(mismatches, 0) << "out of " << checks << " snapshots";
}

TEST_F(DBTest, NonSyncLeaderDoesNotBatchSyncFollower) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextSync();

  std::vector<std::thread> writers;
  std::vector<Status> statuses(5);

  // Writer 0 (sync=true) is held in WAL Sync()
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[0] = Put("k0", "v0", wo);
  });
  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Writer 1 (sync=false)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = false;
    statuses[1] = Put("k1", "v1", wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(2));  // queue order = creation order

  // Writer 2 (sync=false)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = false;
    statuses[2] = Put("k2", "v2", wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(3));  // queue order = creation order

  // Writer 3 (sync=true)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[3] = Put("k3", "v3", wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(4));  // queue order = creation order

  // Writer 4 (sync=false)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = false;
    statuses[4] = Put("k4", "v4", wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(5));  // queue order = creation order

  // Release Writer 0.
  // Writer 0 finishes (Sync 1).
  // Writer 1 (sync=false) becomes leader. It batches Writer 2 (sync=false),
  // but MUST STOP before Writer 3 (sync=true). Group {1, 2} committed without sync.
  // Writer 3 (sync=true) becomes leader. It batches Writer 4 (sync=false).
  // Group {3, 4} committed WITH sync (Sync 2).
  env_ptr->ReleaseSync();

  for (auto& t : writers) {
    t.join();
  }

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(statuses[i].ok());
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }

  // Exactly 2 syncs: 1 for Writer 0, 1 for Writer 3's group.
  EXPECT_EQ(env_ptr->wal_sync_count(), 2);
}

TEST_F(DBTest, BuildBatchGroupThrottlesOnSmallLeader) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextSync();

  std::vector<std::thread> writers;
  std::vector<Status> statuses(4);

  // Writer 0 (sync=true) holds in WAL Sync()
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[0] = Put("hold_k", "hold_v", wo);
  });
  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Writer 1: small leader batch (1 KB <= 128 KB) -> max group size = size + 128 KB (~129 KB)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[1] = Put("sm_k1", std::string(1024, 'a'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(2));  // queue order = creation order

  // Writer 2: follower batch of 60 KB (total so far ~61 KB <= ~129 KB -> should be included)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[2] = Put("sm_k2", std::string(size_t{60} * 1024, 'b'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(3));  // queue order = creation order

  // Writer 3: follower batch of 80 KB (total would be ~141 KB > ~129 KB -> must NOT be included)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[3] = Put("sm_k3", std::string(size_t{80} * 1024, 'c'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(4));  // queue order = creation order

  // Release Writer 0 (Sync 1).
  // Writer 1 batches Writer 2, stops before Writer 3, and executes Sync 2.
  // Writer 3 becomes leader and executes Sync 3.
  // Total syncs must be exactly 3!
  env_ptr->ReleaseSync();

  for (auto& t : writers) {
    t.join();
  }

  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  EXPECT_EQ(Get("hold_k"), "hold_v");
  EXPECT_EQ(Get("sm_k1"), std::string(1024, 'a'));
  EXPECT_EQ(Get("sm_k2"), std::string(size_t{60} * 1024, 'b'));
  EXPECT_EQ(Get("sm_k3"), std::string(size_t{80} * 1024, 'c'));

  EXPECT_EQ(env_ptr->wal_sync_count(), 3);
}

TEST_F(DBTest, BuildBatchGroupOneMegabyteCap) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  env_ptr->reset_wal_sync_count();
  env_ptr->HoldNextSync();

  std::vector<std::thread> writers;
  std::vector<Status> statuses(4);

  // Writer 0 (sync=true) holds in WAL Sync()
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[0] = Put("hold_k", "hold_v", wo);
  });
  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Writer 1: 200 KB (> 128 KB, so max_size = 1 MB)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[1] = Put("cap_k1", std::string(size_t{200} * 1024, 'a'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(2));  // queue order = creation order

  // Writer 2: 500 KB (total 700 KB <= 1 MB -> included)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[2] = Put("cap_k2", std::string(size_t{500} * 1024, 'b'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(3));  // queue order = creation order

  // Writer 3: 400 KB (total 1100 KB > 1 MB -> must NOT be included)
  writers.emplace_back([this, &statuses] {
    WriteOptions wo;
    wo.sync = true;
    statuses[3] = Put("cap_k3", std::string(size_t{400} * 1024, 'c'), wo);
  });
  EXPECT_TRUE(WaitForQueuedWriters(4));  // queue order = creation order

  // Release Writer 0 (Sync 1).
  // Writer 1 batches Writer 2, stops at 1 MB cap before Writer 3, executes Sync 2.
  // Writer 3 executes Sync 3.
  // Total syncs must be exactly 3!
  env_ptr->ReleaseSync();

  for (auto& t : writers) {
    t.join();
  }

  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(statuses[i].ok());
  }
  EXPECT_EQ(Get("cap_k1"), std::string(size_t{200} * 1024, 'a'));
  EXPECT_EQ(Get("cap_k2"), std::string(size_t{500} * 1024, 'b'));
  EXPECT_EQ(Get("cap_k3"), std::string(size_t{400} * 1024, 'c'));

  EXPECT_EQ(env_ptr->wal_sync_count(), 3);
}

TEST_F(DBTest, EmptyAndCorruptedBatchValidation) {
  auto test_env = std::make_unique<GroupCommitTestEnv>(Env::Default());
  GroupCommitTestEnv* env_ptr = test_env.get();
  custom_env_ = std::move(test_env);

  Options opts;
  opts.create_if_missing = true;
  opts.env = env_ptr;
  ASSERT_TRUE(Open(opts).ok());

  // 1. Empty batch returns OK and does not increment sequence number
  WriteBatch empty;
  EXPECT_TRUE(db_->Write(WriteOptions(), &empty).ok());
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 0u);

  // 2. Corrupted batch returns Corruption and does not increment sequence number
  WriteBatch corrupted;
  WriteBatchInternal::SetContents(&corrupted, "too_small");
  Status s = db_->Write(WriteOptions(), &corrupted);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 0u);

  // 3. Concurrency check: an empty batch and an invalid batch do not block or join the queue
  env_ptr->HoldNextSync();
  std::thread leader([this] {
    WriteOptions wo;
    wo.sync = true;
    EXPECT_TRUE(Put("lead_k", "lead_v", wo).ok());
  });
  EXPECT_TRUE(env_ptr->WaitForSyncToHold());

  // Call empty and corrupted write while leader is held in Sync.
  // Since they return before queueing, they must return immediately without blocking!
  WriteBatch empty2;
  EXPECT_TRUE(db_->Write(WriteOptions(), &empty2).ok());

  WriteBatch corrupted2;
  WriteBatchInternal::SetContents(&corrupted2, "bad_payload");
  EXPECT_TRUE(db_->Write(WriteOptions(), &corrupted2).IsCorruption());

  // Leader was the only writer in writers_
  EXPECT_EQ(dbfull()->TEST_WritersCount(), 1);

  env_ptr->ReleaseSync();
  leader.join();

  EXPECT_EQ(Get("lead_k"), "lead_v");
  EXPECT_EQ(dbfull()->TEST_LastSequence(), 1u);
}

TEST_F(DBTest, SequenceNumberOverflowOnGroup) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Set sequence number close to maximum: kMaxSequenceNumber - 2
  constexpr SequenceNumber kNearMax = kMaxSequenceNumber - 2;
  dbfull()->TEST_SetLastSequence(kNearMax);

  // A single put of 1 key succeeds (kNearMax + 1 <= kMaxSequenceNumber)
  EXPECT_TRUE(Put("ok_k", "ok_v").ok());
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kNearMax + 1);

  // Now LastSequence is kMaxSequenceNumber - 1.
  // A batch of 2 keys would reach kMaxSequenceNumber + 1 > kMaxSequenceNumber.
  WriteBatch batch;
  batch.Put("overflow_k1", "v1");
  batch.Put("overflow_k2", "v2");
  Status s = db_->Write(WriteOptions(), &batch);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_NE(s.ToString().find("sequence number overflow"), std::string::npos);

  // Verify that an invalid batch sequence error did not corrupt DB state
  // and LastSequence remains at kMaxSequenceNumber - 1
  EXPECT_EQ(dbfull()->TEST_LastSequence(), kNearMax + 1);
}

TEST_F(DBTest, MultipleFlushesInterspersedWithWrites) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;
  ASSERT_TRUE(Open(opts).ok());

  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "th_" + std::to_string(t) + "_k_" + std::to_string(i);
        std::string val = "val_" + std::to_string(i);
        WriteOptions wo;
        wo.sync = (i % 5 == 0);
        EXPECT_TRUE(Put(key, val, wo).ok());
        if (i % 20 == 0) {
          EXPECT_TRUE(db_->Flush().ok());
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Final flush to ensure everything is durable
  EXPECT_TRUE(db_->Flush().ok());

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "th_" + std::to_string(t) + "_k_" + std::to_string(i);
      std::string val = "val_" + std::to_string(i);
      EXPECT_EQ(Get(key), val);
    }
  }
}

TEST_F(DBTest, SecondReadIsFree) {
  class ReadCountingEnv : public EnvWrapper {
   public:
    explicit ReadCountingEnv(Env* target) : EnvWrapper(target) {}

    Status NewRandomAccessFile(const std::string& fname,
                               std::unique_ptr<RandomAccessFile>* result) override {
      std::unique_ptr<RandomAccessFile> target_file;
      Status s = target()->NewRandomAccessFile(fname, &target_file);
      if (!s.ok()) {
        return s;
      }
      if (fname.size() >= 4 && fname.substr(fname.size() - 4) == ".sst") {
        *result = std::make_unique<CountingFile>(std::move(target_file), &total_reads_);
      } else {
        *result = std::move(target_file);
      }
      return Status::OK();
    }

    int total_reads() const { return total_reads_.load(std::memory_order_relaxed); }
    void ResetReads() { total_reads_.store(0, std::memory_order_relaxed); }

   private:
    class CountingFile : public RandomAccessFileWrapper {
     public:
      CountingFile(std::unique_ptr<RandomAccessFile> target, std::atomic<int>* reads)
          : RandomAccessFileWrapper(std::move(target)), reads_(reads) {}

      Status Read(uint64_t offset, size_t n, std::string_view* result,
                  char* scratch) const override {
        reads_->fetch_add(1, std::memory_order_relaxed);
        return RandomAccessFileWrapper::Read(offset, n, result, scratch);
      }

     private:
      std::atomic<int>* reads_;
    };

    std::atomic<int> total_reads_{0};
  };

  auto counting_env = std::make_unique<ReadCountingEnv>(Env::Default());
  auto* env_ptr = counting_env.get();
  custom_env_ = std::move(counting_env);
  Options options;
  options.create_if_missing = true;
  options.block_cache_capacity = size_t{8} * 1024 * 1024;
  options.env = env_ptr;
  ASSERT_TRUE(Open(options).ok());

  // Write table 1
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("k1_" + std::to_string(i), std::string(100, 'v')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  // Write table 2
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("k2_" + std::to_string(i), std::string(100, 'v')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  auto* impl = static_cast<DBImpl*>(db_.get());
  ASSERT_NE(impl->TEST_block_cache(), nullptr);

  // 1. First read with fill_cache = true
  env_ptr->ResetReads();
  ReadOptions cache_ropts;
  cache_ropts.fill_cache = true;
  std::string val;
  EXPECT_TRUE(db_->Get(cache_ropts, "k1_10", &val).ok());
  EXPECT_GT(env_ptr->total_reads(), 0);
  const size_t charge_after_first = impl->TEST_block_cache()->TotalCharge();
  EXPECT_GT(charge_after_first, 0u);

  // 2. Second read of same key: block is cached, zero file reads!
  env_ptr->ResetReads();
  EXPECT_TRUE(db_->Get(cache_ropts, "k1_10", &val).ok());
  EXPECT_EQ(env_ptr->total_reads(), 0);

  // 3. Read uncached key from table 2 with fill_cache = false
  ReadOptions no_cache_ropts;
  no_cache_ropts.fill_cache = false;
  env_ptr->ResetReads();
  EXPECT_TRUE(db_->Get(no_cache_ropts, "k2_10", &val).ok());
  EXPECT_GT(env_ptr->total_reads(), 0);
  EXPECT_EQ(impl->TEST_block_cache()->TotalCharge(), charge_after_first);  // cache doesn't grow

  // 4. Read again with fill_cache = false: reads again from disk
  env_ptr->ResetReads();
  EXPECT_TRUE(db_->Get(no_cache_ropts, "k2_10", &val).ok());
  EXPECT_GT(env_ptr->total_reads(), 0);
  EXPECT_EQ(impl->TEST_block_cache()->TotalCharge(), charge_after_first);
}

TEST_F(DBTest, FileLimitHolds) {
  class LiveFileCountingEnv : public EnvWrapper {
   public:
    explicit LiveFileCountingEnv(Env* target) : EnvWrapper(target) {}

    Status NewRandomAccessFile(const std::string& fname,
                               std::unique_ptr<RandomAccessFile>* result) override {
      std::unique_ptr<RandomAccessFile> target_file;
      Status s = target()->NewRandomAccessFile(fname, &target_file);
      if (!s.ok()) {
        return s;
      }
      if (fname.size() >= 4 && fname.substr(fname.size() - 4) == ".sst") {
        *result = std::make_unique<CountingFile>(std::move(target_file), this);
      } else {
        *result = std::move(target_file);
      }
      return Status::OK();
    }

    int live_files() const { return live_files_.load(std::memory_order_relaxed); }
    int max_live_files() const { return max_live_files_.load(std::memory_order_relaxed); }

   private:
    class CountingFile : public RandomAccessFileWrapper {
     public:
      CountingFile(std::unique_ptr<RandomAccessFile> target, LiveFileCountingEnv* env)
          : RandomAccessFileWrapper(std::move(target)), env_(env) {
        int cur = env_->live_files_.fetch_add(1, std::memory_order_relaxed) + 1;
        int prev_max = env_->max_live_files_.load(std::memory_order_relaxed);
        while (cur > prev_max && !env_->max_live_files_.compare_exchange_weak(prev_max, cur)) {
        }
      }
      ~CountingFile() override { env_->live_files_.fetch_sub(1, std::memory_order_relaxed); }

     private:
      LiveFileCountingEnv* env_;
    };

    std::atomic<int> live_files_{0};
    std::atomic<int> max_live_files_{0};
  };

  auto counting_env = std::make_unique<LiveFileCountingEnv>(Env::Default());
  auto* env_ptr = counting_env.get();
  custom_env_ = std::move(counting_env);
  Options options;
  options.create_if_missing = true;
  options.max_open_files = 74;  // capacity = 74 - 10 = 64
  options.env = env_ptr;
  ASSERT_TRUE(Open(options).ok());

  // Make about 150 SSTs
  constexpr int kNumFiles = 150;
  for (int i = 0; i < kNumFiles; ++i) {
    std::string key = "key_" + std::to_string(i);
    std::string val = "value_" + std::to_string(i);
    ASSERT_TRUE(Put(key, val).ok());
    ASSERT_TRUE(db_->Flush().ok());
  }

  // Find an SST file to pin with a table iterator
  std::vector<std::string> filenames;
  ASSERT_TRUE(env_ptr->GetChildren(test_dir_, &filenames).ok());
  uint64_t pinned_file_number = 0;
  FileType type;
  for (const auto& fn : filenames) {
    if (ParseFileName(fn, &pinned_file_number, &type) && type == FileType::kTableFile) {
      break;
    }
  }
  ASSERT_GT(pinned_file_number, 0ULL);

  uint64_t file_size = 0;
  ASSERT_TRUE(env_ptr->GetFileSize(TableFileName(test_dir_, pinned_file_number), &file_size).ok());
  auto iter = static_cast<DBImpl*>(db_.get())->TEST_table_cache()->NewIterator(
      ReadOptions(), pinned_file_number, file_size);
  ASSERT_NE(iter, nullptr);

  // Now read every key. The count should never go over 64 plus the 1 pinned iterator.
  for (int i = 0; i < kNumFiles; ++i) {
    std::string key = "key_" + std::to_string(i);
    std::string expected_val = "value_" + std::to_string(i);
    EXPECT_EQ(Get(key), expected_val);
    EXPECT_LE(env_ptr->live_files(), 64 + 1);
  }

  // Peak live files may momentarily include the 1 table opened before cache insertion
  // triggers eviction of an old entry (64 cached + 1 pinned + 1 transient open).
  EXPECT_LE(env_ptr->max_live_files(), 64 + 1 + 1);

  // Release the iterator
  iter.reset();

  // Re-read every key after iterator dropped. Live files never exceed 64.
  for (int i = 0; i < kNumFiles; ++i) {
    std::string key = "key_" + std::to_string(i);
    std::string expected_val = "value_" + std::to_string(i);
    EXPECT_EQ(Get(key), expected_val);
    EXPECT_LE(env_ptr->live_files(), 64);
  }

  EXPECT_LE(env_ptr->live_files(), 64);
}

TEST_F(DBTest, CompactionDoesntTouchTheCache) {
  Options options;
  options.create_if_missing = true;
  options.block_cache_capacity = size_t{8} * 1024 * 1024;
  ASSERT_TRUE(Open(options).ok());

  // Write some data and flush to L0
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  // Read a key to populate cache
  std::string val;
  ReadOptions ropts;
  ropts.fill_cache = true;
  EXPECT_TRUE(db_->Get(ropts, "k0", &val).ok());

  auto* impl = static_cast<DBImpl*>(db_.get());
  ASSERT_NE(impl->TEST_block_cache(), nullptr);
  const uint64_t inserts_before = impl->TEST_block_cache()->inserts();
  EXPECT_GT(inserts_before, 0u);

  // Compact the entire DB. Compaction reads must use fill_cache = false!
  ASSERT_TRUE(db_->CompactRange("", "").ok());

  // Cache inserts count must remain identical
  EXPECT_EQ(impl->TEST_block_cache()->inserts(), inserts_before);
}

TEST_F(DBTest, BlockCacheCapacitySmallerThanBlockSize) {
  Options options;
  options.create_if_missing = true;
  // Block cache capacity 100 bytes is much smaller than a single ~4 KB data block
  options.block_cache_capacity = 100;
  ASSERT_TRUE(Open(options).ok());

  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(Put("key_" + std::to_string(i), std::string(200, 'x')).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  auto* impl = static_cast<DBImpl*>(db_.get());
  ASSERT_NE(impl->TEST_block_cache(), nullptr);

  // Read keys. Since each block is ~4KB, which is > 100 bytes capacity (and > per-shard capacity),
  // blocks cannot fit in any shard. Reading must succeed correctly without caching.
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(Get("key_" + std::to_string(i)), std::string(200, 'x'));
  }
  EXPECT_EQ(impl->TEST_block_cache()->TotalCharge(), 0u);
}

TEST_F(DBTest, BlockCacheCapacityZero) {
  Options options;
  options.create_if_missing = true;
  options.block_cache_capacity = 0;  // Caching disabled
  ASSERT_TRUE(Open(options).ok());

  auto* impl = static_cast<DBImpl*>(db_.get());
  EXPECT_EQ(impl->TEST_block_cache(), nullptr);

  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(Put("k_" + std::to_string(i), "v_" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->Flush().ok());

  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(Get("k_" + std::to_string(i)), "v_" + std::to_string(i));
  }
}

}  // namespace
}  // namespace strata
