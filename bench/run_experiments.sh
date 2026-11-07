#!/usr/bin/env bash
# Runs experiments 1-4 from docs/BENCHMARKS.md and writes one CSV for each. Any failure stops
# the script with a non-zero exit. Experiment 5 is the before and after of the profiling pass.
#
#   1  Bloom bits per key vs readmissing, warm page cache
#   2  fillsync throughput by thread count, with and without group commit
#   3  Throughput and L0 files over a fillrandom
#   4  Block cache size vs readrandom hit rate and p99, under the disk tier's memory limit
#
# Experiment 4 takes hours per run and defaults to one, because readrandom in the disk tier
# stayed within 1.1% across runs in the published results. Its CSV says how many it had.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BENCH_BIN="${REPO_ROOT}/build/bench/strata_bench"
NUM=10M
SYNC_NUM=100K
RUNS=3
CACHE_RUNS=1
MEMORY_LIMIT=512M
READ_AHEAD_KB=128
TIMEOUT=10800
EXPERIMENTS=1,2,3,4
DB_DIR=""
OUTPUT_DIR=""
BLOOM_BITS="0 2 4 6 8 10 14"
THREAD_COUNTS="1 2 4 8 16"
CACHE_SIZES="8MB 32MB 64MB 128MB 256MB"

usage() {
  cat << EOF
Usage: $(basename "$0") --db-dir=DIR --output-dir=DIR [options]

  --db-dir=DIR        Where the databases go. Put this on the disk being measured. Required.
  --output-dir=DIR    Where the CSVs and logs go. Required.
  --experiments=LIST  Comma-separated, from 1, 2, 3 and 4 [default: $EXPERIMENTS]
  --num=N             Keys [default: $NUM]
  --sync-num=N        Writes for fillsync [default: $SYNC_NUM]
  --runs=N            Runs for experiments 1-3 [default: $RUNS]
  --cache-runs=N      Runs for experiment 4 [default: $CACHE_RUNS]
  --memory-limit=SZ   Limit for experiment 4 [default: $MEMORY_LIMIT]
  --bloom-bits=LIST   Quoted list [default: $BLOOM_BITS]
  --thread-counts=L   Quoted list [default: $THREAD_COUNTS]
  --cache-sizes=LIST  Quoted list [default: $CACHE_SIZES]
  --timeout=SEC       Fail if one workload takes longer than this [default: $TIMEOUT]
  --bench-bin=PATH    Benchmark binary [default: build/bench/strata_bench]
EOF
}

die() {
  echo "Error: $*" >&2
  exit 1
}

for arg in "$@"; do
  case "$arg" in
    --db-dir=*) DB_DIR="${arg#*=}" ;;
    --output-dir=*) OUTPUT_DIR="${arg#*=}" ;;
    --experiments=*) EXPERIMENTS="${arg#*=}" ;;
    --num=*) NUM="${arg#*=}" ;;
    --sync-num=*) SYNC_NUM="${arg#*=}" ;;
    --runs=*) RUNS="${arg#*=}" ;;
    --cache-runs=*) CACHE_RUNS="${arg#*=}" ;;
    --memory-limit=*) MEMORY_LIMIT="${arg#*=}" ;;
    --bloom-bits=*) BLOOM_BITS="${arg#*=}" ;;
    --thread-counts=*) THREAD_COUNTS="${arg#*=}" ;;
    --cache-sizes=*) CACHE_SIZES="${arg#*=}" ;;
    --timeout=*) TIMEOUT="${arg#*=}" ;;
    --bench-bin=*) BENCH_BIN="${arg#*=}" ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $arg" ;;
  esac
done
[ -n "$DB_DIR" ] && [ -n "$OUTPUT_DIR" ] || { usage >&2; die "--db-dir and --output-dir are required"; }
[ -x "$BENCH_BIN" ] || die "$BENCH_BIN not found"
sudo -n true 2> /dev/null || die "this script needs passwordless sudo"

mkdir -p "$DB_DIR" "$OUTPUT_DIR/logs"
DB_DIR="$(cd "$DB_DIR" && pwd)"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
RUNNER_UID="${SUDO_USER:-$(id -un)}"

