#include "table/block.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "strata/status.h"
#include "table/block_builder.h"
#include "util/coding.h"
#include "util/comparator.h"
#include "util/random.h"

namespace strata {
namespace {

using Model = std::map<std::string, std::string>;

const Comparator* Bytewise() { return BytewiseComparator(); }

std::string BuildBlock(const Model& model, int restart_interval) {
  BlockBuilder builder(restart_interval, Bytewise());
  for (const auto& [key, value] : model) {
    builder.Add(key, value);
  }
  return std::string(builder.Finish());
}

// Keys with long shared prefixes, plus a random binary tail, like real keys.
Model RandomModel(Random* rnd, int n) {
  Model model;
  while (static_cast<int>(model.size()) < n) {
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "user:%06u:", rnd->Uniform(1000000));
    std::string key = prefix;
    const uint32_t tail = rnd->Skewed(4);
    for (uint32_t i = 0; i < tail; ++i) {
      key.push_back(static_cast<char>(rnd->Uniform(256)));
    }
    std::string value(rnd->Skewed(6), static_cast<char>('a' + rnd->Uniform(26)));
    model[key] = value;
  }
  return model;
}

void FlipBit(std::string& s, size_t byte, size_t bit) {
  s[byte] = static_cast<char>(static_cast<uint8_t>(s[byte]) ^ (1U << bit));
}

// Checks every way of moving through the block against the model.
void CheckBlockMatchesModel(const std::string& contents, const Model& model, Random* rnd) {
  Block block(contents);
  std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());

  // Forward scan.
  it->SeekToFirst();
  for (const auto& [key, value] : model) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), key);
    ASSERT_EQ(it->value(), value);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->status().ok()) << it->status().ToString();

  // Backward scan with Prev, which crosses every restart boundary.
  it->SeekToLast();
  for (auto rit = model.rbegin(); rit != model.rend(); ++rit) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), rit->first);
    ASSERT_EQ(it->value(), rit->second);
    it->Prev();
  }
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->status().ok()) << it->status().ToString();

  // Seek to every key, then step back and forward again.
  for (auto mit = model.begin(); mit != model.end(); ++mit) {
    it->Seek(mit->first);
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), mit->first);
    it->Prev();
    if (mit == model.begin()) {
      EXPECT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(it->key(), std::prev(mit)->first);
      it->Next();
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(it->key(), mit->first);
    }
  }

  // Seek between keys: must land on the first key >= target.
  for (int i = 0; i < 500; ++i) {
    std::string target(rnd->Skewed(5), '\0');
    for (char& c : target) {
      c = static_cast<char>(rnd->Uniform(256));
    }
    if (rnd->OneIn(2) && !model.empty()) {
      // Something just after an existing key.
      auto pick = model.begin();
      std::advance(pick, rnd->Uniform(static_cast<uint32_t>(model.size())));
      target = pick->first + std::string(1, '\0');
    }
    const auto expected = model.lower_bound(target);
    it->Seek(target);
    if (expected == model.end()) {
      EXPECT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(it->key(), expected->first);
    }
  }

  // Before the first key and after the last.
  it->Seek("");
  if (model.empty()) {
    EXPECT_FALSE(it->Valid());
  } else {
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), model.begin()->first);
  }
  it->Seek(std::string(64, '\xff'));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

// Walks a possibly corrupt block every way it can. It must never crash, never
// loop forever, and report either OK or Corruption.
void ExerciseWithoutCrashing(const std::string& contents) {
  Block block(contents);
  std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());
  constexpr int kStepLimit = 100000;

  int steps = 0;
  for (it->SeekToFirst(); it->Valid() && steps < kStepLimit; it->Next(), ++steps) {
    (void)it->key();
    (void)it->value();
  }
  ASSERT_LT(steps, kStepLimit) << "forward scan did not terminate";
  EXPECT_TRUE(it->status().ok() || it->status().IsCorruption());

  steps = 0;
  for (it->SeekToLast(); it->Valid() && steps < kStepLimit; it->Prev(), ++steps) {
    (void)it->key();
  }
  ASSERT_LT(steps, kStepLimit) << "backward scan did not terminate";

  for (const char* target : {"", "user:", "user:500000:", "zzz"}) {
    it->Seek(target);
    if (it->Valid()) {
      (void)it->key();
      it->Next();
    }
  }
  EXPECT_TRUE(it->status().ok() || it->status().IsCorruption());
}

