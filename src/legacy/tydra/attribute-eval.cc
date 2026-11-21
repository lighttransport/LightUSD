// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment, Inc.
//
#include "attribute-eval.hh"
#include "scene-access.hh"

#include "common-macros.inc"
#include "layer.hh"
#include "pprint-enum.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "value-clip-utils.hh"
#include "value-pprint.hh"
#include "core/spline-eval.hh"

#include <memory>
#include <mutex>

namespace lightusd { namespace legacy {
// Shared spline evaluator (core/spline-eval.hh).
namespace tydra { using namespace ::lightusd::spline_eval; }  // NOLINT
namespace tydra {

// For PUSH_ERROR_AND_RETURN
#define PushError(msg) \
  if (err) {           \
    (*err) +=  msg;     \
  }

namespace {

namespace {

// Build a typed Spline<T> from the type-erased SplineData and evaluate at `t`.
template <typename T>
bool BuildAndEvalSpline(const primvar::PrimVar::SplineData &sd, double t,
                        T *out) {
  Spline<T> s;
  s.curveType =
      (sd.curveType == 1) ? SplineCurveType::Hermite : SplineCurveType::Bezier;
  s.preExtrapolation =
      static_cast<SplineExtrapolationMode>(sd.preExtrapolation);
  s.postExtrapolation =
      static_cast<SplineExtrapolationMode>(sd.postExtrapolation);
  s.preExtrapolationSlope = sd.preExtrapolationSlope;
  s.postExtrapolationSlope = sd.postExtrapolationSlope;
  if (sd.hasLoop) {
    s.loopParams.protoStart = sd.loopProtoStart;
    s.loopParams.protoEnd = sd.loopProtoEnd;
    s.loopParams.numPreLoops = sd.loopNumPreLoops;
    s.loopParams.numPostLoops = sd.loopNumPostLoops;
    s.loopParams.valueOffset = sd.loopValueOffset;
  }
  s.knots.reserve(sd.knots.size());

  for (const auto &kd : sd.knots) {
    SplineKnot<T> k;
    k.time = kd.time;
    auto v = kd.val.get_value<T>();
    if (!v) {
      return false;  // knot value type does not match T
    }
    k.value = v.value();
    if (kd.hasDualValue) {
      auto pv = kd.preValue.get_value<T>();
      k.preValue = pv ? pv.value() : k.value;
      k.hasDualValue = true;
    } else {
      k.preValue = k.value;
    }
    k.preTangentSlope = kd.preTangentSlope;
    k.preTangentWidth = kd.preTangentWidth;
    k.postTangentSlope = kd.postTangentSlope;
    k.postTangentWidth = kd.postTangentWidth;
    k.nextInterpolationMode =
        static_cast<SplineInterpolationMode>(kd.interpolationMode);
    s.knots.push_back(k);
  }

  return EvaluateSpline<T>(s, t, out);
}

// Evaluate a type-erased spline at `t`, dispatching on the knot value type.
// USD spline value types are scalar double / float / half.
bool EvalSplineData(const primvar::PrimVar::SplineData &sd, double t,
                    value::Value *out) {
  if (sd.knots.empty()) {
    return false;
  }
  const uint32_t tid = sd.knots[0].val.type_id();
  if (tid == value::TypeTraits<double>::type_id()) {
    double r;
    if (BuildAndEvalSpline<double>(sd, t, &r)) {
      *out = value::Value(r);
      return true;
    }
  } else if (tid == value::TypeTraits<float>::type_id()) {
    float r;
    if (BuildAndEvalSpline<float>(sd, t, &r)) {
      *out = value::Value(r);
      return true;
    }
  } else if (tid == value::TypeTraits<value::half>::type_id()) {
    // half has no direct double cast in the evaluator; evaluate in float and
    // convert back.
    Spline<float> s;
    s.curveType = (sd.curveType == 1) ? SplineCurveType::Hermite
                                      : SplineCurveType::Bezier;
    s.preExtrapolation =
        static_cast<SplineExtrapolationMode>(sd.preExtrapolation);
    s.postExtrapolation =
        static_cast<SplineExtrapolationMode>(sd.postExtrapolation);
    s.preExtrapolationSlope = sd.preExtrapolationSlope;
    s.postExtrapolationSlope = sd.postExtrapolationSlope;
    if (sd.hasLoop) {
      s.loopParams.protoStart = sd.loopProtoStart;
      s.loopParams.protoEnd = sd.loopProtoEnd;
      s.loopParams.numPreLoops = sd.loopNumPreLoops;
      s.loopParams.numPostLoops = sd.loopNumPostLoops;
      s.loopParams.valueOffset = sd.loopValueOffset;
    }
    bool ok = true;
    for (const auto &kd : sd.knots) {
      auto v = kd.val.get_value<value::half>();
      if (!v) {
        ok = false;
        break;
      }
      SplineKnot<float> k;
      k.time = kd.time;
      k.value = value::half_to_float(v.value());
      if (kd.hasDualValue) {
        auto pv = kd.preValue.get_value<value::half>();
        k.preValue = pv ? value::half_to_float(pv.value()) : k.value;
        k.hasDualValue = true;
      } else {
        k.preValue = k.value;
      }
      k.preTangentSlope = kd.preTangentSlope;
      k.preTangentWidth = kd.preTangentWidth;
      k.postTangentSlope = kd.postTangentSlope;
      k.postTangentWidth = kd.postTangentWidth;
      k.nextInterpolationMode =
          static_cast<SplineInterpolationMode>(kd.interpolationMode);
      s.knots.push_back(k);
    }
    float r;
    if (ok && EvaluateSpline<float>(s, t, &r)) {
      *out = value::Value(value::float_to_half_full(r));
      return true;
    }
  }
  return false;
}

}  // namespace

bool ToTerminalAttributeValue(
    const Attribute &attr, TerminalAttributeValue *value, std::string *err,
    const double t, const value::TimeSampleInterpolationType tinterp) {
  if (!value) {
    // ???
    return false;
  }

  if (attr.is_blocked()) {
    PUSH_ERROR_AND_RETURN("Attribute is None(Value Blocked).");
  }

  const primvar::PrimVar &var = attr.get_var();

  value->meta() = attr.metas();
  value->variability() = attr.variability();

  DCOUT("var has_default " << var.has_default());
  DCOUT("var has_timesamples " << var.has_default());
  DCOUT("var is_blocked " << var.is_blocked());
  DCOUT("var has_value||has_ts " << (var.has_value() || var.has_timesamples()));

  if (!var.has_value() && !var.has_timesamples() && !var.has_spline()) {
    PUSH_ERROR_AND_RETURN("[InternalError] Attribute is invalid.");
  }

  // AOUSD Core Spec 12.3: Value resolution priority:
  //   timeSamples > spline > default > clips > fallback
  //
  // When time is specified (not default time):
  //   1. If timeSamples exist, interpolate at time t
  //   2. Else if default exists, return default (time ignored)
  //
  // When time is default:
  //   1. Return default if it exists
  //   2. Else return first timeSample value (held at default time)

  bool isDefaultTime = value::TimeCode(t).is_default();

  if (isDefaultTime) {
    // Default time: prefer default value, fall back to timeSamples
    if (var.has_value()) {
      const value::Value &v = var.value_raw();
      DCOUT("Attribute is scalar type:" << v.type_name());
      value->set_value(v);
    } else if (var.has_timesamples()) {
      // No default, use timeSamples at default time (held behavior)
      value::Value v;
      if (!var.get_interpolated_value(t, tinterp, &v)) {
        PUSH_ERROR_AND_RETURN("Interpolate TimeSamples at default time failed.");
        return false;
      }
      value->set_value(v);
    }
  } else {
    // Specific time: prefer timeSamples, fall back to spline, then default
    if (var.has_timesamples()) {
      value::Value v;
      if (!var.get_interpolated_value(t, tinterp, &v)) {
        PUSH_ERROR_AND_RETURN("Interpolate TimeSamples failed.");
        return false;
      }
      value->set_value(v);
    } else if (var.has_spline()) {
      // AOUSD Core Spec 12.3 / 7.4.2.4: Spline is second priority after
      // timeSamples. Evaluate the cubic (Bezier/Hermite) spline at time `t`
      // via typed dispatch into EvaluateSpline<T> (spline-eval.hh).
      const auto &spline = var.spline_data();
      value::Value sv;
      if (EvalSplineData(spline, t, &sv)) {
        value->set_value(sv);
      } else if (!spline.knots.empty()) {
        // Value-block segment or unsupported value type: fall back to the
        // nearest authored knot value (held behaviour).
        size_t idx = 0;
        for (size_t i = 0; i < spline.knots.size(); i++) {
          if (spline.knots[i].time <= t) {
            idx = i;
          } else {
            break;
          }
        }
        const auto &knot = spline.knots[idx];
        if (knot.hasDualValue && t < knot.time) {
          value->set_value(knot.preValue);
        } else {
          value->set_value(knot.val);
        }
      }
    } else if (var.has_value()) {
      // No timeSamples or spline: return default regardless of requested time
      const value::Value &v = var.value_raw();
      DCOUT("No timeSamples, returning default value");
      value->set_value(v);
    }
  }

  return true;
}

// AOUSD Core Spec 12.3.4: Evaluate attribute from value clips.
// This is the fallback when an attribute has neither timeSamples nor default value.
// Loads the active clip asset, finds the target prim, and queries the attribute.
//
// Simple clip asset cache to avoid reloading the same file repeatedly.
static constexpr size_t kMaxClipCacheEntries = 64;

static std::map<std::string, std::shared_ptr<Layer>> &GetClipCache() {
  static std::map<std::string, std::shared_ptr<Layer>> s_clip_cache;
  return s_clip_cache;
}

static std::mutex &GetClipCacheMutex() {
  static std::mutex s_clip_cache_mu;
  return s_clip_cache_mu;
}

static std::vector<std::string> GetClipPrimPathCandidates(
    const std::string &primPath, const Path &fallback_prim_path) {
  std::vector<std::string> candidates;

  std::string p = primPath.empty() ? fallback_prim_path.full_path_name() : primPath;
  if (!p.empty()) {
    candidates.push_back(p);
  }

  if (!p.empty() && p[0] != '/') {
    std::string rel = p;
    if (rel == ".") {
      rel.clear();
    } else if (rel.size() >= 2 && rel[0] == '.' && rel[1] == '/') {
      rel = rel.substr(2);
    }

    if (fallback_prim_path.is_valid()) {
      Path parent = fallback_prim_path.get_parent_prim_path();
      if (parent.is_valid()) {
        std::string parent_path = parent.full_path_name();
        if (!parent_path.empty() && parent_path != "/") {
          if (parent_path.back() != '/') {
            parent_path.push_back('/');
          }
          if (!rel.empty()) {
            candidates.push_back(parent_path + rel);
          } else {
            candidates.push_back(parent_path);
          }
        } else if (!rel.empty()) {
          candidates.push_back(std::string("/") + rel);
        }
      }
    }
  }

  if (candidates.empty() && fallback_prim_path.is_valid()) {
    candidates.push_back(fallback_prim_path.full_path_name());
  }

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

// Helper: load a clip layer from file with caching.
static std::shared_ptr<Layer> LoadClipLayer(const std::string &clipAssetPath) {
  {
    std::lock_guard<std::mutex> lock(GetClipCacheMutex());
    auto &cache = GetClipCache();
    auto cache_it = cache.find(clipAssetPath);
    if (cache_it != cache.end()) {
      return cache_it->second;
    }
  }

  Layer loaded_layer;
  std::string warn, load_err;
  if (LoadLayerFromFile(clipAssetPath, &loaded_layer, &warn, &load_err)) {
    auto loaded = std::make_shared<Layer>(std::move(loaded_layer));
    std::lock_guard<std::mutex> lock(GetClipCacheMutex());
    auto &cache = GetClipCache();
    if (cache.size() >= kMaxClipCacheEntries) {
      cache.erase(cache.begin());
    }
    cache[clipAssetPath] = loaded;
    return loaded;
  }

  DCOUT("Failed to load clip asset: " << clipAssetPath << " : " << load_err);
  return {};
}

// Helper: query an attribute from a clip layer at a given time.
static bool QueryClipAttribute(
    const Layer *clip_layer, const std::string &primPath,
    const Path &fallback_prim_path,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, double clipTime,
    value::TimeSampleInterpolationType tinterp) {
  if (!clip_layer) {
    return false;
  }

  const std::vector<std::string> candidates =
      GetClipPrimPathCandidates(primPath, fallback_prim_path);

  const PrimSpec *target_ps = nullptr;
  std::string find_err;
  for (const auto &candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
    Path clip_prim_path(candidate, "");
    if (clip_prim_path.is_valid() &&
        clip_layer->find_primspec_at(clip_prim_path, &target_ps, &find_err)) {
      break;
    }
  }

  if (target_ps) {
    const auto &props = target_ps->props();
    auto prop_it = props.find(attr_name);
    if (prop_it != props.end() && prop_it->second.is_attribute()) {
      const Attribute &clip_attr = prop_it->second.get_attribute();
      if (ToTerminalAttributeValue(clip_attr, value, err, clipTime, tinterp)) {
        return true;
      }
    }
  }

  // Fallback using Stage conversion in case primspec lookup misses
  // some metadata representation variants.
  if (!candidates.empty()) {
    Layer layer_copy = *clip_layer;
    Stage clip_stage;
    std::string warn;
    std::string layer_err;
    if (LayerToStage(std::move(layer_copy), &clip_stage, &warn, &layer_err)) {
      const Prim *clip_prim = nullptr;
      for (const auto &candidate : candidates) {
        if (candidate.empty()) {
          continue;
        }
        Path clip_prim_path(candidate, "");
        std::string prim_find_err;
        if (clip_prim_path.is_valid() &&
            clip_stage.find_prim_at_path(clip_prim_path, clip_prim,
                                         &prim_find_err)) {
          break;
        }
      }

      if (!clip_prim) {
        return false;
      }

      Property prop;
      std::string prim_get_err;
      if (!GetProperty(*clip_prim, attr_name, &prop, &prim_get_err)) {
        return false;
      }
      if (!prop.is_attribute()) {
        return false;
      }

      const Attribute &prim_attr = prop.get_attribute();
      return ToTerminalAttributeValue(prim_attr, value, err, clipTime, tinterp);
    }
  }

  return false;
}

// Forward declaration: resolve one attribute from a single clip set (defined
// after EvaluateAttributeFromClipsImpl, which iterates over all clip sets).
static bool ResolveAttrFromClipSet(
    const Prim &prim,
    const ClipSetMetadata &clipMeta,
    const std::string &attr_name,
    TerminalAttributeValue *value,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp);

bool EvaluateAttributeFromClipsImpl(
    const Prim &prim,
    const std::string &attr_name,
    TerminalAttributeValue *value,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {

  if (!prim.metas().has_clips()) {
    return false;
  }

  Dictionary clips_dict = prim.metas().get_clips();
  if (clips_dict.empty()) {
    return false;
  }

  // AOUSD Core Spec 12.3.4.1.1: a prim may author multiple named clip sets.
  // Parse every set, then consult them in order; the first set that resolves a
  // value for this attribute at time `t` wins. This lets different sets supply
  // different attributes (or cover different time ranges).
  std::vector<ClipSetMetadata> clipSets;
  if (!ParseAllClipSetMetadata(clips_dict, &clipSets, err)) {
    return false;
  }

  for (const ClipSetMetadata &clipMeta : clipSets) {
    if (clipMeta.assetPaths.empty()) {
      continue;
    }
    if (ResolveAttrFromClipSet(prim, clipMeta, attr_name, value, err, t,
                               tinterp)) {
      return true;
    }
  }

  return false;
}

// Resolve `attr_name` at stage time `t` from a single clip set. Returns true if
// a value was produced. Factored out of EvaluateAttributeFromClipsImpl so that
// multiple clip sets can be tried in order.
static bool ResolveAttrFromClipSet(
    const Prim &prim,
    const ClipSetMetadata &clipMeta,
    const std::string &attr_name,
    TerminalAttributeValue *value,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {

  // AOUSD Core Spec 12.3.4.2: Manifest-based attribute discovery.
  // If a manifest is provided, verify the attribute exists before loading clips.
  if (!clipMeta.manifestAssetPath.empty()) {
    std::shared_ptr<Layer> manifest = LoadClipLayer(clipMeta.manifestAssetPath);
    if (manifest) {
      const PrimSpec *manifest_ps = nullptr;
      const std::vector<std::string> candidates =
          GetClipPrimPathCandidates(clipMeta.primPath, prim.element_path());
      std::string find_err;
      for (const auto &candidate : candidates) {
        if (candidate.empty()) {
          continue;
        }
        Path manifest_path(candidate, "");
        if (manifest_path.is_valid() &&
            manifest->find_primspec_at(manifest_path, &manifest_ps, &find_err)) {
          break;
        }
      }

      if (manifest_ps) {
        // Check if the requested attribute exists in the manifest
        if (manifest_ps->props().find(attr_name) == manifest_ps->props().end()) {
          DCOUT("Attribute " << attr_name << " not listed in clip manifest");
          return false;  // Attribute not available in clips
        }
      }
    }
  }

  // Resolve which clip and time to use
  std::string clipAssetPath;
  double clipTime = 0;
  if (!ResolveValueClipQuery(clipMeta.active, clipMeta.times,
                              clipMeta.assetPaths, t,
                              &clipAssetPath, &clipTime)) {
    return false;
  }

  DCOUT("Clip query: asset=" << clipAssetPath << " clipTime=" << clipTime);

  // Load the clip asset (with caching)
  std::shared_ptr<Layer> clip_layer = LoadClipLayer(clipAssetPath);

  if (clip_layer) {
    if (QueryClipAttribute(clip_layer.get(), clipMeta.primPath, prim.element_path(), attr_name,
                            value, err, clipTime, tinterp)) {
      return true;
    }
  }

  // AOUSD Core Spec 12.3.4.6: interpolateMissingClipValues
  // If the active clip is unavailable and this flag is true, try to
  // interpolate from the nearest available adjacent clips.
  if (clipMeta.interpolateMissingClipValues && clipMeta.active.size() >= 2) {
    // Find the previous and next available clips
    int activeIdx = FindActiveClipIndex(clipMeta.active, t);
    int prevIdx = -1, nextIdx = -1;

    // Search backward for previous available clip
    for (int i = activeIdx - 1; i >= 0; i--) {
      int assetIdx = clipMeta.active[static_cast<size_t>(i)].second;
      if (assetIdx >= 0 && assetIdx < static_cast<int>(clipMeta.assetPaths.size())) {
        std::shared_ptr<Layer> prev = LoadClipLayer(clipMeta.assetPaths[static_cast<size_t>(assetIdx)]);
        if (prev) { prevIdx = i; break; }
      }
    }

    // Search forward for next available clip
    for (size_t i = static_cast<size_t>(activeIdx) + 1; i < clipMeta.active.size(); i++) {
      int assetIdx = clipMeta.active[i].second;
      if (assetIdx >= 0 && assetIdx < static_cast<int>(clipMeta.assetPaths.size())) {
        std::shared_ptr<Layer> next = LoadClipLayer(clipMeta.assetPaths[static_cast<size_t>(assetIdx)]);
        if (next) { nextIdx = static_cast<int>(i); break; }
      }
    }

    // If we found both neighbors, return value from the nearest one
    // (Full interpolation between two clips requires type-aware lerp
    // which is complex; for now return the nearest available value)
    if (prevIdx >= 0) {
      int assetIdx = clipMeta.active[static_cast<size_t>(prevIdx)].second;
      std::shared_ptr<Layer> prev = LoadClipLayer(clipMeta.assetPaths[static_cast<size_t>(assetIdx)]);
      double prevClipTime = RemapStageTimeToClipTime(clipMeta.times,
          clipMeta.active[static_cast<size_t>(prevIdx)].first);
      if (prev &&
          QueryClipAttribute(prev.get(), clipMeta.primPath, prim.element_path(),
                            attr_name, value, err, prevClipTime,
                            tinterp)) {
        return true;
      }
    }

    if (nextIdx >= 0) {
      int assetIdx = clipMeta.active[static_cast<size_t>(nextIdx)].second;
      std::shared_ptr<Layer> next = LoadClipLayer(clipMeta.assetPaths[static_cast<size_t>(assetIdx)]);
      double nextClipTime = RemapStageTimeToClipTime(clipMeta.times,
          clipMeta.active[static_cast<size_t>(nextIdx)].first);
      if (next &&
          QueryClipAttribute(next.get(), clipMeta.primPath, prim.element_path(),
                            attr_name, value, err, nextClipTime,
                            tinterp)) {
        return true;
      }
  }
  }

  return false;
}

//
// visited_paths : To prevent circular referencing of attribute connection.
//
bool EvaluateAttributeImpl(
    const lightusd::legacy::Stage &stage, const lightusd::legacy::Prim &prim,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, std::set<std::string> &visited_paths, const double t,
    const lightusd::legacy::value::TimeSampleInterpolationType tinterp) {

  // Iterative connection-following loop (replaces tail recursion)
  const Prim *current_prim = &prim;
  std::string current_attr_name = attr_name;
  constexpr size_t kMaxConnectionChain = 1024;

  for (size_t iter = 0; iter < kMaxConnectionChain; ++iter) {
    DCOUT("Prim : " << current_prim->element_path().element_name() << "("
                    << current_prim->type_name() << ") attr_name " << current_attr_name);

    Property prop;
    if (!GetProperty(*current_prim, current_attr_name, &prop, err)) {
      DCOUT("Get property failed: " << current_attr_name);
      return false;
    }

    if (prop.is_attribute_connection()) {
      // Follow connection target Path(single targetPath only).
      std::vector<Path> pv = prop.get_attribute().connections();
      Path target;
      if (!detail::ResolveSingleConnectionTargetPath(pv, current_attr_name, &target,
                                                     err)) {
        return false;
      }

      std::string targetPrimPath = target.prim_part();
      std::string targetPrimPropName = target.prop_part();
      DCOUT("connection targetPath : " << target << "(Prim: " << targetPrimPath
                                       << ", Prop: " << targetPrimPropName
                                       << ")");

      auto targetPrimRet =
          stage.GetPrimAtPath(Path(targetPrimPath, /* prop */ ""));
      if (targetPrimRet) {
        std::string abs_path = target.full_path_name();

        if (visited_paths.count(abs_path)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Circular referencing detected. connectionTargetPath = {}",
              to_string(target)));
        }
        visited_paths.insert(abs_path);

        // Continue loop with the target prim/attr (iterative tail call)
        current_prim = targetPrimRet.value();
        current_attr_name = targetPrimPropName;
        continue;

      } else {
        PUSH_ERROR_AND_RETURN(targetPrimRet.error());
        return false;
      }
    } else if (prop.is_attribute()) {
      DCOUT("IsAttrib");

      const Attribute &attr = prop.get_attribute();

      if (attr.is_blocked()) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Attribute `{}` is ValueBlocked(None).", current_attr_name));
      }

      if (!ToTerminalAttributeValue(attr, value, err, t, tinterp)) {
        // AOUSD Core Spec 12.3.4: Fallback to value clips
        // If the attribute has no timeSamples/default, try clips.
        if (EvaluateAttributeFromClipsImpl(*current_prim, current_attr_name,
                                          value, err, t, tinterp)) {
          return true;
        }
        return false;
      }

      return true;

    } else if (prop.is_relationship()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Property `{}` is a Relation.", current_attr_name));
    } else if (prop.is_empty()) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Attribute `{}` is a define-only attribute(no value assigned).",
          current_attr_name));
    } else {
      PUSH_ERROR_AND_RETURN(
          fmt::format("[InternalError] Invalid Attribute `{}`.", current_attr_name));
    }
  }

