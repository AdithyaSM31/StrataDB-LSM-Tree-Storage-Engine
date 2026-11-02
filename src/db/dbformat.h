#ifndef STRATA_DB_DBFORMAT_H_
#define STRATA_DB_DBFORMAT_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"

namespace strata {

using SequenceNumber = uint64_t;

// Trailer is 8 bytes: high 56 bits for sequence number, low 8 bits for type.
inline constexpr SequenceNumber kMaxSequenceNumber = (1ULL << 56) - 1;

enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
};

// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a LookupKey for seeking. Since trailers sort in descending
// order, the highest ValueType sorts first.
inline constexpr ValueType kValueTypeForSeek = kTypeValue;

// Helper to pack sequence number and value type into a 64-bit integer trailer.
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  STRATA_DCHECK(seq <= kMaxSequenceNumber);
  STRATA_DCHECK(static_cast<uint8_t>(type) <= static_cast<uint8_t>(kTypeValue));
  return (seq << 8) | static_cast<uint8_t>(type);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

// Append the internal key representation of `key` to `result`.
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Parse an internal key from `internal_key` into `*result`.
// Returns false if `internal_key.size() < 8` or the type byte is invalid.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

// Extract the user key component from an internal key.
// REQUIRES: internal_key.size() >= 8
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  STRATA_DCHECK(internal_key.size() >= 8);
  return (internal_key.size() >= 8) ? internal_key.substr(0, internal_key.size() - 8)
                                    : std::string_view();
}

// Helper to construct a std::string containing an encoded internal key.
inline std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string result;
  AppendInternalKey(&result, ParsedInternalKey{user_key, seq, type});
  return result;
}

// Comparator for internal keys.
// Compares user keys in ascending order. If equal, compares trailers in
// descending order, so that higher sequence numbers come first.
class InternalKeyComparator {
 public:
  InternalKeyComparator() = default;

  // A key shorter than 8 bytes is not a valid internal key; it sorts before
  // every valid internal key, so seeking with one positions at the start.
  int Compare(std::string_view a, std::string_view b) const;
  int Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const;

  int operator()(std::string_view a, std::string_view b) const { return Compare(a, b); }

  const char* Name() const { return "strata.InternalKeyComparator"; }
};

// Helper class for looking up keys in a MemTable and SSTables.
// Holds user_key + sequence number formatted in a single buffer:
//   [ varint32: internal key length ][ user key ][ 8-byte trailer ]
//   ^ memtable_key()                 ^ internal_key()
//                                    ^ user_key()
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);
  ~LookupKey();

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  LookupKey(LookupKey&& other) noexcept;
  LookupKey& operator=(LookupKey&& other) noexcept;

  // Key format for memtable search: [ varint32 internal_key_len ][ internal key ]
  std::string_view memtable_key() const {
    return (start_ != nullptr && end_ >= start_)
               ? std::string_view(start_, static_cast<size_t>(end_ - start_))
               : std::string_view();
  }

  // Key format for SSTable search: [ user key ][ 8-byte trailer ]
  std::string_view internal_key() const {
    return (kstart_ != nullptr && end_ >= kstart_)
               ? std::string_view(kstart_, static_cast<size_t>(end_ - kstart_))
               : std::string_view();
  }

  // Plain user key
  std::string_view user_key() const {
    return (kstart_ != nullptr && end_ >= kstart_ + 8)
               ? std::string_view(kstart_, static_cast<size_t>(end_ - kstart_ - 8))
               : std::string_view();
  }

 private:
  const char* start_ = nullptr;
  const char* kstart_ = nullptr;
  const char* end_ = nullptr;
  char space_[200];
};

}  // namespace strata

#endif  // STRATA_DB_DBFORMAT_H_
