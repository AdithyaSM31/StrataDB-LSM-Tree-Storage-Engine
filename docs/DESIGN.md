# StrataDB design

## Overview and goals

StrataDB is an embedded, ordered key-value store in C++20, built as a log-structured merge tree: a write-ahead log, a skiplist memtable, immutable SSTables with bloom filters, and leveled compaction. It follows LevelDB's overall design, and I compare the two directly in [BENCHMARKS.md](BENCHMARKS.md).

The goals, in order:

1. **Don't lose or corrupt data.** A synced write survives power loss, recovery always gives a prefix of history, and damage is reported rather than returned as data.
2. **Be testable.** All file access goes through an `Env`, so tests can inject faults and simulate crashes.
3. **Be fast enough to compare with LevelDB honestly.** Performance comes third, and the gaps are documented rather than hidden.

Out of scope: compression, custom comparators, multiple background threads and replication.

## Public API

```cpp
Status DB::Open(const Options&, const std::string& name, std::unique_ptr<DB>* out);

Status Put(const WriteOptions&, std::string_view key, std::string_view value);
Status Delete(const WriteOptions&, std::string_view key);
Status Write(const WriteOptions&, WriteBatch*);          // atomic across its entries
Status Get(const ReadOptions&, std::string_view key, std::string* value);
std::unique_ptr<Iterator> NewIterator(const ReadOptions&);  // forward and reverse
const Snapshot* GetSnapshot();
void ReleaseSnapshot(const Snapshot*);
Status Flush();
Status CompactRange(std::string_view begin, std::string_view end);
bool GetProperty(std::string_view name, std::string* value);
```

`WriteOptions::sync` picks the durability level for a write. `ReadOptions` can name a snapshot and can skip filling the block cache. The properties are `strata.stats`, `strata.num-files-at-level<N>` and `strata.recovery`. A `DB` is safe to use from multiple threads.

## Architecture: write path and read path

```
Write                                     Read
  |                                         |
  v                                         v
writer queue (group commit)               memtable
  |                                         |
  v                                       immutable memtable (if any)
WAL append, then fdatasync if sync=true     |
  |                                         v
  v                                       L0 files, newest first, each one that covers the key
memtable (skiplist)                         |
  |  full: becomes the immutable memtable   v
  v                                       L1 to L6, at most one file per level
flush to an L0 SSTable                      |
  |                                         v
  v                                       in each file: range check, bloom filter,
leveled compaction, L0 down to L6         index block, data block (block cache first)
```

**Writes.** Writers join a queue, and the one at the front becomes the leader. It makes room if the memtable is full, merges queued batches into one, assigns sequence numbers, appends one WAL record, syncs if asked, and inserts into the memtable. `LastSequence` only moves after that, so readers never see a half-applied group.

**Reads.** `Get` takes the mutex just long enough to pick a sequence number and reference the memtables and the current version. The lookup itself runs without the lock. The first version found for the key decides the answer, and a tombstone returns NotFound, so older files can't bring a deleted value back.

## Internal key format

```
[ user key ][ 8-byte little-endian trailer: (sequence << 8) | type ]
```

The type is `0` for a deletion and `1` for a value, and sequence numbers have 56 bits. Internal keys sort by user key ascending, then trailer **descending**, so the newest version of a key comes first. A lookup builds a key with the read's sequence number and seeks to the first entry at or after it, which is the newest version visible to that read.

Memtable entries add lengths around this:

```
[ varint32 internal key length ][ internal key ][ varint32 value length ][ value ]
```

## WAL and MANIFEST record format

Both use the same log format. The file is divided into 32 KiB blocks, and each block holds physical records:

```
[ masked CRC32C: 4 ][ length: 2 ][ type: 1 ][ payload ]
```

The CRC covers the type byte and the payload. A logical record that doesn't fit is split into `First`, `Middle` and `Last` pieces, and one that fits is `Full`. If fewer than 7 bytes remain in a block, they are zero-filled.

**WAL payload** is a `WriteBatch`:

```
[ sequence: fixed64 ][ count: fixed32 ]
then for each entry: [ type: 1 ][ varint key length ][ key ][ varint value length ][ value ]
```

