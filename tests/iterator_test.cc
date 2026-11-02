#include "strata/iterator.h"

#include <gtest/gtest.h>

#include <memory>

#include "strata/status.h"

namespace strata {
namespace {

void ExpectNeverValid(Iterator* it) {
  EXPECT_FALSE(it->Valid());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  EXPECT_FALSE(it->Valid());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
}

TEST(IteratorTest, EmptyIteratorIsNeverValidAndOk) {
  std::unique_ptr<Iterator> it = NewEmptyIterator();
  ASSERT_NE(it, nullptr);
  ExpectNeverValid(it.get());
  EXPECT_TRUE(it->status().ok());
}

TEST(IteratorTest, ErrorIteratorCarriesItsStatus) {
  std::unique_ptr<Iterator> it = NewErrorIterator(Status::Corruption("bad block", "000007.sst"));
  ASSERT_NE(it, nullptr);
  ExpectNeverValid(it.get());
  EXPECT_TRUE(it->status().IsCorruption());
  EXPECT_EQ(it->status().ToString(), "Corruption: bad block: 000007.sst");
}

#if STRATA_DEBUG_CHECKS
TEST(IteratorTest, AccessingAnInvalidIteratorAborts) {
  std::unique_ptr<Iterator> it = NewEmptyIterator();
  EXPECT_DEATH(it->Next(), "check failed");
  EXPECT_DEATH(it->Prev(), "check failed");
  EXPECT_DEATH((void)it->key(), "check failed");
  EXPECT_DEATH((void)it->value(), "check failed");
}
#endif

}  // namespace
}  // namespace strata
