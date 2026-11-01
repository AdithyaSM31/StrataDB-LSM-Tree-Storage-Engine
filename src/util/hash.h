#ifndef STRATA_UTIL_HASH_H_
#define STRATA_UTIL_HASH_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace strata {

// Murmur-style 32-bit hash function.
//
// Bloom filter bit patterns are derived from this hash and saved directly
// into SSTables on disk. Because on-disk filters depend on this exact hash,
// its outputs are frozen and must remain identical across compilers,
// architectures, and standard libraries.
//
// Words are decoded in little-endian order via DecodeFixed32 so that the
// resulting hash is endian-independent.
uint32_t Hash(const char* data, size_t n, uint32_t seed = 0);

inline uint32_t Hash(std::string_view s, uint32_t seed = 0) {
  return Hash(s.data(), s.size(), seed);
}

}  // namespace strata

#endif  // STRATA_UTIL_HASH_H_
