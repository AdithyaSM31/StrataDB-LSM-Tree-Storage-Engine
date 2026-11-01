#ifndef STRATA_UTIL_CRC32C_H_
#define STRATA_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace strata::crc32c {

// Return the CRC32C of `data[0..n-1]` appended to the byte sequence that
// generated `init_crc`.
//
// In particular: Extend(Value(A), B) == Value(A + B).
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Return the CRC32C of `s` appended to the byte sequence that generated `init_crc`.
inline uint32_t Extend(uint32_t init_crc, std::string_view s) {
  return Extend(init_crc, s.data(), s.size());
}

// Return the CRC32C of `data[0..n-1]`.
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Return the CRC32C of `s`.
inline uint32_t Value(std::string_view s) { return Extend(0, s.data(), s.size()); }

inline constexpr uint32_t kMaskDelta = 0xa282ead8U;

// Return a masked representation of crc.
//
// Motivation: it is problematic if the information being checksummed
// contains a CRC itself. For example, a WAL record embedded inside a block
// has its own CRC. Masking scrambles the bits so the outer CRC cannot easily
// cancel or collide with inner CRCs.
inline constexpr uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }

// Return the CRC whose masked representation is `masked`.
inline constexpr uint32_t Unmask(uint32_t masked) {
  const uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace strata::crc32c

#endif  // STRATA_UTIL_CRC32C_H_
