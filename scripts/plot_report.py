#!/usr/bin/env python3
"""Generate report figures from benchmark CSV files.

Usage:
  # After running benchmarks on Linux:
  python scripts/plot_report.py \
      --sweep-csv   results/sweep.csv \
      --ablation-csv results/ablation.csv \
      --cache-csv   results/cache_sweep.csv \
      --thread-csv  results/thread_sweep.csv \
      --stress-csv  results/stress.csv

A CSV can be omitted if the corresponding figure is not needed.
"""

import argparse
import csv
import os
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PROJECT_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = PROJECT_ROOT / "results" / "figures"
os.makedirs(OUT_DIR, exist_ok=True)

plt.rcParams.update({
    "font.family": "sans-serif", "font.size": 11,
    "axes.titlesize": 13, "axes.labelsize": 12,
    "figure.dpi": 150, "savefig.dpi": 150, "savefig.bbox": "tight",
})


# ---------------------------------------------------------------------------
#  helpers
# ---------------------------------------------------------------------------

def _load_csv(path):
    """Load CSV, return list of dicts with numeric values parsed."""
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: _try_float(v) for k, v in r.items()})
    if not rows:
        raise SystemExit(f"Empty or missing CSV: {path}")
    return rows

def _try_float(v):
    try: return float(v)
    except (ValueError, TypeError): return v

def _unique_sorted(rows, key):
    return sorted(set(r[key] for r in rows if key in r))


# ---------------------------------------------------------------------------
#  figures
# ---------------------------------------------------------------------------

def fig_heatmap(sweep_csv):
    """Fig 6-1: Recall@10 heatmap (beam x max_visits)."""
    rows = _load_csv(sweep_csv)
    beams = _unique_sorted(rows, "beam")
    visits = _unique_sorted(rows, "max_visits")

    data = np.full((len(beams), len(visits)), np.nan)
    for r in rows:
        i = beams.index(r["beam"])
        j = visits.index(r["max_visits"])
        data[i, j] = r["recall_10"]

    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(data, cmap="YlOrRd", aspect="auto", vmin=0.96, vmax=1.0)

    for i in range(len(beams)):
        for j in range(len(visits)):
            if not np.isnan(data[i, j]):
                clr = "black" if data[i, j] < 0.993 else "white"
                ax.text(j, i, f"{data[i, j]:.4f}", ha="center", va="center",
                        fontsize=10, color=clr)

    ax.set_xticks(range(len(visits))); ax.set_xticklabels([str(int(v)) for v in visits])
    ax.set_yticks(range(len(beams))); ax.set_yticklabels([str(int(b)) for b in beams])
    ax.set_xlabel("max_visits"); ax.set_ylabel("beam")
    ax.set_title("Fig 6-1: Recall@10 Heatmap")

    cbar = fig.colorbar(im, ax=ax, shrink=0.85); cbar.set_label("Recall@10")
    fig.savefig(OUT_DIR / "fig6-1_heatmap.png"); plt.close(fig)
    print("  -> fig6-1_heatmap.png")


def fig_ablation(ablation_csv):
    """Fig 6-2: Ablation bar chart."""
    rows = _load_csv(ablation_csv)
    labels = [r["config"] for r in rows]

    # Use total time from avg latency and num queries if available.
    # Fall back to using QPS directly.
    times_s = []
    for r in rows:
        num_q = float(r.get("num_queries", 1))
        avg_lat = float(r.get("lat_avg_ms", 0))
        n_threads = float(r.get("num_threads", 1))
        if avg_lat > 0 and num_q > 0:
            times_s.append(num_q * avg_lat / 1000.0 / n_threads)
        else:
            qps = float(r.get("qps", 1))
            times_s.append(num_q / qps if qps > 0 else 0)

    baseline = times_s[0] if times_s else 1

    fig, ax1 = plt.subplots(figsize=(10, 5))
    x = np.arange(len(labels))
    bars = ax1.bar(x, times_s, 0.55,
                   color=plt.cm.tab10(np.linspace(0, 1, len(labels))))
    ax1.set_ylabel("Total Time (s)")
    ax1.set_title("Fig 6-2: Ablation Study")
    ax1.set_xticks(x)
    ax1.set_xticklabels(labels, fontsize=8, rotation=20, ha="right")

    for i, (t, bar) in enumerate(zip(times_s, bars)):
        sp = baseline / t if t > 0 else 1
        ax1.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(times_s) * 0.02,
                 f"{sp:.1f}x", ha="center", fontsize=9, fontweight="bold")

    ax2 = ax1.twinx()
    recalls = [float(r.get("recall_10", 0)) for r in rows]
    ax2.plot(x, recalls, "ko-", linewidth=2, markersize=8)
    ax2.set_ylabel("Recall@10"); ax2.set_ylim(0.9, 1.0)
    ax2.axhline(y=0.85, color="red", linestyle="--", linewidth=1, alpha=0.5)

    fig.savefig(OUT_DIR / "fig6-2_ablation.png"); plt.close(fig)
    print("  -> fig6-2_ablation.png")


