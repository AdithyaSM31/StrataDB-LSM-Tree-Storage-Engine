#include "util/random.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace strata {
namespace {

TEST(RandomTest, SeedReproducibility) {
  Random rnd1(12345);
  Random rnd2(12345);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(rnd1.Next(), rnd2.Next());
  }

  // Different seeds produce different sequences.
  Random rnd3(54321);
  bool any_diff = false;
  for (int i = 0; i < 100; ++i) {
    if (rnd1.Next() != rnd3.Next()) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(RandomTest, SeedZeroWorks) {
  Random rnd(0);
  uint64_t v1 = rnd.Next();
  uint64_t v2 = rnd.Next();
  EXPECT_NE(v1, 0ULL);
  EXPECT_NE(v1, v2);
}

TEST(RandomTest, FixedExpectedOutputsSeed42) {
  // SplitMix64 deterministic sequence test vectors for seed 42.
  // These exact values must be reproduced across all platforms, compilers,
  // and architectures forever.
  constexpr uint64_t kExpected[5] = {
      0xbdd732262feb6e95ULL, 0x28efe333b266f103ULL, 0x47526757130f9f52ULL,
      0x581ce1ff0e4ae394ULL, 0x09bc585a244823f2ULL,
  };

  Random rnd(42);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(rnd.Next(), kExpected[i]) << "Mismatch at index " << i;
  }
}

TEST(RandomTest, UniformBoundaries) {
  Random rnd(301);

  // n = 1 must always return 0.
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(rnd.Uniform(1), 0U);
  }

  // n = UINT32_MAX must always return strictly less than UINT32_MAX.
  constexpr uint32_t kMax = UINT32_MAX;
  for (int i = 0; i < 10000; ++i) {
    uint32_t u = rnd.Uniform(kMax);
    EXPECT_LT(u, kMax);
  }

  // General arbitrary bounds.
  const uint32_t test_limits[] = {2, 3, 7, 10, 16, 255, 1024, 65536, 1000000, 0x80000000U};
  for (uint32_t limit : test_limits) {
    for (int i = 0; i < 1000; ++i) {
      EXPECT_LT(rnd.Uniform(limit), limit);
    }
  }
}

TEST(RandomTest, UniformDistribution) {
  // With 100,000 draws of Uniform(10), each bucket should land within ±5%
  // of 10,000 (i.e. [9500, 10500]).
  Random rnd(1337);
  constexpr int kDraws = 100000;
  constexpr int kBuckets = 10;
  int counts[kBuckets] = {0};

  for (int i = 0; i < kDraws; ++i) {
    uint32_t b = rnd.Uniform(kBuckets);
    ASSERT_LT(b, static_cast<uint32_t>(kBuckets));
    ++counts[b];
  }

  constexpr int kExpected = kDraws / kBuckets;
  constexpr int kTolerance = kExpected * 5 / 100;  // 5% -> 500
  for (int i = 0; i < kBuckets; ++i) {
    EXPECT_GE(counts[i], kExpected - kTolerance) << "Bucket " << i << " too low";
    EXPECT_LE(counts[i], kExpected + kTolerance) << "Bucket " << i << " too high";
  }
}

TEST(RandomTest, OneInDistribution) {
  // OneIn(4) should evaluate to true about 25% of the time.
  Random rnd(2024);
  constexpr int kTrials = 100000;
  int true_count = 0;

  for (int i = 0; i < kTrials; ++i) {
    if (rnd.OneIn(4)) {
      ++true_count;
    }
  }

  // 25,000 expected. Allow ±3% of total trials (±3,000).
  EXPECT_GE(true_count, 24000);
  EXPECT_LE(true_count, 26000);

  // OneIn(1) must always be true.
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(rnd.OneIn(1));
  }
}

TEST(RandomTest, SkiplistHeightSimulation) {
  // Skiplist height simulation:
  // height = 1; while (height < kMaxHeight && rnd.OneIn(4)) ++height;
  // With a branching factor of 4:
  // P(height >= 2) = 1/4 = 0.25 (25,000 in 100k)
  // P(height >= 3) = 1/16 = 0.0625 (6,250 in 100k)
  // P(height >= 4) = 1/64 = 0.015625 (1,562.5 in 100k)
  Random rnd(9999);
  constexpr int kTrials = 100000;
  constexpr int kMaxHeight = 12;

  int ge2 = 0;
  int ge3 = 0;
  int ge4 = 0;

  for (int i = 0; i < kTrials; ++i) {
    int height = 1;
    while (height < kMaxHeight && rnd.OneIn(4)) {
      ++height;
    }
    if (height >= 2) ++ge2;
    if (height >= 3) ++ge3;
    if (height >= 4) ++ge4;
  }

  // Expected ge2: ~25000 (allow ±1000)
  EXPECT_GE(ge2, 24000);
  EXPECT_LE(ge2, 26000);

  // Expected ge3: ~6250 (allow ±500)
  EXPECT_GE(ge3, 5750);
  EXPECT_LE(ge3, 6750);

  // Expected ge4: ~1563 (allow ±250)
  EXPECT_GE(ge4, 1313);
  EXPECT_LE(ge4, 1813);
}

TEST(RandomTest, SkewedDistribution) {
  Random rnd(777);

  // Skewed(0) must always return 0 (1 << 0 = 1, Uniform(1) = 0).
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(rnd.Skewed(0), 0U);
  }

  // Skewed(max_log) values must all be strictly less than 1 << max_log.
  for (int max_log = 1; max_log <= 16; ++max_log) {
    uint32_t limit = 1U << max_log;
    for (int i = 0; i < 200; ++i) {
      EXPECT_LT(rnd.Skewed(max_log), limit);
    }
  }

  // Boundary max_log = 31 must work without undefined behavior or overflow.
  for (int i = 0; i < 200; ++i) {
    EXPECT_LT(rnd.Skewed(31), 0x80000000U);
  }

  // Check skewed property: in Skewed(10), values should be heavily concentrated
  // at the lower end.
  // With max_log = 10, range is [0, 1024). Half the choices of b are <= 5 (values < 32).
  int small_count = 0;
  constexpr int kTrials = 10000;
  for (int i = 0; i < kTrials; ++i) {
    if (rnd.Skewed(10) < 64) {
      ++small_count;
    }
  }
  // At least 60% of values should be < 64 despite upper range reaching 1023.
  EXPECT_GT(small_count, 6000);
}

#if STRATA_DEBUG_CHECKS
TEST(RandomTest, InvalidArgumentsAbort) {
  Random rnd(1234);
  EXPECT_DEATH(rnd.Uniform(0), "check failed: n > 0");
  EXPECT_DEATH(rnd.OneIn(0), "check failed: n > 0");
  EXPECT_DEATH(rnd.Skewed(-1), "check failed: max_log >= 0");
  EXPECT_DEATH(rnd.Skewed(32), "check failed: max_log <= 31");
}
#endif

}  // namespace
}  // namespace strata
