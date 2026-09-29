// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomPointInstancer Schema Implementation

#include "geom-point-instancer.hh"
#include "../layer/property-index.hh"

#include "../types/value-view.hh"

#include <cmath>
#include <unordered_set>

namespace lightusd {
namespace next {
namespace {

const ::lightusd::next::PropNameId& kIdProtoIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("protoIndices");
  return id;
}

const ::lightusd::next::PropNameId& kIdPositions() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("positions");
  return id;
}

const ::lightusd::next::PropNameId& kIdOrientations() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("orientations");
  return id;
}

const ::lightusd::next::PropNameId& kIdScales() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("scales");
  return id;
}

const ::lightusd::next::PropNameId& kIdVelocities() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("velocities");
  return id;
}

const ::lightusd::next::PropNameId& kIdAngularVelocities() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("angularVelocities");
  return id;
}

const ::lightusd::next::PropNameId& kIdIds() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("ids");
  return id;
}

const ::lightusd::next::PropNameId& kIdInvisibleIds() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("invisibleIds");
  return id;
}

const ::lightusd::next::PropNameId& kIdInactiveIds() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inactiveIds");
  return id;
}

// Linear interpolation between samples (pxr semantics), held/default
// fallback. The interpolated value is parked in the caller-owned `hold`
// slot (the ArrayScratch) so returned views stay valid while other
// attributes are read.
const Value* ValueAtOrDefault(const UsdPrim& prim, const PropNameId name_id,
                              double time, Value* hold) {
  if (!name_id.is_valid()) return nullptr;
  if (!std::isnan(time)) {
    Value v = prim.GetInterpolatedValue(name_id, time);
    if (!v.is_empty()) {
      *hold = std::move(v);
      return hold;
    }
    if (const Value* held = prim.GetValueAtTime(name_id, time)) return held;
  }
  return prim.GetPropertyValue(name_id);
}

template <typename T>
std::vector<T> CopyView(const ArrayView<T>& view) {
  if (view.empty()) return {};
  return std::vector<T>(view.begin(), view.end());
}

bool ReadFloatView(const UsdPrim& prim, const PropNameId name_id, double time,
                   ArrayScratch<float>* scratch, ArrayView<float>* view) {
  const Value* v = ValueAtOrDefault(prim, name_id, time, &scratch->materialized);
  return v && GetFloatArrayView(*v, scratch, view);
}

bool ReadIntView(const UsdPrim& prim, const PropNameId name_id, double time,
                 ArrayScratch<int32_t>* scratch, ArrayView<int32_t>* view) {
  const Value* v = ValueAtOrDefault(prim, name_id, time, &scratch->materialized);
  return v && GetIntArrayView(*v, scratch, view);
}

bool ReadInt64View(const UsdPrim& prim, const PropNameId name_id, double time,
                   ArrayScratch<int64_t>* scratch, ArrayView<int64_t>* view) {
  const Value* v = ValueAtOrDefault(prim, name_id, time, &scratch->materialized);
  return v && GetInt64ArrayView(*v, scratch, view);
}

bool ReadFloatView(const UsdPrim& prim, const char* name, double time,
                  ArrayScratch<float>* scratch, ArrayView<float>* view) {
  return ReadFloatView(prim,
                       lightusd::next::GetPropNameTable().find(name), time,
                       scratch, view);
}

bool ReadIntView(const UsdPrim& prim, const char* name, double time,
                ArrayScratch<int32_t>* scratch, ArrayView<int32_t>* view) {
  return ReadIntView(prim, lightusd::next::GetPropNameTable().find(name), time,
                     scratch, view);
}

bool ReadInt64View(const UsdPrim& prim, const char* name, double time,
                  ArrayScratch<int64_t>* scratch, ArrayView<int64_t>* view) {
  return ReadInt64View(prim,
                       lightusd::next::GetPropNameTable().find(name), time,
                       scratch, view);
}

std::vector<float> ReadFloatArrayById(const UsdPrim& prim, PropNameId name_id,
                                     double time) {
  if (!prim.IsValid() || !name_id.is_valid()) return {};
  ArrayScratch<float> scratch;
  ArrayView<float> view;
  return ReadFloatView(prim, name_id, time, &scratch, &view) ? CopyView(view)
                                                           : std::vector<float>();
}

