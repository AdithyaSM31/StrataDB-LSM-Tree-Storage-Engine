# StrataDB

An embedded key-value storage engine in C++20, built as a log-structured merge tree:
write-ahead log, skiplist memtable, SSTables with bloom filters, and leveled compaction.

> Status: **Week 5: benchmarks, profiling and write-up.** Standalone benchmark harness with LevelDB adapter in place. See [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Build and test

Requires CMake ≥ 3.25, Ninja, and GCC ≥ 12 or Clang ≥ 16.

```bash
cmake --preset debug          # or: asan, tsan, release, bench
cmake --build --preset debug
ctest --preset debug          # add -L unit or -L slow to filter
```

| Preset    | What it is for                                                     |
|-----------|--------------------------------------------------------------------|
| `debug`   | Day-to-day development, `STRATA_DCHECK` on                         |
| `asan`    | AddressSanitizer + UndefinedBehaviorSanitizer, `-Werror`           |
| `tsan`    | ThreadSanitizer, `-Werror`                                         |
| `release` | `-O2`, debug checks compiled out, `-Werror`                        |
| `bench`   | `-O2 -g -fno-omit-frame-pointer` for `perf` flame graphs, builds `strata_bench`, fetches LevelDB 1.23 |

CI runs `asan`, `tsan` and `release` (GCC and Clang) plus a `clang-format` check on every push.

Format before committing:

```bash
cmake --build --preset debug --target format
```

Run clang-tidy as part of a build:

```bash
cmake --preset debug -B build/tidy -DSTRATA_CLANG_TIDY=ON && cmake --build build/tidy
```

> **WSL2:** keep the repo inside the Linux filesystem (e.g. `~/strata`), not under `/mnt/c`.
> For TSan on recent kernels you may need `sudo sysctl -w vm.mmap_rnd_bits=28`.

## Repository layout

```
include/strata/   public API
src/util/         coding, crc32c, arena, hash, bloom, cache, histogram, random
src/env/          PosixEnv, FaultInjectionEnv
src/memtable/     skiplist, memtable
src/log/          WAL / MANIFEST record writer and reader
src/table/        blocks, SSTable builder and reader, iterators
src/db/           DBImpl, versions, compaction, DBIter
tests/            one *_test.cc per unit, plus model / crash / kill tests
bench/            strata_bench and engine adapters
tools/            sst_dump, manifest_dump
docs/             DESIGN.md, BENCHMARKS.md, ROADMAP.md
```

Adding code needs no CMake edits:

- any `src/**/*.cc` is compiled into the `strata` library;
- each `tests/*_test.cc` becomes its own test binary (`model_test`, `crash_test` and
  `kill_test` are labelled `slow`, everything else `unit`);
- each `tools/*.cc` becomes an executable.

Re-run the configure step after adding a new file, or just build — the globs are checked on
every build.

## Benchmarks (`strata_bench`)

StrataDB includes `strata_bench`, a standalone benchmarking harness that evaluates identical workloads against StrataDB and LevelDB (pinned tag `1.23`).

### Build

```bash
cmake --preset bench
cmake --build --preset bench --target strata_bench
```

### Run

Run all workloads across both engines:

```bash
./build/bench/strata_bench --num=1000000 --threads=1
```

Quick smoke run:

```bash
./build/bench/strata_bench --num=10000 --engine=both --benchmarks=fillseq,readrandom --threads=2
```

Print each engine's stats, including write amplification, after a fill. Stats are printed after write workloads, once compactions finish, because their counters reset when the database reopens:

```bash
./build/bench/strata_bench --num=1000000 --benchmarks=fillrandom --stats
```

`--num` and the other count flags accept `K`, `M` and `G` (`--num=10M`). YCSB workloads can be tuned with `--ycsb_ops=N` (defaults to `--num`) and `--zipf_theta=F` (skew parameter in `(0, 1)`, default `0.99`). Run `strata_bench --help` for every option.

### Supported Workloads

- **Writes**: `fillseq`, `fillrandom`, `overwrite`, `fillsync`
- **Reads**:
  - `readrandom`: point lookups of keys that exist.
  - `readmissing`: lookups of keys that don't exist but fall inside the stored key range, so they exercise the bloom filters.
  - `readseq`: walks the whole key space with an iterator.
  - `seekrandom`: one seek per operation.
  - `scan`: a seek plus 100 entries per operation.
- **YCSB (Zipfian skew)**:
  - `ycsbc`: 100% reads (read-only).
  - `ycsbb`: 95% reads, 5% updates.
  - `ycsba`: 50% reads, 50% updates.
- **Mixed**: `readwhilewriting`: reader threads alongside one writer held to `--write_rate` (default 10,000 writes/s, 0 for no limit). The rate the writer reached is reported with the row.

### Datasets

Read workloads reuse the dataset left by an earlier fill only if it was built with the same `--num`, `--value_size`, `--write_buffer_size`, `--block_size`, `--bloom_bits` and `--threads`; otherwise it is rebuilt with `fillrandom`. `--cache_size` and `--max_open_files` can vary over one dataset. Workloads that change the data mark the dataset invalid first.

Before each read workload the database is reopened, leaving the block cache cold, and the harness waits until no level is over its compaction target. The CSV records which fill made the dataset (`dataset`) and the files per level at the start (`levels_at_start`). LevelDB also compacts in response to reads, which StrataDB doesn't, so its shape can change during one read workload and differ at the start of the next.

### Failures

Every read is checked: keys must be found with the right value size, missing keys must be missing, and seeks and scans must land where expected. If any operation fails, the row shows the error count and is left out of the CSV, and `strata_bench` exits 1. Bad arguments exit 2. A database that fails to open is reported and never deleted.

### Fairness

- **Compression**: off on both engines, since StrataDB doesn't compress.
- **Checksums**: every block read is verified on both engines, since StrataDB can't turn that off. LevelDB's default is not to verify, so the adapter sets `verify_checksums` and `paranoid_checks`.
- **Block cache**: `--cache_size=0` disables it on both engines.
- **Key sequences**: the same seeds give both engines the same keys in the same order.
- **Latency recording**: each thread has its own `Histogram` on its own cache lines; they are merged after the run.

Each run prints its options, the commit and LevelDB's optional dependencies. Results are appended to `bench/results/benchmarks.csv` with a run ID and the options, so repeat runs can be told apart. YCSB rows also record the skew, the read and update counts, and each side's latency percentiles.

## Durability

_TODO: the sync=false / sync=true table from the design doc, once the WAL exists._

## Results

_TODO: filled in from [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) in Week 5._

