#!/usr/bin/env python3
import csv, glob, os
import matplotlib.pyplot as plt
from collections import defaultdict

def load_results():
    rows = []
    for path in glob.glob("results/*.csv"):
        with open(path, newline="") as f:
            r = list(csv.DictReader(f))
            rows.extend(r)
    return rows

def plot_throughput_vs_threads(rows):
    series = defaultdict(list)  # key: impl -> list of (threads, thr)
    for r in rows:
        impl = r["impl"]
        t = int(r["P"])
        thr = float(r["throughput_ops"])
        series[impl].append((t, thr))
    plt.figure()
    for impl, pts in series.items():
        pts.sort()
        plt.plot([x for x,_ in pts], [y for _,y in pts], marker="o", label=impl)
    plt.xlabel("Threads (P=C)")
    plt.ylabel("Throughput (ops/s)")
    plt.title("Throughput vs Threads")
    plt.legend()
    os.makedirs("results", exist_ok=True)
    plt.savefig("results/fig_throughput_vs_threads.png", dpi=160)
    print("Saved results/fig_throughput_vs_threads.png")

if __name__ == "__main__":
    rows = load_results()
    if not rows:
        print("No CSV under results/. Run scripts/run_matrix.sh first.")
    else:
        plot_throughput_vs_threads(rows)