A deletion has no value. Because a batch is one logical record with a checksum, it is applied entirely or not at all.

**MANIFEST payload** is a `VersionEdit`, a sequence of varint-tagged fields:

| Tag | Field | Encoding |
|---|---|---|
| 1 | Comparator name | length-prefixed string |
| 2 | Log number | varint64 |
| 3 | Next file number | varint64 |
| 4 | Last sequence | varint64 |
| 5 | Deleted file | level, file number |
| 6 | New file | level, number, size, smallest key, largest key |
| 7 | Compact pointer | level, internal key |

## SSTable layout

```
[ data block 0 ][ trailer ] ... [ data block N ][ trailer ]
[ filter block ][ trailer ]      one bloom filter for the whole file (optional)
[ metaindex block ][ trailer ]   "filter.bloom" -> filter block handle
[ index block ][ trailer ]       last key of each data block -> its handle
[ footer: 48 bytes ]             metaindex handle, index handle, padding, magic

trailer = compression type (1 byte) + masked CRC32C (4 bytes) of contents and type
```

The magic number spells `StrataDB` when read as big-endian ASCII. A block handle is two varint64s, offset and size.

**Blocks** hold sorted entries with prefix-compressed keys:

```
entry: [ shared: varint32 ][ non-shared: varint32 ][ value length: varint32 ][ key suffix ][ value ]
block: entries, [ restart offset: fixed32 ]..., [ restart count: fixed32 ]
```

Every 16th key (`block_restart_interval`) is stored whole and recorded as a restart point, so a reader can binary search the restarts and scan forward from there. The index block restarts at every key, because it is searched on every lookup. A data block is cut at about 4 KB.

**Filters** default to 10 bits per user key with 7 probes, using double hashing. The probe count is stored in the filter, so a file written with other settings still reads correctly. Multiple versions of one user key add one filter entry.

## Files in the database directory

| File | Purpose |
|---|---|
| `NNNNNN.log` | Write-ahead log |
| `NNNNNN.sst` | SSTable (`.ldb` is also accepted when reading a directory) |
| `MANIFEST-NNNNNN` | Log of version edits |
| `CURRENT` | Name of the live MANIFEST, plus a newline |
| `LOCK` | `flock` that stops a second process opening the database |
| `NNNNNN.dbtmp` | Temporary file, used to replace `CURRENT` atomically |

All numbered files share one counter, which is stored in the MANIFEST.

## Versions and the MANIFEST

A `Version` is an immutable list of files per level, across 7 levels. A new version is the current one plus a `VersionEdit`, applied by `VersionSet::LogAndApply`:

1. Build the new version from the edit.
2. On the first edit after Open, create a new MANIFEST holding a full snapshot, sync it, and point `CURRENT` at it. `CURRENT` is replaced by writing a temp file, syncing it, renaming it and syncing the directory.
3. Append the edit and sync the MANIFEST.
4. Install the new version.

If that sync fails, the MANIFEST might end with part of a record, and a later record after that would look like corruption. So the file is abandoned, and the next edit starts a fresh MANIFEST with a full snapshot.

Versions are reference-counted. An iterator or running compaction keeps its version alive, and with it the files it uses. A table file is only deleted once no live version lists it and it isn't the output of a running flush or compaction. After a background error, deletion stops altogether, because a failed MANIFEST write may still have reached disk. The next Open cleans up once it knows which version won.

## Compaction: picking and running

**Scoring.** Each new version gets a score per level, and the highest one wins. Compaction runs when it reaches 1.0.

- L0: files divided by 4 (`l0_compaction_trigger`). L0 counts files because they overlap, so each one adds a probe to reads.
- L*n*: bytes divided by the limit, which is 10 MB for L1 and 10x larger per level.

**Picking inputs.**

1. Each level has a persisted compact pointer. The first file past it is picked, wrapping to the start, so compaction moves round-robin through the key space.
2. In L0, the input grows to every file that overlaps, repeatedly, because a key's versions can't be split across a level boundary out of order.
3. Every overlapping file in L*n+1* is added.
4. The L*n* side is grown if that adds no L*n+1* files and the total stays within 25 × `max_file_size` (50 MB).
5. The overlapping files in L*n+2* are recorded as grandparents.

