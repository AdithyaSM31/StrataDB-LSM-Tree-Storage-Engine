#include "util/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strata {
namespace {

TEST(CodingTest, Fixed32ByteOrder) {
  char buf[4];
  EncodeFixed32(buf, 0x04030201U);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x03);
  EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0x04);
}

TEST(CodingTest, Fixed64ByteOrder) {
  char buf[8];
  EncodeFixed64(buf, 0x0807060504030201ULL);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x03);
  EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0x04);
  EXPECT_EQ(static_cast<uint8_t>(buf[4]), 0x05);
  EXPECT_EQ(static_cast<uint8_t>(buf[5]), 0x06);
  EXPECT_EQ(static_cast<uint8_t>(buf[6]), 0x07);
  EXPECT_EQ(static_cast<uint8_t>(buf[7]), 0x08);
}

TEST(CodingTest, FixedHighBitValues) {
  // Verifies that decoding values with high bits set does not sign-extend.
  std::vector<uint32_t> test_values32 = {0xFFFFFFFFU, 0x80808080U, 0xF0E0D0C0U, 0x80000000U,
                                         0x7FFFFFFFU};
  for (uint32_t val : test_values32) {
    char buf[4];
    EncodeFixed32(buf, val);
    EXPECT_EQ(DecodeFixed32(buf), val);
  }

  std::vector<uint64_t> test_values64 = {0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
                                         0xF0E0D0C0B0A09080ULL};
  for (uint64_t val : test_values64) {
    char buf[8];
    EncodeFixed64(buf, val);
    EXPECT_EQ(DecodeFixed64(buf), val);
  }
}

TEST(CodingTest, FixedAppendAndConsume) {
  std::string s;
  PutFixed32(&s, 0x12345678U);
  PutFixed64(&s, 0x0102030405060708ULL);

  EXPECT_EQ(s.size(), 12U);
  EXPECT_EQ(DecodeFixed32(s.data()), 0x12345678U);
  EXPECT_EQ(DecodeFixed64(s.data() + 4), 0x0102030405060708ULL);
}

TEST(CodingTest, Varint32RoundTripAndLength) {
  std::vector<uint32_t> values = {0, 127, 128, 16383, 16384, 0xFFFFFFFFU};
  for (int i = 0; i < 32; ++i) {
    uint32_t p = 1U << i;
    values.push_back(p);
    if (p > 0) {
      values.push_back(p - 1);
    }
    if (p < 0xFFFFFFFFU) {
      values.push_back(p + 1);
    }
  }

  for (uint32_t val : values) {
    char buf[5];
    char* end = EncodeVarint32(buf, val);
    int encoded_len = static_cast<int>(end - buf);
    EXPECT_EQ(VarintLength(val), encoded_len) << "Value: " << val;

    uint32_t decoded = 0;
    const char* p = GetVarint32Ptr(buf, end, &decoded);
    EXPECT_EQ(p, end) << "Value: " << val;
    EXPECT_EQ(decoded, val) << "Value: " << val;

    // Test through std::string_view
    std::string_view sv(buf, encoded_len);
    uint32_t sv_decoded = 0;
    EXPECT_TRUE(GetVarint32(&sv, &sv_decoded));
    EXPECT_EQ(sv_decoded, val);
    EXPECT_TRUE(sv.empty());
  }
}

