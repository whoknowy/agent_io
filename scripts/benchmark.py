#!/usr/bin/env python3
"""Benchmark orchestration script for vindex vector search engine.

Usage:
  python scripts/benchmark.py sweep    --dataset sift1m-pq
  python scripts/benchmark.py ablation --dataset sift1m

Output: results/sweep.csv, results/ablation.csv
"""

import argparse
import csv
import os
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
VINDEX = PROJECT_ROOT / "build" / "vindex"
if os.name == "nt":
    VINDEX = VINDEX.with_suffix(".exe")


def run_eval(dataset, beam, max_visits, limit, threads=4, cache_mb=0,
             prefetch=False, manifest=None, query=None, groundtruth=None,
             extra_args=None):
    """Run vindex eval and return parsed CSV metrics."""
    cmd = [str(VINDEX), "eval", "--dataset", dataset,
           "--beam", str(beam), "--max-visits", str(max_visits),
           "--limit", str(limit), "--threads", str(threads),
           "--format", "csv"]
    if cache_mb > 0:
        cmd += ["--cache", str(cache_mb)]
    if prefetch:
        cmd.append("--prefetch")
    if manifest:
        cmd += ["--manifest", manifest]
    if query:
        cmd += ["--input", query]
    if groundtruth:
        cmd += ["--groundtruth", groundtruth]
    if extra_args:
        cmd += extra_args

    result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(PROJECT_ROOT))
    lines = [l for l in result.stdout.strip().split("\n") if l and not l.startswith("Warning")]
    if len(lines) < 2:
        print(f"Error running eval: {result.stderr}", file=sys.stderr)
        return None

    reader = csv.DictReader(lines)
    for row in reader:
        return {k: v for k, v in row.items()}
    return None


def cmd_sweep(args):
    """Parameter sweep: beam x max_visits grid."""
    beams = [int(x) for x in args.beams.split(",")]
    visits = [int(x) for x in args.max_visits.split(",")]

    os.makedirs(PROJECT_ROOT / "results", exist_ok=True)
    out_path = PROJECT_ROOT / "results" / "sweep.csv"

    fieldnames = ["beam", "max_visits", "recall_1", "recall_10", "recall_100",
                  "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                  "avg_visited", "avg_exact_reads", "avg_pq_dist", "avg_prefetch_hits",
                  "cache_hit_rate", "num_queries", "num_threads"]
    rows = []

    total = len(beams) * len(visits)
    i = 0
    for beam in beams:
        for mv in visits:
            i += 1
            print(f"[{i}/{total}] beam={beam} max_visits={mv} ...", end=" ", flush=True)
            t0 = time.time()
            row = run_eval(args.dataset, beam, mv, args.limit,
                           threads=args.threads, cache_mb=args.cache,
                           prefetch=args.prefetch)
            elapsed = time.time() - t0
            if row:
                row["beam"] = beam
                row["max_visits"] = mv
                rows.append(row)
                print(f"Recall@10={float(row['recall_10']):.4f} QPS={float(row['qps']):.1f} ({elapsed:.1f}s)")
            else:
                print("FAILED")

    # Write CSV.
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nSaved {len(rows)} results to {out_path}")

    # Print heatmap of Recall@10.
    print("\nRecall@10 heatmap:")
    print(f"{'beam\\max_visits':<16}", end="")
    for mv in visits:
        print(f"{mv:<10}", end="")
    print()
    for beam in beams:
        print(f"{beam:<16}", end="")
        for mv in visits:
            val = next((r for r in rows if r.get("beam") == beam and r.get("max_visits") == mv), None)
            if val:
                print(f"{float(val['recall_10']):.4f}   ", end="")
            else:
                print("N/A       ", end="")
        print()


def cmd_ablation(args):
    """Ablation study: cache x prefetch x threads x PQ."""
    os.makedirs(PROJECT_ROOT / "results", exist_ok=True)
    out_path = PROJECT_ROOT / "results" / "ablation.csv"

    cache_levels = [int(x) for x in args.caches.split(",")]
    thread_levels = [int(x) for x in args.threads.split(",")]
    prefetch_levels = [x == "on" for x in args.prefetch.split(",")]
    datasets = args.datasets.split(",")

    fieldnames = ["config", "dataset", "recall_1", "recall_10", "recall_100",
                  "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                  "avg_visited", "avg_exact_reads", "avg_pq_dist", "avg_prefetch_hits",
                  "cache_hit_rate", "cache_mb", "threads", "prefetch"]
    rows = []

    for ds in datasets:
        for cache_mb in cache_levels:
            for threads in thread_levels:
                for pf in prefetch_levels:
                    config = f"{ds}_cache{cache_mb}_t{threads}"
                    if pf:
                        config += "_pf"
                    print(f"  {config} ...", end=" ", flush=True)
                    t0 = time.time()
                    row = run_eval(ds, args.beam, args.max_visits, args.limit,
                                   threads=threads, cache_mb=cache_mb, prefetch=pf)
                    elapsed = time.time() - t0
                    if row:
                        row["config"] = config
                        row["dataset"] = ds
                        row["cache_mb"] = cache_mb
                        row["threads"] = threads
                        row["prefetch"] = int(pf)
                        rows.append(row)
                        print(f"R@10={float(row['recall_10']):.4f} QPS={float(row['qps']):.1f} ({elapsed:.1f}s)")
                    else:
                        print("FAILED")

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nSaved {len(rows)} results to {out_path}")

    # Print summary table.
    print(f"\n{'Config':<25} {'R@1':>6} {'R@10':>6} {'P50ms':>7} {'P95ms':>7} {'QPS':>7} {'Visited':>7} {'ExactRd':>7} {'CacheHit':>8}")
    print("-" * 85)
    for r in rows:
        print(f"{r['config']:<25} {float(r['recall_1']):6.4f} {float(r['recall_10']):6.4f} "
              f"{float(r['lat_p50_ms']):7.1f} {float(r['lat_p95_ms']):7.1f} "
              f"{float(r['qps']):7.1f} {float(r['avg_visited']):7.0f} "
              f"{float(r['avg_exact_reads']):7.0f} {float(r.get('cache_hit_rate', 0)):7.3f}")


def main():
    parser = argparse.ArgumentParser(description="vindex benchmark suite")
    sub = parser.add_subparsers(dest="command")

    # sweep
    sp = sub.add_parser("sweep", help="Parameter sweep over beam x max_visits")
    sp.add_argument("--dataset", default="sift1m-pq")
    sp.add_argument("--beams", default="4,8,16,32")
    sp.add_argument("--max-visits", dest="max_visits", default="500,1000,2000")
    sp.add_argument("--limit", type=int, default=1000)
    sp.add_argument("--threads", type=int, default=4)
    sp.add_argument("--cache", type=int, default=0)
    sp.add_argument("--prefetch", action="store_true")

    # ablation
    ap = sub.add_parser("ablation", help="Ablation study")
    ap.add_argument("--datasets", default="sift1m-pq")
    ap.add_argument("--beam", type=int, default=8)
    ap.add_argument("--max-visits", dest="max_visits", type=int, default=1000)
    ap.add_argument("--limit", type=int, default=1000)
    ap.add_argument("--caches", default="0,200")
    ap.add_argument("--threads", default="1,4")
    ap.add_argument("--prefetch", default="off,on")

    a = parser.parse_args()
    if a.command == "sweep":
        cmd_sweep(a)
    elif a.command == "ablation":
        cmd_ablation(a)
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
