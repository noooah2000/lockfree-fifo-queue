#!/usr/bin/env python3
import csv
import glob
import os
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

# 設定圖表風格
plt.style.use('ggplot') 
RESULTS_DIR = "results"

def load_data():
    """讀取所有 CSV 檔案並解析數據"""
    data = []
    files = glob.glob(os.path.join(RESULTS_DIR, "*.csv"))
    if not files:
        print(f"Error: No CSV files found in {RESULTS_DIR}/")
        return []

    print(f"Loading {len(files)} CSV files...")
    
    for filename in files:
        with open(filename, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    row = {k.strip(): v.strip() for k, v in row.items()}
                    data.append({
                        'impl': row['impl'],
                        'P': int(row['P']),
                        'C': int(row['C']),
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput']),
                        'avg_lat': float(row['avg_lat']) / 1000.0,
                        'p50': float(row['p50']) / 1000.0,
                        'p99': float(row['p99']) / 1000.0,
                        'p999': float(row['p999']) / 1000.0,
                        'max_lat': float(row['max_lat']) / 1000.0
                    })
                except (KeyError, ValueError):
                    continue
    return data

# ==========================================
# 偵測邏輯
# ==========================================

def detect_scalability_payload(data):
    """偵測用於 Scalability 測試的固定 Payload (尋找 P 變化最多的 Payload)"""
    counts = defaultdict(set)
    for d in data:
        counts[d['payload_us']].add(d['P'])
    
    best_payload = None
    max_vars = -1
    for p, p_set in counts.items():
        if len(p_set) > max_vars:
            max_vars = len(p_set)
            best_payload = p
    
    if best_payload is not None:
        print(f"🔍 [Scalability] Detected fixed Payload: {best_payload} μs (Varied threads)")
    return best_payload

def detect_sensitivity_threads(data):
    """偵測用於 Payload 測試的固定執行緒數 (尋找 Payload 變化最多的 P)"""
    counts = defaultdict(set)
    for d in data:
        counts[d['P']].add(d['payload_us'])
        
    best_p = None
    max_vars = -1
    for p, payload_set in counts.items():
        if len(payload_set) > max_vars:
            max_vars = len(payload_set)
            best_p = p
            
    if best_p is not None:
        print(f"🔍 [Sensitivity] Detected fixed Producer Count: {best_p} (Varied payloads)")
    return best_p

# ==========================================
# 繪圖函式
# ==========================================

def plot_scalability(data, target_payload):
    """圖表 1: Producer 數 vs 吞吐量"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    impls = set(d['impl'] for d in subset)
    plt.figure(figsize=(10, 6))
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}

    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['P'])
        x = [r['P'] for r in rows] 
        y = [r['throughput'] / 1_000_000 for r in rows] 
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), linewidth=2)

    plt.title(f"Throughput Scalability (Fixed Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_throughput_scalability.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_throughput_scalability.png")
    plt.close()

def plot_payload_sensitivity(data, target_p):
    """圖表 2 (新): Payload vs 吞吐量"""
    subset = [d for d in data if d['P'] == target_p]
    if not subset: return

    # 去除重複 (如果 matrix 跑了多次相同設定)
    unique_data = {}
    for d in subset:
        key = (d['impl'], d['payload_us'])
        unique_data[key] = d
    subset = list(unique_data.values())

    impls = set(d['impl'] for d in subset)
    plt.figure(figsize=(10, 6))
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}

    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['payload_us'])
        x = [r['payload_us'] for r in rows]
        y = [r['throughput'] / 1_000_000 for r in rows]
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), linewidth=2)

    plt.title(f"Payload Sensitivity (Fixed Threads={target_p}P/{target_p}C)")
    plt.xlabel("Payload Size (μs)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_payload_sensitivity.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_payload_sensitivity.png")
    plt.close()

def plot_tail_latency(data, target_payload):
    """圖表 3: Producer 數 vs P99.9 Latency"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    impls = set(d['impl'] for d in subset)
    plt.figure(figsize=(10, 6))
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}
    
    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['P'])
        x = [r['P'] for r in rows]
        y = [r['p999'] for r in rows]
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), linewidth=2)

    plt.title(f"Tail Latency P99.9 (Fixed Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)")
    plt.ylabel("Latency (μs) - Log Scale")
    plt.yscale('log')
    plt.legend()
    plt.grid(True, which="both", ls="-", alpha=0.5)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_latency_scalability.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_latency_scalability.png")
    plt.close()

def plot_latency_breakdown(data, target_payload):
    """圖表 4: 延遲分佈長條圖"""
    max_p = 0
    # 找出該 payload 下最大的 P
    sub = [d for d in data if d['payload_us'] == target_payload]
    if sub: max_p = max(d['P'] for d in sub)
    
    if max_p == 0: return

    raw_subset = [d for d in data if d['P'] == max_p and d['payload_us'] == target_payload]
    if not raw_subset: return

    # 去除重複
    unique_data = {}
    for d in raw_subset: unique_data[d['impl']] = d
    subset = sorted(unique_data.values(), key=lambda x: x['impl'])
    
    impls = [d['impl'] for d in subset]
    p50s = [d['p50'] for d in subset]
    p99s = [d['p99'] for d in subset]
    p999s = [d['p999'] for d in subset]
    
    x = np.arange(len(impls))
    width = 0.25

    plt.figure(figsize=(10, 6))
    plt.bar(x - width, p50s, width, label='P50 (Median)', alpha=0.9)
    plt.bar(x, p99s, width, label='P99', alpha=0.9)
    plt.bar(x + width, p999s, width, label='P99.9', alpha=0.9)
    
    plt.xlabel('Implementation')
    plt.ylabel('Latency (μs) - Log Scale')
    plt.title(f'Latency Distribution\n(Threads={max_p}P/{max_p}C, Payload={target_payload}μs)')
    plt.xticks(x, impls)
    plt.legend()
    plt.yscale('log') 
    plt.grid(True, axis='y', which='both', alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_latency_breakdown.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_latency_breakdown.png")
    plt.close()

def main():
    data = load_data()
    if not data: return
    
    # 自動偵測兩種實驗的固定參數
    scalability_payload = detect_scalability_payload(data)
    sensitivity_threads = detect_sensitivity_threads(data)
    
    if scalability_payload is not None:
        plot_scalability(data, scalability_payload)
        plot_tail_latency(data, scalability_payload)
        plot_latency_breakdown(data, scalability_payload)
    else:
        print("⚠ Not enough data to plot Scalability charts.")

    if sensitivity_threads is not None:
        plot_payload_sensitivity(data, sensitivity_threads)
    else:
        print("⚠ Not enough data to plot Payload Sensitivity charts.")
    
    print("\n✅ All plots generated successfully!")

if __name__ == "__main__":
    main()