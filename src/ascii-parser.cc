// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
//
// To reduce compilation time and sections generated in .obj(object file),
// We split implementaion to multiple of .cc for ascii-parser.hh

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
//
#include <algorithm>
#include <atomic>
#include <cstdio>
//#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#if defined(__wasi__)
#else
#include <mutex>
#include <thread>
#endif
#include <vector>

#include "ascii-parser.hh"
#include "parser-timing.hh"
#include "tiny-hashmap.hh"
#include "path-util.hh"
#include "str-util.hh"
#include "tiny-format.hh"

//
#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

//

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external
#include "nonstd/expected.hpp"

//

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

#include "common-macros.inc"

#define CHECK_MEMORY_USAGE(__nbytes) do { \
  uint64_t _chk_nbytes = static_cast<uint64_t>(__nbytes); \
  if (_chk_nbytes > (_max_memory_limit_bytes - _memory_usage)) { \
    PushError(fmt::format("Memory limit exceeded. Limit: {} MB, Current usage: {} MB", \
      _max_memory_limit_bytes / (1024*1024), _memory_usage / (1024*1024))); \
    return false; \
  } \
  _memory_usage += _chk_nbytes; \
  } while(0)

#include "io-util.hh"
#include "pprint-enum.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"

namespace lightusd {

namespace ascii {


constexpr auto kAscii = "[ASCII]";

// Maximum length for identifiers (prim names, property names, etc.)
constexpr size_t kMaxIdentifierLen = 64 * 1024;  // 64KB

// Maximum length for asset path strings (including single and triple-delimited)
constexpr size_t kMaxAssetPathLen = 64 * 1024 * 1024;  // 64MB

// Maximum number of entries in a dictionary or variant map parsed from USDA
constexpr size_t kMaxDictEntries = 1024 * 1024;  // 1M entries

// Register functions moved to ascii-parser-entry.cc


extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<bool>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<int32_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::int4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<uint32_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::uint4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<int64_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<uint64_t>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::half4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<float>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::float4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<double>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double2>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double3>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::double4>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quath>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quatf>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::quatd>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord2d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::texcoord3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::point3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::normal3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::vector3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4h>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::color4d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix2f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix3f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix4f>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix2d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix3d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::matrix4d>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::token>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::StringData>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<std::string>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Reference>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Payload>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<Path>> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<value::AssetPath>> *result);

extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<bool> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<int32_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::int4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint32_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::uint4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<int64_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<uint64_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::half4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<float> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::float4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<double> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::double4> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quath> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quatf> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::quatd> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord2d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::texcoord3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::point3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::normal3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::vector3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4h> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::color4d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix2f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix3f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix4f> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix2d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix3d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::matrix4d> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::token> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::StringData> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<std::string> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Reference> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Payload> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<Path> *result);
extern template bool AsciiParser::ParseBasicTypeArray(
    std::vector<value::AssetPath> *result);

// Register functions (RegisterStageMetas, RegisterPrimMetas, etc.) moved to ascii-parser-entry.cc


namespace {

using ReferenceList = std::vector<std::pair<ListEditQual, Reference>>;

// https://www.techiedelight.com/trim-string-cpp-remove-leading-trailing-spaces/
std::string TrimString(const std::string &str) {
  const std::string WHITESPACE = " \n\r\t\f\v";

  // remove leading and trailing whitespaces
  std::string s = str;
  {
    size_t start = s.find_first_not_of(WHITESPACE);
    s = (start == std::string::npos) ? "" : s.substr(start);
  }

  {
    size_t end = s.find_last_not_of(WHITESPACE);
    s = (end == std::string::npos) ? "" : s.substr(0, end + 1);
  }

  return s;
}

}  // namespace

inline bool isChar(char c) { return std::isalpha(int(c)); }

inline bool hasConnect(const std::string &str) {
  return endsWith(str, ".connect");
}

inline bool hasInputs(const std::string &str) {
  return startsWith(str, "inputs:");
}

inline bool hasOutputs(const std::string &str) {
  return startsWith(str, "outputs:");
}

inline bool is_digit(char x) {
  return (static_cast<unsigned int>((x) - '0') < static_cast<unsigned int>(10));
}

void AsciiParser::SetBaseDir(const std::string &str) { _base_dir = str; }

void AsciiParser::SetStream(StreamReader *sr) { _sr = sr; }

std::string AsciiParser::GetError() {
  if (err_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;

  // Track unique error messages to avoid duplicates
  std::set<std::string> seen_errors;
  std::vector<ErrorDiagnostic> errors;

  // Collect all errors
  while (!err_stack.empty()) {
    errors.push_back(err_stack.top());
    err_stack.pop();
  }

  // Process errors in reverse order (oldest first)
  for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    // Create a unique key for this error location and message
    std::stringstream error_key;
    error_key << diag.cursor.row << ":" << diag.cursor.col << ":" << diag.err;

    // Skip duplicate errors
    if (seen_errors.count(error_key.str()) > 0) {
      continue;
    }
    seen_errors.insert(error_key.str());

    // Format error with error type and precise location
    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ": ";

    // Remove redundant newlines from error message
    std::string clean_err = diag.err;
    if (!clean_err.empty() && clean_err.back() == '\n') {
      clean_err.pop_back();
    }
    ss << clean_err;

    // Add suggestion if available (Priority 5)
    if (!diag.suggestion.empty()) {
      ss << "\n  Suggestion: " << diag.suggestion;
    }

  }

  return ss.str();
}

