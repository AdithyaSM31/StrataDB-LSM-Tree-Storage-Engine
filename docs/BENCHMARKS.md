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
