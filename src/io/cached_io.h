#pragma once

#include <memory>

#include "cache/cache_pool.h"
#include "io/io_backend.h"

namespace vindex {

class CachedIOBackend : public IIOBackend {
 public:
  CachedIOBackend(std::unique_ptr<IIOBackend> backend, CachePool* cache);

  bool Open(const std::string& path) override;
  bool IsOpen() const override;
  bool ReadAt(uint64_t offset, void* buf, size_t size) override;
  bool WriteAt(uint64_t offset, const void* buf, size_t size) override;
  bool Flush() override;
  size_t GetFileSize() override;
  const std::string& path() const override { return backend_->path(); }

 private:
  std::unique_ptr<IIOBackend> backend_;
  CachePool* cache_;
};

}  // namespace vindex
