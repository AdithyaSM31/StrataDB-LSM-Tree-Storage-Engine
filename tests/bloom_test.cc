#include "util/bloom.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/coding.h"

namespace strata {
namespace {

// A 4-byte little-endian key, so keys are distinct and cheap to generate.
std::string Key(uint32_t i) {
  char buf[4];
  EncodeFixed32(buf, i);
  return std::string(buf, sizeof(buf));
}

std::string BuildFilter(const BloomFilterPolicy& policy, const std::vector<std::string>& keys) {
  std::vector<std::string_view> views(keys.begin(), keys.end());
  std::string filter;
  policy.CreateFilter(views.data(), views.size(), &filter);
  return filter;
}

std::vector<std::string> Keys(uint32_t first, uint32_t n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    keys.push_back(Key(first + i));
  }
  return keys;
}

TEST(BloomTest, ProbeCountIsRoundedLnTwoTimesBitsPerKey) {
  EXPECT_EQ(BloomFilterPolicy(10).num_probes(), 7);  // 6.93 rounds to 7
  EXPECT_EQ(BloomFilterPolicy(1).num_probes(), 1);   // 0.69 rounds to 1
  EXPECT_EQ(BloomFilterPolicy(0).num_probes(), 1);   // treated as 1 bit per key
  EXPECT_EQ(BloomFilterPolicy(-5).num_probes(), 1);
  EXPECT_EQ(BloomFilterPolicy(20).num_probes(), 14);   // 13.86
  EXPECT_EQ(BloomFilterPolicy(100).num_probes(), 30);  // clamped
}

TEST(BloomTest, EmptyFilterMatchesNothing) {
  const BloomFilterPolicy policy(10);
  const std::string filter = BuildFilter(policy, {});
  EXPECT_EQ(filter.size(), 8U + 1U);  // the 64-bit minimum, plus k
  EXPECT_FALSE(policy.KeyMayMatch("hello", filter));
  EXPECT_FALSE(policy.KeyMayMatch("", filter));
}

TEST(BloomTest, SmallFilter) {
  const BloomFilterPolicy policy(10);
  const std::string filter = BuildFilter(policy, {"hello", "world"});
  EXPECT_TRUE(policy.KeyMayMatch("hello", filter));
  EXPECT_TRUE(policy.KeyMayMatch("world", filter));
  EXPECT_FALSE(policy.KeyMayMatch("x", filter));
  EXPECT_FALSE(policy.KeyMayMatch("foo", filter));
}

TEST(BloomTest, FrozenBytes) {
  // Filters are written to disk, so their bytes must never change. These were
  // computed by an independent Python implementation of the same algorithm.
  const BloomFilterPolicy policy(10);
  EXPECT_EQ(BuildFilter(policy, {"hello", "world"}),
            std::string("\x11\x45\x00\x41\x44\x10\x40\x10\x07", 9));
  EXPECT_EQ(BuildFilter(policy, Keys(0, 10)),
            std::string("\xad\xab\xa8\x5c\xc2\x3f\xda\x0a\x72\x39\x95\xdd\x59\x07", 14));
}

TEST(BloomTest, NoFalseNegatives) {
  const BloomFilterPolicy policy(10);
  for (const uint32_t n : {1U, 10U, 100U, 1000U, 10000U}) {
    SCOPED_TRACE("n=" + std::to_string(n));
    const std::vector<std::string> keys = Keys(0, n);
    const std::string filter = BuildFilter(policy, keys);
    for (const std::string& key : keys) {
      ASSERT_TRUE(policy.KeyMayMatch(key, filter));
    }
  }
}

TEST(BloomTest, NoFalseNegativesWithVaryingKeyLengths) {
  const BloomFilterPolicy policy(10);
  std::vector<std::string> keys;
  for (size_t len = 0; len <= 300; ++len) {
    keys.push_back(std::string(len, static_cast<char>('a' + len % 26)));
  }
  const std::string filter = BuildFilter(policy, keys);
  for (const std::string& key : keys) {
    EXPECT_TRUE(policy.KeyMayMatch(key, filter)) << "length " << key.size();
  }
}

TEST(BloomTest, FalsePositiveRateWithinOneAndAHalfTimesTheory) {
  const BloomFilterPolicy policy(10);
  constexpr uint32_t kProbes = 10000;
  for (const uint32_t n : {1000U, 2000U, 5000U, 10000U}) {
    const std::string filter = BuildFilter(policy, Keys(0, n));
    // Theory, from this filter's actual m and k: (1 - e^(-kn/m))^k.
    const double m = static_cast<double>((filter.size() - 1) * 8);
    const double k = static_cast<uint8_t>(filter.back());
    const double theory = std::pow(1.0 - std::exp(-k * n / m), k);

    int false_positives = 0;
    for (uint32_t i = 0; i < kProbes; ++i) {
      // Keys far from the ones added, so none of them was added.
      if (policy.KeyMayMatch(Key(1000000000U + i), filter)) {
        ++false_positives;
      }
    }
    const double measured = static_cast<double>(false_positives) / kProbes;
    EXPECT_LE(measured, 1.5 * theory)
        << "n=" << n << " measured " << measured << " theory " << theory;
    EXPECT_LT(measured, 0.02) << "n=" << n;
  }
}

TEST(BloomTest, FilterSize) {
  const BloomFilterPolicy policy(10);
  // 1000 keys x 10 bits = 10000 bits = 1250 bytes, plus the k byte.
  EXPECT_EQ(BuildFilter(policy, Keys(0, 1000)).size(), 1251U);
}

TEST(BloomTest, UsesTheProbeCountStoredInTheFilter) {
  // Written with 10 bits per key (k = 7), read by a policy with k = 14: the
  // reader must use the filter's k, or it would report false negatives.
  const std::vector<std::string> keys = Keys(0, 500);
  const std::string filter = BuildFilter(BloomFilterPolicy(10), keys);
  const BloomFilterPolicy reader(20);
  for (const std::string& key : keys) {
    ASSERT_TRUE(reader.KeyMayMatch(key, filter));
  }
}

TEST(BloomTest, AppendsToExistingContents) {
  const BloomFilterPolicy policy(10);
  std::string dst = "prefix";
  std::vector<std::string_view> keys = {"hello", "world"};
  policy.CreateFilter(keys.data(), keys.size(), &dst);
  ASSERT_EQ(dst.substr(0, 6), "prefix");
  EXPECT_EQ(dst.substr(6), BuildFilter(policy, {"hello", "world"}));
}

TEST(BloomTest, DuplicateKeysAreFine) {
  const BloomFilterPolicy policy(10);
  const std::string filter = BuildFilter(policy, {"dup", "dup", "dup", "other"});
  EXPECT_TRUE(policy.KeyMayMatch("dup", filter));
  EXPECT_TRUE(policy.KeyMayMatch("other", filter));
}

TEST(BloomTest, GarbageFiltersAreSafe) {
  const BloomFilterPolicy policy(10);
  // Too short to hold any bits: nothing can be in it.
  EXPECT_FALSE(policy.KeyMayMatch("key", ""));
  EXPECT_FALSE(policy.KeyMayMatch("key", std::string("\x07", 1)));
  // An unknown encoding (k > 30) must answer "maybe", never a wrong "no".
  EXPECT_TRUE(policy.KeyMayMatch("key", std::string("\x00\x00\x00\x00\x1f", 5)));
  EXPECT_TRUE(policy.KeyMayMatch("key", std::string("\x00\xff", 2)));
}

}  // namespace
}  // namespace strata
