#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace vindex {

class FileReader {
 public:
  FileReader() = default;
  explicit FileReader(const std::string& path);
  bool Open(const std::string& path);
  bool IsOpen() const;
  bool ReadAt(uint64_t offset, void* data, size_t size);
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::ifstream stream_;
};

class FileWriter {
 public:
  explicit FileWriter(const std::string& path);
  bool IsOpen() const;
  bool WriteAt(uint64_t offset, const void* data, size_t size);
  bool Flush();
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::fstream stream_;
};

bool ReadFileAll(const std::string& path, std::vector<uint8_t>& out);
bool WriteFileAll(const std::string& path, const std::vector<uint8_t>& data);

}  // namespace vindex
