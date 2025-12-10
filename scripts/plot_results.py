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

# 顏色定義
COLOR_MAP = {
    'HazardPointer': ('lightcoral', 'firebrick'),      # HP
    'EBR':           ('lightskyblue', 'navy'),         # EBR
    'MutexQueue':    ('silver', 'dimgray'),            # Mutex
    'NoReclamation': ('lightgreen', 'darkgreen'),      # None
    
    # 相容小寫
    'hp':            ('lightcoral', 'firebrick'),
    'ebr':           ('lightskyblue', 'navy'),
    'mutex':         ('silver', 'dimgray'),
    'none':          ('lightgreen', 'darkgreen')
}

def load_data():
    """讀取所有 CSV 檔案並解析數據"""
    data = []
    if not os.path.exists(RESULTS_DIR):
        print(f"Error: Directory '{RESULTS_DIR}' not found.")
        return []

    files = glob.glob(os.path.join(RESULTS_DIR, "*.csv"))
    if not files:
        print(f"Error: No CSV files found in {RESULTS_DIR}/")
        return []

    print(f"Loading {len(files)} CSV files...")
    
    for filename in files:
        is_pool = "_pool_" in filename
        mode_suffix = " (Pool)" if is_pool else " (Malloc)"
        
        with open(filename, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    raw_impl = row['impl'].strip()
                    display_impl = raw_impl + mode_suffix
                    
                    data.append({
                        'raw_impl': raw_impl,
                        'display_impl': display_impl,
                        'is_pool': is_pool,
                        'P': int(row['P']),
                        'C': int(row['C']),
                        'payload_us': int(row['payload_us']),
                        'throughput': float(row['throughput_cons']), 
                        
                        # Latency: ns -> us
                        'avg_lat': float(row['avg_lat']) / 1000.0,
                        'p50': float(row['p50']) / 1000.0,
                        'p95': float(row['p95']) / 1000.0,
                        'p99': float(row['p99']) / 1000.0,
                        'max_lat': float(row['max_lat']) / 1000.0,
                        
                        # Memory: KB -> MB
                        'peak_mem_mb': float(row['peak_mem_kb']) / 1024.0,
                        
                        # Max Depth
                        'max_depth': int(row['max_depth'])
                    })
                except (KeyError, ValueError) as e:
                    print(f"Skipping row in {filename}: {e}")
                    continue
    return data

def get_style(raw_impl, is_pool):
    """回傳 (color, linestyle, marker)"""
    colors = COLOR_MAP.get(raw_impl, ('magenta', 'darkmagenta'))
    color = colors[1] if is_pool else colors[0]
    # 因為分開畫了，線條樣式可以統一，或者保持區別
    linestyle = '-' if is_pool else '--'  
    marker = 'o' if is_pool else 'v'      
    return color, linestyle, marker

# ==========================================
# Helper: 通用折線圖繪製 (左右子圖)
# ==========================================
def plot_side_by_side(data, x_key, y_key, title_main, y_label, filename, log_scale=False, y_limit=None):
    """
    通用函式：將數據分為 Malloc(左) 與 Pool(右) 兩張子圖
    """
    raw_impls = sorted(list(set(d['raw_impl'] for d in data)))
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6)) # sharey=False 以便觀察各自的趨勢
    
    # 定義子圖繪製邏輯
    def plot_subplot(ax, is_pool_target, subplot_title):
        has_data = False
        for impl in raw_impls:
            # 篩選特定實作與模式的數據
            rows = sorted([d for d in data if d['raw_impl'] == impl and d['is_pool'] == is_pool_target], key=lambda r: r[x_key])
            if not rows: continue
            
            has_data = True
            color, ls, marker = get_style(impl, is_pool_target)
            x = [r[x_key] for r in rows]
            y = [r[y_key] for r in rows]
            
            # 使用 display_impl 作為 label，或者只用 raw_impl (因為標題已區分模式)
            label = rows[0]['display_impl']
            
            ax.plot(x, y, label=label, color=color, linestyle=ls, marker=marker, linewidth=2, alpha=0.8)
        
        ax.set_title(subplot_title)
        ax.set_xlabel("Threads (P=C)" if x_key == 'P' else "Payload Size (μs)")
        ax.set_ylabel(y_label)
        if log_scale: ax.set_yscale('log')
        if y_limit: ax.set_ylim(y_limit)
        if has_data: ax.legend()
        ax.grid(True, which="both", ls="-", alpha=0.3)

    # 繪製左圖 (No Pool)
    plot_subplot(ax1, False, "No Pool (Malloc)")
    
    # 繪製右圖 (Pool)
    plot_subplot(ax2, True, "Object Pool")
    
    # 設定總標題
    fig.suptitle(title_main, fontsize=16)
    plt.tight_layout(rect=[0, 0.03, 1, 0.95]) # 預留空間給 suptitle
    
    plt.savefig(f"{RESULTS_DIR}/{filename}")
    print(f"✓ Saved {RESULTS_DIR}/{filename}")
    plt.close()