TEST(BlockTest, EmptyBlock) {
  BlockBuilder builder(16, Bytewise());
  EXPECT_TRUE(builder.empty());
  const std::string contents(builder.Finish());
  // One restart point (offset 0) and the count (1).
  EXPECT_EQ(contents, std::string("\x00\x00\x00\x00\x01\x00\x00\x00", 8));

  Block block(contents);
  std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  EXPECT_FALSE(it->Valid());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

TEST(BlockTest, ExactBytesOfTheFormat) {
  // The worked example: restart interval 2, so "apply" starts a restart point.
  BlockBuilder builder(2, Bytewise());
  builder.Add("apple", "1");
  builder.Add("applesauce", "2");  // shares "apple"
  builder.Add("apply", "3");       // restart point: stored whole
  builder.Add("apricot", "4");     // shares "ap"
  const std::string expected =
      std::string("\x00\x05\x01", 3) + "apple" + "1" + std::string("\x05\x05\x01", 3) + "sauce" +
      "2" + std::string("\x00\x05\x01", 3) + "apply" + "3" + std::string("\x02\x05\x01", 3) +
      "ricot" + "4" + std::string("\x00\x00\x00\x00", 4) +  // restart 0
      std::string("\x12\x00\x00\x00", 4) +                  // restart 18
      std::string("\x02\x00\x00\x00", 4);                   // 2 restarts
  EXPECT_EQ(std::string(builder.Finish()), expected);
}

TEST(BlockTest, RoundTripAgainstModel) {
  for (const int interval : {1, 2, 16, 1000}) {
    SCOPED_TRACE("restart_interval=" + std::to_string(interval));
    Random rnd(301 + static_cast<uint64_t>(interval));
    const Model model = RandomModel(&rnd, 2000);
    CheckBlockMatchesModel(BuildBlock(model, interval), model, &rnd);
  }
}

TEST(BlockTest, SmallBlocks) {
  Random rnd(7);
  for (int n = 1; n <= 20; ++n) {
    for (const int interval : {1, 3, 16}) {
      SCOPED_TRACE("n=" + std::to_string(n) + " interval=" + std::to_string(interval));
      const Model model = RandomModel(&rnd, n);
      CheckBlockMatchesModel(BuildBlock(model, interval), model, &rnd);
    }
  }
}

TEST(BlockTest, AwkwardKeysAndValues) {
  Model model;
  model[""] = "empty key";
  model[std::string("\0", 1)] = "";
  model[std::string("\0\0", 2)] = "two nuls";
  model["a"] = std::string(5000, 'v');  // large value
  model[std::string(1024, 'k')] = "long key";
  model[std::string(1024, 'k') + "x"] = "long shared prefix";
  model["\x7f"] = "";
  model["\x80"] = "high byte";
  model["\xff"] = "max byte";
  model["\xff\xff"] = "";
  Random rnd(11);
  for (const int interval : {1, 4, 16}) {
    CheckBlockMatchesModel(BuildBlock(model, interval), model, &rnd);
  }
}

TEST(BlockTest, PrefixCompressionSavesSpace) {
  Model model;
  size_t raw = 0;
  for (int i = 0; i < 1000; ++i) {
    char key[64];
    std::snprintf(key, sizeof(key), "user_profile:region_eu:%08d", i);
    model[key] = "v";
    raw += std::string_view(key).size() + 1;
  }
  const std::string contents = BuildBlock(model, 16);
  // ~33-byte keys sharing ~30 bytes: the block should be well under half the raw size.
  EXPECT_LT(contents.size(), raw / 2) << "block " << contents.size() << " vs raw " << raw;
}

TEST(BlockTest, SizeEstimateAndReset) {
  Random rnd(5);
  const Model model = RandomModel(&rnd, 300);
  BlockBuilder builder(16, Bytewise());
  for (const auto& [key, value] : model) {
    builder.Add(key, value);
  }
  const size_t estimate = builder.CurrentSizeEstimate();
  const std::string first(builder.Finish());
  EXPECT_EQ(estimate, first.size());

  // Reset makes the builder reusable, producing identical bytes.
  builder.Reset();
  EXPECT_TRUE(builder.empty());
  for (const auto& [key, value] : model) {
    builder.Add(key, value);
  }
  EXPECT_EQ(std::string(builder.Finish()), first);
}

TEST(BlockTest, InternalKeysNewestVersionFirst) {
  // Data blocks hold internal keys; check the read path's Seek works with them.
  const InternalKeyComparator icmp;
  BlockBuilder builder(16, &icmp);
  builder.Add(MakeInternalKey("apple", 7, kTypeValue), "green");
  builder.Add(MakeInternalKey("apple", 5, kTypeDeletion), "");
  builder.Add(MakeInternalKey("apple", 3, kTypeValue), "red");
  builder.Add(MakeInternalKey("banana", 9, kTypeValue), "yellow");
  Block block{std::string(builder.Finish())};
  std::unique_ptr<Iterator> it = block.NewIterator(&icmp);

  // A Get("apple") as of sequence 6 seeks to ("apple", 6) and finds the tombstone.
  it->Seek(LookupKey("apple", 6).internal_key());
  ASSERT_TRUE(it->Valid());
  ParsedInternalKey parsed;
  ASSERT_TRUE(ParseInternalKey(it->key(), &parsed));
  EXPECT_EQ(parsed.user_key, "apple");
  EXPECT_EQ(parsed.sequence, 5U);
  EXPECT_EQ(parsed.type, kTypeDeletion);

  // As of sequence 100 it finds the newest value.
  it->Seek(LookupKey("apple", 100).internal_key());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "green");
}

