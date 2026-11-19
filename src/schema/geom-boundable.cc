// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdGeomBoundable extent computation (see geom-boundable.hh)

#include "geom-boundable.hh"

#include <algorithm>
#include <cmath>
#include <string>

#include "layer/property-index.hh"
#include "stage/stage.hh"
#include "types/value-view.hh"

namespace lightusd {
namespace {

const Value* ValueAt(const UsdPrim& prim, const char* name, double time, Value* hold) {
  const PropNameId id = GetPropNameTable().find(name);
  if (!id.is_valid()) return nullptr;
  Value v = prim.GetInterpolatedValue(id, time);
  if (!v.is_empty()) {
    *hold = std::move(v);
    return hold;
  }
  if (const Value* held = prim.GetValueAtTime(id, time)) return held;
  return prim.GetPropertyValue(id);
}

double ScalarOr(const UsdPrim& prim, const char* name, double time, double fallback) {
  Value hold;
  const Value* v = ValueAt(prim, name, time, &hold);
  if (!v || v->is_array()) return fallback;
  if (const double* d = v->as_double()) return *d;
  if (const float* f = v->as_float()) return static_cast<double>(*f);
  return fallback;
}

// Uniform axis token; 'X'/'Y'/'Z' -> 0/1/2, -1 when invalid.
int AxisOf(const UsdPrim& prim, double time) {
  Value hold;
  const Value* v = ValueAt(prim, "axis", time, &hold);
  if (!v) return 2;  // fallback "Z"
  const StringView token = v->as_token();
  if (token.size() != 1) return -1;
  switch (token.data()[0]) {
    case 'X': return 0;
    case 'Y': return 1;
    case 'Z': return 2;
    default: return -1;
  }
}

bool ReadFloats(const UsdPrim& prim, const char* name, double time,
                ArrayScratch<float>* scratch, ArrayView<float>* view) {
  // An interpolated value lands in scratch->materialized, which outlives the
  // view (same pattern as geom-point-instancer.cc).
  const Value* v = ValueAt(prim, name, time, &scratch->materialized);
  return v && GetFloatArrayView(*v, scratch, view);
}

void SetBox(double out[6], double x, double y, double z) {
  out[0] = -x; out[1] = -y; out[2] = -z;
  out[3] = x; out[4] = y; out[5] = z;
}

// Extents are float3 in OpenUSD (VtVec3fArray); round the same way.
void RoundToFloat(double out[6]) {
  for (int k = 0; k < 6; ++k) out[k] = static_cast<double>(static_cast<float>(out[k]));
}

bool PointsExtent(const ArrayView<float>& points, const float* widths, size_t width_count,
                  bool per_point, float grow, double out[6]) {
  if (points.size < 3) return false;
  const size_t n = points.size / 3;
  if (per_point && width_count != n) return false;
  double lo[3] = {INFINITY, INFINITY, INFINITY};
  double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (size_t i = 0; i < n; ++i) {
    const double half = per_point ? 0.5 * static_cast<double>(static_cast<float>(widths[i]))
                                  : static_cast<double>(grow);
    for (int k = 0; k < 3; ++k) {
      const double p = static_cast<double>(points.data[i * 3 + k]);
      lo[k] = std::min(lo[k], p - half);
      hi[k] = std::max(hi[k], p + half);
    }
  }
  out[0] = lo[0]; out[1] = lo[1]; out[2] = lo[2];
  out[3] = hi[0]; out[4] = hi[1]; out[5] = hi[2];
  return true;
}

bool IsCurves(const std::string& type) {
  return type == "BasisCurves" || type == "NurbsCurves" || type == "HermiteCurves";
}

bool IsPointBased(const std::string& type) {
  return type == "Mesh" || type == "NurbsPatch" || type == "TetMesh";
}

}  // namespace

bool ComputeExtent(const UsdPrim& prim, double time, double out[6]) {
  if (!prim.IsValid()) return false;
  const std::string& type = prim.GetTypeName();

  if (type == "Points" || IsCurves(type) || IsPointBased(type)) {
    ArrayScratch<float> point_scratch;
    ArrayView<float> points;
    if (!ReadFloats(prim, "points", time, &point_scratch, &points)) return false;
    ArrayScratch<float> width_scratch;
    ArrayView<float> widths;
    const bool has_widths =
        (type == "Points" || IsCurves(type)) &&
        ReadFloats(prim, "widths", time, &width_scratch, &widths);
    bool ok = false;
    if (type == "Points" && has_widths) {
      ok = PointsExtent(points, widths.data, widths.size, true, 0.0f, out);
    } else if (IsCurves(type) && has_widths && !widths.empty()) {
      const float max_width = *std::max_element(widths.data, widths.data + widths.size);
      ok = PointsExtent(points, nullptr, 0, false, max_width * 0.5f, out);
    } else {
      ok = PointsExtent(points, nullptr, 0, false, 0.0f, out);
    }
    if (ok) RoundToFloat(out);
    return ok;
  }

  if (type == "Cube") {
    const double h = 0.5 * ScalarOr(prim, "size", time, 2.0);
    SetBox(out, h, h, h);
  } else if (type == "Sphere") {
    const double r = ScalarOr(prim, "radius", time, 1.0);
    SetBox(out, r, r, r);
  } else if (type == "Cylinder" || type == "Cone" || type == "Capsule" ||
             type == "Cylinder_1" || type == "Plane") {
    const int axis = AxisOf(prim, time);
    if (axis < 0) return false;
    double along = 0.0, across = 0.0, across2 = 0.0;
    if (type == "Plane") {
      // width spans the first perpendicular axis, length the second, except
      // axis X where length is Y and width is Z (UsdGeomPlane).
      const double hw = 0.5 * ScalarOr(prim, "width", time, 2.0);
      const double hl = 0.5 * ScalarOr(prim, "length", time, 2.0);
      double box[3] = {0, 0, 0};
      if (axis == 0) { box[1] = hl; box[2] = hw; }
      else if (axis == 1) { box[0] = hw; box[2] = hl; }
      else { box[0] = hw; box[1] = hl; }
      SetBox(out, box[0], box[1], box[2]);
      RoundToFloat(out);
      return true;
    }
    if (type == "Capsule") {
      const double r = ScalarOr(prim, "radius", time, 0.5);
      // OpenUSD accumulates the capped half height in float.
      along = static_cast<double>(
          static_cast<float>(ScalarOr(prim, "height", time, 1.0) * 0.5 + r));
      across = across2 = r;
    } else if (type == "Cylinder_1") {
      const double r = std::max(ScalarOr(prim, "radiusTop", time, 1.0),
                                ScalarOr(prim, "radiusBottom", time, 1.0));
      along = 0.5 * ScalarOr(prim, "height", time, 2.0);
      across = across2 = r;
    } else {
      const double r = ScalarOr(prim, "radius", time, 1.0);
      along = 0.5 * ScalarOr(prim, "height", time, 2.0);
      across = across2 = r;
    }
    double box[3] = {across, across, across2};
    box[axis] = along;
    SetBox(out, box[0], box[1], box[2]);
  } else if (type == "Capsule_1") {
    const int axis = AxisOf(prim, time);
    const double height = ScalarOr(prim, "height", time, 1.0);
    const double rt = ScalarOr(prim, "radiusTop", time, 0.5);
    const double rb = ScalarOr(prim, "radiusBottom", time, 0.5);
    if (axis < 0 || height < 0.0 || rt < 0.0 || rb < 0.0) return false;
    // End spheres tangent to the frustum slant (UsdGeomCapsule_1).
    const double half = 0.5 * height;
    const double slope = height > 0.0 ? (rb - rt) / height : 0.0;
    const double secant = std::sqrt(slope * slope + 1.0);
    const double top_r = rt * secant, bottom_r = rb * secant;
    const double max_h = (half - slope * rt) + top_r;
    const double min_h = (-half - slope * rb) - bottom_r;
    const double r = std::max(top_r, bottom_r);
    double lo[3] = {-r, -r, -r}, hi[3] = {r, r, r};
    lo[axis] = min_h;
    hi[axis] = max_h;
    out[0] = lo[0]; out[1] = lo[1]; out[2] = lo[2];
    out[3] = hi[0]; out[4] = hi[1]; out[5] = hi[2];
  } else {
    return false;
  }
  RoundToFloat(out);
  return true;
}

}  // namespace lightusd
