#pragma once

#include <vector>

#include "core/types.h"
#include "storage/segment.h"

namespace vindex {

class PrefetchScheduler;

struct SearchStats {
  uint32_t visited = 0;
  uint32_t pq_distances = 0;
  uint32_t exact_reads = 0;
  uint32_t prefetch_hits = 0;
};

class GraphSearcher {
 public:
  explicit GraphSearcher(const SegmentReader& segment) : segment_(segment) {}

  void SetPrefetchScheduler(PrefetchScheduler* prefetch) {
    prefetch_ = prefetch;
  }

  std::vector<SearchResult> Search(const float* query, uint32_t dim,
                                   const SearchParams& params,
                                   SearchStats* stats = nullptr) const;

 private:
  const SegmentReader& segment_;
  PrefetchScheduler* prefetch_ = nullptr;
};

}  // namespace vindex