std::vector<int32_t> ReadIntArrayById(const UsdPrim& prim, PropNameId name_id,
                                     double time) {
  if (!prim.IsValid() || !name_id.is_valid()) return {};
  ArrayScratch<int32_t> scratch;
  ArrayView<int32_t> view;
  return ReadIntView(prim, name_id, time, &scratch, &view) ? CopyView(view)
                                                           : std::vector<int32_t>();
}

std::vector<int64_t> ReadInt64ArrayById(const UsdPrim& prim,
                                        PropNameId name_id, double time) {
  if (!prim.IsValid() || !name_id.is_valid()) return {};
  ArrayScratch<int64_t> scratch;
  ArrayView<int64_t> view;
  return ReadInt64View(prim, name_id, time, &scratch, &view) ? CopyView(view)
                                                           : std::vector<int64_t>();
}

void SetIdentity(double m[16]) {
  for (int i = 0; i < 16; ++i) m[i] = 0.0;
  m[0] = m[5] = m[10] = m[15] = 1.0;
}

void ComposeTRS(const float* t, const float* q, const float* s, double out[16]) {
  SetIdentity(out);

  const double tx = t ? static_cast<double>(t[0]) : 0.0;
  const double ty = t ? static_cast<double>(t[1]) : 0.0;
  const double tz = t ? static_cast<double>(t[2]) : 0.0;
  const double sx = s ? static_cast<double>(s[0]) : 1.0;
  const double sy = s ? static_cast<double>(s[1]) : 1.0;
  const double sz = s ? static_cast<double>(s[2]) : 1.0;

  double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
  if (q) {
    w = static_cast<double>(q[0]);
    x = static_cast<double>(q[1]);
    y = static_cast<double>(q[2]);
    z = static_cast<double>(q[3]);
    const double len2 = w * w + x * x + y * y + z * z;
    if (len2 > 0.0) {
      const double inv_len = 1.0 / std::sqrt(len2);
      w *= inv_len;
      x *= inv_len;
      y *= inv_len;
      z *= inv_len;
    } else {
      w = 1.0;
      x = y = z = 0.0;
    }
  }

  const double xx = x * x, yy = y * y, zz = z * z;
  const double xy = x * y, xz = x * z, yz = y * z;
  const double wx = w * x, wy = w * y, wz = w * z;

  out[0] = (1.0 - 2.0 * (yy + zz)) * sx;
  out[1] = (2.0 * (xy - wz)) * sy;
  out[2] = (2.0 * (xz + wy)) * sz;
  out[4] = (2.0 * (xy + wz)) * sx;
  out[5] = (1.0 - 2.0 * (xx + zz)) * sy;
  out[6] = (2.0 * (yz - wx)) * sz;
  out[8] = (2.0 * (xz - wy)) * sx;
  out[9] = (2.0 * (yz + wx)) * sy;
  out[10] = (1.0 - 2.0 * (xx + yy)) * sz;
  out[12] = tx;
  out[13] = ty;
  out[14] = tz;
}

}  // namespace

UsdGeomPointInstancer::UsdGeomPointInstancer(const UsdPrim& prim) : prim_(prim) {
  is_point_instancer_ = prim_.IsValid() && prim_.GetTypeName() == "PointInstancer";
}

std::vector<Path> UsdGeomPointInstancer::GetPrototypes() const {
  if (!IsValid()) return {};
  const std::vector<Path>* targets = prim_.GetRelationship("prototypes");
  return targets ? *targets : std::vector<Path>();
}

std::vector<float> UsdGeomPointInstancer::GetFloatArray(const char* name,
                                                        double time) const {
  if (!IsValid()) return {};
  ArrayScratch<float> scratch;
  ArrayView<float> view;
  return ReadFloatView(prim_, name, time, &scratch, &view) ? CopyView(view)
                                                           : std::vector<float>();
}

std::vector<int32_t> UsdGeomPointInstancer::GetIntArray(const char* name,
                                                        double time) const {
  if (!IsValid()) return {};
  ArrayScratch<int32_t> scratch;
  ArrayView<int32_t> view;
  return ReadIntView(prim_, name, time, &scratch, &view) ? CopyView(view)
                                                         : std::vector<int32_t>();
}

