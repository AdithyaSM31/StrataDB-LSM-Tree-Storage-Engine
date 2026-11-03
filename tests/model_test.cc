#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/db.h"
#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "util/random.h"

namespace strata {
namespace {

class ModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    static std::atomic<uint64_t> counter{0};
    test_dir_ = (std::filesystem::temp_directory_path() /
                 ("stratadb_model_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(env_->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
                    .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();

    options_.create_if_missing = true;
    options_.write_buffer_size = size_t{64} * 1024;  // 64 KB
    options_.max_file_size = size_t{64} * 1024;      // 64 KB
    options_.l0_compaction_trigger = 2;              // aggressive compaction
    ASSERT_TRUE(DB::Open(options_, test_dir_, &db_).ok());
  }

  void TearDown() override {
    ReleaseAllSnapshots();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  void ReleaseAllSnapshots() {
    if (db_ != nullptr) {
      for (const auto& [snap, _] : snapshot_models_) {
        db_->ReleaseSnapshot(snap);
      }
    }
    snapshot_models_.clear();
  }

  void Reopen() {
    ReleaseAllSnapshots();
    db_.reset();
    ASSERT_TRUE(DB::Open(options_, test_dir_, &db_).ok());
  }

  void Put(std::string_view key, std::string_view value) {
    ASSERT_TRUE(db_->Put(WriteOptions(), key, value).ok());
    current_model_[std::string(key)] = std::string(value);
  }

  void Delete(std::string_view key) {
    ASSERT_TRUE(db_->Delete(WriteOptions(), key).ok());
    current_model_.erase(std::string(key));
  }

  void Flush() { ASSERT_TRUE(db_->Flush().ok()); }

  void CompactRange(std::string_view begin, std::string_view end) {
    ASSERT_TRUE(db_->CompactRange(begin, end).ok());
  }

  const Snapshot* TakeSnapshot() {
    const Snapshot* snap = db_->GetSnapshot();
    STRATA_DCHECK(snap != nullptr);
    snapshot_models_[snap] = current_model_;
    return snap;
  }

  void ReleaseSnapshot(const Snapshot* snap) {
    db_->ReleaseSnapshot(snap);
    snapshot_models_.erase(snap);
  }

  void VerifyGet(const ReadOptions& ropts, const std::map<std::string, std::string>& model,
                 std::string_view key) {
    std::string val;
    Status s = db_->Get(ropts, key, &val);
    auto it = model.find(std::string(key));
    if (it == model.end()) {
      EXPECT_TRUE(s.IsNotFound()) << "Expected NotFound for key: " << key
                                  << ", but got: " << s.ToString();
    } else {
      EXPECT_TRUE(s.ok()) << "Expected OK for key: " << key << ", but got: " << s.ToString();
      EXPECT_EQ(val, it->second) << "Value mismatch for key: " << key;
    }
  }

  void VerifyIteratorForward(const ReadOptions& ropts,
                             const std::map<std::string, std::string>& model) {
    auto iter = db_->NewIterator(ropts);
    ASSERT_NE(iter, nullptr);
    ASSERT_TRUE(iter->status().ok());

    auto model_it = model.begin();
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_NE(model_it, model.end()) << "Iterator has more keys than model";
      EXPECT_EQ(iter->key(), model_it->first);
      EXPECT_EQ(iter->value(), model_it->second);
      ++model_it;
    }
    EXPECT_EQ(model_it, model.end()) << "Model has more keys than iterator";
    EXPECT_TRUE(iter->status().ok());
  }

  void VerifyIteratorBackward(const ReadOptions& ropts,
                              const std::map<std::string, std::string>& model) {
    auto iter = db_->NewIterator(ropts);
    ASSERT_NE(iter, nullptr);
    ASSERT_TRUE(iter->status().ok());

    auto model_it = model.rbegin();
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_NE(model_it, model.rend()) << "Iterator has more keys than model";
      EXPECT_EQ(iter->key(), model_it->first);
      EXPECT_EQ(iter->value(), model_it->second);
      ++model_it;
    }
    EXPECT_EQ(model_it, model.rend()) << "Model has more keys than iterator";
    EXPECT_TRUE(iter->status().ok());
  }

