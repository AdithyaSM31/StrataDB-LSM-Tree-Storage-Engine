#include "table/format.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Both fields must have been set: the defaults are deliberately invalid.
  STRATA_DCHECK(offset_ != ~uint64_t{0});
  STRATA_DCHECK(size_ != ~uint64_t{0});
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength);  // zero padding
  PutFixed64(dst, kTableMagicNumber);
  STRATA_DCHECK(dst->size() == original_size + kEncodedLength);
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("bad footer length");
  }
  // Check the magic number first: a wrong one means this is not an SSTable at
  // all, which deserves a clearer message than a bad handle.
  const uint64_t magic = DecodeFixed64(input.data() + kEncodedLength - 8);
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  std::string_view handles = input.substr(0, 2 * BlockHandle::kMaxEncodedLength);
  Status s = metaindex_handle_.DecodeFrom(&handles);
  if (s.ok()) {
    s = index_handle_.DecodeFrom(&handles);
  }
  return s;
}

Status ReadBlock(const RandomAccessFile* file, uint64_t file_size, const BlockHandle& handle,
                 std::string* contents) {
  contents->clear();
  // The block and its trailer must lie inside the file. Written so that no
  // subtraction can wrap: a corrupt handle may hold any 64-bit values.
  const uint64_t size = handle.size();
  const uint64_t offset = handle.offset();
  if (size > file_size || offset > file_size - size ||
      file_size - size - offset < kBlockTrailerSize) {
    return Status::Corruption("block handle points outside the file");
  }

  // Read the contents and trailer straight into *contents, then drop the trailer.
  const size_t n = static_cast<size_t>(size);
  contents->resize(n + kBlockTrailerSize);
  std::string_view result;
  Status s = file->Read(offset, n + kBlockTrailerSize, &result, contents->data());
  if (!s.ok()) {
    contents->clear();
    return s;
  }
  if (result.size() != n + kBlockTrailerSize) {
    contents->clear();
    return Status::Corruption("truncated block read");
  }

  const char* data = result.data();
  // The CRC covers the contents and the type byte.
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(data + n + 1));
  const uint32_t actual = crc32c::Value(data, n + 1);
  if (actual != expected) {
    contents->clear();
    return Status::Corruption("block checksum mismatch");
  }
  if (static_cast<uint8_t>(data[n]) != kNoCompression) {
    contents->clear();
    return Status::Corruption("unknown block compression type");
  }

  if (data == contents->data()) {
    contents->resize(n);  // read into our buffer: just drop the trailer
  } else {
    contents->assign(data, n);  // the file returned its own buffer: copy it
  }
  return Status::OK();
}

}  // namespace strata
