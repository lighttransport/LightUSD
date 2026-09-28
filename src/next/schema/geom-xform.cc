// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomXform Schema Implementation

#include "geom-xform.hh"
#include "../layer/property-index.hh"

#include <algorithm>
#include <cstring>
#include <cmath>

namespace lightusd {
namespace next {

UsdGeomXform::UsdGeomXform(const UsdPrim& prim) : prim_(prim) {}

namespace {

const PropNameId& kIdXformOpOrder() {
  static const PropNameId id = GetPropNameTable().intern("xformOpOrder");
  return id;
}
const PropNameId& kIdTranslate() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:translate");
  return id;
}
const PropNameId& kIdScale() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:scale");
  return id;
}
const PropNameId& kIdRotateX() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:rotateX");
  return id;
}
const PropNameId& kIdRotateY() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:rotateY");
  return id;
}
const PropNameId& kIdRotateZ() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:rotateZ");
  return id;
}
const PropNameId& kIdRotateXYZ() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:rotateXYZ");
  return id;
}
const PropNameId& kIdOrient() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:orient");
  return id;
}
const PropNameId& kIdTransform() {
  static const PropNameId id = GetPropNameTable().intern("xformOp:transform");
  return id;
}

}  // namespace

std::vector<std::string> UsdGeomXform::GetXformOpOrder() const {
  std::vector<std::string> result;
  if (!IsValid()) return result;

  const Value* v = prim_.GetPropertyValue(kIdXformOpOrder());
  if (!v) return result;
  if (const std::vector<std::string>* toks = v->as_token_array()) {
    result = *toks;
  }
  return result;
}

XformOpType UsdGeomXform::ParseOpType(const std::string& op_name) const {
  // Parse op name like "xformOp:translate" or "xformOp:rotateX:pivot"
  if (op_name.find("rotateXYZ") != std::string::npos) return XformOpType::Rotate;
  if (op_name.find("translate") != std::string::npos) return XformOpType::Translate;
  if (op_name.find("rotateX") != std::string::npos) return XformOpType::RotateX;
  if (op_name.find("rotateY") != std::string::npos) return XformOpType::RotateY;
  if (op_name.find("rotateZ") != std::string::npos) return XformOpType::RotateZ;
  if (op_name.find("rotate") != std::string::npos) return XformOpType::Rotate;
  if (op_name.find("scale") != std::string::npos) return XformOpType::Scale;
  if (op_name.find("orient") != std::string::npos) return XformOpType::Orient;
  if (op_name.find("transform") != std::string::npos) return XformOpType::Transform;
  return XformOpType::Unknown;
}

namespace {

void Identity(double* m) {
  std::fill(m, m + 16, 0.0);
  m[0] = m[5] = m[10] = m[15] = 1.0;
}

void Multiply(const double* a, const double* b, double* out) {
  double r[16];
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      double v = 0.0;
      for (int k = 0; k < 4; ++k) {
        v += a[row * 4 + k] * b[k * 4 + col];
      }
      r[row * 4 + col] = v;
    }
  }
  std::memcpy(out, r, sizeof(r));
}

