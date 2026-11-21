// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD logger / trace implementation. The method bodies and the heavy
// std headers they need live here so the 11 units that include logger.hh
// only parse declarations.

#include "core/logger.hh"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <limits>
#include <functional>

namespace lightusd {
namespace logging {

void Logger::setLogLevel(LogLevel level) {
    _level = level;
  }
LogLevel Logger::getLogLevel() const {
    return _level;
  }
void Logger::setStream(std::ostream* stream) {
    if (stream) {
      _stream = stream;
    }
  }
std::ostream* Logger::getStream() const {
    return _stream;
  }
bool Logger::shouldLog(LogLevel msgLevel) const {
    return static_cast<int>(msgLevel) >= static_cast<int>(_level);
  }
void Logger::setDedup(bool enabled) { _dedup_enabled = enabled; }
bool Logger::getDedup() const { return _dedup_enabled; }
void Logger::setDedupCapacity(size_t cap) { _dedup_cap = cap ? cap : 1; }
void Logger::resetDedup() { _dedup_seen.clear(); }
void Logger::emit(LogLevel level, const char* file, const char* func, int line,
            const std::string& msg) {
    if (!shouldLog(level)) {
      return;
    }
    if (_dedup_enabled) {
      size_t h = std::hash<std::string>{}(msg);
      h ^= std::hash<const char*>{}(file) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
      h ^= static_cast<size_t>(line) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
      auto it = _dedup_seen.find(h);
      if (it != _dedup_seen.end()) {
        it->second++;  // count suppressed repeats (for potential diagnostics)
        return;
      }
      if (_dedup_seen.size() >= _dedup_cap) {
        _dedup_seen.clear();
      }
      _dedup_seen.emplace(h, 1u);
    }
    (*_stream) << "[" << getLevelName(level) << "] " << file << ":" << func
               << ":" << std::to_string(line) << " " << msg << "\n";
  }
const char* Logger::getLevelName(LogLevel level) {
    switch (level) {
      case LogLevel::Debug: return "DEBUG";
      case LogLevel::Info: return "INFO";
      case LogLevel::Warn: return "WARN";
      case LogLevel::Error: return "ERROR";
      case LogLevel::Critical: return "CRITICAL";
      case LogLevel::Off: return "UNKNOWN";
    }
    return "UNKNOWN";
  }
void TraceManager::setEnabled(bool enabled) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    enabled_ = enabled;
  }
bool TraceManager::isEnabled() const {
    return enabled_;
  }
void TraceManager::setTagEnabled(const std::string& tag, bool enabled, const std::string& subtag ) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    std::string key = tag;
    if (!subtag.empty()) {
      key += "::" + subtag;
    }
    tag_enabled_map_[key] = enabled;
  }
bool TraceManager::isTagEnabled(const std::string& tag, const std::string& subtag ) const LIGHTUSD_REQUIRES_NOT(mutex_) {
    if (!enabled_) return false;  // Global disable overrides everything

    MutexLockGuard lock(mutex_);

    // Check for exact match with subtag
    std::string key = tag;
    if (!subtag.empty()) {
      key += "::" + subtag;
      auto it = tag_enabled_map_.find(key);
      if (it != tag_enabled_map_.end()) {
        return it->second;
      }
    }

    // Check for tag-level setting
    auto it = tag_enabled_map_.find(tag);
    if (it != tag_enabled_map_.end()) {
      return it->second;
    }

    // Default to enabled if not explicitly set
    return true;
  }
void TraceManager::clearTagSettings() LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    tag_enabled_map_.clear();
  }
std::unordered_map<std::string, bool> TraceManager::getTagSettings() const LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    return tag_enabled_map_;
  }
void TraceManager::setReportFormat(TraceReportFormat format) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    report_format_ = format;
  }
TraceReportFormat TraceManager::getReportFormat() const LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    return report_format_;
  }
void TraceManager::setEventLogging(TraceEventLogging mode) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    event_logging_ = mode;
  }
