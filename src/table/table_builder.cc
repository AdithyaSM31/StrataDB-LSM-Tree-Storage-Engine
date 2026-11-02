#include "table/table_builder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

TableBuilder::TableBuilder(const Options& options, const Comparator* comparator, WritableFile* file)
    : options_(options),
      comparator_(comparator),
      file_(file),
      data_block_(options.block_restart_interval, comparator),
      // Every index key is a restart point: the index is binary searched on
      // every lookup and holds only one key per data block.
      index_block_(1, comparator) {
  STRATA_DCHECK(comparator_ != nullptr);
  STRATA_DCHECK(file_ != nullptr);
  if (options_.bloom_bits_per_key > 0) {
    filter_policy_ = std::make_unique<BloomFilterPolicy>(options_.bloom_bits_per_key);
  }
}

TableBuilder::~TableBuilder() {
  // Forgetting Finish()/Abandon() would silently leave a partial file.
  STRATA_DCHECK(closed_);
}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  STRATA_DCHECK(!closed_);
  if (!status_.ok()) {
    return;
  }
  STRATA_DCHECK(num_entries_ == 0 || comparator_->Compare(key, last_key_) > 0);
  STRATA_DCHECK(key.size() >= 8);  // an internal key

  if (filter_policy_ != nullptr) {
    // One filter entry per distinct user key: consecutive versions of the
    // same user key would only inflate the filter.
    const std::string_view user_key = ExtractUserKey(key);
    if (filter_keys_.empty() || std::string_view(filter_keys_.back()) != user_key) {
      filter_keys_.emplace_back(user_key);
    }
  }

  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  data_block_.Add(key, value);

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  STRATA_DCHECK(!closed_);
  if (!status_.ok() || data_block_.empty()) {
    return;
  }
  BlockHandle handle;
  WriteBlock(&data_block_, &handle);
  if (!status_.ok()) {
    return;
  }
  // Index entry: the block's last key -> where the block is. Seeking the index
  // to a target then lands on the first block whose last key >= target, which
  // is the only block that can hold the first entry >= target.
  std::string encoded_handle;
  handle.EncodeTo(&encoded_handle);
  index_block_.Add(last_key_, encoded_handle);
  // Hand the block to the kernel so writeback can start (not a durability point).
  status_ = file_->Flush();
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  WriteRawBlock(block->Finish(), kNoCompression, handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(std::string_view contents, CompressionType type,
                                 BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(contents.size());
  status_ = file_->Append(contents);
  if (!status_.ok()) {
    return;
  }
  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(type);
  // The CRC covers the contents and the type byte.
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.ok()) {
    offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  STRATA_DCHECK(!closed_);
  FlushDataBlock();
  closed_ = true;

  // Filter block: one bloom filter for the whole file, raw (no block format).
  BlockHandle filter_handle;
  const bool has_filter = filter_policy_ != nullptr && status_.ok();
  if (has_filter) {
    std::vector<std::string_view> keys(filter_keys_.begin(), filter_keys_.end());
    std::string filter;
    filter_policy_->CreateFilter(keys.data(), keys.size(), &filter);
    WriteRawBlock(filter, kNoCompression, &filter_handle);
  }

  // Metaindex block: names of the meta blocks -> their handles.
  BlockHandle metaindex_handle;
  if (status_.ok()) {
    BlockBuilder metaindex_block(1, BytewiseComparator());
    if (has_filter) {
      std::string encoded_handle;
      filter_handle.EncodeTo(&encoded_handle);
      metaindex_block.Add(std::string("filter.") + filter_policy_->Name(), encoded_handle);
    }
    WriteBlock(&metaindex_block, &metaindex_handle);
  }

  // Index block.
  BlockHandle index_handle;
  if (status_.ok()) {
    WriteBlock(&index_block_, &index_handle);
  }

  // Footer.
  if (status_.ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_handle);
    footer.set_index_handle(index_handle);
    std::string encoded;
    footer.EncodeTo(&encoded);
    status_ = file_->Append(encoded);
    if (status_.ok()) {
      offset_ += encoded.size();
    }
  }
  return status_;
}

void TableBuilder::Abandon() {
  STRATA_DCHECK(!closed_);
  closed_ = true;
}

}  // namespace strata
