#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vindex {

struct CacheEntry {
  std::string segment_path;
  uint64_t offset = 0;
  std::vector<uint8_t> data;
  uint32_t access_count = 0;
  bool is_metadata = false;

  size_t Size() const { return data.size(); }
};

struct CacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  size_t current_memory = 0;
  size_t peak_memory = 0;

  double HitRate() const {
    uint64_t total = hits + misses;
    return total > 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
  }
};

}  // namespace vindex
