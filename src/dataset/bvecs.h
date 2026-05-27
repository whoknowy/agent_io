#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vindex {

struct BvecsData {
  uint32_t dim = 0;
  std::vector<float> values;  // uint8 values converted to float

  size_t count() const { return dim > 0 ? values.size() / dim : 0; }
  const float* vector_at(size_t i) const { return values.data() + i * dim; }
};

bool LoadBvecs(const std::string& path, BvecsData& out, size_t max_vectors = 0);

}  // namespace vindex
