#include "util/hash.h"

#include <cstddef>
#include <cstdint>

#include "util/check.h"
#include "util/coding.h"

namespace strata {

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  constexpr uint32_t m = 0xc6a4a793U;
  constexpr uint32_t r = 24;
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  if (n == 0) {
    return h;
  }
  STRATA_DCHECK(data != nullptr);

  const char* limit = data + n;
  while (data + 4 <= limit) {
    uint32_t w = DecodeFixed32(data);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;
  }
  return h;
}

}  // namespace strata
