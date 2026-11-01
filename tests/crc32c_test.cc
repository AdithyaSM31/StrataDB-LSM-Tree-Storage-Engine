#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace strata::crc32c {
namespace {

TEST(Crc32cTest, RFC3720TestVectors) {
  // Test vectors from RFC 3720 (iSCSI), Appendix B.4.
  // 32 bytes of 0x00
  const std::string zeros(32, '\0');
  EXPECT_EQ(Value(zeros.data(), zeros.size()), 0x8A9136AAU);
  EXPECT_EQ(Value(zeros), 0x8A9136AAU);

  // 32 bytes of 0xFF
  const std::string ones(32, static_cast<char>(0xFF));
  EXPECT_EQ(Value(ones.data(), ones.size()), 0x62A8AB43U);
  EXPECT_EQ(Value(ones), 0x62A8AB43U);

  // 32 bytes: 0, 1, 2, ..., 31
  std::string inc(32, '\0');
  for (int i = 0; i < 32; ++i) {
    inc[i] = static_cast<char>(i);
  }
  EXPECT_EQ(Value(inc.data(), inc.size()), 0x46DD794EU);
  EXPECT_EQ(Value(inc), 0x46DD794EU);

  // 32 bytes: 31, 30, ..., 0
  std::string dec(32, '\0');
  for (int i = 0; i < 32; ++i) {
    dec[i] = static_cast<char>(31 - i);
  }
  EXPECT_EQ(Value(dec.data(), dec.size()), 0x113FDB5CU);
  EXPECT_EQ(Value(dec), 0x113FDB5CU);

  // Standard CRC test string: "123456789"
  const std::string digits = "123456789";
  EXPECT_EQ(Value(digits.data(), digits.size()), 0xE3069283U);
  EXPECT_EQ(Value(digits), 0xE3069283U);
}

TEST(Crc32cTest, EmptyInput) {
  EXPECT_EQ(Value("", 0), 0U);
  EXPECT_EQ(Value(nullptr, 0), 0U);
  EXPECT_EQ(Value(std::string_view{}), 0U);
  EXPECT_EQ(Extend(0, "", 0), 0U);
  EXPECT_EQ(Extend(0, nullptr, 0), 0U);

  // Extending a non-zero CRC with empty data must return the same CRC unchanged.
  EXPECT_EQ(Extend(0x12345678U, "", 0), 0x12345678U);
  EXPECT_EQ(Extend(0x12345678U, nullptr, 0), 0x12345678U);
  EXPECT_EQ(Extend(0x8A9136AAU, "", 0), 0x8A9136AAU);
}

TEST(Crc32cTest, SplitAnywhere) {
  // Generate 1 KB of pseudo-random data with fixed seed for reproducibility.
  std::mt19937 rng(1337);
  std::string data(1024, '\0');
  for (char& b : data) {
    b = static_cast<char>(rng() & 0xFF);
  }

  const uint32_t full_crc = Value(data.data(), data.size());

  // Test every single split point from 0 to 1024.
  for (size_t split = 0; split <= data.size(); ++split) {
    std::string_view a(data.data(), split);
    std::string_view b(data.data() + split, data.size() - split);

    const uint32_t crc_a = Value(a.data(), a.size());
    const uint32_t stitched = Extend(crc_a, b.data(), b.size());
    EXPECT_EQ(stitched, full_crc) << "Failed at split point: " << split;
  }
}

TEST(Crc32cTest, IncrementalByteByByte) {
  std::mt19937 rng(42);
  std::string data(256, '\0');
  for (char& b : data) {
    b = static_cast<char>(rng() & 0xFF);
  }

  const uint32_t expected = Value(data);
  uint32_t incremental = 0;
  for (char b : data) {
    incremental = Extend(incremental, &b, 1);
  }
  EXPECT_EQ(incremental, expected);
}

TEST(Crc32cTest, VaryingChunkSizes) {
  std::mt19937 rng(999);
  std::string data(500, '\0');
  for (char& b : data) {
    b = static_cast<char>(rng() & 0xFF);
  }

  const uint32_t full_crc = Value(data);

  // Split into chunks of sizes 1, 2, 3, 7, 16, 64, 128
  const std::vector<size_t> chunk_sizes = {1, 2, 3, 7, 16, 64, 128};
  for (size_t chunk_size : chunk_sizes) {
    uint32_t crc = 0;
    size_t offset = 0;
    while (offset < data.size()) {
      size_t count = std::min(chunk_size, data.size() - offset);
      crc = Extend(crc, data.data() + offset, count);
      offset += count;
    }
    EXPECT_EQ(crc, full_crc) << "Failed for chunk size " << chunk_size;
  }
}

TEST(Crc32cTest, SingleBitFlips) {
  // Test that every single bit flip in a 64-byte buffer changes the CRC.
  auto run_test_on_buffer = [](std::string buf) {
    ASSERT_EQ(buf.size(), 64U);
    const uint32_t base_crc = Value(buf.data(), buf.size());

    for (size_t byte_idx = 0; byte_idx < buf.size(); ++byte_idx) {
      for (int bit = 0; bit < 8; ++bit) {
        buf[byte_idx] ^= static_cast<char>(1 << bit);
        const uint32_t flipped_crc = Value(buf.data(), buf.size());
        EXPECT_NE(flipped_crc, base_crc)
            << "CRC collision on bit flip at byte " << byte_idx << ", bit " << bit;
        buf[byte_idx] ^= static_cast<char>(1 << bit);  // revert
      }
    }
  };

  // Case 1: All zeros
  run_test_on_buffer(std::string(64, '\0'));

  // Case 2: All ones
  run_test_on_buffer(std::string(64, static_cast<char>(0xFF)));

  // Case 3: Pseudo-random pattern
  std::mt19937 rng(2026);
  std::string random_buf(64, '\0');
  for (char& b : random_buf) {
    b = static_cast<char>(rng() & 0xFF);
  }
  run_test_on_buffer(random_buf);
}

TEST(Crc32cTest, TwoBitFlips) {
  // Test pairs of bit flips within a 32-byte buffer.
  std::string buf(32, '\0');
  const uint32_t base_crc = Value(buf);

  const size_t total_bits = buf.size() * 8;
  for (size_t i = 0; i < total_bits; ++i) {
    buf[i / 8] ^= static_cast<char>(1 << (i % 8));
    for (size_t j = i + 1; j < total_bits; ++j) {
      buf[j / 8] ^= static_cast<char>(1 << (j % 8));
      EXPECT_NE(Value(buf), base_crc) << "Two-bit flip collision at bits " << i << " and " << j;
      buf[j / 8] ^= static_cast<char>(1 << (j % 8));
    }
    buf[i / 8] ^= static_cast<char>(1 << (i % 8));
  }
}

TEST(Crc32cTest, MaskingRoundTrip) {
  std::vector<uint32_t> test_cases = {
      0U,
      1U,
      0x80000000U,
      0xFFFFFFFFU,
      0x12345678U,
      0x8A9136AAU,
      0x62A8AB43U,
      0x46DD794EU,
      0x113FDB5CU,
      0xE3069283U,
      kMaskDelta,
      kMaskDelta - 1U,
      kMaskDelta + 1U,
  };

  for (uint32_t val : test_cases) {
    uint32_t masked = Mask(val);
    EXPECT_EQ(Unmask(masked), val) << "Round-trip failed for " << std::hex << val;
  }

  // 100,000 sequential and pseudorandom values.
  std::mt19937 rng(54321);
  for (int i = 0; i < 100000; ++i) {
    uint32_t val = rng();
    uint32_t masked = Mask(val);
    EXPECT_EQ(Unmask(masked), val);
  }
}

TEST(Crc32cTest, MaskingScramblesValue) {
  // For typical values, Mask(x) must differ from x.
  std::vector<uint32_t> typical = {0U,          1U,          0x12345678U, 0x8A9136AAU, 0x62A8AB43U,
                                   0x46DD794EU, 0x113FDB5CU, 0xE3069283U, 0xFFFFFFFFU};

  for (uint32_t val : typical) {
    EXPECT_NE(Mask(val), val) << "Mask did not scramble " << std::hex << val;
  }
}

TEST(Crc32cTest, NegativeCharValues) {
  // Ensure that char values with MSB set (negative when char is signed)
  // do not cause out-of-bounds array access due to sign extension.
  const char high_bytes[] = {static_cast<char>(0x80), static_cast<char>(0x99),
                             static_cast<char>(0xAA), static_cast<char>(0xBB),
                             static_cast<char>(0xFF)};
  uint32_t crc = Value(high_bytes, sizeof(high_bytes));
  EXPECT_NE(crc, 0U);
}

TEST(Crc32cTest, SpeedBenchmark) {
  // Benchmark byte-at-a-time table throughput over a 1 MB buffer.
  constexpr size_t kBufferSize = 1024 * 1024;  // 1 MB
  constexpr int kIterations = 64;              // 64 MB total
  std::string buffer(kBufferSize, 'x');

  auto start = std::chrono::steady_clock::now();
  uint32_t crc = 0;
  for (int i = 0; i < kIterations; ++i) {
    crc = Value(buffer.data(), buffer.size());
  }
  auto end = std::chrono::steady_clock::now();

  std::chrono::duration<double> elapsed = end - start;
  double total_mb = static_cast<double>(kBufferSize * kIterations) / (1024.0 * 1024.0);
  double mb_per_sec = total_mb / elapsed.count();

  std::cout << "[          ] CRC32C speed: " << std::fixed << std::setprecision(2) << mb_per_sec
            << " MB/s (" << total_mb << " MB in " << elapsed.count() << " s, checksum=0x"
            << std::hex << crc << std::dec << ")\n";

  // Sanity check: CRC computation was not optimized out.
  EXPECT_NE(crc, 0U);
}

}  // namespace
}  // namespace strata::crc32c
