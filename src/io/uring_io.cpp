#include "io/uring_io.h"

#ifdef VINDEX_IO_URING

#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace vindex {

UringIOBackend::UringIOBackend(size_t queue_depth, bool sqpoll)
    : queue_depth_(queue_depth), sqpoll_(sqpoll) {
  ring_ = new io_uring;
  unsigned flags = 0;
  if (sqpoll_) {
    flags |= IORING_SETUP_SQPOLL;
  }
  if (io_uring_queue_init(static_cast<unsigned>(queue_depth_), ring_, flags) == 0) {
    ring_init_ = true;
  }
}

UringIOBackend::~UringIOBackend() {
  if (ring_init_) {
    io_uring_queue_exit(ring_);
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  delete ring_;
}

bool UringIOBackend::Open(const std::string& path) {
  path_ = path;
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  fd_ = open(path_.c_str(), O_RDONLY | O_DIRECT);
  if (fd_ < 0) {
    // Fall back to buffered I/O if O_DIRECT is not supported.
    fd_ = open(path_.c_str(), O_RDONLY);
  }
  return fd_ >= 0;
}

bool UringIOBackend::IsOpen() const {
  return fd_ >= 0;
}

bool UringIOBackend::ReadAt(uint64_t offset, void* buf, size_t size) {
  if (fd_ < 0 || !ring_init_) return false;

  io_uring_sqe* sqe = io_uring_get_sqe(ring_);
  if (!sqe) return false;

  io_uring_prep_read(sqe, fd_, buf, static_cast<unsigned>(size), offset);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(1));

  int submitted = io_uring_submit(ring_);
  if (submitted < 0) return false;

  io_uring_cqe* cqe = nullptr;
  int ret = io_uring_wait_cqe(ring_, &cqe);
  if (ret < 0) return false;

  int res = static_cast<int>(cqe->res);
  io_uring_cqe_seen(ring_, cqe);

  return static_cast<size_t>(res) == size;
}

bool UringIOBackend::WriteAt(uint64_t offset, const void* buf, size_t size) {
  if (fd_ < 0 || !ring_init_) return false;

  // For write support we need O_RDWR. Reopen if necessary.
  int write_fd = open(path_.c_str(), O_WRONLY);
  if (write_fd < 0) return false;

  io_uring_sqe* sqe = io_uring_get_sqe(ring_);
  if (!sqe) {
    close(write_fd);
    return false;
  }

  io_uring_prep_write(sqe, write_fd, buf, static_cast<unsigned>(size), offset);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(2));

  int submitted = io_uring_submit(ring_);
  if (submitted < 0) {
    close(write_fd);
    return false;
  }

  io_uring_cqe* cqe = nullptr;
  int ret = io_uring_wait_cqe(ring_, &cqe);
  if (ret < 0) {
    close(write_fd);
    return false;
  }

  int res = static_cast<int>(cqe->res);
  io_uring_cqe_seen(ring_, cqe);
  close(write_fd);

  return static_cast<size_t>(res) == size;
}

bool UringIOBackend::Flush() {
  // io_uring writes with O_DIRECT are not buffered.
  return true;
}

size_t UringIOBackend::GetFileSize() {
  if (fd_ < 0) return 0;
  struct stat st;
  if (fstat(fd_, &st) != 0) return 0;
  return static_cast<size_t>(st.st_size);
}

uint64_t UringIOBackend::SubmitRead(uint64_t offset, void* buf, size_t size) {
  if (fd_ < 0 || !ring_init_) return 0;

  io_uring_sqe* sqe = io_uring_get_sqe(ring_);
  if (!sqe) return 0;

  static uint64_t user_data_counter = 100;
  uint64_t user_data = user_data_counter++;

  io_uring_prep_read(sqe, fd_, buf, static_cast<unsigned>(size), offset);
  io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(user_data));

  io_uring_submit(ring_);
  return user_data;
}

uint64_t UringIOBackend::WaitCQE() {
  if (!ring_init_) return UINT64_MAX;

  io_uring_cqe* cqe = nullptr;
  int ret = io_uring_wait_cqe(ring_, &cqe);
  if (ret < 0) return UINT64_MAX;

  uint64_t user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
  io_uring_cqe_seen(ring_, cqe);
  return user_data;
}

uint64_t UringIOBackend::PeekCQE() {
  if (!ring_init_) return UINT64_MAX;

  io_uring_cqe* cqe = nullptr;
  int ret = io_uring_peek_cqe(ring_, &cqe);
  if (ret < 0 || !cqe) return UINT64_MAX;

  uint64_t user_data = reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe));
  io_uring_cqe_seen(ring_, cqe);
  return user_data;
}

}  // namespace vindex

#endif  // VINDEX_IO_URING
