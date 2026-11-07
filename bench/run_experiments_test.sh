#!/usr/bin/env bash
# Runs run_experiments.sh end to end at a tiny size and checks every CSV has the rows it should.
# Also checks that run_benchmarks.sh --disk-engines leaves out the engines it should.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
chmod 755 "$WORK"

expect_rows() {  # file expected
  local actual
  actual="$(($(wc -l < "$1") - 1))"
  if [ "$actual" -ne "$2" ]; then
    echo "FAILED: $(basename "$1") has $actual rows, expected $2"
    exit 1
  fi
  echo "ok: $(basename "$1") has $2 rows"
}

bash "$SCRIPT_DIR/run_experiments.sh" --db-dir="$WORK/db" --output-dir="$WORK/out" --num=5000 \
    --sync-num=100 --runs=2 --memory-limit=64M --bloom-bits="0 10" --thread-counts="1 2" \
    --cache-sizes="1MB 8MB" > "$WORK/log" 2>&1 || {
  cat "$WORK/log"
  echo "FAILED: run_experiments.sh"
  exit 1
}
expect_rows "$WORK/out/exp1_bloom.csv" 8         # 2 sizes, 2 engines, 2 runs
expect_rows "$WORK/out/exp2_group_commit.csv" 12 # 2 thread counts, 3 setups, 2 runs
expect_rows "$WORK/out/exp3_summary.csv" 4       # 2 engines, 2 runs
expect_rows "$WORK/out/exp4_block_cache.csv" 4   # 2 sizes, 2 engines, 1 run
for run in 1 2; do
  for engine in strata leveldb; do
    [ -s "$WORK/out/exp3_timeseries_${engine}_run${run}.csv" ] || {
      echo "FAILED: no time series for $engine run $run"
      exit 1
    }
  done
done

if bash "$SCRIPT_DIR/run_experiments.sh" --db-dir="$WORK/db" --output-dir="$WORK/out" --nosuch=1 \
    > /dev/null 2>&1; then
  echo "FAILED: run_experiments.sh accepted an unknown option"
  exit 1
fi

bash "$SCRIPT_DIR/run_benchmarks.sh" --skip-build --skip-fio --tier=disk --runs=1 --num=5000 \
    --memory-limit=64M --disk-engines=strata,leveldb-nommap --db-dir="$WORK/db2" \
    --output-dir="$WORK/out2" > "$WORK/log2" 2>&1 || {
  cat "$WORK/log2"
  echo "FAILED: run_benchmarks.sh --disk-engines"
  exit 1
}
if cut -d, -f3 "$WORK/out2/disk.csv" | grep -qx LevelDB; then
  echo "FAILED: --disk-engines ran an engine it should have left out"
  exit 1
fi
expect_rows "$WORK/out2/disk.csv" 10  # 5 workloads, 2 engines, 1 run

if bash "$SCRIPT_DIR/run_benchmarks.sh" --skip-build --disk-engines=nosuch --db-dir="$WORK/db2" \
    > /dev/null 2>&1; then
  echo "FAILED: run_benchmarks.sh accepted an unknown engine"
  exit 1
fi
echo "run_experiments_test passed"
