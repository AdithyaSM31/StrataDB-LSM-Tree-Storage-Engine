#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "db/dbformat.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/check.h"
#include "util/coding.h"

namespace strata {
namespace {

// An iterator over a data block that shares ownership of the block, so the
// two-level iterator or cache can evict/drop it without invalidating an active scan.
class OwningBlockIterator : public Iterator {
 public:
  OwningBlockIterator(std::shared_ptr<const Block> block, const Comparator* comparator)
      : block_(std::move(block)), iter_(block_->NewIterator(comparator)) {}

  bool Valid() const override { return iter_->Valid(); }
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void SeekToLast() override { iter_->SeekToLast(); }
  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  void Prev() override { iter_->Prev(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  // Declared first so it is destroyed last: iter_ points into it.
  std::shared_ptr<const Block> block_;
  std::unique_ptr<Iterator> iter_;
};

}  // namespace

Status Table::Open(const Options& options, const Comparator* comparator,
                   std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                   std::unique_ptr<Table>* table, BlockCache* block_cache) {
  (void)options;
  table->reset();
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  // Footer.
  std::string footer_space(Footer::kEncodedLength, '\0');
  std::string_view footer_input;
  Status s = file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                        footer_space.data());
  if (!s.ok()) {
    return s;
  }
  if (footer_input.size() != Footer::kEncodedLength) {
    return Status::Corruption("truncated sstable footer");
  }
  Footer footer;
  s = footer.DecodeFrom(footer_input);
  if (!s.ok()) {
    return s;
  }

  // Index block: required.
  std::string index_contents;
  s = ReadBlock(file.get(), file_size, footer.index_handle(), &index_contents);
  if (!s.ok()) {
    return s;
  }
  auto index_block = std::make_unique<Block>(std::move(index_contents));

  const uint64_t cache_id = (block_cache != nullptr) ? block_cache->NewId() : 0;
  std::unique_ptr<Table> result(new Table(comparator, std::move(file), file_size,
                                          std::move(index_block), block_cache, cache_id));
  // Filter: optional, so failures here are not errors.
  result->ReadFilter(footer.metaindex_handle());
  *table = std::move(result);
  return Status::OK();
}

Table::Table(const Comparator* comparator, std::unique_ptr<RandomAccessFile> file,
             uint64_t file_size, std::unique_ptr<Block> index_block, BlockCache* block_cache,
             uint64_t cache_id)
    : comparator_(comparator),
      file_(std::move(file)),
      file_size_(file_size),
      index_block_(std::move(index_block)),
      // bits_per_key only matters when writing; reading uses the k stored in
      // the filter itself.
      filter_policy_(1),
      block_cache_(block_cache),
      cache_id_(cache_id) {
  STRATA_DCHECK(comparator_ != nullptr);
}

Table::~Table() = default;

void Table::ReadFilter(const BlockHandle& metaindex_handle) {
  // Any problem here just means "no filter": correct answers never depend on
  // the filter, it only saves reads.
  std::string metaindex_contents;
  if (!ReadBlock(file_.get(), file_size_, metaindex_handle, &metaindex_contents).ok()) {
    return;
  }
  Block metaindex(std::move(metaindex_contents));
  std::unique_ptr<Iterator> it = metaindex.NewIterator(BytewiseComparator());
  const std::string filter_name = std::string("filter.") + filter_policy_.Name();
  it->Seek(filter_name);
  if (!it->Valid() || it->key() != filter_name) {
    return;  // written without a filter
  }

  std::string_view handle_input = it->value();
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(&handle_input).ok()) {
    return;
  }
  std::string filter;
  if (!ReadBlock(file_.get(), file_size_, filter_handle, &filter).ok()) {
    return;
  }
  filter_ = std::move(filter);
}

std::unique_ptr<Iterator> Table::BlockReader(const ReadOptions& options,
                                             std::string_view index_value) const {
  BlockHandle handle;
  Status s = handle.DecodeFrom(&index_value);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  std::shared_ptr<Block> block;
  std::string cache_key;
  if (block_cache_ != nullptr) {
    PutFixed64(&cache_key, cache_id_);
    PutFixed64(&cache_key, handle.offset());
    block = block_cache_->Lookup(cache_key);
  }

  if (block == nullptr) {
    std::string contents;
    s = ReadBlock(file_.get(), file_size_, handle, &contents);
    if (!s.ok()) {
      return NewErrorIterator(s);
    }
    auto new_block = std::make_shared<Block>(std::move(contents));
    if (block_cache_ != nullptr && options.fill_cache) {
      block = block_cache_->Insert(cache_key, new_block, new_block->size());
    } else {
      block = std::move(new_block);
    }
  }

  return std::make_unique<OwningBlockIterator>(std::move(block), comparator_);
}

std::unique_ptr<Iterator> Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      index_block_->NewIterator(comparator_),
      [this, options](std::string_view index_value) { return BlockReader(options, index_value); });
}

std::unique_ptr<Iterator> Table::NewIterator() const { return NewIterator(ReadOptions()); }

Status Table::InternalGet(const ReadOptions& options, std::string_view internal_key, bool* found,
                          std::string* key, std::string* value) const {
  *found = false;

  // The filter holds USER keys: the lookup key's sequence number differs from
  // the stored entry's, so only the user key can be tested.
  if (filter_.has_value() && internal_key.size() >= 8 &&
      !filter_policy_.KeyMayMatch(ExtractUserKey(internal_key), *filter_)) {
    return Status::OK();  // definitely not in this table: no data block read
  }

  // The first data block whose last key is >= internal_key.
  std::unique_ptr<Iterator> index_iter = index_block_->NewIterator(comparator_);
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) {
    return index_iter->status();  // past the last key, or a corrupt index
  }

  std::unique_ptr<Iterator> block_iter = BlockReader(options, index_iter->value());
  block_iter->Seek(internal_key);
  if (block_iter->Valid()) {
    *found = true;
    key->assign(block_iter->key().data(), block_iter->key().size());
    value->assign(block_iter->value().data(), block_iter->value().size());
  }
  return block_iter->status();
}

Status Table::InternalGet(std::string_view internal_key, bool* found, std::string* key,
                          std::string* value) const {
  return InternalGet(ReadOptions(), internal_key, found, key, value);
}

}  // namespace strata
