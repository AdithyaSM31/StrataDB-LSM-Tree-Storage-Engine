#ifndef STRATA_TABLE_FORMAT_H_
#define STRATA_TABLE_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "strata/env.h"
#include "strata/status.h"

namespace strata {

// SSTable layout:
//
//   [data block 0][trailer] ... [data block N][trailer]
//   [filter block][trailer]       one bloom filter for the whole file (optional)
//   [metaindex block][trailer]    "filter.bloom" -> handle of the filter block
//   [index block][trailer]        last key of each data block -> its handle
//   [footer: 48 bytes]            metaindex handle, index handle, padding, magic
//
//   trailer = compression type (1 byte) + masked CRC32C (4 bytes) of the block
//             contents followed by the type byte.

// Where a block lives in the file: its offset and size, not counting the trailer.
class BlockHandle {
 public:
  // Two varint64s.
  static constexpr size_t kMaxEncodedLength = 10 + 10;

  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  // Consumes a handle from the front of *input.
  Status DecodeFrom(std::string_view* input);

 private:
  uint64_t offset_ = ~uint64_t{0};
  uint64_t size_ = ~uint64_t{0};
};

// The fixed-size record at the end of every SSTable.
class Footer {
 public:
  // Two handles padded to their maximum length, then the 8-byte magic number.
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  // `input` must be exactly kEncodedLength bytes.
  Status DecodeFrom(std::string_view input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

static_assert(Footer::kEncodedLength == 48, "the design doc specifies a 48-byte footer");

// Identifies a StrataDB SSTable. Read as big-endian ASCII it spells "StrataDB".
inline constexpr uint64_t kTableMagicNumber = 0x5374726174614442ULL;

// 1-byte compression type + 4-byte CRC.
inline constexpr size_t kBlockTrailerSize = 5;

enum CompressionType : uint8_t {
  kNoCompression = 0,  // the only type written; LZ4 is a stretch goal
};

// Reads the block at `handle` from `file`, verifies its trailer, and stores the
// block contents (without the trailer) in *contents.
//
// The handle may come from a corrupt index, so it is checked against
// `file_size` before anything is read. Returns Corruption if the block lies
// outside the file, is truncated, fails its CRC, or has an unknown
// compression type.
Status ReadBlock(const RandomAccessFile* file, uint64_t file_size, const BlockHandle& handle,
                 std::string* contents);

}  // namespace strata

#endif  // STRATA_TABLE_FORMAT_H_
