// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Tydra Next - Skeleton and animation conversion
#include "render-converter.hh"
#include "next/schema/usd-skel.hh"
#include "next/eval/value-clip.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::Stage; using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {
bool ValueToFloat4(const Value& value, Float4* out) {
  if (!out || value.is_empty() || value.is_array()) return false;

  if (const float* v = value.as_float()) {
    *out = Float4(*v, 0.0f, 0.0f, 0.0f);
    return true;
  }
  if (const double* v = value.as_double()) {
    *out = Float4(static_cast<float>(*v), 0.0f, 0.0f, 0.0f);
    return true;
  }
  if (const float* v = value.as_float3()) {
    *out = Float4(v[0], v[1], v[2], 0.0f);
    return true;
  }
  if (const double* v = value.as_double3()) {
    *out = Float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                  static_cast<float>(v[2]), 0.0f);
    return true;
  }
  if (const float* v = value.as_float4()) {
    *out = Float4(v[0], v[1], v[2], v[3]);
    return true;
  }
  if (const double* v = value.as_double4()) {
    *out = Float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                  static_cast<float>(v[2]), static_cast<float>(v[3]));
    return true;
  }
  // Authored half-precision scalars (half3 rotate/scale, quath orient, ...)
  // store raw half-bit lanes; the converting reads widen them.
  float h[4];
  if (value.to_float3(h)) {
    *out = Float4(h[0], h[1], h[2], 0.0f);
    return true;
  }
  if (value.to_float4(h)) {
    *out = Float4(h[0], h[1], h[2], h[3]);
    return true;
  }
  if (value.to_float(h)) {
    *out = Float4(h[0], 0.0f, 0.0f, 0.0f);
    return true;
  }
  return false;
}

// Closed-form Euler-degrees -> quaternion (xyzw) for all six USD rotation
// orders (rotateXYZ means apply X first: Q = Qz * Qy * Qx). Ported from the
// legacy tydra converter so Rotation channels always carry quaternions.
Float4 EulerDegreesToQuatXYZW(float xdeg, float ydeg, float zdeg,
                              const std::string& order) {
  const double kHalfDegToRad = 3.14159265358979323846 / 360.0;
  const float sx = static_cast<float>(std::sin(double(xdeg) * kHalfDegToRad));
  const float cx = static_cast<float>(std::cos(double(xdeg) * kHalfDegToRad));
  const float sy = static_cast<float>(std::sin(double(ydeg) * kHalfDegToRad));
  const float cy = static_cast<float>(std::cos(double(ydeg) * kHalfDegToRad));
  const float sz = static_cast<float>(std::sin(double(zdeg) * kHalfDegToRad));
  const float cz = static_cast<float>(std::cos(double(zdeg) * kHalfDegToRad));

  if (order == "XZY") {  // Q = Qy * Qz * Qx
    return Float4(cy*cz*sx + sy*sz*cx, cy*sz*sx + sy*cz*cx,
                  cy*sz*cx - sy*cz*sx, cy*cz*cx - sy*sz*sx);
  }
  if (order == "YXZ") {  // Q = Qz * Qx * Qy
    return Float4(cz*sx*cy - sz*cx*sy, cz*cx*sy + sz*sx*cy,
                  cz*sx*sy + sz*cx*cy, cz*cx*cy - sz*sx*sy);
  }
  if (order == "YZX") {  // Q = Qx * Qz * Qy
    return Float4(sx*cz*cy - cx*sz*sy, cx*cz*sy - sx*sz*cy,
                  cx*sz*cy + sx*cz*sy, cx*cz*cy + sx*sz*sy);
  }
  if (order == "ZXY") {  // Q = Qy * Qx * Qz
    return Float4(cy*sx*cz + sy*cx*sz, sy*cx*cz - cy*sx*sz,
                  cy*cx*sz - sy*sx*cz, cy*cx*cz + sy*sx*sz);
  }
  if (order == "ZYX") {  // Q = Qx * Qy * Qz
    return Float4(cx*sy*sz + sx*cy*cz, cx*sy*cz - sx*cy*sz,
                  cx*cy*sz + sx*sy*cz, cx*cy*cz - sx*sy*sz);
  }
  // XYZ (and fallback): Q = Qz * Qy * Qx
  return Float4(cz*cy*sx - sz*sy*cx, cz*sy*cx + sz*cy*sx,
                sz*cy*cx - cz*sy*sx, cz*cy*cx + sz*sy*sx);
}

