#!/usr/bin/env bash
# Checks what strata_bench reports for the experiments: group commit, block cache hits and the
# time series. The unit tests cover the engine, and this covers both engines through the harness.
#
#   bench/strata_bench_test.sh [path/to/strata_bench]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${1:-${SCRIPT_DIR}/../build/bench/strata_bench}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "FAILED: $*"
  exit 1
}

# Prints one column of a CSV's last row whose engine and workload match.
column() {  # file column engine workload
  awk -F, -v want="$2" -v engine="$3" -v workload="$4" '
    NR == 1 { for (i = 1; i <= NF; i++) if ($i == want) col = i; next }
    $3 == engine && $4 == workload { value = $col }
    END { print value }' "$1"
}

expect_rejected() {  # reason, then arguments
  local reason="$1" status=0
  shift
  "$BIN" "$@" > /dev/null 2>&1 || status=$?
  [ "$status" -eq 2 ] || fail "$reason: expected exit status 2, got $status"
  echo "ok: rejects $reason"
}

expect_rejected "--group_commit=2" --group_commit=2 --engine=strata --benchmarks=fillsync
expect_rejected "--group_commit=0 with LevelDB" --group_commit=0 --engine=leveldb --benchmarks=fillsync
expect_rejected "--group_commit=0 with both engines" --group_commit=0 --benchmarks=fillsync
expect_rejected "--timeseries with no write workload" --timeseries="$WORK/x.csv" --benchmarks=readrandom
expect_rejected "--timeseries_interval_ms=0" --timeseries_interval_ms=0 --benchmarks=fillsync

echo "--- group commit"
CSV="$WORK/group.csv"
"$BIN" --engine=strata --group_commit=0 --threads=4 --sync_num=200 --benchmarks=fillsync \
    --db="$WORK/db_group" --csv="$CSV" > /dev/null
[ "$(column "$CSV" group_commit StrataDB fillsync)" = 0 ] || fail "group_commit should be recorded as 0"
"$BIN" --engine=strata --threads=4 --sync_num=200 --benchmarks=fillsync --db="$WORK/db_group" \
    --csv="$CSV" > /dev/null
[ "$(column "$CSV" group_commit StrataDB fillsync)" = 1 ] || fail "group_commit should be recorded as 1"
echo "ok: group_commit is recorded"

echo "--- block cache"
NUM=100000
DB="$WORK/db_cache"
"$BIN" --num="$NUM" --benchmarks=fillrandom --db="$DB" --csv="$WORK/fill.csv" > /dev/null
cache_run() {  # cache size, then extra arguments; prints the path of a CSV of its own
  local size="$1" csv
  shift
  csv="$(mktemp "$WORK/cache_XXXXXX")"
  rm -f "$csv"
  "$BIN" --num="$NUM" --cache_size="$size" --benchmarks=readrandom --db="$DB" --csv="$csv" "$@" > /dev/null
  echo "$csv"
}

small="$(cache_run 1MB --engine=strata)"
large="$(cache_run 64MB --engine=strata)"
none="$(cache_run 0 --engine=strata)"
small_hits="$(column "$small" cache_hits StrataDB readrandom)"
small_misses="$(column "$small" cache_misses StrataDB readrandom)"
large_hits="$(column "$large" cache_hits StrataDB readrandom)"
large_misses="$(column "$large" cache_misses StrataDB readrandom)"
[ "$small_hits" -gt 0 ] && [ "$small_misses" -gt 0 ] || fail "1 MB cache: $small_hits hits, $small_misses misses"
[ "$large_hits" -gt "$small_hits" ] || fail "64 MB cache should hit more than 1 MB: $large_hits vs $small_hits"
[ "$large_misses" -lt "$small_misses" ] || fail "64 MB cache should miss less than 1 MB"
[ "$(column "$none" cache_hits StrataDB readrandom)" = 0 ] || fail "no cache can't have hits"
[ "$(column "$none" cache_misses StrataDB readrandom)" = 0 ] || fail "no cache makes no lookups"
echo "ok: StrataDB hit rate rises with cache size: $small_hits then $large_hits hits"

mapped="$(cache_run 64MB --engine=leveldb)"
pread="$(cache_run 64MB --engine=leveldb --leveldb_mmap=0)"
[ "$(column "$mapped" cache_hits LevelDB readrandom)" = 0 ] || fail "mmap reads should never hit the block cache"
[ "$(column "$mapped" cache_misses LevelDB readrandom)" -gt 0 ] || fail "mmap reads should still look in the cache"
[ "$(column "$pread" cache_hits LevelDB-nommap readrandom)" -gt 0 ] || fail "pread reads should hit the block cache"
echo "ok: LevelDB only hits its block cache without mmap"

echo "--- time series"
SERIES="$WORK/series.csv"
"$BIN" --num=200000 --benchmarks=fillrandom --timeseries="$SERIES" --timeseries_interval_ms=50 \
    --db="$WORK/db_series" --csv="$WORK/series_summary.csv" > /dev/null
[ "$(head -1 "$SERIES")" = "run_id,engine,workload,threads,elapsed_sec,ops_done,ops_per_sec,l0_files,level_files" ] \
    || fail "time series header"
for engine in StrataDB LevelDB; do
  awk -F, -v engine="$engine" -v num=200000 '
    $2 != engine { next }
    {
      rows++
      if ($5 + 0 < last_time || $6 + 0 < last_ops) { print "went backwards at row " rows; bad = 1 }
      split($9, levels, "/")
      if (levels[1] != $8) { print "l0_files does not match level_files at row " rows; bad = 1 }
      last_time = $5 + 0
      last_ops = $6 + 0
    }
    END {
      if (rows < 2) { print "only " rows " rows"; bad = 1 }
      if (last_ops != num) { print "last row has " last_ops " ops, not " num; bad = 1 }
      exit bad
    }' "$SERIES" || fail "time series for $engine"
  echo "ok: $engine time series"
done

# A dataset built as setup isn't the workload being measured, so it adds no rows.
SETUP="$WORK/setup.csv"
"$BIN" --engine=strata --num=20000 --benchmarks=fillsync,readrandom --sync_num=50 --timeseries="$SETUP" \
    --db="$WORK/db_setup" --csv="$WORK/setup_summary.csv" > /dev/null
if tail -n +2 "$SETUP" | grep -q ',fillrandom,'; then
  fail "building a dataset added rows to the time series"
fi
echo "ok: dataset builds stay out of the time series"

echo "strata_bench_test passed"
