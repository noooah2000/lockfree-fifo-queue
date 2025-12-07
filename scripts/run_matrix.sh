#!/usr/bin/env bash
set -euo pipefail

BIN=build/bench_queue
mkdir -p results
rm -f results/*

if [[ ! -x "$BIN" ]]; then
  echo "Building..."
  make -j >/dev/null
fi

impls=("hp" "ebr" "none" "mutex")
threads=(1 2 4 8 10)
payloads=(1 2 3 4 5 6 7 8 9 10)
duration=5
warmup=2

# 測試不同執行緒數 (固定 payload=5us)
echo "Testing different thread counts (payload=5us)..."
for impl in "${impls[@]}"; do
  for t in "${threads[@]}"; do
    csv="results/${impl}_p${t}_c${t}_5us.csv"
    "$BIN" --impl "$impl" --producers "$t" --consumers "$t" \
      --payload-us 5 --duration "$duration" --warmup "$warmup" \
      --csv "$csv"
    echo "  ✓ $impl P=$t C=$t"
  done
done

# 測試不同 payload (固定執行緒數=8)
echo "Testing different payloads (P=8, C=8)..."
for impl in "${impls[@]}"; do
  for payload in "${payloads[@]}"; do
    csv="results/${impl}_payload${payload}us_p8_c8.csv"
    "$BIN" --impl "$impl" --producers 8 --consumers 8 \
      --payload-us "$payload" --duration "$duration" --warmup "$warmup" \
      --csv "$csv"
    echo "  ✓ $impl payload=${payload}us"
  done
done

echo "Done. CSVs under results/."
