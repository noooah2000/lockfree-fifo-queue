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
                        'threads': int(row['P']) + int(row['C']), # 總執行緒數
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput']),
                        # 將 ns 轉為 us，方便閱讀
                        'avg_lat': float(row['avg_lat']) / 1000.0,
                        'p50': float(row['p50']) / 1000.0,
                        'p99': float(row['p99']) / 1000.0,
                        'p999': float(row['p999']) / 1000.0,
                        'max_lat': float(row['max_lat']) / 1000.0
                    })
                except KeyError as e:
                    # 兼容舊版 CSV 或略過錯誤行
                    continue
                except ValueError as e:
                    continue
    return data

def detect_scalability_payload(data):
    """
    自動偵測哪一個 payload 是用來做 Scalability 測試的。
    邏輯：找出擁有「最多不同執行緒數量組合」的 payload。
    """
    payload_thread_counts = defaultdict(set)
    
    for d in data:
        payload_thread_counts[d['payload_us']].add(d['threads'])
        
    # 找出 set 大小最大的那個 payload
    best_payload = None
    max_variations = -1
    
    for p, threads_set in payload_thread_counts.items():
        if len(threads_set) > max_variations:
            max_variations = len(threads_set)
            best_payload = p
        elif len(threads_set) == max_variations:
            # 如果數量一樣，優先選 payload 較小的 (通常負載低更能測出 Queue 本身瓶頸)
            if best_payload is None or p < best_payload:
                best_payload = p
                
    if best_payload is not None:
        print(f"🔍 Auto-detected Scalability Payload: {best_payload} μs (Tested with {max_variations} different thread counts)")
    return best_payload

def get_max_threads_for_payload(data, target_payload):
    """找出指定 payload 下，最大的執行緒數量 (用於繪製 Breakdown 圖)"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset:
        return 0
    return max(d['threads'] for d in subset)

def plot_scalability(data, target_payload):
    """圖表 1: 執行緒數 vs 吞吐量 (Scalability)"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    
    if not subset:
        print(f"⚠ No data found for payload={target_payload}us")
        return

    impls = set(d['impl'] for d in subset)
    
    plt.figure(figsize=(10, 6))
    
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}
    linestyles = {'hp': '-', 'ebr': '-', 'mutex': '--', 'none': ':'}

    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['threads'])
        # 使用總執行緒數 (P+C) 作為 X 軸
        x = [r['threads'] for r in rows] 
        y = [r['throughput'] / 1_000_000 for r in rows] # M ops/sec
        
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), 
                 linestyle=linestyles.get(impl, '-'), linewidth=2)

    plt.title(f"Throughput Scalability (Payload={target_payload}μs)")
    plt.xlabel("Total Threads (Producers + Consumers)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_throughput.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_throughput.png")
    plt.close()

def plot_tail_latency(data, target_payload):
    """圖表 2: 執行緒數 vs P99.9 Latency (Log Scale)"""
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    impls = set(d['impl'] for d in subset)
    
    plt.figure(figsize=(10, 6))
    
    markers = {'hp': 'o', 'ebr': 's', 'mutex': 'x', 'none': '^'}
    
    for impl in sorted(impls):
        rows = sorted([d for d in subset if d['impl'] == impl], key=lambda x: x['threads'])
        x = [r['threads'] for r in rows]
        y = [r['p999'] for r in rows] # 已經是 us
        
        plt.plot(x, y, label=impl, marker=markers.get(impl, 'o'), linewidth=2)

    plt.title(f"Tail Latency P99.9 (Payload={target_payload}μs)")
    plt.xlabel("Total Threads (Producers + Consumers)")
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
    # 自動找出該 Payload 下測試過的最大執行緒數
    max_threads = get_max_threads_for_payload(data, target_payload)
    if max_threads == 0: return

    # 1. 初步過濾
    raw_subset = [d for d in data if d['threads'] == max_threads and d['payload_us'] == target_payload]
    
    if not raw_subset: return

    # 2. 去除重複 (Deduplication)
    # 如果多個 CSV 包含相同的 impl/threads/payload 組合，我們用字典來只保留一筆
    unique_data = {}
    for d in raw_subset:
        unique_data[d['impl']] = d
    
    # 轉回 list 並依照 impl 名稱排序
    subset = sorted(unique_data.values(), key=lambda x: x['impl'])
    
    # 3. 準備繪圖數據
    impls = [d['impl'] for d in subset]
    p50s = [d['p50'] for d in subset]
    p99s = [d['p99'] for d in subset]
    p999s = [d['p999'] for d in subset]
    
    x = np.arange(len(impls))
    width = 0.25

    plt.figure(figsize=(10, 6))
    
    # 使用稍微透明的顏色讓重疊部分不那麼刺眼，這裡分開畫條形圖
    plt.bar(x - width, p50s, width, label='P50 (Median)', alpha=0.9)
    plt.bar(x, p99s, width, label='P99', alpha=0.9)
    plt.bar(x + width, p999s, width, label='P99.9', alpha=0.9)
    
    plt.xlabel('Implementation')
    plt.ylabel('Latency (μs) - Log Scale')
    plt.title(f'Latency Distribution\n(Threads={max_threads}, Payload={target_payload}μs)')
    plt.xticks(x, impls) # 設定 X 軸標籤
    plt.legend()
    plt.yscale('log')    # Log Scale
    plt.grid(True, axis='y', which='both', alpha=0.3)
    
    output_path = f"{RESULTS_DIR}/plot_latency_breakdown.png"
    plt.tight_layout()
    plt.savefig(output_path)
    print(f"✓ Saved {output_path}")
    plt.close()

def main():
    data = load_data()
    if not data: return
    
    # 自動偵測 payload
    target_payload = detect_scalability_payload(data)
    
    if target_payload is None:
        print("❌ Could not detect a valid payload for plotting.")
        return

    plot_scalability(data, target_payload)
    plot_tail_latency(data, target_payload)
    plot_latency_breakdown(data, target_payload)

if __name__ == "__main__":
    main()