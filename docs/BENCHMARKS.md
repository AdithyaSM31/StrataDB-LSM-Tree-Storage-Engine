# Benchmarks

> **LevelDB is ahead on most workloads.** These are 10M-key results from 6 November 2026, measured before any profiling or fixes. With both engines reading through `pread`, LevelDB is 2.5–3.2x faster on in-memory reads other than `readmissing` and 12–29% faster on the same reads from disk. It is also 2.2–2.3x faster on random writes. The two tie on `fillsync` and on `readmissing` in memory, and StrataDB leads `readmissing` on disk by 11%.

Earlier versions of this page reported large StrataDB wins, including a 34x lead on `readmissing` and a reversal of the gap on disk. **Those claims are withdrawn.** They came from 1M-key runs on WSL2, with a flaw in how the missing keys were generated and wrong write amplification figures. The fixes are in commit `d3ea10a`.

## Environment

| Item | Value |
|---|---|
| Machine | Azure `Standard_D4ds_v4`, Malaysia West |
| CPU | Intel Xeon Platinum 8272CL, 2 cores and 4 threads |
| RAM | 16 GiB |
| Disk | 150 GiB local temp SSD, not exposed as NVMe; Microsoft lists a 250 MBps limit, and `fio` measured 236 MiB/s (about 247 MB/s) at most and 8,264 IOPS for 4 KB reads at queue depth 1 |
| Filesystem | ext4, `noatime` |
| Read-ahead | 128 KB during runs (the image's default is 4 MB) |
| OS | Ubuntu 24.04, kernel 7.0.0-1017-azure |
| Compiler | GCC 13.3, `-O2 -g -DNDEBUG -fno-omit-frame-pointer` |
| LevelDB | 1.23, without crc32c, snappy or tcmalloc |
| StrataDB | commit `d3ea10a` |

Each run also saves its own `environment.txt` with the results. This series can land on different Intel CPUs, so compare only numbers from the same CPU model.

The run used an uncommitted source tree that was later committed as `d3ea10a`, so the `commit` column in the CSVs and `environment.txt` shows its parent, `2cdc2eb`. `docs/results/source_tree.txt` records the tree that was measured, `8b18cbe`, which is the tree of `d3ea10a`.

## How to reproduce

Use Ubuntu 24.04, because the build needs CMake 3.25 or newer.

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git fio sysstat tmux
```

Check that the temp disk is the roughly 150 GiB device mounted at `/mnt`:

```bash
lsblk -o NAME,SIZE,TRAN,MODEL,MOUNTPOINT
```

```bash
sudo mount -o remount,noatime /mnt
```

```bash
sudo mkdir -p /mnt/strata && sudo chown "$USER:$USER" /mnt/strata
```

Do a quick dry run with one pass over 1M keys, then the full run inside `tmux`:

```bash
./bench/run_benchmarks.sh --dry-run --runs=1 --db-dir=/mnt/strata/dry --output-dir=results-dry
```

```bash
./bench/run_benchmarks.sh --num=10M --sync-num=100K --db-dir=/mnt/strata/data --output-dir=docs/results
```

The full run took about 14 hours and 15 minutes here: 2 hours for the memory tier and 4 hours for each of the 3 disk runs. The script needs passwordless `sudo`. Any failed or stuck workload stops it with a non-zero exit, and it writes its checks and tables to `summary.md`. To recheck existing results without running anything:

```bash
./bench/run_benchmarks.sh --report-only --output-dir=docs/results
```

To see what the disk was doing during each disk-tier workload:

```bash
python3 bench/iostat_by_workload.py docs/results
```

## Method

Keys are 16 bytes and values 100 bytes, with no compression on either engine and checksums verified on every block read. Each number is the median of 3 runs on the same snapshot, with the engine order alternating between runs.

- **Memory tier:** no memory limit and a warm page cache, so disk reads don't affect the numbers.
- **Disk tier:** a 512 MB cgroup limit on a dataset of about 1.2 GB, with the page cache dropped before each run, so reads reach the disk.

Read workloads run LevelDB twice, because it reads through `mmap` by default and StrataDB uses `pread`:

- **LevelDB:** the default, with `mmap`. Its data blocks don't use its block cache, and its index and filter blocks can be evicted under memory pressure.
- **LevelDB no mmap:** `pread`, the same read path as StrataDB. This is the cleaner engine comparison.

### Known limits

- The disk's throughput limit caps default LevelDB on `readrandom`, `seekrandom`, `scan` and `ycsbc` in the disk tier. Those four numbers measure the Azure limit, as [Disk I/O](#disk-io) shows, so they shouldn't be read as LevelDB's speed.
- `fillsync` measures this temp disk's flush speed, which may not reflect durable storage elsewhere.
- LevelDB compacts in response to reads and StrataDB doesn't, so LevelDB's LSM shape can change during a read workload.
- YCSB records here are 116 bytes, not YCSB's standard 1 KB, so they line up with the other workloads.
- YCSB keys use Gray's Zipfian approximation at theta 0.99, scrambled with `fnv1a(rank) % n`. Only about 46% of keys can be drawn at 1M keys, where a random hash would reach 63%.

## Results

All figures are medians of 3 runs. The raw data, per-run logs and generated tables are in [`docs/results/`](results/). Ratios above 1 mean LevelDB is faster.

### Memory tier

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | LevelDB ÷ StrataDB | No mmap ÷ StrataDB |
|---|---|---|---|---|---|---|
| `fillseq`* | 10,000,000 | 472,160 | 556,157 | – | 1.18 | – |
| `fillrandom` | 10,000,000 | 94,916 | 211,550 | – | 2.23 | – |
| `overwrite` | 10,000,000 | 73,149 | 165,687 | – | 2.27 | – |
| `fillsync` | 100,000 | 4,556 | 4,498 | – | 0.99 | – |
| `readseq` | 10,000,000 | 1,509,695 | 4,104,609 | – | 2.72 | – |
| `readrandom` | 10,000,000 | 59,160 | 237,564 | 162,276 | 4.02 | 2.74 |
| `readmissing` | 10,000,000 | 583,172 | 582,019 | 566,598 | 1.00 | 0.97 |
| `seekrandom` | 10,000,000 | 25,443 | 109,420 | 80,688 | 4.30 | 3.17 |
| `scan` | 1,000,000 | 9,569 | 40,369 | 30,882 | 4.22 | 3.23 |
| `ycsbc` | 10,000,000 | 85,015 | 254,062 | 211,801 | 2.99 | 2.49 |
| `ycsbb` | 10,000,000 | 108,426 | 270,025 | – | 2.49 | – |
| `ycsba` | 10,000,000 | 156,226 | 289,941 | – | 1.86 | – |
| `readwhilewriting`† | 10,000,000 | 57,544 | 221,570 | – | 3.85 | – |

\* `fillseq` is the only workload with a spread over 10%. LevelDB's three runs were 452,913, 568,312 and 556,157 ops/s, so treat this ratio as rough. Every other workload stayed within 5.3%.

† StrataDB's run lasted longer, so its background writer wrote 3.9x as much data during it. This makes the comparison harder on StrataDB.

Latency in microseconds:

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|---|---|---|---|---|---|---|
| `fillrandom` | 2.16 | 1.95 | – | 10.26 | 5.61 | – |
| `overwrite` | 2.15 | 1.96 | – | 1,001.95 | 6.09 | – |
| `fillsync` | 208.00 | 211.36 | – | 430.73 | 429.75 | – |
| `readrandom` | 16.31 | 3.93 | 5.88 | 31.53 | 6.68 | 9.32 |
| `readmissing` | 1.22 | 1.42 | 1.44 | 16.89 | 3.95 | 7.07 |
| `seekrandom` | 35.34 | 8.04 | 11.45 | 64.28 | 16.01 | 22.44 |
| `scan` | 101.63 | 23.50 | 31.22 | 141.25 | 35.67 | 46.60 |
| `ycsbc` | 15.56 | 3.76 | 5.55 | 30.95 | 6.42 | 8.40 |

### Disk tier

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | LevelDB ÷ StrataDB | No mmap ÷ StrataDB |
|---|---|---|---|---|---|---|
| `readrandom` | 10,000,000 | 8,435 | 3,726‡ | 9,454 | 0.44‡ | 1.12 |
| `readmissing` | 10,000,000 | 368,622 | 263,586 | 331,406 | 0.72 | 0.90 |
| `seekrandom` | 10,000,000 | 6,377 | 3,134‡ | 8,069 | 0.49‡ | 1.27 |
| `scan` | 1,000,000 | 2,798 | 2,930‡ | 3,616 | 1.05‡ | 1.29 |
| `ycsbc` | 10,000,000 | 18,152 | 6,729‡ | 20,773 | 0.37‡ | 1.14 |

‡ Limited by the disk, not the engine. See [Disk I/O](#disk-io).

Every disk workload stayed within 2.1% across its runs. `readmissing` takes under 40 seconds, because it rarely touches the disk, so it's the least settled of these.

Latency in microseconds:

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|---|---|---|---|---|---|---|
| `readrandom` | 135.68 | 187.01 | 124.29 | 318.55 | 644.18 | 278.33 |
| `readmissing` | 1.34 | 1.58 | 1.59 | 18.38 | 5.50 | 7.95 |
| `seekrandom` | 162.98 | 206.79 | 134.53 | 397.92 | 830.02 | 320.09 |
| `scan` | 369.13 | 228.46 | 291.68 | 889.02 | 900.84 | 776.08 |
| `ycsbc` | 18.33 | 7.45 | 8.29 | 280.30 | 518.38 | 252.70 |

### Disk I/O

`iostat` sampled the disk every 5 seconds through the disk tier. With no database running, `fio` tops out at 236 MiB/s on this disk ([`fio_cap.txt`](results/fio_cap.txt)).

| Workload | Engine | Read MiB/s | Samples near the limit | Reads per op | KB per op |
|---|---|---|---|---|---|
| `readrandom` | StrataDB | 60.6 | 0% | 0.83 | 7.4 |
| | LevelDB | 235.6 | 100% | 0.79 | 64.7 |
| | LevelDB no mmap | 68.4 | 0% | 0.83 | 7.4 |
| `seekrandom` | StrataDB | 47.5 | 0% | 0.87 | 7.6 |
| | LevelDB | 235.6 | 100% | 1.35 | 77.0 |
| | LevelDB no mmap | 60.3 | 0% | 0.88 | 7.7 |
| `scan` | StrataDB | 102.6 | 0% | 2.42 | 37.6 |
| | LevelDB | 235.1 | 100% | 1.44 | 82.2 |
| | LevelDB no mmap | 133.1 | 0% | 2.43 | 37.7 |
| `ycsbc` | StrataDB | 51.7 | 0% | 0.34 | 2.9 |
| | LevelDB | 235.6 | 100% | 0.46 | 35.8 |
| | LevelDB no mmap | 60.2 | 0% | 0.34 | 3.0 |

"Near the limit" means at least 95% of 236 MiB/s. Two things follow from this table:

- **Default LevelDB's disk numbers are set by the Azure limit.** It reads 2–12x as much per operation as the `pread` rows, because page faults on `mmap` pull in read-ahead, and it holds at the limit for the whole run. Its throughput barely changes between runs either: 3,134 ops/s on `seekrandom` in all three. StrataDB's 2–2.7x lead over this row on `readrandom`, `seekrandom` and `ycsbc` isn't an engine result, and a faster disk would change it.
- **StrataDB and LevelDB no mmap do the same I/O.** Their reads per operation and KB per operation match, so the disk-tier gap isn't caused by extra I/O. See below.

## Amplification

Table bytes written per byte of keys and values written, with one denominator for both engines. The WAL isn't counted.

| Workload | User data | StrataDB | LevelDB |
|---|---|---|---|
| `fillseq` | 1,106.3 MiB | 1.00 | 1.00 |
| `fillrandom` | 1,106.3 MiB | 9.21 | 9.05 |
| `overwrite` | 1,106.3 MiB | 11.39 | 10.95 |

The two engines are within about 4% of each other, so compaction volume doesn't explain the gap in write throughput.

These three aren't comparable between engines, because read-triggered compaction affects LevelDB and, for `readwhilewriting`, the amount written differs:

| Workload | StrataDB user data | LevelDB user data | StrataDB | LevelDB |
|---|---|---|---|---|
| `ycsba` | 553.3 MiB | 553.3 MiB | 5.79 | 11.75 |
| `ycsbb` | 55.3 MiB | 55.3 MiB | 5.10 | 134.42 |
| `readwhilewriting` | 192.2 MiB | 49.9 MiB | 3.77 | 21.45 |

## Experiments

Results are pending for all five. The commands below assume `--db` points at the disk being measured.

### 1. Bloom bits per key vs `readmissing`

Each bits-per-key value needs its own dataset.

```bash
./build/bench/strata_bench --num=10M --bloom_bits=10 --benchmarks=fillrandom,readmissing --db=/mnt/strata/exp1 --csv=exp1.csv
```

### 2. `fillsync` throughput, 1–16 threads, with and without group commit

LevelDB has no switch for this, so it only appears with group commit on.

```bash
./build/bench/strata_bench --engine=strata --group_commit=0 --threads=4 --sync_num=100K --benchmarks=fillsync --db=/mnt/strata/exp2 --csv=exp2.csv
```

### 3. Throughput and L0 file count over a 10M `fillrandom`

`--timeseries` writes one row per interval with the time, the operations done so far, the throughput and the files per level.

```bash
./build/bench/strata_bench --num=10M --benchmarks=fillrandom --timeseries=exp3.csv --db=/mnt/strata/exp3 --csv=exp3_summary.csv
```

### 4. Block cache size vs `readrandom` hit rate and p99

Each CSV row records `cache_hits` and `cache_misses`. Use LevelDB with `--leveldb_mmap=0` here, because with `mmap` its data blocks never enter the cache and its hit rate is always 0. Compaction lookups count too, so LevelDB's figures include its read-triggered compactions.

```bash
./build/bench/strata_bench --num=10M --cache_size=64MB --benchmarks=readrandom --db=/mnt/strata/exp4 --csv=exp4.csv
```

### 5. Profiling pass: before and after

Azure doesn't expose hardware counters on this VM size, so `perf` has to sample `cpu-clock`. Both builds will be measured on the same VM.

## Where LevelDB wins, and why

Profiling hasn't been done yet, so the causes below are what the data points to, not what has been confirmed.

**Reads spend more CPU per operation.** The extra time StrataDB takes over LevelDB no mmap is nearly the same with or without the disk, and it matches the identical I/O shown above:

| Workload | Extra time per op, memory | Extra time per op, disk |
|---|---|---|
| `readrandom` | 10.7 µs | 12.7 µs |
| `seekrandom` | 26.3 µs | 32.2 µs |
| `scan` | 71.4 µs | 79.9 µs |
| `ycsbc` | 7.1 µs | 6.9 µs |

The likely main cause is StrataDB's byte-at-a-time software CRC32C. A test build with hardware CRC took warm `readrandom` from about 94k to 317k ops/s on a different machine, using 1M keys. LevelDB is built here without its `crc32c` library, so it also uses software CRC. The profiling pass needs to confirm this on this VM.

**`mmap` helps LevelDB further in memory.** Default LevelDB is 1.2–1.5x faster than its own `pread` mode on memory-tier reads other than `readmissing`, and StrataDB has no equivalent read path.

**Random writes lose in the tail.** Median write latency is close (2.16 vs 1.95 µs on `fillrandom`), but mean latency is 10.33 vs 4.53 µs. On `overwrite`, StrataDB's p99 is 1,002 µs against LevelDB's 6 µs. That looks like the 1 ms L0 slowdown delay reaching more than 1% of StrataDB's writes, which would mean compaction isn't keeping up as well. Experiment 3 will test this.

**StrataDB's lead on disk `readmissing` is small.** It leads by 11% on throughput with a better p50 (1.34 vs 1.59 µs), but its p99 is worse (18.38 vs 7.95 µs).
