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
| `bench`   | `-O3 -DNDEBUG`, builds `strata_bench` and fetches LevelDB 1.23     |

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

Inspect engine stats after run:

```bash
./build/bench/strata_bench --num=10000 --engine=strata --benchmarks=fillrandom,readrandom --stats
```

### Supported Workloads

- **Writes**: `fillseq`, `fillrandom`, `overwrite`, `fillsync`
- **Reads**: `readrandom`, `readmissing`, `readseq`, `seekrandom`, `scan`
- **Mixed**: `readwhilewriting` (concurrent writer thread + reader threads)

### Fairness Guarantees

- **No Compression**: LevelDB compression is explicitly disabled (`leveldb::kNoCompression`) to ensure fair comparison since StrataDB does not compress.
- **Identical PRNG Streams**: Worker threads share deterministic seed offsets across engines so both process identical key sequences.
- **Zero Measurement Contention**: Each worker thread times individual operations into its own log-scale `Histogram` without locks; histograms are merged after completion.
- **Results Export**: Formatted summary table printed to stdout and full metrics exported to CSV under `bench/results/benchmarks.csv`.

## Durability

_TODO: the sync=false / sync=true table from the design doc, once the WAL exists._

## Results

_TODO: filled in from [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) in Week 5._