  void VerifyIteratorSeek(const ReadOptions& ropts, const std::map<std::string, std::string>& model,
                          std::string_view target) {
    auto iter = db_->NewIterator(ropts);
    ASSERT_NE(iter, nullptr);
    iter->Seek(target);

    auto model_it = model.lower_bound(std::string(target));
    if (model_it == model.end()) {
      EXPECT_FALSE(iter->Valid()) << "Expected invalid iterator for target: " << target;
    } else {
      ASSERT_TRUE(iter->Valid()) << "Expected valid iterator for target: " << target;
      EXPECT_EQ(iter->key(), model_it->first);
      EXPECT_EQ(iter->value(), model_it->second);
    }
    EXPECT_TRUE(iter->status().ok());
  }

  Env* env_ = nullptr;
  std::string test_dir_;
  Options options_;
  std::unique_ptr<DB> db_;
  std::map<std::string, std::string> current_model_;
  std::map<const Snapshot*, std::map<std::string, std::string>> snapshot_models_;
};

std::string FormatKey(uint32_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%05u", i);
  return std::string(buf);
}

std::string RandomValue(Random* rnd, size_t len) {
  std::string res;
  res.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    res.push_back(static_cast<char>('a' + rnd->Uniform(26)));
  }
  return res;
}

TEST_F(ModelTest, IntensiveCompactRangeAndSnapshots) {
  constexpr int kNumKeys = 300;
  Random rnd(12345);

  // Phase 1: Write initial set of keys
  for (int i = 0; i < kNumKeys; ++i) {
    Put(FormatKey(i), RandomValue(&rnd, 32));
  }
  Flush();

  // Snapshot 1
  const Snapshot* snap1 = TakeSnapshot();

  // Phase 2: Overwrite even keys, delete keys divisible by 3
  for (int i = 0; i < kNumKeys; ++i) {
    if (i % 3 == 0) {
      Delete(FormatKey(i));
    } else if (i % 2 == 0) {
      Put(FormatKey(i), "overwritten_" + std::to_string(i));
    }
  }
  Flush();

  // Snapshot 2
  const Snapshot* snap2 = TakeSnapshot();

  // Phase 3: Insert new keys, delete remaining odd keys
  for (int i = 0; i < kNumKeys; ++i) {
    if (i % 2 != 0 && i % 3 != 0) {
      Delete(FormatKey(i));
    }
  }
  for (int i = kNumKeys; i < kNumKeys + 100; ++i) {
    Put(FormatKey(i), "extra_" + std::to_string(i));
  }
  Flush();

  // Compact subrange
  CompactRange(FormatKey(50), FormatKey(150));

  // Compact entire database
  CompactRange("", "");

  // Verify Snapshot 1
  ReadOptions ro1;
  ro1.snapshot = snap1;
  VerifyIteratorForward(ro1, snapshot_models_[snap1]);
  VerifyIteratorBackward(ro1, snapshot_models_[snap1]);

  // Verify Snapshot 2
  ReadOptions ro2;
  ro2.snapshot = snap2;
  VerifyIteratorForward(ro2, snapshot_models_[snap2]);
  VerifyIteratorBackward(ro2, snapshot_models_[snap2]);

  // Verify Current
  ReadOptions ro_cur;
  VerifyIteratorForward(ro_cur, current_model_);
  VerifyIteratorBackward(ro_cur, current_model_);

  ReleaseSnapshot(snap1);
  ReleaseSnapshot(snap2);
}