**Trivial move.** One input, nothing to merge with below and no more than 20 MB of grandparent overlap means the file just moves down with a metadata change. `CompactRange` doesn't use this shortcut, because moving a file would carry its tombstones and old values with it.

**Merge rules.** With `S` as the smallest live snapshot, an entry is dropped if:

- **Rule A:** a newer version of the key exists at a sequence ≤ `S`, so no reader can reach this one.
- **Rule B:** it is a tombstone at a sequence ≤ `S`, and no deeper level could hold an older version it must still hide.

**Output files.** A file is cut at 2 MB, or once it overlaps more than 20 MB of grandparents, which limits how expensive its own later compaction can be. Cuts only happen between user keys, so all versions of a key stay in one file and levels below L0 never overlap.

**Priorities and backpressure.** One background thread does the work: memtable flush first, then manual compaction, then automatic compaction. A running compaction checks between keys for a waiting memtable and flushes it straight away, so a long merge can't stall writers. Writers sleep 1 ms once per write at 8 L0 files and block entirely at 12, or while an immutable memtable is still being flushed.

**Install.** Each output is synced, its directory entry synced, and the file reopened with `Table::Open` to check it. A single edit then removes the inputs, adds the outputs and advances the compact pointer.

## Recovery on Open

1. Take the `LOCK`. Create the database if needed.
2. Read `CURRENT` and replay the MANIFEST into a version. A torn last record is dropped, but any other corruption here fails the Open, because a wrong file list can't be recovered from safely.
3. Scan the directory. A table the version lists but the disk lacks fails the Open.
4. Replay logs numbered at or above the MANIFEST's log number, oldest first, into a new memtable, tracking the highest sequence number.
5. Create a new WAL and sync the directory.
6. If the memtable has data, write it to L0 and record the new log number, so those logs are no longer needed.
7. Delete obsolete files and start the background thread.

**Damaged logs.** A record cut short at the end of the log is a torn write, which is normal after a crash, and it is dropped silently. Anything else depends on `paranoid_checks`:

- **On:** Open fails.
- **Off (default):** replay stops at the damage and later records and logs are discarded, so the result is still a prefix of history. The reason is available from `GetProperty("strata.recovery")`.

## Durability guarantees

| Failure | `sync=false` (default) | `sync=true` |
|---|---|---|
| Process crash, including `kill -9` | Acknowledged writes survive: each record reaches the kernel with `write` before the call returns | Acknowledged writes survive |
| OS crash or power loss | Recent acknowledged writes may be lost, and the rest are a prefix of history | Acknowledged writes survive: the WAL is `fdatasync`ed before the call returns |

This holds for every write regardless of `sync`:

- **Atomic batches:** a `WriteBatch` is applied entirely or not at all.
- **Prefix property:** after any crash, the state matches some prefix of the write history, with no gaps.
- **Flushed data is durable:** the table, its directory entry and the MANIFEST edit are all synced before a log is retired.
- **New logs are durable:** the directory is synced when a WAL is created, because a synced write is worthless if the file's name is lost.
- **Group commit is safe:** a non-sync write may share a sync leader's `fdatasync`, but a sync write never joins a non-sync leader's group.
- **Failed fsync is sticky:** after a failed WAL write or sync, or a failed flush or compaction install, all later writes return the error until the database is reopened. I don't retry a failed `fsync`, because the kernel may already have dropped the dirty pages. Recovery then decides what actually reached disk.
- **Bit rot is detected:** every block is CRC-checked when read from disk, and a failure returns Corruption, never wrong data. Blocks already in the block cache aren't rechecked.

These guarantees assume the device honours `fdatasync`.

## Concurrency model

