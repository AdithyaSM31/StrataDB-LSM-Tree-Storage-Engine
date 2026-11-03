#include "db/db_iter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "strata/status.h"
#include "util/check.h"
#include "util/comparator.h"

namespace strata {
namespace {

class VectorInternalIterator : public Iterator {
 public:
  VectorInternalIterator(std::vector<std::pair<std::string, std::string>> entries,
                         const InternalKeyComparator* icmp)
      : entries_(std::move(entries)), icmp_(icmp), index_(entries_.size()) {
    std::sort(entries_.begin(), entries_.end(), [this](const auto& a, const auto& b) {
      return icmp_->Compare(a.first, b.first) < 0;
    });
  }

  ~VectorInternalIterator() override = default;

  bool Valid() const override { return index_ < entries_.size(); }

  void SeekToFirst() override { index_ = 0; }

  void SeekToLast() override { index_ = entries_.empty() ? 0 : entries_.size() - 1; }

  void Seek(std::string_view target) override {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), target,
                               [this](const auto& entry, std::string_view t) {
                                 return icmp_->Compare(entry.first, t) < 0;
                               });
    index_ = static_cast<size_t>(it - entries_.begin());
  }

  void Next() override {
    STRATA_DCHECK(Valid());
    ++index_;
  }

  void Prev() override {
    STRATA_DCHECK(Valid());
    if (index_ == 0) {
      index_ = entries_.size();
    } else {
      --index_;
    }
  }

  std::string_view key() const override {
    STRATA_DCHECK(Valid());
    return entries_[index_].first;
  }

  std::string_view value() const override {
    STRATA_DCHECK(Valid());
    return entries_[index_].second;
  }

  Status status() const override { return status_; }

  void SetStatus(Status s) { status_ = std::move(s); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  const InternalKeyComparator* icmp_;
  size_t index_;
  Status status_;
};

std::unique_ptr<DBIter> CreateDBIter(std::vector<std::pair<std::string, std::string>> entries,
                                     SequenceNumber seq, const InternalKeyComparator* icmp) {
  auto internal_iter = std::make_unique<VectorInternalIterator>(std::move(entries), icmp);
  return NewDBIterator(BytewiseComparator(), std::move(internal_iter), seq);
}

TEST(DBIterTest, EmptyIterator) {
  InternalKeyComparator icmp;
  auto iter = CreateDBIter({}, 100, &icmp);

  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());

  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());

  iter->SeekToLast();
  EXPECT_FALSE(iter->Valid());

  iter->Seek("foo");
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, SingleEntryForwardAndBackward) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("key1", 10, kTypeValue), "val1"}};

  auto iter = CreateDBIter(entries, 100, &icmp);

  // Forward
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "key1");
  EXPECT_EQ(iter->value(), "val1");
  iter->Next();
  EXPECT_FALSE(iter->Valid());

  // Backward
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "key1");
  EXPECT_EQ(iter->value(), "val1");
  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, SnapshotHidesFutureKeys) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("k1", 10, kTypeValue), "v1"},
      {MakeInternalKey("k2", 20, kTypeValue), "v2"},
      {MakeInternalKey("k3", 30, kTypeValue), "v3"}};

  // As of seq 15, only k1 is visible
  auto iter15 = CreateDBIter(entries, 15, &icmp);
  iter15->SeekToFirst();
  ASSERT_TRUE(iter15->Valid());
  EXPECT_EQ(iter15->key(), "k1");
  iter15->Next();
  EXPECT_FALSE(iter15->Valid());

  iter15->SeekToLast();
  ASSERT_TRUE(iter15->Valid());
  EXPECT_EQ(iter15->key(), "k1");
  iter15->Prev();
  EXPECT_FALSE(iter15->Valid());

  // As of seq 25, k1 and k2 are visible
  auto iter25 = CreateDBIter(entries, 25, &icmp);
  iter25->SeekToFirst();
  ASSERT_TRUE(iter25->Valid());
  EXPECT_EQ(iter25->key(), "k1");
  iter25->Next();
  ASSERT_TRUE(iter25->Valid());
  EXPECT_EQ(iter25->key(), "k2");
  iter25->Next();
  EXPECT_FALSE(iter25->Valid());

  iter25->SeekToLast();
  ASSERT_TRUE(iter25->Valid());
  EXPECT_EQ(iter25->key(), "k2");
  iter25->Prev();
  ASSERT_TRUE(iter25->Valid());
  EXPECT_EQ(iter25->key(), "k1");
  iter25->Prev();
  EXPECT_FALSE(iter25->Valid());
}

