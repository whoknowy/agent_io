#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vindex {

struct IvecsData {
  uint32_t dim = 0;
  std::vector<int32_t> values;

  size_t count() const { return dim > 0 ? values.size() / dim : 0; }
  const int32_t* vector_at(size_t i) const { return values.data() + i * dim; }
};

bool LoadIvecs(const std::string& path, IvecsData& out, size_t max_vectors = 0);

}  // namespace vindex
