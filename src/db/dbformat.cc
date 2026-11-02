#include "db/dbformat.h"

#include <cstring>

namespace strata {

static uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  STRATA_DCHECK(seq <= kMaxSequenceNumber);
  STRATA_DCHECK(static_cast<uint8_t>(type) <= static_cast<uint8_t>(kTypeValue));
  return (seq << 8) | static_cast<uint8_t>(type);
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  STRATA_DCHECK(result != nullptr);
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  STRATA_DCHECK(result != nullptr);
  if (internal_key.size() < 8) {
    return false;
  }
  const size_t n = internal_key.size();
  const uint64_t trailer = DecodeFixed64(internal_key.data() + n - 8);
  const uint8_t type_byte = static_cast<uint8_t>(trailer & 0xff);
  if (type_byte > static_cast<uint8_t>(kTypeValue)) {
    return false;
  }
  result->user_key = internal_key.substr(0, n - 8);
  result->sequence = trailer >> 8;
  result->type = static_cast<ValueType>(type_byte);
  return true;
}

int InternalKeyComparator::Compare(std::string_view a, std::string_view b) const {
  STRATA_DCHECK(a.size() >= 8);
  STRATA_DCHECK(b.size() >= 8);
  std::string_view ukey_a = ExtractUserKey(a);
  std::string_view ukey_b = ExtractUserKey(b);
  int r = ukey_a.compare(ukey_b);
  if (r != 0) {
    return r;
  }
  const uint64_t a_num = DecodeFixed64(a.data() + a.size() - 8);
  const uint64_t b_num = DecodeFixed64(b.data() + b.size() - 8);
  if (a_num > b_num) {
    return -1;
  }
  if (a_num < b_num) {
    return 1;
  }
  return 0;
}

int InternalKeyComparator::Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const {
  int r = a.user_key.compare(b.user_key);
  if (r != 0) {
    return r;
  }
  const uint64_t a_num = PackSequenceAndType(a.sequence, a.type);
  const uint64_t b_num = PackSequenceAndType(b.sequence, b.type);
  if (a_num > b_num) {
    return -1;
  }
  if (a_num < b_num) {
    return 1;
  }
  return 0;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  STRATA_DCHECK(sequence <= kMaxSequenceNumber);
  const size_t usize = user_key.size();
  const size_t internal_key_size = usize + 8;
  const size_t needed = VarintLength(internal_key_size) + internal_key_size;
  char* dst = (needed <= sizeof(space_)) ? space_ : new char[needed];
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(internal_key_size));
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(sequence, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
  STRATA_DCHECK(end_ == start_ + needed);
}

LookupKey::~LookupKey() {
  if (start_ != space_) {
    delete[] start_;
  }
}

LookupKey::LookupKey(LookupKey&& other) noexcept {
  if (other.start_ == other.space_) {
    const size_t size = other.end_ - other.start_;
    std::memcpy(space_, other.space_, size);
    start_ = space_;
    kstart_ = space_ + (other.kstart_ - other.start_);
    end_ = space_ + size;
  } else {
    start_ = other.start_;
    kstart_ = other.kstart_;
    end_ = other.end_;
    other.start_ = other.space_;
    other.kstart_ = other.space_;
    other.end_ = other.space_;
  }
}

LookupKey& LookupKey::operator=(LookupKey&& other) noexcept {
  if (this != &other) {
    if (start_ != space_) {
      delete[] start_;
    }
    if (other.start_ == other.space_) {
      const size_t size = other.end_ - other.start_;
      std::memcpy(space_, other.space_, size);
      start_ = space_;
      kstart_ = space_ + (other.kstart_ - other.start_);
      end_ = space_ + size;
    } else {
      start_ = other.start_;
      kstart_ = other.kstart_;
      end_ = other.end_;
      other.start_ = other.space_;
      other.kstart_ = other.space_;
      other.end_ = other.space_;
    }
  }
  return *this;
}

}  // namespace strata
