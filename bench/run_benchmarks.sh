#!/usr/bin/env bash
# ==============================================================================
# bench/run_benchmarks.sh
# Automated StrataDB vs LevelDB benchmark runner for two-tier evaluation:
#   1. In-memory tier (no memory limit, CPU path)
#   2. On-disk tier (MemoryMax=512M, real NVMe reads)
#
# Requirements met:
#   - Builds with CMake preset 'bench'
#   - Records git commit and LevelDB features
#   - Creates pristine datasets and restores snapshots per workload
#   - Drops OS page cache before every workload run
#   - 3 runs per workload, alternating engine order (StrataDB / LevelDB)
#   - Write amplification extraction from --stats logs
#   - Verifications: levels_at_start match, repeatability <= 10%, check_ycsb_mix.awk
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Defaults
NUM="10000000"
SYNC_NUM="100000"
YCSB_OPS=""
THREADS=1
RUNS=3
MEMORY_LIMIT="512M"
DB_DIR=""
OUTPUT_DIR=""
SKIP_BUILD=0
SKIP_FIO=0
DRY_RUN=0
REPEATABILITY_THRESHOLD=0.10

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Options:
  --num=N             Number of keys/entries (e.g. 10M, 1M, 100K) [default: 10M]
  --sync-num=N        Number of sync writes for fillsync [default: 100K, or min(num, 100K)]
  --ycsb-ops=N        Operations for YCSB workloads [default: num]
  --threads=N         Worker threads [default: 1]
  --runs=N            Number of benchmark iterations [default: 3]
  --memory-limit=SZ   Memory limit for on-disk tier [default: 512M]
  --db-dir=DIR        Directory for benchmark database [default: /mnt/nvme/strata_bench_data or \$HOME/strata_bench_data]
  --output-dir=DIR    Directory for CSVs and logs [default: bench/results]
  --skip-build        Skip cmake build of bench preset
  --skip-fio          Skip fio raw disk baseline
  --dry-run           Quick 1M dry run to verify setup and scripts
  -h, --help          Show this help message
EOF
  exit 0
}

parse_human_number() {
  local input="$1"
  local upper
  upper="$(echo "$input" | tr '[:lower:]' '[:upper:]')"
  case "$upper" in
    *K)
      local val="${upper%K}"
      echo "$(( val * 1000 ))"
      ;;
    *M)
      local val="${upper%M}"
      echo "$(( val * 1000000 ))"
      ;;
    *G)
      local val="${upper%G}"
      echo "$(( val * 1000000000 ))"
      ;;
    *)
      echo "$input"
      ;;
  esac
}

# Parse command line arguments
for arg in "$@"; do
  case "$arg" in
    --num=*)
      NUM="$(parse_human_number "${arg#*=}")"
      ;;
    --sync-num=*)
      SYNC_NUM="$(parse_human_number "${arg#*=}")"
      ;;
    --ycsb-ops=*)
      YCSB_OPS="$(parse_human_number "${arg#*=}")"
      ;;
    --threads=*)
      THREADS="${arg#*=}"
      ;;
    --runs=*)
      RUNS="${arg#*=}"
      ;;
    --memory-limit=*)
      MEMORY_LIMIT="${arg#*=}"
      ;;
    --db-dir=*)
      DB_DIR="${arg#*=}"
      ;;
    --output-dir=*)
      OUTPUT_DIR="${arg#*=}"
      ;;
    --skip-build)
      SKIP_BUILD=1
      ;;
    --skip-fio)
      SKIP_FIO=1
      ;;
    --dry-run)
      DRY_RUN=1
      ;;
    -h|--help)
      usage
      ;;
    *)
      echo "Unknown option: $arg" >&2
      usage
      ;;
  esac
done

if [ "$DRY_RUN" -eq 1 ]; then
  echo "=== Running in DRY RUN mode ==="
  if [ "$NUM" -eq 10000000 ]; then
    NUM="1000000"
  fi
  if [ "$SYNC_NUM" -eq 100000 ]; then
    SYNC_NUM="1000"
  fi
fi

if [ -z "$YCSB_OPS" ]; then
  YCSB_OPS="$NUM"
fi

