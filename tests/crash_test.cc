#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crash_model.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"
#include "util/fault_injection_env.h"

#if defined(__SANITIZE_ADDRESS__)
// Lets ASan see the background thread touch a CompactRange frame that returned.
extern "C" const char* __asan_default_options() { return "detect_stack_use_after_return=1"; }
#endif

namespace strata {
namespace {

using UnsyncedData = FaultInjectionEnv::UnsyncedData;

constexpr size_t kWriteBuffer = size_t{64} * 1024;

std::string KeyStr(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%06d", i);
  return std::string(buf);
}

std::string ValStr(int i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "value_payload_%06d", i);
  return std::string(buf);
}

// Reads a counter out of strata.stats, e.g. "Flush count: ".
uint64_t Stat(DB* db, const std::string& label) {
  std::string stats;
  EXPECT_TRUE(db->GetProperty("strata.stats", &stats));
  const auto pos = stats.find(label);
  EXPECT_NE(pos, std::string::npos) << label;
  return pos == std::string::npos ? 0 : std::stoull(stats.substr(pos + label.size()));
}

// While the gate is closed, table file creation waits: a flush or compaction
// stops before it can sync the directory.
class TableGateEnv : public FaultInjectionEnv {
 public:
  explicit TableGateEnv(Env* target) : FaultInjectionEnv(target) {}

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    if (fname.size() > 4 && fname.compare(fname.size() - 4, 4, ".sst") == 0) {
      std::unique_lock<std::mutex> lock(gate_mutex_);
      waiting_ = true;
      gate_cv_.notify_all();
      gate_cv_.wait(lock, [this] { return !closed_; });
    }
    return FaultInjectionEnv::NewWritableFile(fname, result);
  }

  void CloseTableGate() {
    std::lock_guard<std::mutex> lock(gate_mutex_);
    closed_ = true;
  }

  void WaitUntilSomeoneIsHeld() {
    std::unique_lock<std::mutex> lock(gate_mutex_);
    gate_cv_.wait(lock, [this] { return waiting_; });
  }

  void OpenTableGate() {
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      closed_ = false;
    }
    gate_cv_.notify_all();
  }

 private:
  std::mutex gate_mutex_;
  std::condition_variable gate_cv_;
  bool closed_ = false;
  bool waiting_ = false;
};

class CrashTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_crash_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(Env::Default()->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
            .string();
    fault_env_ = std::make_unique<TableGateEnv>(Env::Default());
    FreshDir();
  }

  void TearDown() override {
    fault_env_->OpenTableGate();  // a failed assertion may have left it closed
    db_.reset();
    fault_env_.reset();
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  // Starts over with an empty directory, for tests that loop over crash points.
  void FreshDir() {
    db_.reset();
    fault_env_->Reset();
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  Options DefaultOptions() {
    Options opts;
    opts.create_if_missing = true;
    opts.write_buffer_size = kWriteBuffer;
    return opts;
  }

  Status OpenDB(Options opts) {
    db_.reset();
    opts.env = fault_env_.get();
    return DB::Open(opts, test_dir_, &db_);
  }

  // Power loss: applies the crash, destroys the DB, and reopens from what is left.
  Status CrashAndReopen(UnsyncedData mode = UnsyncedData::kDropAll, uint32_t seed = 0) {
    Status s = fault_env_->SimulateCrash(mode, seed);
    if (!s.ok()) {
      return s;
    }
    db_.reset();
    fault_env_->ResumeAfterCrash();
    return OpenDB(DefaultOptions());
  }

  std::string test_dir_;
  std::unique_ptr<TableGateEnv> fault_env_;
  std::unique_ptr<DB> db_;
};

TEST_F(CrashTest, SyncedWritesSurviveAndPrefixProperty) {
  constexpr int kTotalWrites = 500;
  constexpr int kSyncInterval = 10;

  for (uint32_t seed = 0; seed < 5; ++seed) {
    FreshDir();
    ASSERT_TRUE(OpenDB(DefaultOptions()).ok());

    int last_synced_index = -1;
    for (int i = 0; i < kTotalWrites; ++i) {
      WriteOptions wopts;
      wopts.sync = (i % kSyncInterval == 0);
      Status s = db_->Put(wopts, KeyStr(i), ValStr(i));
      ASSERT_TRUE(s.ok()) << s.ToString();
      if (wopts.sync) {
        last_synced_index = i;
      }
    }

    // Seed 0 loses every unsynced byte; the others keep a torn tail of the WAL.
    const UnsyncedData mode = seed == 0 ? UnsyncedData::kDropAll : UnsyncedData::kKeepRandomTail;
    ASSERT_TRUE(CrashAndReopen(mode, seed).ok());

    int recovered_count = 0;
    for (int i = 0; i < kTotalWrites; ++i) {
      std::string val;
      Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
      if (i == recovered_count && s.ok()) {
        EXPECT_EQ(val, ValStr(i));
        ++recovered_count;
      } else {
        EXPECT_TRUE(s.IsNotFound()) << "Key " << KeyStr(i) << " past prefix end " << recovered_count
                                    << ": " << s.ToString();
      }
    }
    EXPECT_GE(recovered_count, last_synced_index + 1);
    if (mode == UnsyncedData::kDropAll) {
      EXPECT_EQ(recovered_count, last_synced_index + 1);
    }
  }
}

TEST_F(CrashTest, BatchAtomicityAcrossCrash) {
  constexpr int kNumBatches = 50;
  constexpr int kKeysPerBatch = 8;

  // A torn WAL tail can end mid-batch; every seed tears it somewhere else.
  for (uint32_t seed = 0; seed < 20; ++seed) {
    FreshDir();
    ASSERT_TRUE(OpenDB(DefaultOptions()).ok());

    int last_synced_batch = -1;
    for (int b = 0; b < kNumBatches; ++b) {
      WriteBatch batch;
      for (int k = 0; k < kKeysPerBatch; ++k) {
        char key[64];
        char val[64];
        std::snprintf(key, sizeof(key), "batch_%04d_key_%d", b, k);
        std::snprintf(val, sizeof(val), "batch_%04d_val_%d", b, k);
        batch.Put(key, val);
      }
      WriteOptions wopts;
      if (b % 5 == 0) {
        wopts.sync = true;
        last_synced_batch = b;
      }
      ASSERT_TRUE(db_->Write(wopts, &batch).ok());
    }

    ASSERT_TRUE(CrashAndReopen(UnsyncedData::kKeepRandomTail, seed).ok());

    for (int b = 0; b < kNumBatches; ++b) {
      int found_count = 0;
      for (int k = 0; k < kKeysPerBatch; ++k) {
        char key[64];
        char expected_val[64];
        std::snprintf(key, sizeof(key), "batch_%04d_key_%d", b, k);
        std::snprintf(expected_val, sizeof(expected_val), "batch_%04d_val_%d", b, k);
        std::string val;
        Status s = db_->Get(ReadOptions(), key, &val);
        if (s.ok()) {
          EXPECT_EQ(val, expected_val);
          found_count++;
        }
      }
      EXPECT_TRUE(found_count == 0 || found_count == kKeysPerBatch)
          << "Batch " << b << " was partially recovered! found " << found_count << "/"
          << kKeysPerBatch;
      if (b <= last_synced_batch) {
        EXPECT_EQ(found_count, kKeysPerBatch) << "Batch " << b << " lost, sync came after it";
      }
    }
  }
}

TEST_F(CrashTest, CrashWhileFlushesAreRunning) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());

  // Values large enough that the memtable keeps filling, so the background
  // thread is usually mid-flush or mid-compaction when the crash comes.
  CrashWorkload workload;
  workload.value_size = 2048;
  uint64_t durable = 0;
  for (uint64_t seq = 1; seq <= 600; ++seq) {
    const bool sync = seq % 16 == 0;
    ASSERT_TRUE(workload.Apply(db_.get(), seq, sync).ok());
    if (sync) {
      durable = seq;
    }
  }
  ASSERT_GT(Stat(db_.get(), "Flush count: "), 0U) << "the crash must follow real flushes";

  ASSERT_TRUE(CrashAndReopen().ok());
  CrashModel model(workload);
  Status s = model.Verify(db_.get(), durable);
  EXPECT_TRUE(s.ok()) << s.ToString();

  // The recovered DB must take new writes.
  ASSERT_TRUE(db_->Put(WriteOptions(), "new_key", "new_val").ok());
  std::string new_val;
  ASSERT_TRUE(db_->Get(ReadOptions(), "new_key", &new_val).ok());
  EXPECT_EQ(new_val, "new_val");
}

