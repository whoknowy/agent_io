#include <algorithm>
#include <atomic>
#include <filesystem>
#include <future>
#include <iostream>
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
  if (use_pq) {
    std::cout << "PQ mode detected, M=" << segments[0].PqM() << "\n";
  }

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

  // Snapshot segment metadata so each thread can open its own reader.
  struct SegInfo {
    std::string path;
    uint64_t id_offset;
  };
  std::vector<SegInfo> seg_infos;
  for (const auto& seg : segments) {
    seg_infos.push_back({seg.path(), seg.id_offset()});
  }

  size_t queries_count = std::min(queries.count(), groundtruth.count());
  double recall_sum = 0.0;
  std::mutex recall_mutex;
  std::vector<std::future<void>> futures;

  for (size_t qi = 0; qi < queries_count; ++qi) {
    auto task = [&, qi]() {
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
        auto results = searcher.Search(queries.vector_at(qi), queries.dim, params, nullptr);
        for (const auto& r : results) {
          merged.Add(r);
        }
      }

      std::unordered_set<int32_t> gt_ids;
      const int32_t* gt = groundtruth.vector_at(qi);
      for (uint32_t k = 0; k < params.top_k && k < groundtruth.dim; ++k) {
        gt_ids.insert(gt[k]);
      }

      uint32_t hit = 0;
      for (const auto& r : merged.results()) {
        if (gt_ids.find(static_cast<int32_t>(r.id)) != gt_ids.end()) {
          ++hit;
        }
      }
      {
        std::lock_guard lk(recall_mutex);
        recall_sum += static_cast<double>(hit) / static_cast<double>(params.top_k);
      }
    };

    if (pool) {
      futures.push_back(pool->Submit(task));
    } else {
      futures.push_back(std::async(std::launch::deferred, task));
    }
  }

  for (auto& f : futures) {
    f.get();
  }

  double recall = queries_count > 0 ? recall_sum / static_cast<double>(queries_count) : 0.0;
  std::cout << "Recall@" << params.top_k << ": " << recall << "\n";

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
  if (command == "train-pq") {
    return CmdTrainPq(args);
  }

  vindex::ArgParser::PrintHelp();
  return 1;
}