# Ensure SYNC_NUM does not exceed NUM
if [ "$SYNC_NUM" -gt "$NUM" ]; then
  SYNC_NUM="$NUM"
fi

# Determine default DB directory
if [ -z "$DB_DIR" ]; then
  if [ -d "/mnt/nvme" ] && mountpoint -q "/mnt/nvme" 2>/dev/null; then
    DB_DIR="/mnt/nvme/strata_bench_data"
  elif [ -n "${HOME:-}" ] && [ -d "$HOME" ]; then
    DB_DIR="${HOME}/strata_bench_data"
  elif [ -d "/tmp" ]; then
    DB_DIR="/tmp/strata_bench_data"
  else
    DB_DIR="${REPO_ROOT}/strata_bench_data"
  fi
fi

# Determine default Output directory
if [ -z "$OUTPUT_DIR" ]; then
  OUTPUT_DIR="${REPO_ROOT}/bench/results"
fi

mkdir -p "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/logs"
mkdir -p "$DB_DIR"

echo "================================================================================"
echo "StrataDB vs LevelDB Benchmark Suite"
echo "================================================================================"
echo "Keys/Entries:    $NUM"
echo "Sync Ops:        $SYNC_NUM"
echo "YCSB Ops:        $YCSB_OPS"
echo "Threads:         $THREADS"
echo "Runs:            $RUNS"
echo "Memory Limit:    $MEMORY_LIMIT"
echo "DB Directory:    $DB_DIR"
echo "Output Directory:$OUTPUT_DIR"
echo "================================================================================"

# Build preset if requested
if [ "$SKIP_BUILD" -eq 0 ]; then
  echo "--- Building bench preset ---"
  cmake --preset bench -B "${REPO_ROOT}/build/bench" -S "${REPO_ROOT}"
  cmake --build "${REPO_ROOT}/build/bench" --preset bench
fi

BENCH_BIN="${REPO_ROOT}/build/bench/strata_bench"
if [ ! -x "$BENCH_BIN" ]; then
  echo "Error: Benchmark executable not found at $BENCH_BIN" >&2
  exit 1
fi

COMMIT="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo "unknown")"
echo "StrataDB Commit: $COMMIT"

# Drop caches helper
drop_caches() {
  sync
  if [ "$EUID" -eq 0 ]; then
    echo 3 > /proc/sys/vm/drop_caches
  elif command -v sudo >/dev/null 2>&1; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  else
    echo "Warning: cannot drop caches (no root/sudo)" >&2
  fi
}

# Raw disk baseline with fio
if [ "$SKIP_FIO" -eq 0 ] && command -v fio >/dev/null 2>&1; then
  echo "--- Running fio disk baseline ---"
  FIO_LOG="${OUTPUT_DIR}/fio_baseline.txt"
  FIO_TEST_FILE="${DB_DIR}/fio_test_tmp.dat"
  fio --name=baseline_randread --filename="$FIO_TEST_FILE" --size=512M --rw=randread \
      --bs=4k --direct=1 --numjobs=1 --time_based --runtime=5s --group_reporting \
      --name=baseline_randwrite --filename="$FIO_TEST_FILE" --size=512M --rw=randwrite \
      --bs=4k --direct=1 --numjobs=1 --time_based --runtime=5s --group_reporting \
      > "$FIO_LOG" 2>&1 || true
  rm -f "$FIO_TEST_FILE"
  echo "fio baseline saved to $FIO_LOG"
fi

# Pristine dataset generation
echo "--- Creating pristine datasets for StrataDB and LevelDB ($NUM keys) ---"
TMP_CSV="$(mktemp /tmp/throwaway_XXXXXX.csv)"
for eng in strata leveldb; do
  echo "Building snapshot for $eng..."
  rm -rf "${DB_DIR:?}/${eng}" "${DB_DIR:?}/${eng}.snapshot"
  "$BENCH_BIN" --engine="$eng" --benchmarks=fillrandom,ycsbc --ycsb_ops=1 \
      --num="$NUM" --threads="$THREADS" --db="$DB_DIR" --csv="$TMP_CSV" > /dev/null
  cp -a "${DB_DIR}/${eng}" "${DB_DIR}/${eng}.snapshot"
  if [ ! -f "${DB_DIR}/${eng}.snapshot/.bench_dataset" ]; then
    echo "Error: Dataset marker missing in ${DB_DIR}/${eng}.snapshot" >&2
    rm -f "$TMP_CSV"
    exit 1
  fi
  echo "Snapshot for $eng verified."
