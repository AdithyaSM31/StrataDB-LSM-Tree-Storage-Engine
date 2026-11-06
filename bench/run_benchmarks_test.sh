#!/usr/bin/env bash
# A killed or stuck run has to stop run_benchmarks.sh with a failure, and read-ahead has to go back
# to what it was. A stub stands in for strata_bench: it builds snapshots normally, then exits with
# the given status, or hangs, on every measured workload.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/bench" "$WORK/build/bench"
cp "$SCRIPT_DIR/run_benchmarks.sh" "$SCRIPT_DIR/check_ycsb_mix.awk" "$SCRIPT_DIR/summarize_results.py" \
   "$WORK/bench/"
cat > "$WORK/build/bench/strata_bench" << 'EOF'
#!/usr/bin/env bash
for arg in "$@"; do
  case "$arg" in
    --db=*) db="${arg#*=}" ;;
    --engine=*) engine="${arg#*=}" ;;
    --benchmarks=*) workload="${arg#*=}" ;;
  esac
done
if [ "$workload" = "fillrandom,ycsbc" ]; then
  mkdir -p "$db/$engine"
  touch "$db/$engine/.bench_dataset"
  exit 0
fi
status="$(cat "$(dirname "$0")/stub_status")"
[ "$status" != hang ] || exec sleep 120
exit "$status"
EOF
chmod +x "$WORK/build/bench/strata_bench"
# Read from a file because sudo drops environment variables on the way to the stub.
chmod 755 "$WORK"

check() {  # tier status expected_message
  local tier="$1" status="$2" expected="$3" output rc=0
  echo "$status" > "$WORK/build/bench/stub_status"
  output="$(bash "$WORK/bench/run_benchmarks.sh" --tier="$tier" --runs=1 --num=1000 --skip-build \
      --skip-fio --workload-timeout=2 --read-ahead-kb=256 --db-dir="$WORK/db" \
      --output-dir="$WORK/out" 2>&1)" || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "FAILED: exit status $status in the $tier tier was reported as success"
    echo "$output"
    exit 1
  fi
  if ! grep -q "$expected" <<< "$output"; then
    echo "FAILED: the $tier tier didn't report \"$expected\":"
    echo "$output"
    exit 1
  fi
  if ! grep -q "Read-ahead:   256 KB" <<< "$output"; then
    echo "FAILED: the $tier tier didn't set read-ahead:"
    echo "$output"
    exit 1
  fi
  local device after
  device="$(grep -m1 -o '256 KB on [a-z0-9]*' <<< "$output" | awk '{print $4}')"
  before="$(grep -m1 -o 'was [0-9]* KB' <<< "$output" | awk '{print $2}')"
  after="$(cat "/sys/block/${device}/queue/read_ahead_kb")"
  if [ "$after" != "$before" ]; then
    echo "FAILED: read-ahead on $device is $after, it should be back to $before"
    exit 1
  fi
  echo "ok: $tier tier, $status"
}

check memory 1 "exited with status 1"
check memory 137 "out of memory"
check disk 137 "out of memory"
check memory hang "time limit"
check disk hang "time limit"
echo "run_benchmarks_test passed"
