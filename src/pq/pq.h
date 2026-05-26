#pragma once

#include <cstdint>
#include <vector>

namespace vindex {

struct PQConfig {
  uint32_t M = 64;
  uint32_t K = 256;
  int max_iters = 25;
};

struct PQCodebook {
  uint32_t M = 0;
  uint32_t K = 0;
  uint32_t subspace_dim = 0;
  std::vector<float> centroids;

  bool IsValid() const {
    return M > 0 && K > 0 && subspace_dim > 0 &&
           centroids.size() == static_cast<size_t>(M) * K * subspace_dim;
  }
};

}  // namespace vindex
