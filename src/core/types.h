#pragma once

#include <cstdint>

namespace vindex {

using VectorId = uint32_t;
using Distance = float;

struct SearchParams {
  uint32_t top_k = 10;
  uint32_t beam_width = 32;
  uint32_t max_visits = 10000;
};

struct SearchResult {
  VectorId id = 0;
  Distance distance = 0.0f;
};

}  // namespace vindex
