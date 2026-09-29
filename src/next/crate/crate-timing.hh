// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - opt-in phase timing for the crate reader/writer.

#pragma once

#include <chrono>

#include "../../logger.hh"

namespace lightusd {
namespace next {

// Logs "[tag] phase=<ms>ms" at INFO for each lap() when enabled (the caller's
// enable_timing option); a disabled timer costs one branch per phase.
class CratePhaseTimer {
 public:
  CratePhaseTimer(bool enabled, const char* tag)
      : enabled_(enabled), tag_(tag), last_(std::chrono::steady_clock::now()) {}

  void lap(const char* phase) {
    if (!enabled_) return;
    const auto now = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;
    LIGHTUSD_LOG_I("[" << tag_ << "] " << phase << "=" << ms << "ms");
  }

 private:
  bool enabled_;
  const char* tag_;
  std::chrono::steady_clock::time_point last_;
};

}  // namespace next
}  // namespace lightusd
