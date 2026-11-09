// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Shared transform evaluation for core and render consumers.
#include "xform-eval.hh"
#include "../stage/stage.hh"
#include "../layer/prim-spec.hh"
#include "../schema/schema-registry.hh"
#include <cmath>
#include <cstring>
#include <string_view>

#include "../c/lightusd-core-c.h"  // lu_mat4_*

namespace lightusd {
namespace next {

namespace {

const ::lightusd::next::PropNameId& kXformOpOrder() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("xformOpOrder");
  return id;
}

}  // namespace

//
// Transform operations
//

namespace {

void SetIdentity(double* m) { lu_mat4_identity(m); }

// Unknown schemas may be external Xformable types; preserve their authored
// operations until their ancestry is available. Untyped prims and known
// non-Xformable types do not contribute a transform to their descendants.
bool MayBeXformable(const UsdPrim& prim) {
  const std::string type = prim.GetTypeName();
  if (type.empty()) return false;
  const SchemaRegistry& registry = GetSchemaRegistry();
  return !registry.HasParentEntry(type) || registry.InheritsFrom(type, "Xformable");
}

// --- Bit-exact xform evaluation -------------------------------------------
//
// To produce world/local matrices that are byte-identical to the legacy
// lightusd evaluator (Xformable::EvaluateXformOps in src/xform.cc), the helpers
// below replicate its exact arithmetic: the `sin_pi`/`cos_pi` reduced-argument
// trig (NOT std::sin/cos(deg * pi/180)), the per-op matrix layouts, and the
// `cm = op * cm` / `world = local * parent` (row-vector) multiply order.

void MakeTranslationD(double* m, double x, double y, double z) {
  lu_mat4_translation(m, x, y, z);
}

void MakeScaleD(double* m, double x, double y, double z) {
  lu_mat4_scale(m, x, y, z);
}

// dst = a * b (row-vector convention), via the existing bit-compatible multiply.
void MatMulD(double* dst, const double* a, const double* b) {
  lu_mat4_mul(dst, a, b);
}

// Read a property either at the default value (NaN time) or held at a specific
// time sample. A NaN time keeps the exact default-value path (byte-identical to
// the previous, time-unaware behaviour).
const Value* PropAtTime(const UsdPrim& prim,
                        ::lightusd::next::PropNameId name_id,
                        double time) {
  if (!name_id.is_valid()) return nullptr;
  if (std::isnan(time)) return prim.GetPropertyValue(name_id);
  // Linear interpolation between samples (pxr semantics); held/default
  // fallback. Scratch is per-thread; callers consume the pointer before the
  // next PropAtTime call (single-live-pointer pattern in this TU).
  static thread_local Value scratch;
  Value v = prim.GetInterpolatedValue(name_id, time);
  if (!v.is_empty()) {
    scratch = std::move(v);
    return &scratch;
  }
  return prim.GetValueAtTime(name_id, time);
}

// Read a 3-component op value (translate/scale/rotate) as double, trying
// float3 then double3 (matches the legacy evaluator's exact-type promotion),
// then the converting read for authored half3 (raw half-bit lanes).
bool ReadVec3D(const UsdPrim& prim, ::lightusd::next::PropNameId name_id,
               double v[3],
               double time) {
  const Value* val = PropAtTime(prim, name_id, time);
  if (!val) return false;
  if (const float* f = val->as_float3()) {
    v[0] = double(f[0]); v[1] = double(f[1]); v[2] = double(f[2]);
    return true;
  }
  if (const double* d = val->as_double3()) {
    v[0] = d[0]; v[1] = d[1]; v[2] = d[2];
    return true;
  }
  float h[3];
  if (val->to_float3(h)) {
    v[0] = double(h[0]); v[1] = double(h[1]); v[2] = double(h[2]);
    return true;
  }
  return false;
}

bool ReadFloat1D(const UsdPrim& prim, ::lightusd::next::PropNameId name_id,
                 double* out,
                 double time) {
  const Value* val = PropAtTime(prim, name_id, time);
  if (!val) return false;
  if (const float* f = val->as_float()) { *out = double(*f); return true; }
  if (const double* d = val->as_double()) { *out = *d; return true; }
  float h = 0.0f;
  if (val->to_float(&h)) { *out = double(h); return true; }
  return false;
}

}  // namespace

namespace {

// Build a single-axis rotation (degrees) for axis 'X'/'Y'/'Z'.
void MakeRotAxisD(double* m, char axis, double deg) {
  lu_mat4_rot_axis(m, axis, deg);
}

// Quaternion (w,x,y,z) -> row-vector rotation matrix, matching
// lightusd::to_matrix3x3(quatd) embedded in a 4x4.
void MakeOrientD(double* m, double w, double x, double y, double z) {
  lu_mat4_orient(m, w, x, y, z);
}

void Invert4x4D(const double* a, double* out) { lu_mat4_invert(a, out); }

// Strip "xformOp:" prefix and any ":suffix"; returns the bare op name
// (e.g. "rotateXYZ"). Returns empty for non-xformOp tokens.
std::string_view XformOpName(std::string_view tok) {
  constexpr std::string_view kPfx = "xformOp:";
  if (tok.rfind(kPfx, 0) != 0) return std::string_view();
  std::string_view rest = tok.substr(kPfx.size());
  size_t colon = rest.find(':');
  if (colon != std::string::npos) rest = rest.substr(0, colon);
  return rest;
}

// Bit-exact local matrix from authored xformOpOrder, replicating
// lightusd::Xformable::EvaluateXformOps (row-vector, cm = op * cm). Sets
// *reset when the op list begins with "!resetXformStack!".
bool EvalLocalXformD(const UsdPrim& prim, double* out, bool* reset, double time) {
  if (reset) *reset = false;
  SetIdentity(out);
  if (!prim.IsValid()) return false;
  if (!MayBeXformable(prim)) return true;

  // xformOpOrder is uniform (not time-sampled) -> default value.
  const Value* orderv = prim.GetPropertyValue(kXformOpOrder());
  auto order =
      orderv ? orderv->as_token_array() : nullptr;
  if (!order || order->empty()) return true;  // no ops -> identity

  for (size_t i = 0; i < order->size(); ++i) {
    std::string_view tok = (*order)[i];
    bool inverted = false;
    if (tok.rfind("!invert!", 0) == 0) {
      inverted = true;
      tok.remove_prefix(8);
    }
    if (tok == "!resetXformStack!") {
      if (i == 0 && reset) *reset = true;
      continue;
    }
    const std::string_view op = XformOpName(tok);
    if (op.empty()) continue;
    const auto name_id = ::lightusd::next::GetPropNameTable().find(tok);

    double m[16];
    SetIdentity(m);

    if (op == "transform") {
      double mm[16];
      bool got = false;
      if (const Value* val = PropAtTime(prim, name_id, time)) {
        if (const double* md = val->as_matrix4d()) {
          std::memcpy(mm, md, 16 * sizeof(double));
          got = true;
        } else if (const float* mf = val->as_matrix4f()) {
          for (int e = 0; e < 16; ++e) mm[e] = double(mf[e]);
          got = true;
        }
      }
      if (got) {
        if (inverted) Invert4x4D(mm, m);
        else std::memcpy(m, mm, 16 * sizeof(double));
      }
    } else if (op == "translate") {
      double v[3] = {0, 0, 0};
      ReadVec3D(prim, name_id, v, time);
      if (inverted) { v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2]; }
      MakeTranslationD(m, v[0], v[1], v[2]);
    } else if (op == "scale") {
      double v[3] = {1, 1, 1};
      ReadVec3D(prim, name_id, v, time);
      if (inverted) { v[0] = 1.0 / v[0]; v[1] = 1.0 / v[1]; v[2] = 1.0 / v[2]; }
      MakeScaleD(m, v[0], v[1], v[2]);
    } else if (op == "orient") {
      const Value* val = PropAtTime(prim, name_id, time);
      double q[4] = {1, 0, 0, 0};  // w,x,y,z
      if (val) {
        float h[4];
        if (const float* f = val->as_float4()) {
          q[0] = f[0]; q[1] = f[1]; q[2] = f[2]; q[3] = f[3];
        } else if (const double* d = val->as_double4()) {
          q[0] = d[0]; q[1] = d[1]; q[2] = d[2]; q[3] = d[3];
        } else if (val->to_float4(h)) {  // authored quath (half-bit lanes)
          q[0] = h[0]; q[1] = h[1]; q[2] = h[2]; q[3] = h[3];
        }
      }
      if (inverted) { q[1] = -q[1]; q[2] = -q[2]; q[3] = -q[3]; }
      MakeOrientD(m, q[0], q[1], q[2], q[3]);
    } else if (op == "rotateX" || op == "rotateY" || op == "rotateZ") {
      double a = 0;
      ReadFloat1D(prim, name_id, &a, time);
      if (inverted) a = -a;
      MakeRotAxisD(m, op[6], a);  // 'X'/'Y'/'Z'
    } else if (op.size() == 9 && op.rfind("rotate", 0) == 0) {
      // rotateXYZ / rotateZYX / ... : product of single-axis rotations in the
      // letter order (m = m * R_axis), exactly as XformEvaluator does.
      double v[3] = {0, 0, 0};
      ReadVec3D(prim, name_id, v, time);
      const char ax[3] = {op[6], op[7], op[8]};
      // map axis letter -> angle component (X->v[0], Y->v[1], Z->v[2]).
      auto angleFor = [&](char c) -> double {
        return c == 'X' ? v[0] : (c == 'Y' ? v[1] : v[2]);
      };
      SetIdentity(m);
      if (!inverted) {
        for (int k = 0; k < 3; ++k) {
          double r[16], tmp[16];
          MakeRotAxisD(r, ax[k], angleFor(ax[k]));
          MatMulD(tmp, m, r);
          std::memcpy(m, tmp, 16 * sizeof(double));
        }
      } else {
        for (int k = 2; k >= 0; --k) {
          double r[16], tmp[16];
          MakeRotAxisD(r, ax[k], -angleFor(ax[k]));
          MatMulD(tmp, m, r);
          std::memcpy(m, tmp, 16 * sizeof(double));
        }
      }
    } else {
      continue;  // unknown op -> identity contribution
    }

    // cm = op * cm
    double tmp[16];
    MatMulD(tmp, m, out);
    std::memcpy(out, tmp, 16 * sizeof(double));
  }
  return true;
}

}  // namespace

