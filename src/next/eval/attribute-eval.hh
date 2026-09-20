// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Attribute Evaluation
//
// Provides unified attribute value resolution with:
// - Time sample interpolation
// - Default value fallback
// - Connection following for shaders
// - Type-safe accessors

#pragma once

#include "../types/value.hh"
#include "../types/interpolation.hh"
#include "../stage/stage.hh"
#include "value-clip.hh"
#include <string>
#include <optional>
#include <functional>
#include <memory>

namespace lightusd {
namespace next {

namespace pcp { class Cache; }  // lazy (cache-backed) evaluation entry point
class Path;

/// Explicit USD time query. DefaultTime is semantically distinct from numeric
/// time 0: it consults authored defaults and schema fallbacks, never samples.
class TimeQuery {
 public:
  enum class Kind : uint8_t { Default, Numeric };

  TimeQuery() = default;
  TimeQuery(double time) : kind_(Kind::Numeric), numeric_time_(time) {}

  static TimeQuery Default() {
    TimeQuery query;
    query.kind_ = Kind::Default;
    return query;
  }
  static TimeQuery Numeric(double time) { return TimeQuery(time); }

  bool is_default() const { return kind_ == Kind::Default; }
  bool is_numeric() const { return kind_ == Kind::Numeric; }
  double numeric_time() const { return numeric_time_; }
  Kind kind() const { return kind_; }

 private:
  Kind kind_{Kind::Numeric};
  double numeric_time_{0.0};
};

/// Evaluation options
struct EvalOptions {
  TimeQuery time = TimeQuery::Numeric(0.0);       // DefaultTime or numeric time
  TimeInterpolation interp = TimeInterpolation::Linear;  // Interpolation mode
  bool follow_connections = true;                 // Follow shader connections
  int max_connection_depth = 16;                  // Max connection chain depth
  bool strict_aousd_conformance = false;
  /// Loader used by core value-clip resolution. When absent, compatibility
  /// mode skips clips; strict mode returns an evaluation error.
  std::function<bool(const std::string&, Stage*, std::string*, std::string*)>
      clip_stage_loader;
  /// Optional cache shared across queries. The caller owns invalidation when
  /// assets or the loader's resolution context change.
  std::shared_ptr<ValueClipStageCache> clip_stage_cache;
};

/// Evaluation result with metadata
struct EvalResult {
  Value value;
  bool success = false;
  bool from_time_sample = false;    // Value came from time sample
  bool from_default = false;        // Value came from default
  bool from_connection = false;     // Value resolved via connection
  bool from_schema_fallback = false;
  /// An authored value block participated in resolution. The block itself is
  /// never returned as a consumer value; success may still be true when a
  /// schema fallback resolves beneath it.
  bool blocked = false;
  bool interpolated = false;        // Value was interpolated
  std::string source_path;          // Path where value was found
  std::string source_asset;         // Value-clip asset, when applicable
  std::string source_clip_set;      // Named clip set, when applicable
  ValueClipResolutionInfo clip_resolution;
  std::string error;
};

/// AttributeEval - attribute value evaluation
class AttributeEval {
public:
  explicit AttributeEval(const Stage* stage);

  /// Set evaluation time
  void SetTime(double time) { options_.time = TimeQuery::Numeric(time); }
  void SetDefaultTime() { options_.time = TimeQuery::Default(); }
  TimeQuery GetTimeQuery() const { return options_.time; }
  double GetTime() const { return options_.time.numeric_time(); }

  /// Set interpolation mode
  void SetInterpolation(TimeInterpolation mode) { options_.interp = mode; }

  /// Set connection following
  void SetFollowConnections(bool follow) { options_.follow_connections = follow; }

  /// Get current options
  const EvalOptions& GetOptions() const { return options_; }
  void SetOptions(const EvalOptions& opts) { options_ = opts; }

  // ============================================================
  // Main evaluation methods
  // ============================================================

  /// Evaluate attribute and return result with metadata
  EvalResult Eval(const UsdPrim& prim, const std::string& attr_name) const;

  /// Evaluate attribute at specific time
  EvalResult EvalAt(const UsdPrim& prim, const std::string& attr_name, double time) const;

  /// Evaluate with custom options
  EvalResult EvalWith(const UsdPrim& prim, const std::string& attr_name,
                      const EvalOptions& options) const;

  // ============================================================
  // Type-safe evaluation (returns std::optional)
  // ============================================================

  std::optional<bool> EvalBool(const UsdPrim& prim, const std::string& attr_name) const;
  std::optional<int32_t> EvalInt(const UsdPrim& prim, const std::string& attr_name) const;
  std::optional<float> EvalFloat(const UsdPrim& prim, const std::string& attr_name) const;
  std::optional<double> EvalDouble(const UsdPrim& prim, const std::string& attr_name) const;

  // Vector types (return false if not found, fill output arrays)
  bool EvalFloat2(const UsdPrim& prim, const std::string& attr_name, float* out) const;
  bool EvalFloat3(const UsdPrim& prim, const std::string& attr_name, float* out) const;
  bool EvalFloat4(const UsdPrim& prim, const std::string& attr_name, float* out) const;
  bool EvalDouble2(const UsdPrim& prim, const std::string& attr_name, double* out) const;
  bool EvalDouble3(const UsdPrim& prim, const std::string& attr_name, double* out) const;
  bool EvalDouble4(const UsdPrim& prim, const std::string& attr_name, double* out) const;

