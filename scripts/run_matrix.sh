#!/usr/bin/env bash
set -euo pipefail

# 定義路徑
BIN_DIR=./build
OUT_DIR=results
RUNS=5  # 定義每個實驗重複跑幾次

# 確保編譯完成
echo "[Script] Building all executables..."
make all -j4 > /dev/null

mkdir -p "$OUT_DIR"
# 清除舊結果
rm -f "$OUT_DIR"/*.csv

# 參數設定
impls=("hp" "ebr" "none" "mutex")
fixed_thread=8
threads=(1 2 3 4 5 6 7 8 9 10 11) 
fixed_payload=3        
payloads=(0 1 2 3 4 5 6)
duration=5                
warmup=1            

echo "=================================================="
echo "Starting Benchmark Matrix"
echo "Strategy: Interleaved Execution (Round-Robin)"
echo "Total Global Rounds: $RUNS"
echo "=================================================="

# 定義 Part 1 的 Binary
MAIN_MODE_NAME="pool_backoff"
MAIN_BIN="$BIN_DIR/bench_queue_${MAIN_MODE_NAME}"

# 定義 Part 2 的參數
CHECK_MODES=("nopool_nobackoff" "pool_nobackoff" "nopool_backoff" "pool_backoff")
CHECK_THREADS=8
CHECK_PAYLOAD=3

# 檢查 Binary 是否存在 (只檢查一次)
if [[ ! -x "$MAIN_BIN" ]]; then
    echo "Error: Binary '$MAIN_BIN' not found. Please check Makefile."
    exit 1
fi
for mode in "${CHECK_MODES[@]}"; do
    bin_path="$BIN_DIR/bench_queue_${mode}"
    if [[ ! -x "$bin_path" ]]; then
        echo "Error: Binary '$bin_path' not found."
        exit 1
    fi
done

# ==========================================================
# 大外層迴圈：全域輪次 (Global Round)
# 第 1 輪跑完所有組合 -> 第 2 輪跑完所有組合 -> ...
# ==========================================================
for (( run=1; run<=RUNS; run++ )); do
    echo ""
    echo "##################################################"
    echo "   GLOBAL ROUND: $run / $RUNS"
    echo "##################################################"

    # ==========================================================
    # Part 1: Main Matrix (Pool + Backoff)
    # ==========================================================
    echo ">>> [Round $run][Part 1] Running Main Matrix ($MAIN_MODE_NAME)"

    # 1.1 Scalability Test
    echo "    [Exp 1.1] Scalability (Threads vs Throughput)..."
    for impl in "${impls[@]}"; do
        for t in "${threads[@]}"; do
            csv="$OUT_DIR/${impl}_${MAIN_MODE_NAME}_threads_${t}.csv"
            
            # 執行一次 (C++ code 會用 append 模式寫入)
            "$MAIN_BIN" --impl "$impl" \
                   --producers "$t" --consumers "$t" \
                   --payload-us "$fixed_payload" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
    done
    echo "      [Part 1.1] All threads done for Round $run."

    # 1.2 Payload Sensitivity Test
    echo "    [Exp 1.2] Sensitivity (Payload vs Throughput)..."
    for impl in "${impls[@]}"; do
        for p in "${payloads[@]}"; do
            csv="$OUT_DIR/${impl}_${MAIN_MODE_NAME}_payload_${p}us.csv"
            
            # 執行一次
            "$MAIN_BIN" --impl "$impl" \
                   --producers "$fixed_thread" --consumers "$fixed_thread" \
                   --payload-us "$p" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
    done
    echo "      [Part 1.2] All payloads done for Round $run."

    # ==========================================================
    # Part 2: Spot Check (4 Modes Comparison)
    # ==========================================================
    echo ">>> [Round $run][Part 2] Running Spot Check (P=9, Payload=4us)"

    for mode in "${CHECK_MODES[@]}"; do
        CHECK_BIN="$BIN_DIR/bench_queue_${mode}"
        
        for impl in "${impls[@]}"; do
            csv="$OUT_DIR/${impl}_${mode}_spotcheck.csv"
            
            # 執行一次
            "$CHECK_BIN" --impl "$impl" \
                   --producers "$CHECK_THREADS" --consumers "$CHECK_THREADS" \
                   --payload-us "$CHECK_PAYLOAD" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
    done
    echo "      [Part 2] Spot checks done for Round $run."

done

echo "=================================================="
echo "Benchmark Finished. Results in $OUT_DIR/"
echo "Run 'python3 plot_results.py' to generate charts."