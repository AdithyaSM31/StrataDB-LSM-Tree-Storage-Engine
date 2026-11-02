#include "util/bloom.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/hash.h"

namespace strata {
namespace {

// Seed shared with LevelDB's bloom filter, so this is the same hash LevelDB
// uses for its filters (the probe count differs; see the constructor).
constexpr uint32_t kBloomSeed = 0xbc9f1d34U;
constexpr int kMaxProbes = 30;   // larger k bytes are reserved for future encodings
constexpr size_t kMinBits = 64;  // tiny filters would otherwise have very high FP rates

uint32_t BloomHash(std::string_view key) { return Hash(key.data(), key.size(), kBloomSeed); }

// Rotate right by 17 bits: the step between probes.
uint32_t ProbeDelta(uint32_t h) { return (h >> 17) | (h << 15); }

}  // namespace

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key) : bits_per_key_(std::max(bits_per_key, 1)) {
  // The optimal probe count is (m/n) * ln 2. Round it (10 bits per key gives
  // 6.93 -> 7), then clamp to [1, kMaxProbes].
  const long k = std::lround(static_cast<double>(bits_per_key_) * 0.69314718056);
  k_ = static_cast<int>(std::clamp(k, 1L, static_cast<long>(kMaxProbes)));
}

void BloomFilterPolicy::CreateFilter(const std::string_view* keys, size_t n,
                                     std::string* dst) const {
  size_t bits = n * static_cast<size_t>(bits_per_key_);
  bits = std::max(bits, kMinBits);
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, '\0');
  dst->push_back(static_cast<char>(k_));  // remember k in the filter itself
  char* const array = dst->data() + init_size;

  for (size_t i = 0; i < n; ++i) {
    uint32_t h = BloomHash(keys[i]);
    const uint32_t delta = ProbeDelta(h);
    for (int j = 0; j < k_; ++j) {
      const size_t bit_pos = h % bits;
      array[bit_pos / 8] =
          static_cast<char>(static_cast<uint8_t>(array[bit_pos / 8]) | (1U << (bit_pos % 8)));
      h += delta;
    }
  }
}

bool BloomFilterPolicy::KeyMayMatch(std::string_view key, std::string_view filter) const {
  const size_t len = filter.size();
  if (len < 2) {
    return false;  // no bits at all: an empty filter matches nothing
  }
  const size_t bits = (len - 1) * 8;

  // Use the k recorded in the filter, not ours: the file may have been written
  // with a different bits_per_key.
  const int k = static_cast<uint8_t>(filter[len - 1]);
  if (k > kMaxProbes) {
    return true;  // an encoding we do not understand: "maybe" is always safe
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = ProbeDelta(h);
  for (int j = 0; j < k; ++j) {
    const size_t bit_pos = h % bits;
    if ((static_cast<uint8_t>(filter[bit_pos / 8]) & (1U << (bit_pos % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace strata
