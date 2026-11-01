#include "util/coding.h"

#include "util/check.h"

namespace strata {

void PutFixed32(std::string* dst, uint32_t v) {
  char buf[sizeof(uint32_t)];
  EncodeFixed32(buf, v);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t v) {
  char buf[sizeof(uint64_t)];
  EncodeFixed64(buf, v);
  dst->append(buf, sizeof(buf));
}

// Same bytes as the 64-bit encoder; a 32-bit value never needs more than 5.
char* EncodeVarint32(char* dst, uint32_t v) { return EncodeVarint64(dst, v); }

char* EncodeVarint64(char* dst, uint64_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  static constexpr uint8_t kMask = 0x80;
  while (v >= 0x80) {
    *ptr++ = static_cast<uint8_t>((v & 0x7F) | kMask);
    v >>= 7;
  }
  *ptr++ = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* end = EncodeVarint32(buf, v);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* end = EncodeVarint64(buf, v);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutLengthPrefixed(std::string* dst, std::string_view s) {
  // The length is stored as a varint32; refuse to silently truncate it.
  STRATA_CHECK(s.size() <= UINT32_MAX);
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* v) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28; shift += 7) {
    if (p >= limit) {
      return nullptr;
    }
    const uint8_t byte = static_cast<uint8_t>(*p++);
    // The 5th byte holds bits 28-31 only. Anything in its top nibble means the
    // value overflows 32 bits or the encoding runs on (0x80 is in 0xF0).
    if (shift == 28 && (byte & 0xF0) != 0) {
      return nullptr;
    }
    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63; shift += 7) {
    if (p >= limit) {
      return nullptr;
    }
    const uint8_t byte = static_cast<uint8_t>(*p++);
    // The 10th byte holds bit 63 only, so it may be 0 or 1 and nothing else.
    if (shift == 63 && (byte & 0xFE) != 0) {
      return nullptr;
    }
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(std::string_view* input, uint32_t* v) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, v);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetVarint64(std::string_view* input, uint64_t* v) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, v);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) {
    return false;
  }
  if (input->size() < len) {
    return false;
  }
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace strata
