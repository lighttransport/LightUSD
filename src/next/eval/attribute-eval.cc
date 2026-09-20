// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Attribute Evaluation Implementation

#include "attribute-eval.hh"
#include "value-clip.hh"
#include "../crate/crate-format.hh"  // FloatToHalf (spline value cast)
#include "../types/spline.hh"
#include "../layer/property-index.hh"
#include "../pcp/cache.hh"  // EvalAttributeLazy: cache-backed lazy evaluation
#include "../schema/schema-registry.hh"
#include <algorithm>
#include <cstring>

namespace lightusd {
namespace next {

// ============================================================
// AttributeEval
// ============================================================

AttributeEval::AttributeEval(const Stage* stage) : stage_(stage) {}

EvalResult AttributeEval::Eval(const UsdPrim& prim, const std::string& attr_name) const {
  return EvalInternal(prim, attr_name, options_, 0);
}

EvalResult AttributeEval::EvalAt(const UsdPrim& prim, const std::string& attr_name,
                                  double time) const {
  EvalOptions opts = options_;
  opts.time = TimeQuery::Numeric(time);
  return EvalInternal(prim, attr_name, opts, 0);
}

EvalResult AttributeEval::EvalWith(const UsdPrim& prim, const std::string& attr_name,
                                    const EvalOptions& options) const {
  return EvalInternal(prim, attr_name, options, 0);
}

EvalResult AttributeEval::EvalInternal(const UsdPrim& prim, const std::string& attr_name,
                                        const EvalOptions& opts, int depth) const {
  EvalResult result;

  if (!prim.IsValid()) {
    return result;
  }

  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) {
    return result;
  }

  // Check for connection first (if enabled)
  if (opts.follow_connections && depth < opts.max_connection_depth) {
    // Composed stages store canonical connection targets separately from the
    // legacy `.connect` value slot.  Prefer that representation so namespace
    // qualified property names survive the hop unchanged.
    if (const std::vector<Path>* connections = spec->connection(attr_name)) {
      if (!connections->empty()) {
        const Path& target = connections->front();
        const Path target_prim_path = target.prim_path();
        const std::string target_property = target.property_name();
        if (!target_prim_path.str().empty() && !target_property.empty()) {
          EvalResult connection_result = EvalInternal(
              stage_->GetPrimAtPath(target_prim_path), target_property, opts,
              depth + 1);
          if (connection_result.success) {
            connection_result.from_connection = true;
            return connection_result;
          }
        }
      }
    }
    // Connection attributes typically have "inputs:" prefix and ".connect" suffix
    // Check if there's a connection for this attribute
    std::string conn_attr = attr_name + ".connect";
    const PropSlot* conn_slot = spec->property(conn_attr);
    if (!conn_slot) {
      // Also check without .connect suffix (some APIs store connections directly)
      conn_slot = spec->property(attr_name);
    }

    if (conn_slot && conn_slot->is_connection()) {
      const Value* conn_value = spec->property_value(conn_slot->name_id);
      if (conn_value) {
        const std::string* path = conn_value->as_string();
        if (path && !path->empty()) {
          EvalResult conn_result = FollowConnection(*path, opts, depth + 1);
          if (conn_result.success) {
            conn_result.from_connection = true;
            return conn_result;
          }
        }
      }
    }
  }

