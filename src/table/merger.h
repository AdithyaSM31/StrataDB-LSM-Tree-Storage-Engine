#ifndef STRATA_TABLE_MERGER_H_
#define STRATA_TABLE_MERGER_H_

#include <memory>
#include <vector>

#include "strata/iterator.h"
#include "util/comparator.h"

namespace strata {

// Return an iterator that provides the union of the data in
// children[0, ... children.size() - 1].
//
// The result does no synchronization and is not thread-safe.
// Each child must be ordered by the provided comparator.
// If multiple children contain identical keys, all occurrences are yielded.
std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children);

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::unique_ptr<Iterator>* children, int n);

}  // namespace strata

#endif  // STRATA_TABLE_MERGER_H_