TEST(CodingTest, Varint64RoundTripAndLength) {
  std::vector<uint64_t> values = {0, 127, 128, 16383, 16384, 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
  for (int i = 0; i < 64; ++i) {
    uint64_t p = 1ULL << i;
    values.push_back(p);
    if (p > 0) {
      values.push_back(p - 1);
    }
    if (p < 0xFFFFFFFFFFFFFFFFULL) {
      values.push_back(p + 1);
    }
  }

  for (uint64_t val : values) {
    char buf[10];
    char* end = EncodeVarint64(buf, val);
    int encoded_len = static_cast<int>(end - buf);
    EXPECT_EQ(VarintLength(val), encoded_len) << "Value: " << val;

    uint64_t decoded = 0;
    const char* p = GetVarint64Ptr(buf, end, &decoded);
    EXPECT_EQ(p, end) << "Value: " << val;
    EXPECT_EQ(decoded, val) << "Value: " << val;

    // Test through std::string_view
    std::string_view sv(buf, encoded_len);
    uint64_t sv_decoded = 0;
    EXPECT_TRUE(GetVarint64(&sv, &sv_decoded));
    EXPECT_EQ(sv_decoded, val);
    EXPECT_TRUE(sv.empty());
  }
}

TEST(CodingTest, ManyValuesInARow) {
  std::string s;
  std::vector<uint32_t> expected32;
  std::vector<uint64_t> expected64;

  for (uint32_t i = 0; i < 1000; ++i) {
    uint32_t v32 = (i * 1000003U) ^ (i << 13);
    uint64_t v64 = (static_cast<uint64_t>(v32) << 32) | (static_cast<uint64_t>(i) * 37U);
    expected32.push_back(v32);
    expected64.push_back(v64);
    PutVarint32(&s, v32);
    PutVarint64(&s, v64);
  }

  std::string_view input(s);
  for (size_t i = 0; i < 1000; ++i) {
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    ASSERT_TRUE(GetVarint32(&input, &v32)) << "At index " << i;
    EXPECT_EQ(v32, expected32[i]);
    ASSERT_TRUE(GetVarint64(&input, &v64)) << "At index " << i;
    EXPECT_EQ(v64, expected64[i]);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, TruncationVarint32) {
  std::vector<uint32_t> multi_byte_values = {128, 16384, 0xFFFFFFFFU};
  for (uint32_t val : multi_byte_values) {
    char buf[5];
    char* end = EncodeVarint32(buf, val);
    size_t full_len = static_cast<size_t>(end - buf);
    ASSERT_GT(full_len, 1U);

    // Every strictly shorter prefix must fail to decode
    for (size_t len = 0; len < full_len; ++len) {
      uint32_t decoded = 0;
      EXPECT_EQ(GetVarint32Ptr(buf, buf + len, &decoded), nullptr)
          << "Should fail at len " << len << " for value " << val;

      std::string_view sv(buf, len);
      EXPECT_FALSE(GetVarint32(&sv, &decoded))
          << "Should fail at len " << len << " for value " << val;
    }
  }
}

TEST(CodingTest, TruncationVarint64) {
  std::vector<uint64_t> multi_byte_values = {128, 16384, 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
  for (uint64_t val : multi_byte_values) {
    char buf[10];
    char* end = EncodeVarint64(buf, val);
    size_t full_len = static_cast<size_t>(end - buf);
    ASSERT_GT(full_len, 1U);

    for (size_t len = 0; len < full_len; ++len) {
      uint64_t decoded = 0;
      EXPECT_EQ(GetVarint64Ptr(buf, buf + len, &decoded), nullptr)
          << "Should fail at len " << len << " for value " << val;

      std::string_view sv(buf, len);
      EXPECT_FALSE(GetVarint64(&sv, &decoded))
          << "Should fail at len " << len << " for value " << val;
    }
  }
}

TEST(CodingTest, TooLongVarint) {
  // 6 bytes of 0x80 must fail to decode as a 32-bit varint
  char bad32[6];
  for (int i = 0; i < 6; ++i) {
    bad32[i] = static_cast<char>(0x80);
  }
  uint32_t v32 = 0;
  EXPECT_EQ(GetVarint32Ptr(bad32, bad32 + 6, &v32), nullptr);
  std::string_view sv32(bad32, 6);
  EXPECT_FALSE(GetVarint32(&sv32, &v32));

  // 11 bytes of 0x80 must fail to decode as a 64-bit varint
  char bad64[11];
  for (int i = 0; i < 11; ++i) {
    bad64[i] = static_cast<char>(0x80);
  }
  uint64_t v64 = 0;
  EXPECT_EQ(GetVarint64Ptr(bad64, bad64 + 11, &v64), nullptr);
  std::string_view sv64(bad64, 11);
  EXPECT_FALSE(GetVarint64(&sv64, &v64));
}

TEST(CodingTest, Varint32Overflow) {
  // A 5-byte varint with bits set in bits 4..6 of the 5th byte (0xF0 mask)
  // would overflow a 32-bit integer (shift is 28, so bits 4..6 represent bits 32..34).
  uint32_t val = 0;

  // 4 bytes with MSB set followed by 0x10 (bit 4 set)
  const char bad1[5] = {static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
                        static_cast<char>(0x80), 0x10};
  EXPECT_EQ(GetVarint32Ptr(bad1, bad1 + 5, &val), nullptr);
  std::string_view sv1(bad1, 5);
  EXPECT_FALSE(GetVarint32(&sv1, &val));

  // 4 bytes 0xFF followed by 0x10
  const char bad2[5] = {static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF),
                        static_cast<char>(0xFF), 0x10};
  EXPECT_EQ(GetVarint32Ptr(bad2, bad2 + 5, &val), nullptr);
  std::string_view sv2(bad2, 5);
  EXPECT_FALSE(GetVarint32(&sv2, &val));
  EXPECT_EQ(sv2.size(), 5U) << "a failed decode must not consume input";

  // 4 bytes 0xFF followed by 0x7F (maximum 7-bit value in last byte)
  const char bad3[5] = {static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF),
                        static_cast<char>(0xFF), 0x7F};
  EXPECT_EQ(GetVarint32Ptr(bad3, bad3 + 5, &val), nullptr);
  std::string_view sv3(bad3, 5);
  EXPECT_FALSE(GetVarint32(&sv3, &val));

  // Valid maximum 32-bit varint: 4 bytes 0xFF followed by 0x0F
  const char good[5] = {static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF),
                        static_cast<char>(0xFF), 0x0F};
  EXPECT_EQ(GetVarint32Ptr(good, good + 5, &val), good + 5);
  EXPECT_EQ(val, 0xFFFFFFFFU);
  std::string_view sv_good(good, 5);
  EXPECT_TRUE(GetVarint32(&sv_good, &val));
  EXPECT_EQ(val, 0xFFFFFFFFU);
}

