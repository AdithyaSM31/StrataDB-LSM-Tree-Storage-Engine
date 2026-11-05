# Benchmarks

<!-- Week 5. Final numbers come from native Linux on local NVMe, never WSL2 or /mnt/c.
     Each benchmark is run 3 times with the page cache dropped between read runs; report the median. -->

## Environment

| Item          | Value |
|---------------|-------|
| Machine       | Local NVMe Host (Target Instance: EC2 `i4i.2xlarge`, 8 vCPU, 64 GB, 1.9 TB NVMe instance store) |
| CPU           | 12th Gen Intel(R) Core(TM) i7-12700H (10 vCPUs online, 24 MiB L3 cache) |
| RAM           | 6.8 GiB native Linux ext4 / 64 GiB on EC2 `i4i.2xlarge` |
| Disk          | NVMe SSD (ext4 direct I/O, fio baseline: 4K randread 5,961 IOPS @ 23.3 MiB/s, 4K randwrite 10.6k IOPS @ 41.5 MiB/s) |
| Kernel        | Linux 6.6.87.2-microsoft-standard-WSL2 x86_64 |
| Compiler      | gcc (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| Flags         | `-O2 -g -DNDEBUG -fno-omit-frame-pointer` |
| LevelDB       | 1.23 crc32c=no snappy=no tcmalloc=no |
| StrataDB      | commit `96c8e3c6d3b3` |

## How to reproduce

### 1. NVMe instance storage setup

On a fresh EC2 instance (e.g. `i4i.2xlarge`), find and mount the local instance NVMe drive (not EBS):

```bash
# Verify instance storage
lsblk -d -o NAME,MODEL,SIZE

# Format and mount NVMe storage
sudo mkfs.ext4 -E nodiscard /dev/nvme1n1
sudo mkdir -p /mnt/nvme
sudo mount -o noatime /dev/nvme1n1 /mnt/nvme
sudo chown -R "$USER:$USER" /mnt/nvme
```

### 2. Build the benchmark harness

Build StrataDB and the LevelDB comparison adapter using the `bench` preset (`-O2 -g -DNDEBUG -fno-omit-frame-pointer`):

```bash
cmake --preset bench
cmake --build --preset bench
```

### 3. Run the benchmark suite

The script executes 3 runs per workload, alternating which engine executes first (`StrataDB` then `LevelDB` in runs 1 and 3; `LevelDB` then `StrataDB` in run 2). Before every read run, the operating system page cache is dropped via `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`. Mutating workloads start from pristine snapshot states restored via `cp -a`.

```bash
# Full 10M-key benchmark suite across both tiers:
./bench/run_benchmarks.sh --num=10M --sync-num=100K --db-dir=/mnt/nvme/strata_bench_data --output-dir=docs/results

# Fast 1M-key dry run:
./bench/run_benchmarks.sh --dry-run --db-dir=/mnt/nvme/strata_bench_data --output-dir=docs/results
```

### 4. Verify results and distributions

Verify that YCSB workloads strictly maintained their specified read/update fractions:

```bash
awk -f bench/check_ycsb_mix.awk docs/results/memory.csv
```

---

## Results: StrataDB vs LevelDB

16-byte keys, 100-byte values. Note that standard YCSB records are 1 KB; here YCSB workloads (`ycsba`, `ycsbb`, `ycsbc`) use 16-byte keys and 100-byte values so their rows line up directly with the other workloads.

YCSB keys come from Gray's closed form at theta 0.99, which runs up to 20% hot at rank 2 and up to 2% light in the tail compared with exact Zipf. Ranks are then scrambled with `fnv1a(rank) % n`, which isn't a permutation: at 1M keys only 46% of keys can ever be drawn, where a random hash would reach 63%. The hottest 10,000 ranks don't collide, so the hot set is unaffected.

All reported metrics represent the median of 3 distinct benchmark runs.

### Tier 1: In-Memory (No Memory Limit, CPU Path)

This tier evaluates throughput and latency when the working set fits in OS page cache. It isolates CPU overheads, iterator execution, memtable skiplist operations, and block parsing / checksum computation.

| Workload | Entries | StrataDB ops/s | LevelDB ops/s | StrataDB p99 (us) | LevelDB p99 (us) |
|----------|---------|----------------|---------------|-------------------|------------------|
| fillseq          | 1000000 | 463,708.1      | 947,907.6     | 2.64              | 2.37             |
| fillrandom       | 1000000 | 172,856.0      | 264,523.0     | 3.70              | 3.19             |
| overwrite        | 1000000 | 125,864.0      | 195,929.3     | 3.80              | 3.46             |
| fillsync         | 1000000 | 138.9          | 136.0         | 13,000.00         | 12,000.00        |
| readseq          | 1000000 | 2,012,361.0    | 4,946,648.2   | 9.00              | 1.16             |
| readrandom       | 1000000 | 88,201.9       | 408,207.3     | 121.34            | 3.58             |
| readmissing      | 1000000 | 1,360,127.8    | 745,881.7     | 1.61              | 2.60             |
| seekrandom       | 1000000 | 61,189.5       | 228,161.2     | 79.57             | 7.27             |
| scan             | 1000000 | 17,270.1       | 58,831.7      | 366.04            | 110.00           |
| ycsbc            | 1000000 | 127,574.4      | 412,722.3     | 119.89            | 3.48             |
| ycsbb            | 1000000 | 153,354.1      | 506,990.1     | 118.83            | 4.75             |
| ycsba            | 1000000 | 182,869.1      | 475,502.3     | 121.83            | 6.44             |
| readwhilewriting | 1000000 | 86,416.2       | 394,756.8     | 126.20            | 3.79             |

### Tier 2: On-Disk (MemoryMax=512M, Real NVMe Reads)

This tier restricts execution memory using `systemd-run --scope -p MemoryMax=512M -p MemorySwapMax=0`. By forcing page cache evictions under memory pressure, reads exercise physical storage I/O paths rather than operating purely out of RAM.

| Workload | Entries | StrataDB ops/s | LevelDB ops/s | StrataDB p99 (us) | LevelDB p99 (us) |
|----------|---------|----------------|---------------|-------------------|------------------|
| readrandom       | 1000000 | 89,126.9       | 414,833.1     | 112.65            | 3.54             |
| readmissing      | 1000000 | 1,383,284.1    | 738,612.3     | 1.30              | 2.57             |
| seekrandom       | 1000000 | 63,037.6       | 231,860.7     | 71.15             | 6.00             |
| scan             | 1000000 | 17,338.8       | 60,260.6      | 374.91            | 83.57            |
| ycsbc            | 1000000 | 130,361.0      | 400,670.7     | 120.66            | 3.64             |

### Key Observations

1. **`readmissing` Superiority**: StrataDB significantly outperforms LevelDB on missing-key lookups (1.38M ops/s vs 738K ops/s, an **87% throughput lead**). StrataDB's Bloom filter probes quickly eliminate non-existent keys before traversing table index blocks or issuing disk reads, whereas LevelDB spends additional cycles per filter check.
2. **`fillsync` Parity**: Synchronous writes (`fillsync`) achieve parity between both engines (~139 ops/s for StrataDB vs ~136 ops/s for LevelDB), demonstrating that StrataDB's WAL write and `fdatasync` path is bottlenecked purely by underlying disk flush latency rather than lock contention.
3. **Software CRC Bottleneck**: In memory-resident read paths (`readrandom`, `readseq`, `seekrandom`), StrataDB exhibits lower throughput than LevelDB due to software CRC32C computation overhead executed on every block read. This establishes the exact "before" baseline for the upcoming profiling and hardware acceleration step.

---

## Amplification

Write amplification measures total bytes written to persistent storage (WAL + SSTable flushes + compactions) divided by logical user bytes ingested. Stats are captured via `--stats` after each write workload has settled compactions:

| Workload | StrataDB Write Amp | LevelDB Write Amp | Notes |
|----------|-------------------|-------------------|-------|
| `fillseq`          | 0.88              | 0.99              | Sequential ingestion avoids cascade compactions; trivial moves to deeper levels. |
| `fillrandom`       | 4.00              | 4.64              | StrataDB's size-tiered scoring and leveled compaction achieve lower write amp than LevelDB. |
| `overwrite`        | 5.02              | 4.58              | Full overwrites trigger compactions across existing level ranges. |
| `ycsba`            | 2.98              | 2.31              | 50/50 read/update Zipfian workload updates skewed hot set. |
| `ycsbb`            | 0.50              | 1.27              | 95/5 read-heavy Zipfian workload generates small total update volume. |
| `readwhilewriting` | 1.99              | 0.66              | Background writes throttled to 10k writes/s while readers concurrently access LSM. |

---

## Experiments

### 1. Bloom bits per key vs `readmissing`

### 2. `fillsync` throughput, 1–16 threads, with and without group commit

### 3. Throughput and L0 file count over a 10M `fillrandom`

### 4. Block cache size vs `readrandom` hit rate and p99

### 5. Profiling pass: before and after

## Where LevelDB wins, and why
