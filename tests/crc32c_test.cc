#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "strata/status.h"
#include "util/coding.h"

namespace strata::crc32c {
namespace {

// Mask and Unmask are constexpr and invert each other.
static_assert(Mask(0) == kMaskDelta);
static_assert(Unmask(kMaskDelta) == 0);
static_assert(Unmask(Mask(0x12345678U)) == 0x12345678U);
static_assert(Unmask(Mask(0x8A9136AAU)) == 0x8A9136AAU);
static_assert(Mask(0x12345678U) != 0x12345678U);

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

TEST(Crc32cTest, StringViewOverloads) {
  std::string_view null_view;
  EXPECT_EQ(Value(null_view), 0U);
  EXPECT_EQ(Extend(0, null_view), 0U);
  EXPECT_EQ(Extend(0x12345678U, null_view), 0x12345678U);

  const std::string text = "the quick brown fox jumps over the lazy dog";
  EXPECT_EQ(Value(std::string_view(text)), Value(text.data(), text.size()));
  EXPECT_EQ(Extend(0x12345678U, std::string_view(text)),
            Extend(0x12345678U, text.data(), text.size()));
}

TEST(Crc32cTest, SingleBitFlipsShortBuffers) {
  // Test bit flips on 1-byte, 2-byte, 3-byte, and 4-byte buffers.
  for (size_t len = 1; len <= 4; ++len) {
    std::string buf(len, '\x55');
    const uint32_t base_crc = Value(buf);
    for (size_t i = 0; i < len; ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        buf[i] ^= static_cast<char>(1 << bit);
        EXPECT_NE(Value(buf), base_crc);
        buf[i] ^= static_cast<char>(1 << bit);
      }
    }
  }
}

TEST(Crc32cTest, NegativeCharValues) {
  // Ensure that char values with MSB set (negative when char is signed)
  // produce identical results to uint8_t and do not cause out-of-bounds array access.
  const char high_bytes[] = {static_cast<char>(0x80), static_cast<char>(0x99),
                             static_cast<char>(0xAA), static_cast<char>(0xBB),
                             static_cast<char>(0xFF)};
  const uint8_t unsigned_bytes[] = {0x80, 0x99, 0xAA, 0xBB, 0xFF};

  const uint32_t crc_signed = Value(high_bytes, sizeof(high_bytes));
  const uint32_t crc_unsigned =
      Value(reinterpret_cast<const char*>(unsigned_bytes), sizeof(unsigned_bytes));

  EXPECT_EQ(crc_signed, crc_unsigned);
  EXPECT_NE(crc_signed, 0U);
}

TEST(Crc32cTest, WalRecordBitFlipReturnsCorruption) {
  // Simulates a WAL record header: [masked_crc: 4][length: 2][type: 1][payload: N]
  // CRC covers the type byte plus the payload.
  const char record_type = 1;  // kFullType
  const std::string payload = "wal record payload with simulated key-value mutation";

  // Build WAL record.
  const uint32_t raw_crc = Extend(Value(&record_type, 1), payload);
  std::string record;
  PutFixed32(&record, Mask(raw_crc));
  record.push_back(static_cast<char>(payload.size() & 0xFF));
  record.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
  record.push_back(record_type);
  record.append(payload);

  auto parse_and_verify = [](std::string_view rec) -> Status {
    if (rec.size() < 7U) {
      return Status::Corruption("WAL record too short");
    }
    const uint32_t stored_masked = DecodeFixed32(rec.data());
    const uint16_t length =
        static_cast<uint8_t>(rec[4]) | (static_cast<uint16_t>(static_cast<uint8_t>(rec[5])) << 8);
    if (rec.size() < 7U + length) {
      return Status::Corruption("WAL record truncated");
    }
    const char type = rec[6];
    std::string_view rec_payload = rec.substr(7, length);
    const uint32_t expected_crc = Unmask(stored_masked);
    const uint32_t actual_crc = Extend(Value(&type, 1), rec_payload);
    if (actual_crc != expected_crc) {
      return Status::Corruption("WAL record checksum mismatch");
    }
    return Status::OK();
  };

  // Valid record succeeds.
  EXPECT_TRUE(parse_and_verify(record).ok());

  // Flipping any single bit in the record must cause Corruption.
  for (size_t byte_idx = 0; byte_idx < record.size(); ++byte_idx) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string corrupted = record;
      corrupted[byte_idx] ^= static_cast<char>(1 << bit);
      Status s = parse_and_verify(corrupted);
      EXPECT_TRUE(s.IsCorruption())
          << "Failed to detect corruption at byte " << byte_idx << ", bit " << bit;
    }
  }
}

