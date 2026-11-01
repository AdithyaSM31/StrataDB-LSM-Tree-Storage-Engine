#include "memtable/skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "util/arena.h"
#include "util/check.h"
#include "util/hash.h"
#include "util/random.h"

namespace strata {
namespace {

struct Uint64Comparator {
  int operator()(uint64_t a, uint64_t b) const {
    if (a < b) {
      return -1;
    }
    if (a > b) {
      return 1;
    }
    return 0;
  }
};

struct CStringComparator {
  int operator()(const char* a, const char* b) const { return std::strcmp(a, b); }
};

TEST(SkipListTest, EmptyList) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);

  EXPECT_FALSE(list.Contains(0));
  EXPECT_FALSE(list.Contains(1));
  EXPECT_FALSE(list.Contains(100));
  EXPECT_FALSE(list.Contains(UINT64_MAX));

  SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
  EXPECT_FALSE(iter.Valid());

  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());

  iter.SeekToLast();
  EXPECT_FALSE(iter.Valid());

  iter.Seek(0);
  EXPECT_FALSE(iter.Valid());

  iter.Seek(100);
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipListTest, Insert2000RandomDistinctKeysAndIterate) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  std::set<uint64_t> ref_set;

  Random rnd(1001);
  constexpr int kNumKeys = 2000;

  while (ref_set.size() < kNumKeys) {
    uint64_t k = rnd.Next();
    if (ref_set.insert(k).second) {
      list.Insert(k);
    }
  }

  // 1. Contains is true for every inserted key.
  for (uint64_t k : ref_set) {
    EXPECT_TRUE(list.Contains(k));
  }

  // 2. Contains is false for keys that were not inserted.
  for (int i = 0; i < 1000; ++i) {
    uint64_t k = rnd.Next();
    if (ref_set.find(k) == ref_set.end()) {
      EXPECT_FALSE(list.Contains(k));
    }
  }

  // 3. Forward iteration matches the set's order.
  {
    SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
    iter.SeekToFirst();
    for (auto it = ref_set.begin(); it != ref_set.end(); ++it) {
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(iter.key(), *it);
      iter.Next();
    }
    EXPECT_FALSE(iter.Valid());
  }

  // 4. Backward iteration with Prev matches its reverse order.
  {
    SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
    iter.SeekToLast();
    for (auto it = ref_set.rbegin(); it != ref_set.rend(); ++it) {
      ASSERT_TRUE(iter.Valid());
      EXPECT_EQ(iter.key(), *it);
      iter.Prev();
    }
    EXPECT_FALSE(iter.Valid());
  }
}

TEST(SkipListTest, SeekLandsOnLowerBound) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  std::set<uint64_t> ref_set;

  Random rnd(2023);
  constexpr int kNumKeys = 1000;

  // Insert numbers in a spread out range (e.g. multiples of 10 plus random jitter)
  while (ref_set.size() < kNumKeys) {
    uint64_t k = (rnd.Uniform(100000) * 10) + 1000;
    if (ref_set.insert(k).second) {
      list.Insert(k);
    }
  }

  uint64_t min_key = *ref_set.begin();
  uint64_t max_key = *ref_set.rbegin();

  SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);

  // 1. Below the smallest key: should land on min_key.
  iter.Seek(0);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), min_key);

  if (min_key > 1) {
    iter.Seek(min_key - 1);
    ASSERT_TRUE(iter.Valid());
    EXPECT_EQ(iter.key(), min_key);
  }

  // 2. Above the largest key: iterator should become invalid.
  iter.Seek(max_key + 1);
  EXPECT_FALSE(iter.Valid());

  iter.Seek(UINT64_MAX);
  EXPECT_FALSE(iter.Valid());

  // 3. For existing keys and points in-between: verify against std::set::lower_bound.
  for (int i = 0; i < 2000; ++i) {
    uint64_t target = rnd.Uniform(1200000);
    auto expected_it = ref_set.lower_bound(target);

    iter.Seek(target);
    if (expected_it == ref_set.end()) {
      EXPECT_FALSE(iter.Valid()) << "Expected invalid for target " << target;
    } else {
      ASSERT_TRUE(iter.Valid()) << "Expected valid for target " << target;
      EXPECT_EQ(iter.key(), *expected_it) << "Mismatch for target " << target;
    }
  }
}

#if STRATA_DEBUG_CHECKS
TEST(SkipListTest, NullArenaAborts) {
  EXPECT_DEATH((SkipList<uint64_t, Uint64Comparator>(Uint64Comparator{}, nullptr)),
               "check failed:");
}

TEST(SkipListTest, DuplicateKeyAborts) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);

  list.Insert(100);
  EXPECT_TRUE(list.Contains(100));

  EXPECT_DEATH(list.Insert(100), "check failed:");
}