// Extracts the axis order ("XYZ", "ZYX", ...) from an xformOp:rotate<ORDER>
// property name. Returns false for single-axis rotateX/Y/Z and non-rotate ops.
bool EulerRotationOrderFromPropName(const std::string& prop_name,
                                    std::string* out_order) {
  const size_t pos = prop_name.find("rotate");
  if (pos == std::string::npos) return false;
  const std::string tail = prop_name.substr(pos + 6, 3);
  if (tail == "XYZ" || tail == "XZY" || tail == "YXZ" || tail == "YZX" ||
      tail == "ZXY" || tail == "ZYX") {
    *out_order = tail;
    return true;
  }
  return false;
}

bool ValueToAnimationFloat4(const std::string& prop_name,
                            const Value& value,
                            Float4* out) {
  if (!out || value.is_empty() || value.is_array()) return false;

  float scalar = 0.0f;
  bool is_scalar = false;
  if (const float* v = value.as_float()) {
    scalar = *v;
    is_scalar = true;
  } else if (const double* v = value.as_double()) {
    scalar = static_cast<float>(*v);
    is_scalar = true;
  }

  if (is_scalar) {
    // Single-axis rotations become quaternions: Rotation channels are
    // consumed as xyzw quats by the render layer, never as raw degrees.
    if (prop_name.find("rotateX") != std::string::npos) {
      *out = EulerDegreesToQuatXYZW(scalar, 0.0f, 0.0f, "XYZ");
    } else if (prop_name.find("rotateY") != std::string::npos) {
      *out = EulerDegreesToQuatXYZW(0.0f, scalar, 0.0f, "XYZ");
    } else if (prop_name.find("rotateZ") != std::string::npos) {
      *out = EulerDegreesToQuatXYZW(0.0f, 0.0f, scalar, "XYZ");
    } else if (prop_name.find("scale") != std::string::npos) {
      *out = Float4(scalar, scalar, scalar, 0.0f);
    } else {
      *out = Float4(scalar, 0.0f, 0.0f, 0.0f);
    }
    return true;
  }

  if (!ValueToFloat4(value, out)) return false;

  // next-core Values keep quats real-first (w, x, y, z); render animation
  // channels use xyzw (three.js quaternion order). xformOp:orient is the
  // quat-valued xform op.
  const ::lightusd::next::TypeId tid = value.type_id();
  if (tid == ::lightusd::next::TypeId::Quatf ||
      tid == ::lightusd::next::TypeId::Quatd ||
      tid == ::lightusd::next::TypeId::Quath) {
    *out = Float4(out->y, out->z, out->w, out->x);
    return true;
  }

  // Three-axis Euler rotate ops (float3 degrees) also convert to quats.
  std::string rot_order;
  if (EulerRotationOrderFromPropName(prop_name, &rot_order)) {
    *out = EulerDegreesToQuatXYZW(out->x, out->y, out->z, rot_order);
  }
  return true;
}


void SetIdentity(Matrix4* m) {
  if (!m) return;
  *m = Matrix4::Identity();
}

void CopyMatrixFromDoubles(const std::vector<double>& values,
                           size_t matrix_index,
                           Matrix4* out) {
  if (!out) return;
  SetIdentity(out);
  const size_t offset = matrix_index * 16;
  if (offset + 16 > values.size()) return;
  for (size_t i = 0; i < 16; ++i) {
    out->m[i] = static_cast<float>(values[offset + i]);
  }
}


std::string LeafNameFromJointPath(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return "";
  return path.substr(pos + 1);
}
bool InvertMatrix4x4D(const double m[16], double out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = (r == c) ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
    }
    if (std::fabs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
    }
    const double inv_p = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv_p;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return true;
}

