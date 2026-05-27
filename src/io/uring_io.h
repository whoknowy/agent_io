#pragma once

#ifdef VINDEX_IO_URING

#include <cstdint>
#include <string>
#include <vector>

#include "io/io_backend.h"

// Forward declaration of liburing's io_uring.
struct io_uring;

namespace vindex {

// io_uring-based I/O backend for Linux 5.1+ with liburing.
// When writable=true, the file is opened with O_RDWR | O_CREAT.
class UringIOBackend : public IIOBackend {
 public:
  explicit UringIOBackend(size_t queue_depth = 64, bool sqpoll = false,
                          bool writable = false);
  ~UringIOBackend() override;

  UringIOBackend(const UringIOBackend&) = delete;
  UringIOBackend& operator=(const UringIOBackend&) = delete;

  bool Open(const std::string& path) override;
  bool IsOpen() const override;
  bool ReadAt(uint64_t offset, void* buf, size_t size) override;
  bool WriteAt(uint64_t offset, const void* buf, size_t size) override;
  bool Flush() override;
  size_t GetFileSize() override;
  const std::string& path() const override { return path_; }

  // Async submission: returns user_data for later retrieval via WaitCQE.
  uint64_t SubmitRead(uint64_t offset, void* buf, size_t size);
  uint64_t WaitCQE();
  uint64_t PeekCQE();

  int fd() const { return fd_; }

 private:
  std::string path_;
  int fd_ = -1;
  size_t queue_depth_;
  bool sqpoll_;
  bool writable_;
  bool ring_init_ = false;
  io_uring* ring_ = nullptr;
};

}  // namespace vindex

#endif  // VINDEX_IO_URING
