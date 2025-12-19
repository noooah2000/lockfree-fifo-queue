#!/usr/bin/env bash
# ==============================================================================
# Benchmarking Driver Script
# ==============================================================================
# This script executes the performance benchmark suite for the Lock-Free Queue.
# It uses an interleaved (round-robin) execution strategy to minimize the impact
# of transient system noise or thermal throttling on specific configurations.
#
# Workflow:
# 1. Builds all necessary binaries.
# 2. Cleans up previous results.
# 3. Iterates through 'RUNS' global rounds.
# 4. In each round, executes:
#    a. Main Matrix: Scalability & Sensitivity tests using the optimized config.
#    b. Spot Checks: Component analysis across different optimization modes.
# ==============================================================================

set -euo pipefail

# Configuration: Paths
BIN_DIR=./build
OUT_DIR=results

# Configuration: Execution
RUNS=5  # Total number of global rounds (repetitions per experiment)

# Ensure the build is up-to-date
echo "[Script] Building all executables..."
make all -j4 > /dev/null

# Prepare output directory
mkdir -p "$OUT_DIR"
# Remove previous results to ensure clean append operations
rm -f "$OUT_DIR"/*.csv

# ==============================================================================
# Experiment Parameters
# ==============================================================================

# Implementations to test
impls=("hp" "ebr" "none" "mutex")

# Scalability Test Parameters (Varying Threads)
fixed_payload=3        
threads=(1 2 3 4 5 6 7 8 9 10 11) 

# Sensitivity Test Parameters (Varying Payload)
fixed_thread=8
payloads=(0 1 2 3 4 5 6)

# Time Settings
duration=5   # Measurement duration (seconds) per run
warmup=1     # Warmup duration (seconds) per run

# ==============================================================================
# Binary Definitions
# ==============================================================================

# Part 1: Main Matrix (Best Configuration: Object Pool + Backoff)
MAIN_MODE_NAME="pool_backoff"
MAIN_BIN="$BIN_DIR/bench_queue_${MAIN_MODE_NAME}"

# Part 2: Spot Check (Optimization Impact Analysis)
# Compare combinations of Memory Pool and Backoff strategies
CHECK_MODES=("nopool_nobackoff" "pool_nobackoff" "nopool_backoff" "pool_backoff")
CHECK_THREADS=8
CHECK_PAYLOAD=3

# Validation: Ensure binaries exist before starting the long loop
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

echo "=================================================="
echo "Starting Benchmark Matrix"
echo "Strategy: Interleaved Execution (Round-Robin)"
echo "Total Global Rounds: $RUNS"
echo "=================================================="

# ==============================================================================
# Main Execution Loop
# ==============================================================================
for (( run=1; run<=RUNS; run++ )); do
    echo ""
    echo "##################################################"
    echo "   GLOBAL ROUND: $run / $RUNS"
    echo "##################################################"

    # --------------------------------------------------------------------------
    # Part 1: Main Performance Matrix
    # Focus: Scalability and Payload Sensitivity using the optimized implementation.
    # --------------------------------------------------------------------------
    echo ">>> [Round $run][Part 1] Running Main Matrix ($MAIN_MODE_NAME)"

    # 1.1 Scalability Test: Throughput vs. Thread Count
    echo "    [Exp 1.1] Scalability (Threads vs Throughput)..."
    for impl in "${impls[@]}"; do
        for t in "${threads[@]}"; do
            csv="$OUT_DIR/${impl}_${MAIN_MODE_NAME}_threads_${t}.csv"
            
            # Execute benchmark (Results are appended to the CSV)
            "$MAIN_BIN" --impl "$impl" \
                   --producers "$t" --consumers "$t" \
                   --payload-us "$fixed_payload" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
    done
    echo "      [Part 1.1] All threads done for Round $run."

    # 1.2 Sensitivity Test: Throughput vs. Payload Size
    echo "    [Exp 1.2] Sensitivity (Payload vs Throughput)..."
    for impl in "${impls[@]}"; do
        for p in "${payloads[@]}"; do
            csv="$OUT_DIR/${impl}_${MAIN_MODE_NAME}_payload_${p}us.csv"
            
            # Execute benchmark
            "$MAIN_BIN" --impl "$impl" \
                   --producers "$fixed_thread" --consumers "$fixed_thread" \
                   --payload-us "$p" \
                   --duration "$duration" --warmup "$warmup" \
                   --csv "$csv"
        done
    done
    echo "      [Part 1.2] All payloads done for Round $run."

    # --------------------------------------------------------------------------
    # Part 2: Optimization Impact Analysis (Spot Check)
    # Focus: Isolate the impact of Object Pool and Backoff strategies.
    # Runs at a fixed thread count and payload.
    # --------------------------------------------------------------------------
    echo ">>> [Round $run][Part 2] Running Spot Check (P=8, Payload=3us)"

    for mode in "${CHECK_MODES[@]}"; do
        CHECK_BIN="$BIN_DIR/bench_queue_${mode}"
        
        for impl in "${impls[@]}"; do
            csv="$OUT_DIR/${impl}_${mode}_spotcheck.csv"
            
            # Execute benchmark
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