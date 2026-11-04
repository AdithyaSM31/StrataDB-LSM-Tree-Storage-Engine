#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"
#include "util/fault_injection_env.h"
#include "util/random.h"

namespace strata {
namespace {

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

class CrashTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_crash_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(Env::Default()->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();

    fault_env_ = std::make_unique<FaultInjectionEnv>(Env::Default());
  }

  void TearDown() override {
    db_.reset();
    fault_env_.reset();
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  Status OpenDB(Options opts) {
    db_.reset();
    opts.env = fault_env_.get();
    return DB::Open(opts, test_dir_, &db_);
  }

  std::string test_dir_;
  std::unique_ptr<FaultInjectionEnv> fault_env_;
  std::unique_ptr<DB> db_;
};

TEST_F(CrashTest, SyncedWritesSurviveAndPrefixProperty) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;
  ASSERT_TRUE(OpenDB(opts).ok());

  constexpr int kTotalWrites = 500;
  constexpr int kSyncInterval = 10;
  int last_synced_index = -1;

  for (int i = 0; i < kTotalWrites; ++i) {
    WriteOptions wopts;
    if (i % kSyncInterval == 0) {
      wopts.sync = true;
    }
    Status s = db_->Put(wopts, KeyStr(i), ValStr(i));
    ASSERT_TRUE(s.ok()) << s.ToString();
    if (wopts.sync) {
      last_synced_index = i;
    }
  }

  // Crash without clean DB close: un-synced WAL data will be truncated
  ASSERT_TRUE(fault_env_->SimulateCrash().ok());

  // Destroy DB instance (simulating process death)
  db_.reset();

  // Reopen DB
  opts.create_if_missing = false;
  ASSERT_TRUE(OpenDB(opts).ok());

  // All synced writes MUST be present
  for (int i = 0; i <= last_synced_index; ++i) {
    if (i % kSyncInterval == 0) {
      std::string val;
      Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
      ASSERT_TRUE(s.ok()) << "Synced key " << KeyStr(i) << " was lost after crash!";
      EXPECT_EQ(val, ValStr(i));
    }
  }

  // The recovered state must be a contiguous prefix of 0..K
  int recovered_count = 0;
  for (int i = 0; i < kTotalWrites; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    if (s.ok()) {
      EXPECT_EQ(i, recovered_count) << "Sequence gap detected at key " << i;
      EXPECT_EQ(val, ValStr(i));
      recovered_count++;
    } else {
      break;
    }
  }

  // Ensure no "future" keys leaked past the prefix boundary
  for (int i = recovered_count; i < kTotalWrites; ++i) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    EXPECT_TRUE(s.IsNotFound()) << "Key " << KeyStr(i) << " found past prefix end "
                                << recovered_count;
  }

  EXPECT_GE(recovered_count, last_synced_index + 1);
}

TEST_F(CrashTest, BatchAtomicityAcrossCrash) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;
  ASSERT_TRUE(OpenDB(opts).ok());

  constexpr int kNumBatches = 50;
  constexpr int kKeysPerBatch = 8;
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

  // Simulate crash
  ASSERT_TRUE(fault_env_->SimulateCrash().ok());
  db_.reset();

  // Reopen
  opts.create_if_missing = false;
  ASSERT_TRUE(OpenDB(opts).ok());

  // Check batch atomicity: for every batch, either all keys exist or none exist
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

    if (b <= last_synced_batch && b % 5 == 0) {
      EXPECT_EQ(found_count, kKeysPerBatch) << "Synced batch " << b << " was lost!";
    }
  }
}

TEST_F(CrashTest, CrashDuringFlushPreservesSyncedWrites) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;
  ASSERT_TRUE(OpenDB(opts).ok());

  // Write enough data to fill the memtable
  for (int i = 0; i < 200; ++i) {
    WriteOptions wopts;
    wopts.sync = (i % 10 == 0);
    ASSERT_TRUE(db_->Put(wopts, KeyStr(i), ValStr(i)).ok());
  }

  // Crash without waiting for flush
  ASSERT_TRUE(fault_env_->SimulateCrash().ok());
  db_.reset();

  // Reopen and check data
  opts.create_if_missing = false;
  ASSERT_TRUE(OpenDB(opts).ok());

  // The database should have replayed the WAL and all synced keys should exist
  for (int i = 0; i <= 190; i += 10) {
    std::string val;
    Status s = db_->Get(ReadOptions(), KeyStr(i), &val);
    EXPECT_TRUE(s.ok()) << "Key " << KeyStr(i) << " missing after crash during flush";
    EXPECT_EQ(val, ValStr(i));
  }

  // Database must accept new writes after crash recovery
  ASSERT_TRUE(db_->Put(WriteOptions(), "new_key", "new_val").ok());
  std::string new_val;
  ASSERT_TRUE(db_->Get(ReadOptions(), "new_key", &new_val).ok());
  EXPECT_EQ(new_val, "new_val");
}

TEST_F(CrashTest, MultiCrashCycles) {
  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;

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

    // Crash simulation
    ASSERT_TRUE(fault_env_->SimulateCrash().ok());
    db_.reset();
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

}  // namespace
}  // namespace strata