std::string AsciiParser::GetWarning() {
  if (warn_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;

  // Track unique warning messages to avoid duplicates
  std::set<std::string> seen_warnings;
  std::vector<ErrorDiagnostic> warnings;

  // Collect all warnings
  while (!warn_stack.empty()) {
    warnings.push_back(warn_stack.top());
    warn_stack.pop();
  }

  // Process warnings in reverse order (oldest first)
  for (auto it = warnings.rbegin(); it != warnings.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    // Create a unique key for this warning location and message
    std::stringstream warning_key;
    warning_key << diag.cursor.row << ":" << diag.cursor.col << ":" << diag.err;

    // Skip duplicate warnings
    if (seen_warnings.count(warning_key.str()) > 0) {
      continue;
    }
    seen_warnings.insert(warning_key.str());

    // Format warning with error type and precise location
    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ": ";

    // Remove redundant newlines from warning message
    std::string clean_warn = diag.err;
    if (!clean_warn.empty() && clean_warn.back() == '\n') {
      clean_warn.pop_back();
    }
    ss << clean_warn;

    // Add suggestion if available (Priority 5)
    if (!diag.suggestion.empty()) {
      ss << "\n  Suggestion: " << diag.suggestion;
    }

  }

  return ss.str();
}

std::string AsciiParser::GetErrorWithContext(int context_lines) {
  (void)context_lines;  // Not yet implemented - parameter reserved for future use
  if (err_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;

  // Track unique error messages to avoid duplicates
  std::set<std::string> seen_errors;
  std::vector<ErrorDiagnostic> errors;

  // Collect all errors
  while (!err_stack.empty()) {
    errors.push_back(err_stack.top());
    err_stack.pop();
  }

  // Process errors in reverse order (oldest first)
  for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    // Create a unique key for this error location and message
    std::stringstream error_key;
    error_key << diag.cursor.row << ":" << diag.cursor.col << ":" << diag.err;

    // Skip duplicate errors
    if (seen_errors.count(error_key.str()) > 0) {
      continue;
    }
    seen_errors.insert(error_key.str());

    // Format error with error type and precise location
    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ":\n";

    // Remove redundant newlines from error message
    std::string clean_err = diag.err;
    if (!clean_err.empty() && clean_err.back() == '\n') {
      clean_err.pop_back();
    }
    ss << "  " << clean_err << "\n";

    // Add visual caret indicator for error location
    if (diag.cursor.col > 0) {
      ss << "  ";
      for (int i = 0; i < diag.cursor.col; i++) {
        ss << " ";
      }
      ss << "^\n";
    }
  }

  return ss.str();
}

std::string AsciiParser::GetWarningWithContext(int context_lines) {
  (void)context_lines;  // Not yet implemented - parameter reserved for future use
  if (warn_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;

  // Track unique warning messages to avoid duplicates
  std::set<std::string> seen_warnings;
  std::vector<ErrorDiagnostic> warnings;

  // Collect all warnings
  while (!warn_stack.empty()) {
    warnings.push_back(warn_stack.top());
    warn_stack.pop();
  }

  // Process warnings in reverse order (oldest first)
  for (auto it = warnings.rbegin(); it != warnings.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    // Create a unique key for this warning location and message
    std::stringstream warning_key;
    warning_key << diag.cursor.row << ":" << diag.cursor.col << ":" << diag.err;

    // Skip duplicate warnings
    if (seen_warnings.count(warning_key.str()) > 0) {
      continue;
    }
    seen_warnings.insert(warning_key.str());

    // Format warning with error type and precise location
    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ":\n";

    // Remove redundant newlines from warning message
    std::string clean_warn = diag.err;
    if (!clean_warn.empty() && clean_warn.back() == '\n') {
      clean_warn.pop_back();
    }
    ss << "  " << clean_warn << "\n";

    // Add visual caret indicator for warning location
    if (diag.cursor.col > 0) {
      ss << "  ";
      for (int i = 0; i < diag.cursor.col; i++) {
        ss << " ";
      }
      ss << "^\n";
    }
  }

  return ss.str();
}

std::string AsciiParser::GetErrorWithHints(bool show_hints) {
  (void)show_hints;  // Not yet implemented - parameter reserved for future use
  if (err_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;
  std::set<std::string> seen_errors;
  std::vector<ErrorDiagnostic> errors;

  // Collect all errors
  while (!err_stack.empty()) {
    errors.push_back(err_stack.top());
    err_stack.pop();
  }

  // Process errors in reverse order (oldest first) with aggressive deduplication
  lightusd::HashMap<std::string, int> error_counts;  // Group similar errors by message
  for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;
    error_counts[diag.err]++;
  }

  // Now output with counts for grouped errors
  std::set<std::string> seen_messages;
  for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    if (seen_messages.count(diag.err) > 0) {
      continue;
    }
    seen_messages.insert(diag.err);

    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ": ";

    std::string clean_err = diag.err;
    if (!clean_err.empty() && clean_err.back() == '\n') {
      clean_err.pop_back();
    }
    ss << clean_err;

    // Add occurrence count if this error appears multiple times
    int count = error_counts[diag.err];
    if (count > 1) {
      ss << " [" << count << " occurrence" << (count > 1 ? "s" : "") << "]";
    }

    ss << "\n";

    // Add recovery hint if requested
    if (show_hints && diag.hint != ErrorRecoveryHint::NoHint) {
      const char* hint = diag.GetHint();
      if (hint && std::strlen(hint) > 0) {
        ss << "  Hint: " << hint << "\n";
      }
    }
  }

  return ss.str();
}

std::string AsciiParser::GetWarningWithHints(bool show_hints) {
  (void)show_hints;  // Not yet implemented - parameter reserved for future use
  if (warn_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;
  std::set<std::string> seen_warnings;
  std::vector<ErrorDiagnostic> warnings;

  // Collect all warnings
  while (!warn_stack.empty()) {
    warnings.push_back(warn_stack.top());
    warn_stack.pop();
  }

  // Process warnings in reverse order (oldest first) with aggressive deduplication
  lightusd::HashMap<std::string, int> warning_counts;  // Group similar warnings by message
  for (auto it = warnings.rbegin(); it != warnings.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;
    warning_counts[diag.err]++;
  }

  // Now output with counts for grouped warnings
  std::set<std::string> seen_messages;
  for (auto it = warnings.rbegin(); it != warnings.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    if (seen_messages.count(diag.err) > 0) {
      continue;
    }
    seen_messages.insert(diag.err);

    ss << diag.TypeName() << " at line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ": ";

    std::string clean_warn = diag.err;
    if (!clean_warn.empty() && clean_warn.back() == '\n') {
      clean_warn.pop_back();
    }
    ss << clean_warn;

    // Add occurrence count if this warning appears multiple times
    int count = warning_counts[diag.err];
    if (count > 1) {
      ss << " [" << count << " occurrence" << (count > 1 ? "s" : "") << "]";
    }

    ss << "\n";

    // Add recovery hint if requested
    if (show_hints && diag.hint != ErrorRecoveryHint::NoHint) {
      const char* hint = diag.GetHint();
      if (hint && std::strlen(hint) > 0) {
        ss << "  Hint: " << hint << "\n";
      }
    }
  }

  return ss.str();
}

std::string AsciiParser::GetErrorWithSourceContext(const std::string& filename, int context_lines, int column_width) {
  (void)filename;  // Filename no longer needed as we use StreamReader
  (void)context_lines;  // Parameter reserved for future use
  if (err_stack.empty()) {
    return std::string();
  }

  std::stringstream ss;
  auto is_blank_line = [](const std::string &s) {
    for (char ch : s) {
      if ((ch != ' ') && (ch != '\t')) {
        return false;
      }
    }
    return true;
  };

  // Use StreamReader instead of re-reading file
  if (!_sr || !_sr->data() || _sr->size() == 0) {
    // Fallback to basic error display if StreamReader not available
    return GetError();
  }

  // Parse lines from StreamReader data
  std::vector<std::string> file_lines;
  const uint8_t* data = _sr->data();
  uint64_t size = _sr->size();
  std::string line;

  for (uint64_t i = 0; i < size; ++i) {
    if (data[i] == '\n') {
      file_lines.push_back(line);
      line.clear();
    } else if (data[i] != '\r') {  // Skip CR in CRLF
      line += static_cast<char>(data[i]);
    }
  }
  // Add last line if file doesn't end with newline
  if (!line.empty()) {
    file_lines.push_back(line);
  }

  std::set<std::string> seen_errors;
  std::set<std::string> seen_locations;  // Track locations where context was shown
  std::vector<ErrorDiagnostic> errors;

  // Collect all errors
  while (!err_stack.empty()) {
    errors.push_back(err_stack.top());
    err_stack.pop();
  }

  // Process errors in reverse order (oldest first)
  for (auto it = errors.rbegin(); it != errors.rend(); ++it) {
    const ErrorDiagnostic& diag = *it;

    // Create unique key and skip duplicate error messages
    std::stringstream error_key;
    error_key << diag.cursor.row << ":" << diag.cursor.col << ":" << diag.err;

    if (seen_errors.count(error_key.str()) > 0) {
      continue;
    }
    seen_errors.insert(error_key.str());

    // Check if we've already shown context for this location
    std::stringstream location_key;
    location_key << diag.cursor.row << ":" << diag.cursor.col;
    bool context_already_shown = (seen_locations.count(location_key.str()) > 0);

    // Display error type and location (without header decoration)
    // Use "at" for exact position, "near" for approximate position
    const char* position_word = (diag.position_mode == ErrorPositionMode::Exact) ? "at" : "near";
    ss << diag.TypeName() << " " << position_word << " line " << (diag.cursor.row + 1)
       << ", column " << (diag.cursor.col + 1) << ": ";

    // Clean and display error message on same line
    std::string clean_err = diag.err;
    while (!clean_err.empty() &&
           ((clean_err.back() == '\n') || (clean_err.back() == '\r'))) {
      clean_err.pop_back();
    }
    ss << clean_err << "\n";

    // Display suggestion and context only if not already shown for this location
    if (!context_already_shown) {
      // Display suggestion if available
      if (!diag.suggestion.empty()) {
        ss << "  Suggestion: " << diag.suggestion << "\n";
      }

      // Display source context if file lines are available
      if (static_cast<size_t>(diag.cursor.row) < file_lines.size()) {
      int start_line = std::max(0, diag.cursor.row - 1);
      int end_line = std::min(static_cast<int>(file_lines.size()) - 1, diag.cursor.row + 1);
      while ((end_line > diag.cursor.row) &&
             is_blank_line(file_lines[static_cast<size_t>(end_line)])) {
        end_line--;
      }
      while ((start_line < diag.cursor.row) &&
             is_blank_line(file_lines[static_cast<size_t>(start_line)])) {
        start_line++;
      }

      // Show context lines with proper indentation
      for (int i = start_line; i <= end_line; ++i) {
        bool is_error_line = (i == diag.cursor.row);
        const std::string& source_line = file_lines[static_cast<size_t>(i)];

        // Column snipping for long lines (centered around error column)
        std::string display_line = source_line;
        int caret_offset = diag.cursor.col;

        if (is_error_line && static_cast<int>(source_line.length()) > column_width) {
          int half_width = column_width / 2;
          int start_col = std::max(0, diag.cursor.col - half_width);
          int end_col = std::min(static_cast<int>(source_line.length()), start_col + column_width);

          // Adjust start if we're near the end
          if (end_col - start_col < column_width) {
            start_col = std::max(0, end_col - column_width);
          }

          std::string snippet = source_line.substr(static_cast<size_t>(start_col), static_cast<size_t>(end_col - start_col));

          // Add ellipsis indicators
          if (start_col > 0) {
            display_line = "..." + snippet;
            caret_offset = diag.cursor.col - start_col + 3;  // Account for "..."
          } else {
            display_line = snippet;
            caret_offset = diag.cursor.col;
          }

          if (end_col < static_cast<int>(source_line.length())) {
            display_line += "...";
          }
        }

        // Show source line with indicator
        ss << "  " << (is_error_line ? ">" : " ") << " " << display_line << "\n";

        // Show caret indicator on error line
        if (is_error_line) {
          ss << "    ";
          // Add spaces up to the error column (adjusted for snipping)
          for (int col = 0; col < caret_offset; col++) {
            ss << " ";
          }
          // Add visual indicator (caret)
          ss << "^\n";
        }
      }
      }  // End: Display source context if file lines are available

      // Mark this location as having had context shown
      seen_locations.insert(location_key.str());
    }  // End: Display suggestion and context only if not already shown

  }

  return ss.str();
}

// -- end basic

// types: Allowd in dict.
// std::string is not included since its represented as StringData or
// std::string.
// TODO: Include timecode?
#define APPLY_TO_METAVARIABLE_TYPE(__FUNC) \
  __FUNC(value::token)                     \
  __FUNC(bool)                             \
  __FUNC(value::half)                      \
  __FUNC(value::half2)                     \
  __FUNC(value::half3)                     \
  __FUNC(value::half4)                     \
  __FUNC(int32_t)                          \
  __FUNC(uint32_t)                         \
  __FUNC(value::int2)                      \
  __FUNC(value::int3)                      \
  __FUNC(value::int4)                      \
  __FUNC(value::uint2)                     \
  __FUNC(value::uint3)                     \
  __FUNC(value::uint4)                     \
  __FUNC(int64_t)                          \
  __FUNC(uint64_t)                         \
  __FUNC(float)                            \
  __FUNC(value::float2)                    \
  __FUNC(value::float3)                    \
  __FUNC(value::float4)                    \
  __FUNC(double)                           \
  __FUNC(value::double2)                   \
  __FUNC(value::double3)                   \
  __FUNC(value::double4)                   \
  __FUNC(value::matrix2f)                  \
  __FUNC(value::matrix3f)                  \
  __FUNC(value::matrix4f)                  \
  __FUNC(value::matrix2d)                  \
  __FUNC(value::matrix3d)                  \
  __FUNC(value::matrix4d)                  \
  __FUNC(value::quath)                     \
  __FUNC(value::quatf)                     \
  __FUNC(value::quatd)                     \
  __FUNC(value::normal3h)                  \
  __FUNC(value::normal3f)                  \
  __FUNC(value::normal3d)                  \
  __FUNC(value::vector3h)                  \
  __FUNC(value::vector3f)                  \
  __FUNC(value::vector3d)                  \
  __FUNC(value::point3h)                   \
  __FUNC(value::point3f)                   \
  __FUNC(value::point3d)                   \
  __FUNC(value::color3f)                   \
  __FUNC(value::color3d)                   \
  __FUNC(value::color4f)                   \
  __FUNC(value::color4d)                   \
  __FUNC(value::texcoord2h)                \
  __FUNC(value::texcoord2f)                \
  __FUNC(value::texcoord2d)                \
  __FUNC(value::texcoord3h)                \
  __FUNC(value::texcoord3f)                \
  __FUNC(value::texcoord3d)

bool AsciiParser::ParseDictElement(std::string *out_key,
                                   MetaVariable *out_var) {
  (void)out_key;
  (void)out_var;

  // dict_element: type (array_qual?) name '=' value
  //           ;

  std::string type_name;

  if (!ReadIdentifier(&type_name)) {
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  if (!IsSupportedPrimAttrType(type_name)) {
    PUSH_ERROR_AND_RETURN("Unknown or unsupported type `" + type_name + "`\n");
  }

  // Has array qualifier? `[]`
  bool array_qual = false;
  {
    char c0, c1;
    if (!Char1(&c0)) {
      return false;
    }

    if (c0 == '[') {
      if (!Char1(&c1)) {
        return false;
      }

      if (c1 == ']') {
        array_qual = true;
      } else {
        // Invalid syntax
        PUSH_ERROR_AND_RETURN("Invalid syntax found.");
      }

    } else {
      if (!Rewind(1)) {
        return false;
      }
    }
  }

  if (!SkipWhitespace()) {
    return false;
  }

  std::string key_name;
  if (!ReadIdentifier(&key_name)) {
    // string literal is also supported. e.g. "0"
    if (ReadStringLiteral(&key_name)) {
      // ok
    } else {
      PUSH_ERROR_AND_RETURN("Failed to parse dictionary key identifier.\n");
    }
  }

  if (!SkipWhitespace()) {
    return false;
  }

  if (!Expect('=')) {
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  uint32_t tyid = value::GetTypeId(type_name);

  primvar::PrimVar var;

  //
  // Supports limited types for customData/Dictionary.
  //

  // TODO: Unify code with ParseMetaValue()

#define PARSE_BASE_TYPE(__ty)                                     \
  case value::TypeTraits<__ty>::type_id(): {                      \
    if (array_qual) {                                             \
      std::vector<__ty> vss;                                      \
      if (!ParseBasicTypeArray(&vss)) {                           \
        PUSH_ERROR_AND_RETURN(                                    \
            fmt::format("Failed to parse a value of type `{}[]`", \
                        value::TypeTraits<__ty>::type_name()));   \
      }                                                           \
      var.set_value(vss);                                         \
    } else {                                                      \
      __ty val;                                                   \
      if (!ReadBasicType(&val)) {                                 \
        PUSH_ERROR_AND_RETURN(                                    \
            fmt::format("Failed to parse a value of type `{}`",   \
                        value::TypeTraits<__ty>::type_name()));   \
      }                                                           \
      var.set_value(val);                                         \
    }                                                             \
    break;                                                        \
  }

  switch (tyid) {
    APPLY_TO_METAVARIABLE_TYPE(PARSE_BASE_TYPE)
    case value::TYPE_ID_STRING: {
      // FIXME: Use std::string
      if (array_qual) {
        std::vector<value::StringData> strs;
        if (!ParseBasicTypeArray(&strs)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `string[]`");
        }
        var.set_value(strs);
      } else {
        value::StringData str;
        if (!ReadBasicType(&str)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `string`");
        }
        var.set_value(str);
      }
      break;
    }
    case value::TYPE_ID_ASSET_PATH: {
      if (array_qual) {
        std::vector<value::AssetPath> arrs;
        if (!ParseBasicTypeArray(&arrs)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `asset[]`");
        }
        var.set_value(arrs);
      } else {
        value::AssetPath asset;
        if (!ReadBasicType(&asset)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `asset`");
        }
        var.set_value(asset);
      }
      break;
    }
    case value::TYPE_ID_DICT: {
      Dictionary dict;

      DCOUT("Parse dictionary");
      if (!ParseDict(&dict)) {
        PUSH_ERROR_AND_RETURN("Failed to parse `dictionary`");
      }
      var.set_value(dict);
      break;
    }
    default: {
      PUSH_ERROR_AND_RETURN("Unsupported or invalid type for Metadatum:" +
                            type_name);
    }
  }

#undef PARSE_BASE_TYPE

  MetaVariable metavar;
  metavar.set_value(key_name, var.value_raw());

  DCOUT("key: " << key_name << ", type: " << type_name);

  (*out_key) = key_name;
  (*out_var) = metavar;

  return true;
}

bool AsciiParser::MaybeCustom() {
  std::string tok;

  auto loc = CurrLoc();
  bool ok = ReadIdentifier(&tok);

  if (!ok) {
    // revert
    SeekTo(loc);
    return false;
  }

  if (tok == "custom") {
    // cosume `custom` token.
    return true;
  }

  // revert
  SeekTo(loc);
  return false;
}

bool AsciiParser::ParseDict(std::map<std::string, MetaVariable> *out_dict) {
  // '{' comment | (type name '=' value)+ '}'
  if (_dict_nesting_depth > 64) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii, "Dictionary nesting depth limit exceeded (> 64).");
  }
  _dict_nesting_depth++;
  struct DictDepthGuard {
    uint32_t &depth;
    ~DictDepthGuard() { depth--; }
  } dict_depth_guard{_dict_nesting_depth};

  if (!Expect('{')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '}') {
      break;
    } else {
      if (!Rewind(1)) {
        return false;
      }

      std::string key;
      MetaVariable var;
      if (!ParseDictElement(&key, &var)) {
        PUSH_ERROR_AND_RETURN("Failed to parse dict element.");
      }

      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      if (!var.is_valid()) {
        PUSH_ERROR_AND_RETURN("Invalid Dict element(probably internal issue).");
      }

      DCOUT("Add to dict: " << key);
      (*out_dict)[key] = var;

      if (out_dict->size() > kMaxDictEntries) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Dictionary too large (max {} entries).", kMaxDictEntries));
      }
    }
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  return true;
}

bool AsciiParser::ParseVariantsElement(std::string *out_key,
                                       std::string *out_var) {
  // variants_element: string name '=' value
  //           ;

  std::string type_name;

  if (!ReadIdentifier(&type_name)) {
    return false;
  }

  // must be `string`
  if (type_name != value::kString) {
    PUSH_ERROR_AND_RETURN(
        "LightUSD only accepts type `string` for `variants` element.");
  }

  if (!SkipWhitespace()) {
    return false;
  }

  std::string key_name;
  if (!ReadIdentifier(&key_name)) {
    // string literal is also supported. e.g. "0"
    if (ReadStringLiteral(&key_name)) {
      // ok
    } else {
      PUSH_ERROR_AND_RETURN("Failed to parse dictionary key identifier.\n");
    }
  }

  if (!SkipWhitespace()) {
    return false;
  }

  if (!Expect('=')) {
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  std::string var;
  if (!ReadBasicType(&var)) {
    PUSH_ERROR_AND_RETURN("Failed to parse `string`");
  }

  DCOUT("key: " << key_name << ", value: " << var);

  (*out_key) = key_name;
  (*out_var) = var;

  return true;
}

bool AsciiParser::ParseVariants(VariantSelectionMap *out_map) {
  // '{' (string name '=' value)+ '}'
  if (!Expect('{')) {
    return false;
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '}') {
      break;
    } else {
      if (!Rewind(1)) {
        return false;
      }

      std::string key;
      std::string var;
      if (!ParseVariantsElement(&key, &var)) {
        PUSH_ERROR_AND_RETURN("Failed to parse an element of `variants`.");
      }

      if (!SkipWhitespaceAndNewline()) {
        return false;
      }

      DCOUT("Add to variants: " << key);
      (*out_map)[key] = var;

      if (out_map->size() > kMaxDictEntries) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Variants map too large (max {} entries).", kMaxDictEntries));
      }
    }
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  return true;
}

// 'None'
bool AsciiParser::MaybeNone() {
  std::vector<char> buf;

  auto loc = CurrLoc();

  if (!CharN(4, &buf)) {
    SeekTo(loc);
    return false;
  }

  if ((buf[0] == 'N') && (buf[1] == 'o') && (buf[2] == 'n') &&
      (buf[3] == 'e')) {
    // got it
    return true;
  }

  SeekTo(loc);

  return false;
}

// Consume and return true iff the upcoming token is the literal keyword
// `AnimationBlock` (the SdfAnimationBlock sentinel, parallel to `None`).
// Uses identifier reading so it respects word boundaries.
bool AsciiParser::MaybeAnimationBlock() {
  auto loc = CurrLoc();

  std::string tok;
  if (!ReadIdentifier(&tok)) {
    SeekTo(loc);
    return false;
  }

  if (tok == "AnimationBlock") {
    return true;  // consumed
  }

  SeekTo(loc);
  return false;
}

// Consume and return true iff the upcoming token is the `edit` keyword (the
// start of a VtArrayEdit value: `edit [ ... ]`).
bool AsciiParser::MaybeArrayEdit() {
  auto loc = CurrLoc();

  std::string tok;
  if (!ReadIdentifier(&tok)) {
    SeekTo(loc);
    return false;
  }

  if (tok == "edit") {
    return true;  // consumed
  }

  SeekTo(loc);
  return false;
}

bool AsciiParser::MaybeListEditQual(lightusd::ListEditQual *qual) {
  if (!SkipWhitespace()) {
    return false;
  }

  std::string tok;

  auto loc = CurrLoc();
  if (!ReadIdentifier(&tok)) {
    SeekTo(loc);
    return false;
  }

  if (tok == "prepend") {
    DCOUT("`prepend` list edit qualifier.");
    (*qual) = lightusd::ListEditQual::Prepend;
  } else if (tok == "append") {
    DCOUT("`append` list edit qualifier.");
    (*qual) = lightusd::ListEditQual::Append;
  } else if (tok == "add") {
    DCOUT("`add` list edit qualifier.");
    (*qual) = lightusd::ListEditQual::Add;
  } else if (tok == "delete") {
    DCOUT("`delete` list edit qualifier.");
    (*qual) = lightusd::ListEditQual::Delete;
  } else if (tok == "order") {
    DCOUT("`order` list edit qualifier.");
    (*qual) = lightusd::ListEditQual::Order;
  } else {
    DCOUT("No ListEdit qualifier.");
    // unqualified
    // rewind
    SeekTo(loc);
    (*qual) = lightusd::ListEditQual::ResetToExplicit;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  return true;
}

bool AsciiParser::MaybeVariability(lightusd::Variability *variability,
                                   bool *varying_authored) {
  if (!SkipWhitespace()) {
    return false;
  }

  std::string tok;

  auto loc = CurrLoc();
  if (!ReadIdentifier(&tok)) {
    SeekTo(loc);
    return false;
  }

  if (tok == "uniform") {
    (*variability) = lightusd::Variability::Uniform;
    (*varying_authored) = false;
  } else if (tok == "varying") {
    (*variability) = lightusd::Variability::Varying;
    (*varying_authored) = true;
  } else {
    (*varying_authored) = false;
    // rewind
    SeekTo(loc);
  }

  if (!SkipWhitespace()) {
    return false;
  }

  return true;
}

bool AsciiParser::IsSupportedPrimType(const std::string &ty) {
  return _supported_prim_types.count(ty);
}

bool AsciiParser::IsSupportedPrimAttrType(const std::string &ty) {
  return _supported_prim_attr_types.count(ty);
}

bool AsciiParser::IsSupportedAPISchema(const std::string &ty) {
  return _supported_api_schemas.count(ty);
}

bool AsciiParser::ReadStringLiteral(std::string *literal) {
  constexpr size_t kMaxStringLen = 64 * 1024 * 1024; // 64MB, matches MaybeString
  std::string buf;
  buf.reserve(64);

  // Detect a triple-quoted multi-line string ("""..."""  or '''...''').
  // Without this, an opening `"""` is misread as an empty string `""`
  // followed by stray content, and the surrounding metadata parser
  // fails. Triple-quoted strings appear in MDL shader inputs authored
  // by Omniverse Kit (e.g. `doc = """multi-line text"""`).
  {
    auto peek_loc = CurrLoc();
    std::array<char, 3> peek;
    if (CharN(3, &peek[0])) {
      SeekTo(peek_loc);
      const bool triple_double = peek[0] == '"' && peek[1] == '"' && peek[2] == '"';
      const bool triple_single = peek[0] == '\'' && peek[1] == '\'' && peek[2] == '\'';
      if (triple_double || triple_single) {
        value::StringData sd;
        if (MaybeTripleQuotedString(&sd)) {
          (*literal) = sd.value;
          return true;
        }
        // Fall through if MaybeTripleQuotedString rejected the input
        // (e.g. opening triple-quote but no closing triple within
        // length budget). The single-quote path below will then emit
        // a more specific error.
      }
    }
  }

  char c0;
  if (!Char1(&c0)) {
    return false;
  }

  bool single_quote{false};

  if (c0 == '"') {
    // ok
  } else if (c0 == '\'') {
    // ok
    single_quote = true;
  } else {
    DCOUT("c0 = " << c0);
    PUSH_ERROR_AND_RETURN(
        "String or Token literal expected but it does not start with \" or '");
  }

  bool end_with_quotation{false};

  // Displayed(raw) length of the string literal(excluding quotation chars).
  // Note that this can differ from the length of the unescaped result.
  size_t displayed_len = 0;

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if ((c == '\n') || (c == '\r')) {
      PUSH_ERROR_AND_RETURN("New line in string literal.");
    }

    if (buf.size() >= kMaxStringLen) {
      PushError(fmt::format("String literal too large (> {} bytes).", kMaxStringLen));
      return false;
    }

    if (c == '\\') {
      // Consume the escape pair atomically so that an escaped quotation
      // char(\" or \') does not terminate the literal, and `\\` is not
      // mis-interpreted(e.g. `"a\\"` = token `a\`).
      char nc;
      if (!Char1(&nc)) {
        PUSH_ERROR_AND_RETURN("Truncated escape sequence in string literal.");
      }
      if ((nc == '\n') || (nc == '\r')) {
        PUSH_ERROR_AND_RETURN("New line in string literal.");
      }
      if ((nc == '"') || (nc == '\'')) {
        // Unescape quotation char here. Other escape sequences(\t, \\, \xNN,
        // ...) are decoded by unescapeControlSequence() below.
        buf += nc;
      } else {
        buf += c;
        buf += nc;
      }
      displayed_len += 2;
      continue;
    }

    if (single_quote) {
      if (c == '\'') {
        end_with_quotation = true;
        break;
      }
    } else if (c == '"') {
      end_with_quotation = true;
      break;
    }

    buf += c;
    displayed_len++;
  }

  if (!end_with_quotation) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("String literal expected but it does not end with {}.",
                    single_quote ? "'" : "\""));
  }

  (*literal) = unescapeControlSequence(buf);

  _curr_cursor.col += int(displayed_len + 2);  // +2 for quotation chars

  return true;
}