  // Try to get value from prim spec
  result = EvalFromPrimSpec(spec, attr_name, opts);
  // LVRPS precedence: opinions from sources at-or-stronger than the
  // clips-introducing source beat clips; opinions composition filled from
  // WEAKER sources (recorded per-property as clipShadowedProps) lose to
  // clips. EvalFromPrimSpec may also have produced the schema fallback;
  // clips replace that too. When clips cannot resolve the property, the
  // shadowed/fallback result below is kept.
  bool clip_shadowed = false;
  if (opts.time.is_numeric() && result.success &&
      !spec->meta().clipShadowedProps().empty()) {
    const std::vector<PropNameId>& shadowed = spec->meta().clipShadowedProps();
    const PropNameId name_id = GetPropNameTable().find(attr_name);
    clip_shadowed = name_id.is_valid() &&
                    std::find(shadowed.begin(), shadowed.end(), name_id) !=
                        shadowed.end();
  }
  if (opts.time.is_numeric() &&
      (!result.success || result.from_schema_fallback || clip_shadowed) &&
      spec->meta().clips().is_dictionary()) {
    Value clipped;
    std::string asset;
    std::string clip_set;
    std::string clip_error;
    ValueClipResolutionInfo clip_resolution;
    if (ResolveValueClip(prim, attr_name, opts.time.numeric_time(),
                         opts.clip_stage_loader, &clipped, &asset, &clip_error,
                         &clip_set, opts.clip_stage_cache.get(),
                         &clip_resolution)) {
      result = EvalResult();
      result.value = std::move(clipped);
      result.success = true;
      result.from_time_sample = true;
      result.source_asset = std::move(asset);
      result.source_clip_set = std::move(clip_set);
      result.clip_resolution = std::move(clip_resolution);
    } else if (opts.strict_aousd_conformance && !clip_error.empty()) {
      result = EvalResult();
      result.error = std::move(clip_error);
      return result;
    }
  }
  if (result.success) {
    result.source_path = prim.GetPath().str();
  }