# ==========================================
# 1. Throughput Scalability
# ==========================================
def plot_throughput_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return
    
    # 預處理數據：Throughput 單位換算
    for d in subset: d['_tp_million'] = d['throughput'] / 1_000_000

    plot_side_by_side(
        subset, x_key='P', y_key='_tp_million',
        title_main=f"Throughput Scalability (Fixed Payload={target_payload}μs)",
        y_label="Consumer Throughput (Million ops/sec)",
        filename="1_throughput_scalability.png"
    )

# ==========================================
# 2. Latency Scalability (P99)
# ==========================================
def plot_latency_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    plot_side_by_side(
        subset, x_key='P', y_key='p99',
        title_main=f"Tail Latency P99 (Fixed Payload={target_payload}μs)",
        y_label="Latency (μs) - Log Scale",
        filename="2_latency_scalability_p99.png",
        log_scale=True
    )

# ==========================================
# 3. Latency Distribution (Modified Bar Chart)
# ==========================================
def plot_latency_distribution(data, target_payload):
    # 篩選數據
    subset_payload = [d for d in data if d['payload_us'] == target_payload]
    if not subset_payload: return
    
    target_p = 8
    subset = [d for d in subset_payload if d['P'] == target_p]
    if not subset: return

    # 去重邏輯 (針對 run_matrix.sh 重複執行的部分)
    unique_map = {}
    for d in subset:
        unique_map[(d['raw_impl'], d['is_pool'])] = d
    subset = list(unique_map.values())

    # 準備畫布 (1行2列)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, 7), sharey=True) # Bar chart 建議 sharey 方便比較
    
    metrics = ['avg_lat', 'p50', 'p95', 'p99', 'max_lat']
    metric_labels = ['Avg', 'P50', 'P95', 'P99', 'Max']
    colors = ['forestgreen', 'skyblue', 'orange', 'firebrick', 'purple']
    width = 0.15

    def plot_bar_subplot(ax, is_pool_target, title):
        # 篩選並排序
        rows = [d for d in subset if d['is_pool'] == is_pool_target]
        rows.sort(key=lambda x: x['raw_impl']) # 依實作名稱排序
        
        if not rows: return

        labels = [d['raw_impl'] for d in rows] # 簡化標籤，不顯示 (Pool)/(Malloc)
        x = np.arange(len(labels))
        
        # 繪製 5 根柱子
        for i, (metric, color, label) in enumerate(zip(metrics, colors, metric_labels)):
            values = [d[metric] for d in rows]
            # 位移：中間是 0，左右分散 (e.g., -2w, -1w, 0, 1w, 2w)
            offset = (i - 2) * width 
            ax.bar(x + offset, values, width, label=label, color=color, edgecolor='black')

        ax.set_title(title)
        ax.set_xlabel('Implementation')
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=15)
        ax.set_yscale('log')
        ax.grid(True, axis='y', which='both', alpha=0.3)
        ax.legend()

    # 左圖
    plot_bar_subplot(ax1, False, f"Latency Dist - No Pool (Malloc) (P={target_p})")
    ax1.set_ylabel('Latency (μs) - Log Scale')
    
    # 右圖
    plot_bar_subplot(ax2, True, f"Latency Dist - Object Pool (P={target_p})")
    
    fig.suptitle(f"Latency Distribution (Threads={target_p}, Payload={target_payload}μs)", fontsize=16)
    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    plt.savefig(f"{RESULTS_DIR}/3_latency_distribution.png")
    print(f"✓ Saved {RESULTS_DIR}/3_latency_distribution.png")
    plt.close()