bool AsciiParser::MaybeString(value::StringData *str) {
  std::string buf;
  buf.reserve(64);

  if (!str) {
    return false;
  }

  auto loc = CurrLoc();
  auto start_cursor = _curr_cursor;

  char c0;
  if (!Char1(&c0)) {
    SeekTo(loc);
    return false;
  }

  // ' or " allowed.
  if ((c0 != '"') && (c0 != '\'')) {
    SeekTo(loc);
    return false;
  }

  bool single_quote = (c0 == '\'');

  bool end_with_quotation{false};

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      SeekTo(loc);
      return false;
    }

    if ((c == '\n') || (c == '\r')) {
      SeekTo(loc);
      return false;
    }

    if (c == '\\') {
      // escaped quote? \" \'
      char nc;
      if (!LookChar1(&nc)) {
        return false;
      }

      if (nc == '\'') {
        buf += '\'';
        _sr->seek_from_current(1);  // advance 1 char
        continue;
      } else if (nc == '"') {
        buf += '"';
        _sr->seek_from_current(1);  // advance 1 char
        continue;
      }
    }

    if (single_quote) {
      if (c == '\'') {
        end_with_quotation = true;
        break;
      }
    } else {
      if (c == '"') {
        end_with_quotation = true;
        break;
      }
    }

    constexpr size_t kMaxStringLen = 64 * 1024 * 1024; // 64MB
    if (buf.size() >= kMaxStringLen) {
      SeekTo(loc);
      PushError(fmt::format("String literal too large (> {} bytes).", kMaxStringLen));
      return false;
    }
    buf += c;
  }

  if (!end_with_quotation) {
    SeekTo(loc);
    return false;
  }

  DCOUT("Single quoted string found. col " << start_cursor.col << ", row "
                                           << start_cursor.row);

  size_t displayed_string_len = buf.size();
  str->value = unescapeControlSequence(buf);
  str->line_col = start_cursor.col;
  str->line_row = start_cursor.row;
  str->is_triple_quoted = false;

  _curr_cursor.col += int(displayed_string_len + 2);  // +2 for quotation chars

  return true;
}

