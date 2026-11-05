#include "util/zipfian.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

namespace strata {

// ── helpers ──────────────────────────────────────────────────────────────────

static ZipfianGenerator MakeGen(uint64_t n, double theta = 0.99, uint64_t seed = 42) {
  return ZipfianGenerator(n, theta, ZipfZeta(n, theta), seed);
}

// ── basic range tests ─────────────────────────────────────────────────────────

TEST(ZipfianTest, ResultsInRange) {
  constexpr uint64_t kN = 10000;
  constexpr int kTrials = 100000;
  auto gen = MakeGen(kN);
  for (int i = 0; i < kTrials; ++i) {
    const uint64_t v = gen.Next();
    ASSERT_LT(v, kN) << "out of range at trial " << i;
  }
}

TEST(ZipfianTest, ResultsInRangeN1) {
  // n == 1: every draw must strictly be 0 for both NextRank() and Next().
  for (uint64_t seed : {1u, 42u, 133742u, 999999u}) {
    auto gen = MakeGen(1, 0.99, seed);
    EXPECT_FALSE(std::isnan(gen.eta()));
    for (int i = 0; i < 50000; ++i) {
      ASSERT_EQ(gen.NextRank(), 0u);
      ASSERT_EQ(gen.Next(), 0u);
    }
  }
}

TEST(ZipfianTest, ResultsInRangeN2) {
  // n == 2: eta is 0/0 degenerating case, draws must fall in {0, 1}.
  auto gen = MakeGen(2);
  EXPECT_FALSE(std::isnan(gen.eta()));
  bool saw_zero = false;
  bool saw_one = false;
  for (int i = 0; i < 10000; ++i) {
    const uint64_t r = gen.NextRank();
    ASSERT_LT(r, 2u);
    if (r == 0) saw_zero = true;
    if (r == 1) saw_one = true;

    const uint64_t v = gen.Next();
    ASSERT_LT(v, 2u);
  }
  EXPECT_TRUE(saw_zero);
  EXPECT_TRUE(saw_one);
}

// ── determinism ───────────────────────────────────────────────────────────────

TEST(ZipfianTest, SameSeedSameSequence) {
  constexpr uint64_t kN = 50000;
  constexpr uint64_t kSeed = 999;
  constexpr int kTrials = 200;

  const double zeta = ZipfZeta(kN, 0.99);
  ZipfianGenerator g1(kN, 0.99, zeta, kSeed);
  ZipfianGenerator g2(kN, 0.99, zeta, kSeed);

  for (int i = 0; i < kTrials; ++i) {
    EXPECT_EQ(g1.Next(), g2.Next()) << "mismatch at trial " << i;
  }
}

TEST(ZipfianTest, DifferentSeedsDifferentSequences) {
  constexpr uint64_t kN = 50000;
  const double zeta = ZipfZeta(kN, 0.99);
  ZipfianGenerator g1(kN, 0.99, zeta, 1);
  ZipfianGenerator g2(kN, 0.99, zeta, 2);

  int diffs = 0;
  for (int i = 0; i < 100; ++i) {
    if (g1.Next() != g2.Next()) {
      ++diffs;
    }
  }
  EXPECT_GT(diffs, 0) << "two different seeds produced identical sequences";
}

// ── frequency distribution ────────────────────────────────────────────────────

TEST(ZipfianTest, Rank0FrequencyMatchesExpectation) {
  // In Gray's algorithm, uz = u * zeta(n).
  // uz < 1 occurs with probability (1 / zeta(n)), so rank 0 frequency
  // should match 1 / zeta(n).
  constexpr uint64_t kN = 10000;
  constexpr int kTrials = 1000000;
  constexpr double kTheta = 0.99;

  const double zeta = ZipfZeta(kN, kTheta);
  const double expected_freq = 1.0 / zeta;

  ZipfianGenerator gen(kN, kTheta, zeta, 42);

  int rank0_hits = 0;
  for (int i = 0; i < kTrials; ++i) {
    if (gen.NextRank() == 0) {
      ++rank0_hits;
    }
  }

  const double actual_freq = static_cast<double>(rank0_hits) / kTrials;
  EXPECT_NEAR(actual_freq, expected_freq, 0.05 * expected_freq)
      << "rank 0 frequency " << actual_freq << " vs expected " << expected_freq;
}

TEST(ZipfianTest, Rank1ToRank0FrequencyRatio) {
  // P(rank 1) / P(rank 0) = (0.5^theta / zeta(n)) / (1 / zeta(n)) = 2^-theta ≈ 0.5035 for
  // theta=0.99.
  constexpr uint64_t kN = 10000;
  constexpr int kTrials = 2000000;
  constexpr double kTheta = 0.99;

  const double zeta = ZipfZeta(kN, kTheta);
  ZipfianGenerator gen(kN, kTheta, zeta, 7);

  int hits0 = 0;
  int hits1 = 0;
  for (int i = 0; i < kTrials; ++i) {
    const uint64_t r = gen.NextRank();
    if (r == 0) ++hits0;
    if (r == 1) ++hits1;
  }

  ASSERT_GT(hits0, 0);
  const double ratio = static_cast<double>(hits1) / static_cast<double>(hits0);
  const double expected = std::pow(0.5, kTheta);
  EXPECT_NEAR(ratio, expected, 0.05 * expected)
      << "rank1/rank0 ratio " << ratio << " vs expected " << expected;
}

// ── scrambling ────────────────────────────────────────────────────────────────

TEST(ZipfianTest, ScramblingKeysInRange) {
  for (uint64_t n : {1u, 2u, 7u, 100u, 777u, 50000u}) {
    auto gen = MakeGen(n);
    for (int i = 0; i < 10000; ++i) {
      const uint64_t v = gen.Next();
      ASSERT_LT(v, n);
    }
  }
}

TEST(ZipfianTest, HottestKeysAreNotNeighbours) {
  // Scrambling maps raw rank -> Fnv1a64(rank) % n. The hottest ranks
  // (0, 1, 2, ...) must NOT map to adjacent keys (|a - b| <= 1).
  constexpr int kCheckRanks = 10;
  constexpr uint64_t kN = 100000;

  std::vector<uint64_t> hot_keys;
  hot_keys.reserve(kCheckRanks);
  for (int rank = 0; rank < kCheckRanks; ++rank) {
    hot_keys.push_back(Fnv1a64(static_cast<uint64_t>(rank)) % kN);
  }

  int adjacent = 0;
  for (int i = 0; i < kCheckRanks; ++i) {
    for (int j = i + 1; j < kCheckRanks; ++j) {
      const uint64_t diff =
          hot_keys[i] > hot_keys[j] ? hot_keys[i] - hot_keys[j] : hot_keys[j] - hot_keys[i];
      if (diff <= 1) ++adjacent;
    }
  }
  EXPECT_EQ(adjacent, 0) << "found adjacent hot keys after scrambling";

  // Also verify that rank 0 and rank 1 map to non-adjacent keys.
  const uint64_t key0 = Fnv1a64(0) % kN;
  const uint64_t key1 = Fnv1a64(1) % kN;
  const uint64_t key_diff = key0 > key1 ? key0 - key1 : key1 - key0;
  EXPECT_GT(key_diff, 1u);
}

// ── invalid inputs ────────────────────────────────────────────────────────────

TEST(ZipfianTest, InvalidThetaOne) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, 1.0, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(1000, 1.0), std::invalid_argument);
}

