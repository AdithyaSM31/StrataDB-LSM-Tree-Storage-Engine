#include "memtable/memtable.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "strata/status.h"

namespace strata {
namespace {

TEST(MemTableTest, AddAndGet) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  mem->Add(10, kTypeValue, "key1", "val1");
  mem->Add(11, kTypeValue, "key2", "val2");

  std::string value;
  Status s;

  LookupKey lk1("key1", 10);
  EXPECT_TRUE(mem->Get(lk1, &value, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(value, "val1");

  LookupKey lk2("key2", 15);
  EXPECT_TRUE(mem->Get(lk2, &value, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(value, "val2");

  mem->Unref();
}

TEST(MemTableTest, AbsentKey) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  mem->Add(1, kTypeValue, "foo", "bar");

  std::string value;
  Status s;
  LookupKey lk("nonexistent", 10);
  EXPECT_FALSE(mem->Get(lk, &value, &s));

  mem->Unref();
}

TEST(MemTableTest, PrefixMismatchReturnsFalse) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  // Only "ab" exists
  mem->Add(1, kTypeValue, "ab", "val_ab");

  std::string value;
  Status s;
  // Looking up "a" must not return "ab"
  LookupKey lk("a", 10);
  EXPECT_FALSE(mem->Get(lk, &value, &s));

  mem->Unref();
}

TEST(MemTableTest, Tombstones) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  // Add at seq 1, delete at seq 2
  mem->Add(1, kTypeValue, "k", "v1");
  mem->Add(2, kTypeDeletion, "k", "");

  std::string value;
  Status s;

  // As of seq 1: returns v1
  LookupKey lk1("k", 1);
  EXPECT_TRUE(mem->Get(lk1, &value, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(value, "v1");

  // As of seq 2: returns true, with NotFound status!
  LookupKey lk2("k", 2);
  s = Status::OK();
  value = "old";
  EXPECT_TRUE(mem->Get(lk2, &value, &s));
  EXPECT_TRUE(s.IsNotFound());

  // As of seq 3 (later than delete): returns true, with NotFound status!
  LookupKey lk3("k", 3);
  s = Status::OK();
  EXPECT_TRUE(mem->Get(lk3, &value, &s));
  EXPECT_TRUE(s.IsNotFound());

  mem->Unref();
}

TEST(MemTableTest, Snapshots) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  // Sequence of updates on "apple":
  // seq 1: "red"
  // seq 3: "green"
  // seq 5: deleted
  // seq 7: "yellow"
  mem->Add(1, kTypeValue, "apple", "red");
  mem->Add(3, kTypeValue, "apple", "green");
  mem->Add(5, kTypeDeletion, "apple", "");
  mem->Add(7, kTypeValue, "apple", "yellow");

  std::string value;
  Status s;

  // seq 0: not present
  EXPECT_FALSE(mem->Get(LookupKey("apple", 0), &value, &s));

  // seq 1, 2: "red"
  for (SequenceNumber seq : {1u, 2u}) {
    EXPECT_TRUE(mem->Get(LookupKey("apple", seq), &value, &s));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(value, "red");
  }

  // seq 3, 4: "green"
  for (SequenceNumber seq : {3u, 4u}) {
    EXPECT_TRUE(mem->Get(LookupKey("apple", seq), &value, &s));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(value, "green");
  }

  // seq 5, 6: deleted (returns true, IsNotFound)
  for (SequenceNumber seq : {5u, 6u}) {
    s = Status::OK();
    EXPECT_TRUE(mem->Get(LookupKey("apple", seq), &value, &s));
    EXPECT_TRUE(s.IsNotFound());
  }

  // seq 7, 8, 100: "yellow"
  for (SequenceNumber seq : {7u, 8u, 100u}) {
    EXPECT_TRUE(mem->Get(LookupKey("apple", seq), &value, &s));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(value, "yellow");
  }

  mem->Unref();
}

TEST(MemTableTest, IteratorOperations) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  mem->Add(10, kTypeValue, "b", "val_b_10");
  mem->Add(20, kTypeValue, "b", "val_b_20");
  mem->Add(15, kTypeValue, "a", "val_a_15");
  mem->Add(30, kTypeDeletion, "c", "");

  std::unique_ptr<Iterator> it = mem->NewIterator();
  EXPECT_TRUE(it->status().ok());

  // Test SeekToFirst and full forward iteration
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());

  ParsedInternalKey parsed;
  // 1: a @ 15
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "a");
  EXPECT_EQ(parsed.sequence, 15u);
  EXPECT_EQ(parsed.type, kTypeValue);
  EXPECT_EQ(it->value(), "val_a_15");

  // 2: b @ 20 (higher sequence sorts first)
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "b");
  EXPECT_EQ(parsed.sequence, 20u);
  EXPECT_EQ(parsed.type, kTypeValue);
  EXPECT_EQ(it->value(), "val_b_20");

  // 3: b @ 10
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "b");
  EXPECT_EQ(parsed.sequence, 10u);
  EXPECT_EQ(parsed.type, kTypeValue);
  EXPECT_EQ(it->value(), "val_b_10");

  // 4: c @ 30 (deletion)
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "c");
  EXPECT_EQ(parsed.sequence, 30u);
  EXPECT_EQ(parsed.type, kTypeDeletion);
  EXPECT_EQ(it->value(), "");

  // Past the end
  it->Next();
  EXPECT_FALSE(it->Valid());

  // SeekToLast and reverse iteration with Prev()
  it->SeekToLast();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "c");

  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "b");
  EXPECT_EQ(parsed.sequence, 10u);

  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "b");
  EXPECT_EQ(parsed.sequence, 20u);

  it->Prev();
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "a");

  it->Prev();
  EXPECT_FALSE(it->Valid());

  // Seek tests
  // Seek to target internal key for "b" @ 25: lands on "b" @ 20
  it->Seek(MakeInternalKey("b", 25, kValueTypeForSeek));
  ASSERT_TRUE(it->Valid());
  EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "b");
  EXPECT_EQ(parsed.sequence, 20u);

  // Seek past everything
  it->Seek(MakeInternalKey("z", 100, kValueTypeForSeek));
  EXPECT_FALSE(it->Valid());

  mem->Unref();
}