READ_AHEAD_FILE=""
ORIGINAL_READ_AHEAD=""
restore_read_ahead() {
  if [ -n "$READ_AHEAD_FILE" ]; then
    echo "$ORIGINAL_READ_AHEAD" | sudo tee "$READ_AHEAD_FILE" > /dev/null || true
  fi
}
trap restore_read_ahead EXIT
trap 'exit 143' TERM INT

source_device="$(findmnt -no SOURCE -T "$DB_DIR" 2> /dev/null || true)"
device="$(lsblk -ndo PKNAME "$source_device" 2> /dev/null || true)"
if [ -n "$device" ] && [ -e "/sys/block/${device}/queue/read_ahead_kb" ]; then
  READ_AHEAD_FILE="/sys/block/${device}/queue/read_ahead_kb"
  ORIGINAL_READ_AHEAD="$(cat "$READ_AHEAD_FILE")"
  echo "$READ_AHEAD_KB" | sudo tee "$READ_AHEAD_FILE" > /dev/null
else
  echo "WARNING: can't set read-ahead for ${source_device:-the database disk}." >&2
fi

{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "args: $*"
  echo "commit: $(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2> /dev/null || echo unknown)"
  echo "uncommitted files: $(git -C "$REPO_ROOT" status --porcelain 2> /dev/null | wc -l)"
  echo "read-ahead: ${READ_AHEAD_KB} KB on ${device:-unknown}"
  echo "memory limit for experiment 4: $MEMORY_LIMIT"
  echo "runs: $RUNS, experiment 4: $CACHE_RUNS"
  grep -m1 'model name' /proc/cpuinfo || true
} > "${OUTPUT_DIR}/environment.txt"

wanted() {
  [[ ",${EXPERIMENTS}," == *",$1,"* ]]
}

# Runs one engine: bench NAME ENGINE ARGS... Output goes to logs/NAME.log.
bench() {
  local name="$1" eng="$2" status=0
  shift 2
  local -a engine_args=("--engine=leveldb")
  case "$eng" in
    strata) engine_args=("--engine=strata") ;;
    leveldb-nommap) engine_args+=("--leveldb_mmap=0") ;;
    leveldb) ;;
    *) die "unknown engine: $eng" ;;
  esac
  echo "  [$name | $eng]"
  timeout --kill-after=30 "$TIMEOUT" "$BENCH_BIN" "${engine_args[@]}" "--num=${NUM}" \
      "--db=${DB_DIR}" "$@" >> "${OUTPUT_DIR}/logs/${name}.log" 2>&1 || status=$?
  [ "$status" -eq 0 ] || die "$eng $name exited with status $status, see logs/${name}.log"
}

engine_dir() {
  if [ "$1" = strata ]; then echo strata; else echo leveldb; fi
}

# Builds a settled dataset and keeps a copy. The one YCSB operation makes the harness settle
# compactions before it closes the database.
build_snapshot() {
  local eng="$1" snapshot="$2" dir tmp
  shift 2
  dir="$(engine_dir "$eng")"
  tmp="$(mktemp)"
  rm -rf "${DB_DIR:?}/${dir}" "$snapshot"
  bench "snapshot" "$eng" --benchmarks=fillrandom,ycsbc --ycsb_ops=1 "--csv=${tmp}" "$@"
  rm -f "$tmp"
  mv "${DB_DIR}/${dir}" "$snapshot"
  [ -f "${snapshot}/.bench_dataset" ] || die "no dataset marker in $snapshot"
}

restore_snapshot() {
  local eng="$1" snapshot="$2" dir
  dir="$(engine_dir "$eng")"
  rm -rf "${DB_DIR:?}/${dir}"
  cp -a "$snapshot" "${DB_DIR}/${dir}"
}

