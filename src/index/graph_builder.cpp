#include "index/graph_builder.h"

#include <algorithm>
#include <iostream>
#include <vector>

#include "core/distance.h"
#include "index/vamana_builder.h"

namespace vindex {

bool BuildKnnGraphBruteForce(const std::vector<float>& vectors, uint32_t dim,
                             const GraphBuildConfig& cfg,
                             std::vector<std::vector<VectorId>>& out_neighbors) {
  if (dim == 0 || cfg.degree == 0) {
    return false;
  }
  if (vectors.size() % dim != 0) {
    return false;
  }

  size_t count = vectors.size() / dim;
  out_neighbors.assign(count, std::vector<VectorId>());

  for (size_t i = 0; i < count; ++i) {
    std::vector<std::pair<Distance, VectorId>> dist;
    dist.reserve(count > 0 ? count - 1 : 0);
    const float* vi = vectors.data() + i * dim;
    for (size_t j = 0; j < count; ++j) {
      if (i == j) {
        continue;
      }
      const float* vj = vectors.data() + j * dim;
      Distance d = L2Squared(vi, vj, dim);
      dist.emplace_back(d, static_cast<VectorId>(j));
    }

    size_t degree = std::min(static_cast<size_t>(cfg.degree), dist.size());
    if (degree > 0 && dist.size() > degree) {
      auto mid = dist.begin() + static_cast<std::ptrdiff_t>(degree);
      std::nth_element(dist.begin(), mid, dist.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      dist.resize(degree);
    }

    std::sort(dist.begin(), dist.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    out_neighbors[i].reserve(cfg.degree);
    for (const auto& item : dist) {
      out_neighbors[i].push_back(item.second);
    }
    while (out_neighbors[i].size() < cfg.degree) {
      out_neighbors[i].push_back(static_cast<VectorId>(i));
    }
  }

  return true;
}

bool BuildKnnGraph(const std::vector<float>& vectors, uint32_t dim,
                   const GraphBuildConfig& cfg,
                   std::vector<std::vector<VectorId>>& out_neighbors) {
  if (dim == 0 || cfg.degree == 0) return false;
  if (vectors.size() % dim != 0) return false;

  size_t count = vectors.size() / dim;

  // Determine which builder to use.
  bool use_vamana = false;
  if (cfg.builder == "vamana") {
    use_vamana = true;
  } else if (cfg.builder == "brute") {
    use_vamana = false;
  } else {
    // "auto": Vamana for datasets > 10K, brute force for smaller.
    use_vamana = (count > 10000);
  }

  if (use_vamana) {
    std::cout << "Using Vamana builder for " << count << " vectors\n";
    VamanaConfig vcfg;
    vcfg.degree = cfg.degree;
    vcfg.beam_width = cfg.beam_width;
    vcfg.alpha = cfg.alpha;
    vcfg.max_visits = cfg.max_visits;
    return BuildVamanaGraph(vectors, dim, vcfg, out_neighbors);
  } else {
    std::cout << "Using brute-force builder for " << count << " vectors\n";
    return BuildKnnGraphBruteForce(vectors, dim, cfg, out_neighbors);
  }
}

}  // namespace vindex
