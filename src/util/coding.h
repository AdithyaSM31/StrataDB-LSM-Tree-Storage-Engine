#ifndef STRATA_UTIL_CODING_H_
#define STRATA_UTIL_CODING_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

// Fixed-width little-endian encoding and decoding.
inline void EncodeFixed32(char* dst, uint32_t v) {
  auto* const buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(v);
  buf[1] = static_cast<uint8_t>(v >> 8);
  buf[2] = static_cast<uint8_t>(v >> 16);
  buf[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t DecodeFixed32(const char* p) {
  const auto* const buf = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
}

inline void EncodeFixed64(char* dst, uint64_t v) {
  auto* const buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(v);
  buf[1] = static_cast<uint8_t>(v >> 8);
  buf[2] = static_cast<uint8_t>(v >> 16);
  buf[3] = static_cast<uint8_t>(v >> 24);
  buf[4] = static_cast<uint8_t>(v >> 32);
  buf[5] = static_cast<uint8_t>(v >> 40);
  buf[6] = static_cast<uint8_t>(v >> 48);
  buf[7] = static_cast<uint8_t>(v >> 56);
}

inline uint64_t DecodeFixed64(const char* p) {
  const auto* const buf = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint64_t>(buf[0]) | (static_cast<uint64_t>(buf[1]) << 8) |
         (static_cast<uint64_t>(buf[2]) << 16) | (static_cast<uint64_t>(buf[3]) << 24) |
         (static_cast<uint64_t>(buf[4]) << 32) | (static_cast<uint64_t>(buf[5]) << 40) |
         (static_cast<uint64_t>(buf[6]) << 48) | (static_cast<uint64_t>(buf[7]) << 56);
}

// Append fixed-width numbers to a string.
void PutFixed32(std::string* dst, uint32_t v);
void PutFixed64(std::string* dst, uint64_t v);

// Append varints to a string.
void PutVarint32(std::string* dst, uint32_t v);
void PutVarint64(std::string* dst, uint64_t v);

// Append a length-prefixed string (32-bit varint length followed by data).
void PutLengthPrefixed(std::string* dst, std::string_view s);

// Low-level varint encoders. Writes the encoded bytes into `dst` and returns
// a pointer past the last written byte.
// `dst` must have space for at least 5 bytes (for 32-bit) or 10 bytes (for 64-bit).
char* EncodeVarint32(char* dst, uint32_t v);
char* EncodeVarint64(char* dst, uint64_t v);

// Returns the number of bytes needed to encode `v` as a varint.
inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

// Low-level pointer decoders.
// Returns a pointer just past the parsed varint on success, or nullptr on error
// (truncated or corrupted input).
const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* v);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v);

inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v) {
  if (p < limit) {
    uint32_t result = static_cast<uint8_t>(*p);
    if ((result & 0x80) == 0) {
      *v = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, v);
}

// Consume from the front of a string_view: on success, advance *input and return true.
bool GetVarint32(std::string_view* input, uint32_t* v);
bool GetVarint64(std::string_view* input, uint64_t* v);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

}  // namespace strata

#endif  // STRATA_UTIL_CODING_H_
