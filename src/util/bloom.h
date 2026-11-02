#ifndef STRATA_UTIL_BLOOM_H_
#define STRATA_UTIL_BLOOM_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace strata {

// Bloom filter: answers "definitely not present" or "maybe present" for a key,
// with no false negatives.
//
// Filter format (written to disk, so it must never change under the same Name()):
//   [ m bits, rounded up to whole bytes, at least 64 bits ][ k: 1 byte ]
// k is stored in the filter itself, so files stay readable if bits_per_key
// changes later.
//
// The k probe positions come from one 32-bit hash by double hashing
// (Kirsch-Mitzenmacher): bit_j = (h + j * delta) mod m, delta = h rotated
// right by 17 bits.
//
// IMPORTANT for callers: an SSTable's filter must be built from and queried
// with USER keys, not internal keys. A Get searches with a different sequence
// number than the stored entry has, so internal keys would never match.
class BloomFilterPolicy {
 public:
  // bits_per_key: memory per key. 10 gives k = 7 and about a 0.8% false-positive
  // rate. Values below 1 are treated as 1.
  explicit BloomFilterPolicy(int bits_per_key);

  // Recorded in the SSTable's metaindex as "filter." + Name(). Change it if the
  // encoding ever changes, so old files are not misread.
  const char* Name() const { return "bloom"; }

  int bits_per_key() const { return bits_per_key_; }
  int num_probes() const { return k_; }

  // Appends a filter for keys[0..n-1] to *dst. Duplicate keys are allowed.
  void CreateFilter(const std::string_view* keys, size_t n, std::string* dst) const;

  // Returns false only if `key` was definitely not among the keys `filter` was
  // built from. `filter` may come from disk: anything it cannot interpret is
  // answered with true ("maybe"), because a wrong "no" loses data while a wrong
  // "maybe" only costs a read.
  bool KeyMayMatch(std::string_view key, std::string_view filter) const;

 private:
  int bits_per_key_;
  int k_;  // number of probes, 1..30
};

}  // namespace strata

#endif  // STRATA_UTIL_BLOOM_H_