bool AsciiParser::MaybeTripleQuotedString(value::StringData *str) {
  auto loc = CurrLoc();
  auto start_cursor = _curr_cursor;

  std::vector<char> triple_quote;
  if (!CharN(3, &triple_quote)) {
    SeekTo(loc);
    return false;
  }

  if (triple_quote.size() != 3) {
    SeekTo(loc);
    return false;
  }

  bool single_quote = false;

  if (triple_quote[0] == '"' && triple_quote[1] == '"' &&
      triple_quote[2] == '"') {
    // ok
  } else if (triple_quote[0] == '\'' && triple_quote[1] == '\'' &&
             triple_quote[2] == '\'') {
    // ok
    single_quote = true;
  } else {
    SeekTo(loc);
    return false;
  }

  // Read until next triple-quote `"""` or "'''"
  // Limit to prevent OOM from unclosed/huge triple-quoted strings.
  constexpr size_t kMaxTripleQuotedStringLen = 64 * 1024 * 1024; // 64MB
  std::string str_buf;
  str_buf.reserve(256);

  auto locinfo = _curr_cursor;

  int single_quote_count = 0;  // '
  int double_quote_count = 0;  // "

  bool got_closing_triple_quote{false};

  while (!Eof()) {
    char c;

    if (!Char1(&c)) {
      SeekTo(loc);
      return false;
    }

    // Seek \""" or \'''
    // Unescape '\'
    if (c == '\\') {
      std::vector<char> buf(3, '\0');
      if (!LookCharN(3, &buf)) {
        // at least 3 chars should be read
        return false;
      }

      if (buf[0] == '\'' && buf[1] == '\'' && buf[2] == '\'') {
        str_buf += "'''";
        // advance
        _sr->seek_from_current(3);
        locinfo.col += 3;
        continue;
      } else if (buf[0] == '"' && buf[1] == '"' && buf[2] == '"') {
        str_buf += "\"\"\"";
        // advance
        _sr->seek_from_current(3);
        locinfo.col += 3;
        continue;
      }
    }

    if (str_buf.size() >= kMaxTripleQuotedStringLen) {
      SeekTo(loc);
      PUSH_ERROR_AND_RETURN_TAG(kAscii, fmt::format("Triple-quoted string literal too large (> {} bytes).", kMaxTripleQuotedStringLen));
    }
    str_buf += c;

    if (c == '"') {
      double_quote_count++;
      single_quote_count = 0;
    } else if (c == '\'') {
      double_quote_count = 0;
      single_quote_count++;
    } else {
      double_quote_count = 0;
      single_quote_count = 0;
    }

    // Update loc info
    locinfo.col++;
    if (c == '\n') {
      locinfo.col = 0;
      locinfo.row++;
    } else if (c == '\r') {
      // CRLF?
      if (_sr->tell() < (_sr->size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          SeekTo(loc);
          return false;
        }

        if (d == '\n') {
          // CRLF
          str_buf += d;
        } else {
          // unwind 1 char
          if (!_sr->seek_from_current(-1)) {
            // this should not happen.
            SeekTo(loc);
            return false;
          }
        }
      }
      locinfo.col = 0;
      locinfo.row++;
    }

    if (double_quote_count == 3) {
      // got '"""'
      if (single_quote) {
        // continue
      } else {
        got_closing_triple_quote = true;
        break;
      }
    }
    if (single_quote_count == 3) {
      // got '''
      if (!single_quote) {
        // inside """ string, ''' doesn't close it
        // continue
      } else {
        got_closing_triple_quote = true;
        break;
      }
    }
  }

  if (!got_closing_triple_quote) {
    SeekTo(loc);
    return false;
  }

  DCOUT("single_quote = " << single_quote);
  DCOUT("Triple quoted string found. col " << start_cursor.col << ", row "
                                           << start_cursor.row);

  // remove last '"""' or '''
  str->single_quote = single_quote;
  if (str_buf.size() > 3) {  // just in case
    str_buf.erase(str_buf.size() - 3);
  }

  DCOUT("str = " << str_buf);

  str->value = unescapeControlSequence(str_buf);

  DCOUT("unescape str = " << str->value);

  str->line_col = start_cursor.col;
  str->line_row = start_cursor.row;
  str->is_triple_quoted = true;

  _curr_cursor = locinfo;

  return true;
}

bool AsciiParser::ReadPrimAttrIdentifier(std::string *token) {
  // Example:
  // - xformOp:transform
  // - primvars:uvmap1

  std::string buf;
  buf.reserve(64);
  Cursor start_cursor;  // Will be set at the first character
  bool first_char = true;

  // Save stream position and row before reading any characters
  uint64_t start_stream_pos = _sr->tell();
  int start_row = _curr_cursor.row;

  // Helper lambda to calculate correct column from stream position
  auto calculate_cursor_from_stream_pos = [&]() {
    uint64_t line_start_pos = start_stream_pos;
    uint64_t saved_pos = _sr->tell();
    _sr->seek_set(start_stream_pos);

    int col_offset = 0;
    while (line_start_pos > 0) {
      _sr->seek_set(line_start_pos - 1);
      char c_tmp;
      if (_sr->read1(&c_tmp) && (c_tmp == '\n' || c_tmp == '\r')) {
        break;
      }
      line_start_pos--;
      col_offset++;
    }

    _sr->seek_set(saved_pos);
    _curr_cursor.row = start_row;
    _curr_cursor.col = col_offset;
  };

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '_') {
      // ok
    } else if (c == ':') {  // namespace
      // ':' must lie in the middle of string literal
      if (buf.empty()) {
        calculate_cursor_from_stream_pos();
        PUSH_ERROR_AND_RETURN("PrimAttr name must not starts with `:`");
      }
    } else if (c == '.') {  // delimiter for `connect`
      // '.' must lie in the middle of string literal
      if (buf.empty()) {
        calculate_cursor_from_stream_pos();
        PUSH_ERROR_AND_RETURN("PrimAttr name must not starts with `.`");
      }
    } else if (std::isalnum(int(c))) {
      // number must not be allowed for the first char.
      if (buf.empty()) {
        if (!std::isalpha(int(c))) {
          calculate_cursor_from_stream_pos();
          PUSH_ERROR_AND_RETURN("PrimAttr name must not starts with number.");
        }
      }
    } else {
      _sr->seek_from_current(-1);
      break;
    }

    _curr_cursor.col++;

    // Save cursor position after reading the first character
    if (first_char) {
      start_cursor = _curr_cursor;
      first_char = false;
    }

    buf += c;

    if (buf.size() > kMaxIdentifierLen) {
      calculate_cursor_from_stream_pos();
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          fmt::format("PrimAttr identifier too long (max {} bytes).", kMaxIdentifierLen));
    }
  }

  {
    std::string name_err;
    if (!pathutil::ValidatePropPath(Path("", buf), &name_err)) {
      calculate_cursor_from_stream_pos();
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii,
          fmt::format("Invalid Property name `{}`: {}", buf, name_err));
    }
  }

  if (buf.empty()) {
    calculate_cursor_from_stream_pos();
    PUSH_ERROR_AND_RETURN("Empty PrimAttr identifier.");
  }

  // '.' must lie in the middle of string literal
  if (buf.back() == '.') {
    calculate_cursor_from_stream_pos();
    PUSH_ERROR_AND_RETURN("PrimAttr name must not ends with `.`\n");
  }

  std::string tok = std::move(buf);

  if (contains(tok, '.')) {
    if (endsWith(tok, ".connect") || endsWith(tok, ".timeSamples") ||
        endsWith(tok, ".spline")) {
      // OK
    } else {
      // Restore cursor to start position for accurate error reporting
      _curr_cursor = start_cursor;
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii,
          fmt::format("Must ends with `.connect`, `.timeSamples` or `.spline` "
                      "for attrbute name: `{}`",
                      tok));
    }

    // Multiple `.` is not allowed(e.g. attr.connect.timeSamples)
    if (counts(tok, '.') > 1) {
      // Restore cursor to start position for accurate error reporting
      _curr_cursor = start_cursor;
      PUSH_ERROR_AND_RETURN_TAG(
          kAscii, fmt::format("Attribute identifier `{}` containing multiple "
                              "`.` is not allowed.",
                              tok));
    }
  }

  (*token) = std::move(tok);
  DCOUT("primAttr identifier = " << (*token));
  return true;
}

bool AsciiParser::ReadIdentifier(std::string *token) {
  // identifier = (`_` | [a-zA-Z]) (`_` | [a-zA-Z0-9]+)
  std::string buf;
  buf.reserve(64);

  // The first character.
  {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      DCOUT("read1 failed.");
      return false;
    }

    if (c == '_') {
      // ok
    } else if (!std::isalpha(int(c))) {
      DCOUT(fmt::format("Invalid identiefier: '{}'", c));
      _sr->seek_from_current(-1);
      return false;
    }
    _curr_cursor.col++;

    buf += c;
  }

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '_') {
      // ok
    } else if (!std::isalnum(int(c))) {
      _sr->seek_from_current(-1);
      break;  // end of identifier(e.g. ' ')
    }

    _curr_cursor.col++;

    buf += c;

    if (buf.size() > kMaxIdentifierLen) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          fmt::format("Identifier too long (max {} bytes).", kMaxIdentifierLen));
    }
  }

  (*token) = std::move(buf);
  return true;
}

bool AsciiParser::ReadPathIdentifier(std::string *path_identifier) {
  // path_identifier = `<` string `>`
  std::string buf;
  buf.reserve(64);

  if (!Expect('<')) {
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  // read until '>'
  bool ok = false;
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '>') {
      // end
      ok = true;
      _curr_cursor.col++;
      break;
    }

    // TODO: Check if character is valid for path identifier
    buf += c;

    if (buf.size() > kMaxIdentifierLen) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          fmt::format("Path identifier too long (max {} bytes).", kMaxIdentifierLen));
    }
  }

  if (!ok) {
    return false;
  }

  (*path_identifier) = TrimString(buf);
  // std::cout << "PathIdentifier: " << (*path_identifier) << "\n";

  return true;
}