void Translation(double x, double y, double z, double* m) {
  Identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

void Scale(double x, double y, double z, double* m) {
  Identity(m);
  m[0] = x;
  m[5] = y;
  m[10] = z;
}

void RotateX(double degrees, double* m) {
  Identity(m);
  const double a = degrees * std::acos(-1.0) / 180.0;
  const double c = std::cos(a);
  const double s = std::sin(a);
  m[5] = c;
  m[6] = s;
  m[9] = -s;
  m[10] = c;
}

void RotateY(double degrees, double* m) {
  Identity(m);
  const double a = degrees * std::acos(-1.0) / 180.0;
  const double c = std::cos(a);
  const double s = std::sin(a);
  m[0] = c;
  m[2] = -s;
  m[8] = s;
  m[10] = c;
}

void RotateZ(double degrees, double* m) {
  Identity(m);
  const double a = degrees * std::acos(-1.0) / 180.0;
  const double c = std::cos(a);
  const double s = std::sin(a);
  m[0] = c;
  m[1] = s;
  m[4] = -s;
  m[5] = c;
}

bool ReadScalar(const Value& value, double* out) {
  if (const float* f = value.as_float()) {
    *out = static_cast<double>(*f);
    return true;
  }
  if (const double* d = value.as_double()) {
    *out = *d;
    return true;
  }
  return false;
}

bool ReadVec3(const Value& value, double* x, double* y, double* z) {
  if (const float* f = value.as_float3()) {
    *x = static_cast<double>(f[0]);
    *y = static_cast<double>(f[1]);
    *z = static_cast<double>(f[2]);
    return true;
  }
  if (const double* d = value.as_double3()) {
    *x = d[0];
    *y = d[1];
    *z = d[2];
    return true;
  }
  return false;
}

bool ReadMatrix(const Value& value, double* m) {
  if (const float* f = value.as_matrix4f()) {
    for (int i = 0; i < 16; ++i) m[i] = static_cast<double>(f[i]);
    return true;
  }
  if (const double* d = value.as_matrix4d()) {
    std::memcpy(m, d, 16 * sizeof(double));
    return true;
  }
  return false;
}

bool ReadQuat(const Value& value, double* w, double* x, double* y, double* z) {
  // USD quaternions are stored in real-first (w, x, y, z) order. The Value
  // accessors retain that order for both float and double quaternion values.
  if (const float* f = value.as_float4()) {
    *w = static_cast<double>(f[0]);
    *x = static_cast<double>(f[1]);
    *y = static_cast<double>(f[2]);
    *z = static_cast<double>(f[3]);
    return true;
  }
  if (const double* d = value.as_double4()) {
    *w = d[0];
    *x = d[1];
    *y = d[2];
    *z = d[3];
    return true;
  }
  return false;
}

void Orient(double w, double x, double y, double z, double* m) {
  // Matrices in the next transform evaluator use USD's row-vector layout.
  // Normalize authored quaternions so slightly non-unit values cannot scale
  // the mesh while converting the orientation to a 3x3 rotation.
  const double length = std::sqrt(w * w + x * x + y * y + z * z);
  if (length == 0.0) {
    Identity(m);
    return;
  }
  w /= length;
  x /= length;
  y /= length;
  z /= length;
  Identity(m);
  m[0] = 1.0 - 2.0 * (y * y + z * z);
  m[1] = 2.0 * (x * y + w * z);
  m[2] = 2.0 * (x * z - w * y);
  m[4] = 2.0 * (x * y - w * z);
  m[5] = 1.0 - 2.0 * (x * x + z * z);
  m[6] = 2.0 * (y * z + w * x);
  m[8] = 2.0 * (x * z + w * y);
  m[9] = 2.0 * (y * z - w * x);
  m[10] = 1.0 - 2.0 * (x * x + y * y);
}

bool BuildRotateABC(const XformOpType type, const double x, const double y,
                    const double z, double* m) {
  double rx[16], ry[16], rz[16], tmp[16];
  RotateX(x, rx);
  RotateY(y, ry);
  RotateZ(z, rz);
  Identity(m);
  auto append = [&](const char axis) {
    const double* r = (axis == 'X') ? rx : ((axis == 'Y') ? ry : rz);
    // USD uses row-vector matrices. Apply rotateXYZ in authored axis order,
    // matching tydra::next::ComputeLocalTransform and the legacy evaluator.
    Multiply(m, r, tmp);
    std::memcpy(m, tmp, sizeof(tmp));
  };
  switch (type) {
    case XformOpType::Rotate:
      append('X'); append('Y'); append('Z'); return true;
    default:
      return false;
  }
}

}  // namespace

std::vector<XformOp> UsdGeomXform::GetXformOps() const {
  std::vector<XformOp> ops;
  if (!IsValid()) return ops;

  auto order = GetXformOpOrder();
  for (const auto& op_name : order) {
    XformOp op;
    op.is_inverse = (op_name.find("!invert!") == 0);

    std::string actual_name = op.is_inverse ? op_name.substr(8) : op_name;
    op.type = ParseOpType(actual_name);

    // Get the value
    const auto op_prop_id = GetPropNameTable().find(actual_name);
    const Value* val = (op_prop_id.is_valid() ? prim_.GetPropertyValue(op_prop_id)
                                              : nullptr);
    if (val) {
      op.value = *val;
    }

    ops.push_back(std::move(op));
  }
  return ops;
}

