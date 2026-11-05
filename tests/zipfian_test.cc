#include "util/zipfian.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
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
    if (r == 0) {
      saw_zero = true;
    }
    if (r == 1) {
      saw_one = true;
    }

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
    if (r == 0) {
      ++hits0;
    }
    if (r == 1) {
      ++hits1;
    }
  }

  ASSERT_GT(hits0, 0);
  const double ratio = static_cast<double>(hits1) / static_cast<double>(hits0);
  const double expected = std::pow(0.5, kTheta);
  EXPECT_NEAR(ratio, expected, 0.05 * expected)
      << "rank1/rank0 ratio " << ratio << " vs expected " << expected;
}

TEST(ZipfianTest, RanksAboveOneFollowGraysFormula) {
  // Gray's form is not exact Zipf at these ranks, so exact Zipf is the wrong yardstick.
  // Inverting it gives P(rank < r) for r >= 2. eta is worked out here rather than read
  // from the generator, so that a wrong eta shows up.
  constexpr uint64_t kN = 1000000;
  constexpr int kTrials = 5000000;
  constexpr double kTheta = 0.99;
  constexpr uint64_t kLastRank = 9;

  const double zeta = ZipfZeta(kN, kTheta);
  const double zeta2 = 1.0 + std::pow(0.5, kTheta);
  const double numerator = 1.0 - std::pow(2.0 / static_cast<double>(kN), 1.0 - kTheta);
  const double eta = numerator / (1.0 - zeta2 / zeta);
  const auto below = [&](uint64_t rank) {
    const double share = static_cast<double>(rank) / static_cast<double>(kN);
    return (std::pow(share, 1.0 - kTheta) - 1.0 + eta) / eta;
  };

  ZipfianGenerator gen(kN, kTheta, zeta, 11);
  std::vector<int> hits(kLastRank + 1, 0);
  for (int i = 0; i < kTrials; ++i) {
    const uint64_t r = gen.NextRank();
    if (r <= kLastRank) {
      ++hits[r];
    }
  }

  for (uint64_t rank = 2; rank <= kLastRank; ++rank) {
    const double expected = below(rank + 1) - below(rank);
    const double actual = static_cast<double>(hits[rank]) / kTrials;
    EXPECT_NEAR(actual, expected, 0.03 * expected) << "rank " << rank;
  }
}

TEST(ZipfianTest, TailMatchesExactZipf) {
  // Ranks from 10 up are close to exact Zipf, which makes this independent of the
  // generator's own formula. Each decade should get its share of the draws.
  constexpr uint64_t kN = 1000000;
  constexpr int kTrials = 3000000;

  for (double theta : {0.5, 0.8, 0.99}) {
    const double zeta = ZipfZeta(kN, theta);
    std::vector<std::pair<uint64_t, uint64_t>> decades;
    for (uint64_t low = 10; low < kN; low *= 10) {
      decades.emplace_back(low, low * 10);
    }

    ZipfianGenerator gen(kN, theta, zeta, 13);
    std::vector<int> hits(decades.size(), 0);
    for (int i = 0; i < kTrials; ++i) {
      const uint64_t r = gen.NextRank();
      for (size_t d = 0; d < decades.size(); ++d) {
        if (r >= decades[d].first && r < decades[d].second) {
          ++hits[d];
          break;
        }
      }
    }

    for (size_t d = 0; d < decades.size(); ++d) {
      double expected = 0.0;
      for (uint64_t rank = decades[d].first; rank < decades[d].second; ++rank) {
        expected += std::pow(static_cast<double>(rank + 1), -theta);
      }
      expected /= zeta;
      const double actual = static_cast<double>(hits[d]) / kTrials;
      EXPECT_NEAR(actual, expected, 0.03 * expected)
          << "theta " << theta << ", ranks from " << decades[d].first;
    }
  }
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

TEST(ZipfianTest, NextIsTheHashOfTheRank) {
  constexpr uint64_t kN = 50000;
  const double zeta = ZipfZeta(kN, 0.99);
  ZipfianGenerator ranks(kN, 0.99, zeta, 21);
  ZipfianGenerator keys(kN, 0.99, zeta, 21);

  for (int i = 0; i < 10000; ++i) {
    ASSERT_EQ(keys.Next(), Fnv1a64(ranks.NextRank()) % kN) << "at trial " << i;
  }
}

TEST(ZipfianTest, HottestKeysAreNotNeighbours) {
  // Finds the hot keys from what Next() actually returns: the ten most drawn
  // must not sit next to each other in the key space.
  constexpr size_t kCheckKeys = 10;
  constexpr uint64_t kN = 100000;
  constexpr int kTrials = 500000;

  auto gen = MakeGen(kN);
  std::unordered_map<uint64_t, int> counts;
  for (int i = 0; i < kTrials; ++i) {
    ++counts[gen.Next()];
  }

  std::vector<std::pair<int, uint64_t>> by_count;
  by_count.reserve(counts.size());
  for (const auto& [key, count] : counts) {
    by_count.emplace_back(count, key);
  }
  ASSERT_GE(by_count.size(), kCheckKeys);
  std::partial_sort(by_count.begin(), by_count.begin() + kCheckKeys, by_count.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });

  for (size_t i = 0; i < kCheckKeys; ++i) {
    for (size_t j = i + 1; j < kCheckKeys; ++j) {
      const uint64_t a = by_count[i].second;
      const uint64_t b = by_count[j].second;
      EXPECT_GT(a > b ? a - b : b - a, 1u) << "hot keys " << a << " and " << b << " are adjacent";
    }
  }
}

TEST(ZipfianTest, Fnv1a64KnownValues) {
  // Worked out separately, by hashing each value's 8 little-endian bytes.
  EXPECT_EQ(Fnv1a64(0), UINT64_C(12161962213042174405));
  EXPECT_EQ(Fnv1a64(1), UINT64_C(9929646806074584996));
  EXPECT_EQ(Fnv1a64(UINT64_C(0x0102030405060708)), UINT64_C(895447315735140821));
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

TEST(ZipfianTest, InvalidThetaNaN) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW({ ZipfianGenerator gen(1000, nan, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(1000, nan), std::invalid_argument);
  EXPECT_FALSE(IsValidZipfTheta(nan));
}

TEST(ZipfianTest, InvalidNZero) {
  EXPECT_THROW({ ZipfianGenerator gen(0, 0.99, 1.0, 42); }, std::invalid_argument);
  EXPECT_THROW(ZipfZeta(0, 0.99), std::invalid_argument);
}

TEST(ZipfianTest, InvalidZetaZeroOrNegative) {
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.99, 0.0, 42); }, std::invalid_argument);
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.99, -5.0, 42); }, std::invalid_argument);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW({ ZipfianGenerator gen(1000, 0.99, nan, 42); }, std::invalid_argument);
}

}  // namespace strata
