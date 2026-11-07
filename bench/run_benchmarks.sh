#!/usr/bin/env bash
# Runs StrataDB and LevelDB through strata_bench in two tiers, then summarises the results.
#
#   memory  no memory limit and a warm page cache, so disk reads don't touch the numbers
#   disk    a cgroup memory limit and a dropped page cache, so reads reach the disk
#
# Read-ahead on the database's disk is set for the run and put back afterwards, because it changes
# results: Azure images use 4 MB, and under the memory limit that made LevelDB's mmap reads 18
# times slower than at the usual 128 KB.
#
# Every workload except fillseq, fillrandom and fillsync starts from a copy of one snapshot.
# Read workloads also run LevelDB with mmap off, which shows how much of a result comes from
# LevelDB's default of mapping its files. Any failed run stops the script with a non-zero exit,
# because a median over fewer runs would look like a good result.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NUM=10000000
SYNC_NUM=100000
YCSB_OPS=""
THREADS=1
RUNS=3
TIER=all
MEMORY_LIMIT=""
RUNNER_UID=""
DB_DIR=""
OUTPUT_DIR="${REPO_ROOT}/bench/results"
FIO_SECONDS=30
READ_AHEAD_KB=128
WORKLOAD_TIMEOUT=10800
DISK_ENGINES=strata,leveldb,leveldb-nommap
SKIP_BUILD=0
SKIP_FIO=0
SKIP_SNAPSHOT=0
REPORT_ONLY=0
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --db-dir=DIR [options]

  --db-dir=DIR        Where the databases go. Put this on the disk being measured. Required.
  --num=N             Keys, with K, M or G [default: 10M]
  --sync-num=N        Writes for fillsync [default: 100K, never more than --num]
  --ycsb-ops=N        Operations per YCSB workload [default: --num]
  --threads=N         Worker threads [default: 1]
  --runs=N            Runs per workload and engine [default: 3]
  --tier=TIER         all, memory or disk [default: all]
  --memory-limit=SZ   Limit for the disk tier [default: 512M from 5M keys, otherwise 64M]
  --uid=USER          User that runs the memory-limited process [default: the invoking user]
  --output-dir=DIR    CSVs, logs and summary [default: bench/results]
  --read-ahead-kb=N   Read-ahead on the database's disk during the run [default: 128]
  --workload-timeout=SEC  Fail if one workload takes longer than this [default: 10800]
  --disk-engines=LIST Comma-separated engines for the disk tier, from strata, leveldb and
                      leveldb-nommap [default: all three]
  --skip-build      Use the existing build/bench/strata_bench
  --skip-fio          Skip the raw disk baseline
  --skip-snapshot     Reuse the snapshots already in --db-dir
  --dry-run           1M keys, 1,000 sync writes and a short fio run, unless set otherwise
  --report-only       Only re-check and re-summarise the results in --output-dir
  -h, --help          Show this message
EOF
}

die() {
  echo "Error: $*" >&2
  exit 1
}

parse_human_number() {
  local upper
  upper="$(echo "$1" | tr '[:lower:]' '[:upper:]')"
  case "$upper" in
    *K) echo "$((${upper%K} * 1000))" ;;
    *M) echo "$((${upper%M} * 1000000))" ;;
    *G) echo "$((${upper%G} * 1000000000))" ;;
    *) echo "$upper" ;;
  esac
}

NUM_SET=0
SYNC_NUM_SET=0
for arg in "$@"; do
  case "$arg" in
    --num=*) NUM="$(parse_human_number "${arg#*=}")"; NUM_SET=1 ;;
    --sync-num=*) SYNC_NUM="$(parse_human_number "${arg#*=}")"; SYNC_NUM_SET=1 ;;
    --ycsb-ops=*) YCSB_OPS="$(parse_human_number "${arg#*=}")" ;;
    --threads=*) THREADS="${arg#*=}" ;;
    --runs=*) RUNS="${arg#*=}" ;;
    --tier=*) TIER="${arg#*=}" ;;
    --memory-limit=*) MEMORY_LIMIT="${arg#*=}" ;;
    --uid=*) RUNNER_UID="${arg#*=}" ;;
    --db-dir=*) DB_DIR="${arg#*=}" ;;
    --output-dir=*) OUTPUT_DIR="${arg#*=}" ;;
    --read-ahead-kb=*) READ_AHEAD_KB="${arg#*=}" ;;
    --workload-timeout=*) WORKLOAD_TIMEOUT="${arg#*=}" ;;
    --disk-engines=*) DISK_ENGINES="${arg#*=}" ;;
    --skip-build) SKIP_BUILD=1 ;;
    --skip-fio) SKIP_FIO=1 ;;
    --skip-snapshot) SKIP_SNAPSHOT=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --report-only) REPORT_ONLY=1 ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $arg" ;;
  esac
