#include "storage/compaction.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>

#include "dataset/fvecs.h"
#include "index/graph_builder.h"
#include "pq/pq_codec.h"
#include "storage/manifest.h"
#include "storage/segment.h"

namespace vindex {

static constexpr uint32_t kMaxLevel0Segments = 4;
static constexpr uint32_t kMaxLevelNSegments = 2;

CompactionScheduler::CompactionScheduler(const std::string& manifest_path,
                                         const std::string& data_dir,
                                         uint32_t dim)
    : manifest_path_(manifest_path), data_dir_(data_dir), dim_(dim),
      worker_(&CompactionScheduler::WorkerLoop, this) {}

CompactionScheduler::~CompactionScheduler() {
  stop_.store(true);
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void CompactionScheduler::ScheduleCheck() {
  pending_.store(true);
  cv_.notify_one();
}

void CompactionScheduler::Pause() {
  paused_.store(true);
}

void CompactionScheduler::Resume() {
  paused_.store(false);
  cv_.notify_one();
}

void CompactionScheduler::WaitIdle() {
  while (pending_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void CompactionScheduler::WorkerLoop() {
  while (!stop_.load()) {
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return pending_.load() || stop_.load();
      });
    }

    if (stop_.load()) break;
    if (paused_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    pending_.store(false);

    Manifest manifest;
    if (!manifest.Load(manifest_path_)) continue;

    bool did_work = false;

    // Group segments by level (only segments in our output directory).
    std::map<uint32_t, std::vector<SegmentMeta>> by_level;
    for (const auto& seg : manifest.segments()) {
      if (seg.path.rfind(data_dir_, 0) != 0) continue;  // skip original dataset segments
      by_level[seg.level].push_back(seg);
    }

    for (auto& [level, segs] : by_level) {
      uint32_t max_allowed = (level == 0) ? kMaxLevel0Segments : kMaxLevelNSegments;
      if (segs.size() > max_allowed) {
        if (CompactLevel(level, manifest)) {
          did_work = true;
          break;  // One compaction per check.
        }
      }
    }

    if (did_work) {
      compaction_count_.fetch_add(1);
      // Re-check in case cascading compactions are needed.
      pending_.store(true);
      cv_.notify_one();
    }
  }
}

bool CompactionScheduler::CompactLevel(uint32_t level, Manifest& manifest) {
  if (callback_) {
    callback_("Compacting level " + std::to_string(level));
  }

  // Collect segments at this level.
  std::vector<SegmentMeta> source_metas;
  for (const auto& seg : manifest.segments()) {
    if (seg.level == level) {
      source_metas.push_back(seg);
    }
  }

  if (source_metas.empty()) return false;

  // Read all vectors from source segments in one sequential read each.
  std::vector<float> all_vectors;
  uint64_t total_count = 0;

  for (const auto& meta : source_metas) {
    SegmentReader reader;
    if (!reader.Open(meta.path, meta.id_offset)) {
      if (callback_) {
        callback_("Failed to open segment: " + meta.path);
      }
      return false;
    }

    std::vector<float> seg_vectors;
    if (!reader.ReadAllVectors(seg_vectors)) {
      if (callback_) {
        callback_("Failed to read vectors from: " + meta.path);
      }
      return false;
    }
    all_vectors.insert(all_vectors.end(), seg_vectors.begin(), seg_vectors.end());
    total_count += reader.count();
  }

  if (total_count == 0) return false;

  // Scale max_visits to batch size — small batches don't need deep search.
  uint32_t scaled_visits = max_build_visits_;
  if (total_count < 5000) {
    scaled_visits = std::max(500u, static_cast<uint32_t>(total_count / 4));
  }

  // Rebuild KNN graph.
  GraphBuildConfig cfg;
  cfg.degree = degree_;
  cfg.beam_width = build_beam_;
  cfg.alpha = 1.2f;
  cfg.max_visits = scaled_visits;
  cfg.builder = (total_count > 10000) ? "vamana" : "brute";
  std::vector<std::vector<VectorId>> new_neighbors;
  if (!BuildKnnGraph(all_vectors, dim_, cfg, new_neighbors)) {
    return false;
  }

  // Preserve PQ codebook from source segment if available.
  const PQCodebook* codebook = nullptr;
  {
    SegmentReader first;
    if (first.Open(source_metas.front().path, source_metas.front().id_offset) &&
        first.HasPQ()) {
      codebook = &first.codebook();
    }
  }

  // Write new segment at next level.
  uint64_t new_offset = source_metas.front().id_offset;
  std::string new_path = data_dir_ + "/segment_L" +
                         std::to_string(level + 1) + "_" +
                         std::to_string(compaction_count_.load()) + ".vsg";

  bool ok;
  if (codebook && codebook->IsValid()) {
    std::vector<uint8_t> pq_codes;
    PQCodec::Encode(all_vectors.data(), total_count, *codebook, pq_codes);
    ok = SegmentWriter::WriteSegment(new_path, dim_, total_count, degree_,
                                     0, all_vectors, new_neighbors,
                                     *codebook, pq_codes);
  } else {
    ok = SegmentWriter::WriteSegment(new_path, dim_, total_count, degree_,
                                     0, all_vectors, new_neighbors);
  }
  if (!ok) {
    return false;
  }

  // Reload manifest to pick up any segments added during graph build,
  // then atomically swap source segments for the new compacted segment.
  Manifest latest;
  if (!latest.Load(manifest_path_)) {
    return false;
  }

  for (const auto& meta : source_metas) {
    latest.Remove(meta.path);
  }

  SegmentMeta new_meta;
  new_meta.path = new_path;
  new_meta.id_offset = new_offset;
  new_meta.level = level + 1;
  latest.Add(new_meta);

  if (!latest.Save(manifest_path_)) {
    return false;
  }

  // Delete old segment files.
  for (const auto& meta : source_metas) {
    std::error_code ec;
    std::filesystem::remove(meta.path, ec);
  }

  if (callback_) {
    callback_("Compacted " + std::to_string(source_metas.size()) +
              " segments (level " + std::to_string(level) +
              ") into " + new_path);
  }

  return true;
}

}  // namespace vindex
