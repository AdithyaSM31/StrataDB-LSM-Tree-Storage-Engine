#include "table/two_level_iterator.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "strata/iterator.h"
#include "strata/status.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "util/comparator.h"

namespace strata {
namespace {

std::unique_ptr<Block> MakeBlock(const std::vector<std::pair<std::string, std::string>>& entries) {
  BlockBuilder builder(1, BytewiseComparator());
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }
  return std::make_unique<Block>(std::string(builder.Finish()));
}

// Four "data blocks" named "0".."3" by the index values:
//   0: a, b      1: (empty)      2: e, f      3: unreadable (Corruption)
// The index key of each block is (at least) its last key.
class TwoLevelIteratorTest : public ::testing::Test {
 protected:
  TwoLevelIteratorTest() {
    blocks_["0"] = MakeBlock({{"a", "A"}, {"b", "B"}});
    blocks_["1"] = MakeBlock({});
    blocks_["2"] = MakeBlock({{"e", "E"}, {"f", "F"}});
    index_ = MakeBlock({{"b", "0"}, {"c", "1"}, {"f", "2"}, {"h", "3"}});
  }

  std::unique_ptr<Iterator> NewIterator(int* block_reads = nullptr) {
    return NewTwoLevelIterator(index_->NewIterator(BytewiseComparator()),
                               [this, block_reads](std::string_view name) {
                                 if (block_reads != nullptr) {
                                   ++*block_reads;
                                 }
                                 auto it = blocks_.find(std::string(name));
                                 if (it == blocks_.end()) {
                                   return NewErrorIterator(Status::Corruption("bad block", name));
                                 }
                                 return it->second->NewIterator(BytewiseComparator());
                               });
  }

  std::map<std::string, std::unique_ptr<Block>> blocks_;
  std::unique_ptr<Block> index_;
};

TEST_F(TwoLevelIteratorTest, ForwardSkipsEmptyBlocksAndRemembersErrors) {
  std::unique_ptr<Iterator> it = NewIterator();
  std::string seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen += std::string(it->key()) + "=" + std::string(it->value()) + " ";
  }
  EXPECT_EQ(seen, "a=A b=B e=E f=F ");
  // Block 3 could not be read: iteration ended, and the error is reported.
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

TEST_F(TwoLevelIteratorTest, BackwardSkipsEmptyBlocksAndRemembersErrors) {
  std::unique_ptr<Iterator> it = NewIterator();
  std::string seen;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    seen += std::string(it->key()) + " ";
  }
  // SeekToLast lands on the unreadable block 3, skips back past it, then past
  // the empty block 1.
  EXPECT_EQ(seen, "f e b a ");
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(TwoLevelIteratorTest, Seek) {
  std::unique_ptr<Iterator> it = NewIterator();
  it->Seek("a");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "a");

  it->Seek("ab");  // between a and b, same block
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");

  it->Seek("c");  // the index points at the empty block 1; skip to block 2
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "e");
  EXPECT_TRUE(it->status().ok());

  it->Seek("z");  // past every index key
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST_F(TwoLevelIteratorTest, SeekWithinTheSameBlockDoesNotReread) {
  int reads = 0;
  std::unique_ptr<Iterator> it = NewIterator(&reads);
  it->Seek("a");
  it->Seek("b");
  it->Seek("a");
  EXPECT_EQ(reads, 1);
  it->Seek("e");  // a different block
  EXPECT_EQ(reads, 2);
}

TEST(TwoLevelIteratorEmptyTest, EmptyIndex) {
  std::unique_ptr<Iterator> it =
      NewTwoLevelIterator(NewEmptyIterator(), [](std::string_view) { return NewEmptyIterator(); });
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  EXPECT_FALSE(it->Valid());
  it->Seek("x");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(TwoLevelIteratorEmptyTest, IndexErrorIsReported) {
  std::unique_ptr<Iterator> it =
      NewTwoLevelIterator(NewErrorIterator(Status::Corruption("bad index")),
                          [](std::string_view) { return NewEmptyIterator(); });
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

}  // namespace
}  // namespace strata
