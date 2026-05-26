#include "mem/memtable.h"

#include <algorithm>

#include "core/distance.h"
#include "core/topk.h"

namespace vindex {

void MemTable::Add(const float* vector) {
  if (dim_ == 0) {
    return;
  }
  values_.insert(values_.end(), vector, vector + dim_);
}

void MemTable::Clear() {
  values_.clear();
}

std::vector<float> MemTable::TakeValues() {
  std::vector<float> values;
  values.swap(values_);
  return values;
}

std::vector<SearchResult> MemTable::Search(const float* query,
                                           uint32_t top_k) const {
  TopK topk(top_k);
  size_t n = count();
  for (size_t i = 0; i < n; ++i) {
    float dist = L2Squared(query, values_.data() + i * dim_, dim_);
    topk.Add({static_cast<VectorId>(i), dist});
  }
  return topk.results();
}

}  // namespace vindex

