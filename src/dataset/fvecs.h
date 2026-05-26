#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vindex {

struct FvecsData {
  uint32_t dim = 0;
  std::vector<float> values;

  size_t count() const { return dim > 0 ? values.size() / dim : 0; }
  const float* vector_at(size_t i) const { return values.data() + i * dim; }
};

bool LoadFvecs(const std::string& path, FvecsData& out, size_t max_vectors = 0);

}  // namespace vindex
