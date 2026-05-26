#include "index/vamana_builder.h"

#include <algorithm>
#include <random>
#include <unordered_set>

#include "core/distance.h"

namespace vindex {

namespace {

// Greedy beam search used during graph construction.
// Returns up to beam_width nearest candidates.
std::vector<uint32_t> GreedySearch(const std::vector<float>& vectors,
                                   uint32_t dim,
                                   const std::vector<std::vector<VectorId>>& graph,
                                   uint32_t entry,
                                   const float* query,
                                   uint32_t beam_width,
                                   uint32_t max_visits) {
  size_t count = vectors.size() / dim;
  std::vector<uint8_t> visited(count, 0);
  std::vector<uint32_t> frontier;

  frontier.push_back(entry);
  visited[entry] = 1;

  uint32_t visited_count = 1;
  std::vector<std::pair<float, uint32_t>> candidates;

  while (!frontier.empty() && visited_count < max_visits) {
    // Find the closest unvisited node in the frontier.
    uint32_t best_idx = 0;
    float best_dist = std::numeric_limits<float>::max();
    for (size_t fi = 0; fi < frontier.size(); ++fi) {
      uint32_t nid = frontier[fi];
      float d = L2Squared(query, vectors.data() + nid * dim, dim);
      if (d < best_dist) {
        best_dist = d;
        best_idx = static_cast<uint32_t>(fi);
      }
    }

    uint32_t current = frontier[best_idx];
    frontier.erase(frontier.begin() + best_idx);
    visited[current] = 1;
    ++visited_count;

    candidates.emplace_back(best_dist, current);

    for (uint32_t neighbor : graph[current]) {
      if (neighbor >= count || visited[neighbor]) continue;
      visited[neighbor] = 1;
      frontier.push_back(neighbor);
    }
  }

  // Return the closest beam_width nodes.
  std::sort(candidates.begin(), candidates.end());
  if (candidates.size() > beam_width) {
    candidates.resize(beam_width);
  }

  std::vector<uint32_t> result;
  result.reserve(candidates.size());
  for (const auto& [d, id] : candidates) {
    result.push_back(id);
  }
  return result;
}

// RobustPrune: select diverse neighbors using the alpha-RNG criterion.
std::vector<uint32_t> RobustPrune(const std::vector<float>& vectors,
                                  uint32_t dim,
                                  uint32_t node_id,
                                  const std::vector<uint32_t>& candidates,
                                  uint32_t degree,
                                  float alpha) {
  const float* p_vec = vectors.data() + node_id * dim;

  // Sort candidates by distance to node_id.
  std::vector<std::pair<float, uint32_t>> sorted;
  sorted.reserve(candidates.size());
  for (uint32_t cid : candidates) {
    if (cid == node_id) continue;
    float d = L2Squared(p_vec, vectors.data() + cid * dim, dim);
    sorted.emplace_back(d, cid);
  }
  std::sort(sorted.begin(), sorted.end());

  std::vector<uint32_t> result;
  for (const auto& [dist_v, v] : sorted) {
    bool keep = true;
    const float* v_vec = vectors.data() + v * dim;
    for (uint32_t u : result) {
      float duv = L2Squared(v_vec, vectors.data() + u * dim, dim);
      if (alpha * dist_v > duv) {
        keep = false;
        break;
      }
    }
    if (keep) {
      result.push_back(v);
      if (result.size() >= degree) break;
    }
  }

  return result;
}

}  // namespace

bool BuildVamanaGraph(const std::vector<float>& vectors, uint32_t dim,
                      const VamanaConfig& config,
                      std::vector<std::vector<VectorId>>& out_neighbors) {
  if (dim == 0 || config.degree == 0) return false;
  if (vectors.size() % dim != 0) return false;

  size_t count = vectors.size() / dim;
  uint32_t R = config.degree;

  out_neighbors.assign(count, std::vector<VectorId>());

  // Initialize with a random graph (R random neighbors each).
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint32_t> dist_id(0, static_cast<uint32_t>(count) - 1);
  for (size_t i = 0; i < count; ++i) {
    std::unordered_set<uint32_t> seen;
    seen.insert(static_cast<uint32_t>(i));
    while (out_neighbors[i].size() < R && seen.size() < count) {
      uint32_t cand = dist_id(rng);
      if (seen.insert(cand).second) {
        out_neighbors[i].push_back(cand);
      }
    }
    while (out_neighbors[i].size() < R) {
      out_neighbors[i].push_back(static_cast<uint32_t>(i));
    }
  }

  // Find medoid (node with smallest average distance to a random sample).
  uint32_t medoid = 0;
  {
    std::uniform_int_distribution<uint32_t> sample_dist(0, static_cast<uint32_t>(count) - 1);
    float min_avg = std::numeric_limits<float>::max();
    for (uint32_t trial = 0; trial < std::min<uint32_t>(100, static_cast<uint32_t>(count)); ++trial) {
      uint32_t candidate = sample_dist(rng);
      float sum_dist = 0.0f;
      const float* c_vec = vectors.data() + candidate * dim;
      uint32_t samples = std::min<uint32_t>(100, static_cast<uint32_t>(count));
      for (uint32_t j = 0; j < samples; ++j) {
        uint32_t s = sample_dist(rng);
        sum_dist += L2Squared(c_vec, vectors.data() + s * dim, dim);
      }
      float avg = sum_dist / static_cast<float>(samples);
      if (avg < min_avg) {
        min_avg = avg;
        medoid = candidate;
      }
    }
  }

  // Iterative improvement: for each node, search and prune.
  for (size_t i = 0; i < count; ++i) {
    const float* query = vectors.data() + i * dim;

    // Search for candidates using the current graph.
    auto candidates = GreedySearch(vectors, dim, out_neighbors, medoid,
                                   query, config.beam_width, config.max_visits);

    // RobustPrune to select final neighbors.
    auto pruned = RobustPrune(vectors, dim, static_cast<uint32_t>(i),
                              candidates, R, config.alpha);

    // Update graph: add undirected edges, prune overflowing nodes.
    out_neighbors[i] = pruned;
    for (uint32_t u : pruned) {
      auto& u_neighbors = out_neighbors[u];
      bool has_i = false;
      for (uint32_t n : u_neighbors) {
        if (n == i) { has_i = true; break; }
      }
      if (!has_i) {
        u_neighbors.push_back(static_cast<uint32_t>(i));
        if (u_neighbors.size() > R) {
          // Prune u's neighbors to stay within degree bound.
          u_neighbors = RobustPrune(vectors, dim, u, u_neighbors, R, config.alpha);
        }
      }
    }
  }

  // Ensure all nodes have exactly R neighbors (fill with self-loops).
  for (size_t i = 0; i < count; ++i) {
    while (out_neighbors[i].size() < R) {
      out_neighbors[i].push_back(static_cast<uint32_t>(i));
    }
    // Truncate to R.
    if (out_neighbors[i].size() > R) {
      out_neighbors[i].resize(R);
    }
  }

  return true;
}

}  // namespace vindex