TraceEventLogging TraceManager::getEventLogging() const LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    return event_logging_;
  }
void TraceManager::setEventLogLevel(LogLevel level) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    event_log_level_ = level;
  }
LogLevel TraceManager::getEventLogLevel() const LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    return event_log_level_;
  }
void TraceManager::logTraceBegin(const std::string& tag, const std::string& subtag,
                     const std::string& function, const std::string& file, int line) {
    if (event_logging_ == TraceEventLogging::None) return;
    if (!Logger::getInstance().shouldLog(event_log_level_)) return;

    auto& stream = *Logger::getInstance().getStream();

    if (event_logging_ == TraceEventLogging::JSON) {
      stream << "{\"event\":\"trace_begin\",\"tag\":\"" << escapeJSON(tag) << "\"";
      if (!subtag.empty()) {
        stream << ",\"subtag\":\"" << escapeJSON(subtag) << "\"";
      }
      stream << ",\"function\":\"" << escapeJSON(function) << "\""
             << ",\"file\":\"" << escapeJSON(file) << "\""
             << ",\"line\":" << line
             << ",\"timestamp_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()
             << "}\n";
    } else {
      stream << "[TRACE_BEGIN] " << tag;
      if (!subtag.empty()) {
        stream << "::" << subtag;
      }
      stream << " in " << function << " at " << file << ":" << line << "\n";
    }
  }
void TraceManager::logTraceEnd(const std::string& tag, const std::string& subtag,
                   const std::string& function, double duration_ms) {
    if (event_logging_ == TraceEventLogging::None) return;
    if (!Logger::getInstance().shouldLog(event_log_level_)) return;

    auto& stream = *Logger::getInstance().getStream();

    if (event_logging_ == TraceEventLogging::JSON) {
      stream << "{\"event\":\"trace_end\",\"tag\":\"" << escapeJSON(tag) << "\"";
      if (!subtag.empty()) {
        stream << ",\"subtag\":\"" << escapeJSON(subtag) << "\"";
      }
      stream << ",\"function\":\"" << escapeJSON(function) << "\""
             << ",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration_ms
             << ",\"timestamp_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()
             << "}\n";
    } else {
      stream << "[TRACE_END] " << tag;
      if (!subtag.empty()) {
        stream << "::" << subtag;
      }
      stream << " in " << function << " took "
             << std::fixed << std::setprecision(3) << duration_ms << " ms\n";
    }
  }
void TraceManager::addRecord(const TraceRecord& record) LIGHTUSD_REQUIRES_NOT(mutex_) {
    if (!isTagEnabled(record.tag, record.subtag)) return;

    MutexLockGuard lock(mutex_);
    std::string key = record.tag;
    if (!record.subtag.empty()) {
      key += "::" + record.subtag;
    }
    records_by_tag_[key].push_back(record);
  }
void TraceManager::clearRecords() LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);
    records_by_tag_.clear();
  }
void TraceManager::printSummary(std::ostream& out ) LIGHTUSD_REQUIRES_NOT(mutex_) {
    if (report_format_ == TraceReportFormat::JSON) {
      printSummaryJSON(out);
    } else {
      printSummaryPlainText(out);
    }
  }
void TraceManager::printSummaryPlainText(std::ostream& out ) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);

    out << "\n=== Trace Summary ===\n";
    for (const auto& kv : records_by_tag_) {
      const std::string& key = kv.first;
      const std::vector<TraceRecord>& records = kv.second;

      double total_ms = 0.0;
      double min_ms = (std::numeric_limits<double>::max)();
      double max_ms = 0.0;

      for (const auto& record : records) {
        total_ms += record.duration_ms;
        min_ms = (std::min)(min_ms, record.duration_ms);
        max_ms = (std::max)(max_ms, record.duration_ms);
      }

      double avg_ms = records.empty() ? 0.0 : total_ms / static_cast<double>(records.size());

      out << std::fixed << std::setprecision(3);
      out << "Tag: " << key << "\n";
      out << "  Count: " << records.size() << "\n";
      out << "  Total: " << total_ms << " ms\n";
      out << "  Avg: " << avg_ms << " ms\n";
      out << "  Min: " << min_ms << " ms\n";
      out << "  Max: " << max_ms << " ms\n";
    }
    out << "====================\n";
  }