  PUSH_ERROR_AND_RETURN("Connection chain too long (possible cycle).");
  return false;
}

bool EvaluateAttributeImpl(
    const lightusd::legacy::Stage &stage, const lightusd::legacy::Attribute &attr,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, std::set<std::string> &visited_paths, const double t,
    const lightusd::legacy::value::TimeSampleInterpolationType tinterp) {

  if (attr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the value only if the connection cannot be resolved. (is_connection() is
    // false when a value is also present, so dispatch on has_connections().)
    std::string conn_err;
    bool resolved = false;

    // Follow connection target Path(single targetPath only).
    std::vector<Path> pv = attr.connections();
    Path target;
    if (detail::ResolveSingleConnectionTargetPath(pv, attr_name, &target,
                                                  &conn_err)) {
      std::string targetPrimPath = target.prim_part();
      std::string targetPrimPropName = target.prop_part();
      DCOUT("connection targetPath : " << target << "(Prim: " << targetPrimPath
                                       << ", Prop: " << targetPrimPropName
                                       << ")");

      auto targetPrimRet =
          stage.GetPrimAtPath(Path(targetPrimPath, /* prop */ ""));
      if (targetPrimRet) {
        const Prim *targetPrim = targetPrimRet.value();

        std::string abs_path = target.full_path_name();

        if (visited_paths.count(abs_path)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Circular referencing detected. connectionTargetPath = {}",
              to_string(target)));
        }
        visited_paths.insert(abs_path);

        // Delegate to the iterative Prim-based overload
        resolved = EvaluateAttributeImpl(stage, *targetPrim, targetPrimPropName,
                                         value, &conn_err, visited_paths, t,
                                         tinterp);
      } else {
        conn_err += targetPrimRet.error();
      }
    }

    if (resolved) {
      return true;
    }
    // Connection unresolved — fall back to the authored value if present.
    if (attr.has_value() &&
        ToTerminalAttributeValue(attr, value, err, t, tinterp)) {
      return true;
    }
    if (err) {
      (*err) += conn_err;
    }
    return false;

  } else if (attr.is_blocked()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Attribute `{}` is ValueBlocked(None).", attr_name));
  } else {

    if (!ToTerminalAttributeValue(attr, value, err, t, tinterp)) {
      return false;
    }

  }

