#ifndef STRATA_LOG_LOG_FORMAT_H_
#define STRATA_LOG_LOG_FORMAT_H_

#include <cstddef>
#include <cstdint>

namespace strata::log {

enum RecordType : uint8_t {
  // Zero is reserved for preallocated or zero-filled space
  kZeroType = 0,

  kFullType = 1,

  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

inline constexpr uint8_t kMaxRecordType = kLastType;

inline constexpr size_t kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
inline constexpr size_t kHeaderSize = 4 + 2 + 1;

static_assert(kBlockSize - kHeaderSize <= 0xffff,
              "kBlockSize - kHeaderSize must fit in 16-bit length field");

}  // namespace strata::log

#endif  // STRATA_LOG_LOG_FORMAT_H_
