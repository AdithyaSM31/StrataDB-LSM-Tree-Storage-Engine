#include "db/version_edit.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "util/coding.h"

namespace strata {
namespace {

TEST(VersionEditTest, EmptyEdit) {
  VersionEdit edit;
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_TRUE(encoded.empty());

  VersionEdit decoded;
  Status s = decoded.DecodeFrom(encoded);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_FALSE(decoded.has_comparator());
  EXPECT_FALSE(decoded.has_log_number());
  EXPECT_FALSE(decoded.has_next_file_number());
  EXPECT_FALSE(decoded.has_last_sequence());
  EXPECT_TRUE(decoded.deleted_files().empty());
  EXPECT_TRUE(decoded.new_files().empty());
}

TEST(VersionEditTest, RoundTripAllFields) {
  VersionEdit edit;
  edit.SetComparatorName("strata.InternalKeyComparator");
  edit.SetLogNumber(100);
  edit.SetNextFile(200);
  edit.SetLastSequence(300);

  std::string smallest1 = MakeInternalKey("key_a", 10, kTypeValue);
  std::string largest1 = MakeInternalKey("key_z", 15, kTypeValue);
  edit.AddFile(0, 10, 1024, smallest1, largest1);

  std::string smallest2 = MakeInternalKey("key_b", 20, kTypeValue);
  std::string largest2 = MakeInternalKey("key_m", 25, kTypeDeletion);
  edit.AddFile(1, 11, 2048, smallest2, largest2);

  edit.RemoveFile(0, 5);
  edit.RemoveFile(2, 8);

  std::string encoded1;
  edit.EncodeTo(&encoded1);
  EXPECT_FALSE(encoded1.empty());

  VersionEdit decoded1;
  Status s = decoded1.DecodeFrom(encoded1);
  ASSERT_TRUE(s.ok()) << s.ToString();

  EXPECT_TRUE(decoded1.has_comparator());
  EXPECT_EQ(decoded1.comparator(), "strata.InternalKeyComparator");
  EXPECT_TRUE(decoded1.has_log_number());
  EXPECT_EQ(decoded1.log_number(), 100u);
  EXPECT_TRUE(decoded1.has_next_file_number());
  EXPECT_EQ(decoded1.next_file_number(), 200u);
  EXPECT_TRUE(decoded1.has_last_sequence());
  EXPECT_EQ(decoded1.last_sequence(), 300u);

  ASSERT_EQ(decoded1.new_files().size(), 2u);
  EXPECT_EQ(decoded1.new_files()[0].first, 0);
  EXPECT_EQ(decoded1.new_files()[0].second.number, 10u);
  EXPECT_EQ(decoded1.new_files()[0].second.file_size, 1024u);
  EXPECT_EQ(decoded1.new_files()[0].second.smallest, smallest1);
  EXPECT_EQ(decoded1.new_files()[0].second.largest, largest1);

  EXPECT_EQ(decoded1.new_files()[1].first, 1);
  EXPECT_EQ(decoded1.new_files()[1].second.number, 11u);
  EXPECT_EQ(decoded1.new_files()[1].second.file_size, 2048u);
  EXPECT_EQ(decoded1.new_files()[1].second.smallest, smallest2);
  EXPECT_EQ(decoded1.new_files()[1].second.largest, largest2);

  ASSERT_EQ(decoded1.deleted_files().size(), 2u);
  EXPECT_EQ(decoded1.deleted_files()[0].first, 0);
  EXPECT_EQ(decoded1.deleted_files()[0].second, 5u);
  EXPECT_EQ(decoded1.deleted_files()[1].first, 2);
  EXPECT_EQ(decoded1.deleted_files()[1].second, 8u);

  std::string encoded2;
  decoded1.EncodeTo(&encoded2);
  EXPECT_EQ(encoded1, encoded2);
}

TEST(VersionEditTest, UnknownTagReturnsCorruption) {
  std::string buf;
  PutVarint32(&buf, 99);  // Tag 99 is invalid
  PutVarint32(&buf, 0);

  VersionEdit edit;
  Status s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  // Tag 0 is also invalid
  buf.clear();
  PutVarint32(&buf, 0);
  s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(VersionEditTest, BadLevelReturnsCorruption) {
  // Bad level in deleted file (Tag 5)
  std::string buf;
  PutVarint32(&buf, 5);
  PutVarint32(&buf, kNumLevels);  // Level kNumLevels is out of range
  PutVarint64(&buf, 10);

  VersionEdit edit;
  Status s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  // Bad level in new file (Tag 6)
  buf.clear();
  PutVarint32(&buf, 6);
  PutVarint32(&buf, kNumLevels + 1);  // Out of range
  PutVarint64(&buf, 10);
  PutVarint64(&buf, 100);
  std::string key = MakeInternalKey("k", 1, kTypeValue);
  PutLengthPrefixed(&buf, key);
  PutLengthPrefixed(&buf, key);

  s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  // Negative level encoded as uint32_t
  buf.clear();
  PutVarint32(&buf, 5);
  PutVarint32(&buf, static_cast<uint32_t>(-1));
  PutVarint64(&buf, 10);
  s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(VersionEditTest, BadInternalKeyReturnsCorruption) {
  // New file with smallest key shorter than 8 bytes
  std::string buf;
  PutVarint32(&buf, 6);
  PutVarint32(&buf, 0);
  PutVarint64(&buf, 10);
  PutVarint64(&buf, 100);
  PutLengthPrefixed(&buf, "short");  // Invalid internal key (< 8 bytes)
  std::string valid_key = MakeInternalKey("k", 1, kTypeValue);
  PutLengthPrefixed(&buf, valid_key);

  VersionEdit edit;
  Status s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();

  // New file with largest key having invalid ValueType
  buf.clear();
  PutVarint32(&buf, 6);
  PutVarint32(&buf, 0);
  PutVarint64(&buf, 10);
  PutVarint64(&buf, 100);
  PutLengthPrefixed(&buf, valid_key);
  std::string bad_key = "user_key";
  bad_key.push_back('\x05');  // Invalid ValueType > kTypeValue at byte 0 of trailer
  bad_key.append(7, '\0');
  PutLengthPrefixed(&buf, bad_key);

  s = edit.DecodeFrom(buf);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(VersionEditTest, TruncationAtEveryByte) {
  VersionEdit edit;
  edit.SetComparatorName("strata.InternalKeyComparator");
  edit.SetLogNumber(42);
  edit.SetNextFile(43);
  edit.SetLastSequence(44);
  std::string k1 = MakeInternalKey("k1", 1, kTypeValue);
  std::string k2 = MakeInternalKey("k2", 2, kTypeValue);
  edit.AddFile(0, 10, 500, k1, k2);
  edit.RemoveFile(1, 20);

  std::string encoded;
  edit.EncodeTo(&encoded);

  // For every truncation from 0 bytes to full size:
  // It either decodes cleanly as a partial edit (when cut cleanly between fields),
  // or reports Corruption. It must NEVER crash.
  int ok_count = 0;
  int corruption_count = 0;
  for (size_t i = 0; i <= encoded.size(); ++i) {
    std::string_view slice(encoded.data(), i);
    VersionEdit decoded;
    Status s = decoded.DecodeFrom(slice);
    if (s.ok()) {
      ok_count++;
    } else {
      EXPECT_TRUE(s.IsCorruption()) << "At length " << i << ": " << s.ToString();
      corruption_count++;
    }
  }

  // 0-length is empty and ok; full-length is complete and ok.
  // There are field boundaries where truncation decodes successfully as a smaller edit.
  EXPECT_GE(ok_count, 2);
  EXPECT_GT(corruption_count, 0);
}

TEST(VersionEditTest, LastSequenceAbove56BitsReturnsCorruption) {
  std::string encoded;
  PutVarint32(&encoded, 4);  // the last-sequence tag
  PutVarint64(&encoded, kMaxSequenceNumber + 1);
  VersionEdit edit;
  EXPECT_TRUE(edit.DecodeFrom(encoded).IsCorruption());

  encoded.clear();
  PutVarint32(&encoded, 4);
  PutVarint64(&encoded, kMaxSequenceNumber);
  ASSERT_TRUE(edit.DecodeFrom(encoded).ok());
  EXPECT_EQ(edit.last_sequence(), kMaxSequenceNumber);
}

}  // namespace
}  // namespace strata
