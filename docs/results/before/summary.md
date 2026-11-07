# Benchmark results

## Memory tier

Warm page cache, no memory limit. Medians.

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | Widest spread |
|----------|-----|----------------|---------------|-----------------------|---------------|
| fillseq | 10,000,000 | 472,160 | 556,157 | - | 20.7% **over 10%** |
| fillrandom | 10,000,000 | 94,916 | 211,550 | - | 3.7% |
| overwrite | 10,000,000 | 73,149 | 165,687 | - | 1.0% |
| fillsync | 100,000 | 4,556 | 4,498 | - | 3.8% |
| readseq | 10,000,000 | 1,509,695 | 4,104,609 | - | 2.1% |
| readrandom | 10,000,000 | 59,160 | 237,564 | 162,276 | 2.9% |
| readmissing | 10,000,000 | 583,172 | 582,019 | 566,598 | 2.6% |
| seekrandom | 10,000,000 | 25,443 | 109,420 | 80,688 | 1.4% |
| scan | 1,000,000 | 9,569 | 40,369 | 30,882 | 2.7% |
| ycsbc | 10,000,000 | 85,015 | 254,062 | 211,801 | 1.7% |
| ycsbb | 10,000,000 | 108,426 | 270,025 | - | 5.3% |
| ycsba | 10,000,000 | 156,226 | 289,941 | - | 0.8% |
| readwhilewriting | 10,000,000 | 57,544 | 221,570 | - | 2.1% |

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|----------|--------------|-------------|-------------|--------------|-------------|-------------|
| fillseq | 1.74 | 1.49 | - | 4.40 | 4.09 | - |
| fillrandom | 2.16 | 1.95 | - | 10.26 | 5.61 | - |
| overwrite | 2.15 | 1.96 | - | 1001.95 | 6.09 | - |
| fillsync | 208.00 | 211.36 | - | 430.73 | 429.75 | - |
| readseq | 0.21 | 0.11 | - | 13.54 | 2.29 | - |
| readrandom | 16.31 | 3.93 | 5.88 | 31.53 | 6.68 | 9.32 |
| readmissing | 1.22 | 1.42 | 1.44 | 16.89 | 3.95 | 7.07 |
| seekrandom | 35.34 | 8.04 | 11.45 | 64.28 | 16.01 | 22.44 |
| scan | 101.63 | 23.50 | 31.22 | 141.25 | 35.67 | 46.60 |
| ycsbc | 15.56 | 3.76 | 5.55 | 30.95 | 6.42 | 8.40 |
| ycsbb | 3.25 | 3.52 | - | 30.73 | 10.01 | - |
| ycsba | 2.46 | 2.16 | - | 26.95 | 10.80 | - |
| readwhilewriting | 16.52 | 4.19 | - | 31.93 | 8.14 | - |

## Disk tier

Dropped page cache, cgroup memory limit. Medians.

| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | Widest spread |
|----------|-----|----------------|---------------|-----------------------|---------------|
| readrandom | 10,000,000 | 8,435 | 3,726 | 9,454 | 1.1% |
| readmissing | 10,000,000 | 368,622 | 263,586 | 331,406 | 2.1% |
| seekrandom | 10,000,000 | 6,377 | 3,134 | 8,069 | 1.6% |
| scan | 1,000,000 | 2,798 | 2,930 | 3,616 | 1.0% |
| ycsbc | 10,000,000 | 18,152 | 6,729 | 20,773 | 0.6% |

| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap | p99 StrataDB | p99 LevelDB | p99 no mmap |
|----------|--------------|-------------|-------------|--------------|-------------|-------------|
| readrandom | 135.68 | 187.01 | 124.29 | 318.55 | 644.18 | 278.33 |
| readmissing | 1.34 | 1.58 | 1.59 | 18.38 | 5.50 | 7.95 |
| seekrandom | 162.98 | 206.79 | 134.53 | 397.92 | 830.02 | 320.09 |
| scan | 369.13 | 228.46 | 291.68 | 889.02 | 900.84 | 776.08 |
| ycsbc | 18.33 | 7.45 | 8.29 | 280.30 | 518.38 | 252.70 |

## Write amplification

Table bytes written per byte of keys and values written. The WAL is not counted, and data still in the memtable is not yet written, which is how a value can be under 1.

| Workload | StrataDB user MiB | LevelDB user MiB | StrataDB | LevelDB |
|----------|-------------------|------------------|----------|---------|
| fillseq | 1106.3 | 1106.3 | 1.00 | 1.00 |
| fillrandom | 1106.3 | 1106.3 | 9.21 | 9.05 |
| overwrite | 1106.3 | 1106.3 | 11.39 | 10.95 |

### Not comparable between engines

These workloads also read, and reads trigger compaction in LevelDB but not in StrataDB. The writes also differ in volume.

| Workload | StrataDB user MiB | LevelDB user MiB | StrataDB | LevelDB |
|----------|-------------------|------------------|----------|---------|
| ycsba | 553.3 | 553.3 | 5.79 | 11.75 |
| ycsbb | 55.3 | 55.3 | 5.10 | 134.42 |
| readwhilewriting | 192.2 | 49.9 | 3.77 | 21.45 |
