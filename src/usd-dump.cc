// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.
//
// USD Layer/Stage inspection utilities (similar to pxrUSD's sdfdump)
//
#include "usd-dump.hh"

#include <sstream>
#include <iomanip>
#include <cmath>

#include "core/prim-spec.hh"
#include "core/layer-types.hh"
#include "layer.hh"
#include "stage.hh"
#include "str-util.hh"
#include "value-pprint.hh"
#include "pprint-enum.hh"
#include "usd-to-json.hh"
#include "minijson.hh"

namespace lightusd {

namespace {

// Helper: create indentation string
std::string Indent(uint32_t level, uint32_t width = 2) {
  return std::string(level * width, ' ');
}

// Forward declarations
void inspect_primspec(std::stringstream &ss, const PrimSpec &ps,
                      const std::string &path, uint32_t depth,
                      const InspectOptions &opts);

void inspect_property(std::stringstream &ss, const std::string &prop_name,
                      const Property &prop, uint32_t depth,
                      const InspectOptions &opts);

// Inspect value with snipping support
std::string inspect_value(const value::Value &val, const InspectOptions &opts) {
  if (opts.value_mode == InspectValueMode::NoValue) {
    return "<omitted>";
  }

  // Use existing pprint infrastructure
  std::string result = value::pprint_value(val, 0, false);

  if (opts.value_mode == InspectValueMode::Full) {
    return result;
  }

  // Snip mode - check if value is large
  // For now, truncate long strings
  if (result.size() > 100) {
    result = result.substr(0, 100) + "...";
  }

  return result;
}

// Inspect TimeSamples
void inspect_timesamples(std::stringstream &ss, const value::TimeSamples &ts,
                         uint32_t depth, const InspectOptions &opts) {
  const auto &samples = ts.get_samples();

  if (samples.empty()) {
    ss << Indent(depth, opts.indent_width) << "timeSamples: {}\n";
    return;
  }

  ss << Indent(depth, opts.indent_width) << "timeSamples:\n";

  size_t count = 0;
  for (size_t i = 0; i < samples.size(); i++) {
    double t = samples[i].t;

    // Time filtering
    if (opts.has_time_query) {
      // Check for single time vs range query using epsilon
      if (std::abs(opts.time_start - opts.time_end) < 1e-9) {
        // Single time query - find closest
        if (i > 0 && std::abs(samples[i - 1].t - opts.time_start) <
                         std::abs(t - opts.time_start)) {
          continue;
        }
        if (i + 1 < samples.size() && std::abs(samples[i + 1].t - opts.time_start) <
                                        std::abs(t - opts.time_start)) {
          continue;
        }
      } else {
        // Range query
        if (t < opts.time_start || t > opts.time_end) {
          continue;
        }
      }
    }

    // Snip mode - limit number of time samples shown
    if (opts.value_mode == InspectValueMode::Snip && count >= opts.snip_count) {
      ss << Indent(depth + 1, opts.indent_width) << "... (" << samples.size()
         << " total samples)\n";
      break;
    }

    ss << Indent(depth + 1, opts.indent_width) << t << ": ";

    if (samples[i].blocked) {
      ss << "None";
    } else {
      ss << inspect_value(samples[i].value, opts);
    }
    ss << "\n";

    count++;
  }
}

// Inspect an Attribute
void inspect_attribute(std::stringstream &ss, const std::string &attr_name,
                       const Attribute &attr, uint32_t depth,
                       const InspectOptions &opts) {
  // Filter by attribute pattern
  if (!opts.attr_pattern.empty()) {
    if (!GlobMatch(opts.attr_pattern, attr_name)) {
      return;
    }
  }

  ss << Indent(depth, opts.indent_width) << attr_name << ":\n";

  // Type name
  if (!attr.type_name().empty()) {
    ss << Indent(depth + 1, opts.indent_width) << "typeName: "
       << attr.type_name() << "\n";
  }

  // Variability
  ss << Indent(depth + 1, opts.indent_width) << "variability: "
     << to_string(attr.variability()) << "\n";

  // Value
  if (attr.has_value()) {
    const auto &pvar = attr.get_var();
    if (opts.value_mode != InspectValueMode::NoValue) {
      ss << Indent(depth + 1, opts.indent_width) << "value: "
         << inspect_value(pvar.value_raw(), opts) << "\n";
    } else {
      ss << Indent(depth + 1, opts.indent_width) << "value: <omitted>\n";
    }
  }

  // TimeSamples
  if (attr.has_timesamples()) {
    // Get timesamples from the PrimVar
    const value::TimeSamples &ts = attr.get_var().ts_raw();
    inspect_timesamples(ss, ts, depth + 1, opts);
  }

  // Connections
  if (attr.has_connections()) {
    ss << Indent(depth + 1, opts.indent_width) << "connections:\n";
    const auto &conns = attr.connections();
    for (const auto &conn : conns) {
      ss << Indent(depth + 2, opts.indent_width) << "- " << to_string(conn)
         << "\n";
    }
  }
}

// Inspect a Property
void inspect_property(std::stringstream &ss, const std::string &prop_name,
                      const Property &prop, uint32_t depth,
                      const InspectOptions &opts) {
  if (prop.is_relationship()) {
    // Filter by attribute pattern (also applies to relationships)
    if (!opts.attr_pattern.empty()) {
      if (!GlobMatch(opts.attr_pattern, prop_name)) {
        return;
      }
    }

    ss << Indent(depth, opts.indent_width) << prop_name << ": (relationship)\n";
    const auto &rel = prop.get_relationship();
    // Get targets from relationship
    std::vector<Path> targets;
    if (rel.is_path()) {
      targets.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      targets = rel.targetPathVector;
    }
    if (!targets.empty()) {
      ss << Indent(depth + 1, opts.indent_width) << "targets:\n";
      for (const auto &target : targets) {
        ss << Indent(depth + 2, opts.indent_width) << "- " << to_string(target)
           << "\n";
      }
    }
  } else if (prop.is_attribute()) {
    inspect_attribute(ss, prop_name, prop.get_attribute(), depth, opts);
  }
}

// Inspect PrimSpec
void inspect_primspec(std::stringstream &ss, const PrimSpec &ps,
                      const std::string &path, uint32_t depth,
                      const InspectOptions &opts) {
  // Filter by path pattern
  if (!opts.prim_path_pattern.empty()) {
    if (!GlobMatchPath(opts.prim_path_pattern, path)) {
      // Still need to check children
      for (const auto &child : ps.children()) {
        std::string child_path = path + "/" + child.name();
        inspect_primspec(ss, child, child_path, depth, opts);
      }
      return;
    }
  }

  // Print path header
  ss << "\n" << Indent(depth, opts.indent_width) << path << ":\n";

  // Specifier
  ss << Indent(depth + 1, opts.indent_width) << "specifier: "
     << to_string(ps.specifier()) << "\n";

  // Type name
  if (!ps.typeName().empty()) {
    ss << Indent(depth + 1, opts.indent_width) << "typeName: " << ps.typeName()
       << "\n";
  }

  // Property names
  const auto &prop_names = ps.propertyNames();
  if (!prop_names.empty()) {
    ss << Indent(depth + 1, opts.indent_width) << "properties: [";
    for (size_t i = 0; i < prop_names.size(); i++) {
      if (i > 0) ss << ", ";
      ss << prop_names[i].str();
    }
    ss << "]\n";
  }

  // Prim children names
  const auto &prim_children = ps.primChildren();
  if (!prim_children.empty()) {
    ss << Indent(depth + 1, opts.indent_width) << "primChildren: [";
    for (size_t i = 0; i < prim_children.size(); i++) {
      if (i > 0) ss << ", ";
      ss << prim_children[i].str();
    }
    ss << "]\n";
  }

  // Properties
  const auto &props = ps.props();
  for (const auto &prop_item : props) {
    inspect_property(ss, prop_item.first, prop_item.second, depth + 1, opts);
  }

  // Children (recursive)
  for (const auto &child : ps.children()) {
    std::string child_path = path + "/" + child.name();
    inspect_primspec(ss, child, child_path, depth, opts);
  }
}

// Inspect Layer metadata (root)
void inspect_layer_metas(std::stringstream &ss, const LayerMetas &metas,
                         const InspectOptions &opts) {
  ss << "/:\n";

  if (metas.upAxis.authored()) {
    ss << Indent(1, opts.indent_width) << "upAxis: "
       << to_string(metas.upAxis.get_value()) << "\n";
  }

  if (metas.metersPerUnit.authored()) {
    ss << Indent(1, opts.indent_width) << "metersPerUnit: "
       << metas.metersPerUnit.get_value() << "\n";
  }

  if (metas.kilogramsPerUnit.authored()) {
    ss << Indent(1, opts.indent_width) << "kilogramsPerUnit: "
       << metas.kilogramsPerUnit.get_value() << "\n";
  }

  if (metas.timeCodesPerSecond.authored()) {
    ss << Indent(1, opts.indent_width) << "timeCodesPerSecond: "
       << metas.timeCodesPerSecond.get_value() << "\n";
  }

  if (metas.framesPerSecond.authored()) {
    ss << Indent(1, opts.indent_width) << "framesPerSecond: "
       << metas.framesPerSecond.get_value() << "\n";
  }

  if (metas.startTimeCode.authored()) {
    ss << Indent(1, opts.indent_width) << "startTimeCode: "
       << metas.startTimeCode.get_value() << "\n";
  }

  if (metas.endTimeCode.authored()) {
    ss << Indent(1, opts.indent_width) << "endTimeCode: "
       << metas.endTimeCode.get_value() << "\n";
  }

  if (!metas.doc.value.empty()) {
    ss << Indent(1, opts.indent_width) << "documentation: \""
       << metas.doc.value << "\"\n";
  }

  if (!metas.defaultPrim.str().empty()) {
    ss << Indent(1, opts.indent_width) << "defaultPrim: "
       << metas.defaultPrim.str() << "\n";
  }

  // List prim children at root
  // (Will be computed from primspecs)
}

}  // namespace

std::string InspectLayer(const Layer &layer, const InspectOptions &opts) {
  std::stringstream ss;

  if (opts.format == InspectOutputFormat::Json) {
    // Keep inspection JSON on the native minijson path.  Besides avoiding a
    // second serializer, this gives callers the same bounded serialization
    // behavior as the public USD-to-JSON API.
    minijson::SerializeOptions json_opts;
    json_opts.indent = static_cast<int>(opts.indent_width);
    json_opts.sort_keys = true;
    json_opts.max_depth = 256;
    std::string json;
    minijson::Error json_error;
    const minijson::Value value = ToJSONValue(layer);
    if (!minijson::Serialize(value, &json, &json_error, json_opts)) {
      ss << "{\"error\":\"JSON serialization failed at offset "
         << json_error.offset << "\"}";
    } else {
      ss << json;
    }
    return ss.str();
  }

  // YAML-like output
  ss << "# USD Layer Inspection\n";

  // Layer metadata (pseudo-root)
  inspect_layer_metas(ss, layer.metas(), opts);

  // Collect root prim names
  std::vector<std::string> root_names;
  for (const auto &ps_item : layer.primspecs()) {
    root_names.push_back(ps_item.first);
  }
  if (!root_names.empty()) {
    ss << Indent(1, opts.indent_width) << "primChildren: [";
    for (size_t i = 0; i < root_names.size(); i++) {
      if (i > 0) ss << ", ";
      ss << root_names[i];
    }
    ss << "]\n";
  }

  // Traverse primspecs
  for (const auto &ps_item : layer.primspecs()) {
    std::string path = "/" + ps_item.first;
    inspect_primspec(ss, ps_item.second, path, 0, opts);
  }

  return ss.str();
}

std::string InspectStage(const Stage &stage, const InspectOptions &opts) {
  std::stringstream ss;

  if (opts.format == InspectOutputFormat::Json) {
    minijson::SerializeOptions json_opts;
    json_opts.indent = static_cast<int>(opts.indent_width);
    json_opts.sort_keys = true;
    json_opts.max_depth = 256;
    std::string json;
    minijson::Error json_error;
    const minijson::Value value = ToJSONValue(stage, nullptr);
    if (!minijson::Serialize(value, &json, &json_error, json_opts)) {
      ss << "{\"error\":\"JSON serialization failed at offset "
         << json_error.offset << "\"}";
    } else {
      ss << json;
    }
    return ss.str();
  }

  // YAML-like output
  ss << "# USD Stage Inspection\n";

  // Stage metadata (same as Layer metadata)
  inspect_layer_metas(ss, stage.metas(), opts);

  // Root prim names
  const auto &root_prims = stage.root_prims();
  if (!root_prims.empty()) {
    ss << Indent(1, opts.indent_width) << "primChildren: [";
    for (size_t i = 0; i < root_prims.size(); i++) {
      if (i > 0) ss << ", ";
      ss << root_prims[i].element_name();
    }
    ss << "]\n";
  }

  // For Stage, we would need to traverse Prims instead of PrimSpecs
  // For now, use ExportToString and note this limitation
  ss << "\n# Note: Stage inspection uses simplified traversal.\n";
  ss << "# Use --flatten with Layer inspection for detailed output.\n";

  // TODO: Implement proper Prim traversal using tydra::VisitPrims

  return ss.str();
}

}  // namespace lightusd