TEST(SkipListTest, InvalidIteratorAccessAborts) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);

  EXPECT_FALSE(iter.Valid());
  EXPECT_DEATH(iter.key(), "check failed:");
  EXPECT_DEATH(iter.Next(), "check failed:");
  EXPECT_DEATH(iter.Prev(), "check failed:");

  EXPECT_DEATH((SkipList<uint64_t, Uint64Comparator>::Iterator(nullptr)), "check failed:");
}
#endif

TEST(SkipListTest, IteratorsSurviveInterleavedInsertions) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);

  list.Insert(10);
  list.Insert(30);
  list.Insert(50);

  SkipList<uint64_t, Uint64Comparator>::Iterator iter1(&list);
  iter1.SeekToFirst();
  ASSERT_TRUE(iter1.Valid());
  EXPECT_EQ(iter1.key(), 10U);

  // Insert 20 between 10 and 30
  list.Insert(20);

  // Advancing iter1 should now see 20
  iter1.Next();
  ASSERT_TRUE(iter1.Valid());
  EXPECT_EQ(iter1.key(), 20U);

  iter1.Next();
  ASSERT_TRUE(iter1.Valid());
  EXPECT_EQ(iter1.key(), 30U);

  // Insert 40 between 30 and 50 while iter1 is at 30
  list.Insert(40);
  iter1.Next();
  ASSERT_TRUE(iter1.Valid());
  EXPECT_EQ(iter1.key(), 40U);

  // Backward navigation with interleaved insertion
  SkipList<uint64_t, Uint64Comparator>::Iterator iter2(&list);
  iter2.SeekToLast();
  ASSERT_TRUE(iter2.Valid());
  EXPECT_EQ(iter2.key(), 50U);

  // Insert 60 at the end
  list.Insert(60);

  // iter2 was at 50; calling Prev should go to 40
  iter2.Prev();
  ASSERT_TRUE(iter2.Valid());
  EXPECT_EQ(iter2.key(), 40U);

  // Insert 5 at the beginning
  list.Insert(5);
  iter2.Prev();
  EXPECT_EQ(iter2.key(), 30U);
  iter2.Prev();
  EXPECT_EQ(iter2.key(), 20U);
  iter2.Prev();
  EXPECT_EQ(iter2.key(), 10U);
  iter2.Prev();
  EXPECT_EQ(iter2.key(), 5U);
  iter2.Prev();
  EXPECT_FALSE(iter2.Valid());
}

TEST(SkipListTest, CStringPointerComparator) {
  Arena arena;
  SkipList<const char*, CStringComparator> list(CStringComparator{}, &arena);

  // Allocate distinct memory copies in the arena for same content strings
  auto make_str = [&](const char* s) -> const char* {
    size_t len = std::strlen(s) + 1;
    char* p = arena.Allocate(len);
    std::memcpy(p, s, len);
    return p;
  };

  const char* apple1 = make_str("apple");
  const char* banana1 = make_str("banana");
  const char* cherry1 = make_str("cherry");

  list.Insert(apple1);
  list.Insert(banana1);
  list.Insert(cherry1);

  // Create separate pointers with identical contents
  const char* apple2 = make_str("apple");
  const char* banana2 = make_str("banana");
  const char* cherry2 = make_str("cherry");
  const char* durian = make_str("durian");

  // Pointers must differ
  ASSERT_NE(apple1, apple2);
  ASSERT_NE(banana1, banana2);
  ASSERT_NE(cherry1, cherry2);

  // Contains should match via CStringComparator, not pointer equality
  EXPECT_TRUE(list.Contains(apple2));
  EXPECT_TRUE(list.Contains(banana2));
  EXPECT_TRUE(list.Contains(cherry2));
  EXPECT_FALSE(list.Contains(durian));

  SkipList<const char*, CStringComparator>::Iterator iter(&list);
  iter.Seek(banana2);
  ASSERT_TRUE(iter.Valid());
  EXPECT_STREQ(iter.key(), "banana");

#if STRATA_DEBUG_CHECKS
  // Inserting duplicate by content must abort
  EXPECT_DEATH(list.Insert(apple2), "check failed:");
#endif
}