TEST(CodingTest, Varint64Overflow) {
  // A 10-byte varint with bits set in bits 1..6 of the 10th byte (0xFE mask)
  // would overflow a 64-bit integer (shift is 63, so bits 1..6 represent bits 64..69).
  uint64_t val = 0;

  // 9 bytes with MSB set followed by 0x02 (bit 1 set)
  char bad1[10];
  for (int i = 0; i < 9; ++i) {
    bad1[i] = static_cast<char>(0x80);
  }
  bad1[9] = 0x02;
  EXPECT_EQ(GetVarint64Ptr(bad1, bad1 + 10, &val), nullptr);
  std::string_view sv1(bad1, 10);
  EXPECT_FALSE(GetVarint64(&sv1, &val));

  // 9 bytes 0xFF followed by 0x02
  char bad2[10];
  for (int i = 0; i < 9; ++i) {
    bad2[i] = static_cast<char>(0xFF);
  }
  bad2[9] = 0x02;
  EXPECT_EQ(GetVarint64Ptr(bad2, bad2 + 10, &val), nullptr);
  std::string_view sv2(bad2, 10);
  EXPECT_FALSE(GetVarint64(&sv2, &val));
  EXPECT_EQ(sv2.size(), 10U) << "a failed decode must not consume input";

  // 9 bytes 0xFF followed by 0x7F
  char bad3[10];
  for (int i = 0; i < 9; ++i) {
    bad3[i] = static_cast<char>(0xFF);
  }
  bad3[9] = 0x7F;
  EXPECT_EQ(GetVarint64Ptr(bad3, bad3 + 10, &val), nullptr);
  std::string_view sv3(bad3, 10);
  EXPECT_FALSE(GetVarint64(&sv3, &val));

  // Valid maximum 64-bit varint: 9 bytes 0xFF followed by 0x01
  char good[10];
  for (int i = 0; i < 9; ++i) {
    good[i] = static_cast<char>(0xFF);
  }
  good[9] = 0x01;
  EXPECT_EQ(GetVarint64Ptr(good, good + 10, &val), good + 10);
  EXPECT_EQ(val, 0xFFFFFFFFFFFFFFFFULL);
  std::string_view sv_good(good, 10);
  EXPECT_TRUE(GetVarint64(&sv_good, &val));
  EXPECT_EQ(val, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(CodingTest, LengthPrefixed) {
  std::string s;
  PutLengthPrefixed(&s, "");
  PutLengthPrefixed(&s, "hello");
  PutLengthPrefixed(&s, std::string(300, 'x'));

  std::string_view input(s);
  std::string_view out;

  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, "");

  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, "hello");

  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, std::string(300, 'x'));

  EXPECT_TRUE(input.empty());

  // Test truncated length-prefixed string
  std::string trunc;
  PutLengthPrefixed(&trunc, "world");
  std::string_view trunc_view(trunc.data(), trunc.size() - 1);  // missing last byte
  std::string_view trunc_out;
  EXPECT_FALSE(GetLengthPrefixed(&trunc_view, &trunc_out));
}

}  // namespace
}  // namespace strata
