#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vindex {

struct SegmentMeta {
  std::string path;
  uint64_t id_offset = 0;
  uint32_t level = 0;  // LSM level: 0 = MemTable flush, 1+ = compacted.
};

class Manifest {
 public:
  bool Load(const std::string& path);
  bool Save(const std::string& path) const;

  void Add(const SegmentMeta& meta);
  void Remove(const std::string& segment_path);
  void Replace(const std::string& old_path, const SegmentMeta& new_meta);

  const std::vector<SegmentMeta>& segments() const { return segments_; }
  std::vector<SegmentMeta>& mutable_segments() { return segments_; }

 private:
  std::vector<SegmentMeta> segments_;
};

}  // namespace vindex
