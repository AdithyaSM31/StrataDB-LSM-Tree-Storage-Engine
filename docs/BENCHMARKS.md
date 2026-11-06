# Benchmarks

> **Results are pending.** The full 10M-key run is in progress on the machine described below, and its numbers will go here when they have been checked.

Earlier versions of this page reported large StrataDB wins, including a 34x lead on `readmissing` and a reversal of the gap on disk. **Those claims are withdrawn.** They came from 1M-key runs on WSL2, with a flaw in how the missing keys were generated and wrong write amplification figures. The fixes are in commit `d3ea10a`.

## Environment

| Item | Value |
|---|---|
| Machine | Azure `Standard_D4ds_v4`, Malaysia West |
| CPU | Intel Xeon Platinum 8272CL, 2 cores and 4 threads |
| RAM | 16 GiB |
| Disk | 150 GiB local temp SSD, not exposed as NVMe; Microsoft lists best-case limits of 19,000 read IOPS and 250 MBps |
| Filesystem | ext4, `noatime` |
| Read-ahead | 128 KB during runs (the image's default is 4 MB) |
| OS | Ubuntu 24.04, kernel 7.0.0-1017-azure |
| Compiler | GCC 13.3, `-O2 -g -DNDEBUG -fno-omit-frame-pointer` |
| LevelDB | 1.23, without crc32c, snappy or tcmalloc |
| StrataDB | commit `d3ea10a` |

Each run also saves its own `environment.txt` with the results. This series can land on different Intel CPUs, so compare only numbers from the same CPU model.

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

The script needs passwordless `sudo`. Any failed or stuck workload stops it with a non-zero exit, and it writes its checks and tables to `summary.md`. To recheck existing results without running anything:

```bash
./bench/run_benchmarks.sh --report-only --output-dir=docs/results
```

## Method

Keys are 16 bytes and values 100 bytes, with no compression on either engine and checksums verified on every block read. Each number is the median of 3 runs on the same snapshot, with the engine order alternating between runs.

- **Memory tier:** no memory limit and a warm page cache, so disk reads don't affect the numbers.
- **Disk tier:** a 512 MB cgroup limit on a dataset of about 1.2 GB, with the page cache dropped before each run, so reads reach the disk.

Read workloads run LevelDB twice, because it reads through `mmap` by default and StrataDB uses `pread`:

- **LevelDB:** the default, with `mmap`. Its data blocks don't use its block cache, and its index and filter blocks can be evicted under memory pressure.
- **LevelDB no mmap:** `pread`, the same read path as StrataDB. This is the cleaner engine comparison.

### Known limits

- The disk's throughput cap can limit the `mmap` rows, because they read much more data. `iostat_disk.log` shows when a run sits near the cap.
- `fillsync` measures this temp disk's flush speed, which may not reflect durable storage elsewhere.
- LevelDB compacts in response to reads and StrataDB doesn't, so LevelDB's LSM shape can change during a read workload.
- YCSB records here are 116 bytes, not YCSB's standard 1 KB, so they line up with the other workloads.
- YCSB keys use Gray's Zipfian approximation at theta 0.99, scrambled with `fnv1a(rank) % n`. Only about 46% of keys can be drawn at 1M keys, where a random hash would reach 63%.

## Results

Pending.

## Amplification

Pending. Figures will be table bytes written per byte of keys and values written, with one denominator for both engines. The WAL isn't counted. `ycsba`, `ycsbb` and `readwhilewriting` will be shown separately, because they aren't comparable between engines: read-triggered compaction affects LevelDB, and the amount written differs.

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

Pending profiling. Early 1M-key runs show LevelDB ahead on most workloads when both engines use `pread`.
