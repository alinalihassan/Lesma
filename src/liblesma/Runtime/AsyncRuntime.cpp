#include "liblesma/Runtime/AsyncRuntime.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lesma::runtime {

class AsyncRuntime final {
public:
  static auto instance() -> AsyncRuntime& {
    static AsyncRuntime runtime;
    return runtime;
  }

  auto init(std::uint64_t requestedWorkerCount) -> void {
    std::lock_guard<std::mutex> lock(mutex);
    if (requestedWorkerCount > 0U) {
      configuredWorkerCount = resolveWorkerCount(requestedWorkerCount);
    }
    initDepth++;
    startWorkersLocked();
  }

  auto shutdown() -> void {
    std::vector<std::thread> threadsToJoin;
    std::vector<std::pair<void*, LesmaAsyncDestroyFn>> tasksToDestroy;
    {
      std::unique_lock<std::mutex> lock(mutex);
      if (initDepth > 0U) {
        initDepth--;
      }
      if (initDepth > 0U) {
        return;
      }
      if (!tasks.empty()) {
        startWorkersLocked();
        for (auto& [taskHandle, record] : tasks) {
          enqueueTaskLocked(taskHandle, record);
        }
        cv.wait(lock, [this]() { return allTasksCompletedLocked(); });
      }
      if (workers.empty()) {
        return;
      }
      stopping = true;
      threadsToJoin.swap(workers);
      cv.notify_all();
      for (const auto& [taskHandle, record] : tasks) {
        tasksToDestroy.emplace_back(taskHandle, record.destroyFn);
      }
    }
    for (auto& worker : threadsToJoin) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    for (const auto& [taskHandle, destroyFn] : tasksToDestroy) {
      if (destroyFn != nullptr) {
        destroyFn(taskHandle);
      }
    }
    std::lock_guard<std::mutex> lock(mutex);
    tasks.clear();
    runnableTasks.clear();
    stopping = false;
  }