  return result;
}

EvalResult AttributeEval::EvalFromPrimSpec(const PrimSpec* spec, const std::string& attr_name,
                                            const EvalOptions& opts) {
  EvalResult result;

  PropNameId name_id = GetPropNameTable().find(attr_name);

  // Try time samples first (if time is specified)
  if (opts.time.is_numeric() && name_id.is_valid() &&
      spec->has_time_samples(name_id)) {
    SampleResult sample = spec->interpolate_time_sample(
        name_id, opts.time.numeric_time(), opts.interp);
    if (sample.success) {
      result.value = std::move(sample.value);
      result.success = true;
      result.from_time_sample = true;
      result.interpolated = sample.interpolated;
      return result;
    }
    if (sample.blocked) {
      // Authored per-sample BLOCK: the attribute has no value at this time
      // and weaker sources (spline/default/fallback) must not shine through.
      result.blocked = true;
      return result;
    }
  }

  // Spline (AOUSD 12.3 precedence: timeSamples > spline > default). The
  // authored text parses on demand; the result is cast to the attribute's
  // declared scalar type.
  if (opts.time.is_numeric() && name_id.is_valid()) {
    if (const std::string* spline_text = spec->spline_source(name_id)) {
      SplineData sd;
      if (ParseSplineText(*spline_text, &sd, nullptr)) {
        double v = 0.0;
        if (EvaluateSplineData(sd, opts.time.numeric_time(), &v)) {
          const std::string* tn = spec->property_type_name(attr_name);
          if (tn && *tn == "float") {
            result.value = Value(static_cast<float>(v));
          } else if (tn && *tn == "half") {
            uint16_t bits = FloatToHalf(static_cast<float>(v));
            result.value = Value::MakeFromRaw(TypeId::Half, &bits);
          } else {
            result.value = Value(v);
          }
          result.success = true;
          result.from_time_sample = true;
          result.interpolated = true;
          return result;
        }
        // Spline yields no value at this time (a `none` segment or
        // extrapolation): fall through to the weaker value sources, matching
        // how an uncovered timeSamples query degrades.
      }
    }
  }

  // Fall back to default value
  const Value* default_val = spec->property_value(attr_name);
  if (default_val && default_val->is_block()) result.blocked = true;
  if (default_val && !default_val->is_empty() && !default_val->is_block()) {
    result.value = *default_val;
    result.success = true;
    result.from_default = true;
    return result;
  }

  // Try by name_id if string lookup failed
  if (name_id.is_valid()) {
    default_val = spec->property_value(name_id);
    if (default_val && default_val->is_block()) result.blocked = true;
    if (default_val && !default_val->is_empty() && !default_val->is_block()) {
      result.value = *default_val;
      result.success = true;
      result.from_default = true;
      return result;
    }
  }

  if (const SchemaPropertyDefinition* def =
          GetSchemaRegistry().FindProperty(*spec, attr_name)) {
    if (def->has_fallback) {
      result.value = def->fallback;
      result.success = true;
      result.from_schema_fallback = true;
      return result;
    }
  }

  return result;
}

EvalResult AttributeEval::FollowConnection(const std::string& connection_path,
                                            const EvalOptions& opts, int depth) const {
  EvalResult result;

  if (!stage_ || connection_path.empty() || depth >= opts.max_connection_depth) {
    return result;
  }

  // Parse connection path: /Prim/Path.attribute or /Prim/Path.outputs:name
  size_t dot_pos = connection_path.rfind('.');
  if (dot_pos == std::string::npos) {
    return result;
  }

  std::string prim_path = connection_path.substr(0, dot_pos);
  std::string attr_part = connection_path.substr(dot_pos + 1);

  // Handle outputs:xxx format
  std::string target_attr = attr_part;
  if (attr_part.find("outputs:") == 0) {
    target_attr = attr_part.substr(8);  // Remove "outputs:" prefix
  } else if (attr_part.find("inputs:") == 0) {
    target_attr = attr_part.substr(7);  // Remove "inputs:" prefix
  }

  // Find the connected prim
  UsdPrim connected_prim = stage_->GetPrimAtPath(prim_path);
  if (!connected_prim.IsValid()) {
    return result;
  }

  // Evaluate the attribute on the connected prim
  return EvalInternal(connected_prim, target_attr, opts, depth);
}

// ============================================================
// Type-safe evaluation methods
// ============================================================

std::optional<bool> AttributeEval::EvalBool(const UsdPrim& prim,
                                             const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const bool* v = result.value.as_bool();
  return v ? std::optional<bool>(*v) : std::nullopt;
}

std::optional<int32_t> AttributeEval::EvalInt(const UsdPrim& prim,
                                               const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const int32_t* v = result.value.as_int();
  return v ? std::optional<int32_t>(*v) : std::nullopt;
}

std::optional<float> AttributeEval::EvalFloat(const UsdPrim& prim,
                                               const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const float* v = result.value.as_float();
  return v ? std::optional<float>(*v) : std::nullopt;
}

std::optional<double> AttributeEval::EvalDouble(const UsdPrim& prim,
                                                 const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const double* v = result.value.as_double();
  return v ? std::optional<double>(*v) : std::nullopt;
}

bool AttributeEval::EvalFloat2(const UsdPrim& prim, const std::string& attr_name,
                                float* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const float* v = result.value.as_float2();
  if (!v) return false;
  std::memcpy(out, v, sizeof(float) * 2);
  return true;
}

bool AttributeEval::EvalFloat3(const UsdPrim& prim, const std::string& attr_name,
                                float* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const float* v = result.value.as_float3();
  if (!v) return false;
  std::memcpy(out, v, sizeof(float) * 3);
  return true;
}

bool AttributeEval::EvalFloat4(const UsdPrim& prim, const std::string& attr_name,
                                float* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const float* v = result.value.as_float4();
  if (!v) return false;
  std::memcpy(out, v, sizeof(float) * 4);
  return true;
}

bool AttributeEval::EvalDouble2(const UsdPrim& prim, const std::string& attr_name,
                                 double* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const double* v = result.value.as_double2();
  if (!v) return false;
  std::memcpy(out, v, sizeof(double) * 2);
  return true;
}

bool AttributeEval::EvalDouble3(const UsdPrim& prim, const std::string& attr_name,
                                 double* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const double* v = result.value.as_double3();
  if (!v) return false;
  std::memcpy(out, v, sizeof(double) * 3);
  return true;
}

bool AttributeEval::EvalDouble4(const UsdPrim& prim, const std::string& attr_name,
                                 double* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const double* v = result.value.as_double4();
  if (!v) return false;
  std::memcpy(out, v, sizeof(double) * 4);
  return true;
}

bool AttributeEval::EvalMatrix3f(const UsdPrim& prim, const std::string& attr_name,
                                  float* out9) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const float* v = result.value.as_matrix3f();
  if (!v) return false;
  std::memcpy(out9, v, sizeof(float) * 9);
  return true;
}

