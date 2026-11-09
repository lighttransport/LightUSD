// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "asset-resolver.hh"
#include "../pcp/layer-registry.hh"
#include <cstddef>
#include <string>
#include <vector>

namespace lightusd { namespace next {
enum class DependencyScope {
  AuthoredSelections,
  AllAuthoredVariants,
  ComposedStage,
};
struct DependencyRecord {
  std::string authored_path;
  std::string resolved_identifier;
  std::string referring_layer;
  std::string location;
  std::string kind;  // sublayer, reference, payload, clip, texture, asset
  std::string status;  // resolved, missing, cycle, deferred, error
};
struct DependencyOptions {
  DependencyScope scope = DependencyScope::AuthoredSelections;
  // Backward-compatible spelling. When true it overrides scope with
  // AllAuthoredVariants.
  bool all_authored_variants = false;
  bool follow_payloads = true;
  size_t max_layers = 1024;
  size_t max_records = 100000;
  size_t max_depth = 64;
  size_t max_resident_bytes = size_t(1) << 30;
  // UDIM's supported tile interval is inclusive and bounded.
  unsigned first_udim = 1001, last_udim = 1999;
  pcp::LayerLoadOptions load;
};
struct DependencyReport {
  std::string root;
  bool complete = true;
  std::string scope = "authoredSelections";
  std::vector<DependencyRecord> records;
  std::vector<std::string> diagnostics;
};
// Read-only dependency inventory. Authored scopes preserve source sites;
// ComposedStage follows the participating PCP graph and scans final composed
// values with their inherited expression-variable contexts. Resolver callbacks
// are used for every lookup and layer read.
DependencyReport CollectDependencies(const std::string& root,
    AssetResolver& resolver, const DependencyOptions& options = {});
std::string DependencyReportToJSON(const DependencyReport& report);
} }  // namespace lightusd::next
