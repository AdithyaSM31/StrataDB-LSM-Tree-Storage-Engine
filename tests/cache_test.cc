#include "util/cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "util/random.h"

namespace strata {
namespace {

TEST(LRUCacheTest, BasicsMissInsertHitErase) {
  LRUCache<std::string> cache(100);
  EXPECT_EQ(cache.TotalCharge(), 0u);

  // Miss
  EXPECT_EQ(cache.Lookup("missing"), nullptr);

  // Insert
  auto val1 = std::make_shared<std::string>("hello");
  auto inserted = cache.Insert("key1", val1, 20);
  EXPECT_EQ(inserted, val1);
  EXPECT_EQ(cache.TotalCharge(), 20u);

  // Hit
  auto hit = cache.Lookup("key1");
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(*hit, "hello");

  // Erase
  cache.Erase("key1");
  EXPECT_EQ(cache.Lookup("key1"), nullptr);
  EXPECT_EQ(cache.TotalCharge(), 0u);

  // Erasing non-existent key is a no-op
  cache.Erase("key1");
  EXPECT_EQ(cache.TotalCharge(), 0u);
}

TEST(LRUCacheTest, LRUOrderEviction) {
  // Capacity 3 entries, each with charge 1
  LRUCache<std::string> cache(3);
  cache.Insert("k1", std::make_shared<std::string>("v1"), 1);
  cache.Insert("k2", std::make_shared<std::string>("v2"), 1);
  cache.Insert("k3", std::make_shared<std::string>("v3"), 1);
  EXPECT_EQ(cache.TotalCharge(), 3u);

  // Touch the oldest entry (k1). It moves to the front (most recent).
  auto hit = cache.Lookup("k1");
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(*hit, "v1");

  // Insert k4. Cache usage would be 4 > 3, so the least recently used
  // entry must be evicted. Since k1 was touched, the least recently used is k2.
  cache.Insert("k4", std::make_shared<std::string>("v4"), 1);
  EXPECT_EQ(cache.TotalCharge(), 3u);

  // k1 survived
  EXPECT_NE(cache.Lookup("k1"), nullptr);
  // k2 was evicted
  EXPECT_EQ(cache.Lookup("k2"), nullptr);
  // k3 survived
  EXPECT_NE(cache.Lookup("k3"), nullptr);
  // k4 survived
  EXPECT_NE(cache.Lookup("k4"), nullptr);
}

TEST(LRUCacheTest, ChargeEviction) {
  LRUCache<std::string> cache(100);
  cache.Insert("a", std::make_shared<std::string>("alpha"), 40);
  cache.Insert("b", std::make_shared<std::string>("beta"), 40);
  EXPECT_EQ(cache.TotalCharge(), 80u);

  // Insert 3rd entry with charge 40. Total would be 120 > 100.
  // "a" (oldest) should be evicted; total charge becomes 80 <= 100.
  cache.Insert("c", std::make_shared<std::string>("gamma"), 40);
  EXPECT_LE(cache.TotalCharge(), 100u);
  EXPECT_EQ(cache.TotalCharge(), 80u);

  EXPECT_EQ(cache.Lookup("a"), nullptr);
  EXPECT_NE(cache.Lookup("b"), nullptr);
  EXPECT_NE(cache.Lookup("c"), nullptr);
}

TEST(LRUCacheTest, Pinning) {
  LRUCache<std::string> cache(1);
  std::weak_ptr<std::string> weak_v1;

  std::shared_ptr<std::string> pinned;
  {
    auto v1 = std::make_shared<std::string>("keep_alive_even_when_evicted");
    weak_v1 = v1;
    cache.Insert("k1", v1, 1);
    pinned = cache.Lookup("k1");
    ASSERT_NE(pinned, nullptr);
    EXPECT_EQ(*pinned, "keep_alive_even_when_evicted");
  }

  // Force eviction of k1 by inserting k2
  cache.Insert("k2", std::make_shared<std::string>("v2"), 1);
  EXPECT_EQ(cache.Lookup("k1"), nullptr);

  // pinned is still valid and weak_v1 has not expired
  EXPECT_FALSE(weak_v1.expired());
  EXPECT_EQ(*pinned, "keep_alive_even_when_evicted");

  // Drop our reference
  pinned.reset();
  EXPECT_TRUE(weak_v1.expired());
}

TEST(LRUCacheTest, EdgeCases) {
  // Capacity 0 keeps nothing
  {
    LRUCache<std::string> cache(0);
    auto val = std::make_shared<std::string>("instant_evict");
    auto inserted = cache.Insert("k1", val, 10);
    EXPECT_EQ(inserted, val);
    EXPECT_EQ(cache.Lookup("k1"), nullptr);
    EXPECT_EQ(cache.TotalCharge(), 0u);
  }

  // Oversized entry isn't kept and does not evict existing entries
  {
    LRUCache<std::string> cache(50);
    cache.Insert("existing", std::make_shared<std::string>("val"), 20);
    EXPECT_EQ(cache.TotalCharge(), 20u);

    auto big = std::make_shared<std::string>("big");
    auto inserted = cache.Insert("oversized", big, 60);
    EXPECT_EQ(inserted, big);
    EXPECT_EQ(cache.Lookup("oversized"), nullptr);
    EXPECT_NE(cache.Lookup("existing"), nullptr);
    EXPECT_EQ(cache.TotalCharge(), 20u);
  }

  // Duplicate insert returns the first value ("first one wins")
  {
    LRUCache<std::string> cache(100);
    auto val1 = std::make_shared<std::string>("first");
    auto val2 = std::make_shared<std::string>("second");

    auto inserted1 = cache.Insert("k", val1, 10);
    EXPECT_EQ(inserted1, val1);
    EXPECT_EQ(cache.TotalCharge(), 10u);

    auto inserted2 = cache.Insert("k", val2, 25);
    EXPECT_EQ(inserted2, val1);  // first one wins
    EXPECT_EQ(cache.TotalCharge(), 10u);

    auto hit = cache.Lookup("k");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(*hit, "first");
  }
}

TEST(LRUCacheTest, Concurrency) {
  constexpr size_t kCapacity = 1000;
  LRUCache<std::string> cache(kCapacity);
  constexpr int kNumThreads = 8;
  constexpr int kOpsPerThread = 5000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      Random rnd(1000 + t);
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key_num = rnd.Next() % 200;
        std::string key = "key_" + std::to_string(key_num);
        int op = rnd.Next() % 3;
        if (op == 0) {
          cache.Lookup(key);
        } else if (op == 1) {
          auto val = std::make_shared<std::string>("val_" + std::to_string(key_num));
          size_t charge = 5 + (rnd.Next() % 20);
          cache.Insert(key, val, charge);
        } else {
          cache.Erase(key);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_LE(cache.TotalCharge(), kCapacity);
}

TEST(ShardedLRUCacheTest, CountersAndBasics) {
  ShardedLRUCache<std::string> cache(1600);
  EXPECT_EQ(cache.hits(), 0u);
  EXPECT_EQ(cache.misses(), 0u);
  EXPECT_EQ(cache.inserts(), 0u);

  // Miss
  EXPECT_EQ(cache.Lookup("missing"), nullptr);
  EXPECT_EQ(cache.hits(), 0u);
  EXPECT_EQ(cache.misses(), 1u);

  // Insert
  auto val1 = std::make_shared<std::string>("hello");
  auto inserted = cache.Insert("key1", val1, 20);
  EXPECT_EQ(inserted, val1);
  EXPECT_EQ(cache.inserts(), 1u);
  EXPECT_EQ(cache.TotalCharge(), 20u);

  // Hit
  auto hit = cache.Lookup("key1");
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(*hit, "hello");
  EXPECT_EQ(cache.hits(), 1u);
  EXPECT_EQ(cache.misses(), 1u);

  // Erase
  cache.Erase("key1");
  EXPECT_EQ(cache.Lookup("key1"), nullptr);
  EXPECT_EQ(cache.hits(), 1u);
  EXPECT_EQ(cache.misses(), 2u);
  EXPECT_EQ(cache.TotalCharge(), 0u);
}

TEST(ShardedLRUCacheTest, NewIdIsMonotonicAndUnique) {
  ShardedLRUCache<std::string> cache(100);
  uint64_t id1 = cache.NewId();
  uint64_t id2 = cache.NewId();
  uint64_t id3 = cache.NewId();
  EXPECT_GT(id1, 0u);
  EXPECT_EQ(id2, id1 + 1);
  EXPECT_EQ(id3, id2 + 1);
}

TEST(ShardedLRUCacheTest, ShardEvictionAndCapacity) {
  // 16 shards, each with capacity ceil(160 / 16) = 10
  ShardedLRUCache<std::string> cache(160);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(cache.shard(i)->capacity(), 10u);
  }

  // Insert items with charge 1
  for (int i = 0; i < 300; ++i) {
    std::string key = "item_" + std::to_string(i);
    cache.Insert(key, std::make_shared<std::string>("data"), 1);
  }

  // Each shard has capacity 10, so total charge cannot exceed 16 * 10 = 160
  EXPECT_LE(cache.TotalCharge(), 160u);
}

TEST(ShardedLRUCacheTest, Concurrency) {
  constexpr size_t kCapacity = 1600;
  ShardedLRUCache<std::string> cache(kCapacity);
  constexpr int kNumThreads = 8;
  constexpr int kOpsPerThread = 5000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      Random rnd(2000 + t);
      for (int i = 0; i < kOpsPerThread; ++i) {
        int key_num = rnd.Next() % 500;
        std::string key = "key_" + std::to_string(key_num);
        int op = rnd.Next() % 3;
        if (op == 0) {
          cache.Lookup(key);
        } else if (op == 1) {
          auto val = std::make_shared<std::string>("val_" + std::to_string(key_num));
          size_t charge = 1 + (rnd.Next() % 10);
          cache.Insert(key, val, charge);
        } else {
          cache.Erase(key);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // 16 shards, each capacity 100. Total charge <= 1600
  EXPECT_LE(cache.TotalCharge(), 1600u);
  EXPECT_GT(cache.hits() + cache.misses(), 0u);
  EXPECT_GT(cache.inserts(), 0u);
}

}  // namespace
}  // namespace strata