bool ComputeLocalTransform(const UsdPrim& prim, double* matrix16, double time) {
  if (!prim.IsValid() || !matrix16) return false;
  bool reset = false;
  return EvalLocalXformD(prim, matrix16, &reset, time);
}

bool ComputeLocalTransform(const UsdPrim& prim, float* matrix16, double time) {
  if (!matrix16) return false;
  double dmat[16];
  if (!ComputeLocalTransform(prim, dmat, time)) return false;
  for (int i = 0; i < 16; ++i) matrix16[i] = static_cast<float>(dmat[i]);
  return true;
}

bool ComputeWorldTransform(const Stage& stage, const UsdPrim& prim, double* matrix16, double time) {
  if (!prim.IsValid() || !matrix16) return false;
  SetIdentity(matrix16);

  // Collect this prim and its ancestors (leaf -> root).
  std::vector<UsdPrim> chain;
  std::string path = prim.GetPath().str();
  while (!path.empty() && path != "/") {
    UsdPrim p = stage.GetPrimAtPath(path);
    if (p.IsValid()) chain.push_back(p);
    size_t last_slash = path.rfind('/');
    if (last_slash == 0) path = "/";
    else if (last_slash != std::string::npos) path = path.substr(0, last_slash);
    else break;
  }

  // world = L_leaf * L_parent * ... * L_root  (row-vector: world = local *
  // parent_world). resetXformStack on a prim ignores its ancestors' stack.
  for (auto it = chain.begin(); it != chain.end(); ++it) {
    double local[16];
    bool reset = false;
    EvalLocalXformD(*it, local, &reset, time);
    double tmp[16];
    MatMulD(tmp, matrix16, local);
    std::memcpy(matrix16, tmp, 16 * sizeof(double));
    if (reset) break;  // this prim resets the parent stack
  }
  return true;
}

bool ComputeWorldTransform(const Stage& stage, const UsdPrim& prim, float* matrix16, double time) {
  if (!matrix16) return false;
  double dmat[16];
  if (!ComputeWorldTransform(stage, prim, dmat, time)) return false;
  for (int i = 0; i < 16; ++i) matrix16[i] = static_cast<float>(dmat[i]);
  return true;
}

bool HasResetXformStack(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  if (!MayBeXformable(prim)) return false;
  const Value* orderv = prim.GetPropertyValue(kXformOpOrder());
  auto order =
      orderv ? orderv->as_token_array() : nullptr;
  if (!order || order->empty()) return false;
  return (*order)[0] == "!resetXformStack!";
}


}  // namespace next
}  // namespace lightusd
