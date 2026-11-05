// Zipfian key generator using Gray's algorithm, as used by YCSB.
//
// The distribution is parameterised by theta (default 0.99). After
// construction, calling Next() on the generator returns a scrambled rank in
// [0, n) drawn from the Zipf distribution. Scrambling uses FNV-1a 64-bit so
// that the hottest keys do not all sit next to each other at the low end of
// the key space.

#ifndef STRATA_UTIL_ZIPFIAN_H_
#define STRATA_UTIL_ZIPFIAN_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>

namespace strata {

// Compute zeta(n, theta) = sum_{i=1}^{n} 1/i^theta.
// This is O(n) and should be called once before the timed section.
inline double ZipfZeta(uint64_t n, double theta) {
  if (n == 0) {
    throw std::invalid_argument("ZipfZeta: n must be >= 1");
  }
  if (theta <= 0.0 || theta >= 1.0) {
    throw std::invalid_argument("ZipfZeta: theta must be in (0, 1); got " + std::to_string(theta));
  }
  double z = 0.0;
  for (uint64_t i = 1; i <= n; ++i) {
    z += 1.0 / std::pow(static_cast<double>(i), theta);
  }
  return z;
}

// FNV-1a 64-bit hash used to scramble the raw rank.
inline uint64_t Fnv1a64(uint64_t value) {
  constexpr uint64_t kOffset = UINT64_C(14695981039346656037);
  constexpr uint64_t kPrime = UINT64_C(1099511628211);
  uint64_t h = kOffset;
  for (int byte = 0; byte < 8; ++byte) {
    h ^= static_cast<uint64_t>(value & 0xFF);
    h *= kPrime;
    value >>= 8;
  }
  return h;
}

// Zipfian generator. Construct once per thread; it is NOT thread-safe.
class ZipfianGenerator {
 public:
  // n      – number of distinct keys (must be >= 1)
  // theta  – skew parameter in (0, 1); default is 0.99 (YCSB default)
  // zeta_n – pre-computed ZipfZeta(n, theta); pass the value computed once
  //          outside the timed section so the O(n) work is not repeated.
  // seed   – seed for the internal PRNG
  ZipfianGenerator(uint64_t n, double theta, double zeta_n, uint64_t seed)
      : n_(n), theta_(theta), zeta_n_(zeta_n), rng_(seed) {
    if (theta >= 1.0 || theta <= 0.0) {
      throw std::invalid_argument("ZipfianGenerator: theta must be in (0, 1); got " +
                                  std::to_string(theta));
    }
    if (n == 0) {
      throw std::invalid_argument("ZipfianGenerator: n must be >= 1");
    }
    if (zeta_n <= 0.0) {
      throw std::invalid_argument("ZipfianGenerator: zeta_n must be > 0");
    }

    pow_half_theta_ = std::pow(0.5, theta_);
    const double zeta2 = 1.0 + pow_half_theta_;
    alpha_ = 1.0 / (1.0 - theta);

    // eta: (1 - (2/n)^(1-theta)) / (1 - zeta(2)/zeta(n))
    // For n <= 2 the formula degenerates: eta is 0/0 at n=2 and unused at n=1.
    // The uz < 1 and uz < 1 + 0.5^theta branches handle ranks 0 and 1, so
    // rank >= 2 is never reached for n <= 2.
    if (n_ > 2) {
      eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta)) / (1.0 - zeta2 / zeta_n_);
    } else {
      eta_ = 0.0;
    }
  }

  // Returns the unscrambled rank in [0, n).
  uint64_t NextRank() {
    if (n_ == 1) {
      return 0;
    }
    const double u = dist_(rng_);
    const double uz = u * zeta_n_;

    if (uz < 1.0) {
      return 0;
    }
    if (uz < 1.0 + pow_half_theta_ || n_ == 2) {
      return 1;
    }
    const double base = std::max(0.0, eta_ * u - eta_ + 1.0);
    const double raw = std::floor(static_cast<double>(n_) * std::pow(base, alpha_));
    return std::min(static_cast<uint64_t>(raw), n_ - 1);
  }

  // Returns a scrambled key in [0, n).
  uint64_t Next() { return Fnv1a64(NextRank()) % n_; }

  uint64_t n() const { return n_; }
  double theta() const { return theta_; }
  double zeta_n() const { return zeta_n_; }
  double eta() const { return eta_; }

 private:
  uint64_t n_;
  double theta_;
  double zeta_n_;
  double pow_half_theta_;
  double alpha_;
  double eta_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

}  // namespace strata

#endif  // STRATA_UTIL_ZIPFIAN_H_