TEST(DBIterTest, MultipleVersionsAndDeduplication) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("a", 10, kTypeValue), "a_v1"},
      {MakeInternalKey("a", 20, kTypeValue), "a_v2"},
      {MakeInternalKey("a", 30, kTypeValue), "a_v3"},
      {MakeInternalKey("b", 15, kTypeValue), "b_v1"},
      {MakeInternalKey("b", 25, kTypeValue), "b_v2"}};

  // At seq 22: "a" should be "a_v2", "b" should be "b_v1"
  auto iter = CreateDBIter(entries, 22, &icmp);

  // Forward scan
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "a_v2");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  EXPECT_EQ(iter->value(), "b_v1");
  iter->Next();
  EXPECT_FALSE(iter->Valid());

  // Reverse scan
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  EXPECT_EQ(iter->value(), "b_v1");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "a_v2");
  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, TombstoneHidesKeyAndOlderVersions) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("k1", 10, kTypeValue), "v1"},
      {MakeInternalKey("k2", 10, kTypeValue), "v2_old"},
      {MakeInternalKey("k2", 20, kTypeDeletion), ""},
      {MakeInternalKey("k3", 10, kTypeValue), "v3"}};

  // At seq 25, k2 is deleted
  auto iter = CreateDBIter(entries, 25, &icmp);

  // Forward
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k1");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k3");
  iter->Next();
  EXPECT_FALSE(iter->Valid());

  // Reverse
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k3");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k1");
  iter->Prev();
  EXPECT_FALSE(iter->Valid());

  // Seek to deleted key positions at next key
  iter->Seek("k2");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "k3");
}

TEST(DBIterTest, ReinsertionAfterDeletion) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("k", 10, kTypeValue), "v1"},
      {MakeInternalKey("k", 20, kTypeDeletion), ""},
      {MakeInternalKey("k", 30, kTypeValue), "v2"}};

  // At seq 15: v1
  auto iter15 = CreateDBIter(entries, 15, &icmp);
  iter15->SeekToFirst();
  ASSERT_TRUE(iter15->Valid());
  EXPECT_EQ(iter15->value(), "v1");

  // At seq 25: deleted (invisible)
  auto iter25 = CreateDBIter(entries, 25, &icmp);
  iter25->SeekToFirst();
  EXPECT_FALSE(iter25->Valid());

  // At seq 35: v2
  auto iter35 = CreateDBIter(entries, 35, &icmp);
  iter35->SeekToFirst();
  ASSERT_TRUE(iter35->Valid());
  EXPECT_EQ(iter35->value(), "v2");

  iter35->SeekToLast();
  ASSERT_TRUE(iter35->Valid());
  EXPECT_EQ(iter35->value(), "v2");
}

