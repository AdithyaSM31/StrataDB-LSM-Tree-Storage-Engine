#ifndef STRATA_TABLE_BLOCK_H_
#define STRATA_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "strata/iterator.h"
#include "util/comparator.h"

namespace strata {

// A read-only block in the format written by BlockBuilder.
//
// The contents may come from disk, so nothing in them is trusted: a malformed
// block makes its iterator report Status::Corruption, and never crashes or
// reads outside the block.
class Block {
 public:
  // Takes ownership of the block contents (without the table's trailer).
  explicit Block(std::string contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }

  // Returns an iterator over the block's entries, ordered by `comparator`.
  // The Block (and the comparator) must outlive the returned iterator.
  std::unique_ptr<Iterator> NewIterator(const Comparator* comparator) const;

 private:
  class Iter;

  std::string data_;
  uint32_t restart_offset_ = 0;  // where the restart array starts
  uint32_t num_restarts_ = 0;
  bool malformed_ = false;
};

}  // namespace strata

#endif  // STRATA_TABLE_BLOCK_H_
