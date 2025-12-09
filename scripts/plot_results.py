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
                    # 處理欄位名稱可能前後有空白的問題
                    row = {k.strip(): v.strip() for k, v in row.items()}
                    
                    data.append({
                        'impl': row['impl'],
                        'P': int(row['P']),
                        'C': int(row['C']),
                        # 雖然我們有 P 和 C，但之後繪圖主要只看 P
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput']),
                        # 將 ns 轉為 us
                        'avg_lat': float(row['avg_lat']) / 1000.0,
                        'p50': float(row['p50']) / 1000.0,
                        'p99': float(row['p99']) / 1000.0,
                        'p999': float(row['p999']) / 1000.0,
                        'max_lat': float(row['max_lat']) / 1000.0
                    })
                except (KeyError, ValueError):
                    continue
    return data

def detect_scalability_payload(data):
    """
    自動偵測哪一個 payload 是用來做 Scalability 測試的。
    邏輯：找出擁有「最多不同 Producer 數量組合」的 payload。
    """
    payload_thread_counts = defaultdict(set)
    
    for d in data:
        # 改為偵測 P 的變化數量
        payload_thread_counts[d['payload_us']].add(d['P'])
        
    best_payload = None
    max_variations = -1
    
    for p, p_counts_set in payload_thread_counts.items():
        if len(p_counts_set) > max_variations:
            max_variations = len(p_counts_set)
            best_payload = p
        elif len(p_counts_set) == max_variations:
            if best_payload is None or p < best_payload:
                best_payload = p
                
    if best_payload is not None:
        print(f"🔍 Auto-detected Scalability Payload: {best_payload} μs (Tested with {max_variations} different producer counts)")
    return best_payload

def get_max_producers_for_payload(data, target_payload):
    """找出指定 payload 下，最大的 Producer 數量"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset:
        return 0
    return max(d['P'] for d in subset)

def plot_scalability(data, target_payload):
    """圖表 1: Producer 數 vs 吞吐量 (Scalability)"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    
    if not subset:
        print(f"⚠ No data found for payload={target_payload}us")
        return

    impls = set(d['impl'] for d in subset)
    
    plt.figure(figsize=(10, 6))
    
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}
    linestyles = {'hp': '-', 'ebr': '-', 'mutex': '--', 'none': ':'}

    for impl in sorted(impls):
        # 依照 Producer 數量排序
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['P'])
        
        # X 軸改為只顯示 Producer 數量
        x = [r['P'] for r in rows] 
        y = [r['throughput'] / 1_000_000 for r in rows] # M ops/sec
        
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), 
                 linestyle=linestyles.get(impl, '-'), linewidth=2)

    plt.title(f"Throughput Scalability (Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)") # 更新標籤
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_throughput.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_throughput.png")
    plt.close()

def plot_tail_latency(data, target_payload):
    """圖表 2: Producer 數 vs P99.9 Latency (Log Scale)"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    impls = set(d['impl'] for d in subset)
    
    plt.figure(figsize=(10, 6))
    
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}
    
    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['P'])
        
        # X 軸改為只顯示 Producer 數量
        x = [r['P'] for r in rows]
        y = [r['p999'] for r in rows]
        
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), linewidth=2)

    plt.title(f"Tail Latency P99.9 (Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)") # 更新標籤
    plt.ylabel("Latency (μs) - Log Scale")
    plt.yscale('log')
    plt.legend()
    plt.grid(True, which="both", ls="-", alpha=0.5)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_latency_p999.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_latency_p999.png")
    plt.close()

def plot_latency_breakdown(data, target_payload):
    """圖表 3: 高負載下的延遲分佈對比 (P50, P99, P99.9)"""
    # 找出最大的 Producer 數量
    max_p = get_max_producers_for_payload(data, target_payload)
    if max_p == 0: return

    # 1. 初步過濾 (使用 P 判斷)
    raw_subset = [d for d in data if d['P'] == max_p and d['payload_us'] == target_payload]
    
    if not raw_subset: return

    # 2. 去除重複 (Deduplication)
    unique_data = {}
    for d in raw_subset:
        unique_data[d['impl']] = d
    
    subset = sorted(unique_data.values(), key=lambda x: x['impl'])
    
    # 3. 準備繪圖數據
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
    # 標題顯示 P/C 配對
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
    
    target_payload = detect_scalability_payload(data)
    
    if target_payload is None:
        print("❌ Could not detect a valid payload for plotting.")
        return

    plot_scalability(data, target_payload)
    plot_tail_latency(data, target_payload)
    plot_latency_breakdown(data, target_payload)
    
    print("\n✅ All plots generated successfully!")

if __name__ == "__main__":
    main()