bool AsciiParser::ReadUntilNewline(std::string *str) {
  constexpr size_t kMaxLineLen = 64 * 1024 * 1024; // 64MB
  std::string buf;
  buf.reserve(128);

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (buf.size() >= kMaxLineLen) {
      PushError("Line too long (exceeds 64MB limit).");
      return false;
    }

    if (c == '\n') {
      break;
    } else if (c == '\r') {
      // CRLF?
      if (_sr->tell() < (_sr->size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          return false;
        }

        if (d == '\n') {
          break;
        }

        // unwind 1 char
        if (!_sr->seek_from_current(-1)) {
          // this should not happen.
          return false;
        }

        break;
      }
    }

    buf += c;
  }

  _curr_cursor.row++;
  _curr_cursor.col = 0;

  (*str) = std::move(buf);

  return true;
}

bool AsciiParser::SkipUntilNewline() {
  // Cap the number of bytes skipped to the file size (already bounded by the
  // memory budget) plus a safety limit of 64MB for a single line.
  constexpr size_t kMaxSkipBytes = 64 * 1024 * 1024;  // 64MB
  size_t skipped = 0;

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '\n') {
      break;
    } else if (c == '\r') {
      // CRLF?
      if (_sr->tell() < (_sr->size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          return false;
        }

        if (d == '\n') {
          break;
        }

        // unwind 1 char
        if (!_sr->seek_from_current(-1)) {
          // this should not happen.
          return false;
        }

        break;
      }

    } else {
      // continue
    }

    if (++skipped > kMaxSkipBytes) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          fmt::format("Line too long while skipping (max {} bytes).", kMaxSkipBytes));
    }
  }

  _curr_cursor.row++;
  _curr_cursor.col = 0;
  return true;
}

// metadata_opt := string_literal '\n'
//              |  var '=' value '\n'
//
bool AsciiParser::ParseStageMetaOpt() {
  Cursor layer_meta_cursor = _curr_cursor;

  // Maybe string-only comment.
  // Comment cannot have multiple lines. The last one wins
  {
    value::StringData str;
    if (MaybeTripleQuotedString(&str)) {
      _stage_metas.comment = str;
      RecordLayerMetaCursor("comment", layer_meta_cursor);
      return true;
    } else if (MaybeString(&str)) {
      _stage_metas.comment = str;
      RecordLayerMetaCursor("comment", layer_meta_cursor);
      return true;
    }
  }

  std::string varname;
  if (!ReadIdentifier(&varname)) {
    return false;
  }

  DCOUT("varname = " << varname);

  if (!IsStageMeta(varname)) {
    // Unregistered layer metadata. OpenUSD accepts (and preserves) metadata it
    // does not know, so rejecting the file outright is wrong: a single unknown
    // key made an otherwise valid stage unloadable. Preserve the opinion
    // verbatim (raw USDA text of the value), matching the unregistered *prim*
    // metadata path in ascii-parser-props.cc.
    //
    // Line-oriented, so a multi-line unknown value is not supported.
    if (!Expect('=')) {
      PUSH_ERROR_AND_RETURN(
          "'=' expected after unregistered Stage metadata '" + varname + "'.");
    }
    if (!SkipWhitespace()) {
      return false;
    }
    std::string content;
    if (!ReadUntilNewline(&content)) {
      PUSH_ERROR_AND_RETURN(
          "Failed to parse unregistered Stage metadata '" + varname + "'.");
    }
    DCOUT("Preserved unregistered Stage metadata: " << varname);
    _stage_metas.unregisteredMetas[varname] = content;
    RecordLayerMetaCursor(varname, layer_meta_cursor);
    return true;
  }

  if (!Expect('=')) {
    PUSH_ERROR_AND_RETURN("'=' expected in Stage Metadata opt.");
    return false;
  }

  if (!SkipWhitespace()) {
    return false;
  }

  // `subLayers` items may carry a LayerOffset:
  //   subLayers = [ @a.usda@ (offset = 10; scale = 2), @b.usda@ ]
  // which the generic asset[] parser does not understand, so parse the
  // array with a custom loop here.
  if (varname == "subLayers") {
    if (!Expect('[')) {
      PUSH_ERROR_AND_RETURN("'[' expected for `subLayers` value.");
    }

    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    while (!Eof()) {
      char c;
      if (!LookChar1(&c)) {
        return false;
      }

      if (c == ']') {
        if (!SeekTo(CurrLoc() + 1)) {
          return false;
        }
        break;
      }

      SubLayer sublayer;

      value::AssetPath asset_path;
      bool triple_deliminated{false};
      if (!ParseAssetIdentifier(&asset_path, &triple_deliminated)) {
        PUSH_ERROR_AND_RETURN("Failed to parse asset path in `subLayers`.");
      }
      sublayer.assetPath = asset_path;

      // Optional LayerOffset: `(offset = 10; scale = 2)`
      if (!MaybeParseLayerOffset(&sublayer.layerOffset)) {
        return false;
      }

      CHECK_MEMORY_USAGE(sizeof(SubLayer) +
                         sublayer.assetPath.GetAssetPath().length());
      _stage_metas.subLayers.push_back(sublayer);

      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      // Optional separator
      if (!LookChar1(&c)) {
        return false;
      }
      if (c == ',') {
        if (!SeekTo(CurrLoc() + 1)) {
          return false;
        }
        if (!SkipCommentAndWhitespaceAndNewline()) {
          return false;
        }
      }
    }

    RecordLayerMetaCursor("subLayers", layer_meta_cursor);

    return true;
  }

  // AOUSD Core Spec 10.3.2.6: relocates has special syntax:
  //   relocates = { </source> : </target>, ... }
  // Path-to-path map cannot be parsed by standard ParseMetaValue.
  if (varname == "relocates") {
    if (!Expect('{')) {
      PUSH_ERROR_AND_RETURN("'{' expected for `relocates` value.");
    }
    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    while (!Eof()) {
      char c;
      if (!LookChar1(&c)) {
        return false;
      }
      if (c == '}') {
        if (!SeekTo(CurrLoc() + 1)) { return false; }
        break;
      }

      // Parse source path: </path>
      std::string src_path_str;
      if (!ReadPathIdentifier(&src_path_str)) {
        PUSH_ERROR_AND_RETURN("Failed to parse source path in `relocates`.");
      }

      if (!SkipWhitespace()) { return false; }
      if (!Expect(':')) {
        PUSH_ERROR_AND_RETURN("':' expected between source and target in `relocates`.");
      }
      if (!SkipWhitespace()) { return false; }

      // Parse target path: </path>
      std::string tgt_path_str;
      if (!ReadPathIdentifier(&tgt_path_str)) {
        PUSH_ERROR_AND_RETURN("Failed to parse target path in `relocates`.");
      }

      _stage_metas.relocates.emplace_back(
          Path(src_path_str, ""), Path(tgt_path_str, ""));

      if (_stage_metas.relocates.size() > kMaxDictEntries) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Relocates map too large (max {} entries).", kMaxDictEntries));
      }

      if (!SkipCommentAndWhitespaceAndNewline()) { return false; }

      // Optional trailing comma
      if (!LookChar1(&c)) { return false; }
      if (c == ',') {
        if (!SeekTo(CurrLoc() + 1)) { return false; }
        if (!SkipCommentAndWhitespaceAndNewline()) { return false; }
      }
    }

    return true;
  }

  const VariableDef &vardef = _supported_stage_metas.at(varname);
  MetaVariable var;
  if (!ParseMetaValue(vardef, &var)) {
    PUSH_ERROR_AND_RETURN("Failed to parse meta value.\n");
    return false;
  }
  var.set_name(varname);
  RecordLayerMetaCursor(varname, layer_meta_cursor);
  if (varname == "documentation") {
    RecordLayerMetaCursor("doc", layer_meta_cursor);
  }

  if (varname == "defaultPrim") {
    value::token tok;
    if (var.get_value(&tok)) {
      DCOUT("defaultPrim = " << tok);
      _stage_metas.defaultPrim = tok;
    } else {
      PUSH_ERROR_AND_RETURN("`defaultPrim` isn't a token value.");
    }
  } else if (varname == "subLayers") {
    // NOTE: normally unreachable — `subLayers` is parsed with the custom
    // loop above(to support per-item LayerOffset). Keep as fallback.
    std::vector<value::AssetPath> paths;
    if (var.get_value(&paths)) {
      DCOUT("subLayers = " << paths);
      for (const auto &item : paths) {
        CHECK_MEMORY_USAGE(sizeof(value::AssetPath) + item.GetAssetPath().length());
        SubLayer sublayer;
        sublayer.assetPath = item;
        _stage_metas.subLayers.push_back(sublayer);
      }
    } else {
      PUSH_ERROR_AND_RETURN("`subLayers` isn't an array of asset path");
    }
  } else if (varname == "upAxis") {
    if (auto pv = var.get_value<value::token>()) {
      DCOUT("upAxis = " << pv.value());
      const std::string s = pv.value().str();
      if (s == "X") {
        _stage_metas.upAxis = Axis::X;
      } else if (s == "Y") {
        _stage_metas.upAxis = Axis::Y;
      } else if (s == "Z") {
        _stage_metas.upAxis = Axis::Z;
      } else {
        if (_option.strict_allowedToken_check) {
          PUSH_ERROR_AND_RETURN(
              "Invalid `upAxis` value. Must be \"X\", \"Y\" or \"Z\", but got "
              "\"" +
              s + "\"(Note: Case sensitive)");
        } else {
          PUSH_WARN(
              "Ignore unknown `upAxis` value. Must be \"X\", \"Y\" or \"Z\", "
              "but got "
              "\"" +
              s + "\"(Note: Case sensitive). Use default upAxis `Y`.");
          _stage_metas.upAxis = Axis::Y;
        }
      }
    } else {
      PUSH_ERROR_AND_RETURN("`upAxis` isn't a token value.");
    }
  } else if ((varname == "doc") || (varname == "documentation")) {
    // `documentation` will be shorten to `doc`
    if (auto pv = var.get_value<value::StringData>()) {
      DCOUT("doc = " << to_string(pv.value()));
      _stage_metas.doc = pv.value();
    } else if (auto pvs = var.get_value<std::string>()) {
      value::StringData sdata;
      sdata.value = pvs.value();
      sdata.is_triple_quoted = false;
      _stage_metas.doc = sdata;
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` isn't a string value.", varname));
    }
  } else if (varname == "metersPerUnit") {
    DCOUT("ty = " << var.type_name());
    if (auto pv = var.get_value<float>()) {
      DCOUT("metersPerUnit = " << pv.value());
      _stage_metas.metersPerUnit = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("metersPerUnit = " << pvd.value());
      _stage_metas.metersPerUnit = pvd.value();
    } else {
      PUSH_ERROR_AND_RETURN("`metersPerUnit` isn't a floating-point value.");
    }
  } else if (varname == "kilogramsPerUnit") {
    DCOUT("ty = " << var.type_name());
    if (auto pv = var.get_value<float>()) {
      DCOUT("kilogramsPerUnit = " << pv.value());
      _stage_metas.kilogramsPerUnit = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("kilogramsPerUnit = " << pvd.value());
      _stage_metas.kilogramsPerUnit = pvd.value();
    } else {
      PUSH_ERROR_AND_RETURN("`kilogramsPerUnit` isn't a floating-point value.");
    }
  } else if (varname == "timeCodesPerSecond") {
    DCOUT("ty = " << var.type_name());
    if (auto pv = var.get_value<float>()) {
      DCOUT("metersPerUnit = " << pv.value());
      _stage_metas.timeCodesPerSecond = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("metersPerUnit = " << pvd.value());
      _stage_metas.timeCodesPerSecond = pvd.value();
    } else {
      PUSH_ERROR_AND_RETURN(
          "`timeCodesPerSecond` isn't a floating-point value.");
    }
  } else if (varname == "startTimeCode") {
    if (auto pv = var.get_value<float>()) {
      DCOUT("startTimeCode = " << pv.value());
      _stage_metas.startTimeCode = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("startTimeCode = " << pvd.value());
      _stage_metas.startTimeCode = pvd.value();
    }
  } else if (varname == "endTimeCode") {
    if (auto pv = var.get_value<float>()) {
      DCOUT("endTimeCode = " << pv.value());
      _stage_metas.endTimeCode = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("endTimeCode = " << pvd.value());
      _stage_metas.endTimeCode = pvd.value();
    }
  } else if (varname == "framesPerSecond") {
    if (auto pv = var.get_value<float>()) {
      DCOUT("framesPerSecond = " << pv.value());
      _stage_metas.framesPerSecond = double(pv.value());
    } else if (auto pvd = var.get_value<double>()) {
      DCOUT("framesPerSecond = " << pvd.value());
      _stage_metas.framesPerSecond = pvd.value();
    }
  } else if (varname == "apiSchemas") {
    // TODO: ListEdit qualifer check
    if (auto pv = var.get_value<std::vector<value::token>>()) {
      for (auto &item : pv.value()) {
        if (IsSupportedAPISchema(item.str())) {
          // OK
        } else {
          PUSH_ERROR_AND_RETURN("\"" << item.str()
                                     << "\" is not supported(at the moment) "
                                        "for `apiSchemas` in LightUSD.");
        }
      }
    } else {
      PUSH_ERROR_AND_RETURN("`apiSchemas` isn't an `token[]` type.");
    }
  } else if (varname == "customLayerData") {
    if (auto pv = var.get_value<Dictionary>()) {
      _stage_metas.customLayerData = pv.value();
      _stage_metas.customLayerDataAuthored = true;  // Mark as authored even if empty
    } else {
      PUSH_ERROR_AND_RETURN("`customLayerData` isn't a dictionary value.");
    }
  } else if (varname == "comment") {
    if (auto pv = var.get_value<value::StringData>()) {
      DCOUT("comment = " << to_string(pv.value()));
      _stage_metas.comment = pv.value();
    } else if (auto pvs = var.get_value<std::string>()) {
      value::StringData sdata;
      sdata.value = pvs.value();
      sdata.is_triple_quoted = false;
      _stage_metas.comment = sdata;
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` isn't a string value.", varname));
    }
  } else if (varname == "colorConfiguration") {
    // AOUSD Core Spec: asset path to OCIO config
    if (auto pv = var.get_value<value::AssetPath>()) {
      _stage_metas.colorConfiguration = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`colorConfiguration` isn't an asset path value.");
    }
  } else if (varname == "colorManagementSystem") {
    // AOUSD Core Spec: e.g. "ocio"
    if (auto pv = var.get_value<value::token>()) {
      _stage_metas.colorManagementSystem = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`colorManagementSystem` isn't a token value.");
    }
  } else if (varname == "renderSettingsPrimPath") {
    if (auto pv = var.get_value<value::StringData>()) {
      _stage_metas.renderSettingsPrimPath = pv.value().value;
    } else if (auto pvs = var.get_value<std::string>()) {
      _stage_metas.renderSettingsPrimPath = pvs.value();
    } else {
      PUSH_ERROR_AND_RETURN("`renderSettingsPrimPath` isn't a string value.");
    }
  } else if (varname == "owner") {
    // AOUSD Core Spec: layer owner string
    if (auto pv = var.get_value<value::StringData>()) {
      _stage_metas.owner = pv.value().value;
    } else if (auto pvs = var.get_value<std::string>()) {
      _stage_metas.owner = pvs.value();
    } else {
      PUSH_ERROR_AND_RETURN("`owner` isn't a string value.");
    }
  } else if (varname == "hasOwnedSubLayers") {
    // AOUSD Core Spec
    if (auto pv = var.get_value<bool>()) {
      _stage_metas.hasOwnedSubLayers = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`hasOwnedSubLayers` isn't a bool value.");
    }
  } else if (varname == "expressionVariables") {
    // AOUSD Core Spec: dictionary of variable substitutions
    if (auto pv = var.get_value<Dictionary>()) {
      _stage_metas.expressionVariables = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`expressionVariables` isn't a dictionary value.");
    }
  } else if (varname == "autoPlay") {
    // USDZ extension
    if (auto pv = var.get_value<bool>()) {
      _stage_metas.autoPlay = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`autoPlay` isn't a bool value.");
    }
  } else if (varname == "playbackMode") {
    // USDZ extension
    if (auto pv = var.get_value<value::token>()) {
      _stage_metas.playbackMode = pv.value();
    } else {
      PUSH_ERROR_AND_RETURN("`playbackMode` isn't a token value.");
    }
  } else {
    DCOUT("TODO: Stage meta: " << varname);
    PUSH_WARN("TODO: Stage meta: " << varname);
  }

  return true;
}

