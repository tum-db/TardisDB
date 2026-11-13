#!/bin/bash
# Run by run.sh, in the container of Dockerfile or natively: B1 = lineage build, B2 = interval build, B3 = lineage
# compiled into the scans (COMPILED_LINEAGE), B4 = interval build with lazy membership (LAZY_MEMBERSHIP), OUT = results
# (in the container /b1 to /b4 and /out).
# Usage: bench.sh <phase>...   phases: paper (= depth crossbranch loops fork gc), depth, crossbranch, loops,
# fork, gc, scaling, extra, check. THREADS: threads of all but the scaling runs; SCALING_THREADS: list for scaling.
set -euo pipefail
B1=${B1:-/b1} B2=${B2:-/b2} B3=${B3:-/b3} B4=${B4:-/b4} OUT=${OUT:-/out}
b() { local v=B$1; echo "${!v}"; }   # directory of build 1 to 4
T=${THREADS:-18}
csv() { grep -E '^[^ ]+,' || true; }   # CSV lines only, not the status messages of the statements

depth() {  # Fig. "chain": scan of the leaf of a branch chain over its depth
  for f in 0 0.01; do
    $B1/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 --threads=$T --depths=0,1,2,4,8,16,32,64,128,256 2>/dev/null | csv > $OUT/mt18_lineage_frac$f.csv
    $B2/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 --threads=$T --depths=0,1,2,4,8,16,32,64,128,256 2>/dev/null | csv > $OUT/mt18_interval_frac$f.csv
    $B3/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 --threads=$T --depths=0,1,2,4,8,16,32,64,128,256 2>/dev/null | csv > $OUT/mt18_clineage_frac$f.csv
    # the leaf with a version map, built before its scans
    $B2/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 --threads=$T --version_map=1 --depths=0,1,2,4,8,16,32,64,128,256 2>/dev/null | csv > $OUT/mt18_interval_vmap_frac$f.csv
  done
}
crossbranch() {  # Fig. "fan-out": sum per branch over k branches forked from main
  $B2/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --threads=$T 2>/dev/null | csv > $OUT/mt18_cb_interval_frac0.01.csv
  $B2/branchBench --mode=crossbranch --n=1000000 --frac=0.001 --runs=5 --threads=$T 2>/dev/null | csv > $OUT/mt18_cb_interval_frac0.001.csv
  $B3/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --threads=$T 2>/dev/null | csv > $OUT/mt18_cb_clineage_frac0.01.csv
  $B3/branchBench --mode=crossbranch --n=1000000 --frac=0.001 --runs=5 --threads=$T 2>/dev/null | csv > $OUT/mt18_cb_clineage_frac0.001.csv
}
loops() {  # Fig. "loops": the agent loops in SQL, per phase, LOOP_RUNS runs of the same statements (summarize.py
           # takes the median); the listing is an excerpt of trace_beam.sql
  for r in $(seq 1 ${LOOP_RUNS:-3}); do
    for v in 1 2 4; do
      name=$(case $v in 1) echo lineage ;; 2) echo interval ;; 4) echo lazy ;; esac)
      trace() { [ $v = 2 ] && [ $r = 1 ] && echo "--trace=$OUT/trace_$1.sql" || true; }
      { $(b $v)/branchBench --mode=agentic --topology=mcts --n=1000000 --iterations=500 --width=4 --mutations=1000 --threads=$T $(trace mcts) 2>/dev/null | csv
        $(b $v)/branchBench --mode=agentic --topology=beam --n=1000000 --iterations=50 --width=8 --mutations=1000 --threads=$T $(trace beam) 2>/dev/null | csv | tail -1
        $(b $v)/branchBench --mode=agentic --topology=bisect --n=1000000 --iterations=256 --mutations=1000 --threads=$T $(trace bisect) 2>/dev/null | csv | tail -1
      } > $OUT/loops_${name}_r$r.csv
    done
  done
}
fork() {  # the cost of a fork and the memory of membership: bitmaps per branch (B2) against lazy membership (B4)
  $B2/branchBench --mode=fork --sizes=10000,100000,1000000,10000000 --runs=7 2>/dev/null | csv > $OUT/fork_eager.csv
  $B4/branchBench --mode=fork --sizes=10000,100000,1000000,10000000 --runs=7 2>/dev/null | csv > $OUT/fork_lazy.csv
}
gc() {  # Fig. "gc": garbage collection in bisect, the versions after every snapshot and drop and the time of each,
        # and the agent loops at their end, each with and without collection
  for g in true false; do
    $B4/branchBench --mode=agentic --topology=bisect --n=1000000 --iterations=256 --mutations=1000 --threads=$T --gc=$g --memory=$OUT/gc_bisect_lazy_gc$g.csv >/dev/null 2>&1
  done
  for g in true false; do
    { $B4/branchBench --mode=agentic --topology=mcts --n=1000000 --iterations=500 --width=4 --mutations=1000 --threads=$T --gc=$g 2>/dev/null | csv
      $B4/branchBench --mode=agentic --topology=beam --n=1000000 --iterations=50 --width=8 --mutations=1000 --threads=$T --gc=$g 2>/dev/null | csv | tail -1
      $B4/branchBench --mode=agentic --topology=bisect --n=1000000 --iterations=256 --mutations=1000 --threads=$T --gc=$g 2>/dev/null | csv | tail -1
    } > $OUT/gc_loops_lazy_gc$g.csv
  done
}
scaling() {  # Fig. "scaling": the scans of the chain (all variants) and fan-out (labels, compiled lineage) benchmarks with 1 to 36 threads
  for t in ${SCALING_THREADS:-1 2 4 8 16 18 32 36}; do
    $B2/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --ks=1,16,64,256 --threads=$t 2>/dev/null | csv > $OUT/scaling_cb_t$t.csv
    $B3/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --ks=1,16,64,256 --threads=$t 2>/dev/null | csv > $OUT/scaling_cb_clineage_t$t.csv
    $B2/branchBench --mode=depth --n=1000000 --frac=0.01 --runs=5 --depths=1,16,128 --threads=$t 2>/dev/null | csv > $OUT/scaling_depth_t$t.csv
    $B1/branchBench --mode=depth --n=1000000 --frac=0.01 --runs=5 --depths=1,16,128 --threads=$t 2>/dev/null | csv > $OUT/scaling_depth_lineage_t$t.csv
    $B3/branchBench --mode=depth --n=1000000 --frac=0.01 --runs=5 --depths=1,16,128 --threads=$t 2>/dev/null | csv > $OUT/scaling_depth_clineage_t$t.csv
    $B2/branchBench --mode=depth --n=1000000 --frac=0.01 --runs=5 --version_map=1 --depths=1,16,128 --threads=$t 2>/dev/null | csv > $OUT/scaling_depth_vmap_t$t.csv
  done
}
extra() {  # measurements not plotted in the paper: min/max, the lineage map across branches, fork, ALTER TABLE
  $B2/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --threads=$T --ks=1,4,16,64,256,1024 --aggregates="count(*), sum(b), min(b), max(b)" 2>/dev/null | csv > $OUT/mt18_cb_interval_minmax_frac0.01.csv
  $B1/branchBench --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --threads=$T --ks=1,2,4,8,16,32,64,128,256 2>/dev/null | csv > $OUT/mt18_cb_lineage_frac0.01.csv
  $B1/branchBench --mode=fork --sizes=10000,100000,1000000,10000000 --runs=7 2>/dev/null | csv > $OUT/fork.csv
  $B2/branchBench --mode=schema --n=1000000 --frac=0.01 --runs=7 --threads=$T 2>/dev/null | csv > $OUT/schema.csv
}
check() {  # test suite and comparison with the reference model
  for v in 1 2 3 4; do
    (cd "$(b $v)" && ./tester --gtest_filter='-InteropTest*' | tail -1)
    $(b $v)/branchBench --mode=verify --n=2000 --ops=3000 --crossbranch --threads=1 | tail -1
    $(b $v)/branchBench --mode=verify --n=2000 --ops=3000 --crossbranch --threads=8 --morsel=64 | tail -1
    $(b $v)/branchBench --mode=verify --n=2000 --ops=3000 --crossbranch --threads=36 --morsel=32 --deep | tail -1
    $(b $v)/branchBench --mode=verify --n=2000 --ops=3000 --crossbranch --threads=8 --morsel=64 --schema | tail -1
  done
}
[ $# -gt 0 ] || { echo "usage: $0 paper|depth|crossbranch|loops|fork|gc|scaling|extra|check ..." >&2; exit 1; }
for phase in "$@"; do
  case $phase in
    paper) depth; crossbranch; loops; fork; gc ;;
    depth|crossbranch|loops|fork|gc|scaling|extra|check) $phase ;;
    *) echo "unknown phase $phase" >&2; exit 1 ;;
  esac
done
