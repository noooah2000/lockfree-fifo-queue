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

# 顏色定義：(NoPool/Malloc Color, Pool Color)
# Malloc 用淺色/虛線，Pool 用深色/實線
COLOR_MAP = {
    # 對應 benchmark_main.cpp 的輸出字串
    'HazardPointer': ('lightcoral', 'firebrick'),      # HP: 紅色系
    'EBR':           ('lightskyblue', 'navy'),         # EBR: 藍色系
    'MutexQueue':    ('silver', 'dimgray'),            # Mutex: 灰色系
    'NoReclamation': ('lightgreen', 'darkgreen'),      # NoReclaim: 綠色系
    
    # 保留簡寫 (以防萬一)
    'hp':            ('lightcoral', 'firebrick'),
    'ebr':           ('lightskyblue', 'navy'),
    'mutex':         ('silver', 'dimgray'),
    'none':          ('lightgreen', 'darkgreen')
}

def load_data():
    """讀取所有 CSV 檔案並解析數據"""
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
                    # 顯示名稱：例如 "hp (Pool)"
                    display_impl = raw_impl + mode_suffix
                    
                    data.append({
                        'raw_impl': raw_impl,         # 原始 key，用來查顏色
                        'display_impl': display_impl, # 圖例顯示名稱
                        'is_pool': is_pool,
                        'P': int(row['P']),
                        'C': int(row['C']),
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput']),
                        'p50': float(row['p50']) / 1000.0,   # 轉為微秒 (us)
                        'p99': float(row['p99']) / 1000.0,
                        'p999': float(row['p999']) / 1000.0,
                        'max_lat': float(row['max_lat']) / 1000.0
                    })
                except (KeyError, ValueError) as e:
                    print(f"Skipping row in {filename}: {e}")
                    continue
    return data

def get_style(raw_impl, is_pool):
    """回傳 (color, linestyle, marker)"""
    # 嘗試取得顏色，若找不到 (KeyMismatch) 則回傳明顯的 Magenta (洋紅色) 以便除錯
    colors = COLOR_MAP.get(raw_impl, ('magenta', 'darkmagenta'))
    
    # 決定顏色 (Malloc用淺色，Pool用深色)
    color = colors[1] if is_pool else colors[0]
    
    # 決定線條與點 (Pool=實線/圓點, Malloc=虛線/倒三角)
    linestyle = '-' if is_pool else '--'  
    marker = 'o' if is_pool else 'v'      
    
    return color, linestyle, marker

# ==========================================
# 1. Throughput Scalability (折線圖)
# ==========================================
def plot_throughput_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    unique_labels = sorted(list(set(d['display_impl'] for d in subset)))
    
    plt.figure(figsize=(10, 6))
    
    for label in unique_labels:
        rows = sorted([d for d in subset if d['display_impl'] == label], key=lambda x: x['P'])
        if not rows: continue
        
        raw_impl = rows[0]['raw_impl']
        is_pool = rows[0]['is_pool']
        color, ls, marker = get_style(raw_impl, is_pool)
        
        x = [r['P'] for r in rows] 
        y = [r['throughput'] / 1_000_000 for r in rows] # 轉為 Million ops/sec
        
        # linewidth 設定：Pool 稍微粗一點，突顯重點
        lw = 2.5 if is_pool else 1.5
        
        plt.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=lw, markersize=6, alpha=0.8)

    plt.title(f"Throughput Scalability (Fixed Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/1_throughput_scalability.png")
    print(f"✓ Saved {RESULTS_DIR}/1_throughput_scalability.png")
    plt.close()

# ==========================================
# 2. Latency Scalability (折線圖)
#    Tail Latency P99.9 vs Threads
# ==========================================
def plot_latency_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    unique_labels = sorted(list(set(d['display_impl'] for d in subset)))
    
    plt.figure(figsize=(10, 6))
    
    for label in unique_labels:
        rows = sorted([d for d in subset if d['display_impl'] == label], key=lambda x: x['P'])
        if not rows: continue
        
        raw_impl = rows[0]['raw_impl']
        is_pool = rows[0]['is_pool']
        color, ls, marker = get_style(raw_impl, is_pool)
        
        x = [r['P'] for r in rows]
        y = [r['p999'] for r in rows] # P99.9 in us
        
        lw = 2.5 if is_pool else 1.5

        plt.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=lw, alpha=0.8)

    plt.title(f"Tail Latency P99.9 (Fixed Payload={target_payload}μs)")
    plt.xlabel("Producer Threads (P=C)")
    plt.ylabel("Latency (μs) - Log Scale")
    plt.yscale('log') # 使用對數坐標，因為 Mutex 通常會飆很高
    plt.legend()
    plt.grid(True, which="both", ls="-", alpha=0.5)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/2_latency_scalability_p999.png")
    print(f"✓ Saved {RESULTS_DIR}/2_latency_scalability_p999.png")
    plt.close()

