#include "db/filename.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace strata {
namespace {

TEST(FileNameTest, ConstructionHelpers) {
  EXPECT_EQ(LogFileName("testdb", 7), "testdb/000007.log");
  EXPECT_EQ(LockFileName("testdb"), "testdb/LOCK");
  EXPECT_EQ(TableFileName("testdb", 8), "testdb/000008.sst");
  EXPECT_EQ(DescriptorFileName("testdb", 5), "testdb/MANIFEST-000005");
  EXPECT_EQ(CurrentFileName("testdb"), "testdb/CURRENT");
  EXPECT_EQ(TempFileName("testdb", 9), "testdb/000009.dbtmp");

  // Numbers larger than 6 digits don't get truncated.
  EXPECT_EQ(LogFileName("testdb", 1234567), "testdb/1234567.log");
}

TEST(FileNameTest, ParseSpecialFiles) {
  uint64_t number = 42;
  FileType type = FileType::kTableFile;

  EXPECT_TRUE(ParseFileName("LOCK", &number, &type));
  EXPECT_EQ(number, 0u);
  EXPECT_EQ(type, FileType::kLockFile);

  EXPECT_TRUE(ParseFileName("CURRENT", &number, &type));
  EXPECT_EQ(number, 0u);
  EXPECT_EQ(type, FileType::kCurrentFile);

  EXPECT_FALSE(ParseFileName("LOCK2", &number, &type));
  EXPECT_FALSE(ParseFileName("CURRENT.bak", &number, &type));
  EXPECT_FALSE(ParseFileName("", &number, &type));
}

TEST(FileNameTest, ParseDescriptorFile) {
  uint64_t number = 0;
  FileType type = FileType::kLogFile;

  EXPECT_TRUE(ParseFileName("MANIFEST-000005", &number, &type));
  EXPECT_EQ(number, 5u);
  EXPECT_EQ(type, FileType::kDescriptorFile);

  EXPECT_TRUE(ParseFileName("MANIFEST-123456", &number, &type));
  EXPECT_EQ(number, 123456u);
  EXPECT_EQ(type, FileType::kDescriptorFile);

  EXPECT_FALSE(ParseFileName("MANIFEST-", &number, &type));
  EXPECT_FALSE(ParseFileName("MANIFEST-abc", &number, &type));
  EXPECT_FALSE(ParseFileName("MANIFEST-12x", &number, &type));
}

TEST(FileNameTest, ParseLogFile) {
  uint64_t number = 0;
  FileType type = FileType::kTableFile;

  EXPECT_TRUE(ParseFileName("000007.log", &number, &type));
  EXPECT_EQ(number, 7u);
  EXPECT_EQ(type, FileType::kLogFile);

  EXPECT_TRUE(ParseFileName("0.log", &number, &type));
  EXPECT_EQ(number, 0u);
  EXPECT_EQ(type, FileType::kLogFile);

  EXPECT_TRUE(ParseFileName("123456.log", &number, &type));
  EXPECT_EQ(number, 123456u);
  EXPECT_EQ(type, FileType::kLogFile);

  // Maximum uint64 value
  const std::string max_u64 = std::to_string(std::numeric_limits<uint64_t>::max()) + ".log";
  EXPECT_TRUE(ParseFileName(max_u64, &number, &type));
  EXPECT_EQ(number, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(type, FileType::kLogFile);

  // Rejections specified in requirements:
  EXPECT_FALSE(ParseFileName("abc.log", &number, &type));
  EXPECT_FALSE(ParseFileName("12x.log", &number, &type));
  EXPECT_FALSE(ParseFileName("18446744073709551616.log", &number, &type));  // overflow
  EXPECT_FALSE(ParseFileName("99999999999999999999999999.log", &number, &type));
  EXPECT_FALSE(ParseFileName(".log", &number, &type));
  EXPECT_FALSE(ParseFileName("-1.log", &number, &type));
  EXPECT_FALSE(ParseFileName("+1.log", &number, &type));
  EXPECT_FALSE(ParseFileName(" 7.log", &number, &type));
  EXPECT_FALSE(ParseFileName("7 .log", &number, &type));
  EXPECT_FALSE(ParseFileName("000007.log.bak", &number, &type));
}

TEST(FileNameTest, ParseTableAndTempFiles) {
  uint64_t number = 0;
  FileType type = FileType::kLogFile;

  EXPECT_TRUE(ParseFileName("000008.sst", &number, &type));
  EXPECT_EQ(number, 8u);
  EXPECT_EQ(type, FileType::kTableFile);

  EXPECT_TRUE(ParseFileName("000008.ldb", &number, &type));
  EXPECT_EQ(number, 8u);
  EXPECT_EQ(type, FileType::kTableFile);

  EXPECT_TRUE(ParseFileName("000009.dbtmp", &number, &type));
  EXPECT_EQ(number, 9u);
  EXPECT_EQ(type, FileType::kTempFile);

  EXPECT_FALSE(ParseFileName("abc.sst", &number, &type));
  EXPECT_FALSE(ParseFileName("abc.dbtmp", &number, &type));
  EXPECT_FALSE(ParseFileName("000008.unknown", &number, &type));
}

TEST(FileNameTest, NullOutputPointersAreSafe) {
  EXPECT_TRUE(ParseFileName("000007.log", nullptr, nullptr));
  EXPECT_TRUE(ParseFileName("LOCK", nullptr, nullptr));
  EXPECT_FALSE(ParseFileName("bad.log", nullptr, nullptr));
}

}  // namespace
}  // namespace strata