TEST(DBIterTest, BidirectionalScanningAndDirectionReversals) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("1", 10, kTypeValue), "v1"},
      {MakeInternalKey("2", 10, kTypeValue), "v2"},
      {MakeInternalKey("3", 10, kTypeValue), "v3"},
      {MakeInternalKey("4", 10, kTypeValue), "v4"},
      {MakeInternalKey("5", 10, kTypeValue), "v5"}};

  auto iter = CreateDBIter(entries, 100, &icmp);

  // Start at 1, advance to 3
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "1");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "2");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "3");

  // Reverse back to 2, then 1
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "2");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "1");

  // Reverse before 1 becomes invalid
  iter->Prev();
  EXPECT_FALSE(iter->Valid());

  // Start at 5, advance backwards to 3
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "5");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "4");
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "3");

  // Reverse forward to 4, then 5
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "4");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "5");

  // Forward past 5 becomes invalid
  iter->Next();
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, SeekExactAndInBetween) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("b", 10, kTypeValue), "vb"},
      {MakeInternalKey("d", 10, kTypeValue), "vd"},
      {MakeInternalKey("f", 10, kTypeValue), "vf"}};

  auto iter = CreateDBIter(entries, 100, &icmp);

  // Seek before first
  iter->Seek("a");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");

  // Seek exact
  iter->Seek("d");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "d");

  // Seek between d and f
  iter->Seek("e");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "f");

  // Seek after last
  iter->Seek("g");
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, CleanupCallbacksInvokedInReverseOrder) {
  InternalKeyComparator icmp;
  std::vector<int> order;
  {
    auto iter = CreateDBIter({}, 100, &icmp);
    iter->RegisterCleanup([&order]() { order.push_back(1); });
    iter->RegisterCleanup([&order]() { order.push_back(2); });
    iter->RegisterCleanup([&order]() { order.push_back(3); });
  }
  std::vector<int> expected = {3, 2, 1};
  EXPECT_EQ(order, expected);
}

TEST(DBIterTest, CorruptionReported) {
  InternalKeyComparator icmp;
  // An invalid entry: shorter than 8 bytes
  std::vector<std::pair<std::string, std::string>> entries = {{"short", "v1"}};

  auto internal_iter = std::make_unique<VectorInternalIterator>(entries, &icmp);
  auto iter = NewDBIterator(BytewiseComparator(), std::move(internal_iter), 100);

  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().IsCorruption());
}

TEST(DBIterTest, EmptyKeyAndEmptyValue) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("", 10, kTypeValue), ""}, {MakeInternalKey("a", 10, kTypeValue), "val_a"}};

  auto iter = CreateDBIter(entries, 100, &icmp);

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "");
  EXPECT_EQ(iter->value(), "");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "val_a");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "");
  EXPECT_EQ(iter->value(), "");
}

TEST(DBIterTest, SeekToLastWithLastKeyDeleted) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("a", 10, kTypeValue), "va"}, {MakeInternalKey("z", 10, kTypeDeletion), ""}};

  auto iter = CreateDBIter(entries, 100, &icmp);

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "va");

  iter->Next();
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, ReverseScanSkippingFutureKeys) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("a", 10, kTypeValue), "va"},
      {MakeInternalKey("b", 50, kTypeValue), "vb_future"},
      {MakeInternalKey("b", 10, kTypeValue), "vb_vis"},
      {MakeInternalKey("c", 10, kTypeValue), "vc"}};

  auto iter = CreateDBIter(entries, 20, &icmp);

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "c");
  EXPECT_EQ(iter->value(), "vc");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  EXPECT_EQ(iter->value(), "vb_vis");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");
  EXPECT_EQ(iter->value(), "va");

  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(DBIterTest, DirectionSwitchOverFutureKey) {
  InternalKeyComparator icmp;
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("a", 10, kTypeValue), "va"},
      {MakeInternalKey("b", 100, kTypeValue), "vb_future1"},
      {MakeInternalKey("b", 50, kTypeValue), "vb_future2"},
      {MakeInternalKey("b", 10, kTypeValue), "vb_vis"},
      {MakeInternalKey("c", 10, kTypeValue), "vc"}};

  auto iter = CreateDBIter(entries, 20, &icmp);

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "c");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  EXPECT_EQ(iter->value(), "vb_vis");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");

  // Switch direction to forward: should land on "b" with vb_vis
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "b");
  EXPECT_EQ(iter->value(), "vb_vis");

  // Switch direction to reverse: should land on "a"
  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), "a");

  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

}  // namespace
}  // namespace strata
