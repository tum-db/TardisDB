#!/bin/bash
# Reproduces the measurements of TardisDB as is (one core, LLVM 7): results/depth/t1_asis*, t1_visbench*, results/fork/t1_asis.csv
# Usage: ./run.sh [TardisDB checkout at commit 0d805c4] [output directory, default ./results]
# Without a checkout, commit 0d805c4 is exported from the repository containing this script.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
RESULTS=$(realpath -m "${2:-results}")
if [ -n "${1:-}" ]; then
  cp -r "$(realpath "$1")" "$WORK/src"
else
  mkdir "$WORK/src" && git -C "$(git -C "$HERE" rev-parse --show-toplevel)" archive 0d805c4 | tar -x -C "$WORK/src"
fi

docker build -t tardisdb-dev:20.04 "$HERE/.."

(cd "$WORK/src" && patch -p1 < "$HERE/tardisdb.patch")
cp "$HERE/branchBench.cpp" "$WORK/src/benchmark/"

build() {   # build <dir> <extra cmake flags>
  mkdir -p "$WORK/$1"
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -v "$WORK/src:/src:ro" -v "$WORK/$1:/build" -w /build tardisdb-dev:20.04 \
    bash -c "cmake -DCMAKE_BUILD_TYPE=Release -DBRANCH_BITMAP_COLUMNS=256 $2 /src >/dev/null && make -j16 branchBench >/dev/null"
}
build asis ""
build hoist "-DHOIST_LINEAGE_COPY=ON"

mkdir -p "$RESULTS/depth" "$RESULTS/fork"
docker run --rm --cpuset-cpus=8 --user "$(id -u):$(id -g)" -v "$WORK/asis:/b1" -v "$WORK/hoist:/b2" -v "$RESULTS:/results" tardisdb-dev:20.04 bash -c '
for f in 0.01 0; do
  /b1/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 2>/dev/null | grep , > /results/depth/t1_asis_frac$f.csv
  /b2/branchBench --mode=depth --n=1000000 --frac=$f --runs=5 2>/dev/null | grep , > /results/depth/t1_asis_hoist_frac$f.csv
done
/b1/branchBench --mode=fork --sizes=10000,100000,1000000,10000000 --runs=7 2>/dev/null | grep , > /results/fork/t1_asis.csv'

g++ -O3 -march=native -std=c++17 -o "$WORK/visbench" "$HERE/../visbench.cpp"
taskset -c 9 "$WORK/visbench" 1000000 0.01 5 > "$RESULTS/depth/t1_visbench_frac0.01.csv"
taskset -c 9 "$WORK/visbench" 1000000 0 5 > "$RESULTS/depth/t1_visbench_frac0.csv"
