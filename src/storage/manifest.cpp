#include "storage/manifest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vindex {

namespace {

bool WriteManifest(std::ostream& stream,
                   const std::vector<SegmentMeta>& segments) {
  for (const auto& meta : segments) {
    stream << meta.path << "|" << meta.id_offset << "|" << meta.level << "\n";
    if (!stream) {
      return false;
    }
  }
  stream.flush();
  return static_cast<bool>(stream);
}

bool ReplaceFileAtomically(const std::filesystem::path& source,
                           const std::filesystem::path& target) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return false;
  }
  return true;
#else
  std::error_code ec;
  std::filesystem::rename(source, target, ec);
  return !ec;
#endif
}

}  // namespace

bool Manifest::Load(const std::string& path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return false;
  }

  segments_.clear();
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream iss(line);
    std::string file_path;
    std::string offset_str;
    if (std::getline(iss, file_path, '|') && std::getline(iss, offset_str, '|')) {
      SegmentMeta meta;
      meta.path = file_path;
      meta.id_offset = std::stoull(offset_str);

      // Level is optional for backward compatibility.
      std::string level_str;
      if (std::getline(iss, level_str)) {
        meta.level = static_cast<uint32_t>(std::stoul(level_str));
      }

      segments_.push_back(meta);
    }
  }
  return !segments_.empty();
}

bool Manifest::Save(const std::string& path) const {
  std::filesystem::path target(path);
  std::filesystem::path temp = target;
  temp += ".tmp";

  std::error_code ec;
  std::filesystem::remove(temp, ec);

  std::ofstream stream(temp, std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  if (!WriteManifest(stream, segments_)) {
    stream.close();
    std::filesystem::remove(temp, ec);
    return false;
  }

  stream.close();
  if (!static_cast<bool>(stream)) {
    std::filesystem::remove(temp, ec);
    return false;
  }

  if (!ReplaceFileAtomically(temp, target)) {
    std::filesystem::remove(temp, ec);
    return false;
  }

  return true;
}

void Manifest::Add(const SegmentMeta& meta) {
  segments_.push_back(meta);
}

void Manifest::Remove(const std::string& segment_path) {
  segments_.erase(
      std::remove_if(segments_.begin(), segments_.end(),
                     [&](const SegmentMeta& m) { return m.path == segment_path; }),
      segments_.end());
}

void Manifest::Replace(const std::string& old_path,
                       const SegmentMeta& new_meta) {
  for (auto& seg : segments_) {
    if (seg.path == old_path) {
      seg = new_meta;
      return;
    }
  }
}

}  // namespace vindex
