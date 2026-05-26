#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace vindex {

class MemTable {
 public:
  explicit MemTable(uint32_t dim) : dim_(dim) {}
  void Add(const float* vector);
  void Clear();
  bool empty() const { return values_.empty(); }
  uint32_t dim() const { return dim_; }
  size_t count() const { return dim_ > 0 ? values_.size() / dim_ : 0; }
  const std::vector<float>& values() const { return values_; }
  std::vector<float> TakeValues();

  // Brute-force search over buffered vectors. Returns top-k results.
  // The results use local ids (0-based within the MemTable), which the
  // caller must offset to global ids.
  std::vector<SearchResult> Search(const float* query, uint32_t top_k) const;

 private:
  uint32_t dim_ = 0;
  std::vector<float> values_;
};

}  // namespace vindex
