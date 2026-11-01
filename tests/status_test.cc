#include "strata/status.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace strata {
namespace {

// Only the factories may build a Status, so "OK with a message" and
// out-of-range codes cannot be expressed.
static_assert(!std::is_constructible_v<Status, Status::Code, std::string_view, std::string_view>);
static_assert(!std::is_constructible_v<Status, Status::Code, std::string_view>);

// message() hands out a reference to the Status's own string.
static_assert(
    std::is_same_v<decltype(std::declval<const Status&>().message()), const std::string&>);

TEST(StatusTest, DefaultIsOK) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotSupported());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, FactoryOK) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, FactoryNotFound) {
  Status s = Status::NotFound("missing key");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotSupported());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_EQ(s.code(), Status::Code::kNotFound);
  EXPECT_EQ(s.message(), "missing key");
  EXPECT_EQ(s.ToString(), "NotFound: missing key");
}

TEST(StatusTest, FactoryCorruption) {
  Status s = Status::Corruption("bad checksum");
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotSupported());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_EQ(s.code(), Status::Code::kCorruption);
  EXPECT_EQ(s.message(), "bad checksum");
  EXPECT_EQ(s.ToString(), "Corruption: bad checksum");
}

TEST(StatusTest, FactoryNotSupported) {
  Status s = Status::NotSupported("lz4 compression");
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_TRUE(s.IsNotSupported());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_EQ(s.code(), Status::Code::kNotSupported);
  EXPECT_EQ(s.message(), "lz4 compression");
  EXPECT_EQ(s.ToString(), "Not supported: lz4 compression");
}

TEST(StatusTest, FactoryInvalidArgument) {
  Status s = Status::InvalidArgument("invalid key length");
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotSupported());
  EXPECT_TRUE(s.IsInvalidArgument());
  EXPECT_FALSE(s.IsIOError());
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(s.message(), "invalid key length");
  EXPECT_EQ(s.ToString(), "Invalid argument: invalid key length");
}

TEST(StatusTest, FactoryIOError) {
  Status s = Status::IOError("disk failure");
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotSupported());
  EXPECT_FALSE(s.IsInvalidArgument());
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(s.code(), Status::Code::kIOError);
  EXPECT_EQ(s.message(), "disk failure");
  EXPECT_EQ(s.ToString(), "IO error: disk failure");
}

TEST(StatusTest, TwoMessageParts) {
  Status s = Status::IOError("000007.log", "No space left");
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(s.message(), "000007.log: No space left");
  EXPECT_EQ(s.ToString(), "IO error: 000007.log: No space left");

  Status nf = Status::NotFound("user_table", "key 42");
  EXPECT_TRUE(nf.IsNotFound());
  EXPECT_EQ(nf.message(), "user_table: key 42");
  EXPECT_EQ(nf.ToString(), "NotFound: user_table: key 42");
}

TEST(StatusTest, CopyOperations) {
  Status orig = Status::NotFound("key1", "not present");
  Status copy(orig);  // NOLINT
  EXPECT_EQ(copy.code(), orig.code());
  EXPECT_EQ(copy.message(), orig.message());
  EXPECT_EQ(copy.ToString(), orig.ToString());

  Status assigned;
  assigned = orig;
  EXPECT_EQ(assigned.code(), orig.code());
  EXPECT_EQ(assigned.message(), orig.message());
  EXPECT_EQ(assigned.ToString(), orig.ToString());
}

TEST(StatusTest, MoveOperations) {
  Status orig = Status::Corruption("file.sst", "truncated block");
  std::string expected_msg = "file.sst: truncated block";
  std::string expected_str = "Corruption: file.sst: truncated block";

  Status moved(std::move(orig));
  EXPECT_EQ(moved.code(), Status::Code::kCorruption);
  EXPECT_EQ(moved.message(), expected_msg);
  EXPECT_EQ(moved.ToString(), expected_str);

  // Moved-from is safe to destroy and safe to assign to
  orig = Status::OK();
  EXPECT_TRUE(orig.ok());

  Status moved_assign;
  moved_assign = std::move(moved);
  EXPECT_EQ(moved_assign.code(), Status::Code::kCorruption);
  EXPECT_EQ(moved_assign.message(), expected_msg);
  EXPECT_EQ(moved_assign.ToString(), expected_str);
}

}  // namespace
}  // namespace strata
