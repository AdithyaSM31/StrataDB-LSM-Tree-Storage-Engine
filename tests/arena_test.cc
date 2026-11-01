#include "util/arena.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "util/check.h"
#include "util/random.h"

namespace strata {
namespace {

TEST(ArenaTest, EmptyArenaUsage) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0U);
}

TEST(ArenaTest, NoOverlapUnderSkewedAllocations) {
  // Make 10,000 allocations with sizes from Random::Skewed, fill each with a byte
  // pattern derived from its index, then check every pattern is still intact.
  // Any overlap between allocations will corrupt bytes in neighboring allocations.
  Arena arena;
  Random rnd(301);
  constexpr int kNumAllocations = 10000;

  struct Allocation {
    char* ptr;
    size_t size;
    uint8_t pattern;
  };
  std::vector<Allocation> allocs;
  allocs.reserve(kNumAllocations);

  for (int i = 0; i < kNumAllocations; ++i) {
    // Skewed(11) gives sizes in [0, 2048), mostly small. Allocate requires bytes > 0.
    size_t bytes = static_cast<size_t>(rnd.Skewed(11)) + 1;
    char* p = arena.Allocate(bytes);
    ASSERT_NE(p, nullptr);

    uint8_t pattern = static_cast<uint8_t>(i & 0xFF);
    for (size_t b = 0; b < bytes; ++b) {
      p[b] = static_cast<char>(pattern ^ static_cast<uint8_t>(b & 0xFF));
    }
    allocs.push_back({p, bytes, pattern});
  }

  // Verify all patterns remain uncorrupted.
  for (size_t i = 0; i < allocs.size(); ++i) {
    const auto& alloc = allocs[i];
    for (size_t b = 0; b < alloc.size; ++b) {
      uint8_t expected = static_cast<uint8_t>(alloc.pattern ^ static_cast<uint8_t>(b & 0xFF));
      uint8_t actual = static_cast<uint8_t>(alloc.ptr[b]);
      ASSERT_EQ(actual, expected) << "Corruption detected at allocation " << i << " offset " << b;
    }
  }
}

TEST(ArenaTest, Alignment) {
  // Mix odd-sized Allocate calls with AllocateAligned calls, and check every
  // aligned pointer is a multiple of 8 (max(sizeof(void*), 8)).
  Arena arena;
  Random rnd(42);
  constexpr size_t kAlign = 8;

  for (int i = 0; i < 2000; ++i) {
    // Odd-sized unaligned allocation to deliberately throw off pointer alignment.
    size_t unaligned_bytes = static_cast<size_t>(rnd.Uniform(50) * 2) + 1;
    char* unaligned = arena.Allocate(unaligned_bytes);
    ASSERT_NE(unaligned, nullptr);

    // Aligned allocation.
    size_t aligned_bytes = static_cast<size_t>(rnd.Uniform(100)) + 1;
    char* aligned = arena.AllocateAligned(aligned_bytes);
    ASSERT_NE(aligned, nullptr);

    uintptr_t addr = reinterpret_cast<uintptr_t>(aligned);
    EXPECT_EQ(addr % kAlign, 0U) << "Unaligned address " << addr << " at iteration " << i;
  }
}

TEST(ArenaTest, LargeRequestRule) {
  // Allocate something small, then something over 1 KB, then small again.
  // The second small allocation should come right after the first, showing
  // the current block wasn’t abandoned.
  Arena arena;

  // Initial small allocation inside a fresh 4 KB block.
  char* p1 = arena.Allocate(128);
  ASSERT_NE(p1, nullptr);

  // Large allocation (> kBlockSize / 4 = 1024 bytes).
  // Must be assigned its own dedicated block without abandoning the current 4 KB block.
  constexpr size_t kLargeSize = 2048;
  char* p_large = arena.Allocate(kLargeSize);
  ASSERT_NE(p_large, nullptr);
  EXPECT_NE(p_large, p1 + 128);

  // Second small allocation must be fulfilled from the original 4 KB block
  // directly adjacent to p1.
  char* p2 = arena.Allocate(64);
  EXPECT_EQ(p2, p1 + 128);
}

