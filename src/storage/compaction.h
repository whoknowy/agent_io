#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace vindex {

class Manifest;

using CompactionCallback = std::function<void(const std::string& msg)>;

// Background compaction scheduler implementing level-based merge policy.
// Level 0: small segments from MemTable flushes (max 4 segments).
// Level 1+: merged segments, each level holds at most 2 segments.
class CompactionScheduler {
 public:
  CompactionScheduler(const std::string& manifest_path,
                      const std::string& data_dir,
                      uint32_t dim);
  ~CompactionScheduler();

  CompactionScheduler(const CompactionScheduler&) = delete;
  CompactionScheduler& operator=(const CompactionScheduler&) = delete;

  // Trigger a compaction check (non-blocking).
  void ScheduleCheck();

  // Set a callback for progress messages.
  void SetCallback(CompactionCallback cb) { callback_ = std::move(cb); }

  // Pause/resume compaction (e.g. during heavy read load).
  void Pause();
  void Resume();

  // Wait for any in-progress compaction to finish.
  void WaitIdle();

  size_t compaction_count() const { return compaction_count_.load(); }

 private:
  void WorkerLoop();
  bool CompactLevel(uint32_t level, Manifest& manifest);

  std::string manifest_path_;
  std::string data_dir_;
  uint32_t dim_;
  uint32_t degree_ = 32;
  uint32_t build_beam_ = 64;
  uint32_t max_build_visits_ = 5000;

  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<size_t> compaction_count_{0};
  std::atomic<bool> pending_{false};

  std::mutex mutex_;
  std::condition_variable cv_;

  std::thread worker_;
  CompactionCallback callback_;
};

}  // namespace vindex
