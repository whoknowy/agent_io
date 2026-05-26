#pragma once

#include <cstdint>
#include <string>

namespace vindex {

class IIOBackend {
 public:
  virtual ~IIOBackend() = default;
  virtual bool Open(const std::string& path) = 0;
  virtual bool IsOpen() const = 0;
  virtual bool ReadAt(uint64_t offset, void* buf, size_t size) = 0;
  virtual bool WriteAt(uint64_t offset, const void* buf, size_t size) = 0;
  virtual bool Flush() = 0;
  virtual size_t GetFileSize() = 0;
  virtual const std::string& path() const = 0;
};

}  // namespace vindex