TEST(MemTableTest, ApproximateMemoryUsageGrows) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  size_t initial_usage = mem->ApproximateMemoryUsage();

  for (int i = 0; i < 500; ++i) {
    std::string k = "key_" + std::to_string(i);
    std::string v = std::string(100, 'x');
    mem->Add(static_cast<SequenceNumber>(i + 1), kTypeValue, k, v);
  }

  size_t final_usage = mem->ApproximateMemoryUsage();
  EXPECT_GT(final_usage, initial_usage);

  mem->Unref();
}

TEST(MemTableTest, ModelTestRandomAddsDeletesAndGets) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  struct ModelEntry {
    ValueType type;
    std::string value;
  };

  // Map: user_key -> map of sequence -> entry
  std::map<std::string, std::map<SequenceNumber, ModelEntry>> model;
  std::mt19937_64 rng(1337);

  std::vector<std::string> keys;
  for (int i = 0; i < 20; ++i) {
    keys.push_back("key_" + std::to_string(i));
  }

  SequenceNumber current_seq = 1;
  const int kNumOps = 2000;

  for (int op = 0; op < kNumOps; ++op) {
    int key_idx = rng() % keys.size();
    const std::string& k = keys[key_idx];
    bool is_delete = (rng() % 5 == 0);  // 20% deletes

    SequenceNumber seq = current_seq++;
    if (is_delete) {
      mem->Add(seq, kTypeDeletion, k, "");
      model[k][seq] = ModelEntry{kTypeDeletion, ""};
    } else {
      std::string val = "val_" + std::to_string(seq);
      mem->Add(seq, kTypeValue, k, val);
      model[k][seq] = ModelEntry{kTypeValue, val};
    }

    // Random Gets at various snapshot sequence numbers
    if (op % 10 == 0) {
      int read_key_idx = rng() % keys.size();
      const std::string& read_k = keys[read_key_idx];
      SequenceNumber read_seq = (rng() % current_seq) + 1;

      // Model lookup: find the newest version with seq <= read_seq
      auto& versions = model[read_k];
      auto it = versions.upper_bound(read_seq);  // first version > read_seq
      bool expected_found = false;
      bool expected_deleted = false;
      std::string expected_value;

      if (it != versions.begin()) {
        --it;  // now at greatest version <= read_seq
        expected_found = true;
        if (it->second.type == kTypeDeletion) {
          expected_deleted = true;
        } else {
          expected_value = it->second.value;
        }
      }

      std::string actual_value;
      Status s;
      LookupKey lk(read_k, read_seq);
      bool actual_found = mem->Get(lk, &actual_value, &s);

      EXPECT_EQ(actual_found, expected_found);
      if (expected_found) {
        if (expected_deleted) {
          EXPECT_TRUE(s.IsNotFound());
        } else {
          EXPECT_TRUE(s.ok());
          EXPECT_EQ(actual_value, expected_value);
        }
      }
    }
  }

  mem->Unref();
}

TEST(MemTableTest, ReferenceCounting) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);

  mem->Ref();    // refs = 1
  mem->Ref();    // refs = 2
  mem->Unref();  // refs = 1
  mem->Unref();  // refs = 0 -> deletes self
  // ASan / LSan will fail if memory is leaked.
}

TEST(MemTableTest, ConcurrentOneWriterFourReaders) {
  InternalKeyComparator cmp;
  auto* mem = new MemTable(cmp);
  mem->Ref();

  std::atomic<bool> stop{false};
  std::atomic<SequenceNumber> latest_seq{0};
  constexpr int kNumKeys = 50;
  constexpr int kNumOperations = 5000;

  // 1 Writer thread
  std::thread writer([&]() {
    for (int i = 1; i <= kNumOperations; ++i) {
      std::string key = "key_" + std::to_string(i % kNumKeys);
      std::string val = "val_" + std::to_string(i);
      mem->Add(static_cast<SequenceNumber>(i), kTypeValue, key, val);
      latest_seq.store(static_cast<SequenceNumber>(i), std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
  });

  // 4 Reader threads
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r]() {
      std::mt19937_64 rng(42 + r);
      std::string val;
      Status s;
      while (!stop.load(std::memory_order_acquire)) {
        SequenceNumber cur = latest_seq.load(std::memory_order_acquire);
        if (cur == 0) continue;
        int k_idx = rng() % kNumKeys;
        std::string key = "key_" + std::to_string(k_idx);
        SequenceNumber read_seq = (rng() % cur) + 1;
        LookupKey lk(key, read_seq);
        mem->Get(lk, &val, &s);
      }
    });
  }

  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }

  mem->Unref();
}

}  // namespace
}  // namespace strata
