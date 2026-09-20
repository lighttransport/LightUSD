// SPDX-License-Identifier: Apache-2.0
#include "property-trace.hh"
#include "../writer/value-printer.hh"
#include "../../minijson.hh"
#include <algorithm>
#include <set>

namespace lightusd { namespace next {
PropertyResolutionTrace TraceProperty(pcp::Cache& cache, const Path& path,
    const std::string& property, const EvalOptions& options, size_t max_opinions) {
  PropertyResolutionTrace trace;
  trace.prim_path = path; trace.property = property; trace.time = options.time;
  if (!max_opinions) { trace.complete = false; trace.error = "max_opinions must be positive"; return trace; }
  // Use the authoritative stage evaluator (including clips, fallbacks and
  // instance proxies), rather than guessing values by matching source text.
  Stage stage;
  if (!cache.BuildStage(&stage, &trace.warning, &trace.error)) {
    trace.complete = false; return trace;
  }
  trace.resolved = AttributeEval(&stage).EvalWith(stage.GetPrimAtPath(path), property, options);
  if (options.follow_connections) {
    Path current_prim = path;
    std::string current_property = property;
    std::set<std::string> visited;
    const int depth_limit = std::max(0, options.max_connection_depth);
    for (int depth = 0; depth < depth_limit; ++depth) {
      const std::string key = current_prim.str() + "." + current_property;
      if (!visited.insert(key).second) {
        trace.complete = false;
        trace.error += (trace.error.empty() ? "" : "; ") +
                       std::string("connection cycle at ") + key;
        break;
      }
      const UsdPrim prim = stage.GetPrimAtPath(current_prim);
      const PrimSpec* spec = prim.GetPrimSpec();
      const std::vector<Path>* connections =
          spec ? spec->connection(current_property) : nullptr;
      if (!connections || connections->empty()) break;
      // Attribute evaluation follows the first composed target. Preserve the
      // same rule here so the explanation and resolved value cannot diverge.
      const Path& target = connections->front();
      if (target.prim_path().str().empty() || target.property_name().empty()) {
        trace.complete = false;
        trace.error += (trace.error.empty() ? "" : "; ") +
                       std::string("invalid connection target ") + target.str();
        break;
      }
      trace.connection_chain.push_back(target.str());
      current_prim = target.prim_path();
      current_property = target.property_name();
    }
    if (int(trace.connection_chain.size()) == depth_limit && depth_limit > 0) {
      const UsdPrim prim = stage.GetPrimAtPath(current_prim);
      const PrimSpec* spec = prim.GetPrimSpec();
      if (spec) {
        const std::vector<Path>* more = spec->connection(current_property);
        if (more && !more->empty()) {
          trace.complete = false;
          trace.error += (trace.error.empty() ? "" : "; ") +
                         std::string("connection depth limit reached");
        }
      }
    }
  }
  bool truncated = false;
  trace.opinions = cache.GetPropertyStack(path, property, max_opinions, &truncated,
                                         &trace.warning, &trace.error);
  trace.complete = !truncated && trace.error.empty();
  if (truncated) trace.error += "property opinion limit reached";
  if (!trace.resolved.success && trace.resolved.error.empty())
    trace.resolved.error = "attribute has no resolved value";
  return trace;
}
std::string PropertyResolutionTraceToJSON(const PropertyResolutionTrace& trace) {
  using Json = minijson::Value;
  const auto& r = trace.resolved;
  const char* source = r.from_connection ? "connection" : !r.source_asset.empty() ? "clip" :
      r.from_schema_fallback ? "schemaFallback" : r.from_time_sample ? "timeSample" :
      r.from_default ? "default" : r.blocked ? "blocked" : "none";
  Json opinions = Json::array();
  Json connection_chain = Json::array();
  for (const std::string& target : trace.connection_chain)
    connection_chain.push_back(target);
  bool selected = false;
  for (const auto& o : trace.opinions) {
    bool candidate = r.from_connection ? o.has_connection :
        r.from_time_sample ? o.has_samples : o.has_default;
    // Clip and schema fallback provenance is carried by the resolution record,
    // not falsely attributed to an ordinary authored property opinion.
    bool wins = !selected && !o.suppressed && candidate &&
        !r.from_schema_fallback && r.source_asset.empty() && (r.success || r.blocked);
    if (wins) selected = true;
    const char* status = o.suppressed ? "suppressed" : o.default_value.is_block() ? "blocked" :
        wins ? "winning" : candidate ? "overridden" : "declaration";
    Json samples = Json::array();
    for (double sample : o.sample_times) samples.push_back(sample);
    Json connections = Json::array();
    for (const std::string& connection : o.connections)
      connections.push_back(connection);
    opinions.push_back(Json{{"layer", o.layer_identifier}, {"primPath", o.prim_path},
        {"arc", o.arc}, {"offset", o.offset}, {"scale", o.scale},
        {"hasDefault", o.has_default}, {"hasSamples", o.has_samples},
        {"hasConnection", o.has_connection}, {"status", status}, {"winning", wins},
        {"defaultValue", o.has_default ? PrintValue(o.default_value) : ""},
        {"expressionVariables", o.expression_variables.is_empty()
             ? "" : PrintValue(o.expression_variables)},
        {"sampleTimes", samples}, {"connections", connections}});
  }
  return Json{{"schemaVersion", 1}, {"primPath", trace.prim_path.str()}, {"property", trace.property},
      {"time", trace.time.is_default() ? Json("default") : Json(trace.time.numeric_time())},
      {"complete", trace.complete}, {"warning", trace.warning}, {"error", trace.error},
      {"resolution", Json{{"success", r.success}, {"source", source}, {"sourcePath", r.source_path},
          {"asset", r.source_asset}, {"clipSet", r.source_clip_set}, {"blocked", r.blocked},
          {"interpolated", r.interpolated}, {"value", r.success ? PrintValue(r.value) : ""}, {"error", r.error}}},
      {"clipSelection", Json{{"activeIndex", r.clip_resolution.active_index},
          {"activeAsset", r.clip_resolution.active_asset},
          {"clipTime", r.clip_resolution.clip_time},
          {"interpolatedMissing", r.clip_resolution.interpolated_missing},
          {"lowerAsset", r.clip_resolution.lower_asset},
          {"lowerStageTime", r.clip_resolution.lower_stage_time},
          {"upperAsset", r.clip_resolution.upper_asset},
          {"upperStageTime", r.clip_resolution.upper_stage_time}}},
      {"connectionChain", connection_chain}, {"opinions", opinions}}.dump(2);
}
} }
