#!/usr/bin/env python3
"""Checks the output of run_benchmarks.sh and writes summary.md next to it.

Exits 1 if a check fails: a run is missing, or runs of a workload started from different LSM
shapes. Runs that disagree with each other by more than 10% are flagged in the summary but don't
fail, because rerunning only those would bias the medians.
"""

import argparse
import csv
import os
import re
import statistics
import sys
from collections import defaultdict

ENGINES = ["StrataDB", "LevelDB", "LevelDB-nommap"]
LOG_ENGINES = {"strata": "StrataDB", "leveldb": "LevelDB"}
FRESH_WORKLOADS = {"fillseq", "fillrandom", "fillsync"}
MEMORY_WORKLOADS = [
    "fillseq", "fillrandom", "overwrite", "fillsync", "readseq", "readrandom", "readmissing",
    "seekrandom", "scan", "ycsbc", "ycsbb", "ycsba", "readwhilewriting",
]
DISK_WORKLOADS = ["readrandom", "readmissing", "seekrandom", "scan", "ycsbc"]
COMPARABLE_AMP = ["fillseq", "fillrandom", "overwrite"]
READ_AFFECTED_AMP = ["ycsba", "ycsbb", "readwhilewriting"]
SPREAD_LIMIT = 0.10
KEY_SIZE = 16
MIB = 1048576.0

LOG_NAME = re.compile(r"^memory_([a-z]+)_run\w+_(strata|leveldb)\.log$")
RUN_ID = re.compile(r"^Run ID:\s+(\S+)", re.M)
# Level, files, size, time, read, write. LevelDB's time can be negative.
LEVELDB_ROW = re.compile(r"^\s*\d+\s+\d+\s+-?\d+\s+-?\d+\s+\d+\s+(\d+)\s*$", re.M)
STRATA_SSTABLE_MB = re.compile(r"^SSTable bytes written:\s+([\d.]+) MB", re.M)


def load_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def check_tier(name, rows, runs, problems):
    """Groups rows by engine and workload and records anything that invalidates them."""
    groups = defaultdict(list)
    for row in rows:
        groups[(row["engine"], row["workload"])].append(row)
    for (engine, workload), group in sorted(groups.items()):
        if len(group) != runs:
            problems.append(f"{name}: {engine} {workload} has {len(group)} runs, expected {runs}")
        levels = {row["levels_at_start"] for row in group}
        if workload not in FRESH_WORKLOADS and len(levels) > 1:
            problems.append(f"{name}: {engine} {workload} started from different shapes: {levels}")
    return groups


def median_of(group, column):
    return statistics.median(float(row[column]) for row in group)


def spread_of(group):
    values = [float(row["ops_per_sec"]) for row in group]
    middle = statistics.median(values)
    return (max(values) - min(values)) / middle if middle > 0 else 0.0


def cell(groups, engine, workload, column, fmt):
    group = groups.get((engine, workload))
    return fmt.format(median_of(group, column)) if group else "-"


def tier_tables(groups, workloads):
    lines = [
        "| Workload | Ops | StrataDB ops/s | LevelDB ops/s | LevelDB no mmap ops/s | Widest spread |",
        "|----------|-----|----------------|---------------|-----------------------|---------------|",
    ]
    for workload in workloads:
        present = [groups[(e, workload)] for e in ENGINES if (e, workload) in groups]
        if not present:
            continue
        widest = max(spread_of(group) for group in present)
        flag = " **over 10%**" if widest > SPREAD_LIMIT else ""
        ops = present[0][0]["num_ops"]
        rates = [cell(groups, e, workload, "ops_per_sec", "{:,.0f}") for e in ENGINES]
        lines.append(f"| {workload} | {int(ops):,} | " + " | ".join(rates)
                     + f" | {widest * 100:.1f}%{flag} |")
    lines += [
        "",
        "| Workload | p50 StrataDB | p50 LevelDB | p50 no mmap "
        "| p99 StrataDB | p99 LevelDB | p99 no mmap |",
        "|----------|--------------|-------------|-------------"
        "|--------------|-------------|-------------|",
    ]
    for workload in workloads:
        if not any((e, workload) in groups for e in ENGINES):
            continue
        p50 = [cell(groups, e, workload, "p50_us", "{:.2f}") for e in ENGINES]
        p99 = [cell(groups, e, workload, "p99_us", "{:.2f}") for e in ENGINES]
        lines.append(f"| {workload} | " + " | ".join(p50 + p99) + " |")
    return lines


def user_ops(row):
    workload = row["workload"]
    if workload in ("ycsba", "ycsbb"):
        return int(row["update_ops"])
    if workload == "readwhilewriting":
        return int(row["writer_ops"])
    return int(row["num_ops"])