# ==========================================
# 3. Latency Distribution (長條圖)
#    P50, P99, P99.9 breakdown for Max Threads
# ==========================================
def plot_latency_distribution(data, target_payload):
    # 1. 找出最大的執行緒數 (Stress scenario)
    subset_payload = [d for d in data if d['payload_us'] == target_payload]
    if not subset_payload: return
    max_p = max(d['P'] for d in subset_payload)
    
    # 2. 篩選數據
    subset = [d for d in subset_payload if d['P'] == max_p]
    
    # 3. 排序：讓同一種實作的 Pool/Malloc 靠在一起
    subset.sort(key=lambda x: (x['raw_impl'], x['is_pool']))
    
    labels = [d['display_impl'] for d in subset]
    p50s = [d['p50'] for d in subset]
    p99s = [d['p99'] for d in subset]
    p999s = [d['p999'] for d in subset]
    
    x = np.arange(len(labels))
    width = 0.25  # 每個柱子的寬度

    plt.figure(figsize=(12, 6))
    
    # 繪製三組柱狀圖
    plt.bar(x - width, p50s, width, label='P50 (Median)', color='skyblue', edgecolor='black')
    plt.bar(x,        p99s, width, label='P99',          color='orange', edgecolor='black')
    plt.bar(x + width, p999s, width, label='P99.9',        color='firebrick', edgecolor='black')
    
    plt.xlabel('Implementation')
    plt.ylabel('Latency (μs) - Log Scale')
    plt.title(f'Latency Distribution (High Contention: {max_p} Threads, {target_payload}μs Payload)')
    plt.xticks(x, labels, rotation=15)
    plt.legend()
    plt.yscale('log') # 這裡也建議用 Log，才能看清 Mutex 和 Lock-Free 的差距
    plt.grid(True, axis='y', which='both', alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/3_latency_distribution.png")
    print(f"✓ Saved {RESULTS_DIR}/3_latency_distribution.png")
    plt.close()

# ==========================================
# 4. Payload Sensitivity (折線圖)
# ==========================================
def plot_payload_sensitivity(data, target_p):
    subset = [d for d in data if d['P'] == target_p]
    if not subset: return
    
    # 簡單去重
    unique_map = {}
    for d in subset:
        unique_map[(d['display_impl'], d['payload_us'])] = d
    subset = list(unique_map.values())

    unique_labels = sorted(list(set(d['display_impl'] for d in subset)))
    plt.figure(figsize=(10, 6))

    for label in unique_labels:
        rows = sorted([d for d in subset if d['display_impl'] == label], key=lambda x: x['payload_us'])
        if not rows: continue
        
        raw_impl = rows[0]['raw_impl']
        is_pool = rows[0]['is_pool']
        color, ls, marker = get_style(raw_impl, is_pool)

        x = [r['payload_us'] for r in rows]
        y = [r['throughput'] / 1_000_000 for r in rows]
        plt.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=2)

    plt.title(f"Payload Sensitivity (Fixed Threads={target_p}P/{target_p}C)")
    plt.xlabel("Payload Size (μs)")
    plt.ylabel("Throughput (Million ops/sec)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(f"{RESULTS_DIR}/4_payload_sensitivity.png")
    print(f"✓ Saved {RESULTS_DIR}/4_payload_sensitivity.png")
    plt.close()

# ==========================================
# Main
# ==========================================

def detect_scalability_payload(data):
    """找出樣本數最多的 payload"""
    counts = defaultdict(set)
    for d in data: counts[d['payload_us']].add(d['P'])
    if not counts: return None
    return max(counts.items(), key=lambda x: len(x[1]))[0]

def detect_sensitivity_threads(data):
    """找出樣本數最多的 threads"""
    counts = defaultdict(set)
    for d in data: counts[d['P']].add(d['payload_us'])
    if not counts: return None
    return max(counts.items(), key=lambda x: len(x[1]))[0]

def main():
    data = load_data()
    if not data: return
    
    # 自動偵測實驗參數
    p_load = detect_scalability_payload(data)
    p_threads = detect_sensitivity_threads(data)
    
    print(f"Detected Base Parameters: Payload={p_load}us, Threads={p_threads}")
    
    if p_load is not None:
        plot_throughput_scalability(data, p_load)
        plot_latency_scalability(data, p_load)     # New!
        plot_latency_distribution(data, p_load)    # New!
    
    if p_threads is not None:
        plot_payload_sensitivity(data, p_threads)
    
    print("\n✅ All plots generated successfully!")

if __name__ == "__main__":
    main()