done
rm -f "$TMP_CSV"

MEMORY_CSV="${OUTPUT_DIR}/memory.csv"
DISK_CSV="${OUTPUT_DIR}/disk.csv"
rm -f "$MEMORY_CSV" "$DISK_CSV"

# Workload definitions
MEM_WORKLOADS=("fillseq" "fillrandom" "overwrite" "fillsync" "readseq" "readrandom" \
               "readmissing" "seekrandom" "scan" "ycsbc" "ycsbb" "ycsba" "readwhilewriting")
DISK_WORKLOADS=("readrandom" "readmissing" "seekrandom" "scan" "ycsbc")

is_fresh_workload() {
  local w="$1"
  [ "$w" = "fillseq" ] || [ "$w" = "fillrandom" ] || [ "$w" = "fillsync" ]
}

is_write_workload() {
  local w="$1"
  [ "$w" = "fillseq" ] || [ "$w" = "fillrandom" ] || [ "$w" = "overwrite" ] || \
  [ "$w" = "fillsync" ] || [ "$w" = "readwhilewriting" ] || [ "$w" = "ycsba" ] || [ "$w" = "ycsbb" ]
}

run_workload() {
  local tier="$1"
  local run_idx="$2"
  local eng="$3"
  local workload="$4"
  local is_limited="$5"
  local csv_file="$6"

  local log_file="${OUTPUT_DIR}/logs/${tier}_${workload}_run${run_idx}_${eng}.log"

  # Restore state
  if is_fresh_workload "$workload"; then
    rm -rf "${DB_DIR:?}/${eng}"
  else
    rm -rf "${DB_DIR:?}/${eng}"
    cp -a "${DB_DIR}/${eng}.snapshot" "${DB_DIR}/${eng}"
  fi

  # Drop page cache
  drop_caches

  # Build command args
  local -a cmd_args=(
    "--engine=${eng}"
    "--benchmarks=${workload}"
    "--num=${NUM}"
    "--threads=${THREADS}"
    "--db=${DB_DIR}"
    "--csv=${csv_file}"
  )

  if [ "$workload" = "fillsync" ]; then
    cmd_args+=("--sync_num=${SYNC_NUM}")
  fi

  if is_write_workload "$workload"; then
    cmd_args+=("--stats")
  fi

  if [[ "$workload" =~ ^ycsb ]]; then
    cmd_args+=("--ycsb_ops=${YCSB_OPS}")
  fi

  echo "  [Tier: $tier | Run: $run_idx | Engine: $eng | Workload: $workload]"
  if [ "$is_limited" -eq 1 ]; then
    if command -v systemd-run >/dev/null 2>&1; then
      sudo systemd-run --scope --uid="$(id -u)" -p "MemoryMax=${MEMORY_LIMIT}" -p "MemorySwapMax=0" \
          "$BENCH_BIN" "${cmd_args[@]}" > "$log_file" 2>&1 || {
            local exit_code=$?
            echo "Error running limited workload: exit code $exit_code" >&2
            if [ "$exit_code" -eq 137 ]; then
              echo "Process killed by OOM (exit 137). Consider increasing --memory-limit." >&2
            fi
            return $exit_code
          }
    else
      echo "Warning: systemd-run not found, running without memory limit" >&2
      "$BENCH_BIN" "${cmd_args[@]}" > "$log_file" 2>&1
    fi
  else
    "$BENCH_BIN" "${cmd_args[@]}" > "$log_file" 2>&1
  fi
}

# ==============================================================================
# Tier 1: In-Memory (No limit)
# ==============================================================================
echo ""
echo "================================================================================"
echo "Starting Tier 1: In-Memory Benchmarks ($RUNS runs, alternating engines)"
echo "================================================================================"

