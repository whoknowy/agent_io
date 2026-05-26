#include "cache/cache_pool.h"

#include <algorithm>
#include <cstring>
<<<<<<< HEAD
#include <mutex>
=======
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10

namespace vindex {

CachePool::CachePool(size_t watermark_bytes) : watermark_(watermark_bytes) {}

bool CachePool::Get(const std::string& segment_path, uint64_t offset,
                    size_t size, void* out) {
  Key key{segment_path, offset};
  std::shared_lock lock(mutex_);

  auto it = cache_.find(key);
  if (it != cache_.end() && it->second.Size() >= size) {
    std::memcpy(out, it->second.data.data(), size);
    it->second.access_count++;
    it->second.access_count = std::min(it->second.access_count, 999u);
    ++stats_.hits;
    return true;
  }

  ++stats_.misses;
  return false;
}

void CachePool::Put(const std::string& segment_path, uint64_t offset,
                    const void* data, size_t size, bool is_metadata) {
  if (size == 0) return;

  Key key{segment_path, offset};
  std::unique_lock lock(mutex_);

  auto it = cache_.find(key);
  if (it != cache_.end()) {
    // Update existing entry.
    it->second.data.assign(reinterpret_cast<const uint8_t*>(data),
                           reinterpret_cast<const uint8_t*>(data) + size);
    return;
  }

  // Evict if adding this entry would exceed watermark.
  size_t current = stats_.current_memory;
  if (current + size > watermark_) {
    EvictToWatermark();
  }

  CacheEntry entry;
  entry.segment_path = segment_path;
  entry.offset = offset;
  entry.data.assign(reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + size);
  entry.is_metadata = is_metadata;

  stats_.current_memory += size;
  stats_.peak_memory = std::max(stats_.peak_memory, stats_.current_memory);
  cache_[key] = std::move(entry);
}

void CachePool::PutIfRoom(const std::string& segment_path, uint64_t offset,
                          const void* data, size_t size) {
  if (size == 0) return;

  std::unique_lock lock(mutex_);

  size_t current = stats_.current_memory;
  if (current + size > watermark_) {
    return;  // No room, don't evict.
  }

  Key key{segment_path, offset};
  if (cache_.find(key) != cache_.end()) {
    return;  // Already cached.
  }

  CacheEntry entry;
  entry.segment_path = segment_path;
  entry.offset = offset;
  entry.data.assign(reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + size);
  entry.is_metadata = false;

  stats_.current_memory += size;
  stats_.peak_memory = std::max(stats_.peak_memory, stats_.current_memory);
  cache_[key] = std::move(entry);
}

void CachePool::InvalidateSegment(const std::string& segment_path) {
  std::unique_lock lock(mutex_);
  auto it = cache_.begin();
  while (it != cache_.end()) {
    if (it->first.segment_path == segment_path) {
      stats_.current_memory -= it->second.Size();
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

uint32_t CachePool::ComputeWeight(const CacheEntry& entry) const {
  uint32_t priority = 1;
  if (entry.is_metadata) {
    priority = 3;
  } else if (entry.access_count >= 5) {
    priority = 2;
  }
  return priority * 1000 + std::min(entry.access_count, 999u);
}

void CachePool::EvictToWatermark() {
  // Collect entries sorted by weight ascending (evict smallest first).
  std::vector<std::pair<Key, uint32_t>> weighted;
  weighted.reserve(cache_.size());
  for (const auto& [k, v] : cache_) {
    weighted.emplace_back(k, ComputeWeight(v));
  }
  std::sort(weighted.begin(), weighted.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

  for (const auto& [k, weight] : weighted) {
    if (stats_.current_memory <= watermark_) break;
    auto it = cache_.find(k);
    if (it != cache_.end()) {
      stats_.current_memory -= it->second.Size();
      cache_.erase(it);
      ++stats_.evictions;
    }
  }
}

}  // namespace vindex
