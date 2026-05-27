#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "cache/cache_pool.h"
#include "core/distance.h"
#include "core/topk.h"
#include "dataset/fvecs.h"
#include "dataset/bvecs.h"
#include "dataset/ivecs.h"
#include "index/graph_builder.h"
#include "index/graph_search.h"
#include "io/backend_factory.h"
#include "io/cached_io.h"
#include "io/prefetch_scheduler.h"
#include "mem/memtable.h"
#include "pq/pq_codec.h"
#include "pq/pq_trainer.h"
#include "storage/compaction.h"
#include "storage/manifest.h"
#include "storage/segment.h"
#include "util/arg_parser.h"
#include "util/thread_pool.h"

namespace {

void CheckUnknownArgs(const vindex::ArgParser& args) {
  auto unknown = args.GetUnknownKeys();
  if (!unknown.empty()) {
    std::cerr << "Warning: unknown parameter(s):";
    for (const auto& k : unknown) {
      std::cerr << " --" << k;
    }
    std::cerr << "\n";
  }
}

// Load vectors from .fvecs or .bvecs based on file extension.
bool LoadVectors(const std::string& path, vindex::FvecsData& out,
                 size_t max_vectors = 0) {
  if (path.size() >= 6 && path.compare(path.size() - 6, 6, ".bvecs") == 0) {
    vindex::BvecsData bdata;
    if (!vindex::LoadBvecs(path, bdata, max_vectors)) {
      return false;
    }
    out.dim = bdata.dim;
    out.values = std::move(bdata.values);
    return true;
  }
  return vindex::LoadFvecs(path, out, max_vectors);
}

uint64_t ComputeNextId(const std::vector<vindex::SegmentReader>& segments) {
  uint64_t next_id = 0;
  for (const auto& segment : segments) {
    uint64_t end = segment.id_offset() + segment.count();
    if (end > next_id) {
      next_id = end;
    }
  }
  return next_id;
}

bool LoadSegments(const vindex::Manifest& manifest,
                  std::vector<vindex::SegmentReader>& segments,
                  vindex::CachePool* cache = nullptr) {
  segments.clear();
  for (const auto& meta : manifest.segments()) {
    vindex::SegmentReader reader;
    if (cache) {
      auto cached_io = std::make_unique<vindex::CachedIOBackend>(
          vindex::MakeDefaultIOBackend(), cache);
      if (!reader.OpenWithIO(meta.path, meta.id_offset, std::move(cached_io))) {
        std::cerr << "Failed to open segment: " << meta.path << "\n";
        return false;
      }
    } else {
      if (!reader.Open(meta.path, meta.id_offset)) {
        std::cerr << "Failed to open segment: " << meta.path << "\n";
        return false;
      }
    }
    segments.push_back(std::move(reader));
  }
  return !segments.empty();
}

int CmdTrainPq(const vindex::ArgParser& args) {
  std::string input_path = args.Get("input");
  std::string output_path = args.Get("output");
  if (input_path.empty() || output_path.empty()) {
    std::cerr << "Missing --input or --output\n";
    return 1;
  }

  vindex::FvecsData data;
  size_t limit = args.GetSizeT("limit", 100000);
  if (!LoadVectors(input_path, data, limit)) {
    std::cerr << "Failed to load training vectors\n";
    return 1;
  }

  vindex::PQConfig config;
  config.M = static_cast<uint32_t>(args.GetInt("pq-m", 64));
  config.K = static_cast<uint32_t>(args.GetInt("pq-k", 256));
  config.max_iters = static_cast<int>(args.GetInt("pq-iters", 25));

  if (data.dim % config.M != 0) {
    std::cerr << "Error: dim " << data.dim << " not divisible by pq-m " << config.M << "\n";
    return 1;
  }

  vindex::PQCodebook codebook;
  if (!vindex::PQTrainer::Train(data.values.data(), data.count(), data.dim,
                                config, codebook)) {
    std::cerr << "Failed to train PQ codebook\n";
    return 1;
  }

  if (!vindex::PQTrainer::SaveCodebook(output_path, codebook)) {
    std::cerr << "Failed to save codebook\n";
    return 1;
  }

  std::cout << "PQ codebook saved to: " << output_path << "\n";
  std::cout << "  M=" << codebook.M << " K=" << codebook.K
            << " subspace_dim=" << codebook.subspace_dim << "\n";
  std::cout << "  Training vectors: " << data.count() << "\n";

  size_t centroids_bytes = static_cast<size_t>(codebook.M) * codebook.K *
                           codebook.subspace_dim * sizeof(float);
  std::cout << "  Codebook size: " << centroids_bytes << " bytes\n";

  CheckUnknownArgs(args);
  return 0;
}

int CmdBuild(const vindex::ArgParser& args) {
  std::string input_path = args.Get("input");
  if (input_path.empty()) {
    std::cerr << "Missing --input\n";
    return 1;
  }

  std::string codebook_path = args.Get("codebook");

  std::string output_dir = args.Get("output", "data");
  std::filesystem::create_directories(output_dir);
  std::string segment_path = args.Get("segment", output_dir + "/segment_0.vsg");
  std::string manifest_path = args.Get("manifest", output_dir + "/manifest.txt");

  vindex::FvecsData base;
  size_t limit = args.GetSizeT("limit", 0);
  if (!LoadVectors(input_path, base, limit)) {
    std::cerr << "Failed to load base vectors\n";
    return 1;
  }

  vindex::GraphBuildConfig cfg;
  cfg.degree = static_cast<uint32_t>(args.GetInt("degree", 32));
  cfg.builder = args.Get("builder", "auto");
  cfg.beam_width = static_cast<uint32_t>(args.GetInt("build-beam", 64));
  cfg.alpha = args.GetFloat("alpha", 1.2f);
  cfg.max_visits = static_cast<uint32_t>(args.GetInt("max-build-visits", 5000));

  std::vector<std::vector<vindex::VectorId>> neighbors;
  if (!vindex::BuildKnnGraph(base.values, base.dim, cfg, neighbors)) {
    std::cerr << "Failed to build graph\n";
    return 1;
  }

  bool ok = false;

  if (!codebook_path.empty()) {
    vindex::PQCodebook codebook;
    if (!vindex::PQTrainer::LoadCodebook(codebook_path, codebook)) {
      std::cerr << "Failed to load codebook from: " << codebook_path << "\n";
      return 1;
    }

    if (codebook.M * codebook.subspace_dim != base.dim) {
      std::cerr << "Codebook dim mismatch: codebook covers "
                << (codebook.M * codebook.subspace_dim) << " but data dim is "
                << base.dim << "\n";
      return 1;
    }

    std::vector<uint8_t> pq_codes;
    vindex::PQCodec::Encode(base.values.data(), base.count(), codebook, pq_codes);

    ok = vindex::SegmentWriter::WriteSegment(segment_path, base.dim, base.count(),
                                              cfg.degree, 0, base.values, neighbors,
                                              codebook, pq_codes);
    std::cout << "PQ enabled: M=" << codebook.M << " K=" << codebook.K
              << " codes=" << pq_codes.size() << " bytes\n";
  } else {
    ok = vindex::SegmentWriter::WriteSegment(segment_path, base.dim, base.count(),
                                              cfg.degree, 0, base.values, neighbors);
  }

  if (!ok) {
    std::cerr << "Failed to write segment\n";
    return 1;
  }

  vindex::Manifest manifest;
  manifest.Add({segment_path, 0});
  if (!manifest.Save(manifest_path)) {
    std::cerr << "Failed to write manifest\n";
    return 1;
  }

  std::cout << "Built segment: " << segment_path << "\n";
  std::cout << "Manifest: " << manifest_path << "\n";

  CheckUnknownArgs(args);
  return 0;
}

int CmdQuery(const vindex::ArgParser& args) {
  std::string manifest_path = args.Get("manifest");
  std::string input_path = args.Get("input");
  if (manifest_path.empty() || input_path.empty()) {
    std::cerr << "Missing --manifest or --input\n";
    return 1;
  }

  vindex::Manifest manifest;
  if (!manifest.Load(manifest_path)) {
    std::cerr << "Failed to load manifest\n";
    return 1;
  }

  size_t cache_mb = args.GetSizeT("cache", 0);
  std::unique_ptr<vindex::CachePool> cache;
  if (cache_mb > 0) {
    cache = std::make_unique<vindex::CachePool>(cache_mb * 1024 * 1024);
  }

  std::vector<vindex::SegmentReader> segments;
  if (!LoadSegments(manifest, segments, cache.get())) {
    return 1;
  }

  vindex::FvecsData queries;
  size_t limit = args.GetSizeT("limit", 0);
  if (!LoadVectors(input_path, queries, limit)) {
    std::cerr << "Failed to load queries\n";
    return 1;
  }

  vindex::SearchParams params;
  params.top_k = static_cast<uint32_t>(args.GetInt("topk", 10));
  params.beam_width = static_cast<uint32_t>(args.GetInt("beam", 8));
  params.max_visits = static_cast<uint32_t>(args.GetInt("max-visits", 1000));

  bool use_prefetch = args.HasFlag("prefetch");
  std::unique_ptr<vindex::PrefetchScheduler> prefetch;
  if (use_prefetch) {
    prefetch = std::make_unique<vindex::PrefetchScheduler>(params.beam_width);
  }

  size_t num_threads = args.GetSizeT("threads", 4);
  std::unique_ptr<vindex::ThreadPool> pool;
  if (num_threads > 1) {
    pool = std::make_unique<vindex::ThreadPool>(num_threads);
  }

  struct QueryResult {
    double visited_sum;
    uint32_t pq_dist;
    uint32_t exact_reads;
    uint32_t prefetch_hits;
    std::vector<vindex::SearchResult> top_results;
  };

  struct SegInfo {
    std::string path;
    uint64_t id_offset;
  };
  std::vector<SegInfo> seg_infos;
  for (const auto& seg : segments) {
    seg_infos.push_back({seg.path(), seg.id_offset()});
  }

  std::vector<std::future<QueryResult>> futures;

  for (size_t qi = 0; qi < queries.count(); ++qi) {
    auto task = [&, qi]() -> QueryResult {
      QueryResult qr{};
      vindex::TopK merged(params.top_k);
      for (const auto& info : seg_infos) {
        vindex::SegmentReader reader;
        if (cache) {
          auto cached_io = std::make_unique<vindex::CachedIOBackend>(
              vindex::MakeDefaultIOBackend(), cache.get());
          if (!reader.OpenWithIO(info.path, info.id_offset, std::move(cached_io))) {
            continue;
          }
        } else {
          if (!reader.Open(info.path, info.id_offset)) {
            continue;
          }
        }
        vindex::GraphSearcher searcher(reader);
        if (prefetch) {
          searcher.SetPrefetchScheduler(prefetch.get());
        }
        vindex::SearchStats stats;
        auto results = searcher.Search(queries.vector_at(qi), queries.dim, params, &stats);
        qr.visited_sum += static_cast<double>(stats.visited);
        qr.pq_dist += stats.pq_distances;
        qr.exact_reads += stats.exact_reads;
        qr.prefetch_hits += stats.prefetch_hits;
        for (const auto& r : results) {
          merged.Add(r);
        }
      }
      qr.top_results = merged.results();
      return qr;
    };

    if (pool) {
      futures.push_back(pool->Submit(task));
    } else {
      futures.push_back(std::async(std::launch::deferred, task));
    }
  }

  double avg_visited = 0.0;
  uint32_t total_pq = 0;
  uint32_t total_exact = 0;
  uint32_t total_prefetch_hits = 0;

  for (size_t qi = 0; qi < futures.size(); ++qi) {
    QueryResult qr = futures[qi].get();
    avg_visited += qr.visited_sum;
    total_pq += qr.pq_dist;
    total_exact += qr.exact_reads;
    total_prefetch_hits += qr.prefetch_hits;

    if (qi == 0) {
      std::cout << "Top-" << params.top_k << " results for query 0:\n";
      for (const auto& r : qr.top_results) {
        std::cout << "  id=" << r.id << " dist=" << r.distance << "\n";
      }
    }
  }

  if (queries.count() > 0) {
    avg_visited /= static_cast<double>(queries.count());
  }
  std::cout << "Avg visited per query: " << avg_visited;
  if (total_pq > 0) {
    std::cout << " (PQ dist: " << total_pq << ", exact reads: " << total_exact;
    if (use_prefetch) {
      std::cout << ", prefetch_hits: " << total_prefetch_hits;
    }
    std::cout << ")";
  }
  if (pool) {
    std::cout << " [threads=" << num_threads << "]";
  }
  std::cout << "\n";

  if (cache) {
    const auto& cs = cache->stats();
    std::cout << "Cache: hits=" << cs.hits << " misses=" << cs.misses
              << " hit_rate=" << cs.HitRate()
              << " peak_mb=" << (cs.peak_memory / 1024.0 / 1024.0) << "\n";
  }

  CheckUnknownArgs(args);
  return 0;
}

int CmdEval(const vindex::ArgParser& args) {
  std::string manifest_path = args.Get("manifest");
  std::string input_path = args.Get("input");
  std::string gt_path = args.Get("groundtruth");
  if (manifest_path.empty() || input_path.empty() || gt_path.empty()) {
    std::cerr << "Missing --manifest, --input, or --groundtruth\n";
    return 1;
  }

  vindex::Manifest manifest;
  if (!manifest.Load(manifest_path)) {
    std::cerr << "Failed to load manifest\n";
    return 1;
  }

  size_t cache_mb = args.GetSizeT("cache", 0);
  std::unique_ptr<vindex::CachePool> cache;
  if (cache_mb > 0) {
    cache = std::make_unique<vindex::CachePool>(cache_mb * 1024 * 1024);
  }

  std::vector<vindex::SegmentReader> segments;
  if (!LoadSegments(manifest, segments, cache.get())) {
    return 1;
  }

  bool use_pq = std::any_of(segments.begin(), segments.end(),
                            [](const auto& s) { return s.HasPQ(); });

  vindex::FvecsData queries;
  size_t limit = args.GetSizeT("limit", 0);
  if (!LoadVectors(input_path, queries, limit)) {
    std::cerr << "Failed to load queries\n";
    return 1;
  }

  vindex::IvecsData groundtruth;
  if (!vindex::LoadIvecs(gt_path, groundtruth, limit)) {
    std::cerr << "Failed to load groundtruth\n";
    return 1;
  }

  // Always search for enough results to compute recall at multiple K.
  const uint32_t kEvalTopK = 100;
  vindex::SearchParams params;
  params.top_k = kEvalTopK;
  params.beam_width = static_cast<uint32_t>(args.GetInt("beam", 8));
  params.max_visits = static_cast<uint32_t>(args.GetInt("max-visits", 1000));

  bool use_prefetch = args.HasFlag("prefetch");
  std::unique_ptr<vindex::PrefetchScheduler> prefetch;
  if (use_prefetch) {
    prefetch = std::make_unique<vindex::PrefetchScheduler>(params.beam_width);
  }

  size_t num_threads = args.GetSizeT("threads", 4);
  std::unique_ptr<vindex::ThreadPool> pool;
  if (num_threads > 1) {
    pool = std::make_unique<vindex::ThreadPool>(num_threads);
  }

  struct SegInfo {
    std::string path;
    uint64_t id_offset;
  };
  std::vector<SegInfo> seg_infos;
  for (const auto& seg : segments) {
    seg_infos.push_back({seg.path(), seg.id_offset()});
  }

  size_t queries_count = std::min(queries.count(), groundtruth.count());

  // Per-query results.
  std::vector<double> recall_at_1(queries_count, 0.0);
  std::vector<double> recall_at_10(queries_count, 0.0);
  std::vector<double> recall_at_100(queries_count, 0.0);
  std::vector<double> latencies_ms(queries_count, 0.0);
  vindex::SearchStats total_stats{};
  std::mutex result_mutex;
  std::vector<std::future<void>> futures;

  auto t_start = std::chrono::high_resolution_clock::now();

  for (size_t qi = 0; qi < queries_count; ++qi) {
    auto task = [&, qi]() {
      auto t0 = std::chrono::high_resolution_clock::now();

      vindex::TopK merged(params.top_k);
      vindex::SearchStats local_stats;
      for (const auto& info : seg_infos) {
        vindex::SegmentReader reader;
        if (cache) {
          auto cached_io = std::make_unique<vindex::CachedIOBackend>(
              vindex::MakeDefaultIOBackend(), cache.get());
          if (!reader.OpenWithIO(info.path, info.id_offset, std::move(cached_io))) {
            continue;
          }
        } else {
          if (!reader.Open(info.path, info.id_offset)) {
            continue;
          }
        }
        vindex::GraphSearcher searcher(reader);
        if (prefetch) {
          searcher.SetPrefetchScheduler(prefetch.get());
        }
        vindex::SearchStats seg_stats;
        auto results = searcher.Search(queries.vector_at(qi), queries.dim, params, &seg_stats);
        local_stats.visited += seg_stats.visited;
        local_stats.pq_distances += seg_stats.pq_distances;
        local_stats.exact_reads += seg_stats.exact_reads;
        local_stats.prefetch_hits += seg_stats.prefetch_hits;
        for (const auto& r : results) {
          merged.Add(r);
        }
      }

      auto t1 = std::chrono::high_resolution_clock::now();
      double latency = std::chrono::duration<double, std::milli>(t1 - t0).count();

      auto search_results = merged.results();
      const int32_t* gt = groundtruth.vector_at(qi);
      uint32_t gt_count = std::min(groundtruth.dim, kEvalTopK);

      // Compute recall at multiple K.
      auto recall_at_k = [&](uint32_t k) -> double {
        if (k == 0 || k > search_results.size()) return 0.0;
        std::unordered_set<int32_t> gt_set;
        for (uint32_t i = 0; i < std::min(k, gt_count); ++i) gt_set.insert(gt[i]);
        uint32_t hit = 0;
        for (uint32_t i = 0; i < k && i < search_results.size(); ++i) {
          if (gt_set.find(static_cast<int32_t>(search_results[i].id)) != gt_set.end()) ++hit;
        }
        return static_cast<double>(hit) / static_cast<double>(k);
      };

      double r1 = recall_at_k(1);
      double r10 = recall_at_k(10);
      double r100 = recall_at_k(100);

      {
        std::lock_guard lk(result_mutex);
        recall_at_1[qi] = r1;
        recall_at_10[qi] = r10;
        recall_at_100[qi] = r100;
        latencies_ms[qi] = latency;
        total_stats.visited += local_stats.visited;
        total_stats.pq_distances += local_stats.pq_distances;
        total_stats.exact_reads += local_stats.exact_reads;
        total_stats.prefetch_hits += local_stats.prefetch_hits;
      }
    };

    if (pool) {
      futures.push_back(pool->Submit(task));
    } else {
      futures.push_back(std::async(std::launch::deferred, task));
    }
  }

  for (auto& f : futures) f.get();

  auto t_end = std::chrono::high_resolution_clock::now();
  double total_sec = std::chrono::duration<double>(t_end - t_start).count();

  // Aggregate.
  double avg_r1 = 0, avg_r10 = 0, avg_r100 = 0;
  for (size_t i = 0; i < queries_count; ++i) {
    avg_r1 += recall_at_1[i];
    avg_r10 += recall_at_10[i];
    avg_r100 += recall_at_100[i];
  }
  if (queries_count > 0) {
    avg_r1 /= queries_count;
    avg_r10 /= queries_count;
    avg_r100 /= queries_count;
  }

  // Latency percentiles.
  auto sorted_lat = latencies_ms;
  std::sort(sorted_lat.begin(), sorted_lat.end());
  auto pct = [&](double p) -> double {
    if (sorted_lat.empty()) return 0.0;
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * sorted_lat.size())) - 1;
    if (i >= sorted_lat.size()) i = sorted_lat.size() - 1;
    return sorted_lat[i];
  };
  double avg_lat = 0;
  for (double l : latencies_ms) avg_lat += l;
  if (!latencies_ms.empty()) avg_lat /= latencies_ms.size();
  double qps = total_sec > 0 ? queries_count / total_sec : 0.0;

  bool use_csv = (args.Get("format") == "csv");

  if (use_csv) {
    // Single header line + single data line for script parsing.
    std::cout << "recall_1,recall_10,recall_100,"
              << "lat_avg_ms,lat_p50_ms,lat_p95_ms,lat_p99_ms,qps,"
              << "total_visited,total_pq_dist,total_exact_reads,total_prefetch_hits,"
              << "avg_visited,avg_pq_dist,avg_exact_reads,avg_prefetch_hits,"
              << "num_queries,num_threads,beam,max_visits,prefetch,cache_mb";
    if (cache) {
      std::cout << ",cache_hit_rate,cache_peak_mb";
    }
    std::cout << "\n";

    std::cout << avg_r1 << "," << avg_r10 << "," << avg_r100 << ","
              << avg_lat << "," << pct(50) << "," << pct(95) << "," << pct(99) << "," << qps << ","
              << total_stats.visited << "," << total_stats.pq_distances << ","
              << total_stats.exact_reads << "," << total_stats.prefetch_hits << ",";
    double n = static_cast<double>(queries_count);
    std::cout << (n > 0 ? total_stats.visited / n : 0) << ","
              << (n > 0 ? total_stats.pq_distances / n : 0) << ","
              << (n > 0 ? total_stats.exact_reads / n : 0) << ","
              << (n > 0 ? total_stats.prefetch_hits / n : 0) << ","
              << queries_count << "," << num_threads << ","
              << params.beam_width << "," << params.max_visits << ","
              << (use_prefetch ? 1 : 0) << "," << cache_mb;
    if (cache) {
      const auto& cs = cache->stats();
      std::cout << "," << cs.HitRate() << "," << (cs.peak_memory / 1024.0 / 1024.0);
    }
    std::cout << "\n";
  } else {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "=== Eval Results (" << queries_count << " queries) ===\n";
    if (use_pq) std::cout << "PQ: M=" << segments[0].PqM() << "  ";
    std::cout << "beam=" << params.beam_width
              << "  max_visits=" << params.max_visits
              << "  threads=" << num_threads;
    if (prefetch) std::cout << "  prefetch=on";
    if (cache) std::cout << "  cache=" << cache_mb << "MB";
    std::cout << "\n\n";

    std::cout << "Accuracy:\n";
    std::cout << "  Recall@1:   " << avg_r1 << "\n";
    std::cout << "  Recall@10:  " << avg_r10 << "\n";
    std::cout << "  Recall@100: " << avg_r100 << "\n\n";

    std::cout << std::setprecision(1);
    std::cout << "Latency (ms):\n";
    std::cout << "  avg: " << avg_lat << "   p50: " << pct(50)
              << "   p95: " << pct(95) << "   p99: " << pct(99) << "\n";
    std::cout << "  QPS: " << qps << "\n\n";

    double n = std::max(1.0, static_cast<double>(queries_count));
    std::cout << std::setprecision(1);
    std::cout << "System Metrics (per query avg):\n";
    std::cout << "  visited:         " << (total_stats.visited / n) << "\n";
    std::cout << "  pq_distances:    " << (total_stats.pq_distances / n) << "\n";
    std::cout << "  exact_reads:     " << (total_stats.exact_reads / n) << "\n";
    std::cout << "  prefetch_hits:   " << (total_stats.prefetch_hits / n) << "\n\n";

    if (cache) {
      const auto& cs = cache->stats();
      std::cout << std::setprecision(1);
      std::cout << "Cache:\n";
      std::cout << "  hits=" << cs.hits << "  misses=" << cs.misses
                << "  hit_rate=" << cs.HitRate()
                << "  peak_mb=" << (cs.peak_memory / 1024.0 / 1024.0) << "\n";
    }
  }

  CheckUnknownArgs(args);
  return 0;
}

