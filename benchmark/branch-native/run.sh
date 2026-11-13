#!/bin/bash
# Reproduces the measurements of the paper from this checkout, natively (NATIVE=1, LLVM 18 or LLVM_VERSION) or in
# the container of Dockerfile (LLVM 7, the paper's numbers).
# Usage: [NATIVE=1] benchmark/branch-native/run.sh [output directory, default ./results-<date>] [phases, default "paper scaling plot"]
# Phases: paper (= depth crossbranch loops fork gc), depth, crossbranch, loops, fork, gc, scaling, extra, check (see bench.sh), and plot,
# which builds the figures of the paper from the output directory (needs pdflatex with pgfplots). Environment: CORES (cores of the paper runs, default 0-17), THREADS (18),
# ALL_CORES (cores of the scaling runs, 0-35), SCALING_THREADS ("1 2 4 8 16 18 32 36"), LOOP_RUNS (3), JOBS (build jobs).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(git -C "$HERE" rev-parse --show-toplevel)
OUT=$(realpath -m "${1:-results-$(date +%Y%m%d)}")
PHASES=${2:-paper scaling plot}
BUILD=$OUT/build
mkdir -p "$OUT" "$BUILD/lineage" "$BUILD/interval" "$BUILD/clineage" "$BUILD/lazy"

[ -n "${NATIVE:-}" ] || docker build -q -t tardisdb-dev:20.04 "$HERE" >/dev/null
container() {   # container <cores> <command...>: natively pinned to the cores, or in the container
  local cores=$1; shift
  if [ -n "${NATIVE:-}" ]; then
    taskset -c "$cores" env B1="$BUILD/lineage" B2="$BUILD/interval" B3="$BUILD/clineage" B4="$BUILD/lazy" OUT="$OUT" "$@"
    return
  fi
  docker run --rm --cpuset-cpus="$cores" --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e THREADS="${THREADS:-18}" -e SCALING_THREADS="${SCALING_THREADS:-1 2 4 8 16 18 32 36}" -e LOOP_RUNS="${LOOP_RUNS:-3}" \
    -v "$SRC:/src:ro" -v "$BUILD/lineage:/b1" -v "$BUILD/interval:/b2" -v "$BUILD/clineage:/b3" -v "$BUILD/lazy:/b4" -v "$OUT:/out" -v "$HERE:/scripts:ro" \
    tardisdb-dev:20.04 "$@"
}
BENCH=$([ -n "${NATIVE:-}" ] && echo "$HERE/bench.sh" || echo /scripts/bench.sh)
# lineage map (TardisDB), interval labels, the lineage compiled into the scans, and interval labels
# with lazy membership (no bitmap per branch)
start=$SECONDS
[ "$PHASES" = plot ] || for b in "b1 OFF OFF OFF" "b2 ON OFF OFF" "b3 OFF ON OFF" "b4 ON OFF ON"; do
  set -- $b
  echo "building $1 (INTERVAL_VISIBILITY=$2 COMPILED_LINEAGE=$3 LAZY_MEMBERSHIP=$4)"
  options="-DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=$2 -DCOMPILED_LINEAGE=$3 -DLAZY_MEMBERSHIP=$4"
  if [ -n "${NATIVE:-}" ]; then
    dir=$BUILD/$(case $1 in b1) echo lineage ;; b2) echo interval ;; b3) echo clineage ;; b4) echo lazy ;; esac)
    (cd "$dir" && cmake -Wno-dev $options -DTARDISDB_LLVM_VERSION="${LLVM_VERSION:-18}" "$SRC" >/dev/null && make -j${JOBS:-16} branchBench tester >make.log 2>&1)
  else
    container "${ALL_CORES:-0-35}" bash -c "cd /$1 && cmake $options /src >/dev/null && make -j${JOBS:-16} branchBench tester >make.log 2>&1"
  fi
done
[ "$PHASES" = plot ] || echo "builds: $(( (SECONDS - start) / 60 )) min"
for phase in $PHASES; do
  echo "running $phase"
  phase_start=$SECONDS
  case $phase in
    scaling) container "${ALL_CORES:-0-35}" $BENCH scaling && SCALING_THREADS="${SCALING_THREADS:-1 2 4 8 16 18 32 36}" python3 "$HERE/summarize.py" "$OUT" ;;
    check)   container "${ALL_CORES:-0-35}" $BENCH check | tee "$OUT/check.txt" ;;
    plot)    "$HERE/plots/plot.sh" "$OUT" ;;
    *)       container "${CORES:-0-17}" $BENCH "$phase" ;;
  esac
  echo "$phase: $(( (SECONDS - phase_start) / 60 )) min"
done
echo "results in $OUT ($(( (SECONDS - start) / 60 )) min in all)"