bool AttributeEval::EvalMatrix4f(const UsdPrim& prim, const std::string& attr_name,
                                  float* out16) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const float* v = result.value.as_matrix4f();
  if (!v) return false;
  std::memcpy(out16, v, sizeof(float) * 16);
  return true;
}

bool AttributeEval::EvalMatrix3d(const UsdPrim& prim, const std::string& attr_name,
                                  double* out9) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const double* v = result.value.as_matrix3d();
  if (!v) return false;
  std::memcpy(out9, v, sizeof(double) * 9);
  return true;
}

bool AttributeEval::EvalMatrix4d(const UsdPrim& prim, const std::string& attr_name,
                                  double* out16) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const double* v = result.value.as_matrix4d();
  if (!v) return false;
  std::memcpy(out16, v, sizeof(double) * 16);
  return true;
}

std::optional<std::string> AttributeEval::EvalString(const UsdPrim& prim,
                                                      const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const std::string* v = result.value.as_string();
  return v ? std::optional<std::string>(*v) : std::nullopt;
}

std::optional<std::string> AttributeEval::EvalToken(const UsdPrim& prim,
                                                     const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const std::string* v = result.value.as_token();
  return v ? std::optional<std::string>(*v) : std::nullopt;
}

std::optional<std::string> AttributeEval::EvalAssetPath(const UsdPrim& prim,
                                                         const std::string& attr_name) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return std::nullopt;
  const std::string* v = result.value.as_asset_path();
  return v ? std::optional<std::string>(*v) : std::nullopt;
}

bool AttributeEval::EvalFloatArray(const UsdPrim& prim, const std::string& attr_name,
                                    std::vector<float>* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const std::vector<float>* v = result.value.as_float_array();
  if (!v) return false;
  *out = *v;
  return true;
}

bool AttributeEval::EvalIntArray(const UsdPrim& prim, const std::string& attr_name,
                                  std::vector<int32_t>* out) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) return false;
  const std::vector<int32_t>* v = result.value.as_int_array();
  if (!v) return false;
  *out = *v;
  return true;
}

// ============================================================
// Connection resolution
// ============================================================

bool AttributeEval::HasConnection(const UsdPrim& prim, const std::string& attr_name) const {
  if (!prim.IsValid()) return false;
  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return false;

  // Connection targets are parsed into the PrimSpec connection map (from the
  // crate "connectionPaths" field), keyed by the bare attribute name.
  const std::vector<Path>* targets = spec->connection(attr_name);
  if (targets && !targets->empty()) return true;

  // Fall back to the connection flag on the property slot.
  const PropSlot* slot = spec->property(attr_name);
  return slot && slot->is_connection();
}

std::string AttributeEval::GetConnectionPath(const UsdPrim& prim,
                                              const std::string& attr_name) const {
  if (!prim.IsValid()) return "";
  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return "";

  // The connection target is stored in the PrimSpec connection map (not as the
  // attribute's value), keyed by the bare attribute name.
  const std::vector<Path>* targets = spec->connection(attr_name);
  if (targets && !targets->empty()) return (*targets)[0].str();
  return "";
}