done

case "$TIER" in
  all | memory | disk) ;;
  *) die "--tier must be all, memory or disk, not $TIER" ;;
esac
for eng in ${DISK_ENGINES//,/ }; do
  case "$eng" in
    strata | leveldb | leveldb-nommap) ;;
    *) die "--disk-engines has an unknown engine: $eng" ;;
  esac
done
[ -n "${DISK_ENGINES//,/}" ] || die "--disk-engines is empty"

if [ "$DRY_RUN" -eq 1 ]; then
  [ "$NUM_SET" -eq 1 ] || NUM=1000000
  [ "$SYNC_NUM_SET" -eq 1 ] || SYNC_NUM=1000
  FIO_SECONDS=5
fi
[ -n "$YCSB_OPS" ] || YCSB_OPS="$NUM"
[ "$SYNC_NUM" -le "$NUM" ] || SYNC_NUM="$NUM"
if [ -z "$MEMORY_LIMIT" ]; then
  if [ "$NUM" -ge 5000000 ]; then MEMORY_LIMIT=512M; else MEMORY_LIMIT=64M; fi
fi
[ -n "$RUNNER_UID" ] || RUNNER_UID="${SUDO_USER:-$(id -un)}"

mkdir -p "$OUTPUT_DIR/logs"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
MEMORY_CSV="${OUTPUT_DIR}/memory.csv"
DISK_CSV="${OUTPUT_DIR}/disk.csv"

summarize() {
  local failed=0
  if [ -f "$MEMORY_CSV" ]; then
    awk -f "${SCRIPT_DIR}/check_ycsb_mix.awk" "$MEMORY_CSV" || {
      echo "YCSB mix check FAILED" >&2
      failed=1
    }
  fi
  python3 "${SCRIPT_DIR}/summarize_results.py" "$OUTPUT_DIR" --runs="$RUNS" || failed=1
  return "$failed"
}

if [ "$REPORT_ONLY" -eq 1 ]; then
  summarize
  exit 0
fi

[ -n "$DB_DIR" ] || die "--db-dir is required: without it the run could land on the wrong disk"
mkdir -p "$DB_DIR"
DB_DIR="$(cd "$DB_DIR" && pwd)"

echo "================================================================================"
echo "Keys:         $NUM"
echo "Sync writes:  $SYNC_NUM"
echo "YCSB ops:     $YCSB_OPS"
echo "Threads:      $THREADS"
echo "Runs:         $RUNS"
echo "Tier:         $TIER"
echo "Memory limit: $MEMORY_LIMIT, run as $RUNNER_UID"
echo "Databases:    $DB_DIR"
echo "Results:      $OUTPUT_DIR"
echo "================================================================================"

if [ "$SKIP_BUILD" -eq 0 ]; then
  cmake --preset bench -S "$REPO_ROOT"
  cmake --build "${REPO_ROOT}/build/bench" --target strata_bench
fi
BENCH_BIN="${REPO_ROOT}/build/bench/strata_bench"
[ -x "$BENCH_BIN" ] || die "$BENCH_BIN not found"

# The run can't mean anything without these, so they are checked before any time is spent.
if [ "$(id -u)" -ne 0 ]; then
  sudo -n true 2> /dev/null || die "this script needs passwordless sudo"
fi
if [ "$TIER" != memory ]; then
  command -v systemd-run > /dev/null || die "the disk tier needs systemd-run"
fi

READ_AHEAD_FILE=""
ORIGINAL_READ_AHEAD=""
READ_AHEAD_NOTE="unknown"
set_read_ahead() {
  local source device
  source="$(findmnt -no SOURCE -T "$DB_DIR" 2> /dev/null || true)"
  device="$(lsblk -ndo PKNAME "$source" 2> /dev/null || true)"
  [ -n "$device" ] || device="$(basename "${source:-unknown}")"
  if [ ! -w "/sys/block/${device}/queue/read_ahead_kb" ] && [ ! -e "/sys/block/${device}/queue/read_ahead_kb" ]; then
    echo "WARNING: can't find the read-ahead setting for ${source:-the database disk}." >&2
    return 0
  fi
  READ_AHEAD_FILE="/sys/block/${device}/queue/read_ahead_kb"
  ORIGINAL_READ_AHEAD="$(cat "$READ_AHEAD_FILE")"
  echo "$READ_AHEAD_KB" | sudo tee "$READ_AHEAD_FILE" > /dev/null
  [ "$(cat "$READ_AHEAD_FILE")" = "$READ_AHEAD_KB" ] || die "could not set read-ahead on $device"
  READ_AHEAD_NOTE="${READ_AHEAD_KB} KB on ${device}, was ${ORIGINAL_READ_AHEAD} KB"
  echo "Read-ahead:   $READ_AHEAD_NOTE"
}