experiment_1() {
  echo "--- experiment 1: bloom bits vs readmissing"
  local bits eng run snapshot
  local csv="${OUTPUT_DIR}/exp1_bloom.csv"
  rm -f "$csv"
  for bits in $BLOOM_BITS; do
    for eng in strata leveldb-nommap; do
      snapshot="${DB_DIR}/exp1_${eng}_${bits}.snapshot"
      build_snapshot "$eng" "$snapshot" "--bloom_bits=${bits}"
      for ((run = 1; run <= RUNS; run++)); do
        restore_snapshot "$eng" "$snapshot"
        sync
        cat "${DB_DIR}/$(engine_dir "$eng")"/* > /dev/null
        bench "exp1_bits${bits}" "$eng" --benchmarks=readmissing "--bloom_bits=${bits}" "--csv=${csv}"
      done
      rm -rf "${snapshot:?}"
    done
  done
}

experiment_2() {
  echo "--- experiment 2: fillsync by threads, with and without group commit"
  local threads run
  local csv="${OUTPUT_DIR}/exp2_group_commit.csv"
  rm -f "$csv"
  for ((run = 1; run <= RUNS; run++)); do
    for threads in $THREAD_COUNTS; do
      rm -rf "${DB_DIR:?}/strata" "${DB_DIR:?}/leveldb"
      bench "exp2_threads${threads}" strata --benchmarks=fillsync "--sync_num=${SYNC_NUM}" \
          "--threads=${threads}" --group_commit=1 "--csv=${csv}"
      rm -rf "${DB_DIR:?}/strata"
      bench "exp2_threads${threads}" strata --benchmarks=fillsync "--sync_num=${SYNC_NUM}" \
          "--threads=${threads}" --group_commit=0 "--csv=${csv}"
      bench "exp2_threads${threads}" leveldb --benchmarks=fillsync "--sync_num=${SYNC_NUM}" \
          "--threads=${threads}" "--csv=${csv}"
    done
  done
}

experiment_3() {
  echo "--- experiment 3: throughput and L0 files over a fillrandom"
  local eng run
  local csv="${OUTPUT_DIR}/exp3_summary.csv"
  rm -f "$csv" "${OUTPUT_DIR}"/exp3_timeseries_*.csv
  for ((run = 1; run <= RUNS; run++)); do
    for eng in strata leveldb; do
      rm -rf "${DB_DIR:?}/$(engine_dir "$eng")"
      bench "exp3" "$eng" --benchmarks=fillrandom --timeseries_interval_ms=200 \
          "--timeseries=${OUTPUT_DIR}/exp3_timeseries_${eng}_run${run}.csv" "--csv=${csv}"
    done
  done
}

experiment_4() {
  echo "--- experiment 4: block cache size vs readrandom"
  local size eng run snapshot dir status
  local csv="${OUTPUT_DIR}/exp4_block_cache.csv"
  rm -f "$csv"
  for eng in strata leveldb-nommap; do
    build_snapshot "$eng" "${DB_DIR}/exp4_${eng}.snapshot"
  done
  for ((run = 1; run <= CACHE_RUNS; run++)); do
    for size in $CACHE_SIZES; do
      for eng in strata leveldb-nommap; do
        snapshot="${DB_DIR}/exp4_${eng}.snapshot"
        dir="$(engine_dir "$eng")"
        restore_snapshot "$eng" "$snapshot"
        sync
        echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
        echo "  [exp4 | $eng | $size]"
        local -a args=("--engine=${dir}" --benchmarks=readrandom "--num=${NUM}" "--db=${DB_DIR}"
                       "--cache_size=${size}" "--csv=${csv}")
        [ "$eng" != leveldb-nommap ] || args+=("--leveldb_mmap=0")
        status=0
        sudo systemd-run --scope --quiet --uid="$RUNNER_UID" -p "MemoryMax=${MEMORY_LIMIT}" \
            -p MemorySwapMax=0 timeout --kill-after=30 "$TIMEOUT" "$BENCH_BIN" "${args[@]}" \
            >> "${OUTPUT_DIR}/logs/exp4_${size}.log" 2>&1 || status=$?
        [ "$status" -eq 0 ] || die "$eng at $size exited with status $status, see logs/exp4_${size}.log"
      done
    done
  done
  rm -rf "${DB_DIR:?}"/exp4_*.snapshot
}

wanted 1 && experiment_1
wanted 2 && experiment_2
wanted 3 && experiment_3
wanted 4 && experiment_4
rm -rf "${DB_DIR:?}/strata" "${DB_DIR:?}/leveldb"
echo "Done. Results are in $OUTPUT_DIR"