def fig_cache(cache_csv):
    """Fig 6-3: Cache hit rate & time vs cache size."""
    rows = _load_csv(cache_csv)
    cache_mb = [float(r["cache_mb"]) for r in rows]
    hit_rate = [float(r.get("cache_hit_rate", 0)) for r in rows]
    qps = [float(r.get("qps", 0)) for r in rows]

    fig, ax1 = plt.subplots(figsize=(8, 5))
    ax1.plot(cache_mb, hit_rate, "o-", color="#e74c3c", linewidth=2, markersize=8)
    ax1.set_xlabel("Cache Size (MB)")
    ax1.set_ylabel("Cache Hit Rate", color="#e74c3c")
    ax1.tick_params(axis="y", labelcolor="#e74c3c")
    for x, y in zip(cache_mb, hit_rate):
        if y > 0:
            ax1.annotate(f"{y:.1%}", (x, y), textcoords="offset points",
                         xytext=(0, 10), ha="center", fontsize=9, color="#e74c3c")

    ax2 = ax1.twinx()
    ax2.plot(cache_mb, qps, "s--", color="#3498db", linewidth=2, markersize=8)
    ax2.set_ylabel("QPS", color="#3498db")
    ax2.tick_params(axis="y", labelcolor="#3498db")

    ax1.set_title("Fig 6-3: Cache Hit Rate & QPS vs Capacity")
    fig.savefig(OUT_DIR / "fig6-3_cache.png"); plt.close(fig)
    print("  -> fig6-3_cache.png")


def fig_threads(thread_csv):
    """Fig 6-4: Thread scaling speedup."""
    rows = _load_csv(thread_csv)
    threads = [int(r["threads"]) for r in rows]
    qps = [float(r["qps"]) for r in rows]

    baseline_qps = qps[0] if qps else 1
    speedup = [q / baseline_qps for q in qps]
    ideal = [t / threads[0] for t in threads] if threads else []

    fig, ax = plt.subplots(figsize=(7, 5))
    ax.plot(threads, speedup, "o-", color="#2ecc71", linewidth=2.5,
            markersize=10, label="Actual")
    ax.plot(threads, ideal, "--", color="gray", linewidth=1.5, alpha=0.7,
            label="Linear (ideal)")
    ax.set_xlabel("Threads"); ax.set_ylabel("Speedup vs 1-thread")
    ax.set_title("Fig 6-4: Multi-thread Scalability")
    ax.legend(loc="upper left"); ax.set_xticks(threads)
    ax.grid(True, alpha=0.3)

    fig.savefig(OUT_DIR / "fig6-4_speedup.png"); plt.close(fig)
    print("  -> fig6-4_speedup.png")


