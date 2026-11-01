#ifndef STRATA_UTIL_ARENA_H_
#define STRATA_UTIL_ARENA_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "util/check.h"

namespace strata {

// Bump-pointer memory allocator for MemTable skiplist nodes.
//
// Why an arena for the MemTable?
// 1. MemTable nodes are never individually freed; the entire MemTable is discarded
//    all at once after being flushed to Level 0 SSTable. A bump allocator eliminates
//    per-node malloc/free overhead and gives excellent cache locality.
// 2. The MemTable needs to track its exact byte usage to determine when to freeze
//    and trigger a flush (typically write_buffer_size = 4 MB). Arena tracks memory
//    usage automatically in O(1).
//
// Thread safety:
// NOT thread-safe for allocation. Only ONE thread (the write leader) calls Allocate
// or AllocateAligned.
// However, MemoryUsage() is thread-safe and may be queried concurrently from background
// or coordinator threads (e.g. DBImpl::MakeRoomForWrite) without holding the DB mutex.
//
// Alignment:
// Skiplist nodes embed std::atomic<Node*> pointer arrays. Storing an atomic at an
// unaligned address causes undefined behavior (loss of hardware atomicity on x86,
// or hardware exceptions on architectures like ARM). AllocateAligned guarantees
// 8-byte alignment (matching pointer size).
//
// ASan limitation:
// AddressSanitizer cannot detect buffer overruns between distinct arena allocations
// within the same 4 KB block because the entire block is registered as one heap allocation.
class Arena {
 public:
  static constexpr size_t kBlockSize = 4096;

  Arena();
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Allocates `bytes` from the arena with no alignment guarantee.
  // Requires bytes > 0.
  char* Allocate(size_t bytes);

  // Allocates `bytes` aligned to max(sizeof(void*), 8) (at least 8 bytes).
  // Requires bytes > 0.
  char* AllocateAligned(size_t bytes);

  // Returns total memory allocated by the arena (including block overhead).
  // Thread-safe and safe to call concurrently with allocations.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_;
  size_t alloc_bytes_remaining_;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_;
};

inline char* Arena::Allocate(size_t bytes) {
  STRATA_DCHECK(bytes > 0);
  if (bytes <= alloc_bytes_remaining_ && bytes <= kBlockSize / 4) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace strata

#endif  // STRATA_UTIL_ARENA_H_
