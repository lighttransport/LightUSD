// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <type_traits>

namespace lusdview {
// Opt-in process-local accounting shared by the viewer and the CUDA adapter.
// The reserve covers driver contexts, modules and unobservable driver scratch.
class PreviewGpuBudget {
 public:
  void configure(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_.store(bytes);
    used_ = bytes ? (bytes / 2 < (uint64_t(512) << 20) ? bytes / 2
                                                       : uint64_t(512) << 20)
                  : 0;
    peak_ = used_;
  }
  bool enabled() const { return limit_.load() != 0; }
  bool reserve(uint64_t bytes) {
    if (!enabled()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t limit = limit_.load();
    if (used_ > limit || bytes > limit - used_) {
      std::fprintf(stderr,
                   "[scene-preview] GPU allocation refused: request=%llu "
                   "used=%llu limit=%llu\n",
                   static_cast<unsigned long long>(bytes),
                   static_cast<unsigned long long>(used_),
                   static_cast<unsigned long long>(limit));
      return false;
    }
    used_ += bytes;
    if (used_ > peak_) peak_ = used_;
    return true;
  }
  void record(uint64_t handle, uint64_t bytes) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    allocations_[handle] = bytes;
  }
  void cancel(uint64_t bytes) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    used_ -= bytes;
  }
  void release(uint64_t handle) {
    if (!enabled() || !handle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = allocations_.find(handle);
    if (it != allocations_.end()) {
      used_ -= it->second;
      allocations_.erase(it);
    }
  }
  void report() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stderr,
                 "[scene-preview] accounted_gpu_peak_mib=%.2f limit_mib=%.2f "
                 "(includes runtime reserve)\n",
                 double(peak_) / (1 << 20), double(limit_.load()) / (1 << 20));
  }

 private:
  std::atomic<uint64_t> limit_{0};
  std::mutex mutex_;
  std::map<uint64_t, uint64_t> allocations_;
  uint64_t used_ = 0, peak_ = 0;
};
inline PreviewGpuBudget& ScenePreviewGpuBudget() {
  static PreviewGpuBudget budget;
  return budget;
}
struct PreviewGpuBudgetReport {
  ~PreviewGpuBudgetReport() { ScenePreviewGpuBudget().report(); }
};
template <typename T>
uint64_t PreviewMemoryHandle(T handle) {
  if constexpr (std::is_pointer<T>::value)
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  else
    return static_cast<uint64_t>(handle);
}
}  // namespace lusdview
