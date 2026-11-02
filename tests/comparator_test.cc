#include "util/comparator.h"

#include <gtest/gtest.h>

#include <string>

#include "db/dbformat.h"

namespace strata {
namespace {

TEST(ComparatorTest, BytewiseOrdersUnsignedBytes) {
  const Comparator* cmp = BytewiseComparator();
  EXPECT_EQ(cmp, BytewiseComparator());  // a singleton
  EXPECT_LT(cmp->Compare("a", "b"), 0);
  EXPECT_GT(cmp->Compare("b", "a"), 0);
  EXPECT_EQ(cmp->Compare("abc", "abc"), 0);
  EXPECT_LT(cmp->Compare("", "a"), 0);
  EXPECT_LT(cmp->Compare("a", "ab"), 0);
  // Bytes compare as unsigned: 0xff sorts after 'a', and 0x80 after 0x7f.
  EXPECT_GT(cmp->Compare("\xff", "a"), 0);
  EXPECT_GT(cmp->Compare("\x80", "\x7f"), 0);
  EXPECT_LT(cmp->Compare(std::string("\0", 1), "\x01"), 0);
  EXPECT_STREQ(cmp->Name(), "strata.BytewiseComparator");
}

TEST(ComparatorTest, InternalKeyComparatorThroughTheInterface) {
  const InternalKeyComparator icmp;
  const Comparator* cmp = &icmp;
  const std::string newer = MakeInternalKey("k", 9, kTypeValue);
  const std::string older = MakeInternalKey("k", 3, kTypeValue);
  // Same answers as calling it directly: newest version first.
  EXPECT_LT(cmp->Compare(newer, older), 0);
  EXPECT_EQ(cmp->Compare(newer, older), icmp.Compare(newer, older));
  EXPECT_STREQ(cmp->Name(), "strata.InternalKeyComparator");
}

}  // namespace
}  // namespace strata