int CmdInsert(const vindex::ArgParser& args) {
  std::string manifest_path = args.Get("manifest");
  std::string input_path = args.Get("input");
  std::string output_dir = args.Get("output", "data");
  if (manifest_path.empty() || input_path.empty()) {
    std::cerr << "Missing --manifest or --input\n";
    return 1;
  }

  vindex::Manifest manifest;
  if (!manifest.Load(manifest_path)) {
    std::cerr << "Failed to load manifest\n";
    return 1;
  }

  std::vector<vindex::SegmentReader> segments;
  if (!LoadSegments(manifest, segments)) {
    return 1;
  }

  vindex::FvecsData input;
  size_t limit = args.GetSizeT("limit", 0);
  if (!LoadVectors(input_path, input, limit)) {
    std::cerr << "Failed to load input vectors\n";
    return 1;
  }

  if (input.dim != segments.front().dim()) {
    std::cerr << "Input dim mismatch\n";
    return 1;
  }

  std::filesystem::create_directories(output_dir);

  // Save updated manifest to output directory (don't overwrite original).
  std::string out_manifest_path = output_dir + "/manifest.txt";

  vindex::GraphBuildConfig cfg;
  cfg.degree = static_cast<uint32_t>(args.GetInt("degree", 32));
  cfg.builder = args.Get("builder", "auto");
  cfg.beam_width = static_cast<uint32_t>(args.GetInt("build-beam", 64));
  cfg.alpha = args.GetFloat("alpha", 1.2f);
  cfg.max_visits = static_cast<uint32_t>(args.GetInt("max-build-visits", 5000));
  size_t flush = args.GetSizeT("flush", 0);
  const size_t batch_limit = flush == 0 ? input.count() : flush;

  const vindex::PQCodebook* shared_codebook = nullptr;
  auto pq_it = std::find_if(segments.begin(), segments.end(),
                            [](const auto& segment) { return segment.HasPQ(); });
  if (pq_it != segments.end()) {
    shared_codebook = &pq_it->codebook();
    if (!shared_codebook->IsValid()) {
      std::cerr << "Loaded PQ codebook is invalid\n";
      return 1;
    }
  }

  uint64_t next_id = ComputeNextId(segments);
  size_t batch_index = 0;
  vindex::MemTable buffer(input.dim);

  auto flush_buffer = [&](vindex::MemTable& memtable) -> bool {
    size_t batch_count = memtable.count();
    if (batch_count == 0) {
      return true;
    }

    std::vector<float> batch_vectors = memtable.TakeValues();
    std::vector<std::vector<vindex::VectorId>> neighbors;
    if (!vindex::BuildKnnGraph(batch_vectors, input.dim, cfg, neighbors)) {
      std::cerr << "Failed to build graph for batch\n";
      return false;
    }

    std::string segment_path = output_dir + "/segment_insert_" + std::to_string(batch_index) + ".vsg";
    bool ok = false;
    if (shared_codebook != nullptr) {
      std::vector<uint8_t> pq_codes;
      vindex::PQCodec::Encode(batch_vectors.data(), batch_count, *shared_codebook, pq_codes);
      ok = vindex::SegmentWriter::WriteSegment(segment_path, input.dim, batch_count,
                                               cfg.degree, 0, batch_vectors, neighbors,
                                               *shared_codebook, pq_codes);
    } else {
      ok = vindex::SegmentWriter::WriteSegment(segment_path, input.dim, batch_count,
                                               cfg.degree, 0, batch_vectors, neighbors);
    }

    if (!ok) {
      std::cerr << "Failed to write segment\n";
      return false;
    }

    manifest.Add({segment_path, next_id, 0});
    next_id += batch_count;
    ++batch_index;
    return true;
  };

  vindex::CompactionScheduler compactor(out_manifest_path, output_dir, input.dim);
  compactor.SetCallback([](const std::string& msg) {
    std::cout << "[compaction] " << msg << "\n";
  });

  for (size_t i = 0; i < input.count(); ++i) {
    buffer.Add(input.vector_at(i));
    if (buffer.count() >= batch_limit) {
      if (!flush_buffer(buffer)) {
        return 1;
      }
      if (!manifest.Save(out_manifest_path)) {
        std::cerr << "Failed to save manifest\n";
        return 1;
      }
      compactor.ScheduleCheck();
    }
  }

  if (!buffer.empty()) {
    if (!flush_buffer(buffer)) {
      return 1;
    }
    // Only save if we just flushed remaining data.
    if (!manifest.Save(out_manifest_path)) {
      std::cerr << "Failed to save manifest\n";
      return 1;
    }
  }

  compactor.ScheduleCheck();
  compactor.WaitIdle();

  std::cout << "Inserted " << input.count() << " vectors into " << batch_index << " segment(s)\n";

  CheckUnknownArgs(args);
  return 0;
}