TEST_F(CrashTest, FlushedDataSurvivesCrashRightAfterFlush) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db_->Put(WriteOptions(), KeyStr(i), ValStr(i)).ok());  // never synced
  }
  ASSERT_TRUE(db_->Flush().ok());

  // Flush returned, so the SST, its directory entry and the MANIFEST must all be durable.
  ASSERT_TRUE(CrashAndReopen().ok());
  for (int i = 0; i < 100; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok()) << KeyStr(i) << ": " << s.ToString();
    EXPECT_EQ(val, ValStr(i));
  }
}

TEST_F(CrashTest, CompactedDataSurvivesCrashRightAfterCompaction) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db_->Put(WriteOptions(), KeyStr(i), ValStr(i + round)).ok());
    }
    ASSERT_TRUE(db_->Flush().ok());
  }
  ASSERT_TRUE(db_->CompactRange("a", "z").ok());
  ASSERT_GT(Stat(db_.get(), "Compaction count: "), 0U);

  // The compaction deleted its inputs, so its outputs are the only copy left.
  ASSERT_TRUE(CrashAndReopen().ok());
  for (int i = 0; i < 100; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok()) << KeyStr(i) << ": " << s.ToString();
    EXPECT_EQ(val, ValStr(i + 1));
  }
}

TEST_F(CrashTest, SyncedWriteRightAfterOpenSurvives) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  WriteOptions sync;
  sync.sync = true;
  ASSERT_TRUE(db_->Put(sync, "k", "v").ok());

  // Only Open has synced the directory, so the WAL's entry depends on that one call.
  ASSERT_TRUE(CrashAndReopen().ok());
  std::string val;
  Status s = db_->Get(ReadOptions(), "k", &val);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(val, "v");
}

TEST_F(CrashTest, SyncedWriteAfterWalSwitchSurvives) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  fault_env_->CloseTableGate();

  WriteOptions sync;
  sync.sync = true;
  ASSERT_TRUE(db_->Put(sync, "first", std::string(kWriteBuffer + 1024, 'x')).ok());
  // This write switches the memtable and lands in the WAL the switch created.
  ASSERT_TRUE(db_->Put(sync, "second", "must_survive").ok());
  fault_env_->WaitUntilSomeoneIsHeld();

  ASSERT_TRUE(fault_env_->SimulateCrash().ok());
  fault_env_->OpenTableGate();
  db_.reset();
  fault_env_->ResumeAfterCrash();

  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  std::string val;
  Status s = db_->Get(ReadOptions(), "second", &val);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(val, "must_survive");
}

// Crashes at every file operation of a run with flushes, a compaction and a
// clean reopen, and checks each crash point recovers to a valid state.
class CrashPointTest : public CrashTest, public ::testing::WithParamInterface<UnsyncedData> {
 protected:
  static constexpr uint64_t kWrites = 48;

  // Runs until the filesystem stops. *durable is the last write confirmed durable.
  Status RunWorkload(const CrashWorkload& workload, uint64_t* durable) {
    Status s = OpenDB(DefaultOptions());
    for (uint64_t seq = 1; s.ok() && seq <= kWrites; ++seq) {
      const bool sync = seq % 4 == 0;
      s = workload.Apply(db_.get(), seq, sync);
      if (s.ok() && sync) {
        *durable = seq;
      }
      if (s.ok() && seq % 16 == 0) {
        s = db_->Flush();
        if (s.ok()) {
          *durable = seq;
        }
      }
      if (s.ok() && seq == 32) {
        s = db_->CompactRange("a", "z");
      }
      if (s.ok() && seq == 40) {
        // A clean reopen replays the WAL, flushes it and starts a new MANIFEST.
        s = OpenDB(DefaultOptions());
      }
    }
    return s;
  }
};