TEST(SkipListTest, SingleElementList) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);

  list.Insert(42);
  EXPECT_TRUE(list.Contains(42));
  EXPECT_FALSE(list.Contains(41));
  EXPECT_FALSE(list.Contains(43));

  SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);

  // SeekToFirst
  iter.SeekToFirst();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);
  iter.Next();
  EXPECT_FALSE(iter.Valid());

  // SeekToFirst and Prev
  iter.SeekToFirst();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);
  iter.Prev();
  EXPECT_FALSE(iter.Valid());

  // SeekToLast
  iter.SeekToLast();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);
  iter.Prev();
  EXPECT_FALSE(iter.Valid());

  // SeekToLast and Next
  iter.SeekToLast();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);
  iter.Next();
  EXPECT_FALSE(iter.Valid());

  // Seek
  iter.Seek(0);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);

  iter.Seek(42);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 42U);

  iter.Seek(43);
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipListTest, BoundaryKeys) {
  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);

  list.Insert(0);
  list.Insert(100);
  list.Insert(UINT64_MAX);

  EXPECT_TRUE(list.Contains(0));
  EXPECT_TRUE(list.Contains(100));
  EXPECT_TRUE(list.Contains(UINT64_MAX));
  EXPECT_FALSE(list.Contains(1));
  EXPECT_FALSE(list.Contains(UINT64_MAX - 1));

  SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);

  iter.Seek(0);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 0U);

  iter.Seek(UINT64_MAX);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), UINT64_MAX);

  // Forward iterate: 0, 100, UINT64_MAX
  iter.SeekToFirst();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 0U);
  iter.Next();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 100U);
  iter.Next();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), UINT64_MAX);
  iter.Next();
  EXPECT_FALSE(iter.Valid());

  // Reverse iterate: UINT64_MAX, 100, 0
  iter.SeekToLast();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), UINT64_MAX);
  iter.Prev();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 100U);
  iter.Prev();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), 0U);
  iter.Prev();
  EXPECT_FALSE(iter.Valid());
}

// ---------------------------------------------------------------------------
// Concurrent Tests: One Writer, Four Readers
// ---------------------------------------------------------------------------

inline uint64_t MakeConcurrentKey(uint32_t i) {
  uint32_t h = strata::Hash(reinterpret_cast<const char*>(&i), sizeof(i), 0);
  return (static_cast<uint64_t>(i) << 32) | static_cast<uint64_t>(h);
}

inline bool ChecksumValid(uint64_t key) {
  uint32_t i = static_cast<uint32_t>(key >> 32);
  uint32_t expected_h = strata::Hash(reinterpret_cast<const char*>(&i), sizeof(i), 0);
  uint32_t actual_h = static_cast<uint32_t>(key & 0xFFFFFFFFULL);
  return (expected_h == actual_h);
}

TEST(SkipListTest, ConcurrentOneWriterFourReaders) {
  constexpr size_t kNumKeys = 10000;
  constexpr int kNumReaders = 4;

  std::vector<uint64_t> keys(kNumKeys);
  for (size_t i = 0; i < kNumKeys; ++i) {
    keys[i] = MakeConcurrentKey(static_cast<uint32_t>(i));
  }

  // Shuffle keys using a fixed-seed Random for reproducibility
  Random rnd(42);
  for (size_t i = kNumKeys - 1; i > 0; --i) {
    size_t j = rnd.Uniform(static_cast<uint32_t>(i + 1));
    std::swap(keys[i], keys[j]);
  }

  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  std::atomic<size_t> inserted_count{0};
  std::atomic<bool> start{false};

  std::atomic<int> readers_ready{0};
  std::vector<std::thread> readers;
  readers.reserve(kNumReaders);
  for (int r = 0; r < kNumReaders; ++r) {
    readers.emplace_back([&]() {
      readers_ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }

      std::vector<uint8_t> seen(kNumKeys, 0);
      while (true) {
        size_t done = inserted_count.load(std::memory_order_acquire);
        std::fill(seen.begin(), seen.end(), 0);

        SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
        iter.SeekToFirst();
        uint64_t prev = 0;
        bool first = true;
        size_t observed_count = 0;

        while (iter.Valid()) {
          uint64_t k = iter.key();
          // 1. Checksum must be valid (no torn/garbage reads)
          ASSERT_TRUE(ChecksumValid(k));
          // 2. Keys must be strictly increasing
          if (!first) {
            ASSERT_GT(k, prev);
          }
          first = false;
          prev = k;

          uint32_t idx = static_cast<uint32_t>(k >> 32);
          ASSERT_LT(idx, static_cast<uint32_t>(kNumKeys));
          seen[idx] = 1;
          ++observed_count;

          iter.Next();
        }

        // 3. Every guaranteed key was seen
        ASSERT_GE(observed_count, done);
        for (size_t i = 0; i < done; ++i) {
          uint32_t guaranteed_idx = static_cast<uint32_t>(keys[i] >> 32);
          ASSERT_EQ(seen[guaranteed_idx], 1);
        }

        if (done >= kNumKeys) {
          break;
        }
      }
    });
  }

  std::thread writer([&]() {
    while (readers_ready.load(std::memory_order_acquire) < kNumReaders) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_relaxed);
    for (size_t i = 0; i < kNumKeys; ++i) {
      list.Insert(keys[i]);
      inserted_count.store(i + 1, std::memory_order_release);
    }
  });

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
}

