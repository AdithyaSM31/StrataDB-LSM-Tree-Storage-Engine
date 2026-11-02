#include "db/dbformat.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace strata {
namespace {

TEST(DbFormatTest, RoundTripEmptyUserKey) {
  ParsedInternalKey original{"", 12345, kTypeValue};
  std::string encoded;
  AppendInternalKey(&encoded, original);
  EXPECT_EQ(encoded.size(), 8u);

  ParsedInternalKey parsed;
  EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
  EXPECT_EQ(parsed.user_key, "");
  EXPECT_EQ(parsed.sequence, 12345u);
  EXPECT_EQ(parsed.type, kTypeValue);
  EXPECT_EQ(ExtractUserKey(encoded), "");
}

TEST(DbFormatTest, RoundTripBinaryKeys) {
  std::string binary_key(
      "foo\0bar\xff"
      "baz\x00\x01",
      13);
  for (SequenceNumber seq :
       {SequenceNumber{0}, SequenceNumber{1}, SequenceNumber{42}, kMaxSequenceNumber}) {
    for (ValueType type : {kTypeValue, kTypeDeletion}) {
      ParsedInternalKey original{binary_key, seq, type};
      std::string encoded;
      AppendInternalKey(&encoded, original);
      EXPECT_EQ(encoded.size(), binary_key.size() + 8);

      ParsedInternalKey parsed;
      EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
      EXPECT_EQ(parsed.user_key, binary_key);
      EXPECT_EQ(parsed.sequence, seq);
      EXPECT_EQ(parsed.type, type);
      EXPECT_EQ(ExtractUserKey(encoded), binary_key);
    }
  }
}

TEST(DbFormatTest, RoundTripBoundarySequencesAndTypes) {
  const std::string ukey = "test_user_key";

  // Sequence 0, kTypeValue
  {
    std::string encoded = MakeInternalKey(ukey, 0, kTypeValue);
    ParsedInternalKey parsed;
    EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
    EXPECT_EQ(parsed.user_key, ukey);
    EXPECT_EQ(parsed.sequence, 0u);
    EXPECT_EQ(parsed.type, kTypeValue);
  }

  // Sequence 0, kTypeDeletion
  {
    std::string encoded = MakeInternalKey(ukey, 0, kTypeDeletion);
    ParsedInternalKey parsed;
    EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
    EXPECT_EQ(parsed.user_key, ukey);
    EXPECT_EQ(parsed.sequence, 0u);
    EXPECT_EQ(parsed.type, kTypeDeletion);
  }

  // kMaxSequenceNumber, kTypeValue
  {
    std::string encoded = MakeInternalKey(ukey, kMaxSequenceNumber, kTypeValue);
    ParsedInternalKey parsed;
    EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
    EXPECT_EQ(parsed.user_key, ukey);
    EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
    EXPECT_EQ(parsed.type, kTypeValue);
  }

  // kMaxSequenceNumber, kTypeDeletion
  {
    std::string encoded = MakeInternalKey(ukey, kMaxSequenceNumber, kTypeDeletion);
    ParsedInternalKey parsed;
    EXPECT_TRUE(ParseInternalKey(encoded, &parsed));
    EXPECT_EQ(parsed.user_key, ukey);
    EXPECT_EQ(parsed.sequence, kMaxSequenceNumber);
    EXPECT_EQ(parsed.type, kTypeDeletion);
  }
}

TEST(DbFormatTest, RejectShortInput) {
  ParsedInternalKey parsed;
  for (size_t len = 0; len < 8; ++len) {
    std::string short_key(len, 'x');
    EXPECT_FALSE(ParseInternalKey(short_key, &parsed)) << "Length: " << len;
  }
}

TEST(DbFormatTest, RejectInvalidTypes) {
  ParsedInternalKey parsed;
  std::string key = "valid_key";
  for (uint32_t bad_type = 2; bad_type <= 255; ++bad_type) {
    std::string encoded = key;
    uint64_t trailer = (100ULL << 8) | static_cast<uint8_t>(bad_type);
    PutFixed64(&encoded, trailer);

    EXPECT_FALSE(ParseInternalKey(encoded, &parsed))
        << "Failed to reject invalid type: " << bad_type;
  }
}

TEST(DbFormatTest, ComparatorOrderingRule) {
  InternalKeyComparator cmp;

  // Expected ascending sequence:
  // ("a", 100) < ("a", 99) < ("a", 1) < ("b", 500)
  // Higher sequence number must sort FIRST within identical user keys!
  std::string a_100 = MakeInternalKey("a", 100, kTypeValue);
  std::string a_99 = MakeInternalKey("a", 99, kTypeValue);
  std::string a_1 = MakeInternalKey("a", 1, kTypeValue);
  std::string b_500 = MakeInternalKey("b", 500, kTypeValue);

  EXPECT_LT(cmp.Compare(a_100, a_99), 0);
  EXPECT_LT(cmp.Compare(a_99, a_1), 0);
  EXPECT_LT(cmp.Compare(a_1, b_500), 0);
  EXPECT_LT(cmp.Compare(a_100, b_500), 0);

  EXPECT_GT(cmp.Compare(a_99, a_100), 0);
  EXPECT_GT(cmp.Compare(a_1, a_99), 0);
  EXPECT_GT(cmp.Compare(b_500, a_1), 0);

  // Prefix case: "a" before "ab"
  std::string a_key = MakeInternalKey("a", 100, kTypeValue);
  std::string ab_key = MakeInternalKey("ab", 100, kTypeValue);
  EXPECT_LT(cmp.Compare(a_key, ab_key), 0);
  EXPECT_GT(cmp.Compare(ab_key, a_key), 0);

  // Unsigned-bytes case: "\xff" after "a"
  std::string ff_key = MakeInternalKey("\xff", 100, kTypeValue);
  EXPECT_LT(cmp.Compare(a_key, ff_key), 0);
  EXPECT_GT(cmp.Compare(ff_key, a_key), 0);

  // Same user key, same sequence number: kTypeValue < kTypeDeletion
  // (kTypeValue has trailer ending in 1, kTypeDeletion in 0; descending -> higher comes first)
  std::string a_val = MakeInternalKey("a", 100, kTypeValue);
  std::string a_del = MakeInternalKey("a", 100, kTypeDeletion);
  EXPECT_LT(cmp.Compare(a_val, a_del), 0);
  EXPECT_GT(cmp.Compare(a_del, a_val), 0);

  // Reflexive
  EXPECT_EQ(cmp.Compare(a_100, a_100), 0);
  EXPECT_EQ(cmp.Compare(b_500, b_500), 0);
  EXPECT_EQ(cmp.Compare(ff_key, ff_key), 0);

  // Functor operator()
  EXPECT_LT(cmp(a_100, a_99), 0);
  EXPECT_GT(cmp(a_99, a_100), 0);
  EXPECT_EQ(cmp(a_100, a_100), 0);
}

TEST(DbFormatTest, ComparatorParsedInternalKeyOverload) {
  InternalKeyComparator cmp;
  ParsedInternalKey k1{"apple", 10, kTypeValue};
  ParsedInternalKey k2{"apple", 5, kTypeValue};
  ParsedInternalKey k3{"apple", 5, kTypeDeletion};
  ParsedInternalKey k4{"banana", 1, kTypeValue};

  EXPECT_LT(cmp.Compare(k1, k2), 0);
  EXPECT_LT(cmp.Compare(k2, k3), 0);
  EXPECT_LT(cmp.Compare(k3, k4), 0);
  EXPECT_EQ(cmp.Compare(k1, k1), 0);
  EXPECT_GT(cmp.Compare(k2, k1), 0);
}

TEST(DbFormatTest, LookupKeyShort) {
  std::string ukey = "apple";
  SequenceNumber seq = 42;
  LookupKey lkey(ukey, seq);

  EXPECT_EQ(lkey.user_key(), ukey);

  std::string expected_internal = MakeInternalKey(ukey, seq, kValueTypeForSeek);
  EXPECT_EQ(lkey.internal_key(), expected_internal);

  std::string expected_memtable;
  PutLengthPrefixed(&expected_memtable, expected_internal);
  EXPECT_EQ(lkey.memtable_key(), expected_memtable);
}

TEST(DbFormatTest, LookupKeyLong) {
  // 500 bytes exceeds the 200-byte inline space_
  std::string ukey(500, 'k');
  SequenceNumber seq = 1000500;
  LookupKey lkey(ukey, seq);

  EXPECT_EQ(lkey.user_key(), ukey);

  std::string expected_internal = MakeInternalKey(ukey, seq, kValueTypeForSeek);
  EXPECT_EQ(lkey.internal_key(), expected_internal);

  std::string expected_memtable;
  PutLengthPrefixed(&expected_memtable, expected_internal);
  EXPECT_EQ(lkey.memtable_key(), expected_memtable);
}

TEST(DbFormatTest, LookupKeyMoveSemantics) {
  // Short key move
  {
    LookupKey k1("short_key", 10);
    std::string expected_int(k1.internal_key());
    std::string expected_mem(k1.memtable_key());
    std::string expected_usr(k1.user_key());

    LookupKey k2 = std::move(k1);
    EXPECT_EQ(k2.internal_key(), expected_int);
    EXPECT_EQ(k2.memtable_key(), expected_mem);
    EXPECT_EQ(k2.user_key(), expected_usr);

    LookupKey k3("another", 20);
    k3 = std::move(k2);
    EXPECT_EQ(k3.internal_key(), expected_int);
    EXPECT_EQ(k3.memtable_key(), expected_mem);
    EXPECT_EQ(k3.user_key(), expected_usr);
  }

  // Long key move
  {
    std::string long_key(400, 'z');
    LookupKey k1(long_key, 20);
    std::string expected_int(k1.internal_key());
    std::string expected_mem(k1.memtable_key());
    std::string expected_usr(k1.user_key());

    LookupKey k2 = std::move(k1);
    EXPECT_EQ(k2.internal_key(), expected_int);
    EXPECT_EQ(k2.memtable_key(), expected_mem);
    EXPECT_EQ(k2.user_key(), expected_usr);

    LookupKey k3("short", 1);
    k3 = std::move(k2);
    EXPECT_EQ(k3.internal_key(), expected_int);
    EXPECT_EQ(k3.memtable_key(), expected_mem);
    EXPECT_EQ(k3.user_key(), expected_usr);
  }
}

TEST(DbFormatTest, LookupKeyBoundaryBufferSizes) {
  // Test key sizes right around the 200-byte inline buffer boundary.
  // internal_key_size = usize + 8.
  // For internal_key_size in [128, 16383], VarintLength is 2.
  // needed = 2 + usize + 8 = usize + 10.
  // needed == 199 => usize = 189
  // needed == 200 => usize = 190 (exact fit in space_[200])
  // needed == 201 => usize = 191 (spills to heap)
  for (size_t usize : {189u, 190u, 191u}) {
    std::string ukey(usize, 'a');
    SequenceNumber seq = 42;
    LookupKey lkey(ukey, seq);

    EXPECT_EQ(lkey.user_key(), ukey);
    std::string expected_internal = MakeInternalKey(ukey, seq, kValueTypeForSeek);
    EXPECT_EQ(lkey.internal_key(), expected_internal);

    std::string expected_memtable;
    PutLengthPrefixed(&expected_memtable, expected_internal);
    EXPECT_EQ(lkey.memtable_key(), expected_memtable);
  }
}

TEST(DbFormatTest, LookupKeyMovedFromAccessorsSafe) {
  // Verify moved-from accessors do not underflow size_t or access invalid memory.
  // Heap case:
  {
    std::string long_key(300, 'h');
    LookupKey k1(long_key, 100);
    LookupKey k2 = std::move(k1);
    EXPECT_TRUE(k1.user_key().empty());
    EXPECT_TRUE(k1.internal_key().empty());
    EXPECT_TRUE(k1.memtable_key().empty());
    EXPECT_EQ(k2.user_key(), long_key);
  }

  // Inline case:
  {
    LookupKey k1("short", 50);
    LookupKey k2 = std::move(k1);
    EXPECT_TRUE(k1.user_key().empty());
    EXPECT_TRUE(k1.internal_key().empty());
    EXPECT_TRUE(k1.memtable_key().empty());
    EXPECT_EQ(k2.user_key(), "short");
  }
}

TEST(DbFormatTest, EmptyStringViewNullDataHandling) {
  // Default constructed std::string_view has data() == nullptr and size() == 0.
  std::string_view empty_sv;
  ASSERT_EQ(empty_sv.data(), nullptr);
  ASSERT_EQ(empty_sv.size(), 0u);

  // AppendInternalKey must not invoke undefined behavior passing nullptr to append.
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey{empty_sv, 10, kTypeValue});
  EXPECT_EQ(encoded.size(), 8u);
  EXPECT_EQ(ExtractUserKey(encoded), "");

