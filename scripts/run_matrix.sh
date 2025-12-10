#!/usr/bin/env bash
set -euo pipefail

# 確保你的 binary 路徑正確
BIN=./build/bench_queue 
OUT_DIR=results

mkdir -p "$OUT_DIR"
# 清除舊結果，避免圖表混亂
rm -f "$OUT_DIR"/*.csv

if [[ ! -x "$BIN" ]]; then
  echo "Error: Binary '$BIN' not found. Please build first."
  exit 1
fi

impls=("hp" "ebr" "none" "mutex")
threads=(1 2 4 6 8 10 12 16 20 24) # 模擬從低競爭到高競爭 (超過實體核數)
fixed_payload=3           # 固定負載 (微秒)，模擬真實工作量
payloads=(0 1 2 3 4 5 6)  # 測試不同負載對吞吐量的影響 (0=極限測試)
duration=5                # 執行時間
warmup=1                  # 暖身時間

echo "=================================================="
echo "Starting Benchmark Matrix"
echo "=================================================="

# 1. 測試不同執行緒數 Scalability (固定 payload)
echo "[Exp 1] Testing Scalability (Threads vs Throughput/Latency)..."
for impl in "${impls[@]}"; do
  for t in "${threads[@]}"; do
    csv="$OUT_DIR/${impl}_threads_${t}.csv"
    # P 和 C 數量相同
    "$BIN" --impl "$impl" \
           --producers "$t" --consumers "$t" \
           --payload-us "$fixed_payload" \
           --duration "$duration" --warmup "$warmup" \
           --csv "$csv"
    echo "  [OK] $impl | Threads: $t | Output: $csv"
  done
done

# 2. 測試不同 Payload (固定執行緒數=8)
echo "[Exp 2] Testing Payload Sensitivity (Payload vs Throughput)..."
for impl in "${impls[@]}"; do
  for p in "${payloads[@]}"; do
    # 避免重複跑已經跑過的組合 (例如 threads loop 可能跑過 payload=2)
    # 這裡為了簡單，全部重跑或是存成不同檔名
    csv="$OUT_DIR/${impl}_payload_${p}us.csv"
    "$BIN" --impl "$impl" \
           --producers 8 --consumers 8 \
           --payload-us "$p" \
           --duration "$duration" --warmup "$warmup" \
           --csv "$csv"
    echo "  [OK] $impl | Payload: ${p}us | Output: $csv"
  done
done

echo "=================================================="
echo "Benchmark Finished. Results in $OUT_DIR/"
echo "Run 'python3 plot_results.py' to generate charts."