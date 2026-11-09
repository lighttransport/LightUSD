// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Parser execution time profiling implementation

#include "parser-timing.hh"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace lightusd {

void ParserTimer::StartTimer(const std::string& operation_name) {
  active_timers_[operation_name] = Clock::now();
}

ParserTimer::Duration ParserTimer::EndTimer(const std::string& operation_name) {
  auto end_time = Clock::now();
  auto it = active_timers_.find(operation_name);
  if (it == active_timers_.end()) {
    return Duration{0};
  }

  auto elapsed = std::chrono::duration_cast<Duration>(end_time - it->second);
  active_timers_.erase(it);

  RecordTiming(operation_name, elapsed);
  return elapsed;
}

void ParserTimer::RecordTiming(const std::string& operation_name, Duration elapsed) {
  auto& data = timing_data_[operation_name];
  data.total_time += elapsed;
  data.count++;
}

ParserTimer::Duration ParserTimer::GetTotalTime(const std::string& operation_name) const {
  auto it = timing_data_.find(operation_name);
  return (it != timing_data_.end()) ? it->second.total_time : Duration{0};
}

size_t ParserTimer::GetOperationCount(const std::string& operation_name) const {
  auto it = timing_data_.find(operation_name);
  return (it != timing_data_.end()) ? it->second.count : 0;
}

ParserTimer::Duration ParserTimer::GetAverageTime(const std::string& operation_name) const {
  auto it = timing_data_.find(operation_name);
  if (it == timing_data_.end() || it->second.count == 0) {
    return Duration{0};
  }
  return Duration{static_cast<uint64_t>(it->second.total_time.count()) / it->second.count};
}

std::vector<std::string> ParserTimer::GetOperationNames() const {
  std::vector<std::string> names;
  names.reserve(timing_data_.size());
  for (const auto& pair : timing_data_) {
    names.push_back(pair.first);
  }
  return names;
}

void ParserTimer::Clear() {
  active_timers_.clear();
  timing_data_.clear();
}

std::string ParserTimer::GenerateReport() const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3);

  oss << "Parser Timing Report:\n";
  oss << "=====================\n";

  if (timing_data_.empty()) {
    oss << "No timing data recorded.\n";
    return oss.str();
  }

  // Calculate column widths
  size_t max_name_width = 20;
  for (const auto& pair : timing_data_) {
    max_name_width = std::max(max_name_width, pair.first.length() + 2);
  }

  // Header
  oss << std::left << std::setw(static_cast<int>(max_name_width)) << "Operation"
      << std::right << std::setw(12) << "Total (ms)"
      << std::setw(10) << "Count"
      << std::setw(12) << "Avg (ms)" << "\n";

  oss << std::string(max_name_width + 34, '-') << "\n";

  // Sort operations by total time (descending)
  std::vector<std::pair<std::string, TimingData>> sorted_data(timing_data_.begin(), timing_data_.end());
  std::sort(sorted_data.begin(), sorted_data.end(),
    [](const auto& a, const auto& b) {
      return a.second.total_time > b.second.total_time;
    });

  // Data rows
  for (const auto& pair : sorted_data) {
    const std::string& name = pair.first;
    const TimingData& data = pair.second;

    double total_ms = static_cast<double>(data.total_time.count()) / 1000000.0;
    double avg_ms = (data.count > 0) ? total_ms / static_cast<double>(data.count) : 0.0;

    oss << std::left << std::setw(static_cast<int>(max_name_width)) << name
        << std::right << std::setw(12) << total_ms
        << std::setw(10) << data.count
        << std::setw(12) << avg_ms << "\n";
  }

  return oss.str();
}

ScopedTimer::ScopedTimer(ParserTimer* timer, const std::string& operation_name)
    : timer_(timer), operation_name_(operation_name) {
  if (timer_) {
    timer_->StartTimer(operation_name_);
  }
}

ScopedTimer::~ScopedTimer() {
  if (timer_) {
    timer_->EndTimer(operation_name_);
  }
}

ParserProfiler& ParserProfiler::GetInstance() {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
  static ParserProfiler instance;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  return instance;
}

void ParserProfiler::SetConfig(const ParserProfilingConfig& config) {
  config_ = config;
}

const ParserProfilingConfig& ParserProfiler::GetConfig() const {
  return config_;
}

ParserTimer* ParserProfiler::GetTimer(const std::string& parser_name) {
  // std::map does not invalidate element pointers on insert, so the returned
  // pointer remains valid after the lock is released.
  MutexLockGuard lock(mu_);
  return &timers_[parser_name];
}

std::string ParserProfiler::GenerateReport() const {
  MutexLockGuard lock(mu_);
  std::ostringstream oss;
  oss << "LightUSD Parser Profiling Report\n";
  oss << "================================\n\n";

  if (timers_.empty()) {
    oss << "No profiling data available.\n";
    return oss.str();
  }

  for (const auto& pair : timers_) {
    oss << "Parser: " << pair.first << "\n";
    oss << pair.second.GenerateReport() << "\n";
  }

  return oss.str();
}

void ParserProfiler::ClearAll() {
  MutexLockGuard lock(mu_);
  for (auto& pair : timers_) {
    pair.second.Clear();
  }
}

}  // namespace lightusd
