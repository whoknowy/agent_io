#pragma once

#include <fstream>
#include <string>

#include "io/io_backend.h"

namespace vindex {

class SyncIOBackend : public IIOBackend {
 public:
  SyncIOBackend() = default;
  explicit SyncIOBackend(const std::string& path, bool writable = false);

  bool Open(const std::string& path) override;
  bool IsOpen() const override;
  bool ReadAt(uint64_t offset, void* buf, size_t size) override;
  bool WriteAt(uint64_t offset, const void* buf, size_t size) override;
  bool Flush() override;
  size_t GetFileSize() override;
  const std::string& path() const override { return path_; }

 private:
  std::string path_;
  bool writable_ = false;
  std::fstream stream_;
};

}  // namespace vindex
