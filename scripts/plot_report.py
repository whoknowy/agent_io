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
    if v is None:
        return None
    if isinstance(v, str) and not v.strip():
        return None
    try:
        return float(v)
    except (ValueError, TypeError):
        return v

def _as_float(value, default=0.0):
    if value is None:
        return default
    if isinstance(value, str) and not value.strip():
        return default
    try:
        return float(value)
    except (ValueError, TypeError):
        return default

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


def _find(rows, cache_mb, threads, prefetch):
    """Find a row matching the given config."""
    for r in rows:
        if (int(_as_float(r.get("cache_mb"), 0)) == cache_mb
                and int(_as_float(r.get("threads"), 0)) == threads
                and int(_as_float(r.get("prefetch"), 0)) == (1 if prefetch else 0)):
            return r
    return None

def fig_ablation(ablation_csv):
    """Fig 6-2: Paired ablation — effect of each feature (threads / cache / prefetch)."""
    rows = _load_csv(ablation_csv)

    # Three pairwise comparisons, each: (label, off_config, on_config)
    pairs = [
        ("Threads\n(1 vs 4)",   _find(rows, 0, 1, False), _find(rows, 0, 4, False)),
        ("Cache\n(0 vs 200MB)", _find(rows, 0, 4, False), _find(rows, 200, 4, False)),
        ("Prefetch\n(off vs on)", _find(rows, 0, 4, False), _find(rows, 0, 4, True)),
    ]

    fig, axes = plt.subplots(1, 3, figsize=(12, 5))

    for ax, (title, off, on) in zip(axes, pairs):
        off_qps = _as_float(off["qps"]) if off else 0
        on_qps = _as_float(on["qps"]) if on else 0
        off_lat = _as_float(off["lat_p50_ms"]) if off else 0
        on_lat = _as_float(on["lat_p50_ms"]) if on else 0

        x = [0, 1]
        qps_vals = [off_qps, on_qps]
        lat_vals = [off_lat, on_lat]
        colors = ["#b0b0b0", "#2ecc71"]

        bars = ax.bar(x, qps_vals, 0.5, color=colors, edgecolor="white", linewidth=0.8)
        ax.set_xticks(x)
        ax.set_xticklabels(["off", "on"])
        ax.set_title(title, fontsize=12, fontweight="bold")

        # QPS value on top of each bar
        for bar, v in zip(bars, qps_vals):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(qps_vals) * 0.03,
                    f"{v:.0f}", ha="center", fontsize=11, fontweight="bold")

        # Speedup annotation between the two bars
        if off_qps > 0:
            ratio = on_qps / off_qps
            mid_x = 0.5
            mid_y = max(qps_vals) * 1.15
            color = "#27ae60" if ratio >= 1 else "#e74c3c"
            ax.annotate(f"{ratio:.2f}x", xy=(mid_x, mid_y), ha="center", fontsize=12,
                        fontweight="bold", color=color)

        # P50 latency text below QPS
        for i, (bar, lat) in enumerate(zip(bars, lat_vals)):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 0.15,
                    f"P50={lat:.0f}ms", ha="center", fontsize=9, color="white")

        ax.set_ylabel("QPS")
        ax.set_ylim(0, max(qps_vals) * 1.3)

    fig.suptitle("Fig 6-2: Ablation Study — Feature On/Off Comparison", fontsize=14, y=1.02)
    fig.tight_layout()
    fig.savefig(OUT_DIR / "fig6-2_ablation.png"); plt.close(fig)
    print("  -> fig6-2_ablation.png")


def fig_cache(cache_csv):
    """Fig 6-3: Cache hit rate & time vs cache size."""
    rows = _load_csv(cache_csv)
    cache_mb = [_as_float(r["cache_mb"]) for r in rows]
    hit_rate = [_as_float(r.get("cache_hit_rate")) for r in rows]
    qps = [_as_float(r.get("qps")) for r in rows]

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
    threads = [int(_as_float(r["threads"])) for r in rows]
    qps = [_as_float(r["qps"]) for r in rows]

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
    times = [_as_float(r.get("time"), i) for i, r in enumerate(rows)]
    read_qps = [_as_float(r.get("read_qps")) for r in rows]
    p95 = [_as_float(r.get("p95_ms")) for r in rows]
    write_ops = [_as_float(r.get("write_ops")) for r in rows]

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
    visits_sorted = _unique_sorted(rows, "max_visits")
    markers = {4: "o", 8: "s", 16: "D", 32: "^"}

    fig, ax = plt.subplots(figsize=(8, 6))
    for r in rows:
        b = r["beam"]
        mv = r["max_visits"]
        lat = _as_float(r.get("lat_avg_ms"))
        rec = _as_float(r["recall_10"])
        mk = markers.get(b, "o")
        ax.scatter(lat, rec, marker=mk, s=100, edgecolors="black", linewidth=0.3,
                   label=f"b={int(b)},v={int(mv)}" if mv == visits_sorted[0] else "")

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
