#include "util/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/check.h"
#include "util/coding.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define STRATA_CRC32C_X86 1
#include <nmmintrin.h>
#endif

namespace strata::crc32c {
namespace {

// CRC32C (Castagnoli) reflected polynomial: 0x82F63B78.
constexpr uint32_t kPolynomial = 0x82F63B78U;
constexpr uint32_t kInvert = 0xFFFFFFFFU;

using SliceTables = std::array<std::array<uint32_t, 256>, 8>;

// Tables for slicing-by-8, which takes 8 bytes per step. tables[0] is the
// ordinary byte-at-a-time table, and tables[k] advances a byte by k more zeros.
constexpr SliceTables MakeTables() {
  SliceTables tables{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1) != 0) {
        crc = (crc >> 1) ^ kPolynomial;
      } else {
        crc = crc >> 1;
      }
    }
    tables[0][i] = crc;
  }
  for (size_t t = 1; t < tables.size(); ++t) {
    for (size_t i = 0; i < 256; ++i) {
      const uint32_t previous = tables[t - 1][i];
      tables[t][i] = (previous >> 8) ^ tables[0][previous & 0xFFU];
    }
  }
  return tables;
}

constexpr SliceTables kTables = MakeTables();

}  // namespace

namespace internal {

uint32_t ExtendSoftware(uint32_t init_crc, const char* data, size_t n) {
  // Undo the final inversion applied to init_crc before feeding in new bytes.
  uint32_t crc = init_crc ^ kInvert;
  while (n >= 8) {
    const uint64_t word = DecodeFixed64(data) ^ crc;
    crc = kTables[7][word & 0xFFU] ^ kTables[6][(word >> 8) & 0xFFU] ^
          kTables[5][(word >> 16) & 0xFFU] ^ kTables[4][(word >> 24) & 0xFFU] ^
          kTables[3][(word >> 32) & 0xFFU] ^ kTables[2][(word >> 40) & 0xFFU] ^
          kTables[1][(word >> 48) & 0xFFU] ^ kTables[0][word >> 56];
    data += 8;
    n -= 8;
  }
  for (size_t i = 0; i < n; ++i) {
    crc = kTables[0][(crc ^ static_cast<uint8_t>(data[i])) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ kInvert;
}

#if defined(STRATA_CRC32C_X86)

bool HardwareAvailable() {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") != 0;
}

__attribute__((target("sse4.2"))) uint32_t ExtendHardware(uint32_t init_crc, const char* data,
                                                          size_t n) {
  uint64_t crc = init_crc ^ kInvert;
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, data, sizeof(word));
    crc = _mm_crc32_u64(crc, word);
    data += 8;
    n -= 8;
  }
  auto narrow = static_cast<uint32_t>(crc);
  for (size_t i = 0; i < n; ++i) {
    narrow = _mm_crc32_u8(narrow, static_cast<uint8_t>(data[i]));
  }
  return narrow ^ kInvert;
}

#else

bool HardwareAvailable() { return false; }

uint32_t ExtendHardware(uint32_t init_crc, const char* data, size_t n) {
  return ExtendSoftware(init_crc, data, n);
}

#endif

}  // namespace internal

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  if (n == 0) {
    return init_crc;
  }
  STRATA_DCHECK(data != nullptr);
  static const bool use_hardware = internal::HardwareAvailable();
  return use_hardware ? internal::ExtendHardware(init_crc, data, n)
                      : internal::ExtendSoftware(init_crc, data, n);
}

}  // namespace strata::crc32c
