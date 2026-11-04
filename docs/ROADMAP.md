# Roadmap

Five weeks starting Monday 2 November 2026, plus a buffer week ending 13 December.
Tick items off as they land; each one should arrive with its tests.

## Week 0: scaffolding

- [x] Repo, CMake, GoogleTest, clang-format, clang-tidy config
- [x] CI with ASan/UBSan, TSan and release jobs
- [x] `STRATA_DCHECK` / `STRATA_DEBUG_CHECKS`

## Week 1 (2–8 Nov): foundation, memtable and WAL

**Milestone:** Put, Get and Delete work and survive a restart, with no SSTables yet.

- [x] `Status`, varint/fixed coding, CRC32C, arena, hash, random
- [x] `Env` interface and `PosixEnv` (`pread`, `write`, `fdatasync`, `rename`, directory fsync, `flock`)
- [x] Skiplist with atomic next-pointers, including the concurrent reader test
- [x] Internal key format, comparator, MemTable and MemTable iterator
- [x] WriteBatch (8-byte sequence, 4-byte count, then records)
- [x] Log writer and reader, with truncation and bit-flip tests
- [x] Minimal `DBImpl`: write goes to WAL then memtable; Open replays the WAL
- [x] Delete `tests/smoke_test.cc` and `src/version.cc` once real code replaces them

## Week 2 (9–15 Nov): SSTables and flush

**Milestone:** data larger than the memtable is written to L0 and read back correctly after reopen.

- [x] Block builder and block reader with prefix compression and restart points
- [x] Bloom filter policy, with the measured false-positive rate test
- [x] Table builder and table reader: filter, metaindex, index, footer, trailers
- [x] Two-level iterator (index block to data blocks)
- [x] Flush the immutable memtable to an L0 SSTable on a background thread
- [x] VersionEdit, VersionSet, MANIFEST and `CURRENT`, with replay on Open
- [x] Get across memtable, immutable memtable and L0 files
- [x] `sst_dump` tool

## Week 3 (16–22 Nov): leveled compaction and iterators

**Milestone:** a 10M-key `fillrandom` finishes with L0 under the stop trigger, and the model test passes.

- [x] Level scores, compact pointers, input selection and expansion, trivial move
- [x] Compaction merge loop with drop rules for old versions and tombstones
- [x] Output splitting by size and by grandparent overlap
- [x] Version refcounting and obsolete file deletion
- [x] Write backpressure: slowdown at 8 L0 files, stop at 12
- [x] Merging iterator (forward and reverse) and `DBIter`
- [x] Snapshots: list of live snapshots, oldest-snapshot rule in compaction
- [x] Model-based randomized test with reopen, snapshots and `CompactRange`

## Week 4 (23–29 Nov): concurrency, caching and crash safety

**Milestone:** TSan clean, and 1,000 kill -9 iterations pass in CI.

- [x] Group commit writer queue and `sync` handling
- [x] Sharded LRU block cache and table cache
- [x] Sticky background error handling
- [x] `FaultInjectionEnv` with `SimulateCrash()` and error injection
- [x] Crash test (synced writes present, state is a prefix of history)
- [x] Fork-and-SIGKILL harness
- [x] SSTable corruption test
- [x] `GetProperty("strata.stats")`: per-level files, bytes, compaction counts, write amplification

## Week 5 (30 Nov–6 Dec): benchmarks, profiling and write-up

**Milestone:** BENCHMARKS.md with reproducible numbers against LevelDB, and a README you would send to a recruiter.

- [x] `strata_bench` with StrataDB and LevelDB adapters and all workloads
- [ ] YCSB A/B/C with Zipfian keys
- [ ] Run on an EC2 instance with local NVMe; record hardware and flags
- [ ] Profile with `perf` and flame graphs, fix the top hotspot, re-measure
- [ ] Charts for the five experiments
- [ ] DESIGN.md: formats, compaction rules, durability table, trade-offs you chose
- [ ] README: one diagram, a results table, how to build and run tests

## Buffer (7–13 Dec)

Whatever slipped, or one stretch goal. If short on time, cut YCSB and experiments 4 and 5
first. **Never cut the crash tests.**