TEST(Crc32cTest, SstableBlockTrailerBitFlipReturnsCorruption) {
  // Simulates SSTable block: [block contents: N][type: 1][masked_crc: 4]
  // CRC covers block contents plus type byte.
  const std::string block_contents = "key1:val1,key2:val2,key3:val3 index and data block entries";
  const char compression_type = 0;  // kNoCompression

  const uint32_t raw_crc = Extend(Value(block_contents), &compression_type, 1);
  std::string block = block_contents;
  block.push_back(compression_type);
  PutFixed32(&block, Mask(raw_crc));

  auto parse_and_verify_block = [](std::string_view blk) -> Status {
    if (blk.size() < 5U) {
      return Status::Corruption("Block too short");
    }
    const size_t content_size = blk.size() - 5U;
    std::string_view contents = blk.substr(0, content_size);
    const char type = blk[content_size];
    const uint32_t stored_masked = DecodeFixed32(blk.data() + content_size + 1);
    const uint32_t expected_crc = Unmask(stored_masked);
    const uint32_t actual_crc = Extend(Value(contents), &type, 1);
    if (actual_crc != expected_crc) {
      return Status::Corruption("Block trailer checksum mismatch");
    }
    return Status::OK();
  };

  // Valid block succeeds.
  EXPECT_TRUE(parse_and_verify_block(block).ok());

  // Flipping any single bit in the block (contents, type, or trailer CRC) must cause Corruption.
  for (size_t byte_idx = 0; byte_idx < block.size(); ++byte_idx) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string corrupted = block;
      corrupted[byte_idx] ^= static_cast<char>(1 << bit);
      Status s = parse_and_verify_block(corrupted);
      EXPECT_TRUE(s.IsCorruption())
          << "Failed to detect corruption at byte " << byte_idx << ", bit " << bit;
    }
  }
}

TEST(Crc32cTest, RawCrcSelfAppendIsConstantWhileMaskedVaries) {
  // Mathematical property noted in specification:
  // Computing CRC over data that ends with its own unmasked CRC produces a fixed constant.
  // Masking scrambles the stored value so this structural regularity disappears.
  const std::vector<std::string> test_strings = {
      "hello world", "StrataDB storage engine", std::string(64, 'z'),
      "123456789",   std::string(128, '\0'),    "arbitrary test payload with some length",
  };

  std::optional<uint32_t> raw_constant;
  std::set<uint32_t> masked_results;

  for (const auto& data : test_strings) {
    const uint32_t raw_crc = Value(data);
    char raw_buf[sizeof(uint32_t)];
    EncodeFixed32(raw_buf, raw_crc);
    std::string data_with_raw = data + std::string(raw_buf, sizeof(raw_buf));
    const uint32_t crc_of_raw = Value(data_with_raw);

    if (!raw_constant.has_value()) {
      raw_constant = crc_of_raw;
    } else {
      EXPECT_EQ(crc_of_raw, *raw_constant)
          << "CRC of data ending with unmasked CRC should be constant";
    }

    // Now with masked CRC:
    char masked_buf[sizeof(uint32_t)];
    EncodeFixed32(masked_buf, Mask(raw_crc));
    std::string data_with_masked = data + std::string(masked_buf, sizeof(masked_buf));
    const uint32_t crc_of_masked = Value(data_with_masked);

    EXPECT_NE(crc_of_masked, *raw_constant);
    masked_results.insert(crc_of_masked);
  }

  // Masked values across different payloads should all produce different CRCs.
  EXPECT_EQ(masked_results.size(), test_strings.size());
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