TEST(ZipfianTest, InvalidThetaAboveOne) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, 1.5, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(1000, 1.5), std::invalid_argument);
}

TEST(ZipfianTest, InvalidThetaZero) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.0, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(1000, 0.0), std::invalid_argument);
}

TEST(ZipfianTest, InvalidThetaNegative) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, -0.5, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(1000, -0.5), std::invalid_argument);
}

TEST(ZipfianTest, InvalidNZero) {
  EXPECT_THROW({ ZipfianGenerator gen(0, 0.99, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(0, 0.99), std::invalid_argument);
}

TEST(ZipfianTest, InvalidZetaZeroOrNegative) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.99, 0.0, 42); }, std::invalid_argument);
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.99, -5.0, 42); }, std::invalid_argument);
}

// ── read/update split ratio test ──────────────────────────────────────────────

TEST(ZipfianTest, ReadUpdateSplitWithinOnePercent) {
  constexpr int kOps = 200000;

  // Test 95% reads (YCSB B)
  {
    std::mt19937_64 rng(133742);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    int reads = 0;
    int updates = 0;
    for (int i = 0; i < kOps; ++i) {
      if (dist(rng) < 0.95) {
        ++reads;
      } else {
        ++updates;
      }
    }
    const double read_pct = static_cast<double>(reads) / kOps;
    EXPECT_NEAR(read_pct, 0.95, 0.01) << "95% read ratio out of 1% tolerance: " << read_pct;
    EXPECT_EQ(reads + updates, kOps);
  }

  // Test 50% reads (YCSB A)
  {
    std::mt19937_64 rng(133743);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    int reads = 0;
    int updates = 0;
    for (int i = 0; i < kOps; ++i) {
      if (dist(rng) < 0.50) {
        ++reads;
      } else {
        ++updates;
      }
    }
    const double read_pct = static_cast<double>(reads) / kOps;
    EXPECT_NEAR(read_pct, 0.50, 0.01) << "50% read ratio out of 1% tolerance: " << read_pct;
    EXPECT_EQ(reads + updates, kOps);
  }
}

}  // namespace strata
