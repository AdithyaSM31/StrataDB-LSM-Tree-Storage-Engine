#ifndef STRATA_UTIL_COMPARATOR_H_
#define STRATA_UTIL_COMPARATOR_H_

#include <string_view>

namespace strata {

// A total order over byte strings. Blocks and tables are written in this order
// and searched with it, so an implementation must be a strict weak ordering
// and must never change once data has been written with it.
class Comparator {
 public:
  virtual ~Comparator() = default;

  // Returns < 0 if a < b, 0 if a == b, > 0 if a > b.
  virtual int Compare(std::string_view a, std::string_view b) const = 0;

  // A name identifying the order. It will be recorded on disk (Week 2's
  // MANIFEST), so a database cannot be reopened with a different order.
  virtual const char* Name() const = 0;
};

// Compares bytes as unsigned values, lexicographically ("\xff" sorts after
// "a"). Returns a process-wide singleton that is never destroyed.
const Comparator* BytewiseComparator();

}  // namespace strata

#endif  // STRATA_UTIL_COMPARATOR_H_