AnimationChannel::TargetPath TargetPathForXformOp(const std::string& prop_name) {
  if (prop_name.find("translate") != std::string::npos) {
    return AnimationChannel::TargetPath::Translation;
  }
  if (prop_name.find("scale") != std::string::npos) {
    return AnimationChannel::TargetPath::Scale;
  }
  return AnimationChannel::TargetPath::Rotation;
}

bool IsXformAnimationProperty(const std::string& prop_name) {
  if (prop_name.find("xformOp:") != 0) return false;
  return prop_name.find("translate") != std::string::npos ||
         prop_name.find("scale") != std::string::npos ||
         prop_name.find("rotate") != std::string::npos ||
         prop_name.find("orient") != std::string::npos;
}

std::vector<double> ValueClipSampleTimes(
    const Stage& stage, const ::lightusd::next::ValueClipSet& meta,
    uint32_t max_samples) {
  std::set<double> exact;
  for (const auto& value : meta.times) exact.insert(value.first);
  for (const auto& value : meta.active) exact.insert(value.first);
  const ::lightusd::next::StageMeta stage_meta = stage.GetMeta();
  double start = exact.empty() ? 0.0 : *exact.begin();
  double end = exact.empty() ? start : *exact.rbegin();
  if (stage_meta.startTimeCode_set) start = stage_meta.startTimeCode;
  if (stage_meta.endTimeCode_set) end = stage_meta.endTimeCode;
  if (end < start) std::swap(start, end);
  exact.insert(start);
  exact.insert(end);

  const uint32_t limit = std::max<uint32_t>(2, max_samples);
  const double span = end - start;
  double step = 1.0;
  if (span > static_cast<double>(limit - 1)) {
    step = span / static_cast<double>(limit - 1);
  }
  for (double t = start; t <= end + step * 0.25; t += step) {
    exact.insert(std::min(t, end));
    if (exact.size() >= limit + meta.times.size() + meta.active.size()) break;
  }
  return std::vector<double>(exact.begin(), exact.end());
}

std::vector<std::string> ReadTokenArrayProperty(const UsdPrim& prim,
                                                const std::string& name) {
  std::vector<std::string> out;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return out;
  if (const std::vector<std::string>* arr = value->as_token_array()) {
    return *arr;
  }
  if (const std::string* tok = value->as_token()) {
    out.push_back(*tok);
  } else if (const std::string* str = value->as_string()) {
    out.push_back(*str);
  }
  return out;
}

bool FirstArrayElementToFloat4(const std::vector<float>& values,
                               uint32_t stride,
                               Float4* out) {
  if (!out || values.empty() || stride == 0) return false;
  const float x = values.size() > 0 ? values[0] : 0.0f;
  const float y = values.size() > 1 ? values[1] : 0.0f;
  const float z = values.size() > 2 ? values[2] : 0.0f;
  const float w = values.size() > 3 ? values[3] : 0.0f;
  if (stride == 1) {
    *out = Float4(x, 0.0f, 0.0f, 0.0f);
  } else if (stride == 3) {
    *out = Float4(x, y, z, 0.0f);
  } else {
    *out = Float4(x, y, z, w);
  }
  return true;
}

size_t SaturatingMul(size_t a, size_t b) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return std::numeric_limits<size_t>::max();
  }
  return a * b;
}

}  // namespace

