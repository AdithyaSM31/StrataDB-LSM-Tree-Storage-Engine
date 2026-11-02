#include "table/merger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
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

std::unique_ptr<Iterator> MakeBlockIterator(
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::vector<std::unique_ptr<Block>>* blocks) {
  auto block = MakeBlock(entries);
  auto iter = block->NewIterator(BytewiseComparator());
  blocks->push_back(std::move(block));
  return iter;
}

}  // namespace

TEST(MergerTest, EmptyChildrenList) {
  std::vector<std::unique_ptr<Iterator>> children;
  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());

  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());

  iter->SeekToLast();
  EXPECT_FALSE(iter->Valid());

  iter->Seek("any");
  EXPECT_FALSE(iter->Valid());
}

TEST(MergerTest, SingleChild) {
  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(MakeBlockIterator({{"a", "1"}, {"b", "2"}, {"c", "3"}}, &blocks));

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));
  EXPECT_FALSE(iter->Valid());

  // Forward
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "1");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "c");
  iter->Next();
  EXPECT_FALSE(iter->Valid());

  // Backward
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "c");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(MergerTest, EmptyChildrenMixedWithNonEmpty) {
  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(MakeBlockIterator({}, &blocks));
  children.push_back(MakeBlockIterator({{"b", "2"}, {"d", "4"}}, &blocks));
  children.push_back(MakeBlockIterator({}, &blocks));
  children.push_back(MakeBlockIterator({{"a", "1"}, {"c", "3"}}, &blocks));
  children.push_back(MakeBlockIterator({}, &blocks));

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));

  // Forward
  std::vector<std::pair<std::string, std::string>> seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  std::vector<std::pair<std::string, std::string>> expected = {
      {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}};
  EXPECT_EQ(seen, expected);

  // Backward
  seen.clear();
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  std::reverse(expected.begin(), expected.end());
  EXPECT_EQ(seen, expected);
}

TEST(MergerTest, DisjointAndOverlappingChildren) {
  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(MakeBlockIterator({{"a", "v_a"}, {"c", "v_c1"}, {"e", "v_e"}}, &blocks));
  children.push_back(MakeBlockIterator({{"b", "v_b"}, {"c", "v_c2"}, {"d", "v_d"}}, &blocks));
  children.push_back(MakeBlockIterator({{"f", "v_f"}}, &blocks));

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));

  std::vector<std::pair<std::string, std::string>> seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  EXPECT_EQ(seen.size(), 7u);
  EXPECT_EQ(seen[0], (std::pair<std::string, std::string>{"a", "v_a"}));
  EXPECT_EQ(seen[1], (std::pair<std::string, std::string>{"b", "v_b"}));
  // Two 'c' keys
  EXPECT_EQ(seen[2].first, "c");
  EXPECT_EQ(seen[3].first, "c");
  EXPECT_EQ(seen[4], (std::pair<std::string, std::string>{"d", "v_d"}));
  EXPECT_EQ(seen[5], (std::pair<std::string, std::string>{"e", "v_e"}));
  EXPECT_EQ(seen[6], (std::pair<std::string, std::string>{"f", "v_f"}));

  // Reverse
  seen.clear();
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    seen.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  EXPECT_EQ(seen.size(), 7u);
  EXPECT_EQ(seen[0], (std::pair<std::string, std::string>{"f", "v_f"}));
  EXPECT_EQ(seen[1], (std::pair<std::string, std::string>{"e", "v_e"}));
  EXPECT_EQ(seen[2], (std::pair<std::string, std::string>{"d", "v_d"}));
  EXPECT_EQ(seen[3].first, "c");
  EXPECT_EQ(seen[4].first, "c");
  EXPECT_EQ(seen[5], (std::pair<std::string, std::string>{"b", "v_b"}));
  EXPECT_EQ(seen[6], (std::pair<std::string, std::string>{"a", "v_a"}));
}

TEST(MergerTest, SeekTarget) {
  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(MakeBlockIterator({{"10", "v10"}, {"30", "v30"}, {"50", "v50"}}, &blocks));
  children.push_back(MakeBlockIterator({{"20", "v20"}, {"40", "v40"}, {"60", "v60"}}, &blocks));

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));

  // Seek exact
  iter->Seek("30");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "30");

  // Seek inexact (between keys)
  iter->Seek("25");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "30");

  // Seek before all
  iter->Seek("05");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "10");

  // Seek after all
  iter->Seek("99");
  EXPECT_FALSE(iter->Valid());
}

TEST(MergerTest, SwitchingDirectionRepeatedlyAgainstSortedModel) {
  // Build a model of entries split across multiple children
  std::vector<std::pair<std::string, std::string>> all_entries;
  static constexpr int kNumEntries = 200;
  for (int i = 0; i < kNumEntries; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%05d", i);
    all_entries.emplace_back(std::string(buf), "v" + std::to_string(i));
  }

  // Distribute entries across 5 children
  std::vector<std::vector<std::pair<std::string, std::string>>> child_entries(5);
  for (size_t i = 0; i < all_entries.size(); ++i) {
    child_entries[i % 5].push_back(all_entries[i]);
  }

  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  for (int i = 0; i < 5; ++i) {
    children.push_back(MakeBlockIterator(child_entries[i], &blocks));
  }

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));

  // Test random walks: start at SeekToFirst, then randomly choose Next or Prev
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  int model_idx = 0;
  EXPECT_EQ(iter->key(), all_entries[model_idx].first);

  std::mt19937 rng(42);
  for (int step = 0; step < 1000; ++step) {
    bool go_forward = (rng() % 2 == 1);
    if (go_forward) {
      if (model_idx + 1 < static_cast<int>(all_entries.size())) {
        ++model_idx;
        iter->Next();
        ASSERT_TRUE(iter->Valid()) << "step " << step;
        EXPECT_EQ(iter->key(), all_entries[model_idx].first);
      }
    } else {
      if (model_idx > 0) {
        --model_idx;
        iter->Prev();
        ASSERT_TRUE(iter->Valid()) << "step " << step;
        EXPECT_EQ(iter->key(), all_entries[model_idx].first);
      }
    }
  }

  // Also test Seek followed by Prev and Next
  iter->Seek("k00050");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k00050");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k00049");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k00050");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k00051");
}

TEST(MergerTest, ErrorPropagatesFromChild) {
  std::vector<std::unique_ptr<Block>> blocks;
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(MakeBlockIterator({{"a", "1"}}, &blocks));
  children.push_back(NewErrorIterator(Status::Corruption("simulated disk error")));
  children.push_back(MakeBlockIterator({{"b", "2"}}, &blocks));

  auto iter = NewMergingIterator(BytewiseComparator(), std::move(children));
  EXPECT_FALSE(iter->status().ok());
  EXPECT_TRUE(iter->status().IsCorruption());
  EXPECT_EQ(iter->status().ToString(), "Corruption: simulated disk error");
}

}  // namespace strata