restore_read_ahead() {
  if [ -n "$READ_AHEAD_FILE" ]; then
    echo "$ORIGINAL_READ_AHEAD" | sudo tee "$READ_AHEAD_FILE" > /dev/null || true
    READ_AHEAD_FILE=""
  fi
}

drop_caches() {
  sync
  if [ "$(id -u)" -eq 0 ]; then
    echo 3 > /proc/sys/vm/drop_caches
  else
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  fi
}

record_environment() {
  local dir_source root_source
  dir_source="$(findmnt -no SOURCE -T "$DB_DIR" 2> /dev/null || echo unknown)"
  root_source="$(findmnt -no SOURCE -T / 2> /dev/null || echo unknown)"
  {
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "args: $*"
    echo "commit: $(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2> /dev/null || echo unknown)"
    echo "uncommitted files: $(git -C "$REPO_ROOT" status --porcelain 2> /dev/null | wc -l)"
    echo "memory limit: $MEMORY_LIMIT"
    echo "read-ahead: $READ_AHEAD_NOTE"
    echo "workload time limit: ${WORKLOAD_TIMEOUT}s"
    echo "database disk: $dir_source (root filesystem: $root_source)"
    echo; uname -a
    echo; grep PRETTY_NAME /etc/os-release || true
    echo; gcc --version | head -1 || true
    cmake --version | head -1 || true
    echo; lscpu
    echo; free -h
    echo; lsblk -o NAME,SIZE,TRAN,MODEL,MOUNTPOINT || true
    echo; findmnt -T "$DB_DIR" || true
    echo; grep -m1 "LevelDB Build" "${OUTPUT_DIR}"/logs/*.log 2> /dev/null || true
  } > "${OUTPUT_DIR}/environment.txt"
  if [ "$dir_source" = "$root_source" ]; then
    echo "WARNING: --db-dir is on the root filesystem ($dir_source)." >&2
    echo "         On a cloud VM that is usually the remote OS disk, not local storage." >&2
  fi
}

# Reads and writes run one after the other: --stonewall stops them sharing the disk.
run_fio() {
  local log="${OUTPUT_DIR}/fio_baseline.txt"
  local file="${DB_DIR}/fio_test.dat"
  if ! command -v fio > /dev/null; then
    echo "WARNING: fio is not installed, so there is no raw disk baseline." >&2
    return 0
  fi
  echo "--- fio baseline"
  fio --filename="$file" --size=1G --bs=4k --direct=1 --iodepth=1 --time_based \
      --runtime="${FIO_SECONDS}s" \
      --name=randread --rw=randread \
      --name=randwrite --rw=randwrite --stonewall \
      > "$log" 2>&1 || die "fio failed, see $log"
  rm -f "$file"
}

engine_dir() {
  if [ "$1" = strata ]; then echo strata; else echo leveldb; fi
}

build_snapshots() {
  local eng tmp_csv
  tmp_csv="$(mktemp)"
  for eng in strata leveldb; do
    echo "--- building the $eng snapshot"
    rm -rf "${DB_DIR:?}/${eng}" "${DB_DIR:?}/${eng}.snapshot"
    # The one YCSB operation makes the harness settle compactions before it closes the database.
    "$BENCH_BIN" --engine="$eng" --benchmarks=fillrandom,ycsbc --ycsb_ops=1 --num="$NUM" \
        --threads="$THREADS" --db="$DB_DIR" --csv="$tmp_csv" > /dev/null
    cp -a "${DB_DIR}/${eng}" "${DB_DIR}/${eng}.snapshot"
    [ -f "${DB_DIR}/${eng}.snapshot/.bench_dataset" ] || die "no dataset marker in the $eng snapshot"
  done
  rm -f "$tmp_csv"
}

is_fresh_workload() {
  case "$1" in
    fillseq | fillrandom | fillsync) return 0 ;;
    *) return 1 ;;
  esac
}

is_write_workload() {
  case "$1" in
    fillseq | fillrandom | overwrite | fillsync | readwhilewriting | ycsba | ycsbb) return 0 ;;
    *) return 1 ;;
  esac
}

run_workload() {
  local tier="$1" run="$2" eng="$3" workload="$4" csv="$5"
  local dir log status=0
  dir="$(engine_dir "$eng")"
  log="${OUTPUT_DIR}/logs/${tier}_${workload}_run${run}_${eng}.log"

  rm -rf "${DB_DIR:?}/${dir}"
  if ! is_fresh_workload "$workload"; then
    cp -a "${DB_DIR}/${dir}.snapshot" "${DB_DIR}/${dir}"
  fi
  if [ "$tier" = disk ]; then
    drop_caches
  elif ! is_fresh_workload "$workload"; then
    sync
    cat "${DB_DIR}/${dir}"/* > /dev/null
  fi

  local -a args=(
    "--engine=$(if [ "$eng" = strata ]; then echo strata; else echo leveldb; fi)"
    "--benchmarks=${workload}" "--num=${NUM}" "--threads=${THREADS}" "--db=${DB_DIR}"
    "--csv=${csv}"
  )
  [ "$eng" != leveldb-nommap ] || args+=("--leveldb_mmap=0")
  [ "$workload" != fillsync ] || args+=("--sync_num=${SYNC_NUM}")
  is_write_workload "$workload" && args+=("--stats")
  case "$workload" in
    ycsb*) args+=("--ycsb_ops=${YCSB_OPS}") ;;
  esac

  echo "  [$tier | run $run | $eng | $workload]"
  local -a limited=(timeout --kill-after=30 "$WORKLOAD_TIMEOUT" "$BENCH_BIN" "${args[@]}")
  if [ "$tier" = disk ]; then
    sudo systemd-run --scope --quiet --uid="$RUNNER_UID" -p "MemoryMax=${MEMORY_LIMIT}" \
        -p MemorySwapMax=0 "${limited[@]}" > "$log" 2>&1 || status=$?
  else
    "${limited[@]}" > "$log" 2>&1 || status=$?
  fi
  if [ "$status" -ne 0 ]; then
    tail -n 5 "$log" >&2
    if [ "$status" -eq 124 ]; then
      die "$eng $workload passed the time limit of ${WORKLOAD_TIMEOUT}s. Something is wrong with this run."
    fi
    if [ "$status" -eq 137 ]; then
      die "$eng $workload was killed, most likely out of memory. Try a larger --memory-limit."
    fi
    die "$eng $workload exited with status $status, see $log"
  fi
}

MEMORY_WORKLOADS=(fillseq fillrandom overwrite fillsync readseq readrandom readmissing seekrandom
                  scan ycsbc ycsbb ycsba readwhilewriting)
DISK_WORKLOADS=(readrandom readmissing seekrandom scan ycsbc)

# Workloads that only read also run LevelDB with mmap off. Engine order alternates between runs.
engines_for() {
  local tier="$1" workload="$2" run="$3" eng
  local -a candidates=(strata leveldb) engines=()
  case "$workload" in
    readrandom | readmissing | seekrandom | scan | ycsbc) candidates+=(leveldb-nommap) ;;
  esac
  for eng in "${candidates[@]}"; do
    if [ "$tier" != disk ] || [[ ",${DISK_ENGINES}," == *",${eng},"* ]]; then
      engines+=("$eng")
    fi
  done
  if [ $((run % 2)) -eq 0 ]; then
    local i
    for ((i = ${#engines[@]} - 1; i >= 0; i--)); do echo "${engines[i]}"; done
  else
    printf '%s\n' "${engines[@]}"
  fi
}

run_tier() {
  local tier="$1" csv="$2"
  shift 2
  local run workload eng
  rm -f "$csv" "${OUTPUT_DIR}/logs/${tier}_"*.log
  for ((run = 1; run <= RUNS; run++)); do
    echo "--- $tier tier, run $run of $RUNS"
    for workload in "$@"; do
      for eng in $(engines_for "$tier" "$workload" "$run"); do
        run_workload "$tier" "$run" "$eng" "$workload" "$csv"
      done
    done
  done
}

IOSTAT_PID=""
stop_iostat() {
  if [ -n "$IOSTAT_PID" ]; then
    kill "$IOSTAT_PID" 2> /dev/null || true
    wait "$IOSTAT_PID" 2> /dev/null || true
    IOSTAT_PID=""
  fi
}
trap 'stop_iostat; restore_read_ahead' EXIT
trap 'exit 143' TERM INT

# Shows whether a run is sitting at the disk's IOPS or throughput cap.
start_iostat() {
  if command -v iostat > /dev/null; then
    iostat -xmt 5 > "${OUTPUT_DIR}/iostat_disk.log" &
    IOSTAT_PID=$!
  else
    echo "WARNING: iostat is not installed, so disk caps can't be checked afterwards." >&2
  fi
}

set_read_ahead
record_environment "$@"
[ "$SKIP_FIO" -eq 1 ] || run_fio
[ "$SKIP_SNAPSHOT" -eq 1 ] || build_snapshots

if [ "$TIER" != disk ]; then
  run_tier memory "$MEMORY_CSV" "${MEMORY_WORKLOADS[@]}"
fi
if [ "$TIER" != memory ]; then
  start_iostat
  run_tier disk "$DISK_CSV" "${DISK_WORKLOADS[@]}"
  stop_iostat
fi

record_environment "$@"
summarize
echo "Done. Results are in $OUTPUT_DIR"