for ((run=1; run<=RUNS; run++)); do
  echo "--- In-Memory Run $run of $RUNS ---"
  if [ $((run % 2)) -eq 1 ]; then
    engines=("strata" "leveldb")
  else
    engines=("leveldb" "strata")
  fi

  for w in "${MEM_WORKLOADS[@]}"; do
    for eng in "${engines[@]}"; do
      run_workload "memory" "$run" "$eng" "$w" 0 "$MEMORY_CSV"
    done
  done
done

# ==============================================================================
# Tier 2: On-Disk (Memory limited to 512M)
# ==============================================================================
echo ""
echo "================================================================================"
echo "Starting Tier 2: On-Disk Benchmarks ($RUNS runs, MemoryMax=$MEMORY_LIMIT)"
echo "================================================================================"

for ((run=1; run<=RUNS; run++)); do
  echo "--- On-Disk Run $run of $RUNS ---"
  if [ $((run % 2)) -eq 1 ]; then
    engines=("strata" "leveldb")
  else
    engines=("leveldb" "strata")
  fi

  for w in "${DISK_WORKLOADS[@]}"; do
    for eng in "${engines[@]}"; do
      run_workload "disk" "$run" "$eng" "$w" 1 "$DISK_CSV"
    done
  done
done

# ==============================================================================
# Verification and Validation
# ==============================================================================
echo ""
echo "================================================================================"
echo "Running Validation & Sanity Checks"
echo "================================================================================"

# Check YCSB mix on memory tier
if [ -f "${SCRIPT_DIR}/check_ycsb_mix.awk" ]; then
  echo "Checking YCSB read/update mix on in-memory CSV..."
  if awk -f "${SCRIPT_DIR}/check_ycsb_mix.awk" "$MEMORY_CSV"; then
    echo "YCSB mix check PASSED."
  else
    echo "YCSB mix check FAILED." >&2
  fi
fi

# Python verification & summary generator
RERUN_FILE="${OUTPUT_DIR}/rerun_candidates.txt"
rm -f "$RERUN_FILE"

run_analysis() {
python3 - << 'PYEOF' "$OUTPUT_DIR" "$MEMORY_CSV" "$DISK_CSV" "$REPEATABILITY_THRESHOLD" "$RERUN_FILE"
import csv
import os
import re
import sys
from collections import defaultdict
import statistics

output_dir = sys.argv[1]
mem_csv = sys.argv[2]
disk_csv = sys.argv[3]
rep_threshold = float(sys.argv[4])
rerun_file = sys.argv[5]

def load_csv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)
    return rows

mem_rows = load_csv(mem_csv)
disk_rows = load_csv(disk_csv)

def analyze_tier(tier_name, rows):
    print(f"\n--- Analysis for {tier_name} Tier ---")
    grouped = defaultdict(lambda: {"ops": [], "p99": [], "entries": ""})
    levels_map = defaultdict(list)
    for r in rows:
        eng = r["engine"]
        w = r["workload"]
        ops = float(r["ops_per_sec"])
        p99 = float(r["p99_us"])
        grouped[(eng, w)]["ops"].append(ops)
        grouped[(eng, w)]["p99"].append(p99)
        grouped[(eng, w)]["entries"] = r.get("entries", "")
        levels = r.get("levels_at_start", "-")
        levels_map[(eng, w)].append(levels)

    # 1. Check levels_at_start consistency across runs
    levels_mismatches = 0
    for (eng, w), levels_list in levels_map.items():
        if w in ["fillseq", "fillrandom", "fillsync"]:
            continue
        if len(set(levels_list)) > 1:
            print(f"WARNING: levels_at_start mismatch for {eng} {w}: {levels_list}")
            levels_mismatches += 1
    if levels_mismatches == 0:
        print(f"Consistency check ({tier_name}): levels_at_start matched across all snapshot runs.")

    # 2. Check repeatability: (max - min) <= threshold * median
    repeatability_warnings = 0
    results = {}
    reruns = []
    for (eng, w), data in grouped.items():
        ops_list = data["ops"]
        p99_list = data["p99"]
        med_ops = statistics.median(ops_list)
        med_p99 = statistics.median(p99_list)
        min_ops = min(ops_list)
        max_ops = max(ops_list)
        if len(ops_list) > 1 and med_ops > 0:
            diff = (max_ops - min_ops) / med_ops
            if diff > rep_threshold:
                print(f"NOTE: Repeatability variation for {eng} {w}: (max-min)/median = {diff*100:.1f}% (> {rep_threshold*100:.0f}%) [runs: {[round(x, 1) for x in ops_list]}]")
                repeatability_warnings += 1
                t_str = "disk" if "disk" in tier_name.lower() else "memory"
                eng_str = "strata" if "strata" in eng.lower() else "leveldb"
                reruns.append((t_str, eng_str, w))
        results[(eng, w)] = {
            "entries": data["entries"],
            "median_ops": med_ops,
            "median_p99": med_p99,
        }

    return results, reruns

