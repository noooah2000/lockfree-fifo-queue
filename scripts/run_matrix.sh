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
payloads=(10 50 100 300)
duration=5
warmup=2

# 測試不同執行緒數 (固定 payload=100us)
echo "Testing different thread counts (payload=100us)..."
for impl in "${impls[@]}"; do
  for t in "${threads[@]}"; do
    csv="results/${impl}_p${t}_c${t}_100us.csv"
    "$BIN" --impl "$impl" --producers "$t" --consumers "$t" \
      --payload-us 100 --duration "$duration" --warmup "$warmup" \
      --csv "$csv"
    echo "  ✓ $impl P=$t C=$t"
  done
done

# 測試不同 payload (固定執行緒數=2)
echo "Testing different payloads (P=2, C=2)..."
for impl in "${impls[@]}"; do
  for payload in "${payloads[@]}"; do
    csv="results/${impl}_payload${payload}us_p2_c2.csv"
    "$BIN" --impl "$impl" --producers 2 --consumers 2 \
      --payload-us "$payload" --duration "$duration" --warmup "$warmup" \
      --csv "$csv"
    echo "  ✓ $impl payload=${payload}us"
  done
done

echo "Done. CSVs under results/."
