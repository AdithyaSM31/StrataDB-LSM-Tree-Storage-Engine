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
  s = t1->InternalGet(LookupKey("hello", 10).internal_key(), &found, &k, &v);
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
  cache.Evict(file_num);

  // Next lookup re-opens the file
  std::shared_ptr<Table> t2;
  s = cache.FindTable(file_num, file_size, &t2);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_NE(t1.get(), t2.get());

  // Both the old instance and new instance can be read safely
  bool found = false;
  std::string k, v;
  s = t1->InternalGet(LookupKey("foo", 10).internal_key(), &found, &k, &v);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(v, "bar");

  found = false;
  s = t2->InternalGet(LookupKey("foo", 10).internal_key(), &found, &k, &v);
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

}  // namespace
}  // namespace strata
