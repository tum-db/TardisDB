#!/bin/bash
# The experiments beyond the paper, with 18 threads on cores 0-17 (LLVM 18): eager (BUILD_EAGER) and lazy
# (BUILD_LAZY) membership, interval labels. Each experiment writes into its own directory of the output directory.
# Usage: BUILD_EAGER=<build dir> BUILD_LAZY=<build dir> experiments.sh <output directory> [experiments]
# Experiments (default all): fork depth agentic crossbranch diffmerge gc concurrent
set -u
OUT=$1
EXPERIMENTS=${2:-fork depth agentic crossbranch diffmerge gc concurrent}
T=18
for v in ${VARIANTS:-eager lazy}; do
  B=$([ $v = eager ] && echo $BUILD_EAGER || echo $BUILD_LAZY)/branchBench
  run() { taskset -c 0-17 $B "$@" --threads=$T 2>/dev/null | grep -E '^[^ ]+,'; }
  for e in $EXPERIMENTS; do
    mkdir -p $OUT/$e
    case $e in
    fork)  # latency of a fork over the table size
      run --mode=fork --sizes=10000,100000,1000000,10000000 --runs=7 > $OUT/fork/$v.csv ;;
    depth)  # scan of a leaf over the depth of the branch chain, by chain walk and with a version map
      for f in 0 0.01; do
        run --mode=depth --n=1000000 --frac=$f --runs=5 --depths=0,1,2,4,8,16,32,64,128,256 > $OUT/depth/${v}_frac$f.csv
        run --mode=depth --n=1000000 --frac=$f --runs=5 --version_map=1 --depths=0,1,2,4,8,16,32,64,128,256 > $OUT/depth/${v}_vmap_frac$f.csv
      done ;;
    agentic)  # agent loops in SQL (as bench.sh loops)
      { run --mode=agentic --topology=mcts --n=1000000 --iterations=500 --width=4 --mutations=1000
        run --mode=agentic --topology=beam --n=1000000 --iterations=50 --width=8 --mutations=1000 | tail -1
        run --mode=agentic --topology=bisect --n=1000000 --iterations=256 --mutations=1000 | tail -1
      } > $OUT/agentic/${v}.csv ;;
    crossbranch)  # aggregates over k branches: k queries, one query with k scans, the branch-shared scan
      run --mode=crossbranch --n=1000000 --frac=0.01 --runs=5 --ks=1,16,64,256,1024 --maxk_baseline=64 > $OUT/crossbranch/${v}_frac0.01.csv ;;
    diffmerge)  # DIFF and MERGE over the diverging tuples and over the table
      run --mode=diffmerge --n=1000000 --runs=3 > $OUT/diffmerge/$v.csv ;;
    gc)  # create-and-drop cycles with and without garbage collection, with and without a long-lived branch
      for gc in true false; do
        for keep in false true; do
          run --mode=cycles --n=1000000 --iterations=200 --gc=$gc --keep=$keep > $OUT/gc/${v}_gc${gc}_keep${keep}.csv
        done
      done ;;
    concurrent)  # clients that run statements at the same time: one at a time, latches per table and per
                 # branch, without and with version maps
      for l in serial table branch; do
        run --mode=concurrent --n=1000000 --seconds=5 --clients=1,2,4,8,16 --latching=$l > $OUT/concurrent/${v}_$l.csv
        [ $l = serial ] || run --mode=concurrent --n=1000000 --seconds=5 --clients=1,2,4,8,16 --latching=$l --version_map=1 > $OUT/concurrent/${v}_${l}_vmap.csv
      done ;;
    *) echo "unknown experiment $e" >&2; exit 1 ;;
    esac
  done
  echo "done $v"
done
