#include "db/table_cache.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "db/builder.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "memtable/memtable.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {
namespace {

class TableCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    static std::atomic<uint64_t> test_counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_table_cache_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(env_->NowMicros()) + "_" + std::to_string(test_counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  uint64_t BuildTestTable(uint64_t file_number, const std::string& key, const std::string& val) {
    auto* mem = new MemTable(icmp_);
    mem->Ref();
    mem->Add(1, kTypeValue, key, val);
    FileMetaData meta;
    meta.number = file_number;
    std::unique_ptr<Iterator> iter = mem->NewIterator();
    Status s = BuildTable(test_dir_, env_, options_, icmp_, iter.get(), &meta);
    EXPECT_TRUE(s.ok()) << s.ToString();
    mem->Unref();
    return meta.file_size;
  }

  Env* env_ = nullptr;
  std::string test_dir_;
  Options options_;
  InternalKeyComparator icmp_;
};

TEST_F(TableCacheTest, OpenedOnceAndReused) {
  const uint64_t file_num = 10;
  uint64_t file_size = BuildTestTable(file_num, "hello", "world");
  ASSERT_GT(file_size, 0u);

  TableCache cache(test_dir_, options_, &icmp_);

  std::shared_ptr<Table> t1;
  Status s = cache.FindTable(file_num, file_size, &t1);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_NE(t1, nullptr);

  std::shared_ptr<Table> t2;
  s = cache.FindTable(file_num, file_size, &t2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_NE(t2, nullptr);

  // Exact same table instance reused
  EXPECT_EQ(t1.get(), t2.get());

  bool found = false;
  std::string k, v;
  s = t1->InternalGet(ReadOptions(), LookupKey("hello", 10).internal_key(), &found, &k, &v);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(v, "world");
}

TEST_F(TableCacheTest, EvictWorksAndSharedPtrStaysAlive) {
  const uint64_t file_num = 20;
  uint64_t file_size = BuildTestTable(file_num, "foo", "bar");
  ASSERT_GT(file_size, 0u);

  TableCache cache(test_dir_, options_, &icmp_);

  std::shared_ptr<Table> t1;
  Status s = cache.FindTable(file_num, file_size, &t1);
  ASSERT_TRUE(s.ok()) << s.ToString();

  // Evict the table
  cache.Erase(file_num);

  // Next lookup re-opens the file
  std::shared_ptr<Table> t2;
  s = cache.FindTable(file_num, file_size, &t2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_NE(t1.get(), t2.get());

  // Both the old instance and new instance can be read safely
  bool found = false;
  std::string k, v;
  s = t1->InternalGet(ReadOptions(), LookupKey("foo", 10).internal_key(), &found, &k, &v);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(v, "bar");

  found = false;
  s = t2->InternalGet(ReadOptions(), LookupKey("foo", 10).internal_key(), &found, &k, &v);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(v, "bar");
}

TEST_F(TableCacheTest, MissingFileReturnsError) {
  TableCache cache(test_dir_, options_, &icmp_);

  std::shared_ptr<Table> t;
  Status s = cache.FindTable(9999, 100, &t);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(t, nullptr);
}

TEST_F(TableCacheTest, NullTablePointerReturnsInvalidArgument) {
  TableCache cache(test_dir_, options_, &icmp_);
  Status s = cache.FindTable(10, 100, nullptr);
  EXPECT_TRUE(s.IsInvalidArgument());
}

TEST_F(TableCacheTest, EraseWorksDirectly) {
  const uint64_t file_num = 30;
  uint64_t file_size = BuildTestTable(file_num, "key30", "val30");
  ASSERT_GT(file_size, 0u);

  TableCache cache(test_dir_, options_, &icmp_);

  std::shared_ptr<Table> t1;
  Status s = cache.FindTable(file_num, file_size, &t1);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(cache.TotalCharge(), 1u);

  // Erase the table
  cache.Erase(file_num);
  EXPECT_EQ(cache.TotalCharge(), 0u);

  // Re-open produces a new table instance
  std::shared_ptr<Table> t2;
  s = cache.FindTable(file_num, file_size, &t2);
  ASSERT_TRUE(s.ok());
  EXPECT_NE(t1.get(), t2.get());
  EXPECT_EQ(cache.TotalCharge(), 1u);
}

TEST_F(TableCacheTest, BoundedCapacityEvictsOldest) {
  // max_open_files = 14 => capacity = 14 - 10 = 4.
  // With 16 shards in ShardedLRUCache, capacity 4 assigns 1 per shard to 4 shards, 0 to 12 shards.
  // Let's test with options_.max_open_files = 26 => capacity = 16 (1 per shard).
  Options opts = options_;
  opts.max_open_files = 26;  // capacity = 16
  TableCache cache(test_dir_, opts, &icmp_);

  // Create 20 tables
  std::vector<uint64_t> file_nums;
  std::vector<uint64_t> file_sizes;
  for (uint64_t i = 1; i <= 20; ++i) {
    uint64_t sz = BuildTestTable(i + 100, "k" + std::to_string(i), "v" + std::to_string(i));
    file_nums.push_back(i + 100);
    file_sizes.push_back(sz);
  }

  // Open all 20 tables
  for (size_t i = 0; i < file_nums.size(); ++i) {
    std::shared_ptr<Table> t;
    Status s = cache.FindTable(file_nums[i], file_sizes[i], &t);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  // Total charge in cache must be bounded by capacity (16)
  EXPECT_LE(cache.TotalCharge(), 16u);
}

TEST_F(TableCacheTest, IteratorKeepsEvictedTableValid) {
  Options opts = options_;
  opts.max_open_files = 26;  // capacity 16
  TableCache cache(test_dir_, opts, &icmp_);

  uint64_t f1 = 500;
  uint64_t sz1 = BuildTestTable(f1, "target_key", "target_val");

  std::unique_ptr<Iterator> iter = cache.NewIterator(ReadOptions(), f1, sz1);
  ASSERT_TRUE(iter->status().ok());

  // Now erase/evict f1 from cache
  cache.Erase(f1);
  EXPECT_EQ(cache.TotalCharge(), 0u);

  // Iterator is still completely functional because TableIteratorWrapper pinned the Table
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(ExtractUserKey(iter->key()), "target_key");
  EXPECT_EQ(iter->value(), "target_val");
}

TEST_F(TableCacheTest, MultiplePinnedIteratorsExceedCapacity) {
  Options opts = options_;
  opts.max_open_files = 26;  // capacity = 16
  TableCache cache(test_dir_, opts, &icmp_);

  constexpr int kNumTables = 25;
  std::vector<uint64_t> file_nums;
  std::vector<uint64_t> file_sizes;
  for (int i = 0; i < kNumTables; ++i) {
    uint64_t num = 700 + static_cast<uint64_t>(i);
    uint64_t sz = BuildTestTable(num, "k" + std::to_string(i), "v" + std::to_string(i));
    file_nums.push_back(num);
    file_sizes.push_back(sz);
  }

  // Open 25 simultaneous iterators (exceeding table cache capacity 16)
  std::vector<std::unique_ptr<Iterator>> iters;
  for (int i = 0; i < kNumTables; ++i) {
    auto iter = cache.NewIterator(ReadOptions(), file_nums[i], file_sizes[i]);
    ASSERT_TRUE(iter->status().ok());
    iters.push_back(std::move(iter));
  }

  // All 25 iterators remain fully functional
  for (int i = 0; i < kNumTables; ++i) {
    iters[i]->SeekToFirst();
    ASSERT_TRUE(iters[i]->Valid());
    EXPECT_EQ(ExtractUserKey(iters[i]->key()), "k" + std::to_string(i));
    EXPECT_EQ(iters[i]->value(), "v" + std::to_string(i));
  }

  // Cache usage is bounded by 16
  EXPECT_LE(cache.TotalCharge(), 16u);

  // Release all pinned iterators
  iters.clear();
  EXPECT_LE(cache.TotalCharge(), 16u);
}

}  // namespace
}  // namespace strata