TEST_F(ModelTest, BidirectionalIterationFuzzing) {
  constexpr int kNumKeys = 100;
  Random rnd(54321);

  // Populate data
  for (int i = 0; i < kNumKeys; ++i) {
    Put(FormatKey(i * 2), "val_" + std::to_string(i));
  }
  // Delete some keys to create tombstones
  for (int i = 0; i < kNumKeys; i += 5) {
    Delete(FormatKey(i * 2));
  }
  Flush();

  // Convert model to vector for random indexing
  std::vector<std::pair<std::string, std::string>> model_vec(current_model_.begin(),
                                                             current_model_.end());
  ASSERT_FALSE(model_vec.empty());

  auto iter = db_->NewIterator(ReadOptions());
  ASSERT_NE(iter, nullptr);

  // Start at first
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  int cur_idx = 0;
  EXPECT_EQ(iter->key(), model_vec[cur_idx].first);
  EXPECT_EQ(iter->value(), model_vec[cur_idx].second);

  // Random walk
  for (int step = 0; step < 500; ++step) {
    const int action = static_cast<int>(rnd.Uniform(5));
    if (action == 0) {
      // SeekToFirst
      iter->SeekToFirst();
      cur_idx = 0;
      ASSERT_TRUE(iter->Valid());
      EXPECT_EQ(iter->key(), model_vec[cur_idx].first);
    } else if (action == 1) {
      // SeekToLast
      iter->SeekToLast();
      cur_idx = static_cast<int>(model_vec.size()) - 1;
      ASSERT_TRUE(iter->Valid());
      EXPECT_EQ(iter->key(), model_vec[cur_idx].first);
    } else if (action == 2) {
      // Next
      if (cur_idx < static_cast<int>(model_vec.size()) - 1) {
        iter->Next();
        ++cur_idx;
        ASSERT_TRUE(iter->Valid());
        EXPECT_EQ(iter->key(), model_vec[cur_idx].first);
        EXPECT_EQ(iter->value(), model_vec[cur_idx].second);
      } else if (cur_idx == static_cast<int>(model_vec.size()) - 1) {
        iter->Next();
        cur_idx = static_cast<int>(model_vec.size());
        EXPECT_FALSE(iter->Valid());
        // Reset to first
        iter->SeekToFirst();
        cur_idx = 0;
      }
    } else if (action == 3) {
      // Prev
      if (cur_idx > 0 && cur_idx < static_cast<int>(model_vec.size())) {
        iter->Prev();
        --cur_idx;
        ASSERT_TRUE(iter->Valid());
        EXPECT_EQ(iter->key(), model_vec[cur_idx].first);
        EXPECT_EQ(iter->value(), model_vec[cur_idx].second);
      } else if (cur_idx == 0) {
        iter->Prev();
        cur_idx = -1;
        EXPECT_FALSE(iter->Valid());
        // Reset to last
        iter->SeekToLast();
        cur_idx = static_cast<int>(model_vec.size()) - 1;
      }
    } else {
      // Random Seek
      const int target_num = static_cast<int>(rnd.Uniform(kNumKeys * 2 + 10));
      std::string target = FormatKey(target_num);
      iter->Seek(target);
      auto it = current_model_.lower_bound(target);
      if (it == current_model_.end()) {
        EXPECT_FALSE(iter->Valid());
        iter->SeekToFirst();
        cur_idx = 0;
      } else {
        ASSERT_TRUE(iter->Valid());
        EXPECT_EQ(iter->key(), it->first);
        EXPECT_EQ(iter->value(), it->second);
        cur_idx = static_cast<int>(std::distance(current_model_.begin(), it));
      }
    }
  }
}

TEST_F(ModelTest, MultiReopenCycleWithCompaction) {
  constexpr int kCycles = 5;
  Random rnd(98765);
  int key_counter = 0;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    // Add batch of keys
    for (int i = 0; i < 40; ++i) {
      Put(FormatKey(key_counter++), RandomValue(&rnd, 48));
    }

    // Overwrite / delete some existing keys
    if (!current_model_.empty()) {
      auto it = current_model_.begin();
      for (int i = 0; i < 15 && it != current_model_.end(); ++i) {
        if (i % 2 == 0) {
          Delete(it->first);
          it = current_model_.begin();
        } else {
          Put(it->first, "cycle_" + std::to_string(cycle));
          ++it;
        }
      }
    }

    if (cycle % 2 == 0) {
      Flush();
    }
    if (cycle % 3 == 0) {
      CompactRange("", "");
    }

    Reopen();

    // Verify after reopen
    VerifyIteratorForward(ReadOptions(), current_model_);
    VerifyIteratorBackward(ReadOptions(), current_model_);
  }
}

