# Benchmarks

<!-- Week 5. Final numbers come from native Linux on local NVMe, never WSL2 or /mnt/c.
     Each benchmark is run 3 times with the page cache dropped between read runs; report the median. -->

## Environment

| Item          | Value |
|---------------|-------|
| Machine       |       |
| CPU           |       |
| RAM           |       |
| Disk          |       |
| Kernel        |       |
| Compiler      |       |
| Flags         |       |
| LevelDB       |       |
| StrataDB      | commit `` |

## How to reproduce

## Results: StrataDB vs LevelDB

16-byte keys, 100-byte values. Note that standard YCSB records are 1 KB; here YCSB workloads (`ycsba`, `ycsbb`, `ycsbc`) use 16-byte keys and 100-byte values so their rows line up directly with the other workloads.

YCSB keys come from Gray's closed form at theta 0.99, which runs up to 20% hot at rank 2 and up to 2% light in the tail compared with exact Zipf. Ranks are then scrambled with `fnv1a(rank) % n`, which isn't a permutation: at 1M keys only 46% of keys can ever be drawn, where a random hash would reach 63%. The hottest 10,000 ranks don't collide, so the hot set is unaffected.

| Workload | Entries | StrataDB ops/s | LevelDB ops/s | StrataDB p99 | LevelDB p99 |
|----------|---------|----------------|---------------|--------------|-------------|

## Amplification

## Experiments

### 1. Bloom bits per key vs `readmissing`

### 2. `fillsync` throughput, 1–16 threads, with and without group commit

### 3. Throughput and L0 file count over a 10M `fillrandom`

### 4. Block cache size vs `readrandom` hit rate and p99

### 5. Profiling pass: before and after

## Where LevelDB wins, and why
