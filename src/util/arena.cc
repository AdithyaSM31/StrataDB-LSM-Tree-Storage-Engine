#include "util/arena.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "util/check.h"

namespace strata {

Arena::Arena() : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), blocks_(), memory_usage_(0) {}

Arena::~Arena() = default;

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Large allocation (> 1 KB): allocate a dedicated block of exactly `bytes`.
    // The current 4 KB block is preserved for subsequent smaller requests,
    // capping fragmentation waste to at most 25% of a block.
    return AllocateNewBlock(bytes);
  }

  // Wasted remainder of current block is small (< 1 KB).
  // Discard remaining space in current block and start a fresh 4 KB block.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  STRATA_DCHECK(bytes > 0);
  constexpr size_t kAlign = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((kAlign & (kAlign - 1)) == 0, "kAlign must be a power of two");

  if (bytes > kBlockSize / 4) {
    char* result = AllocateFallback(bytes);
    STRATA_DCHECK((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
    return result;
  }

  const size_t mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t slop = (mod == 0) ? 0 : kAlign - mod;
  const size_t needed = bytes + slop;

  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback allocates a fresh block (either dedicated if bytes > kBlockSize / 4,
    // or a fresh 4 KB block). In either case, standard heap allocation guarantees alignment
    // to std::max_align_t (at least 8 bytes, typically 16 bytes on 64-bit systems).
    result = AllocateFallback(bytes);
  }
  STRATA_DCHECK((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  // Use default initialization (new char[]) to avoid zero-filling the block.
  std::unique_ptr<char[]> block(new char[block_bytes]);
  char* result = block.get();
  blocks_.push_back(std::move(block));
  memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
  return result;
}

}  // namespace strata
