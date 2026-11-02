#include "strata/db.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/filename.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"

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
  std::string val = Get("non_existent_key", &s);
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

  const std::string large(100 * 1024, 'x');
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

  // Verify that log files keep increasing in number.
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
  // With 1 initial open + 5 reopens, there should be 6 log files.
  EXPECT_GE(log_count, 6);
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
  std::ifstream in(newest_log, std::ios::binary);
  std::string log_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  // Damage in the middle: flip a byte inside a record that has more records after it.
  // Record 1 is at 0, Record 2 starts after Record 1 (around byte 20-30).
  // Flipping a byte at offset 25 corrupts record 2 while record 3 follows it.
  ASSERT_GT(log_bytes.size(), 30u);
  log_bytes[25] ^= 0x42;

  std::ofstream out(newest_log, std::ios::binary | std::ios::trunc);
  out.write(log_bytes.data(), static_cast<std::streamsize>(log_bytes.size()));
  out.close();

  // Open must return Corruption!
  Status s = Reopen(opts);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, ZeroedMiddleBlockReturnsCorruption) {
  Options opts;
  opts.create_if_missing = true;
  ASSERT_TRUE(Open(opts).ok());

  // Write three distinct records, each ~30 KB, so the log spans across at least 3 blocks.
  const std::string chunk(30 * 1024, 'a');
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
  std::ifstream in(newest_log, std::ios::binary);
  std::string log_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  // Zero out the entire second block (bytes 32768 to 65535)
  for (size_t i = 32768; i < 65536 && i < log_bytes.size(); ++i) {
    log_bytes[i] = '\0';
  }

  std::ofstream out(newest_log, std::ios::binary | std::ios::trunc);
  out.write(log_bytes.data(), static_cast<std::streamsize>(log_bytes.size()));
  out.close();

  // Open must return Corruption!
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
  std::ifstream in(newest_log, std::ios::binary);
  std::string orig_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  // Corrupt log
  std::string corrupted = orig_bytes;
  corrupted[0] ^= 0xff;
  std::ofstream out(newest_log, std::ios::binary | std::ios::trunc);
  out.write(corrupted.data(), static_cast<std::streamsize>(corrupted.size()));
  out.close();

  // Open should fail due to corruption
  Status s = Reopen(opts);
  EXPECT_TRUE(s.IsCorruption());

  // Repair the log
  std::ofstream repair(newest_log, std::ios::binary | std::ios::trunc);
  repair.write(orig_bytes.data(), static_cast<std::streamsize>(orig_bytes.size()));
  repair.close();

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

  EXPECT_TRUE(db_->Flush().IsNotSupported());
  EXPECT_TRUE(db_->CompactRange("a", "z").IsNotSupported());
  EXPECT_EQ(db_->NewIterator(ReadOptions()), nullptr);

  std::string prop;
  EXPECT_FALSE(db_->GetProperty("strata.stats", &prop));
}

}  // namespace
}  // namespace strata