TEST(SkipListTest, ConcurrentContainsAndSeek) {
  constexpr size_t kNumKeys = 10000;
  constexpr int kNumReaders = 4;

  std::vector<uint64_t> keys(kNumKeys);
  for (size_t i = 0; i < kNumKeys; ++i) {
    keys[i] = MakeConcurrentKey(static_cast<uint32_t>(i));
  }

  Random rnd(1337);
  for (size_t i = kNumKeys - 1; i > 0; --i) {
    size_t j = rnd.Uniform(static_cast<uint32_t>(i + 1));
    std::swap(keys[i], keys[j]);
  }

  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  std::atomic<size_t> inserted_count{0};
  std::atomic<bool> start{false};
  std::atomic<int> readers_ready{0};

  std::vector<std::thread> readers;
  readers.reserve(kNumReaders);
  for (int r = 0; r < kNumReaders; ++r) {
    readers.emplace_back([&, r]() {
      Random reader_rnd(4242 + r);
      readers_ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }

      while (true) {
        size_t done = inserted_count.load(std::memory_order_acquire);
        if (done > 0) {
          for (int probe = 0; probe < 10; ++probe) {
            size_t sample_idx = reader_rnd.Uniform(static_cast<uint32_t>(done));
            uint64_t target = keys[sample_idx];
            ASSERT_TRUE(list.Contains(target));

            SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
            iter.Seek(target);
            ASSERT_TRUE(iter.Valid());
            ASSERT_EQ(iter.key(), target);
          }

          // An invalid key (bad checksum) should not be present
          uint64_t fake_key = (static_cast<uint64_t>(kNumKeys + 100) << 32) | 0xDEADBEEF;
          ASSERT_FALSE(list.Contains(fake_key));
        }

        if (done >= kNumKeys) {
          break;
        }
      }
    });
  }

  std::thread writer([&]() {
    while (readers_ready.load(std::memory_order_acquire) < kNumReaders) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_relaxed);
    for (size_t i = 0; i < kNumKeys; ++i) {
      list.Insert(keys[i]);
      inserted_count.store(i + 1, std::memory_order_release);
    }
  });

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
}

TEST(SkipListTest, ConcurrentReverseIteration) {
  constexpr size_t kNumKeys = 10000;
  constexpr int kNumReaders = 4;

  std::vector<uint64_t> keys(kNumKeys);
  for (size_t i = 0; i < kNumKeys; ++i) {
    keys[i] = MakeConcurrentKey(static_cast<uint32_t>(i));
  }

  Random rnd(9999);
  for (size_t i = kNumKeys - 1; i > 0; --i) {
    size_t j = rnd.Uniform(static_cast<uint32_t>(i + 1));
    std::swap(keys[i], keys[j]);
  }

  Arena arena;
  SkipList<uint64_t, Uint64Comparator> list(Uint64Comparator{}, &arena);
  std::atomic<size_t> inserted_count{0};
  std::atomic<bool> start{false};
  std::atomic<int> readers_ready{0};

  std::vector<std::thread> readers;
  readers.reserve(kNumReaders);
  for (int r = 0; r < kNumReaders; ++r) {
    readers.emplace_back([&]() {
      readers_ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }

      while (true) {
        size_t done = inserted_count.load(std::memory_order_acquire);

        SkipList<uint64_t, Uint64Comparator>::Iterator iter(&list);
        iter.SeekToLast();
        uint64_t prev = UINT64_MAX;
        bool first = true;
        size_t reverse_observed = 0;

        while (iter.Valid()) {
          uint64_t k = iter.key();
          // 1. Checksum must be valid (no torn/garbage reads)
          ASSERT_TRUE(ChecksumValid(k));
          // 2. Keys must be strictly decreasing
          if (!first) {
            ASSERT_LT(k, prev);
          }
          first = false;
          prev = k;
          ++reverse_observed;

          iter.Prev();
        }

        if (done > 0) {
          ASSERT_GT(reverse_observed, 0U);
        }

        if (done >= kNumKeys) {
          ASSERT_EQ(reverse_observed, kNumKeys);
          break;
        }
      }
    });
  }

  std::thread writer([&]() {
    while (readers_ready.load(std::memory_order_acquire) < kNumReaders) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_relaxed);
    for (size_t i = 0; i < kNumKeys; ++i) {
      list.Insert(keys[i]);
      inserted_count.store(i + 1, std::memory_order_release);
    }
  });

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
}

}  // namespace
}  // namespace strata
