// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA Reader implementation

#include "usda-reader.hh"
#include "../next-util.hh"
#include "../parser/ascii-parser.hh"
#include "../strfmt.hh"

#include <fstream>
#include <cstring>
#include <utility>

namespace lightusd {
namespace next {

namespace {


}  // anonymous namespace

LoadResult LoadUSDAFromFile(const char* filename, const LoadOptions& options) {
  LoadResult result;

  if (!filename) {
    result.error_summary = "Null filename";
    return result;
  }

  // Set base directory if not specified
  LoadOptions opts = options;
  if (opts.base_dir.empty()) {
    opts.base_dir = GetBaseDir(filename);
  }

  AsciiParser parser(opts.parse_options);

  if (parser.ParseFile(filename)) {
    result.success = true;
    result.stage = parser.TakeStage();
  } else {
    result.errors = parser.GetErrors();
    if (!result.errors.empty()) {
      const auto& first_err = result.errors[0];
      result.error_summary = "Line " + UIntToStr(first_err.line) +
                             ", column " + UIntToStr(first_err.column) +
                             ": " + first_err.message;
    }
  }

  result.warnings = parser.GetWarnings();
  return result;
}

LoadResult LoadUSDAFromFile(const std::string& filename, const LoadOptions& options) {
  return LoadUSDAFromFile(filename.c_str(), options);
}

LoadResult LoadUSDAFromString(const char* data, size_t length, const LoadOptions& options) {
  LoadResult result;

  if (!data) {
    result.error_summary = "Null data";
    return result;
  }

  AsciiParser parser(options.parse_options);

  if (parser.Parse(data, length)) {
    result.success = true;
    result.stage = parser.TakeStage();
  } else {
    result.errors = parser.GetErrors();
    if (!result.errors.empty()) {
      const auto& first_err = result.errors[0];
      result.error_summary = "Line " + UIntToStr(first_err.line) +
                             ", column " + UIntToStr(first_err.column) +
                             ": " + first_err.message;
    }
  }

  result.warnings = parser.GetWarnings();
  return result;
}

LoadResult LoadUSDAFromString(const std::string& data, const LoadOptions& options) {
  return LoadUSDAFromString(data.c_str(), data.size(), options);
}

LoadResult LoadUSDAFromStringOwned(std::string&& data,
                                   const LoadOptions& options) {
  LoadResult result;

  AsciiParser parser(options.parse_options);

  if (parser.ParseOwned(std::move(data))) {
    result.success = true;
    result.stage = parser.TakeStage();
  } else {
    result.errors = parser.GetErrors();
    if (!result.errors.empty()) {
      const auto& first_err = result.errors[0];
      result.error_summary = "Line " + UIntToStr(first_err.line) +
                             ", column " + UIntToStr(first_err.column) +
                             ": " + first_err.message;
    }
  }

  result.warnings = parser.GetWarnings();
  return result;
}


}  // namespace next
}  // namespace lightusd
