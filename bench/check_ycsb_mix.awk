# Checks a strata_bench CSV: every YCSB row has to have run the read/update mix
# its workload promises, to within 1%, and both engines have to have run A, B
# and C. Exits non-zero otherwise. Needs enough operations for 1% to be well
# outside chance: at 100,000 the 50/50 mix has a standard deviation of 0.16%.
#
#   awk -f bench/check_ycsb_mix.awk bench/results/benchmarks.csv

BEGIN {
  FS = ","
  read_share["ycsbc"] = 1.00
  read_share["ycsbb"] = 0.95
  read_share["ycsba"] = 0.50
}

NR == 1 {
  for (i = 1; i <= NF; i++) {
    column[$i] = i
  }
  next
}

$column["workload"] in read_share {
  workload = $column["workload"]
  reads = $column["read_ops"]
  updates = $column["update_ops"]
  seen[$column["engine"] "," workload] = 1

  share = (reads + updates > 0) ? reads / (reads + updates) : 0
  off = share - read_share[workload]
  if (off < 0) {
    off = -off
  }
  if (reads + updates != $column["num_ops"] || off > 0.01) {
    printf "%s %s: %d reads, %d updates, %d ops; expected %.0f%% reads\n", $column["engine"], \
        workload, reads, updates, $column["num_ops"], read_share[workload] * 100
    failed = 1
  }
}

END {
  engines["StrataDB"]
  engines["LevelDB"]
  for (engine in engines) {
    for (workload in read_share) {
      if (!((engine "," workload) in seen)) {
        printf "no row for %s %s\n", engine, workload
        failed = 1
      }
    }
  }
  exit failed
}