  // LookupKey must not invoke undefined behavior passing nullptr to memcpy.
  LookupKey lkey(empty_sv, 10);
  EXPECT_EQ(lkey.user_key(), "");
  EXPECT_EQ(lkey.internal_key(), encoded);
}

#if STRATA_DEBUG_CHECKS
TEST(DbFormatTest, ParseInternalKeyNullResultAborts) {
  std::string encoded = MakeInternalKey("key", 1, kTypeValue);
  EXPECT_DEATH(ParseInternalKey(encoded, nullptr), "check failed");
}

TEST(DbFormatTest, ExtractUserKeyShortKeyAborts) {
  EXPECT_DEATH(ExtractUserKey("short"), "check failed");
}
#else
TEST(DbFormatTest, ParseInternalKeyNullResult) {
  std::string encoded = MakeInternalKey("key", 1, kTypeValue);
  EXPECT_FALSE(ParseInternalKey(encoded, nullptr));
}

TEST(DbFormatTest, ExtractUserKeyShortKey) {
  EXPECT_EQ(ExtractUserKey("abc"), "");
  EXPECT_EQ(ExtractUserKey(""), "");
}
#endif

TEST(DbFormatTest, ComparatorShortKeys) {
  InternalKeyComparator cmp;
  std::string k_valid = MakeInternalKey("z_valid", 10, kTypeValue);
  std::string k_short = "a_short";

  // Comparison with short key falls back to byte comparison safely without underflow.
  EXPECT_GT(cmp.Compare(k_valid, k_short), 0);
  EXPECT_LT(cmp.Compare(k_short, k_valid), 0);
  EXPECT_EQ(cmp.Compare(k_short, k_short), 0);
}

}  // namespace
}  // namespace strata