bool RenderSceneConverter::ConvertSkeleton(const UsdPrim& prim, Skeleton* out) {
  if (!out || !::lightusd::next::IsSkeleton(prim)) {
    SetLastError("Invalid skeleton prim");
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  out->root_joint = -1;

  Stage stage;
  (void)stage;

  const Stage* stage_ptr = nullptr;
  // GetSkeletonData currently only needs the stage for API symmetry. Keep a
  // local empty Stage out of the hot path and read directly from the prim.
  (void)stage_ptr;

  ::lightusd::next::SkeletonData skel;
  // The schema accessor does not dereference Stage for Skeleton fields.
  if (!::lightusd::next::GetSkeletonData(stage, prim, &skel) ||
      skel.joints.empty()) {
    return true;
  }
  out->animation_source_path = skel.animationSource;

  // Authored-count validation: a short bindTransforms/restTransforms array
  // silently identity-fills the tail joints (visually collapsed limbs with
  // no hint why). Unauthored (empty) is fine — rest derives from bind below.
  if (!skel.bindTransforms.empty() &&
      skel.bindTransforms.size() != skel.joints.size() * 16) {
    AddWarning(
        "Skeleton " + prim.GetPath().str() + " authors " +
        std::to_string(skel.bindTransforms.size() / 16) +
        " bindTransforms for " + std::to_string(skel.joints.size()) +
        " joints; missing entries use identity");
  }
  if (!skel.restTransforms.empty() &&
      skel.restTransforms.size() != skel.joints.size() * 16) {
    AddWarning(
        "Skeleton " + prim.GetPath().str() + " authors " +
        std::to_string(skel.restTransforms.size() / 16) +
        " restTransforms for " + std::to_string(skel.joints.size()) +
        " joints; missing entries derive from bindTransforms");
  }

  std::vector<int> topology;
  std::string err;
  if (!::lightusd::next::BuildSkelTopology(skel.joints, topology, &err)) {
    AddWarning("Invalid skeleton topology for " + prim.GetPath().str() +
                        ": " + err);
    topology.assign(skel.joints.size(), -1);
  }

  out->joints.resize(skel.joints.size());
  for (size_t i = 0; i < skel.joints.size(); ++i) {
    SkeletonJoint& joint = out->joints[i];
    joint.path = skel.joints[i];
    if (i < skel.jointNames.size() && !skel.jointNames[i].empty()) {
      joint.name = skel.jointNames[i];
    } else {
      joint.name = LeafNameFromJointPath(skel.joints[i]);
    }
    joint.parent_id = (i < topology.size()) ? topology[i] : -1;
    CopyMatrixFromDoubles(skel.bindTransforms, i, &joint.bind_transform);
    CopyMatrixFromDoubles(skel.restTransforms, i, &joint.rest_transform);

    if (joint.parent_id < 0 && out->root_joint < 0) {
      out->root_joint = static_cast<int32_t>(i);
    }
  }

  // restTransforms are optional in UsdSkel: when unauthored (or too short),
  // derive the parent-local rest pose from the world-space bindTransforms —
  // rest[i] = bind[i] * inverse(bind[parent]) (row-vector convention).
  // Leaving identity here collapses every joint onto its parent.
  if (skel.restTransforms.size() < skel.joints.size() * 16 &&
      skel.bindTransforms.size() >= skel.joints.size() * 16) {
    const size_t authored_rest = skel.restTransforms.size() / 16;
    for (size_t i = authored_rest; i < out->joints.size(); ++i) {
      SkeletonJoint& joint = out->joints[i];
      const int32_t parent = joint.parent_id;
      if (parent < 0) {
        joint.rest_transform = joint.bind_transform;
        continue;
      }
      double parent_bind[16];
      double parent_inv[16];
      for (int e = 0; e < 16; ++e) {
        parent_bind[e] =
            double(out->joints[static_cast<size_t>(parent)].bind_transform.m[e]);
      }
      if (!InvertMatrix4x4D(parent_bind, parent_inv)) {
        joint.rest_transform = joint.bind_transform;
        continue;
      }
      // rest = bind * parent_inv (row-vector: local * parent = world)
      double bind[16];
      for (int e = 0; e < 16; ++e) bind[e] = double(joint.bind_transform.m[e]);
      double rest[16];
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          double sum = 0.0;
          for (int k = 0; k < 4; ++k) {
            sum += bind[r * 4 + k] * parent_inv[k * 4 + c];
          }
          rest[r * 4 + c] = sum;
        }
      }
      for (int e = 0; e < 16; ++e) {
        joint.rest_transform.m[e] = static_cast<float>(rest[e]);
      }
    }
  }

  for (size_t i = 0; i < out->joints.size(); ++i) {
    const int32_t parent = out->joints[i].parent_id;
    if (parent >= 0 && static_cast<size_t>(parent) < out->joints.size()) {
      out->joints[parent].children.push_back(static_cast<int32_t>(i));
    }
  }

  if (out->root_joint < 0 && !out->joints.empty()) {
    out->root_joint = 0;
  }

  return true;
}

//
// Animation conversion
//

