#!/usr/bin/env python3
"""Benchmark + plot orchestration for vindex vector search engine.

Usage:
  python scripts/benchmark.py sweep       --dataset sift1m-pq
  python scripts/benchmark.py ablation    --datasets sift1m-pq
  python scripts/benchmark.py cache-sweep --dataset sift1m-pq
  python scripts/benchmark.py thread-sweep --dataset sift1m-pq
  python scripts/benchmark.py plot        --sweep-csv results/sweep.csv ...

Output: results/*.csv, results/figures/*.png
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


# ---------------------------------------------------------------------------
#  eval runner
# ---------------------------------------------------------------------------

def run_eval(dataset, beam, max_visits, limit, threads=4, cache_mb=0,
             prefetch=False, manifest=None, query=None, groundtruth=None,
             extra_args=None):
    """Run vindex eval --format csv and return parsed dict, or None on failure."""
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
    lines = [l for l in result.stdout.strip().split("\n")
             if l and not l.startswith("Warning")]
    if len(lines) < 2:
        print(f"  eval failed: {result.stderr}", file=sys.stderr)
        return None

    reader = csv.DictReader(lines)
    for row in reader:
        return {k: v for k, v in row.items()}
    return None


# ---------------------------------------------------------------------------
#  sub-commands
# ---------------------------------------------------------------------------

def cmd_sweep(args):
    """beam x max_visits grid."""
    beams = [int(x) for x in args.beams.split(",")]
    visits = [int(x) for x in args.max_visits.split(",")]
    out_path = _out_csv("sweep.csv")
    rows = []

    total = len(beams) * len(visits)
    for i, beam in enumerate(beams):
        for j, mv in enumerate(visits):
            idx = i * len(visits) + j + 1
            print(f"[{idx}/{total}] beam={beam} visits={mv} ...", end=" ", flush=True)
            t0 = time.time()
            row = run_eval(args.dataset, beam, mv, args.limit,
                           threads=args.threads, cache_mb=args.cache,
                           prefetch=args.prefetch)
            dt = time.time() - t0
            if row:
                row["beam"] = beam
                row["max_visits"] = mv
                rows.append(row)
                print(f"R@10={float(row['recall_10']):.4f}  {dt:.1f}s")
            else:
                print("FAILED")

    _write_csv(rows, out_path,
               ["beam", "max_visits", "recall_1", "recall_10", "recall_100",
                "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                "avg_visited", "avg_exact_reads", "avg_pq_dist", "avg_prefetch_hits",
                "cache_hit_rate", "num_queries", "num_threads"])
    _print_heatmap(rows, beams, visits)


def cmd_ablation(args):
    """cache x prefetch x threads x dataset."""
    out_path = _out_csv("ablation.csv")
    caches = [int(x) for x in args.caches.split(",")]
    threads = [int(x) for x in args.threads.split(",")]
    prefs = [x == "on" for x in args.prefetch.split(",")]
    dss = args.datasets.split(",")
    rows = []

    for ds in dss:
        for cmb in caches:
            for nt in threads:
                for pf in prefs:
                    cfg = f"{ds}_cache{cmb}_t{nt}" + ("_pf" if pf else "")
                    print(f"  {cfg} ...", end=" ", flush=True)
                    t0 = time.time()
                    row = run_eval(ds, args.beam, args.max_visits, args.limit,
                                   threads=nt, cache_mb=cmb, prefetch=pf)
                    dt = time.time() - t0
                    if row:
                        row["config"] = cfg; row["dataset"] = ds
                        row["cache_mb"] = cmb; row["threads"] = nt
                        row["prefetch"] = int(pf)
                        rows.append(row)
                        print(f"R@10={float(row['recall_10']):.4f}  {dt:.1f}s")
                    else:
                        print("FAILED")

    _write_csv(rows, out_path,
               ["config", "dataset", "recall_1", "recall_10", "recall_100",
                "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                "avg_visited", "avg_exact_reads", "avg_pq_dist", "avg_prefetch_hits",
                "cache_hit_rate", "cache_mb", "threads", "prefetch"])
    _print_ablation_table(rows)


def cmd_cache_sweep(args):
    """Vary cache size only."""
    out_path = _out_csv("cache_sweep.csv")
    sizes = [int(x) for x in args.caches.split(",")]
    rows = []

    for sz in sizes:
        print(f"  cache={sz}MB ...", end=" ", flush=True)
        t0 = time.time()
        row = run_eval(args.dataset, args.beam, args.max_visits, args.limit,
                       threads=args.threads, cache_mb=sz)
        dt = time.time() - t0
        if row:
            row["cache_mb"] = sz
            rows.append(row)
            print(f"hit={float(row.get('cache_hit_rate',0)):.3f}  QPS={float(row['qps']):.1f}  {dt:.1f}s")
        else:
            print("FAILED")

    _write_csv(rows, out_path,
               ["cache_mb", "recall_1", "recall_10", "recall_100",
                "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                "avg_visited", "avg_exact_reads", "cache_hit_rate"])


def cmd_thread_sweep(args):
    """Vary thread count only."""
    out_path = _out_csv("thread_sweep.csv")
    nts = [int(x) for x in args.threads.split(",")]
    rows = []

    for nt in nts:
        print(f"  threads={nt} ...", end=" ", flush=True)
        t0 = time.time()
        row = run_eval(args.dataset, args.beam, args.max_visits, args.limit,
                       threads=nt, cache_mb=args.cache)
        dt = time.time() - t0
        if row:
            row["threads"] = nt
            rows.append(row)
            print(f"QPS={float(row['qps']):.1f}  latency_p50={float(row['lat_p50_ms']):.1f}ms  {dt:.1f}s")
        else:
            print("FAILED")

    _write_csv(rows, out_path,
               ["threads", "recall_1", "recall_10", "recall_100",
                "lat_avg_ms", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms", "qps",
                "avg_visited", "avg_exact_reads", "cache_hit_rate"])


# ---------------------------------------------------------------------------
#  helpers
# ---------------------------------------------------------------------------

def _out_csv(name):
    os.makedirs(PROJECT_ROOT / "results", exist_ok=True)
    return PROJECT_ROOT / "results" / name

def _write_csv(rows, path, fieldnames):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"  -> {len(rows)} rows written to {path}")

def _print_heatmap(rows, beams, visits):
    print("\nRecall@10 heatmap:")
    header = f"{'beam\\visits':<16}" + "".join(f"{v:<10}" for v in visits)
    print(header)
    for beam in beams:
        line = f"{beam:<16}"
        for mv in visits:
            r = next((r for r in rows if r.get("beam") == beam and r.get("max_visits") == mv), None)
            line += f"{float(r['recall_10']):.4f}   " if r else "N/A       "
        print(line)

def _print_ablation_table(rows):
    print(f"\n{'Config':<28} {'R@1':>6} {'R@10':>6} {'P50':>6} {'P95':>6} {'QPS':>7} {'Hit':>6}")
    print("-" * 75)
    for r in rows:
        print(f"{r['config']:<28} {float(r['recall_1']):6.4f} {float(r['recall_10']):6.4f} "
              f"{float(r['lat_p50_ms']):6.1f} {float(r['lat_p95_ms']):6.1f} "
              f"{float(r['qps']):7.1f} {float(r.get('cache_hit_rate',0)):6.3f}")


# ---------------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="vindex benchmark suite")
    sub = parser.add_subparsers(dest="command")

    sp = sub.add_parser("sweep", help="beam x max_visits grid")
    sp.add_argument("--dataset", default="sift1m-pq")
    sp.add_argument("--beams", default="4,8,16,32")
    sp.add_argument("--max-visits", dest="max_visits", default="500,1000,2000")
    sp.add_argument("--limit", type=int, default=1000)
    sp.add_argument("--threads", type=int, default=4)
    sp.add_argument("--cache", type=int, default=0)
    sp.add_argument("--prefetch", action="store_true")

    ap = sub.add_parser("ablation", help="cache x prefetch x threads")
    ap.add_argument("--datasets", default="sift1m-pq")
    ap.add_argument("--beam", type=int, default=8)
    ap.add_argument("--max-visits", dest="max_visits", type=int, default=1000)
    ap.add_argument("--limit", type=int, default=1000)
    ap.add_argument("--caches", default="0,200")
    ap.add_argument("--threads", default="1,4")
    ap.add_argument("--prefetch", default="off,on")

    cp = sub.add_parser("cache-sweep", help="vary cache size")
    cp.add_argument("--dataset", default="sift1m-pq")
    cp.add_argument("--beam", type=int, default=8)
    cp.add_argument("--max-visits", dest="max_visits", type=int, default=1000)
    cp.add_argument("--limit", type=int, default=1000)
    cp.add_argument("--threads", type=int, default=4)
    cp.add_argument("--caches", default="0,2,20,80,200")

    tp = sub.add_parser("thread-sweep", help="vary thread count")
    tp.add_argument("--dataset", default="sift1m-pq")
    tp.add_argument("--beam", type=int, default=8)
    tp.add_argument("--max-visits", dest="max_visits", type=int, default=1000)
    tp.add_argument("--limit", type=int, default=1000)
    tp.add_argument("--threads", default="1,2,4,8,16")
    tp.add_argument("--cache", type=int, default=0)

    a = parser.parse_args()
    {"sweep": cmd_sweep, "ablation": cmd_ablation,
     "cache-sweep": cmd_cache_sweep, "thread-sweep": cmd_thread_sweep
    }.get(a.command, lambda _: parser.print_help())(a)


if __name__ == "__main__":
    main()