# ==========================================
# 4. Memory Peak Scalability
# ==========================================
def plot_memory_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    plot_side_by_side(
        subset, x_key='P', y_key='peak_mem_mb',
        title_main=f"Peak Memory Usage (Fixed Payload={target_payload}μs)",
        y_label="Memory Peak (MB)",
        filename="4_memory_scalability.png"
    )

# ==========================================
# 5. Max Depth Scalability
# ==========================================
def plot_max_depth_scalability(data, target_payload):
    subset = [d for d in data if d['payload_us'] == target_payload]
    if not subset: return

    plot_side_by_side(
        subset, x_key='P', y_key='max_depth',
        title_main=f"Max Queue Depth (Fixed Payload={target_payload}μs)",
        y_label="Max Depth (Count)",
        filename="5_max_depth_scalability.png"
    )

# ==========================================
# 6. Payload Sensitivity
# ==========================================
def plot_payload_sensitivity(data, target_p):
    subset = [d for d in data if d['P'] == target_p]
    if not subset: return
    
    # 去重
    unique_map = {}
    for d in subset: unique_map[(d['raw_impl'], d['is_pool'], d['payload_us'])] = d
    subset = list(unique_map.values())

    # 預處理
    for d in subset: d['_tp_million'] = d['throughput'] / 1_000_000

    plot_side_by_side(
        subset, x_key='payload_us', y_key='_tp_million',
        title_main=f"Payload Sensitivity (Threads={target_p}P/{target_p}C)",
        y_label="Consumer Throughput (Million ops/sec)",
        filename="6_payload_sensitivity.png"
    )

# ==========================================
# 7. Efficiency Sensitivity
# ==========================================
def plot_efficiency_sensitivity(data, target_p):
    subset = [d for d in data if d['P'] == target_p and d['payload_us'] > 0]
    if not subset: return

    # 去重
    unique_map = {}
    for d in subset: unique_map[(d['raw_impl'], d['is_pool'], d['payload_us'])] = d
    subset = list(unique_map.values())

    # 計算效率
    for d in subset:
        ideal_ops = d['C'] * (1_000_000.0 / d['payload_us'])
        d['_efficiency'] = (d['throughput'] / ideal_ops) * 100.0

    plot_side_by_side(
        subset, x_key='payload_us', y_key='_efficiency',
        title_main=f"System Efficiency (Threads={target_p}P/{target_p}C)",
        y_label="Efficiency (%)",
        filename="7_efficiency_sensitivity.png",
        y_limit=(0, 110)
    )

# ==========================================
# Main
# ==========================================
def detect_scalability_payload(data):
    counts = defaultdict(set)
    for d in data: counts[d['payload_us']].add(d['P'])
    if not counts: return None
    return max(counts.items(), key=lambda x: len(x[1]))[0]

def detect_sensitivity_threads(data):
    counts = defaultdict(set)
    for d in data: counts[d['P']].add(d['payload_us'])
    if not counts: return None
    return max(counts.items(), key=lambda x: len(x[1]))[0]

def main():
    data = load_data()
    if not data: return
    
    p_load = detect_scalability_payload(data)
    p_threads = detect_sensitivity_threads(data)
    
    print(f"\nDetected Base Parameters:\n  - Payload for Scalability charts: {p_load} us\n  - Threads for Sensitivity charts: {p_threads} (P={p_threads}, C={p_threads})")
    
    if p_load is not None:
        plot_throughput_scalability(data, p_load)
        plot_latency_scalability(data, p_load)
        plot_latency_distribution(data, p_load)
        plot_memory_scalability(data, p_load)
        plot_max_depth_scalability(data, p_load)
    
    if p_threads is not None:
        plot_payload_sensitivity(data, p_threads)
        plot_efficiency_sensitivity(data, p_threads)
    
    print("\n✅ All plots generated successfully!")

if __name__ == "__main__":
    main()