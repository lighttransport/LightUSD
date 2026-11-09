// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: src/next writer/reader round trips.
//
// The input is fuzzed USDA. Once it parses, exercise the valid-data pipeline:
// USDA parse -> USDA write -> USDA parse -> USDC write/read -> USDZ
// write/read. The existing next_usda/next_usdc harnesses cover malformed
// reader inputs; this harness covers interactions between the parser, writers,
// format detection, and generated output.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/reader/usdz-reader.hh"
#include "next/lightusd-next.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"

namespace {

constexpr size_t kMaxInputSize = 16u << 20;
constexpr size_t kMaxArrayElements = 1u << 20;
constexpr size_t kMaxTokens = 1u << 18;
constexpr size_t kMaxStrings = 1u << 18;
constexpr size_t kMaxFields = 1u << 18;
constexpr size_t kMaxSpecs = 1u << 18;
constexpr size_t kMaxPaths = 1u << 18;

lightusd::next::USDCLoadOptions FuzzUSDCOptions() {
  lightusd::next::USDCLoadOptions options;
  options.crate_options.max_array_elements = kMaxArrayElements;
  options.crate_options.max_tokens = kMaxTokens;
  options.crate_options.max_strings = kMaxStrings;
  options.crate_options.max_fields = kMaxFields;
  options.crate_options.max_specs = kMaxSpecs;
  options.crate_options.max_paths = kMaxPaths;
  return options;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data || size == 0 || size > kMaxInputSize) return 0;

  lightusd::next::LoadOptions parse_options;
  parse_options.parse_options.max_depth = 128;
  parse_options.parse_options.max_file_size = kMaxInputSize;
  parse_options.parse_options.enable_usda_lazy_arrays = true;
  parse_options.parse_options.max_usda_lazy_array_elements = kMaxArrayElements;

  const auto parsed = lightusd::next::LoadUSDAFromString(
      reinterpret_cast<const char*>(data), size, parse_options);
  if (!parsed.success) return 0;

  const std::string usda = lightusd::next::WriteUSDAToString(parsed.stage);
  if (usda.empty() || usda.size() > kMaxInputSize) return 0;

  const auto reparsed =
      lightusd::next::LoadUSDAFromString(usda, parse_options);
  if (!reparsed.success) return 0;

  std::vector<uint8_t> usdc;
  const lightusd::next::USDCWriteResult usdc_write =
      lightusd::next::WriteUSDCToMemory(usdc, reparsed.stage);
  if (!usdc_write.success || usdc.empty() || usdc.size() > kMaxInputSize) {
    return 0;
  }

  const auto usdc_read =
      lightusd::next::LoadUSDCFromMemory(usdc.data(), usdc.size(),
                                         FuzzUSDCOptions());
  if (!usdc_read.success) return 0;

  std::vector<uint8_t> usdz;
  const lightusd::next::USDZWriteResult usdz_write =
      lightusd::next::WriteUSDZToMemory(usdz, reparsed.stage);
  if (!usdz_write.success || usdz.empty() || usdz.size() > kMaxInputSize) {
    return 0;
  }

  lightusd::next::USDZReader package;
  lightusd::next::USDZReadOptions package_options;
  package_options.max_archive_size = kMaxInputSize;
  package_options.max_entry_size = kMaxInputSize;
  if (!package.Open(usdz.data(), usdz.size(), package_options) ||
      package.FindRootLayer() < 0) {
    return 0;
  }

  lightusd::next::Stage loaded;
  lightusd::next::LoadUSDOptions load_options;
  load_options.limits.max_input_bytes = kMaxInputSize;
  load_options.limits.max_asset_bytes = kMaxInputSize;
  load_options.limits.max_resident_bytes = kMaxInputSize;
  std::string warning;
  std::string error;
  (void)lightusd::next::LoadUSDFromMemory(
      usdz.data(), usdz.size(), &loaded, load_options, &warning, &error);
  return 0;
}