def write_amplification(output_dir, rows, problems):
    """Table bytes written per byte of keys and values written, per engine and workload.

    Both engines get the same denominator. Their own counters differ: StrataDB's user bytes
    include batch overhead. Neither side counts the WAL.
    """
    by_run_id = {(row["run_id"], row["engine"]): row for row in rows}
    results = defaultdict(list)
    user_mib = defaultdict(list)
    logs_dir = os.path.join(output_dir, "logs")
    if not os.path.isdir(logs_dir):
        return results, user_mib
    for name in sorted(os.listdir(logs_dir)):
        match = LOG_NAME.match(name)
        if not match or match.group(1) not in COMPARABLE_AMP + READ_AFFECTED_AMP:
            continue
        workload, engine = match.group(1), LOG_ENGINES[match.group(2)]
        with open(os.path.join(logs_dir, name), errors="replace") as f:
            text = f.read()
        run_id = RUN_ID.search(text)
        row = by_run_id.get((run_id.group(1) if run_id else "", engine))
        if row is None:
            problems.append(f"{name} has no matching row in memory.csv")
            continue
        if engine == "StrataDB":
            written = STRATA_SSTABLE_MB.search(text)
            mib = float(written.group(1)) if written else None
        elif "--- LevelDB Stats ---" in text:
            # A database that has only ever used its memtable prints no level rows.
            mib = float(sum(int(w) for w in LEVELDB_ROW.findall(text)))
        else:
            mib = None
        if mib is None:
            problems.append(f"{name} has no write statistics")
            continue
        payload = user_ops(row) * (KEY_SIZE + int(row["value_size"])) / MIB
        user_mib[(engine, workload)].append(payload)
        if payload > 0:
            results[(engine, workload)].append(mib / payload)
    return results, user_mib


def amp_table(results, user_mib, workloads):
    lines = [
        "| Workload | StrataDB user MiB | LevelDB user MiB | StrataDB | LevelDB |",
        "|----------|-------------------|------------------|----------|---------|",
    ]
    for workload in workloads:
        cols = []
        for engine in ("StrataDB", "LevelDB"):
            mib = user_mib.get((engine, workload))
            cols.append(f"{statistics.median(mib):.1f}" if mib else "-")
        for engine in ("StrataDB", "LevelDB"):
            amp = results.get((engine, workload))
            cols.append(f"{statistics.median(amp):.2f}" if amp else "-")
        lines.append(f"| {workload} | " + " | ".join(cols) + " |")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir")
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    memory_rows = load_rows(os.path.join(args.output_dir, "memory.csv"))
    disk_rows = load_rows(os.path.join(args.output_dir, "disk.csv"))
    if not memory_rows and not disk_rows:
        print(f"No results in {args.output_dir}", file=sys.stderr)
        return 1

    problems = []
    notes = []
    memory = check_tier("memory", memory_rows, args.runs, problems)
    disk = check_tier("disk", disk_rows, args.runs, problems)

    for (engine, workload), group in sorted(disk.items()):
        other = memory.get((engine, workload))
        if other and group[0]["levels_at_start"] != other[0]["levels_at_start"]:
            notes.append(f"{engine} {workload}: the tiers used different snapshots, "
                         f"{other[0]['levels_at_start']} and {group[0]['levels_at_start']}")
        if other and median_of(group, "ops_per_sec") > median_of(other, "ops_per_sec"):
            notes.append(f"{engine} {workload}: the disk tier was faster than the memory tier, "
                         "so the memory limit may not be constraining it")

    results, user_mib = write_amplification(args.output_dir, memory_rows, problems)

    lines = ["# Benchmark results", ""]
    if memory_rows:
        lines += ["## Memory tier", "", "Warm page cache, no memory limit. Medians.", ""]
        lines += tier_tables(memory, MEMORY_WORKLOADS)
        lines.append("")
    if disk_rows:
        lines += ["## Disk tier", "", "Dropped page cache, cgroup memory limit. Medians.", ""]
        lines += tier_tables(disk, DISK_WORKLOADS)
        lines.append("")
    if results:
        lines += ["## Write amplification", "",
                  "Table bytes written per byte of keys and values written. The WAL is not "
                  "counted, and data still in the memtable is not yet written, which is how a "
                  "value can be under 1.", ""]
        lines += amp_table(results, user_mib, COMPARABLE_AMP)
        lines += ["", "### Not comparable between engines", "",
                  "These workloads also read, and reads trigger compaction in LevelDB but not "
                  "in StrataDB. The writes also differ in volume.", ""]
        lines += amp_table(results, user_mib, READ_AFFECTED_AMP)
        lines.append("")
    if notes:
        lines += ["## Notes", ""] + [f"- {note}" for note in notes] + [""]

    summary_path = os.path.join(args.output_dir, "summary.md")
    with open(summary_path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("\n".join(lines))

    if problems:
        print("\nCHECKS FAILED:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    print(f"Checks passed. Summary written to {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
