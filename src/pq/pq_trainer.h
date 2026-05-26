#pragma once

#include <cstddef>
#include <string>

#include "pq/pq.h"

namespace vindex {

class PQTrainer {
 public:
  static bool Train(const float* vectors, size_t count, uint32_t dim,
                    const PQConfig& config, PQCodebook& out_codebook);

  static bool SaveCodebook(const std::string& path, const PQCodebook& codebook);
  static bool LoadCodebook(const std::string& path, PQCodebook& codebook);
};

}  // namespace vindex
