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
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
VINDEX = PROJECT_ROOT / "build-uring" / "vindex"
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
#  write benchmark
# ---------------------------------------------------------------------------

def cmd_write_bench(args):
    """Run insert with different flush sizes, measure throughput."""
    out_path = _out_csv("write_bench.csv")
    flush_sizes = [int(x) for x in args.flush_sizes.split(",")]
    rows = []

    for flush in flush_sizes:
        label = "all" if flush == 0 else str(flush)
        output_dir = f"data_wb_{label}"
        print(f"  flush={label} (output={output_dir}) ...", end=" ", flush=True)

        cmd = [str(VINDEX), "insert", "--dataset", args.dataset,
               "--input", args.write_input,
               "--output", output_dir,
               "--flush", str(flush),
               "--degree", str(args.degree), "--builder", args.builder]
        if args.write_limit > 0:
            cmd += ["--limit", str(args.write_limit)]

        t0 = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(PROJECT_ROOT))
        dt = time.time() - t0

        stdout = result.stdout + result.stderr
        n_segments = 0
        n_compactions = 0
        n_vectors = 0
        for line in stdout.split("\n"):
            if "Inserted" in line and "vectors into" in line:
                parts = line.split()
                n_vectors = int(parts[1])
                n_segments = int(parts[4])
            if "[compaction]" in line:
                n_compactions += 1

        throughput = n_vectors / dt if dt > 0 else 0
        print(f"{n_vectors}vec {n_segments}seg {n_compactions}comp {dt:.1f}s {throughput:.0f}vec/s")

        rows.append({"flush": flush, "vectors": n_vectors,
                     "segments": n_segments, "compactions": n_compactions,
                     "time_s": round(dt, 1), "throughput_vec_s": round(throughput)})

        # Cleanup
        shutil.rmtree(PROJECT_ROOT / output_dir, ignore_errors=True)

    _write_csv(rows, out_path,
               ["flush", "vectors", "segments", "compactions", "time_s", "throughput_vec_s"])

    print(f"\n{'flush':>8} {'vectors':>8} {'segs':>5} {'comps':>5} {'time':>8} {'vec/s':>8}")
    print("-" * 48)
    for r in rows:
        print(f"{r['flush']:>8} {r['vectors']:>8} {r['segments']:>5} {r['compactions']:>5} "
              f"{r['time_s']:>7.1f}s {r['throughput_vec_s']:>8.0f}")


# ---------------------------------------------------------------------------
#  stress benchmark
# ---------------------------------------------------------------------------

def cmd_stress_bench(args):
    """Run a single stress test and print key metrics."""
    rt = int(args.read_threads.split(",")[0])
    wt = int(args.write_threads.split(",")[0]) if args.write_threads else 0

    label = f"r{rt}_w{wt}"
    print(f"  {label} ...", end=" ", flush=True)

    cmd = [str(VINDEX), "stress", "--dataset", args.dataset,
           "--write-input", args.write_input,
           "--read-threads", str(rt),
           "--write-threads", str(wt),
           "--write-batch-size", str(args.batch_size),
           "--duration", str(args.duration)]
    if args.cache > 0:
        cmd += ["--cache", str(args.cache)]

    result = subprocess.run(cmd, capture_output=True, text=True,
                            cwd=str(PROJECT_ROOT), timeout=args.duration + 300)

    # Extract summary from output.
    total_reads = 0
    total_writes = 0
    avg_recall = 0.0
    avg_qps = 0.0
    p50 = 0.0
    p95 = 0.0
    p99 = 0.0
    avg_visited = 0.0

    for line in result.stdout.split("\n"):
        line = line.strip()
        if "Reads:" in line and "Writes:" in line:
            # "Reads: 12345  Writes: 678  QPS: 411.5"
            parts = line.split()
            for j, p in enumerate(parts):
                if p == "Reads:" and j + 1 < len(parts):
                    total_reads = int(parts[j + 1])
                elif p == "Writes:" and j + 1 < len(parts):
                    total_writes = int(parts[j + 1])
        elif "Recall@10:" in line:
            avg_recall = float(line.split(":")[1].strip())
        elif "Latency (ms):" in line and "avg=" in line:
            # "Latency (ms): avg=4.22 p50=3.85 p95=6.92 p99=10.06"
            for part in line.split():
                if "p50=" in part:
                    p50 = float(part.split("=")[1])
                elif "p95=" in part:
                    p95 = float(part.split("=")[1])
                elif "p99=" in part:
                    p99 = float(part.split("=")[1])

    duration = args.duration
    avg_qps = total_reads / duration if duration > 0 else 0

    print(f"reads={total_reads} writes={total_writes} QPS={avg_qps:.1f} "
          f"P50={p50:.1f}ms P95={p95:.1f}ms P99={p99:.1f}ms Recall@10={avg_recall:.4f}")

    # Save summary CSV.
    out_path = _out_csv("stress_bench.csv")
    _write_csv([{"reads": total_reads, "writes": total_writes, "qps": round(avg_qps, 1),
                 "p50_ms": round(p50, 1), "p95_ms": round(p95, 1), "p99_ms": round(p99, 1),
                 "recall_10": round(avg_recall, 4)}],
               out_path,
               ["reads", "writes", "qps", "p50_ms", "p95_ms", "p99_ms", "recall_10"])


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

    wp = sub.add_parser("write-bench", help="insert benchmark: flush size sweep")
    wp.add_argument("--dataset", default="sift1m-pq")
    wp.add_argument("--write-input", default="SIFT-1M/queries.bvecs")
    wp.add_argument("--write-limit", dest="write_limit", type=int, default=10000)
    wp.add_argument("--flush-sizes", dest="flush_sizes", default="0,1000,5000")
    wp.add_argument("--degree", type=int, default=32)
    wp.add_argument("--builder", default="auto")

    sp2 = sub.add_parser("stress-bench", help="mixed read-write stress test sweep")
    sp2.add_argument("--dataset", default="sift1m-pq")
    sp2.add_argument("--write-input", default="SIFT-1M/queries.bvecs")
    sp2.add_argument("--read-threads", dest="read_threads", default="4,4,2,1")
    sp2.add_argument("--write-threads", dest="write_threads", default="0,1,2,4")
    sp2.add_argument("--write-batch-size", dest="batch_size", type=int, default=100)
    sp2.add_argument("--duration", type=int, default=30)
    sp2.add_argument("--cache", type=int, default=0)

    a = parser.parse_args()
    {
        "sweep": cmd_sweep, "ablation": cmd_ablation,
        "cache-sweep": cmd_cache_sweep, "thread-sweep": cmd_thread_sweep,
        "write-bench": cmd_write_bench, "stress-bench": cmd_stress_bench,
    }.get(a.command, lambda _: parser.print_help())(a)


if __name__ == "__main__":
    main()
