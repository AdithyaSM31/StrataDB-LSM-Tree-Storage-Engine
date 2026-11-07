#!/usr/bin/env python3
"""Shows what the disk was doing during each disk-tier workload.

Joins the 5-second samples in iostat_disk.log to the time windows in disk.csv,
then prints the median across runs for each workload and engine. A row that
spends most of its samples near --cap-mib is measuring the disk's limit rather
than the engine.

Usage: iostat_by_workload.py RESULTS_DIR [--device sdb] [--cap-mib 236]
"""

import argparse
import csv
import gzip
import os
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

# Time left out at the start of long workloads, while the engine opens the database.
OPEN_MARGIN_SEC = 30
LONG_WORKLOAD_SEC = 120
NEAR_CAP = 0.95


def open_log(results_dir):
    path = os.path.join(results_dir, "iostat_disk.log")
    if os.path.exists(path):
        return open(path, encoding="utf-8")
    return gzip.open(path + ".gz", "rt", encoding="utf-8")


def read_samples(results_dir, device):
    """Returns (time, reads/s, read MiB/s) for each sample. iostat's "MB" is MiB."""
    samples = []
    when = None
    header = None
    with open_log(results_dir) as log:
        for line in log:
            fields = line.split()
            if len(fields) == 2 and "/" in fields[0] and ":" in fields[1]:
                when = datetime.strptime(line.strip(), "%m/%d/%y %H:%M:%S").replace(
                    tzinfo=timezone.utc
                )
            elif fields and fields[0] == "Device":
                header = fields
            elif fields and fields[0] == device and when and header:
                row = dict(zip(header, fields))
                samples.append((when, float(row["r/s"]), float(row["rMB/s"])))
    # The first report is the average since boot, not a 5-second sample.
    return samples[1:]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results_dir")
    parser.add_argument("--device", default="sdb")
    parser.add_argument("--cap-mib", type=float, default=236.0)
    args = parser.parse_args()

    samples = read_samples(args.results_dir, args.device)
    if not samples:
        sys.exit(f"no samples for device {args.device}")

    rows = defaultdict(list)
    with open(os.path.join(args.results_dir, "disk.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            seconds = float(row["total_sec"])
            start = datetime.strptime(row["run_id"], "%Y%m%dT%H%M%SZ").replace(
                tzinfo=timezone.utc
            )
            margin = OPEN_MARGIN_SEC if seconds > LONG_WORKLOAD_SEC else 0
            first = start + timedelta(seconds=margin)
            last = start + timedelta(seconds=seconds)
            window = [s for s in samples if first <= s[0] <= last]
            if not window:
                sys.exit(f"no samples during {row['workload']} on {row['engine']}")
            mib = [s[2] for s in window]
            near = sum(1 for m in mib if m >= args.cap_mib * NEAR_CAP)
            rows[(row["workload"], row["engine"])].append(
                {
                    "ops": float(row["ops_per_sec"]),
                    "mib": statistics.mean(mib),
                    "near": 100.0 * near / len(mib),
                    "iops": statistics.mean(s[1] for s in window),
                }
            )

    print(f"device {args.device}, near cap means at least "
          f"{args.cap_mib * NEAR_CAP:.0f} MiB/s of {args.cap_mib:.0f}")
    print(f"{'workload':<13}{'engine':<16}{'ops/s':>9}{'MiB/s':>8}{'% near cap':>12}"
          f"{'reads/s':>9}{'reads/op':>10}{'KB/op':>8}")
    for key in sorted(rows):
        runs = rows[key]
        ops = statistics.median(r["ops"] for r in runs)
        mib = statistics.median(r["mib"] for r in runs)
        near = statistics.median(r["near"] for r in runs)
        iops = statistics.median(r["iops"] for r in runs)
        print(f"{key[0]:<13}{key[1]:<16}{ops:>9.0f}{mib:>8.1f}{near:>12.1f}"
              f"{iops:>9.0f}{iops / ops:>10.2f}{mib * 1024 / ops:>8.1f}")


if __name__ == "__main__":
    main()
