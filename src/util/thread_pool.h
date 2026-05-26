#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace vindex {

class ThreadPool {
 public:
  explicit ThreadPool(size_t num_threads = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Submit a task and get a future for the result.
  template <typename F, typename... Args>
  auto Submit(F&& f, Args&&... args)
      -> std::future<typename std::invoke_result<F, Args...>::type>;

  size_t num_threads() const { return workers_.size(); }

 private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
};

template <typename F, typename... Args>
auto ThreadPool::Submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
  using ReturnType = typename std::invoke_result<F, Args...>::type;

  auto task = std::make_shared<std::packaged_task<ReturnType()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<ReturnType> result = task->get_future();
  {
    std::lock_guard lock(mutex_);
    tasks_.emplace([task]() { (*task)(); });
  }
  cv_.notify_one();
  return result;
}

}  // namespace vindex