- **One mutex (`mutex_`)** protects the writer queue, the memtable pointers, the version set and the snapshot list.
- **One writer at a time.** The queue leader drops the mutex for the WAL write and memtable insert. Only it can touch them, because everyone else is waiting behind it.
- **Lock-free memtable reads.** The skiplist has one writer and any number of readers. A new node is fully built and then linked in with release stores, from the bottom level up, and readers use acquire loads. Nodes are never freed until the whole memtable is.
- **Reference counts** keep memtables and versions alive for as long as a read or iterator uses them. Counts change only under the mutex.
- **One background thread** handles flushes and compactions, dropping the mutex while it does I/O.
- **Caches** are sharded LRU with 16 shards and their own locks. The block cache holds data blocks. The table cache holds open tables, with their index and filter blocks in memory, up to `max_open_files - 10`.

ThreadSanitizer runs on every push and has to stay clean.

## Testing strategy

| What | How |
|---|---|
| Each unit | One `*_test.cc` per component, 447 tests labelled `unit` |
| Logic against a model | `model_test`: randomized operations, reopens, snapshots and `CompactRange` checked against a `std::map`, plus bidirectional iterator fuzzing |
| Power loss | `FaultInjectionEnv` drops unsynced data. `crash_test` checks synced writes, the prefix property and batch atomicity, and crashes at every file operation of a run with flushes and a compaction |
| Process death | `kill_test`: 1,000 cycles of fork, write and `SIGKILL`, verifying everything the child confirmed durable |
| Failing I/O | Injected errors, including failed MANIFEST syncs during flush and compaction |
| Damaged files | Bit flips and truncation across WAL records and SSTables: each must be detected or harmless, and must never return wrong data |
| Memory and races | ASan, UBSan and TSan builds in CI |

The release build runs 466 tests in total, with 19 labelled `slow`. CI runs ASan with UBSan, TSan and release builds with GCC and Clang, a `clang-format` check, a benchmark smoke test and a test of the benchmark runner.

## Trade-offs and what I would change

These are the choices I made, where they differ from LevelDB, and what they cost. Measured figures come from provisional 1M-key runs, and [BENCHMARKS.md](BENCHMARKS.md) has the final numbers.

**Choices I'd keep**

- **One bloom filter per file, checked before the index.** LevelDB builds a filter per 2 KB of file data and searches the index first. Mine gave a similar pass rate on ordinary missing keys (0.76% vs 0.90%), and held up far better on keys one byte away from stored keys in the same block (1.36% vs 18.6%). The cost is that a whole file's filter has to sit in memory.
- **`pread` rather than `mmap`.** It's simpler, and it behaves predictably when memory is tight. LevelDB with its default `mmap` was about 1.4x faster on warm reads than LevelDB with `pread`. Under a memory limit the same comparison reversed, and it read more than 6x as much from disk, because its mapped pages, including index and filter blocks, can be evicted.
- **No read-triggered compaction.** Read-only workloads write nothing and the LSM shape stays fixed, which makes behaviour easier to reason about. It does cost read speed: LevelDB made fewer table probes per lookup (1.06 vs 1.40), and its seek compaction is part of the reason.
- **Syncing every directory entry and reopening every new table.** Both are cheap insurance against silent loss. They add fsyncs and reads to each flush and compaction, and profiling should show whether they matter.

**What I would change**

- **Software CRC32C is the largest read cost I've measured so far.** It is byte-at-a-time, and replacing it with hardware CRC in a test build took warm `readrandom` from about 94k to 317k ops/s. Profiling comes next, and it should confirm this before I fix it.
- **The MANIFEST fsync happens under `mutex_`.** While an edit syncs, new reads and writes wait, because both need that lock to start. LevelDB drops its mutex for this write, and I would too.
- **Flushes always go to L0.** LevelDB can place a memtable's file directly in a deeper level when nothing overlaps. Mine can sit with up to 3 L0 files, which adds probes to reads, and one more flush can then trigger a cascade of compactions.
- **One background thread.** A long compaction delays other compactions, although flushes can interrupt it. Separate flush and compaction threads would reduce write stalls.
- **A failed write requires a reopen.** The sticky error is safe but blunt. A long-running service would need a way to recover without restarting.
- **No compression.** It keeps the format simple, but data takes more disk and every read block costs more cache. LZ4 is the obvious stretch goal, and the block trailer already has a type byte for it.