mem_results, mem_reruns = analyze_tier("In-Memory", mem_rows)
disk_results, disk_reruns = analyze_tier("On-Disk", disk_rows)

if not os.path.exists(rerun_file):
    with open(rerun_file, "w") as rf:
        for t, eng, w in mem_reruns + disk_reruns:
            rf.write(f"{t},{eng},{w}\n")

# Sanity check: on-disk vs in-memory
print("\n--- Sanity Check: On-Disk vs In-Memory ---")
for w in ["readrandom", "readmissing", "seekrandom", "scan", "ycsbc"]:
    for eng in ["StrataDB", "LevelDB"]:
        mem_val = mem_results.get((eng, w), {}).get("median_ops", 0)
        disk_val = disk_results.get((eng, w), {}).get("median_ops", 0)
        if mem_val > 0 and disk_val > 0:
            ratio = mem_val / disk_val
            print(f"{eng:8s} {w:12s}: In-Memory={mem_val:10.1f} ops/s | On-Disk={disk_val:10.1f} ops/s (Speedup: {ratio:.1f}x)")
            if disk_val > mem_val:
                print(f"WARNING: On-disk ({disk_val:.1f}) unexpectedly faster than in-memory ({mem_val:.1f}) for {eng} {w}")

# Parse Write Amplification from logs
print("\n--- Write Amplification Analysis ---")
logs_dir = os.path.join(output_dir, "logs")
amp_results = defaultdict(lambda: defaultdict(list))
if os.path.exists(logs_dir):
    for f in sorted(os.listdir(logs_dir)):
        if "memory" in f and any(w in f for w in ["fillseq", "fillrandom", "overwrite", "ycsba", "ycsbb", "readwhilewriting"]):
            filepath = os.path.join(logs_dir, f)
            with open(filepath, "r", errors="ignore") as lf:
                content = lf.read()
                parts = f.replace(".log", "").split("_")
                w = parts[1]
                eng = "StrataDB" if "strata" in f else "LevelDB"
                if eng == "StrataDB":
                    m = re.search(r"Write amplification:\s+([0-9.]+)", content)
                    if m:
                        amp = float(m.group(1))
                        amp_results["StrataDB"][w].append(amp)
                else:
                    writes = re.findall(r"\s+\d+\s+\d+\s+[\d.]+\s+[\d.]+\s+([\d.]+)\s+([\d.]+)", content)
                    if writes:
                        total_comp_write = sum(float(x[1]) for x in writes)
                        num_entries = float(mem_results.get(("LevelDB", w), {}).get("entries", 0) or 1000000)
                        user_mb = (num_entries * 116.0) / (1024.0 * 1024.0)
                        if user_mb > 0:
                            amp_results["LevelDB"][w].append(total_comp_write / user_mb)

