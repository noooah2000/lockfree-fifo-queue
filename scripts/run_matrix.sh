#!/usr/bin/env bash
set -euo pipefail

BIN=build/bench_queue
mkdir -p results

if [[ ! -x "$BIN" ]]; then
  echo "Building..."
  make -j >/dev/null
fi

impls=("hp" "ebr" "none" "mutex")
threads=(1 2 4 8)
payloads=(50 100 300)
duration=5
warmup=2

for impl in "${impls[@]}"; do
  for t in "${threads[@]}"; do
    csv="results/${impl}_p${t}_c${t}_100us.csv"
    "$BIN" --impl "$impl" --producers "$t" --consumers "$t" \
      --payload-us 100 --duration "$duration" --warmup "$warmup" \
      --csv "$csv"
  done
done

echo "Done. CSVs under results/."
