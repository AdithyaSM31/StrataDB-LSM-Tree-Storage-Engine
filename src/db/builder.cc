#include "db/builder.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "db/filename.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace strata {

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const InternalKeyComparator& icmp, Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  iter->SeekToFirst();

  const std::string fname = TableFileName(dbname, meta->number);
  Status s = iter->status();
  if (!s.ok()) {
    return s;
  }
  if (!iter->Valid()) {
    return Status::OK();
  }

  std::unique_ptr<WritableFile> file;
  s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }

  TableBuilder builder(options, &icmp, file.get());
  meta->smallest.assign(iter->key().data(), iter->key().size());
  for (; iter->Valid(); iter->Next()) {
    std::string_view key = iter->key();
    meta->largest.assign(key.data(), key.size());
    builder.Add(key, iter->value());
  }

  s = iter->status();
  if (s.ok()) {
    s = builder.Finish();
  } else {
    builder.Abandon();
  }

  if (s.ok()) {
    meta->file_size = builder.FileSize();
    s = file->Sync();
  }
  if (file != nullptr) {
    Status close_s = file->Close();
    if (s.ok() && !close_s.ok()) {
      s = close_s;
    }
    file.reset();
  }

  if (s.ok()) {
    s = env->SyncDir(dbname);
  }

  // Reopen it with Table::Open to check it (cheap insurance)
  if (s.ok()) {
    std::unique_ptr<RandomAccessFile> rfile;
    s = env->NewRandomAccessFile(fname, &rfile);
    if (s.ok()) {
      std::unique_ptr<Table> table;
      s = Table::Open(options, &icmp, std::move(rfile), meta->file_size, &table);
    }
  }

  if (!s.ok() || meta->file_size == 0) {
    (void)env->RemoveFile(fname);
  }
  return s;
}

}  // namespace strata
