#pragma once

#include <cstddef>
#include "core/types.h"

namespace vindex {

inline Distance L2Squared(const float* a, const float* b, size_t dim) {
  Distance sum = 0.0f;
  for (size_t i = 0; i < dim; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

}  // namespace vindex
