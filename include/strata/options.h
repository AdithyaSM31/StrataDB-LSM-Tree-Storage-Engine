#ifndef STRATA_INCLUDE_OPTIONS_H_
#define STRATA_INCLUDE_OPTIONS_H_

#include <cstddef>
#include <cstdint>

#include "strata/env.h"

namespace strata {

class Snapshot;

// Options that control write operations.
struct WriteOptions {
  // If true, the write will be flushed and synched to disk (fdatasync)
  // before the Write call returns.
  bool sync = false;
};

// Options that control read operations.
struct ReadOptions {
  // If "snapshot" is non-null, read as of the supplied snapshot
  // (which must belong to the DB being read and not have been released).
  // If "snapshot" is null, use an implicit snapshot of the database state
  // at the beginning of this read operation.
  const Snapshot* snapshot = nullptr;

  // Should the data read for this query be cached in memory?
  bool fill_cache = true;
};

// Options to control the behavior of a database (passed to DB::Open).
struct Options {
  // If true, the database will be created if it is missing.
  // Default: false
  bool create_if_missing = false;

  // If true, an error is raised if the database already exists.
  // Default: false
  bool error_if_exists = false;

  // If true, the implementation will do aggressive checking of the
  // data it is processing and will stop early if it detects any errors.
  // Default: false
  bool paranoid_checks = false;

  // Use the specified object to interact with the environment,
  // e.g. to read/write files, schedule background work, etc.
  // Default: Env::Default()
  Env* env = nullptr;

  // Amount of data to build up in memory (backed by an unsorted log
  // on disk) before converting to a sorted on-disk file.
  // Values below 64 KB are raised to 64 KB when the database opens: an empty
  // memtable already reserves one 4 KB arena block, so a smaller buffer would
  // flush after every write.
  // Default: 4 MB
  size_t write_buffer_size = size_t{4} * 1024 * 1024;

  // Approximate size of user data packed per block (before compression).
  // Default: 4 KB
  size_t block_size = 4096;

  // Number of keys between restart points for delta encoding of keys.
  // Default: 16
  int block_restart_interval = 16;

  // Bloom filter bits per key; 10 bits gives about 1% false positives.
  // Default: 10
  int bloom_bits_per_key = 10;

  // Capacity of block cache (sharded LRU cache of uncompressed data blocks).
  // Default: 8 MB
  size_t block_cache_capacity = size_t{8} * 1024 * 1024;

  // Number of open files that can be used by the DB (table cache size).
  // Default: 1000
  int max_open_files = 1000;

  // Target file size for compaction outputs.
  // Default: 2 MB
  size_t max_file_size = size_t{2} * 1024 * 1024;

  // L0 file count that schedules a compaction.
  // Default: 4
  int l0_compaction_trigger = 4;

  // L0 file count where each write is delayed 1 ms.
  // Default: 8
  int l0_slowdown_trigger = 8;

  // L0 file count where writes block until compaction catches up.
  // Default: 12
  int l0_stop_trigger = 12;

  // Base level (L1) maximum total data size; each deeper level is 10x larger.
  // Default: 10 MB
  uint64_t max_bytes_for_level_base = uint64_t{10} * 1024 * 1024;

  Options() : env(Env::Default()) {}
};

}  // namespace strata

#endif  // STRATA_INCLUDE_OPTIONS_H_