bool RenderSceneConverter::ConvertAnimation(const Stage& stage,
                                            const UsdPrim& prim,
                                            AnimationClip* out) {
  if (!out || !prim.IsValid()) return false;

  const std::string prim_path = prim.GetPath().str();
  out->name = prim.GetName() + "_Anim";
  out->prim_path = prim_path;
  out->start_time = std::numeric_limits<double>::max();
  out->end_time = -std::numeric_limits<double>::max();

  if (config_.animation.bake_value_clips) {
    // Clip METADATA semantics come from the core resolver: parse the sets
    // once, then resolve every (property, time) sample through
    // ResolveValueClipFromSets (set-name strength order, times jump
    // discontinuities, out-of-range mapping, manifest gating, nested clips).
    std::vector<::lightusd::next::ValueClipSet> clip_sets;
    std::string clip_error;
    if (::lightusd::next::ParseValueClipSets(prim, &clip_sets, &clip_error)) {
      // Pre-load every clip asset once: records clip_asset_paths for
      // diagnostics, drives property ENUMERATION (the bake needs the property
      // names, which the resolver does not report), and seeds the shared
      // stage cache the core resolver consumes (no double loading).
      ::lightusd::next::ValueClipStageCache clip_cache;
      for (const ::lightusd::next::ValueClipSet& clip_set : clip_sets) {
        for (const std::string& asset_path : clip_set.asset_paths) {
          if (std::find(out->clip_asset_paths.begin(),
                        out->clip_asset_paths.end(), asset_path) ==
              out->clip_asset_paths.end()) {
            out->clip_asset_paths.push_back(asset_path);
          }
          if (clip_cache.entries.count(asset_path)) continue;
          if (!config_.animation.clip_stage_loader) continue;
          auto clip_stage = std::make_shared<Stage>();
          std::string warn;
          std::string err;
          ::lightusd::next::ValueClipStageCache::Entry entry;
          if (config_.animation.clip_stage_loader(asset_path,
                                                  clip_stage.get(), &warn,
                                                  &err)) {
            entry.stage = std::move(clip_stage);
          } else {
            entry.error = "Unable to load value clip '" + asset_path +
                          "' for " + prim_path +
                          (err.empty() ? std::string() : ": " + err);
            AddWarning(entry.error);
          }
          clip_cache.entries.emplace(asset_path, std::move(entry));
          if (!warn.empty()) AddWarning(std::move(warn));
        }
      }

      // A property provided by more than one set bakes ONCE: the core
      // resolver already applies the set strength order per query.
      std::set<std::string> baked_properties;
      for (const ::lightusd::next::ValueClipSet& clip_set : clip_sets) {
        std::set<std::string> properties;
        const std::string clip_prim_path =
            clip_set.prim_path.empty() ? prim_path
                                       : clip_set.prim_path;
        for (const std::string& asset_path : clip_set.asset_paths) {
          const auto stage_it = clip_cache.entries.find(asset_path);
          if (stage_it == clip_cache.entries.end() ||
              !stage_it->second.stage) {
            continue;
          }
          const UsdPrim clip_prim =
              stage_it->second.stage->GetPrimAtPath(clip_prim_path);
          if (!clip_prim.IsValid()) continue;
          for (const std::string& property : clip_prim.GetPropertyNames()) {
            properties.insert(property);
          }
        }

        const std::vector<double> sample_times = ValueClipSampleTimes(
            stage, clip_set, config_.animation.max_value_clip_samples);
        for (const std::string& property : properties) {
          if (!baked_properties.insert(property).second) continue;
          const auto prop_id =
              ::lightusd::next::GetPropNameTable().find(property);
          const bool has_property = prim.HasProperty(prop_id);
          AnimationChannel channel;
          channel.target_path =
              IsXformAnimationProperty(property)
                  ? TargetPathForXformOp(property)
                  : AnimationChannel::TargetPath::CustomProperty;
          channel.target_prim_path = prim_path;
          channel.property_name = property;
          if (BudgetWouldExceed(
                  SaturatingMul(sample_times.size(), sizeof(Keyframe)),
                  "value-clip animation baking")) {
            break;
          }
          channel.keyframes.reserve(sample_times.size());

          for (double stage_time : sample_times) {
            Value value;
            bool have_value = ::lightusd::next::ResolveValueClipFromSets(
                clip_sets, prim, property, stage_time,
                config_.animation.clip_stage_loader, &value, nullptr, nullptr,
                nullptr, &clip_cache);
            if (!have_value && has_property && prop_id.is_valid()) {
              value = prim.GetInterpolatedValue(prop_id, stage_time);
              have_value = !value.is_empty();
            }
            if (!have_value) continue;
            Float4 converted;
            if (!ValueToAnimationFloat4(property, value, &converted)) {
              continue;
            }
            channel.keyframes.push_back(Keyframe{stage_time, converted});
            out->start_time = std::min(out->start_time, stage_time);
            out->end_time = std::max(out->end_time, stage_time);
          }

          if (!channel.keyframes.empty()) {
            out->channels.push_back(std::move(channel));
            out->value_clip_baked = true;
          }
        }
      }
    } else if (!clip_error.empty()) {
      AddWarning("Invalid value clips on " + prim_path +
                          ": " + clip_error);
    }
  }

  if (::lightusd::next::IsSkelAnimation(prim)) {
    const std::vector<std::string> joint_order =
        ReadTokenArrayProperty(prim, "joints");
    const std::vector<std::string> blend_shape_order =
        ReadTokenArrayProperty(prim, "blendShapes");

    auto append_skel_channel = [&](const char* prop_name,
                                   AnimationChannel::TargetPath target_path,
                                   uint32_t stride) {
      const auto prop_id =
          ::lightusd::next::GetPropNameTable().find(prop_name);
      if (!prop_id.is_valid()) return;
      const auto* samples = prim.GetTimeSamples(prop_id);
      if (!samples || samples->empty()) return;

      AnimationChannel channel;
      channel.target_path = target_path;
      channel.target_prim_path = prim_path;
      channel.property_name = prop_name;
      channel.joint_order = joint_order;
      channel.blend_shape_order = blend_shape_order;
      channel.value_stride = stride;
      channel.is_skeletal = true;

      uint32_t expected_elements = 0;
      std::vector<double> times;
      times.reserve(samples->size());
      for (const auto& sample : *samples) times.push_back(sample.first);
      std::vector<float> swizzled;
      for (double t : times) {
        // Read only this channel. GetSkelAnimationDataAtTime evaluates and
        // copies translations, rotations, scales and blend-shape weights on
        // every call; invoking it once per property made long clips perform
        // the complete animation decode up to four times.
        // `times` contains exact authored sample times, so borrow the stored
        // Value directly. AttributeEval/GetInterpolatedValue returns a Value
        // by copy; for a 3000-joint rig that copied tens of thousands of
        // floats once per frame and then copied them again into the channel.
        const Value* sampled = prim.GetValueAtTime(prop_name, t);
        const std::vector<float>* values =
            sampled ? sampled->as_float_array() : nullptr;
        if (!values || values->empty() || ((*values).size() % stride) != 0) {
          continue;
        }

        const uint32_t element_count =
            static_cast<uint32_t>((*values).size() / stride);
        if (expected_elements == 0) {
          expected_elements = element_count;
          channel.element_count = element_count;
          // Best-effort reserve only: the product of two file-controlled
          // counts (time-sample count x per-sample array width) can amplify
          // well past the parsed layer size, and std::vector::reserve throws
          // (terminating this no-exception build) on an over-large or
          // overflowed count. Saturate and cap; growth falls back to the
          // ordinary append path below.
          const size_t want =
              SaturatingMul(samples->size(), values->size());
          constexpr size_t kMaxSkelArrayReserve = 256u * 1024u * 1024u;
          if (want <= kMaxSkelArrayReserve) {
            channel.array_values.reserve(want);
          }
          // array_values is a plain std::vector<float>, not a ChunkedArray, so
          // MemBudget can't see it allocation-by-allocation the way it sees
          // mesh buffers. A 3000-joint rig with a long clip can still put
          // tens of millions of floats here, so charge the whole channel
          // against the same cumulative budget mesh conversion uses, up
          // front, before committing to the per-sample append loop below.
          if (BudgetWouldExceed(SaturatingMul(want, sizeof(float)),
                                "skeletal animation baking")) {
            return;
          }
          // Width validation: blendShapeWeights samples must be as wide as
          // the declared blendShapes list, or weights drive the wrong shapes.
          if (target_path == AnimationChannel::TargetPath::Weights &&
              !blend_shape_order.empty() &&
              element_count != blend_shape_order.size()) {
            AddWarning(
                "SkelAnimation " + prim_path + " has " +
                std::to_string(element_count) +
                " blendShapeWeights per sample for " +
                std::to_string(blend_shape_order.size()) +
                " declared blendShapes");
          }
        } else if (element_count != expected_elements) {
          AddWarning("Skipping inconsistent SkelAnimation sample for " +
                              prim_path + "." + prop_name);
          continue;
        }

        // next-core keeps quats REAL-FIRST (w, x, y, z); an AnimationChannel
        // is the GPU-facing form and its rotation values are xyzw (what a
        // three.js QuaternionKeyframeTrack expects). Swizzle here, at the
        // boundary -- SkelAnimationData itself stays real-first.
        if (target_path == AnimationChannel::TargetPath::Rotation) {
          swizzled.resize(values->size());
          for (size_t i = 0; i + 3 < values->size(); i += 4) {
            swizzled[i + 0] = (*values)[i + 1];  // x
            swizzled[i + 1] = (*values)[i + 2];  // y
            swizzled[i + 2] = (*values)[i + 3];  // z
            swizzled[i + 3] = (*values)[i + 0];  // w (real)
          }
          values = &swizzled;
        }

        Float4 preview;
        if (!FirstArrayElementToFloat4(*values, stride, &preview)) continue;
        channel.keyframes.push_back(Keyframe{t, preview});
        channel.array_values.insert(channel.array_values.end(),
                                    values->begin(), values->end());
        out->start_time = std::min(out->start_time, t);
        out->end_time = std::max(out->end_time, t);
      }

      if (!channel.keyframes.empty()) {
        out->channels.push_back(std::move(channel));
      }
    };

    append_skel_channel("translations",
                        AnimationChannel::TargetPath::Translation, 3);
    append_skel_channel("rotations",
                        AnimationChannel::TargetPath::Rotation, 4);
    append_skel_channel("scales",
                        AnimationChannel::TargetPath::Scale, 3);
    append_skel_channel("blendShapeWeights",
                        AnimationChannel::TargetPath::Weights, 1);

    if (!out->channels.empty()) {
      return true;
    }
  }

  const ::lightusd::next::PrimSpec* prim_spec = prim.GetPrimSpec();
  if (!prim_spec) {
    if (out->channels.empty()) {
      out->start_time = 0.0;
      out->end_time = 0.0;
      return false;
    }
    return true;
  }

  const std::vector<::lightusd::next::PropNameId> sampled_props =
      prim_spec->time_sampled_properties();
  for (const auto& prop_id : sampled_props) {
    const std::string prop_name(
        ::lightusd::next::GetPropNameTable().get(prop_id));
    const auto* samples = prim.GetTimeSamples(prop_id);
    if (!samples || samples->empty()) continue;

    const bool is_xform = IsXformAnimationProperty(prop_name);
    AnimationChannel channel;
    channel.target_path = is_xform ? TargetPathForXformOp(prop_name)
                                   : AnimationChannel::TargetPath::CustomProperty;
    channel.target_prim_path = prim_path;
    channel.property_name = prop_name;
    channel.keyframes.reserve(samples->size());

    for (const auto& sample : *samples) {
      const double t = sample.first;
      Float4 v;
      const Value* value = prim_spec->time_sample_value(sample.second);
      if (!value) continue;
      if (!ValueToAnimationFloat4(prop_name, *value, &v)) continue;
      channel.keyframes.push_back(Keyframe{t, v});
      out->start_time = std::min(out->start_time, t);
      out->end_time = std::max(out->end_time, t);
    }

    if (!channel.keyframes.empty()) {
      out->channels.push_back(std::move(channel));
    }
  }

  if (out->channels.empty()) {
    out->start_time = 0.0;
    out->end_time = 0.0;
    return false;
  }

  return true;
}

//
// Texture loading
//



} } }  // namespace lightusd::tydra::next