TEST(BlockTest, TruncatedAtEveryLength) {
  Random rnd(17);
  const std::string contents = BuildBlock(RandomModel(&rnd, 100), 4);
  for (size_t len = 0; len < contents.size(); ++len) {
    SCOPED_TRACE("len=" + std::to_string(len));
    ExerciseWithoutCrashing(contents.substr(0, len));
  }
}

TEST(BlockTest, EveryBitFlipped) {
  Random rnd(19);
  const std::string contents = BuildBlock(RandomModel(&rnd, 30), 4);
  for (size_t byte = 0; byte < contents.size(); ++byte) {
    for (size_t bit = 0; bit < 8; ++bit) {
      SCOPED_TRACE("byte=" + std::to_string(byte) + " bit=" + std::to_string(bit));
      std::string corrupted = contents;
      FlipBit(corrupted, byte, bit);
      ExerciseWithoutCrashing(corrupted);
    }
  }
}

TEST(BlockTest, BadRestartCounts) {
  Random rnd(23);
  const std::string contents = BuildBlock(RandomModel(&rnd, 50), 16);
  for (const uint32_t count : {0U, 0xFFFFFFFFU, 0x40000000U}) {
    SCOPED_TRACE("count=" + std::to_string(count));
    std::string corrupted = contents;
    EncodeFixed32(corrupted.data() + corrupted.size() - 4, count);
    Block block(corrupted);
    std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().IsCorruption());
  }
}

TEST(BlockTest, RestartOffsetOutsideEntries) {
  Random rnd(29);
  const std::string contents = BuildBlock(RandomModel(&rnd, 50), 16);
  // Point restart 0 past the end of the entries.
  std::string corrupted = contents;
  const uint32_t num_restarts = DecodeFixed32(corrupted.data() + corrupted.size() - 4);
  const size_t restart_array = corrupted.size() - 4 * (static_cast<size_t>(num_restarts) + 1);
  EncodeFixed32(corrupted.data() + restart_array, 0x7FFFFFFFU);
  Block block(corrupted);
  std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(BlockTest, TooSmallToHoldARestartCount) {
  for (const std::string& contents :
       {std::string(), std::string("\x01", 1), std::string(3, '\0')}) {
    Block block(contents);
    std::unique_ptr<Iterator> it = block.NewIterator(Bytewise());
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    EXPECT_TRUE(it->status().IsCorruption());
  }
}

}  // namespace
}  // namespace strata
