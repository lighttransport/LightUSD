// ld3d12view - asynchronous scene loading.
//
// lmv_load() (compose + convert + texture decode) runs on a worker thread so the
// window keeps rendering and the HUD can show progress. The lmv progress
// callback doubles as the cancellation point. Everything that touches D3D12 or
// the previously open scene stays on the main thread: the caller polls for a
// finished scene and uploads it itself.
//
// A single persistent worker thread serves all loads (it is never torn down
// between loads: the core keeps thread-local state that a scene may touch again
// later from other threads, so the loading thread must outlive the scene).
// One load runs at a time. Requesting another while one is running cancels the
// running one and starts the new request as soon as the worker has stopped.
#pragma once

#include "lmv.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ld3d12 {

struct LoadProgress {
  bool active = false;
  std::string path;
  int phase = LMV_PHASE_IDLE;  // lmv_phase
  float fraction = 0.0f;       // 0..1 within the phase, when known
  float overall = 0.0f;        // 0..1 across the four loading phases
  std::string message;
  double elapsed = 0.0;        // seconds since the load started
  bool cancelling = false;
};

struct LoadResult {
  std::string path;
  lmv_scene* scene = nullptr;  // owned by the caller on success
  lmv_status status = LMV_OK;
  std::string error;
  bool cancelled = false;
};

class AsyncLoader {
 public:
  AsyncLoader() = default;
  ~AsyncLoader();
  AsyncLoader(const AsyncLoader&) = delete;
  AsyncLoader& operator=(const AsyncLoader&) = delete;

  // Main thread only.
  void Request(const std::string& path);
  // Cancels the running load and drops any queued request.
  void Cancel();
  // A load is running (or finishing) and its result has not been polled yet.
  bool Busy() const { return state_ != nullptr; }
  // Returns true once per finished load; `out` then owns the scene (if any).
  // Cancelled loads are reported with cancelled = true and no scene.
  bool Poll(LoadResult* out);
  LoadProgress Progress() const;

  // Test hook: delay (ms) inserted before each load starts, so a load stays in
  // flight long enough to observe progress and cancellation.
  void set_test_delay_ms(int ms) { delay_ms_ = ms; }

 private:
  struct State {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    mutable std::mutex mutex;
    LoadProgress progress;
    LoadResult result;
    std::chrono::steady_clock::time_point start;
  };

  // The scene keeps the load options (progress callback + user pointer) and
  // lmv_scene_set_time() calls the callback again later, long after the load's
  // State is gone. The callback therefore gets a Hook that lives as long as the
  // loader and turns inert once its load has finished.
  struct Hook {
    std::atomic<bool> active{true};
    State* state = nullptr;
  };

  struct Job {
    State* state = nullptr;
    Hook* hook = nullptr;
    std::string path;
    int delay_ms = 0;
  };

  void Launch(const std::string& path);
  void Worker();
  static void Run(State* state, Hook* hook, const std::string& path,
                  int delay_ms);

  std::unique_ptr<State> state_;
  std::vector<std::unique_ptr<Hook>> hooks_;  // never shrinks; tiny
  std::thread thread_;  // persistent worker
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  Job job_;
  bool has_job_ = false;
  bool stop_ = false;
  std::string pending_;
  int delay_ms_ = 0;
};

}  // namespace ld3d12