int CmdStress(const vindex::ArgParser& args) {
  std::string manifest_path = args.Get("manifest");
  std::string input_path = args.Get("input");    // query vectors
  std::string gt_path = args.Get("groundtruth");
  std::string write_input_path = args.Get("write-input");
  if (manifest_path.empty() || input_path.empty() || gt_path.empty() || write_input_path.empty()) {
    std::cerr << "Missing --manifest, --input, --groundtruth, or --write-input\n";
    return 1;
  }

  size_t cache_mb = args.GetSizeT("cache", 0);
  std::unique_ptr<vindex::CachePool> cache;
  if (cache_mb > 0) {
    cache = std::make_unique<vindex::CachePool>(cache_mb * 1024 * 1024);
  }

  vindex::Manifest manifest;
  if (!manifest.Load(manifest_path)) {
    std::cerr << "Failed to load manifest\n";
    return 1;
  }
  std::string data_dir = args.Get("output", "data_stress");
  std::filesystem::create_directories(data_dir);

  // Load query vectors and groundtruth.
  vindex::FvecsData queries;
  size_t qlimit = args.GetSizeT("limit", 0);
  if (!LoadVectors(input_path, queries, qlimit)) {
    std::cerr << "Failed to load queries\n";
    return 1;
  }
  vindex::IvecsData groundtruth;
  if (!vindex::LoadIvecs(gt_path, groundtruth, qlimit)) {
    std::cerr << "Failed to load groundtruth\n";
    return 1;
  }

  // Load vectors for writing.
  vindex::FvecsData write_pool;
  size_t wlimit = args.GetSizeT("write-limit", 0);
  if (!LoadVectors(write_input_path, write_pool, wlimit)) {
    std::cerr << "Failed to load write vectors\n";
    return 1;
  }
  uint32_t dim = write_pool.dim;

  vindex::SearchParams params;
  params.top_k = 100;
  params.beam_width = static_cast<uint32_t>(args.GetInt("beam", 8));
  params.max_visits = static_cast<uint32_t>(args.GetInt("max-visits", 1000));

  size_t num_readers = args.GetSizeT("read-threads", 4);
  size_t num_writers = args.GetSizeT("write-threads", 1);
  size_t write_batch = args.GetSizeT("write-batch-size", 100);
  int duration_sec = args.GetInt("duration", 30);

  // Snapshot initial segment metadata.
  std::vector<vindex::SegmentReader> initial_segments;
  if (!LoadSegments(manifest, initial_segments, cache.get())) {
    return 1;
  }
  struct SegInfo { std::string path; uint64_t id_offset; };
  std::vector<SegInfo> base_seg_infos;
  for (const auto& seg : initial_segments) base_seg_infos.push_back({seg.path(), seg.id_offset()});

  // Shared state.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> total_reads{0};
  std::atomic<uint64_t> total_writes{0};
  std::mutex stats_mutex;
  std::vector<double> all_latencies;
  std::vector<double> all_recalls_10;
  vindex::SearchStats global_stats{};

  // Writer state.
  std::mutex manifest_mutex;
  std::mutex memtable_mutex;
  vindex::MemTable memtable(dim);
  uint64_t next_write_id = ComputeNextId(initial_segments);
  size_t batch_index = 0;
  std::string out_manifest_path = data_dir + "/manifest.txt";
  std::atomic<size_t> write_idx{0};

  vindex::GraphBuildConfig build_cfg;
  build_cfg.degree = static_cast<uint32_t>(args.GetInt("degree", 32));
  build_cfg.builder = args.Get("builder", "auto");
  build_cfg.beam_width = static_cast<uint32_t>(args.GetInt("build-beam", 64));
  build_cfg.alpha = args.GetFloat("alpha", 1.2f);
  build_cfg.max_visits = static_cast<uint32_t>(args.GetInt("max-build-visits", 5000));

  // Check if existing segments use PQ.
  const vindex::PQCodebook* shared_codebook = nullptr;
  auto pq_it = std::find_if(initial_segments.begin(), initial_segments.end(),
                            [](const auto& s) { return s.HasPQ(); });
  if (pq_it != initial_segments.end()) shared_codebook = &pq_it->codebook();

  // Copy manifest for writer.
  vindex::Manifest writer_manifest = manifest;
  if (!writer_manifest.Load(manifest_path)) return 1;

  vindex::CompactionScheduler compactor(out_manifest_path, data_dir, dim);
  compactor.SetCallback([](const std::string& msg) {
    std::cout << "[compaction] " << msg << "\n";
  });

  // --- Writer threads ---
  std::vector<std::thread> writers;
  for (size_t w = 0; w < num_writers; ++w) {
    writers.emplace_back([&]() {
      while (!stop.load()) {
        size_t idx = write_idx.fetch_add(1);
        if (idx >= write_pool.count()) break;

        const float* vec = write_pool.vector_at(idx);

        // Add to protected MemTable.
        std::vector<float> batch_vectors;
        size_t batch_count = 0;
        uint64_t base_id;
        {
          std::lock_guard lk(memtable_mutex);
          memtable.Add(vec);
          if (memtable.count() < write_batch) continue;
          batch_vectors = memtable.TakeValues();
          batch_count = batch_vectors.size() / dim;
          base_id = next_write_id;
          next_write_id += batch_count;
        }

        // Build graph and flush.
        std::vector<std::vector<vindex::VectorId>> neighbors;
        if (!vindex::BuildKnnGraph(batch_vectors, dim, build_cfg, neighbors)) continue;

        std::string seg_path = data_dir + "/stress_seg_" + std::to_string(batch_index) + ".vsg";
        bool ok = false;
        if (shared_codebook) {
          std::vector<uint8_t> pq_codes;
          vindex::PQCodec::Encode(batch_vectors.data(), batch_count, *shared_codebook, pq_codes);
          ok = vindex::SegmentWriter::WriteSegment(seg_path, dim, batch_count,
                                                   build_cfg.degree, 0, batch_vectors, neighbors,
                                                   *shared_codebook, pq_codes);
        } else {
          ok = vindex::SegmentWriter::WriteSegment(seg_path, dim, batch_count,
                                                   build_cfg.degree, 0, batch_vectors, neighbors);
        }
        if (!ok) continue;

        {
          std::lock_guard lk(manifest_mutex);
          writer_manifest.Add({seg_path, base_id, 0});
          writer_manifest.Save(out_manifest_path);
        }
        ++batch_index;
        total_writes.fetch_add(batch_count);
        compactor.ScheduleCheck();
      }
    });
  }

  // Warm up: let readers start first.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // --- Reader threads ---
  std::vector<std::thread> readers;
  std::atomic<size_t> query_idx{0};
  size_t query_pool_size = queries.count();

  auto t_start = std::chrono::high_resolution_clock::now();

  for (size_t r = 0; r < num_readers; ++r) {
    readers.emplace_back([&]() {
      std::mt19937 rng(static_cast<uint32_t>(r + 42));
      std::uniform_int_distribution<size_t> dist(0, query_pool_size - 1);

      while (!stop.load()) {
        size_t qi = dist(rng);
        auto t0 = std::chrono::high_resolution_clock::now();

        vindex::TopK merged(params.top_k);
        vindex::SearchStats local_stats;
        for (const auto& info : base_seg_infos) {
          vindex::SegmentReader reader;
          if (cache) {
            auto io = std::make_unique<vindex::CachedIOBackend>(
                vindex::MakeDefaultIOBackend(), cache.get());
            if (!reader.OpenWithIO(info.path, info.id_offset, std::move(io))) continue;
          } else {
            if (!reader.Open(info.path, info.id_offset)) continue;
          }
          vindex::GraphSearcher searcher(reader);
          vindex::SearchStats seg_stats;
          auto results = searcher.Search(queries.vector_at(qi), queries.dim, params, &seg_stats);
          local_stats.visited += seg_stats.visited;
          local_stats.pq_distances += seg_stats.pq_distances;
          local_stats.exact_reads += seg_stats.exact_reads;
          local_stats.prefetch_hits += seg_stats.prefetch_hits;
          for (const auto& rr : results) merged.Add(rr);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double lat = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Compute Recall@10.
        auto search_results = merged.results();
        const int32_t* gt = groundtruth.vector_at(qi);
        uint32_t gt_count = std::min(groundtruth.dim, 100u);
        std::unordered_set<int32_t> gt_set;
        for (uint32_t k = 0; k < std::min(10u, gt_count); ++k) gt_set.insert(gt[k]);
        uint32_t hit = 0;
        for (uint32_t k = 0; k < 10 && k < search_results.size(); ++k) {
          if (gt_set.find(static_cast<int32_t>(search_results[k].id)) != gt_set.end()) ++hit;
        }
        double r10 = static_cast<double>(hit) / 10.0;

        {
          std::lock_guard lk(stats_mutex);
          all_latencies.push_back(lat);
          all_recalls_10.push_back(r10);
          global_stats.visited += local_stats.visited;
          global_stats.pq_distances += local_stats.pq_distances;
          global_stats.exact_reads += local_stats.exact_reads;
          global_stats.prefetch_hits += local_stats.prefetch_hits;
        }
        total_reads.fetch_add(1);
      }
    });
  }

  // --- Monitor ---
  std::cout << "time,read_qps,p50_ms,p95_ms,p99_ms,write_ops,recall10\n";
  auto last_sample = t_start;
  uint64_t last_reads = 0;

  for (int sec = 0; sec < duration_sec && !stop.load(); ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto now = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_sample).count();
    uint64_t cur_reads = total_reads.load();
    uint64_t cur_writes = total_writes.load();

    double read_qps = elapsed > 0 ? (cur_reads - last_reads) / elapsed : 0;
    last_reads = cur_reads;
    last_sample = now;

    std::vector<double> lat_snapshot;
    std::vector<double> rec_snapshot;
    {
      std::lock_guard lk(stats_mutex);
      lat_snapshot = all_latencies;
      rec_snapshot = all_recalls_10;
    }
    std::sort(lat_snapshot.begin(), lat_snapshot.end());
    auto pct_snap = [&](double p) {
      if (lat_snapshot.empty()) return 0.0;
      size_t i = static_cast<size_t>(p / 100.0 * lat_snapshot.size());
      if (i >= lat_snapshot.size()) i = lat_snapshot.size() - 1;
      return lat_snapshot[i];
    };
    double avg_recall = 0;
    for (double r : rec_snapshot) avg_recall += r;
    if (!rec_snapshot.empty()) avg_recall /= rec_snapshot.size();

    std::cout << sec << "," << read_qps << "," << pct_snap(50) << ","
              << pct_snap(95) << "," << pct_snap(99) << ","
              << cur_writes << "," << avg_recall;
    if (cache) {
      const auto& cs = cache->stats();
      std::cout << "," << cs.HitRate() << "," << (cs.peak_memory / 1024.0 / 1024.0);
    }
    std::cout << "\n";
  }

  stop.store(true);

  for (auto& t : readers) if (t.joinable()) t.join();
  for (auto& t : writers) if (t.joinable()) t.join();
  compactor.WaitIdle();

  auto t_end = std::chrono::high_resolution_clock::now();
  double total_sec = std::chrono::duration<double>(t_end - t_start).count();

  // Final summary.
  std::sort(all_latencies.begin(), all_latencies.end());
  auto fpct = [&](double p) {
    if (all_latencies.empty()) return 0.0;
    size_t i = static_cast<size_t>(p / 100.0 * all_latencies.size());
    if (i >= all_latencies.size()) i = all_latencies.size() - 1;
    return all_latencies[i];
  };
  double avg_lat = 0, avg_r10 = 0;
  for (double l : all_latencies) avg_lat += l;
  for (double r : all_recalls_10) avg_r10 += r;
  if (!all_latencies.empty()) avg_lat /= all_latencies.size();
  if (!all_recalls_10.empty()) avg_r10 /= all_recalls_10.size();

  double n = std::max(1.0, static_cast<double>(all_latencies.size()));
  std::cout << "\n=== Stress Summary (" << total_sec << "s) ===\n";
  std::cout << "Reads: " << total_reads.load() << "  Writes: " << total_writes.load()
            << "  QPS: " << (total_sec > 0 ? total_reads.load() / total_sec : 0) << "\n";
  std::cout << "Recall@10: " << avg_r10 << "\n";
  std::cout << "Latency (ms): avg=" << avg_lat
            << " p50=" << fpct(50) << " p95=" << fpct(95) << " p99=" << fpct(99) << "\n";
  std::cout << "Per-query avg: visited=" << (global_stats.visited / n)
            << " exact_reads=" << (global_stats.exact_reads / n)
            << " prefetch_hits=" << (global_stats.prefetch_hits / n) << "\n";
  if (cache) {
    const auto& cs = cache->stats();
    std::cout << "Cache: hit_rate=" << cs.HitRate()
              << " peak_mb=" << (cs.peak_memory / 1024.0 / 1024.0) << "\n";
  }

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  vindex::ArgParser args(argc, argv);

  // Load config file before anything else (CLI args take precedence).
  std::string config_path = args.Get("config");
  if (!config_path.empty()) {
    if (!args.LoadConfigFile(config_path)) {
      std::cerr << "Warning: failed to load config file: " << config_path << "\n";
    }
  }

  // Resolve --dataset profile (CLI args take precedence).
  std::string dataset_name = args.Get("dataset");
  if (!dataset_name.empty()) {
    args.LoadDatasetProfile(dataset_name);
  }

  if (args.positionals().empty()) {
    vindex::ArgParser::PrintHelp();
    return 1;
  }

  std::string command = args.positionals().front();

  // Per-subcommand help.
  if (args.HasFlag("help")) {
    vindex::ArgParser::PrintHelp(command);
    return 0;
  }

  if (command == "build") {
    return CmdBuild(args);
  }
  if (command == "query") {
    return CmdQuery(args);
  }
  if (command == "eval") {
    return CmdEval(args);
  }
  if (command == "insert") {
    return CmdInsert(args);
  }
  if (command == "stress") {
    return CmdStress(args);
  }
  if (command == "train-pq") {
    return CmdTrainPq(args);
  }

  vindex::ArgParser::PrintHelp();
  return 1;
}