std::vector<int64_t> UsdGeomPointInstancer::GetInt64Array(const char* name,
                                                          double time) const {
  if (!IsValid()) return {};
  ArrayScratch<int64_t> scratch;
  ArrayView<int64_t> view;
  return ReadInt64View(prim_, name, time, &scratch, &view) ? CopyView(view)
                                                           : std::vector<int64_t>();
}

std::vector<int32_t> UsdGeomPointInstancer::GetProtoIndices(double time) const {
  return ReadIntArrayById(prim_, kIdProtoIndices(), time);
}

std::vector<float> UsdGeomPointInstancer::GetPositions(double time) const {
  return ReadFloatArrayById(prim_, kIdPositions(), time);
}

std::vector<float> UsdGeomPointInstancer::GetOrientations(double time) const {
  return ReadFloatArrayById(prim_, kIdOrientations(), time);
}

std::vector<float> UsdGeomPointInstancer::GetScales(double time) const {
  return ReadFloatArrayById(prim_, kIdScales(), time);
}

std::vector<float> UsdGeomPointInstancer::GetVelocities(double time) const {
  return ReadFloatArrayById(prim_, kIdVelocities(), time);
}

std::vector<float> UsdGeomPointInstancer::GetAngularVelocities(double time) const {
  return ReadFloatArrayById(prim_, kIdAngularVelocities(), time);
}

std::vector<int64_t> UsdGeomPointInstancer::GetIds(double time) const {
  return ReadInt64ArrayById(prim_, kIdIds(), time);
}

std::vector<int64_t> UsdGeomPointInstancer::GetInvisibleIds(double time) const {
  return ReadInt64ArrayById(prim_, kIdInvisibleIds(), time);
}

std::vector<int64_t> UsdGeomPointInstancer::GetInactiveIds() const {
  return ReadInt64ArrayById(prim_, kIdInactiveIds(), 0.0);
}

size_t UsdGeomPointInstancer::GetInstanceCount(double time) const {
  if (!IsValid()) return 0;
  ArrayScratch<int32_t> proto_scratch;
  ArrayView<int32_t> proto_indices;
  if (ReadIntView(prim_, kIdProtoIndices(), time, &proto_scratch, &proto_indices) &&
      !proto_indices.empty()) {
    return proto_indices.size;
  }
  ArrayScratch<float> position_scratch;
  ArrayView<float> positions;
  return ReadFloatView(prim_, kIdPositions(), time, &position_scratch, &positions)
             ? positions.size / 3
             : 0;
}

bool UsdGeomPointInstancer::HasValidInstanceArrays(
    double time, std::string* reason) const {
  if (!IsValid()) {
    if (reason) *reason = "not a PointInstancer";
    return false;
  }
  const std::vector<Path> prototypes = GetPrototypes();
  ArrayScratch<int32_t> proto_scratch;
  ArrayView<int32_t> proto_indices;
  ReadIntView(prim_, kIdProtoIndices(), time, &proto_scratch, &proto_indices);
  ArrayScratch<float> position_scratch;
  ArrayView<float> positions;
  ReadFloatView(prim_, kIdPositions(), time, &position_scratch, &positions);
  const size_t n = proto_indices.size;
  if (prototypes.empty()) {
    if (reason) *reason = "missing prototypes relationship";
    return false;
  }
  if (n == 0) {
    if (reason) *reason = "missing protoIndices";
    return false;
  }
  if (positions.size != n * 3) {
    if (reason) *reason = "positions size does not match protoIndices";
    return false;
  }
  for (int32_t idx : proto_indices) {
    if (idx < 0 || static_cast<size_t>(idx) >= prototypes.size()) {
      if (reason) *reason = "protoIndices contains out-of-range prototype index";
      return false;
    }
  }
  ArrayScratch<float> orientation_scratch;
  ArrayView<float> orientations;
  ReadFloatView(prim_, kIdOrientations(), time, &orientation_scratch, &orientations);
  if (!orientations.empty() && orientations.size != n * 4) {
    if (reason) *reason = "orientations size does not match protoIndices";
    return false;
  }
  ArrayScratch<float> scale_scratch;
  ArrayView<float> scales;
  ReadFloatView(prim_, kIdScales(), time, &scale_scratch, &scales);
  if (!scales.empty() && scales.size != n * 3) {
    if (reason) *reason = "scales size does not match protoIndices";
    return false;
  }
  ArrayScratch<float> velocity_scratch;
  ArrayView<float> velocities;
  ReadFloatView(prim_, kIdVelocities(), time, &velocity_scratch, &velocities);
  if (!velocities.empty() && velocities.size != n * 3) {
    if (reason) *reason = "velocities size does not match protoIndices";
    return false;
  }
  ArrayScratch<float> angular_velocity_scratch;
  ArrayView<float> angular_velocities;
  ReadFloatView(prim_, kIdAngularVelocities(), time, &angular_velocity_scratch,
                &angular_velocities);
  if (!angular_velocities.empty() && angular_velocities.size != n * 3) {
    if (reason) *reason = "angularVelocities size does not match protoIndices";
    return false;
  }
  ArrayScratch<int64_t> ids_scratch;
  ArrayView<int64_t> ids;
  ReadInt64View(prim_, kIdIds(), time, &ids_scratch, &ids);
  if (!ids.empty() && ids.size != n) {
    if (reason) *reason = "ids size does not match protoIndices";
    return false;
  }
  if (reason) reason->clear();
  return true;
}

