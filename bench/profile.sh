#!/usr/bin/env bash
# Profiles workloads under the same conditions as the benchmark's memory tier:
# each one starts from a copy of one snapshot, with a warm page cache.
#
# perf samples cpu-clock, because Azure doesn't expose hardware counters on the
# D4ds_v4. Needs passwordless sudo, and a build with frame pointers (the bench preset).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BENCH_BIN="${REPO_ROOT}/build/bench/strata_bench"
NUM=10M
DB_DIR=""
OUTPUT_DIR=""
FREQUENCY=999
ENGINES="strata leveldb-nommap leveldb"
WRITE_WORKLOADS="fillrandom overwrite"
READ_WORKLOADS="readrandom seekrandom scan"

usage() {
  cat <<EOF
Usage: $0 --db-dir=PATH --output-dir=PATH [options]
  --num=N              Dataset size (default: $NUM)
  --engines=LIST       Quoted list from: strata leveldb leveldb-nommap (default: $ENGINES)
  --write-workloads=L  Quoted list, or empty for none (default: $WRITE_WORKLOADS)
  --read-workloads=L   Quoted list, or empty for none (default: $READ_WORKLOADS)
  --bench-bin=PATH     Benchmark binary (default: build/bench/strata_bench)
EOF
}

for arg in "$@"; do
  case "$arg" in
    --db-dir=*) DB_DIR="${arg#*=}" ;;
    --output-dir=*) OUTPUT_DIR="${arg#*=}" ;;
    --num=*) NUM="${arg#*=}" ;;
    --engines=*) ENGINES="${arg#*=}" ;;
    --write-workloads=*) WRITE_WORKLOADS="${arg#*=}" ;;
    --read-workloads=*) READ_WORKLOADS="${arg#*=}" ;;
    --bench-bin=*) BENCH_BIN="${arg#*=}" ;;
    -h | --help) usage; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done
[ -n "$DB_DIR" ] && [ -n "$OUTPUT_DIR" ] || { usage >&2; exit 2; }
[ -x "$BENCH_BIN" ] || { echo "No benchmark binary at $BENCH_BIN" >&2; exit 1; }

UID_NOW="$(id -u)"
mkdir -p "$DB_DIR" "$OUTPUT_DIR"
CSV="${OUTPUT_DIR}/profile.csv"

engine_dir() {
  if [ "$1" = strata ]; then echo strata; else echo leveldb; fi
}

build_snapshot() {
  local dir="$1" tmp
  [ -f "${DB_DIR}/${dir}.snapshot/.bench_dataset" ] && return 0
  echo "--- building the $dir snapshot"
  tmp="$(mktemp)"
  rm -rf "${DB_DIR:?}/${dir}" "${DB_DIR:?}/${dir}.snapshot"
  # The one YCSB operation makes the harness settle compactions before it closes the database.
  "$BENCH_BIN" --engine="$dir" --benchmarks=fillrandom,ycsbc --ycsb_ops=1 --num="$NUM" \
      --db="$DB_DIR" --csv="$tmp" > /dev/null
  cp -a "${DB_DIR}/${dir}" "${DB_DIR}/${dir}.snapshot"
  rm -f "$tmp"
}

profile_one() {
  local eng="$1" workload="$2" fresh="$3" dir name data
  dir="$(engine_dir "$eng")"
  name="${eng}_${workload}"
  data="${DB_DIR}/${name}.perf.data"

  rm -rf "${DB_DIR:?}/${dir}"
  if [ "$fresh" -eq 0 ]; then
    cp -a "${DB_DIR}/${dir}.snapshot" "${DB_DIR}/${dir}"
    sync
    cat "${DB_DIR}/${dir}"/* > /dev/null
  fi

  local -a args=("--engine=$dir" "--benchmarks=${workload}" "--num=${NUM}" "--db=${DB_DIR}" "--csv=${CSV}")
  [ "$eng" != leveldb-nommap ] || args+=("--leveldb_mmap=0")

  echo "  [$name]"
  sudo perf record -q -F "$FREQUENCY" -e cpu-clock --call-graph fp -o "$data" -- \
      sudo -u "#${UID_NOW}" "$BENCH_BIN" "${args[@]}" > "${OUTPUT_DIR}/${name}.log" 2>&1

  # Self time by function, then how the samples split between threads.
  sudo perf report -i "$data" --stdio --no-children -g none --percent-limit 0.2 \
      --sort symbol > "${OUTPUT_DIR}/${name}.report.txt" 2> /dev/null
  sudo perf report -i "$data" --stdio --no-children -g none --percent-limit 0.2 \
      --sort pid > "${OUTPUT_DIR}/${name}.threads.txt" 2> /dev/null
  sudo perf script -i "$data" -F comm,tid,ip,sym,dso 2> /dev/null \
    | python3 "${SCRIPT_DIR}/flamegraph.py" --title "$name, $NUM keys" \
        --svg "${OUTPUT_DIR}/${name}.svg" --folded "${OUTPUT_DIR}/${name}.folded"
  sudo rm -f "$data"
}

{
  date -u +%Y-%m-%dT%H:%M:%SZ
  git -C "$REPO_ROOT" rev-parse HEAD
  git -C "$REPO_ROOT" status --short
  uname -r
  grep -m1 'model name' /proc/cpuinfo
  sudo perf --version
} > "${OUTPUT_DIR}/profile_environment.txt"

for eng in $ENGINES; do
  build_snapshot "$(engine_dir "$eng")"
  if [ "$eng" != leveldb-nommap ]; then
    for workload in $WRITE_WORKLOADS; do
      if [ "$workload" = fillrandom ]; then fresh=1; else fresh=0; fi
      profile_one "$eng" "$workload" "$fresh"
    done
  fi
  for workload in $READ_WORKLOADS; do
    profile_one "$eng" "$workload" 0
  done
done

for eng in strata leveldb; do
  rm -rf "${DB_DIR:?}/${eng}"
done
echo "Done. Results are in $OUTPUT_DIR"
