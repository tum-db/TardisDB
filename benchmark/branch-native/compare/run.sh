#!/bin/bash
# TardisDB and Dolt through their SQL servers (compare.py): the chain, fan-out and loop benchmarks.
# Usage: [NATIVE=1] benchmark/branch-native/compare/run.sh <output directory> [traces directory, default results/paper]
# TardisDB's pgserver is built natively with LLVM 18 (NATIVE=1, LLVM_VERSION) or with LLVM 7 in the container of
# Dockerfile (needs Docker) and runs there. Needs a Dolt binary (DOLT, e.g. from https://github.com/dolthub/dolt/releases) and a Python with psycopg2 and
# pymysql (PYTHON). Environment: CORES (the servers, 0-17), CLIENT_CORES (the driver, 18-35), THREADS (18),
# N (1000000), DEPTHS, KS, RUNS (5), MODES (chain fanout loops), TOPOLOGIES (mcts beam bisect).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(git -C "$HERE" rev-parse --show-toplevel)
OUT=$(realpath -m "${1:?output directory}")
TRACES=$(realpath "${2:-$HERE/../results/paper}")
DOLT=${DOLT:?DOLT names the dolt binary}
PYTHON=${PYTHON:-python3}
CORES=${CORES:-0-17}
CLIENT_CORES=${CLIENT_CORES:-18-35}
N=${N:-1000000}
mkdir -p "$OUT/build" "$OUT/dolt"

if [ -n "${NATIVE:-}" ]; then
  echo "building pgserver (LLVM ${LLVM_VERSION:-18}, INTERVAL_VISIBILITY=ON)"
  (cd "$OUT/build" && cmake -Wno-dev -DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=ON -DTARDISDB_LLVM_VERSION="${LLVM_VERSION:-18}" "$SRC" >/dev/null \
    && make -j${JOBS:-16} pgserver >make.log 2>&1)
  taskset -c "$CORES" "$OUT/build/pgserver" -h 127.0.0.1 -p 55433 -t "${THREADS:-18}" >"$OUT/build/pgserver.log" 2>&1 &
  pgserver_pid=$!
  stop_tardisdb() { kill "$pgserver_pid" 2>/dev/null || true; }
else
  docker build -q -t tardisdb-dev:20.04 "$HERE/.." >/dev/null
  echo "building pgserver (LLVM 7, INTERVAL_VISIBILITY=ON)"
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$SRC:/src:ro" -v "$OUT/build:/build" tardisdb-dev:20.04 \
    bash -c "cd /build && cmake -DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=ON /src >/dev/null && make -j${JOBS:-16} pgserver >make.log 2>&1"
  container=$(docker run -d --rm --network host --cpuset-cpus="$CORES" --user "$(id -u):$(id -g)" -v "$OUT/build:/build" \
    tardisdb-dev:20.04 /build/pgserver -h 127.0.0.1 -p 55433 -t "${THREADS:-18}")
  stop_tardisdb() { docker stop "$container" >/dev/null 2>&1 || true; }
fi
(cd "$OUT/dolt" && HOME="$OUT/dolt" "$DOLT" config --global --add user.email bench@localhost >/dev/null 2>&1 || true
 cd "$OUT/dolt" && HOME="$OUT/dolt" "$DOLT" config --global --add user.name bench >/dev/null 2>&1 || true)
(cd "$OUT/dolt" && HOME="$OUT/dolt" exec taskset -c "$CORES" "$DOLT" sql-server --host 127.0.0.1 --port 33306 >"$OUT/dolt/server.log" 2>&1) &
dolt_pid=$!
trap 'stop_tardisdb; kill "$dolt_pid" 2>/dev/null || true' EXIT
sleep 5

drive() {  # drive <output file> <compare.py arguments...>
  local out=$1; shift
  taskset -c "$CLIENT_CORES" "$PYTHON" -I "$HERE/compare.py" --n "$N" "$@" > "$out"
}
for system in tardisdb dolt; do
  for mode in ${MODES:-chain fanout loops}; do
    echo "$mode on $system"
    case $mode in
      chain) drive "$OUT/compare_chain_$system.csv" --system $system --mode chain --depths "${DEPTHS:-0,1,2,4,8,16,32,64,128}" --runs "${RUNS:-5}" ;;
      fanout) drive "$OUT/compare_fanout_$system.csv" --system $system --mode fanout --ks "${KS:-1,4,16,64,256}" --runs "${RUNS:-5}" ;;
      loops) for topology in ${TOPOLOGIES:-mcts beam bisect}; do
               drive "$OUT/compare_loops_${topology}_$system.csv" --system $system --mode loops --mutations 1000 --trace "$TRACES/trace_$topology.sql"
             done ;;
    esac
  done
done
echo "results in $OUT"
