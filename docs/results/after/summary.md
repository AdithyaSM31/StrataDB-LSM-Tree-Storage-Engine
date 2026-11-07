# Benchmark results

## Memory tier

Warm page cache, no memory limit. Medians.

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | Widest spread |
|----------|-----|----------------|---------------|-----------------------|---------------|
| fillseq | 10,000,000 | 548,149 | 585,046 | - | 21.8% **over 10%** |
| fillrandom | 10,000,000 | 164,362 | 225,638 | - | 2.3% |
| overwrite | 10,000,000 | 126,410 | 176,692 | - | 1.6% |
| fillsync | 100,000 | 4,498 | 4,529 | - | 1.7% |
| readseq | 10,000,000 | 2,250,617 | 4,595,953 | - | 1.0% |
| readrandom | 10,000,000 | 145,822 | 318,467 | 193,805 | 3.1% |
| readmissing | 10,000,000 | 563,898 | 572,516 | 559,352 | 1.9% |
| seekrandom | 10,000,000 | 45,785 | 142,732 | 90,505 | 2.4% |
| scan | 1,000,000 | 15,947 | 46,657 | 33,834 | 1.7% |
| ycsbc | 10,000,000 | 180,979 | 351,284 | 243,851 | 2.7% |
| ycsbb | 10,000,000 | 228,618 | 312,108 | - | 2.3% |
| ycsba | 10,000,000 | 269,012 | 317,836 | - | 2.5% |
| readwhilewriting | 10,000,000 | 137,009 | 292,767 | - | 5.4% |

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|----------|--------------|-------------|-------------|--------------|-------------|-------------|
| fillseq | 1.45 | 1.43 | - | 4.01 | 4.05 | - |
| fillrandom | 1.88 | 1.85 | - | 5.77 | 5.46 | - |
| overwrite | 1.86 | 1.93 | - | 6.67 | 6.02 | - |
| fillsync | 210.93 | 209.98 | - | 430.45 | 429.25 | - |
| readseq | 0.29 | 0.11 | - | 3.79 | 1.12 | - |
| readrandom | 6.46 | 2.87 | 4.87 | 12.90 | 5.48 | 7.89 |
| readmissing | 1.39 | 1.42 | 1.46 | 7.84 | 3.29 | 6.26 |
| seekrandom | 19.90 | 5.91 | 9.68 | 33.86 | 8.55 | 20.38 |
| scan | 60.54 | 20.06 | 28.11 | 78.64 | 32.31 | 42.96 |
| ycsbc | 6.13 | 2.66 | 4.57 | 12.21 | 4.97 | 7.25 |
| ycsbb | 2.89 | 2.67 | - | 12.59 | 8.94 | - |
| ycsba | 2.13 | 2.15 | - | 11.97 | 9.39 | - |
| readwhilewriting | 6.80 | 3.09 | - | 15.26 | 6.89 | - |

## Disk tier

Dropped page cache, cgroup memory limit. Medians.

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | Widest spread |
|----------|-----|----------------|---------------|-----------------------|---------------|
| readrandom | 10,000,000 | 9,410 | - | 9,412 | 0.7% |
| readmissing | 10,000,000 | 351,968 | - | 309,967 | 2.5% |
| seekrandom | 10,000,000 | 7,271 | - | 8,269 | 0.2% |
| scan | 1,000,000 | 3,203 | - | 3,669 | 1.8% |
| ycsbc | 10,000,000 | 20,823 | - | 20,976 | 0.8% |

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|----------|--------------|-------------|-------------|--------------|-------------|-------------|
| readrandom | 125.42 | - | 124.01 | 292.34 | - | 278.76 |
| readmissing | 1.53 | - | 1.62 | 9.50 | - | 7.35 |
| seekrandom | 145.83 | - | 132.46 | 338.06 | - | 310.64 |
| scan | 327.95 | - | 289.25 | 813.60 | - | 758.84 |
| ycsbc | 9.23 | - | 7.32 | 258.06 | - | 250.63 |

## Write amplification

Table bytes written per byte of keys and values written. The WAL is not counted, and data still in the memtable is not yet written, which is how a value can be under 1.

| Workload | StrataDB user MiB | LevelDB user MiB | StrataDB | LevelDB |
|----------|-------------------|------------------|----------|---------|
| fillseq | 1106.3 | 1106.3 | 1.00 | 1.00 |
| fillrandom | 1106.3 | 1106.3 | 9.14 | 9.06 |
| overwrite | 1106.3 | 1106.3 | 11.37 | 11.06 |

### Not comparable between engines

These workloads also read, and reads trigger compaction in LevelDB but not in StrataDB. The writes also differ in volume.

| Workload | StrataDB user MiB | LevelDB user MiB | StrataDB | LevelDB |
|----------|-------------------|------------------|----------|---------|
| ycsba | 553.3 | 553.3 | 6.19 | 11.58 |
| ycsbb | 55.3 | 55.3 | 8.14 | 124.64 |
| readwhilewriting | 80.7 | 37.8 | 6.11 | 34.46 |
