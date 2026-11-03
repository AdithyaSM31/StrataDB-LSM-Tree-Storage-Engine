#ifndef STRATA_UTIL_CACHE_H_
#define STRATA_UTIL_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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
      usage_ -= it->second->charge;
      lru_.erase(it->second);
      index_.erase(it);
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

}  // namespace strata

#endif  // STRATA_UTIL_CACHE_H_
