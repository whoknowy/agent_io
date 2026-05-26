#include "io/prefetch_scheduler.h"

#include <fstream>

namespace vindex {

PrefetchScheduler::PrefetchScheduler(size_t max_in_flight)
    : max_in_flight_(max_in_flight),
      worker_(&PrefetchScheduler::WorkerLoop, this) {}

PrefetchScheduler::~PrefetchScheduler() {
  stop_.store(true);
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

uint64_t PrefetchScheduler::SubmitPrefetch(const std::string& path,
                                           uint64_t offset, size_t size) {
  if (in_flight_.load() >= max_in_flight_) {
    return 0;  // Queue full, best-effort.
  }

  auto req = std::make_unique<Request>();
  req->id = next_id_.fetch_add(1);
  req->path = path;
  req->offset = offset;
  req->size = size;

  uint64_t rid = req->id;

  {
    std::lock_guard lock(mutex_);
    pending_.push_back(std::move(req));
    in_flight_.store(pending_.size());
  }
  cv_.notify_one();
  return rid;
}

bool PrefetchScheduler::TryGet(uint64_t request_id,
                               std::vector<uint8_t>& out) {
  std::lock_guard lock(mutex_);
  auto it = completed_.find(request_id);
  if (it == completed_.end()) {
    return false;
  }
  out = std::move(it->second->data);
  completed_.erase(it);
  return true;
}

bool PrefetchScheduler::WaitGet(uint64_t request_id,
                                std::vector<uint8_t>& out) {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] {
    return completed_.find(request_id) != completed_.end() || stop_.load();
  });
  auto it = completed_.find(request_id);
  if (it == completed_.end()) {
    return false;
  }
  out = std::move(it->second->data);
  completed_.erase(it);
  return true;
}

size_t PrefetchScheduler::Poll() {
  std::lock_guard lock(mutex_);
  return completed_.size();
}

void PrefetchScheduler::WorkerLoop() {
  while (!stop_.load()) {
    std::unique_ptr<Request> req;

    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return !pending_.empty() || stop_.load();
      });
      if (stop_.load() && pending_.empty()) {
        break;
      }
      if (!pending_.empty()) {
        req = std::move(pending_.front());
        pending_.erase(pending_.begin());
        in_flight_.store(pending_.size());
      }
    }

    if (!req) continue;

    // Perform the actual I/O.
    std::ifstream stream(req->path, std::ios::binary);
    if (stream.is_open()) {
      req->data.resize(req->size);
      stream.seekg(static_cast<std::streamoff>(req->offset), std::ios::beg);
      stream.read(reinterpret_cast<char*>(req->data.data()),
                  static_cast<std::streamsize>(req->size));
      req->ok = static_cast<size_t>(stream.gcount()) == req->size;
    }

    uint64_t rid = req->id;
    {
      std::lock_guard lock(mutex_);
      completed_[rid] = req.release();
    }
    cv_.notify_all();
  }
}

}  // namespace vindex
