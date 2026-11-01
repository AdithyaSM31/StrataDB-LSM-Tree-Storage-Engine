#include "util/hash.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace strata {
namespace {

TEST(HashTest, FixedExpectedValuesSeedZero) {
  // Fixed golden values locked with seed 0.
  // These values must remain identical across compilers and architectures forever
  // to prevent silent bloom filter corruption on disk.
  EXPECT_EQ(Hash("", 0, 0), 0x00000000U);
  EXPECT_EQ(Hash("a", 1, 0), 0xca6c9dd6U);
  EXPECT_EQ(Hash("ab", 2, 0), 0x589c01ddU);
  EXPECT_EQ(Hash("abc", 3, 0), 0xac7e1f42U);
  EXPECT_EQ(Hash("abcd", 4, 0), 0x9e87a0d0U);
  EXPECT_EQ(Hash("123456789", 9, 0), 0x1eb0ec09U);
  EXPECT_EQ(Hash("\xe1\x80\xb9\x32", 4, 0), 0xd1e09037U);
}

TEST(HashTest, FixedExpectedValuesCustomSeed) {
  // Fixed golden values with non-zero seed (e.g. LevelDB bloom filter seed 0xbc9f1d34).
  constexpr uint32_t kSeed = 0xbc9f1d34U;
  EXPECT_EQ(Hash("", 0, kSeed), 0xbc9f1d34U);
  EXPECT_EQ(Hash("a", 1, kSeed), 0x286e9db0U);
  EXPECT_EQ(Hash("ab", 2, kSeed), 0x39aca330U);
  EXPECT_EQ(Hash("abc", 3, kSeed), 0x855d012fU);
  EXPECT_EQ(Hash("abcd", 4, kSeed), 0xb9c83353U);
  EXPECT_EQ(Hash("123456789", 9, kSeed), 0x73dddd2aU);
  EXPECT_EQ(Hash("\xe1\x80\xb9\x32", 4, kSeed), 0xed21633aU);
}

TEST(HashTest, StringViewOverloadEquivalence) {
  const std::string test_strings[] = {"", "x", "hello", "strata_db_key_12345",
                                      std::string("\xff\xfe\xfd\x00\x01", 5)};
  for (const auto& s : test_strings) {
    EXPECT_EQ(Hash(s), Hash(s.data(), s.size(), 0));
    EXPECT_EQ(Hash(s, 0x12345678U), Hash(s.data(), s.size(), 0x12345678U));
  }
}

TEST(HashTest, InputLengthsZeroToEight) {
  // Test every input length from 0 to 8 to ensure each leftover path
  // (0, 1, 2, 3 remainder bytes) executes properly.
  const char buffer[8] = {'0', '1', '2', '3', '4', '5', '6', '7'};
  uint32_t hashes[9];
  for (size_t len = 0; len <= 8; ++len) {
    hashes[len] = Hash(buffer, len, 42);
  }

  // Check that all hashes for different lengths are distinct.
  for (size_t i = 0; i <= 8; ++i) {
    for (size_t j = i + 1; j <= 8; ++j) {
      EXPECT_NE(hashes[i], hashes[j]) << "Hash collision between lengths " << i << " and " << j;
    }
  }
}

TEST(HashTest, SeedSensitivity) {
  // Changing the seed must change the output.
  const std::string s = "test_key_sample";
  uint32_t h1 = Hash(s, 0);
  uint32_t h2 = Hash(s, 1);
  uint32_t h3 = Hash(s, 0xdeadbeefU);
  EXPECT_NE(h1, h2);
  EXPECT_NE(h1, h3);
  EXPECT_NE(h2, h3);
}

TEST(HashTest, SignedCharHandling) {
  // High bytes (> 0x7F) must be treated as unsigned uint8_t, not sign-extended.
  // Test lengths 1, 2, and 3 with 0xFF bytes to exercise switch fallthrough cases.
  const char high1[1] = {static_cast<char>(0xFF)};
  const char high2[2] = {static_cast<char>(0xFF), static_cast<char>(0xFE)};
  const char high3[3] = {static_cast<char>(0xFF), static_cast<char>(0xFE), static_cast<char>(0xFD)};

  const unsigned char u_high1[1] = {0xFF};
  const unsigned char u_high2[2] = {0xFF, 0xFE};
  const unsigned char u_high3[3] = {0xFF, 0xFE, 0xFD};

  // 1. Signed buffer must yield exact same hash as unsigned buffer.
  EXPECT_EQ(Hash(high1, 1, 0), Hash(reinterpret_cast<const char*>(u_high1), 1, 0));
  EXPECT_EQ(Hash(high2, 2, 0), Hash(reinterpret_cast<const char*>(u_high2), 2, 0));
  EXPECT_EQ(Hash(high3, 3, 0), Hash(reinterpret_cast<const char*>(u_high3), 3, 0));

  // 2. Exact golden constants locked forever.
  EXPECT_EQ(Hash(high1, 1, 0), 0x640c09b2U);
  EXPECT_EQ(Hash(high2, 2, 0), 0x48590277U);
  EXPECT_EQ(Hash(high3, 3, 0), 0x6aa920c2U);

  // 3. Test lengths 5, 6, 7 to exercise 4-byte loop combined with leftover high bytes.
  const char high5[5] = {static_cast<char>(0x80), static_cast<char>(0x90), static_cast<char>(0xA0),
                         static_cast<char>(0xB0), static_cast<char>(0xC0)};
  const unsigned char u_high5[5] = {0x80, 0x90, 0xA0, 0xB0, 0xC0};
  EXPECT_EQ(Hash(high5, 5, 0), Hash(reinterpret_cast<const char*>(u_high5), 5, 0));
}

TEST(HashTest, NullDataHandling) {
  // Null pointer with length 0 is safe and returns seed.
  EXPECT_EQ(Hash(nullptr, 0, 0), 0U);
  EXPECT_EQ(Hash(nullptr, 0, 42), 42U);

#if STRATA_DEBUG_CHECKS
  // Non-zero length with nullptr must abort.
  EXPECT_DEATH(Hash(nullptr, 1, 0), "check failed: data != nullptr");
#endif
}

TEST(HashTest, ShardDistribution) {
  // Hash 100,000 keys ("key000001", "key000002", ...) and count how many
  // land in each of the 16 shards, using the top 4 bits (hash >> 28).
  // Each count should be within ±10% of 6,250 (i.e. [5625, 6875]).
  constexpr int kNumKeys = 100000;
  constexpr int kNumShards = 16;
  constexpr int kExpectedPerShard = kNumKeys / kNumShards;  // 6250
  constexpr int kTolerance = kExpectedPerShard * 10 / 100;  // 625

  int shard_counts[kNumShards] = {0};
  char key_buf[32];

  for (int i = 0; i < kNumKeys; ++i) {
    int len = std::snprintf(key_buf, sizeof(key_buf), "key%06d", i);
    uint32_t h = Hash(key_buf, static_cast<size_t>(len), 0);
    uint32_t shard = h >> 28;
    ASSERT_LT(shard, static_cast<uint32_t>(kNumShards));
    ++shard_counts[shard];
  }

  for (int s = 0; s < kNumShards; ++s) {
    EXPECT_GE(shard_counts[s], kExpectedPerShard - kTolerance)
        << "Shard " << s << " count too low: " << shard_counts[s];
    EXPECT_LE(shard_counts[s], kExpectedPerShard + kTolerance)
        << "Shard " << s << " count too high: " << shard_counts[s];
  }
}

}  // namespace
}  // namespace strata
