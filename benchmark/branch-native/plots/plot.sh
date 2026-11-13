#!/bin/bash
# Builds the figures of the paper's evaluation from a results directory (the output of run.sh):
# <results directory>/figures.pdf. Needs pdflatex with pgfplots.
# Usage: benchmark/branch-native/plots/plot.sh <results directory>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath "$1")
python3 "$HERE/../summarize.py" "$OUT"
cd "$HERE"
for pass in 1 2; do  # the second pass resolves the references to the legends
  pdflatex -interaction=nonstopmode -jobname figures -output-directory "$OUT" "\\def\\datadir{$OUT}\\input{figures.tex}" > "$OUT/figures.log" \
    || { grep -A3 '^!' "$OUT/figures.log" >&2; exit 1; }
done
rm -f "$OUT/figures.aux" "$OUT/figures.log"
echo "$OUT/figures.pdf"
