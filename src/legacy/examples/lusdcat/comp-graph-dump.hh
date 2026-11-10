#pragma once

#include <string>
#include <vector>

#include "lightusd.hh"
#include "layer.hh"

namespace comp_graph_dump {

struct CompArc {
  std::string arc_type;          // "reference", "payload", "sublayer", "inherits", "specializes", "variantSet"
  std::string source_prim_path;  // Prim path that has this arc (empty for sublayers)
  std::string target_asset_path; // External file path (empty for internal arcs)
  std::string target_prim_path;  // Target prim path within referenced file
  std::string list_edit_qual;    // "prepend", "append", "delete", etc.
  // For variantSet arcs
  std::string variant_set_name;
  std::vector<std::string> variant_names;
  std::string selected_variant;
};

struct CompGraphNode {
  std::string file_path;
  std::string file_format;  // extension-based: "usda", "usdc", "usdz", "usd"
  std::string usd_type;     // content-detected: "ascii", "crate", "usdz", "unknown"
  int64_t file_size{-1};    // bytes, -1 if not a file / not found
  bool parse_attempted{false};
  bool parse_ok{false};
  std::string parse_error;
  int64_t memory_usage{-1}; // Layer memory usage in bytes, -1 = not measured
  std::vector<CompArc> arcs;
};

struct ExtractOptions {
  bool skip_payloads{false};  // --comp-graph-no-payload
  bool parse_only{false};     // -l: only check parse, skip arc extraction
  bool track_memory{false};   // --memstat: capture Layer::estimate_memory_usage()
};

struct CompGraphDump {
  std::string root_file;
  std::vector<CompGraphNode> nodes;

  // Size summaries (computed after extraction)
  int64_t total_file_size{0};
  int64_t total_no_payload{0};
  int64_t total_with_payload{0};
  int64_t file_count{0};
  int64_t file_count_no_payload{0};
  int64_t file_count_with_payload{0};

  // Memory summaries
  int64_t total_memory{0};
  int64_t total_memory_no_payload{0};
  int64_t total_memory_with_payload{0};

  // Parse status
  int64_t parse_ok_count{0};
  int64_t parse_fail_count{0};

  void ComputeSizeSummary();
};

// Extract composition graph from a single loaded Layer
bool ExtractCompGraph(const lightusd::legacy::Layer &layer,
                      const std::string &file_path,
                      CompGraphDump *out, std::string *err);

// Extract composition graph recursively following external references
bool ExtractCompGraphRecursive(const std::string &root_path,
                               CompGraphDump *out,
                               const ExtractOptions &opts,
                               std::string *warn, std::string *err);

std::string CompGraphToJSON(const CompGraphDump &graph);
std::string CompGraphToYAML(const CompGraphDump &graph);
std::string CompGraphToDOT(const CompGraphDump &graph);

}  // namespace comp_graph_dump