TEST_P(CrashPointTest, EveryCrashPointRecovers) {
  CrashWorkload workload;
  workload.key_space = 20;
  workload.value_size = 1024;

  uint64_t crash_points = 0;
  for (uint64_t n = 1;; ++n) {
    FreshDir();
    fault_env_->SetCrashAtOperation(n);
    uint64_t durable = 0;
    const Status run = RunWorkload(workload, &durable);
    const bool crashed = !fault_env_->IsFilesystemActive();
    if (!crashed) {
      ASSERT_TRUE(run.ok()) << "Operation " << n << ": " << run.ToString();
      break;
    }
    ++crash_points;

    Status s = CrashAndReopen(GetParam(), static_cast<uint32_t>(n));
    ASSERT_TRUE(s.ok()) << "Open after a crash at operation " << n << ": " << s.ToString();
    CrashModel model(workload);
    s = model.Verify(db_.get(), durable);
    ASSERT_TRUE(s.ok()) << "After a crash at operation " << n << ": " << s.ToString();

    // Recovery must be durable too: crash again straight after it.
    s = CrashAndReopen(GetParam(), static_cast<uint32_t>(n) + 1);
    ASSERT_TRUE(s.ok()) << "Second Open after a crash at operation " << n << ": " << s.ToString();
    CrashModel model2(workload);
    s = model2.Verify(db_.get(), durable);
    ASSERT_TRUE(s.ok()) << "After a second crash at operation " << n << ": " << s.ToString();
  }
  EXPECT_GT(crash_points, 100U);
}

INSTANTIATE_TEST_SUITE_P(UnsyncedDataModes, CrashPointTest,
                         ::testing::Values(UnsyncedData::kDropAll, UnsyncedData::kKeepRandomTail),
                         [](const ::testing::TestParamInfo<UnsyncedData>& param_info) {
                           return param_info.param == UnsyncedData::kDropAll ? "DropAll"
                                                                             : "KeepRandomTail";
                         });

TEST_F(CrashTest, MultiCrashCycles) {
  Options opts = DefaultOptions();

  int total_synced_keys = 0;
  for (int cycle = 0; cycle < 10; ++cycle) {
    opts.create_if_missing = (cycle == 0);
    ASSERT_TRUE(OpenDB(opts).ok());

    // Verify all keys from previous cycles
    for (int i = 0; i < total_synced_keys; ++i) {
      std::string val;
      Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
      ASSERT_TRUE(s.ok()) << "Key " << KeyStr(i) << " lost in cycle " << cycle;
      EXPECT_EQ(val, ValStr(i));
    }

    // Write new keys
    for (int i = 0; i < 20; ++i) {
      int key_idx = total_synced_keys + i;
      WriteOptions wopts;
      wopts.sync = true;
      ASSERT_TRUE(db_->Put(wopts, KeyStr(key_idx), ValStr(key_idx)).ok());
    }
    total_synced_keys += 20;

    ASSERT_TRUE(fault_env_->SimulateCrash().ok());
    db_.reset();
    fault_env_->ResumeAfterCrash();
  }

  // Final verification
  opts.create_if_missing = false;
  ASSERT_TRUE(OpenDB(opts).ok());
  for (int i = 0; i < total_synced_keys; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(val, ValStr(i));
  }
}

