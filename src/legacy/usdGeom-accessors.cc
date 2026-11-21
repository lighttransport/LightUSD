// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Geom* schema typed accessors — split out of usdGeom.cc. Holds the
// GeomMesh/GeomBasisCurves/GeomNurbsCurves/GeomPoints/GPrim get_points/get_normals/
// get_widths/... accessors, GeomMesh::ValidateTopology, GeomSubset::ValidateSubsets,
// and the extent helpers (+ the file-local GetAnimatedArrayValue<T> /
// SetAxisAlignedExtent / ComputeExtentFromPointsWithWidths statics they use).
// Separated from the GeomPrimvar/GPrim primvar machinery (stays in usdGeom.cc) to
// shorten the build critical path (usdGeom.cc was a ~7.4s pole). Accessor methods
// are declared in usdGeom.hh and call GeomPrimvar::get_value etc.
#include <cstring>
#include <sstream>
#include <type_traits>
#include <unordered_set>

#include "pprinter.hh"
#include "value-types.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "xform.hh"
#include "usdGeom.hh"

#include "common-macros.inc"
#include "math-util.inc"
#include "value-pprint.hh"
#include "core/safe-arithmetic.hh"

namespace lightusd { namespace legacy {

// ExpandWithIndices template helpers (GeomMesh::get_normals uses one). Included
// inside namespace lightusd so its unqualified safe::/value:: references resolve,
// matching usdGeom.cc.
#include "usdGeom-primvar-impl.inc"

const std::vector<value::point3f> GeomMesh::get_points(
    double time, value::TimeSampleInterpolationType interp) const {
  std::vector<value::point3f> dst;

  if (!points.authored() || points.is_blocked()) {
    return dst;
  }

  if (points.is_connection()) {
    // Connection-sourced attributes require composition resolution;
    // callers should resolve connections at the Stage/Tydra level.
    return dst;
  }

  if (auto pv = points.get_value()) {
    std::vector<value::point3f> val;
    if (pv.value().get(time, &val, interp)) {
      dst = std::move(val);
    }
  }

  return dst;
}

const std::vector<value::normal3f> GeomMesh::get_normals(
    double time, value::TimeSampleInterpolationType interp) const {
  std::vector<value::normal3f> dst;

  std::string err;
  if (has_primvar("normals")) {
    GeomPrimvar primvar;
    if (!get_primvar("normals", &primvar, &err)) {
      return dst;
    }

    primvar.flatten_with_indices(time, &dst, interp);
    return dst;
  } else if (normals.authored()) {
    if (normals.is_connection()) {
      // Not supported
      return dst;
    } else if (normals.is_blocked()) {
      return dst;
    }

    std::vector<int> indices;
    if (props.count("normals:indices")) {
      Attribute indexAttr = props.at("normals:indices").get_attribute();

      if (indexAttr.is_connection()) {
        // not supported.
        return dst;
      }

      if (!indexAttr.get_value(time, &indices, interp)) {
        // err
        return dst;
      }

    }

    auto pv = normals.get_value();
    if (!pv) return dst;
    std::vector<value::normal3f> value;
    if (!pv.value().get(time, &value, interp)) {
      return dst;
    }

    if (indices.size()) {
      uint32_t elementSize = normals.metas().has_elementSize() ? normals.metas().get_elementSize() : 1;

      std::vector<value::normal3f> expanded_normals;
      auto ret = ExpandWithIndices(value, elementSize, indices, &expanded_normals);

      if (!ret) {
        return dst;
      }

      dst = expanded_normals;
    } else {
      dst = value;
    }
  }

  return dst;
}

const std::vector<value::color3f> GPrim::get_displayColors(
    double time, value::TimeSampleInterpolationType interp) const {
  std::vector<value::color3f> dst;

  std::string err;
  if (has_primvar("displayColor")) {
    GeomPrimvar primvar;
    if (!get_primvar("displayColor", &primvar, &err)) {
      return dst;
    }

    primvar.flatten_with_indices(time, &dst, interp);
  }

  return dst;
}

Interpolation GeomMesh::get_normalsInterpolation() const {
  if (props.count("primvars:normals")) {
    const auto &prop = props.at("primvars:normals");
    if (prop.get_attribute().metas().has_interpolation()) {
      return prop.get_attribute().metas().get_interpolation_enum();
    }
  } else if (normals.metas().has_interpolation()) {
    return normals.metas().get_interpolation_enum();
  }

  return Interpolation::Vertex;  // default 'vertex'
}

Interpolation GPrim::get_displayColorsInterpolation() const {
  if (props.count("primvars:displayColor")) {
    const auto &prop = props.at("primvars:displayColor");
    if (prop.get_attribute().metas().has_interpolation()) {
      return prop.get_attribute().metas().get_interpolation_enum();
    }
  }

  return Interpolation::Vertex;  // default 'vertex'
}

const std::vector<int32_t> GeomMesh::get_faceVertexCounts(double time) const {
  std::vector<int32_t> dst;

  if (!faceVertexCounts.authored() || faceVertexCounts.is_blocked()) {
    return dst;
  }

  if (faceVertexCounts.is_connection()) {
    // Connection-sourced topology attributes are not supported at this level.
    return dst;
  }

  if (auto pv = faceVertexCounts.get_value()) {
    std::vector<int32_t> val;
    if (pv.value().get(time, &val, value::TimeSampleInterpolationType::Held)) {
      dst = std::move(val);
    }
  }
  return dst;
}

const std::vector<int32_t> GeomMesh::get_faceVertexIndices(double time) const {
  std::vector<int32_t> dst;

  if (!faceVertexIndices.authored() || faceVertexIndices.is_blocked()) {
    return dst;
  }

  if (faceVertexIndices.is_connection()) {
    // Connection-sourced topology attributes are not supported at this level.
    return dst;
  }

  if (auto pv = faceVertexIndices.get_value()) {
    std::vector<int32_t> val;
    if (pv.value().get(time, &val, value::TimeSampleInterpolationType::Held)) {
      dst = std::move(val);
    }
  }
  return dst;
}

// --- Convenience getter helper ---
// Extracts a typed array value from an animated attribute, handling
// authored/blocked/connection guards uniformly.
template <typename T, typename AttrT>
static std::vector<T> GetAnimatedArrayValue(
    const AttrT &attr, double time,
    value::TimeSampleInterpolationType interp) {
  std::vector<T> dst;
  if (!attr.authored() || attr.is_blocked() || attr.is_connection()) return dst;
  if (auto pv = attr.get_value()) {
    std::vector<T> val;
    if (pv.value().get(time, &val, interp)) dst = std::move(val);
  }
  return dst;
}

// --- GeomBasisCurves convenience getters ---

const std::vector<value::point3f> GeomBasisCurves::get_points(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::point3f>(points, time, interp);
}
const std::vector<value::normal3f> GeomBasisCurves::get_normals(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::normal3f>(normals, time, interp);
}
const std::vector<int> GeomBasisCurves::get_curveVertexCounts(double time) const {
  return GetAnimatedArrayValue<int>(curveVertexCounts, time, value::TimeSampleInterpolationType::Held);
}
const std::vector<float> GeomBasisCurves::get_widths(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<float>(widths, time, interp);
}

// --- GeomNurbsCurves convenience getters ---

const std::vector<value::point3f> GeomNurbsCurves::get_points(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::point3f>(points, time, interp);
}
const std::vector<value::normal3f> GeomNurbsCurves::get_normals(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::normal3f>(normals, time, interp);
}
const std::vector<int> GeomNurbsCurves::get_curveVertexCounts(double time) const {
  return GetAnimatedArrayValue<int>(curveVertexCounts, time, value::TimeSampleInterpolationType::Held);
}
const std::vector<float> GeomNurbsCurves::get_widths(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<float>(widths, time, interp);
}
const std::vector<int> GeomNurbsCurves::get_order(double time) const {
  return GetAnimatedArrayValue<int>(order, time, value::TimeSampleInterpolationType::Held);
}
const std::vector<double> GeomNurbsCurves::get_knots(double time) const {
  return GetAnimatedArrayValue<double>(knots, time, value::TimeSampleInterpolationType::Held);
}

// --- GeomPoints convenience getters ---

const std::vector<value::point3f> GeomPoints::get_points(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::point3f>(points, time, interp);
}
const std::vector<value::normal3f> GeomPoints::get_normals(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::normal3f>(normals, time, interp);
}
const std::vector<float> GeomPoints::get_widths(double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<float>(widths, time, interp);
}
const std::vector<int64_t> GeomPoints::get_ids(double time) const {
  return GetAnimatedArrayValue<int64_t>(ids, time, value::TimeSampleInterpolationType::Held);
}

// --- GeomPointInstancer convenience getters ---

const std::vector<int32_t> GeomPointInstancer::get_protoIndices(double time) const {
  return GetAnimatedArrayValue<int32_t>(protoIndices, time,
                                        value::TimeSampleInterpolationType::Held);
}
const std::vector<value::point3f> GeomPointInstancer::get_positions(
    double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::point3f>(positions, time, interp);
}
const std::vector<value::float3> GeomPointInstancer::get_scales(
    double time, value::TimeSampleInterpolationType interp) const {
  return GetAnimatedArrayValue<value::float3>(scales, time, interp);
}
const std::vector<value::quath> GeomPointInstancer::get_orientations(
    double time) const {
  return GetAnimatedArrayValue<value::quath>(
      orientations, time, value::TimeSampleInterpolationType::Held);
}
const std::vector<int64_t> GeomPointInstancer::get_ids(double time) const {
  return GetAnimatedArrayValue<int64_t>(ids, time,
                                        value::TimeSampleInterpolationType::Held);
}
const std::vector<int64_t> GeomPointInstancer::get_invisibleIds(double time) const {
  return GetAnimatedArrayValue<int64_t>(invisibleIds, time,
                                        value::TimeSampleInterpolationType::Held);
}
const std::vector<int64_t> GeomPointInstancer::get_inactiveIds() const {
  std::vector<int64_t> dst;
  inactiveIds.get_value(&dst);
  return dst;
}

// --- GeomPointInstancer instance transform / mask computation ---

bool ComputeInstanceTransformsAtTime(
    const GeomPointInstancer &pi, double time,
    value::TimeSampleInterpolationType interp,
    std::vector<value::matrix4d> *out_xforms, std::string *err,
    const std::vector<value::matrix4d> *proto_xforms) {
  if (!out_xforms) {
    if (err) (*err) += "ComputeInstanceTransformsAtTime: out_xforms is null.\n";
    return false;
  }
  out_xforms->clear();

  // protoIndices is the instance-count authority (integer topology => Held).
  const std::vector<int32_t> protoIndices = GetAnimatedArrayValue<int32_t>(
      pi.protoIndices, time, value::TimeSampleInterpolationType::Held);
  const size_t n = protoIndices.size();
  if (n == 0) {
    // No instances (or protoIndices unauthored): valid empty result.
    return true;
  }

  const std::vector<value::point3f> positions =
      GetAnimatedArrayValue<value::point3f>(pi.positions, time, interp);
  const std::vector<value::float3> scales =
      GetAnimatedArrayValue<value::float3>(pi.scales, time, interp);
  // Held for orientations in the preliminary pass (no slerp).
  const std::vector<value::quath> orientations =
      GetAnimatedArrayValue<value::quath>(
          pi.orientations, time, value::TimeSampleInterpolationType::Held);

  // An authored SRT array must be either empty (=> identity defaults) or == n.
  auto check_len = [&](size_t sz, const char *name) -> bool {
    if (sz != 0 && sz != n) {
      if (err) {
        (*err) += fmt::format(
            "PointInstancer `{}` array length ({}) does not match protoIndices "
            "length ({}).\n",
            name, sz, n);
      }
      return false;
    }
    return true;
  };
  if (!check_len(positions.size(), "positions")) return false;
  if (!check_len(scales.size(), "scales")) return false;
  if (!check_len(orientations.size(), "orientations")) return false;
  if (proto_xforms && proto_xforms->size() != 0 && proto_xforms->size() != n) {
    if (err) {
      (*err) += fmt::format(
          "PointInstancer proto_xforms length ({}) does not match protoIndices "
          "length ({}).\n",
          proto_xforms->size(), n);
    }
    return false;
  }

  out_xforms->resize(n);
  for (size_t i = 0; i < n; i++) {
    // Scale (diagonal).
    value::matrix4d S = value::matrix4d::identity();
    if (!scales.empty()) {
      S.m[0][0] = double(scales[i][0]);
      S.m[1][1] = double(scales[i][1]);
      S.m[2][2] = double(scales[i][2]);
    }
    // Rotation.
    value::matrix4d R = value::matrix4d::identity();
    if (!orientations.empty()) {
      R = to_matrix(orientations[i]);
    }
    // Translation (row-vector convention => last row).
    value::matrix4d T = value::matrix4d::identity();
    if (!positions.empty()) {
      T.m[3][0] = double(positions[i][0]);
      T.m[3][1] = double(positions[i][1]);
      T.m[3][2] = double(positions[i][2]);
    }

    // p * S * R * T
    value::matrix4d local = value::Mult(value::Mult(S, R), T);
    if (proto_xforms && !proto_xforms->empty()) {
      // Prototype xform applied first (prototype-local => instancer space).
      local = value::Mult((*proto_xforms)[i], local);
    }
    (*out_xforms)[i] = local;
  }

  return true;
}

bool ComputeMaskAtTime(const GeomPointInstancer &pi, double time,
                       std::vector<bool> *out_mask, std::string *err) {
  if (!out_mask) {
    if (err) (*err) += "ComputeMaskAtTime: out_mask is null.\n";
    return false;
  }
  out_mask->clear();

  const std::vector<int32_t> protoIndices = GetAnimatedArrayValue<int32_t>(
      pi.protoIndices, time, value::TimeSampleInterpolationType::Held);
  const size_t n = protoIndices.size();
  if (n == 0) {
    return true;
  }

  // Per-instance ids (optional). When absent, the implicit id is the index.
  const std::vector<int64_t> ids = GetAnimatedArrayValue<int64_t>(
      pi.ids, time, value::TimeSampleInterpolationType::Held);

  // Build the set of masked ids: invisibleIds (animatable) + inactiveIds (uniform).
  std::unordered_set<int64_t> masked;
  {
    const std::vector<int64_t> invisibleIds = GetAnimatedArrayValue<int64_t>(
        pi.invisibleIds, time, value::TimeSampleInterpolationType::Held);
    masked.insert(invisibleIds.begin(), invisibleIds.end());

    std::vector<int64_t> inactiveIds;
    if (pi.inactiveIds.get_value(&inactiveIds)) {
      masked.insert(inactiveIds.begin(), inactiveIds.end());
    }
  }

  out_mask->assign(n, true);
  if (masked.empty()) {
    return true;
  }

  for (size_t i = 0; i < n; i++) {
    const int64_t id =
        (i < ids.size()) ? ids[i] : static_cast<int64_t>(i);
    if (masked.count(id)) {
      (*out_mask)[i] = false;
    }
  }

  return true;
}

std::vector<value::token> GeomMesh::get_joints() const {
  constexpr auto kSkelJoints = "skel:joints";
  std::vector<value::token> dst;

  {
    // lookup `skel:joints` prop
    if (!props.count(kSkelJoints)) {
      return dst;
    }

    const auto &prop = props.at(kSkelJoints);
    if (prop.get_attribute().is_uniform() && prop.get_attribute().type_name() == "token[]") {

      if (!prop.get_attribute().get_value(&dst)) {
        return dst;
      }

      return dst;

    }
    DCOUT("`skel:joints` must be uniform token[] attribute, but got " << prop.value_type_name() << " (or Relationship))");
  }
  return dst;
}

// --- H2: ValidateTopology ---

bool GeomMesh::ValidateTopology(std::string *err, double time) const {
  auto fvc = get_faceVertexCounts(time);
  auto fvi = get_faceVertexIndices(time);
  auto pts = get_points(time);

  if (fvc.empty() && fvi.empty()) {
    // No topology authored
    return true;
  }

  // 1. sum(faceVertexCounts) == faceVertexIndices.size()
  size_t totalVerts = 0;
  for (size_t i = 0; i < fvc.size(); i++) {
    if (fvc[i] < 3) {
      if (err) {
        (*err) += fmt::format("faceVertexCounts[{}] = {} is less than 3.\n", i, fvc[i]);
      }
      return false;
    }
    if (totalVerts > (std::numeric_limits<size_t>::max)() - static_cast<size_t>(fvc[i])) {
      if (err) { (*err) += "faceVertexCounts sum overflow.\n"; }
      return false;
    }
    totalVerts += static_cast<size_t>(fvc[i]);
  }

  if (totalVerts != fvi.size()) {
    if (err) {
      (*err) += fmt::format("sum(faceVertexCounts) = {} != faceVertexIndices.size() = {}.\n",
        totalVerts, fvi.size());
    }
    return false;
  }

  // 2. All faceVertexIndices in range [0, points.size())
  if (!pts.empty()) {
    for (size_t i = 0; i < fvi.size(); i++) {
      if (fvi[i] < 0 || static_cast<size_t>(fvi[i]) >= pts.size()) {
        if (err) {
          (*err) += fmt::format("faceVertexIndices[{}] = {} is out of range [0, {}).\n",
            i, fvi[i], pts.size());
        }
        return false;
      }
    }
  }

  // 3. Subdivision surface validation
  size_t numFaces = fvc.size();

  // cornerIndices validation
  if (props.count("cornerIndices") && props.count("cornerSharpnesses")) {
    const auto &ciProp = props.at("cornerIndices");
    const auto &csProp = props.at("cornerSharpnesses");
    std::vector<int32_t> ci_vals;
    std::vector<float> cs_vals;
    if (ciProp.get_attribute().get_value(&ci_vals) &&
        csProp.get_attribute().get_value(&cs_vals)) {
      if (ci_vals.size() != cs_vals.size()) {
        if (err) {
          (*err) += fmt::format("cornerIndices.size() = {} != cornerSharpnesses.size() = {}.\n",
            ci_vals.size(), cs_vals.size());
        }
        return false;
      }
      if (!pts.empty()) {
        for (size_t i = 0; i < ci_vals.size(); i++) {
          if (ci_vals[i] < 0 || static_cast<size_t>(ci_vals[i]) >= pts.size()) {
            if (err) {
              (*err) += fmt::format("cornerIndices[{}] = {} is out of range [0, {}).\n",
                i, ci_vals[i], pts.size());
            }
            return false;
          }
        }
      }
    }
  }

  // creaseIndices/creaseLengths/creaseSharpnesses validation
  if (props.count("creaseIndices") && props.count("creaseLengths")) {
    const auto &crIdxProp = props.at("creaseIndices");
    const auto &crLenProp = props.at("creaseLengths");
    std::vector<int32_t> cr_idx;
    std::vector<int32_t> cr_len;
    if (crIdxProp.get_attribute().get_value(&cr_idx) &&
        crLenProp.get_attribute().get_value(&cr_len)) {
      size_t totalCreaseVerts = 0;
      for (const auto &cl : cr_len) {
        totalCreaseVerts += static_cast<size_t>(cl);
      }
      if (totalCreaseVerts != cr_idx.size()) {
        if (err) {
          (*err) += fmt::format("sum(creaseLengths) = {} != creaseIndices.size() = {}.\n",
            totalCreaseVerts, cr_idx.size());
        }
        return false;
      }
      if (props.count("creaseSharpnesses")) {
        const auto &crShProp = props.at("creaseSharpnesses");
        std::vector<float> cr_sharp;
        if (crShProp.get_attribute().get_value(&cr_sharp)) {
          if (cr_len.size() != cr_sharp.size()) {
            if (err) {
              (*err) += fmt::format("creaseLengths.size() = {} != creaseSharpnesses.size() = {}.\n",
                cr_len.size(), cr_sharp.size());
            }
            return false;
          }
        }
      }
    }
  }

  // holeIndices validation
  if (props.count("holeIndices")) {
    const auto &hiProp = props.at("holeIndices");
    std::vector<int32_t> hi_vals;
    if (hiProp.get_attribute().get_value(&hi_vals)) {
      for (size_t i = 0; i < hi_vals.size(); i++) {
        if (hi_vals[i] < 0 || static_cast<size_t>(hi_vals[i]) >= numFaces) {
          if (err) {
            (*err) += fmt::format("holeIndices[{}] = {} is out of range [0, {}).\n",
              i, hi_vals[i], numFaces);
          }
          return false;
        }
      }
    }
  }

  return true;
}

// --- H1: ComputeExtent ---

// Helper: set axis-aligned extent for Cone/Cylinder/Capsule
static void SetAxisAlignedExtent(Axis axis, float radial, float axial_lo, float axial_hi, Extent *extent) {
  if (axis == Axis::X) {
    *extent = Extent(value::float3{{axial_lo, -radial, -radial}}, value::float3{{axial_hi, radial, radial}});
  } else if (axis == Axis::Y) {
    *extent = Extent(value::float3{{-radial, axial_lo, -radial}}, value::float3{{radial, axial_hi, radial}});
  } else {
    *extent = Extent(value::float3{{-radial, -radial, axial_lo}}, value::float3{{radial, radial, axial_hi}});
  }
}

// Helper: compute extent from points expanded by max half-width (for curves)
static void ComputeExtentFromPointsWithWidths(
    const std::vector<value::point3f> &pts,
    const std::vector<float> &widths,
    Extent *extent) {
  float maxHalfW = 0.0f;
  for (float w : widths) {
    float hw = w * 0.5f;
    if (hw > maxHalfW) maxHalfW = hw;
  }
  Extent e;
  for (const auto &p : pts) {
    e.union_with(value::float3{{p.x - maxHalfW, p.y - maxHalfW, p.z - maxHalfW}});
    e.union_with(value::float3{{p.x + maxHalfW, p.y + maxHalfW, p.z + maxHalfW}});
  }
  *extent = e;
}

bool ComputeExtent(const GeomMesh &mesh, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  auto pts = mesh.get_points(time);
  if (pts.empty()) {
    if (err) (*err) = "No points in mesh.\n";
    return false;
  }
  Extent e;
  for (const auto &p : pts) {
    e.union_with(p);
  }
  *extent = e;
  return true;
}

bool ComputeExtent(const GeomPoints &geom, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  auto pts = geom.get_points(time);
  if (pts.empty()) {
    if (err) (*err) = "No points.\n";
    return false;
  }
  auto ws = geom.get_widths(time);
  Extent e;
  for (size_t i = 0; i < pts.size(); i++) {
    float halfW = (i < ws.size()) ? ws[i] * 0.5f : 0.0f;
    e.union_with(value::float3{{pts[i].x - halfW, pts[i].y - halfW, pts[i].z - halfW}});
    e.union_with(value::float3{{pts[i].x + halfW, pts[i].y + halfW, pts[i].z + halfW}});
  }
  *extent = e;
  return true;
}

bool ComputeExtent(const GeomSphere &sphere, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  (void)err;
  double r = 2.0;
  sphere.radius.get_value().get(time, &r);
  float rf = static_cast<float>(r);
  *extent = Extent(value::float3{{-rf, -rf, -rf}}, value::float3{{rf, rf, rf}});
  return true;
}

bool ComputeExtent(const GeomCube &cube, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  (void)err;
  double s = 2.0;
  cube.size.get_value().get(time, &s);
  float h = static_cast<float>(s) * 0.5f;
  *extent = Extent(value::float3{{-h, -h, -h}}, value::float3{{h, h, h}});
  return true;
}

bool ComputeExtent(const GeomCone &cone, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  (void)err;
  double h = 2.0, r = 1.0;
  cone.height.get_value().get(time, &h);
  cone.radius.get_value().get(time, &r);
  SetAxisAlignedExtent(cone.axis.get_value(), static_cast<float>(r), 0.0f, static_cast<float>(h), extent);
  return true;
}

bool ComputeExtent(const GeomCylinder &cylinder, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  (void)err;
  double h = 2.0, r = 1.0;
  cylinder.height.get_value().get(time, &h);
  cylinder.radius.get_value().get(time, &r);
  float hh = static_cast<float>(h) * 0.5f;
  SetAxisAlignedExtent(cylinder.axis.get_value(), static_cast<float>(r), -hh, hh, extent);
  return true;
}

bool ComputeExtent(const GeomCapsule &capsule, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  (void)err;
  double h = 2.0, r = 0.5;
  capsule.height.get_value().get(time, &h);
  capsule.radius.get_value().get(time, &r);
  float rf = static_cast<float>(r);
  float hh = static_cast<float>(h) * 0.5f + rf;  // extend by radius for hemicaps
  SetAxisAlignedExtent(capsule.axis.get_value(), rf, -hh, hh, extent);
  return true;
}

bool ComputeExtent(const GeomBasisCurves &curves, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  auto pts = curves.get_points(time);
  if (pts.empty()) { if (err) (*err) = "No points in curves.\n"; return false; }
  ComputeExtentFromPointsWithWidths(pts, curves.get_widths(time), extent);
  return true;
}

bool ComputeExtent(const GeomNurbsCurves &curves, Extent *extent,
    double time, std::string *err) {
  if (!extent) return false;
  auto pts = curves.get_points(time);
  if (pts.empty()) { if (err) (*err) = "No points in NURBS curves.\n"; return false; }
  ComputeExtentFromPointsWithWidths(pts, curves.get_widths(time), extent);
  return true;
}

// static
bool GeomSubset::ValidateSubsets(
    const std::vector<const GeomSubset *> &subsets,
    const size_t elementCount,
    const FamilyType &familyType, std::string *err) {

  if (subsets.empty()) {
    return true;
  }

  // All subsets must have the same elementType.
  GeomSubset::ElementType elementType = subsets[0]->elementType.get_value();
  for (const auto psubset : subsets) {
    if (psubset->elementType.get_value() != elementType) {
      if (err) {
        (*err) = fmt::format("GeomSubset {}'s elementType must be `{}`, but got `{}`.\n",
          psubset->name, to_string(elementType), to_string(psubset->elementType.get_value()));
      }

      return false;
    }
  }

  std::set<int32_t> indicesInFamily;

  bool valid = true;
  std::stringstream ss;

  // Note: Currently validates default-value indices only.
  // TimeSampled indices would need per-frame validation at the application level.
  for (const auto psubset : subsets) {
    Animatable<std::vector<int32_t>> indices;
    if (!psubset->indices.get_value(&indices)) {
      ss << fmt::format("GeomSubset {}'s indices is not value Attribute. Connection or ValueBlock?\n",
          psubset->name);

      valid = false;
    }

    if (indices.is_blocked()) {
      ss << fmt::format("GeomSubset {}'s indices is Value Blocked.\n", psubset->name);
      valid = false;
    }

    if (indices.is_timesamples() || !indices.has_value()) {
      ss << fmt::format("ValidateSubsets: TimeSampled GeomSubset.indices is not yet supported.\n");
      valid = false;
    }

    std::vector<int32_t> subsetIndices;
    if (!indices.get_scalar(&subsetIndices)) {
      ss << fmt::format("ValidateSubsets: Internal error. Failed to get GeomSubset.indices.\n");
      valid = false;
    }

    for (const int32_t index : subsetIndices) {
      if (!indicesInFamily.insert(index).second && (familyType != FamilyType::Unrestricted)) {
        ss << fmt::format("Found overlapping index {} in GeomSubset `{}`\n", index, psubset->name);
        valid = false;
      }
    }
  }


  // Make sure every index appears exactly once if it's a partition.
  if ((familyType == FamilyType::Partition) && (indicesInFamily.size() != elementCount)) {
    ss << fmt::format("ValidateSubsets: The number of unique indices {} must be equal to input elementCount {}\n", indicesInFamily.size(), elementCount);
    valid = false;
  }

  // Ensure that non-empty indices are in the range [0, faceCount).
  if (!indicesInFamily.empty()) {
    size_t maxIndex = static_cast<size_t>(*indicesInFamily.rbegin());
    int minIndex = *indicesInFamily.begin();

    if (maxIndex >= elementCount) {
      ss << fmt::format("ValidateSubsets: All indices must be in range [0, elementSize {}), but one or more indices are greater than elementSize. Maximum = {}\n", elementCount, maxIndex);
      valid = false;
    }

    if (minIndex < 0) {
      ss << fmt::format("ValidateSubsets: Found one or more indices that are less than 0. Minumum = {}\n", minIndex);
      valid = false;
    }
  }

  if (!valid) {
    if (err) {
      (*err) += ss.str();
    }
  }

  return valid;

}

}  // namespace legacy
}  // namespace lightusd