  // Matrix types
  bool EvalMatrix3f(const UsdPrim& prim, const std::string& attr_name, float* out9) const;
  bool EvalMatrix4f(const UsdPrim& prim, const std::string& attr_name, float* out16) const;
  bool EvalMatrix3d(const UsdPrim& prim, const std::string& attr_name, double* out9) const;
  bool EvalMatrix4d(const UsdPrim& prim, const std::string& attr_name, double* out16) const;

  // String types
  std::optional<std::string> EvalString(const UsdPrim& prim, const std::string& attr_name) const;
  std::optional<std::string> EvalToken(const UsdPrim& prim, const std::string& attr_name) const;
  std::optional<std::string> EvalAssetPath(const UsdPrim& prim, const std::string& attr_name) const;

  // Array types
  bool EvalFloatArray(const UsdPrim& prim, const std::string& attr_name,
                      std::vector<float>* out) const;
  bool EvalIntArray(const UsdPrim& prim, const std::string& attr_name,
                    std::vector<int32_t>* out) const;

  // ============================================================
  // Connection resolution
  // ============================================================

  /// Check if attribute has a connection
  bool HasConnection(const UsdPrim& prim, const std::string& attr_name) const;

  /// Get connection target path (e.g., "/Materials/Mat1/Shader.outputs:out")
  std::string GetConnectionPath(const UsdPrim& prim, const std::string& attr_name) const;

  /// Resolve connection and return the connected prim
  UsdPrim ResolveConnectionToPrim(const UsdPrim& prim, const std::string& attr_name) const;

  /// Resolve connection chain and get final value
  EvalResult ResolveConnection(const UsdPrim& prim, const std::string& attr_name) const;

  // ============================================================
  // Fallback value support
  // ============================================================

  /// Evaluate with fallback value
  template<typename T>
  T EvalOr(const UsdPrim& prim, const std::string& attr_name, T fallback) const;

  /// Resolve a single attribute's value from one composed PrimSpec: time
  /// samples (interpolated at opts.time) first, else the authored default.
  /// Pure (no stage/connection state) so it is shared by the stage-based and
  /// the cache-based (lazy) evaluators.
  static EvalResult EvalFromPrimSpec(const PrimSpec* spec,
                                     const std::string& attr_name,
                                     const EvalOptions& opts);

private:
  const Stage* stage_;
  EvalOptions options_;

  // Internal helpers
  EvalResult EvalInternal(const UsdPrim& prim, const std::string& attr_name,
                          const EvalOptions& opts, int depth) const;
  EvalResult FollowConnection(const std::string& connection_path,
                              const EvalOptions& opts, int depth) const;
};

// ============================================================
// Convenience functions (use default evaluation options)
// ============================================================

/// Evaluate attribute with time sample interpolation
EvalResult EvalAttribute(const Stage& stage, const UsdPrim& prim,
                         const std::string& attr_name, double time = 0.0);

/// Phase 10: lazily evaluate `attr_name` on the prim at `prim_path` by composing
/// only the prims it touches through the pcp cache (no BuildStage). Resolves
/// time samples / default value via EvalFromPrimSpec and follows connections
/// across lazily-composed prims (the canonical `connection()` targets, bounded
/// by opts.max_connection_depth). Returns an unsuccessful EvalResult if the
/// prim or attribute authors no value.
EvalResult EvalAttributeLazy(pcp::Cache& cache, const Path& prim_path,
                             const std::string& attr_name,
                             const EvalOptions& opts = EvalOptions());

/// Quick type-safe getters (return false if not found)
bool GetFloat(const Stage& stage, const UsdPrim& prim,
              const std::string& name, float* out, double time = 0.0);
bool GetFloat3(const Stage& stage, const UsdPrim& prim,
               const std::string& name, float* out3, double time = 0.0);
bool GetDouble(const Stage& stage, const UsdPrim& prim,
               const std::string& name, double* out, double time = 0.0);
bool GetDouble3(const Stage& stage, const UsdPrim& prim,
                const std::string& name, double* out3, double time = 0.0);
bool GetMatrix4f(const Stage& stage, const UsdPrim& prim,
                 const std::string& name, float* out16, double time = 0.0);
bool GetMatrix4d(const Stage& stage, const UsdPrim& prim,
                 const std::string& name, double* out16, double time = 0.0);

// ============================================================
// Template implementation
// ============================================================

template<typename T>
T AttributeEval::EvalOr(const UsdPrim& prim, const std::string& attr_name, T fallback) const {
  EvalResult result = Eval(prim, attr_name);
  if (!result.success) {
    return fallback;
  }

  // Type dispatch based on T
  if constexpr (std::is_same_v<T, bool>) {
    const bool* v = result.value.as_bool();
    return v ? *v : fallback;
  } else if constexpr (std::is_same_v<T, int32_t>) {
    const int32_t* v = result.value.as_int();
    return v ? *v : fallback;
  } else if constexpr (std::is_same_v<T, float>) {
    const float* v = result.value.as_float();
    return v ? *v : fallback;
  } else if constexpr (std::is_same_v<T, double>) {
    const double* v = result.value.as_double();
    return v ? *v : fallback;
  } else if constexpr (std::is_same_v<T, std::string>) {
    const std::string* v = result.value.as_string();
    if (v) return *v;
    v = result.value.as_token();
    return v ? *v : fallback;
  } else {
    return fallback;
  }
}

}  // namespace next
}  // namespace lightusd