bool UsdGeomXform::HasAnimatedTransform() const {
  if (!IsValid()) return false;

  // Check common transform properties
  if (prim_.HasTimeSamples(kIdTranslate())) return true;
  if (prim_.HasTimeSamples(kIdScale())) return true;
  if (prim_.HasTimeSamples(kIdRotateX())) return true;
  if (prim_.HasTimeSamples(kIdRotateY())) return true;
  if (prim_.HasTimeSamples(kIdRotateZ())) return true;
  if (prim_.HasTimeSamples(kIdRotateXYZ())) return true;
  if (prim_.HasTimeSamples(kIdOrient())) return true;
  if (prim_.HasTimeSamples(kIdTransform())) return true;

  return false;
}

bool UsdGeomXform::GetTranslation(float* x, float* y, float* z) const {
  if (!IsValid() || !x || !y || !z) return false;

  const Value* val = prim_.GetPropertyValue(kIdTranslate());
  if (!val) return false;

  // Try float3
  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  // Try double3
  const double* d3 = val->as_double3();
  if (d3) {
    *x = static_cast<float>(d3[0]);
    *y = static_cast<float>(d3[1]);
    *z = static_cast<float>(d3[2]);
    return true;
  }

  return false;
}

bool UsdGeomXform::GetTranslation(double* x, double* y, double* z) const {
  if (!IsValid() || !x || !y || !z) return false;

  const Value* val = prim_.GetPropertyValue(kIdTranslate());
  if (!val) return false;

  const double* d3 = val->as_double3();
  if (d3) {
    *x = d3[0]; *y = d3[1]; *z = d3[2];
    return true;
  }

  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  return false;
}

bool UsdGeomXform::GetScale(float* x, float* y, float* z) const {
  if (!IsValid() || !x || !y || !z) return false;

  const Value* val = prim_.GetPropertyValue(kIdScale());
  if (!val) return false;

  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  const double* d3 = val->as_double3();
  if (d3) {
    *x = static_cast<float>(d3[0]);
    *y = static_cast<float>(d3[1]);
    *z = static_cast<float>(d3[2]);
    return true;
  }

  return false;
}

bool UsdGeomXform::GetRotation(float* x, float* y, float* z) const {
  if (!IsValid() || !x || !y || !z) return false;

  // Try rotateXYZ first
  const Value* val = prim_.GetPropertyValue(kIdRotateXYZ());
  if (val) {
    const float* f3 = val->as_float3();
    if (f3) {
      *x = f3[0]; *y = f3[1]; *z = f3[2];
      return true;
    }
  }

  // Try individual rotations
  *x = *y = *z = 0.0f;
  bool found = false;

  val = prim_.GetPropertyValue(kIdRotateX());
  if (val) {
    const float* f = val->as_float();
    if (f) { *x = *f; found = true; }
  }

  val = prim_.GetPropertyValue(kIdRotateY());
  if (val) {
    const float* f = val->as_float();
    if (f) { *y = *f; found = true; }
  }

  val = prim_.GetPropertyValue(kIdRotateZ());
  if (val) {
    const float* f = val->as_float();
    if (f) { *z = *f; found = true; }
  }

  return found;
}

bool UsdGeomXform::GetOrientation(float* w, float* x, float* y, float* z) const {
  if (!IsValid() || !w || !x || !y || !z) return false;

  const Value* val = prim_.GetPropertyValue(kIdOrient());
  if (!val) return false;

  // Quaternion stored as (w, x, y, z) or (x, y, z, w) depending on convention
  // USD uses (real, i, j, k) = (w, x, y, z)
  const float* f4 = val->as_float4();
  if (f4) {
    *w = f4[0]; *x = f4[1]; *y = f4[2]; *z = f4[3];
    return true;
  }

  const double* d4 = val->as_double4();
  if (d4) {
    *w = static_cast<float>(d4[0]);
    *x = static_cast<float>(d4[1]);
    *y = static_cast<float>(d4[2]);
    *z = static_cast<float>(d4[3]);
    return true;
  }

  return false;
}

bool UsdGeomXform::ComputeLocalTransform(float* matrix) const {
  if (!IsValid() || !matrix) return false;

  double dmatrix[16];
  if (!ComputeLocalTransform(dmatrix)) return false;
  for (int i = 0; i < 16; ++i) {
    matrix[i] = static_cast<float>(dmatrix[i]);
  }
  return true;
}

