#ifndef STRATA_UTIL_CACHE_H_
#define STRATA_UTIL_CACHE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "util/hash.h"

namespace strata {

// An LRU cache shard mapping string keys to shared_ptr<V>.
//
// Thread safety: all public methods are thread-safe.
// Eviction is based on "charge": each entry has an associated size in bytes or
// count, and the least recently used entries are evicted when total charge
// exceeds capacity. Pinned entries (held by callers via shared_ptr) remain
// valid after eviction, but their charge no longer counts against usage.
template <typename V>
class LRUCache {
 public:
  explicit LRUCache(size_t capacity) : capacity_(capacity), usage_(0) {}
  ~LRUCache() = default;

  LRUCache(const LRUCache&) = delete;
  LRUCache& operator=(const LRUCache&) = delete;
  LRUCache(LRUCache&&) = delete;
  LRUCache& operator=(LRUCache&&) = delete;

  // Looks up `key`. On a hit, moves the entry to the front (most recent) and
  // returns the value. On a miss, returns nullptr.
  std::shared_ptr<V> Lookup(std::string_view key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it == index_.end()) {
      return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->value;
  }

  // Inserts `key` -> `value` with `charge`.
  //
  // If `key` already exists, keeps the existing value and returns it
  // ("first one wins").
  // If `capacity` is 0 or `charge > capacity`, returns `value` without caching it.
  // Evicts least recently used entries from the back while usage > capacity.
  std::shared_ptr<V> Insert(std::string_view key, std::shared_ptr<V> value, size_t charge) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second);
      return it->second->value;
    }

    if (capacity_ == 0 || charge > capacity_) {
      return value;
    }

    lru_.push_front(Entry{std::string(key), value, charge});
    auto new_it = lru_.begin();
    index_[std::string_view(new_it->key)] = new_it;
    usage_ += charge;

    while (usage_ > capacity_ && !lru_.empty()) {
      auto last_it = std::prev(lru_.end());
      usage_ -= last_it->charge;
      index_.erase(last_it->key);
      lru_.pop_back();
    }

    return value;
  }

  // Erases `key` from the cache if present, reducing total charge.
  void Erase(std::string_view key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
      auto lru_it = it->second;
      usage_ -= lru_it->charge;
      index_.erase(it);
      lru_.erase(lru_it);
    }
  }

  // Current total charge of all entries held in this cache shard.
  size_t TotalCharge() const {
    std::lock_guard<std::mutex> lock(mu_);
    return usage_;
  }

  size_t capacity() const { return capacity_; }

 private:
  struct Entry {
    std::string key;
    std::shared_ptr<V> value;
    size_t charge;
  };

  mutable std::mutex mu_;
  std::list<Entry> lru_;
  std::unordered_map<std::string_view, typename std::list<Entry>::iterator> index_;
  size_t capacity_;
  size_t usage_ = 0;
};

// A thread-safe LRU cache sharded into 16 independent LRUCache shards to reduce
// lock contention across concurrent readers and writers.
template <typename V>
class ShardedLRUCache {
 public:
  static constexpr int kNumShardBits = 4;
  static constexpr int kNumShards = 1 << kNumShardBits;  // 16

  explicit ShardedLRUCache(size_t capacity) : capacity_(capacity) {
    const size_t per_shard = (capacity + kNumShards - 1) / kNumShards;
    for (int i = 0; i < kNumShards; ++i) {
      shards_[i] = std::make_unique<LRUCache<V>>(per_shard);
    }
  }

  ~ShardedLRUCache() = default;

  ShardedLRUCache(const ShardedLRUCache&) = delete;
  ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;
  ShardedLRUCache(ShardedLRUCache&&) = delete;
  ShardedLRUCache& operator=(ShardedLRUCache&&) = delete;

  // Looks up `key` in the appropriate shard. Updates hit/miss counters.
  std::shared_ptr<V> Lookup(std::string_view key) {
    const size_t s = Shard(key);
    auto val = shards_[s]->Lookup(key);
    if (val != nullptr) {
      hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return val;
  }

  // Inserts `key` -> `value` into the appropriate shard. Updates insert counter.
  std::shared_ptr<V> Insert(std::string_view key, std::shared_ptr<V> value, size_t charge) {
    inserts_.fetch_add(1, std::memory_order_relaxed);
    const size_t s = Shard(key);
    return shards_[s]->Insert(key, std::move(value), charge);
  }

  // Erases `key` from the appropriate shard if present.
  void Erase(std::string_view key) {
    const size_t s = Shard(key);
    shards_[s]->Erase(key);
  }

  // Total charge currently held across all 16 shards.
  size_t TotalCharge() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
      total += shard->TotalCharge();
    }
    return total;
  }

  // Returns a new unique 64-bit identifier for cache keys (e.g. table cache_id).
  uint64_t NewId() { return last_id_.fetch_add(1, std::memory_order_relaxed) + 1; }

  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  uint64_t inserts() const { return inserts_.load(std::memory_order_relaxed); }
  size_t capacity() const { return capacity_; }

  LRUCache<V>* shard(size_t index) { return shards_[index].get(); }

 private:
  static size_t Shard(std::string_view key) { return Hash(key) >> (32 - kNumShardBits); }

  const size_t capacity_;
  std::array<std::unique_ptr<LRUCache<V>>, kNumShards> shards_;
  mutable std::atomic<uint64_t> hits_{0};
  mutable std::atomic<uint64_t> misses_{0};
  mutable std::atomic<uint64_t> inserts_{0};
  std::atomic<uint64_t> last_id_{0};
};

}  // namespace strata

#endif  // STRATA_UTIL_CACHE_H_
