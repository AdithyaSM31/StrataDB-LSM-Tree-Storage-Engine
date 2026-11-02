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

#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "log/log_writer.h"
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
  std::string log_bytes = ReadFileToString(newest_log);

  // Damage in the middle: flip a byte inside a record that has more records after it.
  // Record 1 is at 0, Record 2 starts after Record 1 (around byte 20-30).
  // Flipping a byte at offset 25 corrupts record 2 while record 3 follows it.
  ASSERT_GT(log_bytes.size(), 30u);
  log_bytes[25] ^= 0x42;

  WriteStringToFile(newest_log, log_bytes);

  // Open must return Corruption!
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

  EXPECT_TRUE(db_->Flush().IsNotSupported());
  EXPECT_TRUE(db_->CompactRange("a", "z").IsNotSupported());
  std::unique_ptr<Iterator> it = db_->NewIterator(ReadOptions());
  ASSERT_NE(it, nullptr);
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsNotSupported());

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

}  // namespace
}  // namespace strata