TEST_F(ModelTest, RandomizedOperationsWithOracle) {
  constexpr int kTotalOps = 1500;
  constexpr uint32_t kKeyUniverse = 120;
  Random rnd(42);

  std::vector<const Snapshot*> active_snapshots;

  for (int op = 0; op < kTotalOps; ++op) {
    const int choice = static_cast<int>(rnd.Uniform(100));

    if (choice < 45) {
      // Put (45%)
      uint32_t k = rnd.Uniform(kKeyUniverse);
      size_t vlen = 8 + rnd.Uniform(64);
      Put(FormatKey(k), RandomValue(&rnd, vlen));
    } else if (choice < 65) {
      // Delete (20%)
      uint32_t k = rnd.Uniform(kKeyUniverse);
      Delete(FormatKey(k));
    } else if (choice < 80) {
      // Get verification (15%)
      uint32_t k = rnd.Uniform(kKeyUniverse);
      ReadOptions ro;
      if (!active_snapshots.empty() && rnd.OneIn(3)) {
        ro.snapshot = active_snapshots[rnd.Uniform(static_cast<uint32_t>(active_snapshots.size()))];
        VerifyGet(ro, snapshot_models_[ro.snapshot], FormatKey(k));
      } else {
        VerifyGet(ro, current_model_, FormatKey(k));
      }
    } else if (choice < 88) {
      // Seek verification (8%)
      uint32_t k = rnd.Uniform(kKeyUniverse);
      ReadOptions ro;
      if (!active_snapshots.empty() && rnd.OneIn(3)) {
        ro.snapshot = active_snapshots[rnd.Uniform(static_cast<uint32_t>(active_snapshots.size()))];
        VerifyIteratorSeek(ro, snapshot_models_[ro.snapshot], FormatKey(k));
      } else {
        VerifyIteratorSeek(ro, current_model_, FormatKey(k));
      }
    } else if (choice < 92) {
      // Snapshot lifecycle (4%)
      if (active_snapshots.size() < 5 && (active_snapshots.empty() || rnd.OneIn(2))) {
        active_snapshots.push_back(TakeSnapshot());
      } else if (!active_snapshots.empty()) {
        size_t idx = rnd.Uniform(static_cast<uint32_t>(active_snapshots.size()));
        ReleaseSnapshot(active_snapshots[idx]);
        active_snapshots.erase(active_snapshots.begin() + static_cast<std::ptrdiff_t>(idx));
      }
    } else if (choice < 95) {
      // Flush (3%)
      Flush();
    } else if (choice < 98) {
      // CompactRange (3%)
      if (rnd.OneIn(3)) {
        CompactRange("", "");
      } else {
        uint32_t k1 = rnd.Uniform(kKeyUniverse);
        uint32_t k2 = rnd.Uniform(kKeyUniverse);
        if (k1 > k2) {
          std::swap(k1, k2);
        }
        CompactRange(FormatKey(k1), FormatKey(k2));
      }
    } else {
      // Reopen (2%)
      for (const auto* s : active_snapshots) {
        db_->ReleaseSnapshot(s);
      }
      active_snapshots.clear();
      snapshot_models_.clear();
      Reopen();
    }

    // Periodic full iterator verification
    if (op % 75 == 0) {
      VerifyIteratorForward(ReadOptions(), current_model_);
      VerifyIteratorBackward(ReadOptions(), current_model_);

      if (!active_snapshots.empty()) {
        const Snapshot* snap =
            active_snapshots[rnd.Uniform(static_cast<uint32_t>(active_snapshots.size()))];
        ReadOptions ro;
        ro.snapshot = snap;
        VerifyIteratorForward(ro, snapshot_models_[snap]);
        VerifyIteratorBackward(ro, snapshot_models_[snap]);
      }
    }
  }

  // Final verification
  VerifyIteratorForward(ReadOptions(), current_model_);
  VerifyIteratorBackward(ReadOptions(), current_model_);
  for (const auto* s : active_snapshots) {
    ReadOptions ro;
    ro.snapshot = s;
    VerifyIteratorForward(ro, snapshot_models_[s]);
    VerifyIteratorBackward(ro, snapshot_models_[s]);
    db_->ReleaseSnapshot(s);
  }
  active_snapshots.clear();
  snapshot_models_.clear();
}

}  // namespace
}  // namespace strata
