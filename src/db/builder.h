#ifndef STRATA_DB_BUILDER_H_
#define STRATA_DB_BUILDER_H_

#include <string>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {

// Build a Table file from the contents of *iter. The generated file will
// be named according to meta->number. On success, the rest of the metadata fields
// (file_size, smallest, largest) will be filled in, the file will be synced and
// closed, and the db directory will be synced.
// On failure or empty memtable, any generated file will be removed.
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const InternalKeyComparator& icmp, Iterator* iter, FileMetaData* meta);

}  // namespace strata

#endif  // STRATA_DB_BUILDER_H_
