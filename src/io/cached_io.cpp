#include "io/cached_io.h"

#include <vector>

namespace vindex {

CachedIOBackend::CachedIOBackend(std::unique_ptr<IIOBackend> backend,
                                 CachePool* cache)
    : backend_(std::move(backend)), cache_(cache) {}

bool CachedIOBackend::Open(const std::string& path) {
  return backend_->Open(path);
}

bool CachedIOBackend::IsOpen() const {
  return backend_->IsOpen();
}

bool CachedIOBackend::ReadAt(uint64_t offset, void* buf, size_t size) {
  // Fast path: cache lookup.
  if (cache_) {
    if (cache_->Get(backend_->path(), offset, size, buf)) {
      return true;
    }
  }

  // Cache miss — read from disk.
  if (!backend_->ReadAt(offset, buf, size)) {
    return false;
  }

  // Only backfill if there's room and we're below 90% capacity.
  // Avoids unique_lock contention when cache is nearly full.
  if (cache_ && cache_->HasRoom()) {
    cache_->PutIfRoom(backend_->path(), offset, buf, size);
  }

  return true;
}

bool CachedIOBackend::WriteAt(uint64_t offset, const void* buf, size_t size) {
  if (cache_) {
    cache_->InvalidateSegment(backend_->path());
  }
  return backend_->WriteAt(offset, buf, size);
}

bool CachedIOBackend::Flush() {
  return backend_->Flush();
}

size_t CachedIOBackend::GetFileSize() {
  return backend_->GetFileSize();
}

}  // namespace vindex
