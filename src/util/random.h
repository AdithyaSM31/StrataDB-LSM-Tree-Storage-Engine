#ifndef STRATA_UTIL_RANDOM_H_
#define STRATA_UTIL_RANDOM_H_

#include <cstdint>

#include "util/check.h"

namespace strata {

// SplitMix64 pseudo-random number generator.
//
// Why not std::rand or <random>?
// 1. std::rand maintains a single unsynchronized global state and its quality
//    is unspecified.
// 2. std::mt19937 is deterministic, but std::uniform_int_distribution's
//    algorithm is NOT specified by the C++ standard (libstdc++ and libc++ yield
//    different streams for the exact same seed). Deterministic simulation tests
//    (--seed=N) and skiplist tests must replay identically on any OS, compiler, or
//    architecture, so a failing seed from CI reproduces locally. (Bloom filters
//    depend on Hash, not on this generator.) Furthermore, mt19937 carries 2.5 KB of
//    state, which is unnecessarily heavy for skiplist node allocation.
//
// SplitMix64 has a 64-bit state, passes BigCrush, and all 64-bit seeds are valid
// (including 0).
//
// Thread safety:
// Not thread-safe. Each caller (e.g. the write leader inserting into the MemTable
// skiplist) must own its own instance.
class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed) {}

  // Advances the generator state and returns a 64-bit pseudo-random value.
  uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Returns a uniform random number in [0, n). Requires n > 0.
  //
  // Note on bias: Next() % n has a slight theoretical bias when n does not divide
  // 2^64 evenly. However, with a 64-bit source and uint32_t n, this bias is at most
  // n / 2^64 (< 1 in 2^32), which is completely negligible in practice.
  uint32_t Uniform(uint32_t n) {
    STRATA_DCHECK(n > 0);
    return static_cast<uint32_t>(Next() % n);
  }

  // Returns true with probability 1/n. Requires n > 0.
  bool OneIn(uint32_t n) {
    STRATA_DCHECK(n > 0);
    return Uniform(n) == 0;
  }

  // Generates mostly small numbers with occasional large ones by picking
  // b in [0, max_log], then Uniform(1 << b).
  // Ideal for simulating skewed key/value lengths where short data dominates.
  uint32_t Skewed(int max_log) {
    STRATA_DCHECK(max_log >= 0);
    STRATA_DCHECK(max_log <= 31);
    return Uniform(1U << Uniform(static_cast<uint32_t>(max_log + 1)));
  }

 private:
  uint64_t state_;
};

}  // namespace strata

#endif  // STRATA_UTIL_RANDOM_H_
