#!/usr/bin/env bash
set -euo pipefail

# 定義路徑
BIN_DIR=./build
OUT_DIR=results

# 確保編譯完成
echo "[Script] Building all executables..."
make all -j4 > /dev/null

mkdir -p "$OUT_DIR"
# 清除舊結果
rm -f "$OUT_DIR"/*.csv

# 定義兩種模式
MODES=("nopool" "pool")

# 參數設定
impls=("hp" "ebr" "none" "mutex")
threads=(1 2 3 5 7 9 11) # 模擬從低競爭到高競爭 (超過實體核數)
fixed_payload=3           # 固定負載 (微秒)，模擬真實工作量
payloads=(0 1 2 3 4 5 6)  # 測試不同負載對吞吐量的影響 (0=極限測試)
duration=5                # 執行時間
warmup=1                  # 暖身時間

echo "=================================================="
echo "Starting Benchmark Matrix (Pool vs No Pool)"
echo "=================================================="

# 外層迴圈：遍歷有無 Pool
for mode in "${MODES[@]}"; do
    BIN="$BIN_DIR/bench_queue_${mode}"
    
    if [[ ! -x "$BIN" ]]; then
        echo "Error: Binary '$BIN' not found."
        exit 1
    fi

    echo ">>> Running Mode: $mode"

    # 1. Scalability Test
    echo "    [Exp 1] Scalability (Threads vs Throughput)..."
    for impl in "${impls[@]}"; do
        for t in "${threads[@]}"; do
            # 檔名加入 mode 標籤
            csv="$OUT_DIR/${impl}_${mode}_threads_${t}.csv"
            "$BIN" --impl "$impl" \
                   --producers "$t" --consumers "$t" \
                   --payload-us "$fixed_payload" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
        echo "      [OK] $impl done."
    done

    # 2. Payload Sensitivity Test
    echo "    [Exp 2] Sensitivity (Payload vs Throughput)..."
    for impl in "${impls[@]}"; do
        for p in "${payloads[@]}"; do
            csv="$OUT_DIR/${impl}_${mode}_payload_${p}us.csv"
            "$BIN" --impl "$impl" \
                   --producers 8 --consumers 8 \
                   --payload-us "$p" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
        echo "      [OK] $impl done."
    done
done

echo "=================================================="
echo "Benchmark Finished. Results in $OUT_DIR/"
echo "Run 'python3 plot_results.py' to generate charts."