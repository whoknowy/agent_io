#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/cache_types.h"

namespace vindex {

class CachePool {
 public:
  explicit CachePool(size_t watermark_bytes);
  ~CachePool() = default;

  // Disable copy.
  CachePool(const CachePool&) = delete;
  CachePool& operator=(const CachePool&) = delete;

  // Look up cached data. Returns true and copies data to out on hit.
  bool Get(const std::string& segment_path, uint64_t offset, size_t size,
           void* out);

  // Store data in cache.
  void Put(const std::string& segment_path, uint64_t offset,
           const void* data, size_t size, bool is_metadata);

  // Prefetch: store only if we have room; never trigger eviction.
  void PutIfRoom(const std::string& segment_path, uint64_t offset,
                 const void* data, size_t size);

  // Clear all cached entries for a given segment.
  void InvalidateSegment(const std::string& segment_path);

  const CacheStats& stats() const { return stats_; }
  size_t watermark() const { return watermark_; }

 private:
  struct Key {
    std::string segment_path;
    uint64_t offset;

    bool operator==(const Key& other) const {
      return segment_path == other.segment_path && offset == other.offset;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<std::string>{}(k.segment_path) ^
             std::hash<uint64_t>{}(k.offset);
    }
  };

  uint32_t ComputeWeight(const CacheEntry& entry) const;
  void EvictToWatermark();

  size_t watermark_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<Key, CacheEntry, KeyHash> cache_;
  CacheStats stats_;
};

}  // namespace vindex
