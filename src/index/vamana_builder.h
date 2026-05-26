#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "index/graph_builder.h"

namespace vindex {

struct VamanaConfig {
  uint32_t degree = 32;       // Max out-degree (R).
  uint32_t beam_width = 64;   // Beam width for candidate search during build.
  float alpha = 1.2f;         // Pruning parameter (larger = sparser graph).
  uint32_t max_visits = 5000; // Max nodes visited per beam search during build.
};

// Build a Vamana/DiskANN-style approximate KNN graph.
// This is O(N log N * dim) instead of the O(N^2 * dim) brute-force approach.
bool BuildVamanaGraph(const std::vector<float>& vectors, uint32_t dim,
                      const VamanaConfig& config,
                      std::vector<std::vector<VectorId>>& out_neighbors);

}  // namespace vindex
