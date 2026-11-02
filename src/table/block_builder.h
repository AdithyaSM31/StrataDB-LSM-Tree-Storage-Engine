#ifndef STRATA_TABLE_BLOCK_BUILDER_H_
#define STRATA_TABLE_BLOCK_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/comparator.h"

namespace strata {

// Builds one block: sorted key/value entries with prefix-compressed keys.
//
//   entry:  [shared: varint32][non_shared: varint32][value_length: varint32]
//           [key bytes after the shared prefix][value]
//   block:  entries..., [restart offset: fixed32]..., [num_restarts: fixed32]
//
// `shared` is the length of the prefix this key has in common with the previous
// key. Every `restart_interval` entries the key is stored whole (shared == 0)
// and its offset is recorded as a restart point, so a reader can binary search
// the restart points instead of decoding the block from the start. The first
// entry is always a restart point, so even an empty block has one (offset 0).
//
// The block trailer (compression type + CRC) is not part of this format; the
// table layer adds and checks it.
class BlockBuilder {
 public:
  // `comparator` is used only to check (in debug builds) that keys are added in
  // strictly increasing order. It may be null to skip that check.
  explicit BlockBuilder(int restart_interval, const Comparator* comparator = nullptr);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // REQUIRES: Finish() has not been called since the last Reset().
  // REQUIRES: key is strictly greater than every key added so far.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block. The returned view
  // stays valid until Reset() or the builder is destroyed.
  std::string_view Finish();

  // The size Finish() would return if called now. The table builder uses this
  // to cut a block once it reaches options.block_size.
  size_t CurrentSizeEstimate() const;

  // True iff no entries have been added since construction or Reset().
  bool empty() const { return buffer_.empty(); }

  // Starts a new block, as if freshly constructed.
  void Reset();

 private:
  const int restart_interval_;
  const Comparator* const comparator_;
  std::string buffer_;              // entries so far, then the trailer after Finish()
  std::vector<uint32_t> restarts_;  // offsets of restart points; starts as {0}
  int counter_ = 0;                 // entries since the last restart point
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace strata

#endif  // STRATA_TABLE_BLOCK_BUILDER_H_