UsdPrim AttributeEval::ResolveConnectionToPrim(const UsdPrim& prim,
                                                const std::string& attr_name) const {
  std::string conn_path = GetConnectionPath(prim, attr_name);
  if (conn_path.empty() || !stage_) return UsdPrim();

  // Extract prim path (before the dot)
  size_t dot_pos = conn_path.rfind('.');
  if (dot_pos == std::string::npos) {
    return stage_->GetPrimAtPath(conn_path);
  }

  std::string prim_path = conn_path.substr(0, dot_pos);
  return stage_->GetPrimAtPath(prim_path);
}

EvalResult AttributeEval::ResolveConnection(const UsdPrim& prim,
                                             const std::string& attr_name) const {
  std::string conn_path = GetConnectionPath(prim, attr_name);
  if (conn_path.empty()) {
    return EvalResult{};
  }

  return FollowConnection(conn_path, options_, 0);
}

// ============================================================
// Convenience functions
// ============================================================

EvalResult EvalAttribute(const Stage& stage, const UsdPrim& prim,
                         const std::string& attr_name, double time) {
  AttributeEval eval(&stage);
  return eval.EvalAt(prim, attr_name, time);
}

namespace {
// Lazy eval worker: resolve `attr` on `prim_path` via the cache, following
// connections (depth-bounded) across lazily-composed prims.
EvalResult EvalAttributeLazyRec(pcp::Cache& cache, const Path& prim_path,
                                const std::string& attr_name,
                                const EvalOptions& opts, int depth) {
  EvalResult result;
  std::string w, e;
  const PrimSpec* spec = cache.ComposePrim(prim_path, &w, &e);
  if (!spec) return result;

  // Connection following: a connected attribute forwards to its target's value
  // (the canonical connection() targets are populated by CopyLocalOpinions).
  if (opts.follow_connections && depth < opts.max_connection_depth) {
    if (const std::vector<Path>* conns = spec->connection(attr_name)) {
      if (!conns->empty()) {
        const Path& target = conns->front();
        Path tprim = target.prim_path();
        std::string tattr = target.property_name();
        if (!tprim.str().empty() && !tattr.empty()) {
          EvalResult cr =
              EvalAttributeLazyRec(cache, tprim, tattr, opts, depth + 1);
          if (cr.success) {
            cr.from_connection = true;
            return cr;
          }
        }
      }
    }
  }

  result = AttributeEval::EvalFromPrimSpec(spec, attr_name, opts);
  if (result.success) result.source_path = prim_path.str();
  return result;
}
}  // namespace

EvalResult EvalAttributeLazy(pcp::Cache& cache, const Path& prim_path,
                             const std::string& attr_name,
                             const EvalOptions& opts) {
  return EvalAttributeLazyRec(cache, prim_path, attr_name, opts, 0);
}

bool GetFloat(const Stage& stage, const UsdPrim& prim,
              const std::string& name, float* out, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  auto result = eval.EvalFloat(prim, name);
  if (result) {
    *out = *result;
    return true;
  }
  return false;
}

bool GetFloat3(const Stage& stage, const UsdPrim& prim,
               const std::string& name, float* out3, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalFloat3(prim, name, out3);
}

bool GetDouble(const Stage& stage, const UsdPrim& prim,
               const std::string& name, double* out, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  auto result = eval.EvalDouble(prim, name);
  if (result) {
    *out = *result;
    return true;
  }
  return false;
}

bool GetDouble3(const Stage& stage, const UsdPrim& prim,
                const std::string& name, double* out3, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalDouble3(prim, name, out3);
}

bool GetMatrix4f(const Stage& stage, const UsdPrim& prim,
                 const std::string& name, float* out16, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalMatrix4f(prim, name, out16);
}

bool GetMatrix4d(const Stage& stage, const UsdPrim& prim,
                 const std::string& name, double* out16, double time) {
  AttributeEval eval(&stage);
  eval.SetTime(time);
  return eval.EvalMatrix4d(prim, name, out16);
}

}  // namespace next
}  // namespace lightusd
