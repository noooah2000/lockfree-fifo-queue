#!/usr/bin/env python3
"""
Plot comprehensive performance metrics from benchmark results.
Generates 6 comparison charts:
  1. Threads vs Throughput
  2. Threads vs Tail Latency (max_depth)
  3. Threads vs Memory Peak
  4. Payload vs Throughput
  5. Payload vs Tail Latency
  6. Payload vs Memory Peak
"""
import csv
import glob
import os
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np

def load_results():
    """Load all CSV results into memory."""
    rows = []
    for path in sorted(glob.glob("results/*.csv")):
        with open(path, newline="") as f:
            r = list(csv.DictReader(f))
            rows.extend(r)
    return rows

def parse_row(r):
    """Parse a CSV row into typed values."""
    try:
        return {
            'impl': r['impl'],
            'P': int(r['P']),
            'C': int(r['C']),
            'payload_us': int(r['payload_us']),
            'duration_s': int(r['duration_s']),
            'throughput_ops': float(r['throughput_ops']),
            'max_depth': int(r['max_depth']),
        }
    except (ValueError, KeyError):
        return None

def plot_threads_vs_metrics(rows):
    """Plot (Threads, Throughput), (Threads, Tail Latency), (Threads, Memory Peak)."""
    # Group by impl and fixed payload (100us)
    fixed_payload = 100
    series = defaultdict(list)  # impl -> list of (threads, metric)
    
    for r in rows:
        if r['payload_us'] != fixed_payload:
            continue
        impl = r['impl']
        threads = r['P']  # P = C = threads
        series[impl].append((threads, r['throughput_ops'], r['max_depth']))
    
    # Sort by threads
    for impl in series:
        series[impl].sort(key=lambda x: x[0])
    
    # Create 3 subplots
    fig, axes = plt.subplots(1, 3, figsize=(15, 4))
    
    # 1. Threads vs Throughput
    ax = axes[0]
    for impl in sorted(series.keys()):
        pts = series[impl]
        ax.plot([x[0] for x in pts], [x[1] for x in pts], marker='o', label=impl, linewidth=2)
    ax.set_xlabel('Threads (P=C)', fontsize=11)
    ax.set_ylabel('Throughput (ops/s)', fontsize=11)
    ax.set_title('Throughput vs Threads\n(payload=100μs)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # 2. Threads vs Max Depth (Tail Latency)
    ax = axes[1]
    for impl in sorted(series.keys()):
        pts = series[impl]
        ax.plot([x[0] for x in pts], [x[2] for x in pts], marker='s', label=impl, linewidth=2)
    ax.set_xlabel('Threads (P=C)', fontsize=11)
    ax.set_ylabel('Max Depth (queue depth)', fontsize=11)
    ax.set_title('Tail Latency vs Threads\n(payload=100μs)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # 3. Threads vs Memory Peak (normalized by max_depth)
    ax = axes[2]
    for impl in sorted(series.keys()):
        pts = series[impl]
        # Estimate memory as proportional to max_depth * node_size
        # Using max_depth as proxy
        ax.plot([x[0] for x in pts], [x[2] for x in pts], marker='^', label=impl, linewidth=2)
    ax.set_xlabel('Threads (P=C)', fontsize=11)
    ax.set_ylabel('Memory Peak (estimated)', fontsize=11)
    ax.set_title('Memory Peak vs Threads\n(payload=100μs)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig('results/fig_threads_metrics.png', dpi=160, bbox_inches='tight')
    print("✓ Saved: results/fig_threads_metrics.png")
    plt.close()

def plot_payload_vs_metrics(rows):
    """Plot (Payload, Throughput), (Payload, Tail Latency), (Payload, Memory Peak)."""
    # Group by impl and fixed threads (P=2, C=2)
    fixed_threads = 2
    series = defaultdict(list)  # impl -> list of (payload, metric)
    
    for r in rows:
        if r['P'] != fixed_threads or r['C'] != fixed_threads:
            continue
        impl = r['impl']
        payload = r['payload_us']
        series[impl].append((payload, r['throughput_ops'], r['max_depth']))
    
    # Sort by payload
    for impl in series:
        series[impl].sort(key=lambda x: x[0])
    
    # Create 3 subplots
    fig, axes = plt.subplots(1, 3, figsize=(15, 4))
    
    # 1. Payload vs Throughput
    ax = axes[0]
    for impl in sorted(series.keys()):
        pts = series[impl]
        ax.plot([x[0] for x in pts], [x[1] for x in pts], marker='o', label=impl, linewidth=2)
    ax.set_xlabel('Payload (μs)', fontsize=11)
    ax.set_ylabel('Throughput (ops/s)', fontsize=11)
    ax.set_title('Throughput vs Payload\n(P=2, C=2)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # 2. Payload vs Max Depth (Tail Latency)
    ax = axes[1]
    for impl in sorted(series.keys()):
        pts = series[impl]
        ax.plot([x[0] for x in pts], [x[2] for x in pts], marker='s', label=impl, linewidth=2)
    ax.set_xlabel('Payload (μs)', fontsize=11)
    ax.set_ylabel('Max Depth (queue depth)', fontsize=11)
    ax.set_title('Tail Latency vs Payload\n(P=2, C=2)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # 3. Payload vs Memory Peak
    ax = axes[2]
    for impl in sorted(series.keys()):
        pts = series[impl]
        ax.plot([x[0] for x in pts], [x[2] for x in pts], marker='^', label=impl, linewidth=2)
    ax.set_xlabel('Payload (μs)', fontsize=11)
    ax.set_ylabel('Memory Peak (estimated)', fontsize=11)
    ax.set_title('Memory Peak vs Payload\n(P=2, C=2)', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig('results/fig_payload_metrics.png', dpi=160, bbox_inches='tight')
    print("✓ Saved: results/fig_payload_metrics.png")
    plt.close()

def plot_implementation_comparison(rows):
    """Create side-by-side bar chart comparing all implementations."""
    # Filter for P=2, C=2, payload=100us
    filtered = [r for r in rows 
                if r['P'] == 2 and r['C'] == 2 and r['payload_us'] == 100]
    
    if not filtered:
        print("⚠ No data for implementation comparison (P=2, C=2, payload=100μs)")
        return
    
    impls = sorted(set(r['impl'] for r in filtered))
    throughputs = [next(r['throughput_ops'] for r in filtered if r['impl'] == impl) for impl in impls]
    max_depths = [next(r['max_depth'] for r in filtered if r['impl'] == impl) for impl in impls]
    
    fig, axes = plt.subplots(1, 2, figsize=(12, 4))
    
    # Throughput comparison
    ax = axes[0]
    colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728']
    ax.bar(impls, throughputs, color=colors[:len(impls)], alpha=0.8, edgecolor='black', linewidth=1.5)
    ax.set_ylabel('Throughput (ops/s)', fontsize=11)
    ax.set_title('Throughput Comparison\n(P=2, C=2, payload=100μs)', fontsize=12, fontweight='bold')
    ax.grid(True, axis='y', alpha=0.3)
    
    # Add value labels on bars
    for i, v in enumerate(throughputs):
        ax.text(i, v, f'{v:.0f}', ha='center', va='bottom', fontsize=10)
    
    # Max depth comparison
    ax = axes[1]
    ax.bar(impls, max_depths, color=colors[:len(impls)], alpha=0.8, edgecolor='black', linewidth=1.5)
    ax.set_ylabel('Max Depth', fontsize=11)
    ax.set_title('Max Depth Comparison\n(P=2, C=2, payload=100μs)', fontsize=12, fontweight='bold')
    ax.grid(True, axis='y', alpha=0.3)
    
    # Add value labels on bars
    for i, v in enumerate(max_depths):
        ax.text(i, v, f'{v}', ha='center', va='bottom', fontsize=10)
    
    plt.tight_layout()
    plt.savefig('results/fig_implementation_comparison.png', dpi=160, bbox_inches='tight')
    print("✓ Saved: results/fig_implementation_comparison.png")
    plt.close()

def generate_summary_table(rows):
    """Generate a summary table of all results."""
    # Parse and filter rows
    parsed_rows = [parse_row(r) for r in rows]
    parsed_rows = [r for r in parsed_rows if r is not None]
    
    if not parsed_rows:
        return
    
    # Group by impl
    by_impl = defaultdict(list)
    for r in parsed_rows:
        by_impl[r['impl']].append(r)
    
    print("\n" + "="*80)
    print("📊 PERFORMANCE SUMMARY")
    print("="*80)
    
    for impl in sorted(by_impl.keys()):
        results = by_impl[impl]
        throughputs = [r['throughput_ops'] for r in results]
        depths = [r['max_depth'] for r in results]
        
        print(f"\n[{impl.upper()}]")
        print(f"  Avg Throughput: {np.mean(throughputs):,.0f} ops/s (min: {np.min(throughputs):,.0f}, max: {np.max(throughputs):,.0f})")
        print(f"  Avg Max Depth:  {np.mean(depths):.1f} (min: {np.min(depths)}, max: {np.max(depths)})")
        
        # Thread scaling efficiency (if we have thread data)
        thread_data = defaultdict(list)
        for r in results:
            if r['payload_us'] == 100:
                thread_data[r['P']].append(r)
        
        if len(thread_data) > 1:
            threads = sorted(thread_data.keys())
            throughputs_by_thread = [np.mean([r['throughput_ops'] for r in thread_data[t]]) for t in threads]
            print(f"  Thread Scaling: {threads[0]}T={throughputs_by_thread[0]:,.0f} ops/s " +
                  f"→ {threads[-1]}T={throughputs_by_thread[-1]:,.0f} ops/s " +
                  f"(speedup: {throughputs_by_thread[-1]/throughputs_by_thread[0]:.2f}x)")

def main():
    """Main entry point."""
    rows = load_results()
    
    if not rows:
        print("⚠ No CSV files found in results/. Run './scripts/run_matrix.sh' first.")
        return
    
    # Parse rows and filter out invalid ones
    parsed_rows = [parse_row(r) for r in rows]
    parsed_rows = [r for r in parsed_rows if r is not None]
    
    if not parsed_rows:
        print("⚠ No valid rows found in CSV files.")
        return
    
    print(f"📈 Loaded {len(parsed_rows)} benchmark results")
    print("\n🎨 Generating plots...")
    
    # Create output directory
    os.makedirs("results", exist_ok=True)
    
    # Generate plots
    plot_threads_vs_metrics(parsed_rows)
    plot_payload_vs_metrics(parsed_rows)
    plot_implementation_comparison(parsed_rows)
    
    # Generate summary
    generate_summary_table(rows)
    
    print("\n" + "="*80)
    print("✅ All plots generated successfully!")
    print("="*80)

if __name__ == "__main__":
    main()