TEST(ArenaTest, LargeRequestThresholdBoundary) {
  Arena arena;

  // Allocate 100 bytes from fresh block
  char* p1 = arena.Allocate(100);
  ASSERT_NE(p1, nullptr);

  // Exactly kBlockSize / 4 = 1024 bytes (<= 1024): fits within current 4 KB block.
  char* p_exact = arena.Allocate(1024);
  EXPECT_EQ(p_exact, p1 + 100);

  // Fill up more of the block using chunks <= 1024:
  char* p_fill1 = arena.Allocate(1000);
  EXPECT_EQ(p_fill1, p_exact + 1024);

  char* p_fill2 = arena.Allocate(1000);
  EXPECT_EQ(p_fill2, p_fill1 + 1000);

  char* p_fill3 = arena.Allocate(500);
  EXPECT_EQ(p_fill3, p_fill2 + 1000);
  // Total in block so far: 100 + 1024 + 1000 + 1000 + 500 = 3624 bytes.
  // Remaining: 4096 - 3624 = 472 bytes.

  // 1025 bytes (> 1024): exceeds kBlockSize / 4.
  // Must receive a dedicated block, leaving the remaining 472 bytes in the 4 KB block intact.
  char* p_large = arena.Allocate(1025);
  ASSERT_NE(p_large, nullptr);
  EXPECT_NE(p_large, p_fill3 + 500);

  // Now request 200 bytes (<= remaining 472).
  // It should come from the current 4 KB block directly after p_fill3!
  char* p_after = arena.Allocate(200);
  EXPECT_EQ(p_after, p_fill3 + 500);
}

TEST(ArenaTest, Accounting) {
  // Accounting: MemoryUsage() never decreases, is at least the total requested,
  // and isn’t wildly larger (for example, under twice the request total when the
  // requests are small).
  Arena arena;
  size_t prev_usage = arena.MemoryUsage();
  size_t total_requested = 0;
  Random rnd(101);

  for (int i = 0; i < 500; ++i) {
    size_t bytes = static_cast<size_t>(rnd.Uniform(50)) + 1;
    total_requested += bytes;
    arena.Allocate(bytes);

    size_t usage = arena.MemoryUsage();
    EXPECT_GE(usage, prev_usage) << "MemoryUsage() must never decrease";
    EXPECT_GE(usage, total_requested) << "MemoryUsage() must be at least total requested";
    prev_usage = usage;
  }

  // When requests are small and pack into 4 KB blocks, overhead should remain well
  // below 2x the requested total.
  EXPECT_LT(arena.MemoryUsage(), total_requested * 2);
}

TEST(ArenaTest, ConcurrentMemoryUsageReads) {
  // Test thread safety of MemoryUsage() under concurrent writes and reads.
  // The write leader is the single allocating thread, while another thread reads
  // MemoryUsage() concurrently (e.g. MakeRoomForWrite).
  Arena arena;
  std::atomic<bool> stop{false};

  std::thread reader([&]() {
    size_t last_seen = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      size_t usage = arena.MemoryUsage();
      EXPECT_GE(usage, last_seen);
      last_seen = usage;
    }
  });

  Random rnd(2025);
  for (int i = 0; i < 5000; ++i) {
    size_t bytes = static_cast<size_t>(rnd.Uniform(200)) + 1;
    if (rnd.OneIn(2)) {
      arena.Allocate(bytes);
    } else {
      arena.AllocateAligned(bytes);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  reader.join();
}

#if STRATA_DEBUG_CHECKS
TEST(ArenaTest, ZeroByteAllocationAborts) {
  Arena arena;
  EXPECT_DEATH(arena.Allocate(0), "check failed: bytes > 0");
  EXPECT_DEATH(arena.AllocateAligned(0), "check failed: bytes > 0");
}
#endif

}  // namespace
}  // namespace strata
