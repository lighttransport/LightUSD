// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Parser execution time profiling infrastructure

#pragma once

#include <chrono>
#include <map>
#include <mutex>

#include "tsa-mutex.hh"
#include <string>
#include <vector>

namespace lightusd {

///
/// High-resolution timing utilities for parser profiling
///
class ParserTimer {
 public:
  using Clock = std::chrono::high_resolution_clock;
  using TimePoint = Clock::time_point;
  using Duration = std::chrono::nanoseconds;

  ///
  /// Start timing a named operation
  ///
  void StartTimer(const std::string& operation_name);

  ///
  /// End timing for the named operation
  /// Returns elapsed time in nanoseconds
  ///
  Duration EndTimer(const std::string& operation_name);

  ///
  /// Record a timing measurement directly
  ///
  void RecordTiming(const std::string& operation_name, Duration elapsed);

  ///
  /// Get total elapsed time for an operation (sum of all measurements)
  ///
  Duration GetTotalTime(const std::string& operation_name) const;

  ///
  /// Get number of times an operation was measured
  ///
  size_t GetOperationCount(const std::string& operation_name) const;

  ///
  /// Get average time per operation
  ///
  Duration GetAverageTime(const std::string& operation_name) const;

  ///
  /// Get all recorded operation names
  ///
  std::vector<std::string> GetOperationNames() const;

  ///
  /// Clear all timing data
  ///
  void Clear();

  ///
  /// Generate human-readable timing report
  ///
  std::string GenerateReport() const;

 private:
  struct TimingData {
    Duration total_time{0};
    size_t count{0};
  };

  std::map<std::string, TimePoint> active_timers_;
  std::map<std::string, TimingData> timing_data_;
};

///
/// RAII timer for automatic scope-based timing
///
class ScopedTimer {
 public:
  ScopedTimer(ParserTimer* timer, const std::string& operation_name);
  ~ScopedTimer();

  // Non-copyable
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;

 private:
  ParserTimer* timer_;
  std::string operation_name_;
};

///
/// Profiling configuration for parsers
///
struct ParserProfilingConfig {
  bool enable_profiling{false};           ///< Enable/disable profiling
  bool profile_parsing_phases{true};      ///< Profile major parsing phases
  bool profile_property_parsing{false};   ///< Profile individual property parsing
  bool profile_value_parsing{false};      ///< Profile value type parsing
  bool profile_memory_operations{false};  ///< Profile memory allocations
};

///
/// Centralized profiling interface for all parsers
///
class ParserProfiler {
 public:
  static ParserProfiler& GetInstance();

  ///
  /// Configure profiling settings
  ///
  void SetConfig(const ParserProfilingConfig& config);
  const ParserProfilingConfig& GetConfig() const;

  ///
  /// Get timer instance for a parser
  ///
  ParserTimer* GetTimer(const std::string& parser_name) LIGHTUSD_REQUIRES_NOT(mu_);

  ///
  /// Generate comprehensive profiling report for all parsers
  ///
  std::string GenerateReport() const LIGHTUSD_REQUIRES_NOT(mu_);

  ///
  /// Clear all profiling data
  ///
  void ClearAll() LIGHTUSD_REQUIRES_NOT(mu_);

 private:
  ParserProfiler() = default;

  ParserProfilingConfig config_;
  std::map<std::string, ParserTimer> timers_;
  // Guards timers_. GetTimer() returns a ParserTimer* into this map; std::map
  // does not invalidate element pointers on insert, so the pointer stays valid
  // after the lock releases. (Set profiling config before spawning threads;
  // per-ParserTimer counters are approximate under concurrent profiling.)
  mutable Mutex mu_;
};

// Helper macro for variable name concatenation
#define LIGHTUSD_CONCAT_IMPL(x, y) x##y
#define LIGHTUSD_CONCAT(x, y) LIGHTUSD_CONCAT_IMPL(x, y)

// Convenience macros for profiling.
//
// NOTE: these gate on enable_profiling (default false) and pass a null timer
// when disabled. ScopedTimer no-ops on a null timer, so a default build never
// touches the shared ParserProfiler::timers_ map — important because parsers run
// on worker threads (pcp parallel build) and from concurrent LoadUSDFromFile
// calls. When enabled, GetTimer() is internally mutex-guarded.
#define LIGHTUSD_PROFILE_FUNCTION(parser_name) \
  lightusd::ScopedTimer LIGHTUSD_CONCAT(timer_scope_, __LINE__)( \
    lightusd::ParserProfiler::GetInstance().GetConfig().enable_profiling \
      ? lightusd::ParserProfiler::GetInstance().GetTimer(parser_name) : nullptr, __FUNCTION__)

#define LIGHTUSD_PROFILE_SCOPE(parser_name, scope_name) \
  lightusd::ScopedTimer LIGHTUSD_CONCAT(timer_scope_, __LINE__)( \
    lightusd::ParserProfiler::GetInstance().GetConfig().enable_profiling \
      ? lightusd::ParserProfiler::GetInstance().GetTimer(parser_name) : nullptr, scope_name)

#define LIGHTUSD_PROFILE_START(parser_name, operation) \
  if (lightusd::ParserProfiler::GetInstance().GetConfig().enable_profiling) { \
    lightusd::ParserProfiler::GetInstance().GetTimer(parser_name)->StartTimer(operation); \
  }

#define LIGHTUSD_PROFILE_END(parser_name, operation) \
  if (lightusd::ParserProfiler::GetInstance().GetConfig().enable_profiling) { \
    lightusd::ParserProfiler::GetInstance().GetTimer(parser_name)->EndTimer(operation); \
  }

}  // namespace lightusd