// Parse Stage meta
// meta = '(' (comment | metadata_opt)+ ')'
//      ;
bool AsciiParser::ParseStageMetas() {
  if (!Expect('(')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  while (!Eof()) {
    char c;
    if (!LookChar1(&c)) {
      return false;
    }

    if (c == ')') {
      if (!SeekTo(CurrLoc() + 1)) {
        return false;
      }

      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      DCOUT("Stage metas end");

      // end
      return true;

    } else {
      if (!SkipCommentAndWhitespaceAndNewline()) {
        // eof
        return false;
      }

      if (!ParseStageMetaOpt()) {
        // parse error
        return false;
      }
    }

    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }
  }

  DCOUT("ParseStageMetas end");
  return true;
}

// `#` style comment
bool AsciiParser::ParseSharpComment() {
  char c;
  if (!Char1(&c)) {
    // eol
    return false;
  }

  if (c != '#') {
    return false;
  }

  return true;
}

// Fetch 1 char. Do not change input stream position.
bool AsciiParser::LookChar1(char *c) {
  if (!Char1(c)) {
    return false;
  }

  Rewind(1);

  return true;
}

// Fetch N chars. Do not change input stream position.
bool AsciiParser::LookCharN(size_t n, std::vector<char> *nc) {
  std::vector<char> buf(n);

  auto loc = CurrLoc();

  bool ok = _sr->read_exact(n, n, reinterpret_cast<uint8_t *>(buf.data()));
  if (ok) {
    (*nc) = buf;
  }

  SeekTo(loc);

  return ok;
}

// AsciiParser::Char1 is defined inline in ascii-parser.hh.

bool AsciiParser::CharN(size_t n, std::vector<char> *nc) {
  std::vector<char> buf(n);

  bool ok = _sr->read_exact(n, n, reinterpret_cast<uint8_t *>(buf.data()));
  if (ok) {
    (*nc) = buf;
  }

  return ok;
}

bool AsciiParser::CharN(size_t n, char *dst) {
  return _sr->read_exact(n, n, reinterpret_cast<uint8_t*>(dst));
}

bool AsciiParser::Rewind(size_t offset) {
  if (!_sr->seek_from_current(-int64_t(offset))) {
    return false;
  }

  return true;
}

uint64_t AsciiParser::CurrLoc() { return _sr->tell(); }

bool AsciiParser::SeekTo(uint64_t pos) {
  if (!_sr->seek_set(pos)) {
    return false;
  }

  return true;
}

bool AsciiParser::PushParserState() {
  // Stack size must be less than the number of input bytes.
  if (parse_stack.size() >= _sr->size()) {
    PUSH_ERROR_AND_RETURN_TAG(kAscii, "Parser state stack become too deep.");
  }

  uint64_t loc = _sr->tell();

  ParseState state;
  state.loc = int64_t(loc);
  parse_stack.push(state);

  return true;
}

bool AsciiParser::PopParserState(ParseState *state) {
  if (parse_stack.empty()) {
    return false;
  }

  (*state) = parse_stack.top();

  parse_stack.pop();

  return true;
}

bool AsciiParser::SkipWhitespace() {
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }
    _curr_cursor.col++;

    if ((c == ' ') || (c == '\t') || (c == '\f')) {
      // continue
    } else {
      break;
    }
  }

  // unwind 1 char
  if (!_sr->seek_from_current(-1)) {
    return false;
  }
  _curr_cursor.col--;

  return true;
}

bool AsciiParser::SkipWhitespaceAndNewline(const bool allow_semicolon) {
  // USDA also allow C-style ';' as a newline separator.
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    // printf("sws c = %c\n", c);

    if ((c == ' ') || (c == '\t') || (c == '\f')) {
      _curr_cursor.col++;
      // continue
    } else if (allow_semicolon && (c == ';')) {
      _curr_cursor.col++;
      // continue
    } else if (c == '\n') {
      _curr_cursor.col = 0;
      _curr_cursor.row++;
      // continue
    } else if (c == '\r') {
      // CRLF?
      if (_sr->tell() < (_sr->size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          return false;
        }

        if (d == '\n') {
          // CRLF
        } else {
          // unwind 1 char
          if (!_sr->seek_from_current(-1)) {
            // this should not happen.
            return false;
          }
        }
      }
      _curr_cursor.col = 0;
      _curr_cursor.row++;
      // continue
    } else {
      // end loop
      if (!_sr->seek_from_current(-1)) {
        return false;
      }
      break;
    }
  }

  return true;
}

bool AsciiParser::SkipCommentAndWhitespaceAndNewline(
    const bool allow_semicolon) {
  // Skip multiple line of comments.
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    // printf("sws c = %c\n", c);

    if (c == '#') {
      if (!SkipUntilNewline()) {
        return false;
      }
    } else if ((c == ' ') || (c == '\t') || (c == '\f')) {
      _curr_cursor.col++;
      // continue
    } else if (allow_semicolon && (c == ';')) {
      _curr_cursor.col++;
      // continue
    } else if (c == '\n') {
      _curr_cursor.col = 0;
      _curr_cursor.row++;
      // continue
    } else if (c == '\r') {
      // CRLF?
      if (_sr->tell() < (_sr->size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          return false;
        }

        if (d == '\n') {
          // CRLF
        } else {
          // unwind 1 char
          if (!_sr->seek_from_current(-1)) {
            // this should not happen.
            return false;
          }
        }
      }
      _curr_cursor.col = 0;
      _curr_cursor.row++;
      // continue
    } else {
      // std::cout << "unwind\n";
      // end loop
      if (!_sr->seek_from_current(-1)) {
        return false;
      }
      break;
    }
  }

  return true;
}

bool AsciiParser::Expect(char expect_c) {
  if (!SkipWhitespace()) {
    return false;
  }

  char c;
  if (!Char1(&c)) {
    // this should not happen.
    return false;
  }

  bool ret = (c == expect_c);

  if (!ret) {
    std::string msg = "Expected `" + std::string(&expect_c, 1) + "` but got `" +
                      std::string(&c, 1) + "`\n";
    PUSH_ERROR_AND_RETURN(msg);

    // unwind
    _sr->seek_from_current(-1);
  } else {
    _curr_cursor.col++;
  }

  return ret;
}

// Parse magic
// #usda FLOAT
bool AsciiParser::ParseMagicHeader() {
  if (!SkipWhitespace()) {
    return false;
  }

  if (Eof()) {
    return false;
  }

  {
    char magic[6];
    if (!_sr->read_exact(6, 6, reinterpret_cast<uint8_t *>(magic))) {
      // eol
      return false;
    }

    if ((magic[0] == '#') && (magic[1] == 'u') && (magic[2] == 's') &&
        (magic[3] == 'd') && (magic[4] == 'a') && (magic[5] == ' ')) {
      // ok
    } else {
      PUSH_ERROR_AND_RETURN(
          "Magic header must start with `#usda `(at least single whitespace "
          "after 'a') but got `" +
          std::string(magic, 6));
    }
  }

  if (!SkipWhitespace()) {
    // eof
    return false;
  }

  // current we only accept "1.0"
  {
    char ver[3];
    if (!_sr->read_exact(3, 3, reinterpret_cast<uint8_t *>(ver))) {
      return false;
    }

    if ((ver[0] == '1') && (ver[1] == '.') && (ver[2] == '0')) {
      // ok
      _version = 1.0f;
    } else {
      PUSH_ERROR_AND_RETURN("Version must be `1.0` but got `" +
                            std::string(ver, 3) + "`");
    }
  }

  SkipUntilNewline();

  return true;
}