TEST_F(CrashTest, BatchAtomicityWithPutsAndDeletesAcrossCrash) {
  for (uint32_t seed = 0; seed < 20; ++seed) {
    FreshDir();
    ASSERT_TRUE(OpenDB(DefaultOptions()).ok());

    // Invariant: sum of account balances must always equal 1000
    WriteBatch initial_batch;
    initial_batch.Put("acc_A", "1000");
    initial_batch.Put("acc_B", "0");
    initial_batch.Put("tx_0", "committed");
    WriteOptions sync_wopts;
    sync_wopts.sync = true;
    ASSERT_TRUE(db_->Write(sync_wopts, &initial_batch).ok());

    int last_synced_tx = 0;
    for (int tx = 1; tx <= 30; ++tx) {
      WriteBatch batch;
      const int bal_a = 1000 - tx * 20;
      const int bal_b = tx * 20;
      batch.Put("acc_A", std::to_string(bal_a));
      batch.Put("acc_B", std::to_string(bal_b));
      batch.Delete("tx_" + std::to_string(tx - 1));
      batch.Put("tx_" + std::to_string(tx), "committed");

      WriteOptions wopts;
      if (tx % 5 == 0) {
        wopts.sync = true;
        last_synced_tx = tx;
      }
      ASSERT_TRUE(db_->Write(wopts, &batch).ok());
    }

    ASSERT_TRUE(CrashAndReopen(UnsyncedData::kKeepRandomTail, seed).ok());

    std::string str_a;
    std::string str_b;
    ASSERT_TRUE(db_->Get(ReadOptions(), "acc_A", &str_a).ok());
    ASSERT_TRUE(db_->Get(ReadOptions(), "acc_B", &str_b).ok());

    const int val_a = std::stoi(str_a);
    const int val_b = std::stoi(str_b);
    EXPECT_EQ(val_a + val_b, 1000) << "Atomicity violated! Sum is " << (val_a + val_b);

    // Synced balance state must be at least up to last_synced_tx
    EXPECT_LE(val_a, 1000 - last_synced_tx * 20);
    EXPECT_GE(val_b, last_synced_tx * 20);

    // Exactly one tx marker should exist
    int found_tx = -1;
    for (int tx = 0; tx <= 30; ++tx) {
      std::string val;
      Status s = db_->Get(ReadOptions(), "tx_" + std::to_string(tx), &val);
      if (s.ok()) {
        EXPECT_EQ(found_tx, -1) << "Multiple tx markers found! tx " << found_tx << " and " << tx;
        found_tx = tx;
      }
    }
    EXPECT_NE(found_tx, -1);
    EXPECT_EQ(val_a, 1000 - found_tx * 20);
    EXPECT_EQ(val_b, found_tx * 20);
  }
}

// A failed fsync leaves the MANIFEST record in the file, so the edit may be
// durable even though LogAndApply reported failure. Whichever way it went, the
// next Open must find every key.
TEST_F(CrashTest, FailedManifestSyncDuringCompactionKeepsData) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db_->Put(WriteOptions(), KeyStr(i), ValStr(i + round)).ok());
    }
    ASSERT_TRUE(db_->Flush().ok());
  }

  fault_env_->SetFailManifestSyncs(true);
  Status compact = db_->CompactRange("a", "z");
  EXPECT_TRUE(compact.IsIOError()) << compact.ToString();
  EXPECT_TRUE(db_->Put(WriteOptions(), "after", "x").IsIOError()) << "the error must be sticky";

  fault_env_->SetFailManifestSyncs(false);
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int i = 0; i < 100; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok()) << KeyStr(i) << ": " << s.ToString();
    EXPECT_EQ(val, ValStr(i + 1));
  }
}

TEST_F(CrashTest, FailedManifestSyncDuringFlushKeepsData) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  WriteOptions sync;
  sync.sync = true;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db_->Put(sync, KeyStr(i), ValStr(i)).ok());
  }

  fault_env_->SetFailManifestSyncs(true);
  EXPECT_TRUE(db_->Flush().IsIOError());

  fault_env_->SetFailManifestSyncs(false);
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int i = 0; i < 100; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok()) << KeyStr(i) << ": " << s.ToString();
    EXPECT_EQ(val, ValStr(i));
  }
}

// A background error can release a CompactRange caller while the background
// thread is still inside that compaction. The thread must not then touch the
// request, which lived on the caller's stack.
TEST_F(CrashTest, ManualCompactionCallerCanLeaveDuringBackgroundError) {
  ASSERT_TRUE(OpenDB(DefaultOptions()).ok());
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db_->Put(WriteOptions(), KeyStr(i), ValStr(i)).ok());
    }
    ASSERT_TRUE(db_->Flush().ok());
  }

  fault_env_->CloseTableGate();
  Status compact;
  std::thread caller([&] { compact = db_->CompactRange("a", "z"); });
  fault_env_->WaitUntilSomeoneIsHeld();  // the background thread is inside the compaction

  fault_env_->SetFailWrites(true);
  EXPECT_TRUE(db_->Put(WriteOptions(), "trigger", "x").IsIOError());
  caller.join();  // returns on the error, with the background thread still held
  EXPECT_TRUE(compact.IsIOError()) << compact.ToString();

  fault_env_->OpenTableGate();  // now the background thread finishes that compaction
  db_.reset();
}

}  // namespace
}  // namespace strata
