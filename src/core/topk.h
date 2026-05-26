#pragma once

#include <algorithm>
#include <queue>
#include <vector>
#include "core/types.h"

namespace vindex {

class TopK {
 public:
  explicit TopK(size_t k) : k_(k) {}

  void Add(const SearchResult& r) {
    if (k_ == 0) return;

    if (heap_.size() < k_) {
      heap_.push(r);
      return;
    }

    // Only add if better than our worst element.
    if (r.distance < heap_.top().distance) {
      heap_.pop();
      heap_.push(r);
    }
  }

  const std::vector<SearchResult>& results() {
    results_.clear();
    results_.reserve(heap_.size());
    while (!heap_.empty()) {
      results_.push_back(heap_.top());
      heap_.pop();
    }
    std::reverse(results_.begin(), results_.end());
    return results_;
  }

 private:
  struct MaxDistance {
    bool operator()(const SearchResult& a, const SearchResult& b) const {
      return a.distance < b.distance;
    }
  };

  size_t k_ = 0;
  std::priority_queue<SearchResult, std::vector<SearchResult>, MaxDistance> heap_;
  std::vector<SearchResult> results_;
};

}  // namespace vindex