  return true;
}

}  // namespace

bool EvaluateAttribute(
    const lightusd::legacy::Stage &stage, const lightusd::legacy::Prim &prim,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t,
    const lightusd::legacy::value::TimeSampleInterpolationType tinterp) {
  std::set<std::string> visited_paths;

  return EvaluateAttributeImpl(stage, prim, attr_name, value, err,
                               visited_paths, t, tinterp);
}

bool EvaluateAttributeFromClips(
    const Prim &prim,
    const std::string &attr_name,
    TerminalAttributeValue *value,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {
  return EvaluateAttributeFromClipsImpl(prim, attr_name, value, err, t, tinterp);
}

bool EvaluateAttribute(
    const lightusd::legacy::Stage &stage, const Attribute &attr,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t,
    const lightusd::legacy::value::TimeSampleInterpolationType tinterp) {
  std::set<std::string> visited_paths;

  return EvaluateAttributeImpl(stage, attr, attr_name, value, err,
                               visited_paths, t, tinterp);
}

// Layer/PrimSpec version
bool EvaluateAttribute(
    const lightusd::legacy::Layer &layer, const lightusd::legacy::PrimSpec &ps,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t,
    const lightusd::legacy::value::TimeSampleInterpolationType tinterp) {
  (void)layer;

  if (!value) {
    PUSH_ERROR_AND_RETURN("[InternalError] nullptr value is not allowed.");
  }

  DCOUT("PrimSpec : " << ps.name() << "(" << ps.typeName() << ") attr_name " << attr_name);

  // Look up the property in PrimSpec's properties
  const auto &props = ps.props();
  auto it = props.find(attr_name);
  if (it == props.end()) {
    PUSH_ERROR_AND_RETURN(fmt::format("Attribute `{}` not found in PrimSpec `{}`", attr_name, ps.name()));
  }

  const Property &prop = it->second;

  // Handle different property types
  if (prop.is_attribute_connection()) {
    // For Layer/PrimSpec version, we cannot follow connections
    // since we don't have the full Stage context for path resolution
    PUSH_ERROR_AND_RETURN(fmt::format("Attribute `{}` is a connection. Connection following is not supported in Layer/PrimSpec version of EvaluateAttribute. Use Stage version instead.", attr_name));

  } else if (prop.is_attribute()) {
    DCOUT("IsAttrib");

    const Attribute &attr = prop.get_attribute();

    if (attr.is_blocked()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Attribute `{}` is ValueBlocked(None).", attr_name));
    }

    // Check if this is an empty attribute (type info only)
    if (prop.is_empty()) {
      // For empty attributes, set as empty with type info
      std::string type_name = attr.type_name();
      if (type_name.empty()) {
        type_name = "unknown";
      }
      value->set_empty_attribute(type_name);
      DCOUT("Empty attribute with type: " << type_name);
    } else {
      if (!ToTerminalAttributeValue(attr, value, err, t, tinterp)) {
        return false;
      }
    }

  } else if (prop.is_relationship()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Property `{}` is a Relation.", attr_name));
  } else if (prop.is_empty()) {
    // "empty" attribute - set as empty with type info
    std::string type_name = "unknown"; // Default fallback

    // Try to get type information from the attribute if available
    if (prop.is_attribute()) {
      const Attribute &attr = prop.get_attribute();
      const primvar::PrimVar &var = attr.get_var();
      if (var.has_value() || var.has_timesamples()) {
        type_name = var.type_name();
      }
    }

    value->set_empty_attribute(type_name);
    DCOUT("Empty attribute with type: " << type_name);

  } else {
    // ???
    PUSH_ERROR_AND_RETURN(
        fmt::format("[InternalError] Invalid Property type for `{}`.", attr_name));
  }

  return true;
}

}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd
