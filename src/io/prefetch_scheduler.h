#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vindex {

// Manages async prefetch of disk data. On Linux with io_uring this uses
// io_uring SQEs; on other platforms it falls back to a background thread pool.
class PrefetchScheduler {
 public:
  explicit PrefetchScheduler(size_t max_in_flight = 32);
  ~PrefetchScheduler();

  PrefetchScheduler(const PrefetchScheduler&) = delete;
  PrefetchScheduler& operator=(const PrefetchScheduler&) = delete;

  // Submit a prefetch request. Returns a request id, or 0 if the queue is full.
  uint64_t SubmitPrefetch(const std::string& path, uint64_t offset,
                          size_t size);

  // Try to retrieve completed prefetch data. Returns true if data was ready.
  bool TryGet(uint64_t request_id, std::vector<uint8_t>& out);

  // Block until the prefetch completes and return data.
  bool WaitGet(uint64_t request_id, std::vector<uint8_t>& out);

  // Non-blocking poll: returns number of completed requests ready to retrieve.
  size_t Poll();

  size_t in_flight() const { return in_flight_.load(); }
  size_t max_in_flight() const { return max_in_flight_; }

 private:
  void WorkerLoop();

  struct Request {
    uint64_t id = 0;
    std::string path;
    uint64_t offset = 0;
    size_t size = 0;
    std::vector<uint8_t> data;
    bool done = false;
    bool ok = false;
  };

  size_t max_in_flight_;
  std::atomic<uint64_t> next_id_{1};
  std::atomic<size_t> in_flight_{0};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Request>> pending_;
  std::unordered_map<uint64_t, Request*> completed_;

  std::atomic<bool> stop_{false};
  std::thread worker_;

  // File handle cache — avoids open/close on every read.
  std::unordered_map<std::string, std::ifstream> files_;
};

}  // namespace vindex
