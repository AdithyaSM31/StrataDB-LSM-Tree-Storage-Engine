#ifndef STRATA_TABLE_TWO_LEVEL_ITERATOR_H_
#define STRATA_TABLE_TWO_LEVEL_ITERATOR_H_

#include <functional>
#include <memory>
#include <string_view>

#include "strata/iterator.h"

namespace strata {

// Turns an index value (for a table, an encoded BlockHandle) into an iterator
// over that block. It returns NewErrorIterator(status) if the block cannot be
// read. The returned iterator must own whatever it reads from.
using BlockFunction = std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;

// Returns an iterator over the concatenation of the blocks named by
// `index_iter`'s values, in index order.
//
// The index's keys must be ordered so that seeking the index to a target lands
// on the first block that can contain a key >= target. For an SSTable the index
// key is each data block's last key; in Week 3 the same iterator walks a level's
// files, indexed by each file's largest key.
//
// Empty blocks are skipped. An error from a block is remembered and reported by
// status(), and iteration moves on to the next block.
std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function);

}  // namespace strata

#endif  // STRATA_TABLE_TWO_LEVEL_ITERATOR_H_