void TraceManager::printSummaryJSON(std::ostream& out ) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);

    out << "{\n";
    out << "  \"trace_summary\": {\n";

    bool first_tag = true;
    for (const auto& kv : records_by_tag_) {
      const std::string& key = kv.first;
      const std::vector<TraceRecord>& records = kv.second;

      double total_ms = 0.0;
      double min_ms = (std::numeric_limits<double>::max)();
      double max_ms = 0.0;

      for (const auto& record : records) {
        total_ms += record.duration_ms;
        min_ms = (std::min)(min_ms, record.duration_ms);
        max_ms = (std::max)(max_ms, record.duration_ms);
      }

      double avg_ms = records.empty() ? 0.0 : total_ms / static_cast<double>(records.size());

      if (!first_tag) {
        out << ",\n";
      }
      first_tag = false;

      out << std::fixed << std::setprecision(3);
      out << "    \"" << escapeJSON(key) << "\": {\n";
      out << "      \"count\": " << records.size() << ",\n";
      out << "      \"total_ms\": " << total_ms << ",\n";
      out << "      \"avg_ms\": " << avg_ms << ",\n";
      out << "      \"min_ms\": " << min_ms << ",\n";
      out << "      \"max_ms\": " << max_ms << "\n";
      out << "    }";
    }

    out << "\n  }\n";
    out << "}\n";
  }
void TraceManager::printDetailedJSON(std::ostream& out ) LIGHTUSD_REQUIRES_NOT(mutex_) {
    MutexLockGuard lock(mutex_);

    out << "{\n";
    out << "  \"trace_records\": [\n";

    bool first_record = true;
    for (const auto& kv : records_by_tag_) {
      const std::vector<TraceRecord>& records = kv.second;

      for (const auto& record : records) {
        if (!first_record) {
          out << ",\n";
        }
        first_record = false;

        out << std::fixed << std::setprecision(3);
        out << "    {\n";
        out << "      \"tag\": \"" << escapeJSON(record.tag) << "\",\n";
        out << "      \"subtag\": \"" << escapeJSON(record.subtag) << "\",\n";
        out << "      \"duration_ms\": " << record.duration_ms << ",\n";
        out << "      \"function\": \"" << escapeJSON(record.function_name) << "\",\n";
        out << "      \"file\": \"" << escapeJSON(record.file_name) << "\",\n";
        out << "      \"line\": " << record.line_number << ",\n";

        // Format timestamp as ISO 8601 string
#ifndef __EMSCRIPTEN__
        // Emscripten has issues with time_point duration conversion
        // Use duration_cast to handle different clock duration types
        auto start_time_t = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now() +
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
            record.start_time - std::chrono::high_resolution_clock::now()));

        std::stringstream timestamp_ss;
        timestamp_ss << std::put_time(std::gmtime(&start_time_t), "%Y-%m-%dT%H:%M:%S");

        out << "      \"timestamp\": \"" << timestamp_ss.str() << "Z\"\n";
#else
        // Simplified timestamp for WASM builds
        auto now = std::chrono::system_clock::now();
        auto start_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream timestamp_ss;
        timestamp_ss << std::put_time(std::gmtime(&start_time_t), "%Y-%m-%dT%H:%M:%S");
        out << "      \"timestamp\": \"" << timestamp_ss.str() << "Z\"\n";
#endif
        out << "    }";
      }
    }

    out << "\n  ]\n";
    out << "}\n";
  }

}  // namespace logging
}  // namespace lightusd
