#include "util/thread_pool.h"

namespace vindex {

ThreadPool::ThreadPool(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
  }
  workers_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back(&ThreadPool::WorkerLoop, this);
  }
}

ThreadPool::~ThreadPool() {
  stop_.store(true);
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void ThreadPool::WorkerLoop() {
  while (!stop_.load()) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return !tasks_.empty() || stop_.load();
      });
      if (stop_.load() && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace vindex
