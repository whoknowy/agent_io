#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"

namespace vindex {

struct GraphBuildConfig {
  uint32_t degree = 32;
  uint32_t beam_width = 64;   // For Vamana candidate search.
  float alpha = 1.2f;          // Vamana pruning parameter.
  uint32_t max_visits = 5000;  // Max visits for Vamana beam search.
  std::string builder = "auto"; // "brute", "vamana", or "auto".
};

bool BuildKnnGraph(const std::vector<float>& vectors, uint32_t dim,
                   const GraphBuildConfig& cfg,
                   std::vector<std::vector<VectorId>>& out_neighbors);

bool BuildKnnGraphBruteForce(const std::vector<float>& vectors, uint32_t dim,
                             const GraphBuildConfig& cfg,
                             std::vector<std::vector<VectorId>>& out_neighbors);

}  // namespace vindex
