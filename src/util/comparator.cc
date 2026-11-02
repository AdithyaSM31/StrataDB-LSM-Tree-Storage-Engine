#include "util/comparator.h"

#include <string_view>

namespace strata {
namespace {

class BytewiseComparatorImpl : public Comparator {
 public:
  // std::string_view::compare uses char_traits<char>::compare, which orders
  // bytes as unsigned char, so "\xff" > "a" on every platform.
  int Compare(std::string_view a, std::string_view b) const override { return a.compare(b); }

  const char* Name() const override { return "strata.BytewiseComparator"; }
};

}  // namespace

const Comparator* BytewiseComparator() {
  // Deliberately never destroyed, like Env::Default(): it may be used by
  // objects that outlive static destruction.
  static const Comparator* const kComparator = new BytewiseComparatorImpl();
  return kComparator;
}

}  // namespace strata
