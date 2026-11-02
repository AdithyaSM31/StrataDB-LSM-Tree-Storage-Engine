#include "table/format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "strata/env.h"
#include "strata/status.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {
namespace {

// A RandomAccessFile over an in-memory string.
class StringSource : public RandomAccessFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}
  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (offset >= contents_.size()) {
      *result = {};
      return Status::OK();
    }
    const size_t len = std::min(n, static_cast<size_t>(contents_.size() - offset));
    std::memcpy(scratch, contents_.data() + offset, len);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

 private:
  std::string contents_;
};

// Block contents followed by a valid trailer, as TableBuilder writes them.
std::string WithTrailer(std::string_view contents, uint8_t type = kNoCompression) {
  std::string out(contents);
  out.push_back(static_cast<char>(type));
  const uint32_t crc = crc32c::Value(out.data(), out.size());
  PutFixed32(&out, crc32c::Mask(crc));
  return out;
}

BlockHandle Handle(uint64_t offset, uint64_t size) {
  BlockHandle h;
  h.set_offset(offset);
  h.set_size(size);
  return h;
}

TEST(FormatTest, BlockHandleRoundTrip) {
  for (const uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128},
                           uint64_t{1} << 40, ~uint64_t{0} - 1}) {
    std::string encoded;
    Handle(v, v / 2).EncodeTo(&encoded);
    EXPECT_LE(encoded.size(), BlockHandle::kMaxEncodedLength);
    std::string_view input = encoded;
    BlockHandle decoded;
    ASSERT_TRUE(decoded.DecodeFrom(&input).ok());
    EXPECT_EQ(decoded.offset(), v);
    EXPECT_EQ(decoded.size(), v / 2);
    EXPECT_TRUE(input.empty());
  }
}

TEST(FormatTest, BlockHandleRejectsGarbage) {
  for (const std::string& bad :
       {std::string(), std::string("\x80", 1), std::string("\x05", 1), std::string(11, '\x80')}) {
    std::string_view input = bad;
    BlockHandle h;
    EXPECT_TRUE(h.DecodeFrom(&input).IsCorruption());
  }
}

TEST(FormatTest, FooterRoundTripIs48Bytes) {
  Footer footer;
  footer.set_metaindex_handle(Handle(1000, 50));
  footer.set_index_handle(Handle(1055, 300));
  std::string encoded;
  footer.EncodeTo(&encoded);
  ASSERT_EQ(encoded.size(), 48U);
  EXPECT_EQ(DecodeFixed64(encoded.data() + 40), kTableMagicNumber);

  Footer decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_EQ(decoded.metaindex_handle().offset(), 1000U);
  EXPECT_EQ(decoded.metaindex_handle().size(), 50U);
  EXPECT_EQ(decoded.index_handle().offset(), 1055U);
  EXPECT_EQ(decoded.index_handle().size(), 300U);
}

TEST(FormatTest, FooterRejectsBadMagicAndLength) {
  Footer footer;
  footer.set_metaindex_handle(Handle(0, 1));
  footer.set_index_handle(Handle(1, 1));
  std::string encoded;
  footer.EncodeTo(&encoded);

  std::string bad_magic = encoded;
  bad_magic[45] = static_cast<char>(bad_magic[45] ^ 0x01);
  Footer decoded;
  Status s = decoded.DecodeFrom(bad_magic);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_NE(s.ToString().find("magic"), std::string::npos) << s.ToString();

  EXPECT_TRUE(decoded.DecodeFrom(encoded.substr(0, 47)).IsCorruption());
}

TEST(FormatTest, ReadBlockReturnsContents) {
  const std::string file = "xx" + WithTrailer("hello block") + "yy";
  StringSource source(file);
  std::string contents;
  ASSERT_TRUE(ReadBlock(&source, file.size(), Handle(2, 11), &contents).ok());
  EXPECT_EQ(contents, "hello block");
}

TEST(FormatTest, ReadBlockDetectsChecksumMismatch) {
  std::string file = WithTrailer("hello block");
  file[3] = static_cast<char>(file[3] ^ 0x20);
  StringSource source(file);
  std::string contents;
  Status s = ReadBlock(&source, file.size(), Handle(0, 11), &contents);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_TRUE(contents.empty());
}

TEST(FormatTest, ReadBlockRejectsUnknownCompression) {
  const std::string file = WithTrailer("hello block", /*type=*/7);  // valid CRC, bad type
  StringSource source(file);
  std::string contents;
  EXPECT_TRUE(ReadBlock(&source, file.size(), Handle(0, 11), &contents).IsCorruption());
}

TEST(FormatTest, ReadBlockRejectsHandlesOutsideTheFile) {
  const std::string file = WithTrailer("hello block");  // 16 bytes
  StringSource source(file);
  std::string contents;
  const uint64_t kMax = ~uint64_t{0};
  // Each of these must be caught before any read or allocation, including the
  // ones where offset + size or size + trailer would overflow 64 bits.
  for (const auto& [offset, size] : {std::pair<uint64_t, uint64_t>{0, 12},  // trailer cut off
                                     {1, 11},
                                     {16, 0},
                                     {kMax, 1},
                                     {1, kMax},
                                     {kMax - 3, 4},
                                     {0, kMax - 4}}) {
    EXPECT_TRUE(ReadBlock(&source, file.size(), Handle(offset, size), &contents).IsCorruption())
        << "offset=" << offset << " size=" << size;
  }
}

TEST(FormatTest, ReadBlockDetectsAFileShorterThanClaimed) {
  // The caller's file_size says 100 bytes, but the file only has 16.
  const std::string file = WithTrailer("hello block");
  StringSource source(file);
  std::string contents;
  EXPECT_TRUE(ReadBlock(&source, 100, Handle(50, 20), &contents).IsCorruption());
}

}  // namespace
}  // namespace strata