def fig_stress(stress_csv):
    """Fig 6-5: Stress test time-series or boxplot."""
    rows = _load_csv(stress_csv)
    # CSV columns: time,read_qps,p50_ms,p95_ms,p99_ms,write_ops,recall10,...
    times = [float(r.get("time", i)) for i, r in enumerate(rows)]
    read_qps = [float(r.get("read_qps", 0)) for r in rows]
    p95 = [float(r.get("p95_ms", 0)) for r in rows]
    write_ops = [float(r.get("write_ops", 0)) for r in rows]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 7), sharex=True)

    ax1.plot(times, read_qps, "o-", color="#2ecc71", linewidth=1.5, markersize=3)
    ax1.set_ylabel("Read QPS")
    ax1.set_title("Fig 6-5: Mixed Read-Write Stress Test")
    ax1.grid(True, alpha=0.3)

    ax2.plot(times, p95, "s-", color="#e74c3c", linewidth=1.5, markersize=3, label="P95 latency")
    ax2.set_ylabel("P95 Latency (ms)", color="#e74c3c")
    ax2.tick_params(axis="y", labelcolor="#e74c3c")
    ax2.set_xlabel("Time (s)")
    ax2.grid(True, alpha=0.3)

    ax3 = ax2.twinx()
    ax3.plot(times, write_ops, "^-", color="#3498db", linewidth=1.5, markersize=3,
             alpha=0.7, label="Write ops")
    ax3.set_ylabel("Cumulative Write Ops", color="#3498db")
    ax3.tick_params(axis="y", labelcolor="#3498db")

    lines1, labels1 = ax2.get_legend_handles_labels()
    lines2, labels2 = ax3.get_legend_handles_labels()
    ax2.legend(lines1 + lines2, labels1 + labels2, loc="upper right")

    fig.savefig(OUT_DIR / "fig6-5_stress.png"); plt.close(fig)
    print("  -> fig6-5_stress.png")


def fig_scatter(sweep_csv):
    """Fig 7-1: Recall vs time scatter with Pareto frontier."""
    rows = _load_csv(sweep_csv)
    beams = _unique_sorted(rows, "beam")
    markers = {4: "o", 8: "s", 16: "D", 32: "^"}

    fig, ax = plt.subplots(figsize=(8, 6))
    for r in rows:
        b = r["beam"]
        mv = r["max_visits"]
        lat = float(r.get("lat_avg_ms", 0))
        rec = float(r["recall_10"])
        mk = markers.get(b, "o")
        ax.scatter(lat, rec, marker=mk, s=100, edgecolors="black", linewidth=0.3,
                   label=f"b={int(b)},v={int(mv)}" if mv == visits_sorted[0] else "")

    visits_sorted = _unique_sorted(rows, "max_visits")
    # Pareto frontier.
    points = sorted([(float(r["lat_avg_ms"]), float(r["recall_10"])) for r in rows])
    pareto_x, pareto_y = [], []
    max_y = 0
    for x, y in points:
        if y > max_y:
            max_y = y
            pareto_x.append(x); pareto_y.append(y)
    ax.plot(pareto_x, pareto_y, "k--", linewidth=1, alpha=0.5, label="Pareto")

    # Default annotation.
    default = next((r for r in rows if r["beam"] == 8 and r["max_visits"] == 1000), None)
    if default:
        dx, dy = float(default["lat_avg_ms"]), float(default["recall_10"])
        ax.scatter([dx], [dy], marker="*", s=300, c="blue", edgecolors="black",
                   linewidth=1, zorder=10, label="default")
        ax.annotate("default", (dx, dy), textcoords="offset points",
                    xytext=(10, -15), fontsize=9, color="blue", fontweight="bold")

    ax.set_xlabel("Avg Query Latency (ms)"); ax.set_ylabel("Recall@10")
    ax.set_title("Fig 7-1: Recall-Latency Trade-off")
    ax.axhline(y=0.85, color="red", linestyle=":", alpha=0.5)
    ax.grid(True, alpha=0.3)

    fig.savefig(OUT_DIR / "fig7-1_scatter.png"); plt.close(fig)
    print("  -> fig7-1_scatter.png")


# ---------------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate report figures from benchmark CSVs")
    parser.add_argument("--sweep-csv", help="Path to sweep.csv")
    parser.add_argument("--ablation-csv", help="Path to ablation.csv")
    parser.add_argument("--cache-csv", help="Path to cache_sweep.csv")
    parser.add_argument("--thread-csv", help="Path to thread_sweep.csv")
    parser.add_argument("--stress-csv", help="Path to stress.csv")
    a = parser.parse_args()

    generated = 0
    if a.sweep_csv:
        fig_heatmap(a.sweep_csv)
        fig_scatter(a.sweep_csv)
        generated += 2
    if a.ablation_csv:
        fig_ablation(a.ablation_csv)
        generated += 1
    if a.cache_csv:
        fig_cache(a.cache_csv)
        generated += 1
    if a.thread_csv:
        fig_threads(a.thread_csv)
        generated += 1
    if a.stress_csv:
        fig_stress(a.stress_csv)
        generated += 1

    if generated == 0:
        parser.print_help()
    else:
        print(f"\n{generated} figure(s) saved to {OUT_DIR}/")


if __name__ == "__main__":
    main()