summary_md_path = os.path.join(output_dir, "summary.md")
with open(summary_md_path, "w") as sm:
    sm.write("# Benchmark Results Summary\n\n")
    sm.write("## In-Memory Tier (No Limit)\n\n")
    sm.write("| Workload | Entries | StrataDB ops/s | LevelDB ops/s | StrataDB p99 | LevelDB p99 |\n")
    sm.write("|----------|---------|----------------|---------------|--------------|-------------|\n")
    all_workloads = ["fillseq", "fillrandom", "overwrite", "fillsync", "readseq", "readrandom",
                     "readmissing", "seekrandom", "scan", "ycsbc", "ycsbb", "ycsba", "readwhilewriting"]
    for w in all_workloads:
        s_data = mem_results.get(("StrataDB", w), {})
        l_data = mem_results.get(("LevelDB", w), {})
        entries = s_data.get("entries") or l_data.get("entries") or "-"
        s_ops = f"{s_data.get('median_ops', 0):.1f}" if "median_ops" in s_data else "-"
        l_ops = f"{l_data.get('median_ops', 0):.1f}" if "median_ops" in l_data else "-"
        s_p99 = f"{s_data.get('median_p99', 0):.2f}" if "median_p99" in s_data else "-"
        l_p99 = f"{l_data.get('median_p99', 0):.2f}" if "median_p99" in l_data else "-"
        row = f"| {w:<16} | {entries:<7} | {s_ops:<14} | {l_ops:<13} | {s_p99:<12} | {l_p99:<11} |\n"
        sm.write(row)
        print(row.strip())

    sm.write("\n## On-Disk Tier (MemoryMax=512M)\n\n")
    sm.write("| Workload | Entries | StrataDB ops/s | LevelDB ops/s | StrataDB p99 | LevelDB p99 |\n")
    sm.write("|----------|---------|----------------|---------------|--------------|-------------|\n")
    for w in ["readrandom", "readmissing", "seekrandom", "scan", "ycsbc"]:
        s_data = disk_results.get(("StrataDB", w), {})
        l_data = disk_results.get(("LevelDB", w), {})
        entries = s_data.get("entries") or l_data.get("entries") or "-"
        s_ops = f"{s_data.get('median_ops', 0):.1f}" if "median_ops" in s_data else "-"
        l_ops = f"{l_data.get('median_ops', 0):.1f}" if "median_ops" in l_data else "-"
        s_p99 = f"{s_data.get('median_p99', 0):.2f}" if "median_p99" in s_data else "-"
        l_p99 = f"{l_data.get('median_p99', 0):.2f}" if "median_p99" in l_data else "-"
        row = f"| {w:<16} | {entries:<7} | {s_ops:<14} | {l_ops:<13} | {s_p99:<12} | {l_p99:<11} |\n"
        sm.write(row)
        print(row.strip())

    sm.write("\n## Write Amplification\n\n")
    sm.write("| Workload | StrataDB Write Amp | LevelDB Write Amp |\n")
    sm.write("|----------|-------------------|-------------------|\n")
    for w in ["fillseq", "fillrandom", "overwrite", "ycsba", "ycsbb", "readwhilewriting"]:
        s_amp_list = amp_results["StrataDB"].get(w, [])
        l_amp_list = amp_results["LevelDB"].get(w, [])
        s_amp_str = f"{statistics.median(s_amp_list):.2f}" if s_amp_list else "-"
        l_amp_str = f"{statistics.median(l_amp_list):.2f}" if l_amp_list else "-"
        sm.write(f"| {w:<16} | {s_amp_str:<17} | {l_amp_str:<17} |\n")

print(f"\nMarkdown summary saved to: {summary_md_path}")
PYEOF
}

run_analysis

# Optional Repeatability Rerun (1 pass)
if [ -f "$RERUN_FILE" ] && [ -s "$RERUN_FILE" ]; then
  echo ""
  echo "--- Repeatability Rerun for High-Variance Workloads ---"
  while IFS=',' read -r t eng w; do
    echo "Rerunning high-variance workload: tier=$t, engine=$eng, workload=$w"
    is_lim=0
    csv="$MEMORY_CSV"
    if [ "$t" = "ondisk" ] || [ "$t" = "disk" ]; then
      is_lim=1
      csv="$DISK_CSV"
    fi
    run_workload "$t" "extra" "$eng" "$w" "$is_lim" "$csv"
  done < "$RERUN_FILE"
  rm -f "$RERUN_FILE"
  echo "Re-analyzing with extra runs included..."
  run_analysis
fi

echo ""
echo "================================================================================"
echo "Benchmarks complete!"
echo "Raw CSVs: $MEMORY_CSV, $DISK_CSV"
echo "Logs: $OUTPUT_DIR/logs/"
echo "Summary: $OUTPUT_DIR/summary.md"
echo "================================================================================"