bool AsciiParser::ParseCustomMetaValue() {
  // type identifier '=' value

  // return ParseAttributeMeta();
  PUSH_ERROR_AND_RETURN("TODO");
}

bool AsciiParser::ParseAssetIdentifier(value::AssetPath *out,
                                       bool *triple_deliminated) {
  // '..' or "..." are also allowed.
  // @...@
  // or @@@...@@@ (Triple '@'-deliminated asset identifier.)
  // @@@ = Path containing '@'. '@@@' in Path is encoded as '\@@@'
  //
  // Example:
  //   @bora@
  //   @@@bora@@@
  //   @@@bora\@@@dora@@@

  // TODO: Correctly support escape characters

  // look ahead.
  std::vector<char> buf;
  uint64_t curr = _sr->tell();
  bool maybe_triple{false};

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  if (CharN(3, &buf)) {
    if (buf[0] == '@' && buf[1] == '@' && buf[2] == '@') {
      maybe_triple = true;
    }
  }

  bool valid{false};

  if (!maybe_triple) {
    // delimiter = " ' @

    SeekTo(curr);
    char s;
    if (!Char1(&s)) {
      return false;
    }

    char delim = s;

    if ((s == '@') || (s == '\'') || (s == '"')) {
      // ok
    } else {
      std::string sstr{s};
      PUSH_ERROR_AND_RETURN(
          "Asset must start with '@', '\'' or '\"', but got '" + sstr + "'");
    }

    std::string tok;

    // Read until next delimiter
    bool found_delimiter = false;
    while (!Eof()) {
      char c;

      if (!Char1(&c)) {
        return false;
      }

      if (c == delim) {
        found_delimiter = true;
        break;
      }

      tok += c;

      if (tok.size() > kMaxAssetPathLen) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Asset path too long (max {} bytes).", kMaxAssetPathLen));
      }
    }

    if (found_delimiter) {
      (*out) = tok;
      (*triple_deliminated) = false;

      valid = true;
    }

  } else {
    bool found_delimiter{false};
    bool escape_sequence{false};
    int at_cnt{0};
    std::string tok;

    // Read until '@@@' appears
    // Need to escaped '@@@'("\\@@@")
    while (!Eof()) {
      char c;

      if (!Char1(&c)) {
        return false;
      }

      if (c == '\\') {
        escape_sequence = true;
      }

      if (c == '@') {
        at_cnt++;
      } else {
        at_cnt--;
        if (at_cnt < 0) {
          at_cnt = 0;
        }
      }

      tok += c;

      if (tok.size() > kMaxAssetPathLen) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Asset path too long (max {} bytes).", kMaxAssetPathLen));
      }

      if (at_cnt == 3) {
        if (escape_sequence) {
          // Still in path identifier...
          // Unescape "\\@@@"

          if (tok.size() > 3) {            // this should be true.
            if (endsWith(tok, "\\@@@")) {  // this also should be true.
              tok.erase(tok.size() - 4);
              tok.append("@@@");
            }
          }
          at_cnt = 0;
          escape_sequence = false;
        } else {
          // Got it. '@@@'
          found_delimiter = true;
          break;
        }
      }
    }

    if (found_delimiter) {
      // remote last '@@@'
      (*out) = removeSuffix(tok, "@@@");
      (*triple_deliminated) = true;

      valid = true;
    }
  }

  return valid;
}

bool AsciiParser::ParseOptionalLayerOffset(LayerOffset *out,
                                           Dictionary *out_customData) {
  // Look ahead: an optional `(...)` clause may follow. If the next
  // non-whitespace char is not '(', return without consuming anything.
  if (!SkipWhitespace()) {
    return true;  // EOF is fine; caller decides.
  }
  char c;
  if (!LookChar1(&c)) {
    return true;
  }
  if (c != '(') {
    return true;
  }
  // Consume '('.
  if (!Char1(&c)) return false;

  // Parse `key = value` separated by ';' or ','. Accept any subset of
  // {offset, scale, customData}. Trailing separator allowed.
  for (;;) {
    if (!SkipWhitespaceAndNewline()) return false;
    if (!LookChar1(&c)) return false;
    if (c == ')') {
      // consume and done
      Char1(&c);
      return true;
    }

    std::string key;
    if (!ReadIdentifier(&key)) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          "Expected `offset`, `scale`, or `customData` clause.");
    }
    const bool is_offset = (key == "offset");
    const bool is_scale = (key == "scale");
    const bool is_customData = (key == "customData");
    if (!is_offset && !is_scale && !is_customData) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          fmt::format("Unknown clause key `{}`. Expected `offset`, `scale`,"
                      " or `customData`.", key));
    }
    if (is_customData && !out_customData) {
      PUSH_ERROR_AND_RETURN_TAG(kAscii,
          "`customData` is not allowed on Payload (Reference only).");
    }

    if (!SkipWhitespaceAndNewline()) return false;
    if (!Expect('=')) return false;
    if (!SkipWhitespaceAndNewline()) return false;

    if (is_customData) {
      Dictionary dict;
      if (!ParseDict(&dict)) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            "Failed to parse `customData` dictionary.");
      }
      *out_customData = std::move(dict);
    } else {
      double v = 0.0;
      if (!ReadBasicType(&v)) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
            fmt::format("Failed to parse value for `{}`.", key));
      }
      if (is_offset) {
        out->_offset = v;
      } else {
        out->_scale = v;
      }
    }

    // Separator: optional ';' or ',' OR a newline (newlines are
    // implicit separators in pxr's multi-line form). The closing ')'
    // is handled at top of the loop.
    if (!SkipWhitespaceAndNewline(/*allow_semicolon=*/false)) return false;
    if (!LookChar1(&c)) return false;
    if (c == ';' || c == ',') {
      Char1(&c);
      continue;
    }
    // Either a closing `)` or the next clause's identifier — both fine.
    continue;
  }
}

bool AsciiParser::ParseReference(Reference *out, bool *triple_deliminated) {
  /*
    Asset reference = AsssetIdentifier + optially followd by prim path

    AssetIdentifier could be empty(self-reference?)

    Example:
     "bora"
     @bora@
     @bora@</dora>
     </bora>
  */

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  // Parse AssetIdentifier
  {
    char nc;
    if (!LookChar1(&nc)) {
      return false;
    }

    if (nc == '<') {
      // No Asset Identifier.
      out->asset_path = value::AssetPath("");
    } else {
      value::AssetPath ap;
      if (!ParseAssetIdentifier(&ap, triple_deliminated)) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
                                  "Failed to parse asset path identifier.");
      }
      out->asset_path = ap;
    }
  }

  // Parse optional prim_path
  if (!SkipWhitespace()) {
    return false;
  }

  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '<') {
      if (!Rewind(1)) {
        return false;
      }

      std::string path;
      if (!ReadPathIdentifier(&path)) {
        return false;
      }

      out->prim_path = Path(path, "");
    } else {
      if (!Rewind(1)) {
        return false;
      }
    }
  }

  // Optional `(offset = N; scale = M; customData = {...})` suffix.
  if (!ParseOptionalLayerOffset(&out->layerOffset, &out->customData)) {
    return false;
  }

  return true;
}

bool AsciiParser::ParsePayload(Payload *out, bool *triple_deliminated) {
  // Reference, but no customData.

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  // Parse AssetIdentifier
  {
    char nc;
    if (!LookChar1(&nc)) {
      return false;
    }

    if (nc == '<') {
      // No Asset Identifier.
      out->asset_path = value::AssetPath("");
    } else {
      value::AssetPath ap;
      if (!ParseAssetIdentifier(&ap, triple_deliminated)) {
        PUSH_ERROR_AND_RETURN_TAG(kAscii,
                                  "Failed to parse asset path identifier.");
      }
      out->asset_path = ap;
    }
  }

  // Parse optional prim_path
  if (!SkipWhitespace()) {
    return false;
  }

  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '<') {
      if (!Rewind(1)) {
        return false;
      }

      std::string path;
      if (!ReadPathIdentifier(&path)) {
        return false;
      }

      out->prim_path = Path(path, "");
    } else {
      if (!Rewind(1)) {
        return false;
      }
    }
  }

  if (!ParseOptionalLayerOffset(&out->layerOffset)) {
    return false;
  }

  return true;
}

///
/// Parse optional LayerOffset group: `(offset = <double>; scale = <double>)`.
/// pxr USDA syntax: fields are optional and separated by `;`.
/// When no `(` follows at the current location, this is a no-op(returns true).
///
bool AsciiParser::MaybeParseLayerOffset(LayerOffset *out) {
  if (!out) {
    return false;
  }

  auto loc = CurrLoc();

  // Only skip spaces/tabs — the LayerOffset group must follow on the same
  // line as the asset path/prim path.
  if (!SkipWhitespace()) {
    SeekTo(loc);
    return true;
  }

  char c;
  if (!LookChar1(&c)) {
    SeekTo(loc);
    return true;
  }

  if (c != '(') {
    SeekTo(loc);
    return true;
  }

  // Consume '('
  if (!Char1(&c)) {
    return false;
  }

  while (!Eof()) {
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    if (!LookChar1(&c)) {
      return false;
    }

    if (c == ')') {
      Char1(&c);  // consume
      return true;
    }

    if (c == ';') {
      Char1(&c);  // consume separator
      continue;
    }

    std::string id;
    if (!ReadIdentifier(&id)) {
      PUSH_ERROR_AND_RETURN("Failed to parse parameter name in LayerOffset.");
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    if (!Expect('=')) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("`=` expected after `{}` in LayerOffset.", id));
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    double value{0.0};
    if (!ReadBasicType(&value)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to parse value for `{}` in LayerOffset.", id));
    }

    if (id == "offset") {
      out->_offset = value;
    } else if (id == "scale") {
      out->_scale = value;
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Unsupported parameter `{}` in LayerOffset. Only `offset` and "
          "`scale` are supported.",
          id));
    }
  }

  PUSH_ERROR_AND_RETURN("Unterminated LayerOffset. `)` expected.");
}

