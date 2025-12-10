#!/usr/bin/env python3
import csv
import glob
import os
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

plt.style.use('ggplot') 
RESULTS_DIR = "results"

# 顏色定義：(NoPool Color, Pool Color)
COLOR_MAP = {
    'hp':    ('salmon', 'firebrick'),
    'ebr':   ('skyblue', 'dodgerblue'),
    'mutex': ('lightgray', 'gray'),
    'none':  ('lightgreen', 'forestgreen')
}

def load_data():
    data = []
    files = glob.glob(os.path.join(RESULTS_DIR, "*.csv"))
    if not files:
        print(f"Error: No CSV files found in {RESULTS_DIR}/")
        return []

    print(f"Loading {len(files)} CSV files...")
    
    for filename in files:
        # 從檔名判斷是否為 Pool 模式
        is_pool = "_pool_" in filename
        mode_suffix = " (Pool)" if is_pool else " (Malloc)"
        
        with open(filename, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    raw_impl = row['impl'].strip()
                    # 將實作名稱加上後綴，例如 "hp (Pool)"
                    display_impl = raw_impl + mode_suffix
                    
                    data.append({
                        'raw_impl': raw_impl,       # 原始名稱 (用來查顏色)
                        'display_impl': display_impl, # 顯示名稱
                        'is_pool': is_pool,
                        'P': int(row['P']),
                        'C': int(row['C']),
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput']),
                        'p999': float(row['p999']) / 1000.0,
                        'p50': float(row['p50']) / 1000.0
                    })
                except (KeyError, ValueError):
                    continue
    return data

def get_style(raw_impl, is_pool):
    """回傳 (color, linestyle, marker)"""
    colors = COLOR_MAP.get(raw_impl, ('black', 'black'))
    color = colors[1] if is_pool else colors[0]
    linestyle = '-' if is_pool else '--'  # Pool用實線，Malloc用虛線
    marker = 'o' if is_pool else 'v'      # Pool用圓點，Malloc用倒三角
    return color, linestyle, marker

def plot_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    # 取得所有顯示名稱並排序
    unique_labels = sorted(list(set(d['display_impl'] for d in subset)))
    
    plt.figure(figsize=(12, 7))
    
    for label in unique_labels:
        rows = sorted([d for d in subset if d['display_impl'] == label], key=lambda x: x['P'])
        if not rows: continue
        
        raw_impl = rows[0]['raw_impl']
        is_pool = rows[0]['is_pool']
        color, ls, marker = get_style(raw_impl, is_pool)
        
        x = [r['P'] for r in rows] 
        y = [r['throughput'] / 1_000_000 for r in rows] 
        
        plt.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=2, markersize=6)

    plt.title(f"Throughput Scalability (Payload={target_payload}μs)\nSolid=Pool, Dashed=Malloc")
    plt.xlabel("Producer Threads (P=C)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_throughput_scalability.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_throughput_scalability.png")
    plt.close()

def plot_payload_sensitivity(data, target_p):
    subset = [d for d in data if d['P'] == target_p]
    if not subset: return
    
    # 處理重複數據 (取平均或最新)
    # 這裡簡單用 impl+payload 作為 key 去重
    unique_map = {}
    for d in subset:
        unique_map[(d['display_impl'], d['payload_us'])] = d
    subset = list(unique_map.values())

    unique_labels = sorted(list(set(d['display_impl'] for d in subset)))
    plt.figure(figsize=(12, 7))

    for label in unique_labels:
        rows = sorted([d for d in subset if d['display_impl'] == label], key=lambda x: x['payload_us'])
        if not rows: continue
        
        raw_impl = rows[0]['raw_impl']
        is_pool = rows[0]['is_pool']
        color, ls, marker = get_style(raw_impl, is_pool)

        x = [r['payload_us'] for r in rows]
        y = [r['throughput'] / 1_000_000 for r in rows]
        plt.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=2)

    plt.title(f"Payload Sensitivity (Threads={target_p}P/{target_p}C)")
    plt.xlabel("Payload Size (μs)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/plot_payload_sensitivity.png")
    print(f"✓ Saved {RESULTS_DIR}/plot_payload_sensitivity.png")
    plt.close()

# 偵測邏輯同前
def detect_scalability_payload(data):
    counts = defaultdict(set)
    for d in data: counts[d['payload_us']].add(d['P'])
    best = max(counts.items(), key=lambda x: len(x[1]))[0] if counts else None
    return best

def detect_sensitivity_threads(data):
    counts = defaultdict(set)
    for d in data: counts[d['P']].add(d['payload_us'])
    best = max(counts.items(), key=lambda x: len(x[1]))[0] if counts else None
    return best

def main():
    data = load_data()
    if not data: return
    
    p_load = detect_scalability_payload(data)
    p_threads = detect_sensitivity_threads(data)
    
    if p_load is not None:
        plot_scalability(data, p_load)
    
    if p_threads is not None:
        plot_payload_sensitivity(data, p_threads)

if __name__ == "__main__":
    main()