std::vector<PointInstancerTransform>
UsdGeomPointInstancer::ComputeInstanceTransforms(double time) const {
  std::vector<PointInstancerTransform> out;
  if (!IsValid()) return out;
  ArrayScratch<int32_t> proto_scratch;
  ArrayView<int32_t> proto_indices;
  ReadIntView(prim_, kIdProtoIndices(), time, &proto_scratch, &proto_indices);
  ArrayScratch<float> position_scratch;
  ArrayView<float> positions;
  ReadFloatView(prim_, kIdPositions(), time, &position_scratch, &positions);
  if (proto_indices.empty() || positions.size != proto_indices.size * 3) {
    return out;
  }
  ArrayScratch<float> orientation_scratch;
  ArrayView<float> orientations;
  ReadFloatView(prim_, kIdOrientations(), time, &orientation_scratch, &orientations);
  ArrayScratch<float> scale_scratch;
  ArrayView<float> scales;
  ReadFloatView(prim_, kIdScales(), time, &scale_scratch, &scales);
  const size_t n = proto_indices.size;
  out.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const float* t = positions.data + i * 3;
    const float* q =
        orientations.size == n * 4 ? orientations.data + i * 4 : nullptr;
    const float* s = scales.size == n * 3 ? scales.data + i * 3 : nullptr;
    ComposeTRS(t, q, s, out[i].matrix);
  }
  return out;
}

std::vector<bool> UsdGeomPointInstancer::ComputeMaskAtTime(double time) const {
  const size_t n = GetInstanceCount(time);
  std::vector<bool> mask(n, true);
  if (n == 0) return mask;
  const std::vector<int64_t> invisible = GetInvisibleIds(time);
  const std::vector<int64_t> inactive = GetInactiveIds();
  if (invisible.empty() && inactive.empty()) return mask;  // all visible
  std::unordered_set<int64_t> hidden(invisible.begin(), invisible.end());
  hidden.insert(inactive.begin(), inactive.end());
  // ids[i] is instance i's authored id; absent -> id == index.
  const std::vector<int64_t> ids = GetIds(time);
  for (size_t i = 0; i < n; ++i) {
    const int64_t id = (i < ids.size()) ? ids[i] : static_cast<int64_t>(i);
    if (hidden.count(id)) mask[i] = false;
  }
  return mask;
}

bool IsPointInstancer(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "PointInstancer";
}

std::vector<UsdGeomPointInstancer> GetAllPointInstancers(const Stage& stage) {
  std::vector<UsdGeomPointInstancer> instancers;
  stage.Traverse([&instancers](const UsdPrim& prim) {
    if (IsPointInstancer(prim)) instancers.emplace_back(prim);
    return true;
  });
  return instancers;
}

}  // namespace next
}  // namespace lightusd