bool AsciiParser::ParseMetaValue(const VariableDef &def, MetaVariable *outvar) {
  std::string vartype = def.type;
  const std::string varname = def.name;

  MetaVariable var;

  bool array_qual{false};

  DCOUT("parseMeta: vartype " << vartype);

  if (endsWith(vartype, "[]")) {
    vartype = removeSuffix(vartype, "[]");
    array_qual = true;
  } else if (def.allow_array_type) {  // variable can be array
    // Seek '['
    char c;
    if (LookChar1(&c)) {
      if (c == '[') {
        array_qual = true;
      }
    }
  }

  uint32_t tyid = value::GetTypeId(vartype);

  // `metaName = None` is USD's ValueBlock — an explicitly empty value.
  // Common for array-typed metas (e.g. `apiSchemas = None` to clear
  // an inherited list). The downstream meta-reconstruction code
  // handles the ValueBlock case per-meta (see usda-reader.cc:
  // ReconstructPrimMeta for `apiSchemas`, `variantSets`, etc.).
  if (array_qual) {
    if (MaybeNone()) {
      var.set_value(value::ValueBlock());
      (*outvar) = var;
      return true;
    }
  }

#define PARSE_BASE_TYPE(__ty)                                     \
  case value::TypeTraits<__ty>::type_id(): {                      \
    if (array_qual) {                                             \
      std::vector<__ty> vss;                                      \
      if (!ParseBasicTypeArray(&vss)) {                           \
        PUSH_ERROR_AND_RETURN(                                    \
            fmt::format("Failed to parse a value of type `{}[]`", \
                        value::TypeTraits<__ty>::type_name()));   \
      }                                                           \
      var.set_value(vss);                                         \
    } else {                                                      \
      __ty val;                                                   \
      if (!ReadBasicType(&val)) {                                 \
        PUSH_ERROR_AND_RETURN(                                    \
            fmt::format("Failed to parse a value of type `{}`",   \
                        value::TypeTraits<__ty>::type_name()));   \
      }                                                           \
      var.set_value(val);                                         \
    }                                                             \
    break;                                                        \
  }

  // Special treatment for "Reference" and "Payload"
  if (vartype == "Reference") {
    if (array_qual) {
      std::vector<Reference> refs;
      if (!ParseBasicTypeArray(&refs)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse `{}` in Prim metadataum.", def.name));
      }
      var.set_value(refs);
    } else {
      nonstd::optional<Reference> ref;
      if (!ReadBasicType(&ref)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse `{}` in Prim metadataum.", def.name));
      }
      if (ref) {
        var.set_value(ref.value());
      } else {
        // None
        var.set_value(value::ValueBlock());
      }
    }
  } else if (vartype == "Payload") {
    if (array_qual) {
      std::vector<Payload> refs;
      if (!ParseBasicTypeArray(&refs)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse `{}` in Prim metadataum.", def.name));
      }
      var.set_value(refs);
    } else {
      nonstd::optional<Payload> ref;
      if (!ReadBasicType(&ref)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse `{}` in Prim metadataum.", def.name));
      }
      if (ref) {
        var.set_value(ref.value());
      } else {
        // None
        var.set_value(value::ValueBlock());
      }
    }
  } else if (vartype == value::kPath) {
    if (array_qual) {
      std::vector<Path> paths;
      if (!ParseBasicTypeArray(&paths)) {
        PUSH_ERROR_AND_RETURN_TAG(
            kAscii,
            fmt::format("Failed to parse `{}` in Prim metadatum.", def.name));
      }
      var.set_value(paths);

    } else {
      // `inherits = None` / `specializes = None`: an explicit-clear list op
      // (pxr parses it; a bare Path read fails on the None token).
      if (MaybeNone()) {
        var.set_value(value::ValueBlock());
      } else {
        Path path;
        if (!ReadBasicType(&path)) {
          PUSH_ERROR_AND_RETURN_TAG(
              kAscii,
              fmt::format("Failed to parse `{}` in Prim metadatum.", def.name));
        }
        var.set_value(path);
      }
    }
  } else {
    switch (tyid) {
      APPLY_TO_METAVARIABLE_TYPE(PARSE_BASE_TYPE)
      case value::TYPE_ID_STRING: {
        if (array_qual) {
          std::vector<std::string> strs;
          if (!ParseBasicTypeArray(&strs)) {
            PUSH_ERROR_AND_RETURN("Failed to parse `string[]`");
          }
          var.set_value(strs);
        } else {
          std::string str;
          if (!ReadBasicType(&str)) {
            PUSH_ERROR_AND_RETURN("Failed to parse `string`");
          }
          var.set_value(str);
        }
        break;
      }
      case value::TYPE_ID_ASSET_PATH: {
        if (array_qual) {
          std::vector<value::AssetPath> arrs;
          if (!ParseBasicTypeArray(&arrs)) {
            PUSH_ERROR_AND_RETURN("Failed to parse `asset[]`");
          }
          var.set_value(arrs);
        } else {
          value::AssetPath asset;
          if (!ReadBasicType(&asset)) {
            PUSH_ERROR_AND_RETURN("Failed to parse `asset`");
          }
          var.set_value(asset);
        }
        break;
      }
      case value::TYPE_ID_DICT: {
        Dictionary dict;

        DCOUT("Parse dictionary");
        if (!ParseDict(&dict)) {
          PUSH_ERROR_AND_RETURN("Failed to parse `dictionary`");
        }
        var.set_value(dict);
        break;
      }
      default: {
        std::string tyname = vartype;
        if (array_qual) {
          tyname += "[]";
        }
        PUSH_ERROR_AND_RETURN("Unsupported or invalid type for Metadatum:" +
                              tyname);
      }
    }
  }

#undef PARSE_BASE_TYPE

  (*outvar) = var;

  return true;
}

bool AsciiParser::LexFloat(std::string *result) {
  // FLOATVAL : ('+' or '-')? FLOAT
  // FLOAT
  //     :   ('0'..'9')+ '.' ('0'..'9')* EXPONENT?
  //     |   '.' ('0'..'9')+ EXPONENT?
  //     |   ('0'..'9')+ EXPONENT
  //     ;
  // EXPONENT : ('e'|'E') ('+'|'-')? ('0'..'9')+ ;

  // Stack buffer. A valid IEEE-754 double in scientific form is ≤24 chars
  // (e.g. "-1.7976931348623157e+308"); 64 is a safe upper bound. The
  // previous version reserved a std::string with 32 bytes which exceeded
  // libstdc++'s SSO threshold (15) and forced one heap allocation per call,
  // visible at ~5% of total runtime on USDA-heavy assets.
  constexpr size_t kBufCap = 64;
  char buf[kBufCap];
  size_t n = 0;
  // Match the previous "build into *result then clear on failure" contract:
  // a caller that reads *result after a failure return sees an empty string,
  // not stale data from a previous call.
  result->clear();

#define LEX_APPEND(c)                                            \
  do {                                                           \
    if (n >= kBufCap) {                                          \
      PUSH_ERROR_AND_RETURN("Float literal exceeds " +           \
                            std::to_string(kBufCap) +            \
                            " characters.");                     \
    }                                                            \
    buf[n++] = (c);                                              \
  } while (0)

  bool has_sign{false};
  bool leading_decimal_dots{false};
  {
    char sc;
    if (!Char1(&sc)) {
      return false;
    }
    _curr_cursor.col++;

    // sign, '.' or [0-9]
    if ((sc == '+') || (sc == '-')) {
      LEX_APPEND(sc);
      has_sign = true;

      char c;
      if (!Char1(&c)) {
        return false;
      }

      if (c == '.') {
        // ok. something like `+.7`, `-.53`
        leading_decimal_dots = true;
        _curr_cursor.col++;
        LEX_APPEND(c);

      } else {
        // unwind and continue
        _sr->seek_from_current(-1);
      }

    } else if ((sc >= '0') && (sc <= '9')) {
      // ok
      LEX_APPEND(sc);
    } else if (sc == '.') {
      // ok but rescan again in 2.
      leading_decimal_dots = true;
      if (!Rewind(1)) {
        return false;
      }
      _curr_cursor.col--;
    } else {
      PUSH_ERROR_AND_RETURN("Sign or `.` or 0-9 expected.");
    }
  }

  (void)has_sign;

  // 1. Read the integer part
  char curr;
  if (!leading_decimal_dots) {
    while (!Eof()) {
      if (!Char1(&curr)) {
        return false;
      }

      if ((curr >= '0') && (curr <= '9')) {
        // continue
        LEX_APPEND(curr);
      } else {
        _sr->seek_from_current(-1);
        break;
      }
    }
  }

  if (Eof()) {
    result->assign(buf, n);
    return true;
  }

  if (!Char1(&curr)) {
    return false;
  }

  // 2. Read the decimal part
  if (curr == '.') {
    LEX_APPEND(curr);

    while (!Eof()) {
      if (!Char1(&curr)) {
        return false;
      }

      if ((curr >= '0') && (curr <= '9')) {
        LEX_APPEND(curr);
      } else {
        break;
      }
    }

  } else if ((curr == 'e') || (curr == 'E')) {
    // go to 3.
  } else {
    // end
    _sr->seek_from_current(-1);
    result->assign(buf, n);
    return true;
  }

  if (Eof()) {
    result->assign(buf, n);
    return true;
  }

  // 3. Read the exponent part
  bool has_exp_sign{false};
  if ((curr == 'e') || (curr == 'E')) {
    LEX_APPEND(curr);

    if (!Char1(&curr)) {
      return false;
    }

    if ((curr == '+') || (curr == '-')) {
      // exp sign
      LEX_APPEND(curr);
      has_exp_sign = true;

    } else if ((curr >= '0') && (curr <= '9')) {
      // ok
      LEX_APPEND(curr);
    } else {
      // Empty E is not allowed.
      PUSH_ERROR_AND_RETURN("Empty `E' is not allowed.");
    }

    while (!Eof()) {
      if (!Char1(&curr)) {
        return false;
      }

      if ((curr >= '0') && (curr <= '9')) {
        // ok
        LEX_APPEND(curr);

      } else if ((curr == '+') || (curr == '-')) {
        if (has_exp_sign) {
          // No multiple sign characters
          PUSH_ERROR_AND_RETURN("No multiple exponential sign characters.");
        }

        LEX_APPEND(curr);
        has_exp_sign = true;
      } else {
        // end
        _sr->seek_from_current(-1);
        break;
      }
    }
  } else {
    _sr->seek_from_current(-1);
  }

  result->assign(buf, n);
  return true;

#undef LEX_APPEND
}

nonstd::optional<AsciiParser::VariableDef> AsciiParser::GetStageMetaDefinition(
    const std::string &name) {
  if (_supported_stage_metas.count(name)) {
    return _supported_stage_metas.at(name);
  }

  return nonstd::nullopt;
}

nonstd::optional<AsciiParser::VariableDef> AsciiParser::GetPrimMetaDefinition(
    const std::string &name) {
  if (_supported_prim_metas.count(name)) {
    return _supported_prim_metas.at(name);
  }

  return nonstd::nullopt;
}

nonstd::optional<AsciiParser::VariableDef> AsciiParser::GetPropMetaDefinition(
    const std::string &name) {
  if (_supported_prop_metas.count(name)) {
    return _supported_prop_metas.at(name);
  }

  return nonstd::nullopt;
}

bool AsciiParser::ParseStageMeta(std::pair<ListEditQual, MetaVariable> *out) {
  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  lightusd::ListEditQual qual{ListEditQual::ResetToExplicit};
  if (!MaybeListEditQual(&qual)) {
    return false;
  }

  DCOUT("list-edit qual: " << lightusd::to_string(qual));

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  std::string varname;
  if (!ReadIdentifier(&varname)) {
    return false;
  }

  // std::cout << "varname = `" << varname << "`\n";

  if (!IsStageMeta(varname)) {
    PUSH_ERROR_AND_RETURN("Unsupported or invalid/empty variable name `" +
                          varname + "` for Stage metadatum");
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect('=')) {
    PUSH_ERROR_AND_RETURN("`=` expected.");
    return false;
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  auto pvardef = GetStageMetaDefinition(varname);
  if (!pvardef) {
    // This should not happen though;
    return false;
  }

  auto vardef = (*pvardef);

  MetaVariable var;
  if (!ParseMetaValue(vardef, &var)) {
    return false;
  }
  var.set_name(varname);

  std::get<0>(*out) = qual;
  std::get<1>(*out) = var;

  return true;
}

// Property and attribute parsing moved to ascii-parser-props.cc
// (ParsePrimMeta, ParsePrimMetas, ParseAttrMeta, ParseRelationship,
//  ParseBasicPrimAttr, ParsePrimProps, ParseProperties)

// Entry point, block parsing, and utilities moved to ascii-parser-entry.cc
// (GetCurrentPrimPath, AsciiParser ctors, Setup, ReportProgress,
//  GenerateSuggestion, CheckHeader, IsRegisteredPrimMeta, IsStageMeta,
//  ParseVariantSet, ParseBlock, Parse)

}  // namespace ascii
}  // namespace lightusd

#else  // LIGHTUSD_DISABLE_MODULE_USDA_READER

bool ParseUnregistredValue(const std::string &typeName, const std::string &str,
                           value::Value *value, std::string *err) {
  if (err) {
    (*err) += "USDA_READER module is disabled.\n";
  }
  return false;
}

#endif  // LIGHTUSD_DISABLE_MODULE_USDA_READER