  auto registerTask(void* taskHandle, LesmaAsyncResumeFn resumeFn, LesmaAsyncDoneFn doneFn,
                    LesmaAsyncDestroyFn destroyFn) -> void {
    if (taskHandle == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    startWorkersLocked();
    TaskRecord& record = tasks[taskHandle];
    record.resumeFn = resumeFn;
    record.doneFn = doneFn;
    record.destroyFn = destroyFn;
    record.started = false;
    record.running = false;
    record.completed = false;
    record.queued = false;
  }

  auto startTask(void* taskHandle) -> void {
    if (taskHandle == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    startWorkersLocked();
    auto it = tasks.find(taskHandle);
    if (it == tasks.end()) {
      return;
    }
    enqueueTaskLocked(taskHandle, it->second);
  }

  auto waitTask(void* taskHandle) -> void {
    if (taskHandle == nullptr) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    startWorkersLocked();
    auto it = tasks.find(taskHandle);
    if (it == tasks.end()) {
      return;
    }
    enqueueTaskLocked(taskHandle, it->second);
    if (!isWorkerThread()) {
      cv.wait(lock, [&]() {
        auto it = tasks.find(taskHandle);
        return it == tasks.end() || it->second.completed;
      });
      return;
    }
    // Worker threads must not park here: nested awaits would otherwise consume the pool and leave
    // no thread available to resume the awaited tasks. Instead, keep draining queued work until the
    // awaited task completes.
    while (true) {
      auto awaitedIt = tasks.find(taskHandle);
      if (awaitedIt == tasks.end() || awaitedIt->second.completed) {
        return;
      }

      void* runnableTaskHandle = nullptr;
      LesmaAsyncResumeFn resumeFn = nullptr;
      LesmaAsyncDoneFn doneFn = nullptr;
      if (!claimRunnableTaskLocked(runnableTaskHandle, resumeFn, doneFn)) {
        cv.wait(lock, [&]() {
          auto it = tasks.find(taskHandle);
          return it == tasks.end() || it->second.completed || !runnableTasks.empty();
        });
        continue;
      }

      lock.unlock();
      bool const completed = runClaimedTask(runnableTaskHandle, resumeFn, doneFn);
      lock.lock();
      markTaskFinishedLocked(runnableTaskHandle, completed);
      cv.notify_all();
    }
  }

  auto releaseTask(void* taskHandle) -> void {
    if (taskHandle == nullptr) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() {
      auto it = tasks.find(taskHandle);
      return it == tasks.end() || (!it->second.running && !it->second.queued);
    });
    tasks.erase(taskHandle);
  }

private:
  struct TaskRecord {
    LesmaAsyncResumeFn resumeFn = nullptr;
    LesmaAsyncDoneFn doneFn = nullptr;
    LesmaAsyncDestroyFn destroyFn = nullptr;
    bool started = false;
    bool running = false;
    bool completed = false;
    bool queued = false;
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<void*, TaskRecord> tasks;
  std::deque<void*> runnableTasks;
  std::vector<std::thread> workers;
  size_t configuredWorkerCount = 0U;
  size_t initDepth = 0U;
  bool stopping = false;

  AsyncRuntime() = default;
  ~AsyncRuntime() { forceShutdown(); }

  static auto workerThreadFlag() -> bool& {
    static thread_local bool isWorker = false;
    return isWorker;
  }

  static auto isWorkerThread() -> bool { return workerThreadFlag(); }

  static auto resolveWorkerCount(std::uint64_t requestedWorkerCount) -> size_t {
    if (requestedWorkerCount > 0U) {
      return static_cast<size_t>(requestedWorkerCount);
    }
    unsigned const detected = std::thread::hardware_concurrency();
    size_t const fallbackCount = detected == 0U ? 4U : static_cast<size_t>(detected) * 2U;
    return std::max<size_t>(4U, fallbackCount);
  }

  auto startWorkersLocked() -> void {
    if (!workers.empty()) {
      return;
    }
    stopping = false;
    size_t const workerCount =
        configuredWorkerCount == 0U ? resolveWorkerCount(0) : configuredWorkerCount;
    workers.reserve(workerCount);
    for (size_t index = 0; index < workerCount; ++index) {
      workers.emplace_back([this]() { workerLoop(); });
    }
  }

  auto enqueueTaskLocked(void* taskHandle, TaskRecord& record) -> void {
    if (record.completed || record.running || record.queued || record.resumeFn == nullptr) {
      return;
    }
    record.started = true;
    record.queued = true;
    runnableTasks.push_back(taskHandle);
    cv.notify_one();
  }

  auto claimRunnableTaskLocked(void*& taskHandle, LesmaAsyncResumeFn& resumeFn,
                               LesmaAsyncDoneFn& doneFn) -> bool {
    while (!runnableTasks.empty()) {
      taskHandle = runnableTasks.front();
      runnableTasks.pop_front();
      auto it = tasks.find(taskHandle);
      if (it == tasks.end()) {
        continue;
      }
      it->second.queued = false;
      if (it->second.completed || it->second.running) {
        continue;
      }
      it->second.running = true;
      resumeFn = it->second.resumeFn;
      doneFn = it->second.doneFn;
      return true;
    }
    return false;
  }

  auto markTaskFinishedLocked(void* taskHandle, bool completed) -> void {
    auto it = tasks.find(taskHandle);
    if (it == tasks.end()) {
      return;
    }
    it->second.running = false;
    it->second.completed = completed;
  }

  auto allTasksCompletedLocked() const -> bool {
    return std::ranges::all_of(tasks, [](const auto& entry) { return entry.second.completed; });
  }

  auto forceShutdown() -> void {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (initDepth == 0U && workers.empty()) {
        return;
      }
      initDepth = 1U;
    }
    shutdown();
  }

  static auto runClaimedTask(void* taskHandle, LesmaAsyncResumeFn resumeFn, LesmaAsyncDoneFn doneFn)
      -> bool {
    if (resumeFn != nullptr) {
      resumeFn(taskHandle);
    }
    return doneFn == nullptr || doneFn(taskHandle);
  }

  auto workerLoop() -> void {
    workerThreadFlag() = true;
    while (true) {
      void* taskHandle = nullptr;
      LesmaAsyncResumeFn resumeFn = nullptr;
      LesmaAsyncDoneFn doneFn = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return stopping || !runnableTasks.empty(); });
        if (stopping && runnableTasks.empty()) {
          return;
        }
        if (!claimRunnableTaskLocked(taskHandle, resumeFn, doneFn)) {
          continue;
        }
      }

      bool const completed = runClaimedTask(taskHandle, resumeFn, doneFn);

      {
        std::lock_guard<std::mutex> lock(mutex);
        markTaskFinishedLocked(taskHandle, completed);
      }
      cv.notify_all();
    }
  }
};

} // namespace lesma::runtime

extern "C" {

void lesma_async_runtime_init(std::uint64_t workerCount) {
  try {
    lesma::runtime::AsyncRuntime::instance().init(workerCount);
  } catch (...) {
    std::abort();
  }
}

void lesma_async_runtime_shutdown() {
  try {
    lesma::runtime::AsyncRuntime::instance().shutdown();
  } catch (...) {
    std::abort();
  }
}

void lesma_async_runtime_register_task(void* taskHandle, LesmaAsyncResumeFn resumeFn,
                                       LesmaAsyncDoneFn doneFn, LesmaAsyncDestroyFn destroyFn) {
  try {
    lesma::runtime::AsyncRuntime::instance().registerTask(taskHandle, resumeFn, doneFn, destroyFn);
  } catch (...) {
    std::abort();
  }
}

void lesma_async_runtime_start_task(void* taskHandle) {
  try {
    lesma::runtime::AsyncRuntime::instance().startTask(taskHandle);
  } catch (...) {
    std::abort();
  }
}

void lesma_async_runtime_wait_task(void* taskHandle) {
  try {
    lesma::runtime::AsyncRuntime::instance().waitTask(taskHandle);
  } catch (...) {
    std::abort();
  }
}

void lesma_async_runtime_release_task(void* taskHandle) {
  try {
    lesma::runtime::AsyncRuntime::instance().releaseTask(taskHandle);
  } catch (...) {
    std::abort();
  }
}

} // extern "C"
