#include "io/sync_io.h"

namespace vindex {

SyncIOBackend::SyncIOBackend(const std::string& path, bool writable)
    : path_(path), writable_(writable) {
  Open(path);
}

bool SyncIOBackend::Open(const std::string& path) {
  path_ = path;
  if (stream_.is_open()) {
    stream_.close();
  }
  auto mode = std::ios::binary | std::ios::in;
  if (writable_) {
    mode |= std::ios::out | std::ios::trunc;
  }
  stream_.open(path_, mode);
  return stream_.is_open();
}

bool SyncIOBackend::IsOpen() const {
  return stream_.is_open();
}

bool SyncIOBackend::ReadAt(uint64_t offset, void* buf, size_t size) {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    return false;
  }
  stream_.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(size));
  return static_cast<size_t>(stream_.gcount()) == size;
}

bool SyncIOBackend::WriteAt(uint64_t offset, const void* buf, size_t size) {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    return false;
  }
  stream_.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(size));
  return static_cast<bool>(stream_);
}

bool SyncIOBackend::Flush() {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.flush();
  return static_cast<bool>(stream_);
}

size_t SyncIOBackend::GetFileSize() {
  if (!stream_.is_open()) {
    return 0;
  }
  auto pos = stream_.tellg();
  stream_.seekg(0, std::ios::end);
  auto size = stream_.tellg();
  stream_.seekg(pos, std::ios::beg);
  return static_cast<size_t>(size);
}

}  // namespace vindex
