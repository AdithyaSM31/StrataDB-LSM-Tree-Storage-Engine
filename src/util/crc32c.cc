#include "util/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "util/check.h"

namespace strata::crc32c {
namespace {

// CRC32C (Castagnoli) reflected polynomial: 0x82F63B78.
constexpr uint32_t kPolynomial = 0x82F63B78U;

// Precompute the byte-at-a-time lookup table at compile time.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1) != 0) {
        crc = (crc >> 1) ^ kPolynomial;
      } else {
        crc = crc >> 1;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  if (n == 0) {
    return init_crc;
  }
  STRATA_DCHECK(data != nullptr);
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  // Undo the final inversion applied to init_crc before feeding in new bytes.
  uint32_t crc = init_crc ^ 0xFFFFFFFFU;
  const auto* const limit = p + n;
  while (p < limit) {
    crc = kTable[(crc ^ *p++) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace strata::crc32c