bool UsdGeomXform::ComputeLocalTransform(double* matrix) const {
  if (!IsValid() || !matrix) return false;

  std::vector<XformOp> ops = GetXformOps();
  if (ops.empty()) {
    Identity(matrix);
    return true;
  }

  Identity(matrix);
  for (const XformOp& op : ops) {
    if (op.type == XformOpType::Unknown) continue;
    double m[16];
    Identity(m);
    switch (op.type) {
      case XformOpType::Translate: {
        double x = 0.0, y = 0.0, z = 0.0;
        if (!ReadVec3(op.value, &x, &y, &z)) return false;
        if (op.is_inverse) {
          x = -x;
          y = -y;
          z = -z;
        }
        Translation(x, y, z, m);
        break;
      }
      case XformOpType::Scale: {
        double x = 1.0, y = 1.0, z = 1.0;
        if (!ReadVec3(op.value, &x, &y, &z)) return false;
        if (op.is_inverse) {
          if (x == 0.0 || y == 0.0 || z == 0.0) return false;
          x = 1.0 / x;
          y = 1.0 / y;
          z = 1.0 / z;
        }
        Scale(x, y, z, m);
        break;
      }
      case XformOpType::RotateX: {
        double a = 0.0;
        if (!ReadScalar(op.value, &a)) return false;
        RotateX(op.is_inverse ? -a : a, m);
        break;
      }
      case XformOpType::RotateY: {
        double a = 0.0;
        if (!ReadScalar(op.value, &a)) return false;
        RotateY(op.is_inverse ? -a : a, m);
        break;
      }
      case XformOpType::RotateZ: {
        double a = 0.0;
        if (!ReadScalar(op.value, &a)) return false;
        RotateZ(op.is_inverse ? -a : a, m);
        break;
      }
      case XformOpType::Rotate: {
        double x = 0.0, y = 0.0, z = 0.0;
        if (!ReadVec3(op.value, &x, &y, &z)) return false;
        if (op.is_inverse) {
          x = -x;
          y = -y;
          z = -z;
        }
        if (!BuildRotateABC(op.type, x, y, z, m)) return false;
        break;
      }
      case XformOpType::Transform: {
        if (!ReadMatrix(op.value, m)) return false;
        if (op.is_inverse) return false;
        break;
      }
      case XformOpType::Orient: {
        double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
        if (!ReadQuat(op.value, &w, &x, &y, &z)) return false;
        if (op.is_inverse) {
          // Unit-quaternion inverse is its conjugate. Orient() normalizes,
          // so conjugating here also handles authored non-unit quaternions.
          x = -x;
          y = -y;
          z = -z;
        }
        Orient(w, x, y, z, m);
        break;
      }
      case XformOpType::Unknown:
        break;
    }
    double tmp[16];
    Multiply(m, matrix, tmp);
    std::memcpy(matrix, tmp, sizeof(tmp));
  }
  return true;
}

bool UsdGeomXform::GetTranslationAtTimecode(double timecode, float* x,
                                            float* y, float* z) const {
  if (!IsValid() || !x || !y || !z) return false;

  Value hold;
  const Value* val = nullptr;
  if (!std::isnan(timecode)) {
    Value v = prim_.GetInterpolatedValue(kIdTranslate(), timecode);
    if (!v.is_empty()) {
      hold = std::move(v);
      val = &hold;
    } else {
      val = prim_.GetValueAtTime(kIdTranslate(), timecode);
    }
  }
  if (!val) val = prim_.GetPropertyValue(kIdTranslate());
  if (!val) return false;

  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  const double* d3 = val->as_double3();
  if (d3) {
    *x = static_cast<float>(d3[0]);
    *y = static_cast<float>(d3[1]);
    *z = static_cast<float>(d3[2]);
    return true;
  }

  return false;
}

bool IsXformable(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  const std::string& type = prim.GetTypeName();
  // Most geometry types are xformable
  return type == "Xform" || type == "Mesh" || type == "Sphere" ||
         type == "Cube" || type == "Cylinder" || type == "Cone" ||
         type == "Capsule" || type == "Camera" || type == "Points" ||
         type == "BasisCurves" || type == "NurbsCurves" ||
         type == "SkelRoot" || type == "Skeleton";
}

bool IsXform(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  return prim.GetTypeName() == "Xform";
}

}  // namespace next
}  // namespace lightusd
