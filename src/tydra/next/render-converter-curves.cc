// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Curves and instancers conversion

#include "render-converter.hh"
#include "next/eval/value-clip.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace lightusd {
namespace tydra {
namespace next {

namespace {

void ComputePointBounds(const FloatChunked& points, Float3* bbox_min,
                        Float3* bbox_max, bool* has_bbox) {
  if (!bbox_min || !bbox_max || !has_bbox) return;
  *has_bbox = false;
  if (points.size() < 3) return;
  *bbox_min = Float3(1e30f, 1e30f, 1e30f);
  *bbox_max = Float3(-1e30f, -1e30f, -1e30f);
  const size_t point_count = points.size() / 3;
  for (size_t i = 0; i < point_count; ++i) {
    const float x = points[i * 3 + 0];
    const float y = points[i * 3 + 1];
    const float z = points[i * 3 + 2];
    bbox_min->x = std::min(bbox_min->x, x);
    bbox_min->y = std::min(bbox_min->y, y);
    bbox_min->z = std::min(bbox_min->z, z);
    bbox_max->x = std::max(bbox_max->x, x);
    bbox_max->y = std::max(bbox_max->y, y);
    bbox_max->z = std::max(bbox_max->z, z);
  }
  *has_bbox = true;
}

Interpolation ParsePrimvarInterp(const std::string& s) {
  if (s == "constant") return Interpolation::Constant;
  if (s == "uniform") return Interpolation::Uniform;
  if (s == "faceVarying") return Interpolation::FaceVarying;
  if (s == "varying") return Interpolation::Varying;
  return Interpolation::Vertex;
}

}  // namespace


//
// Curves conversion (BasisCurves / NurbsCurves / HermiteCurves)
//

namespace {

// Cubic blending weights for control points [P0,P1,P2,P3] at span-local
// parameter t in [0,1]. Standard uniform basis matrices.
void EvalCubicBasisWeights(CurveBasis basis, float t, float w[4]) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  switch (basis) {
    case CurveBasis::BSpline:
      w[0] = (1.0f - 3.0f * t + 3.0f * t2 - t3) / 6.0f;
      w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
      w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
      w[3] = t3 / 6.0f;
      break;
    case CurveBasis::CatmullRom:
      w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
      w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
      w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
      w[3] = 0.5f * (t3 - t2);
      break;
    case CurveBasis::Bezier:
    default: {
      const float s = 1.0f - t;
      w[0] = s * s * s;
      w[1] = 3.0f * t * s * s;
      w[2] = 3.0f * t2 * s;
      w[3] = t3;
      break;
    }
  }
}

// Linear sample of a per-curve scalar channel (e.g. widths) at normalized
// curve parameter u01 in [0,1]. Periodic channels wrap so the closing point
// maps back to element 0.
float SampleChannelLinear(const float* vals, size_t count, float u01,
                          bool periodic, size_t stride = 1,
                          size_t component = 0) {
  if (!vals || count == 0) return 0.0f;
  if (count == 1) return vals[component];
  u01 = std::min(std::max(u01, 0.0f), 1.0f);
  if (periodic) {
    const float f = u01 * static_cast<float>(count);
    const size_t i = static_cast<size_t>(f) % count;
    const size_t j = (i + 1) % count;
    const float frac = f - std::floor(f);
    return vals[i * stride + component] * (1.0f - frac) +
           vals[j * stride + component] * frac;
  }
  const float f = u01 * static_cast<float>(count - 1);
  const size_t i = static_cast<size_t>(f);
  if (i >= count - 1) return vals[(count - 1) * stride + component];
  const float frac = f - static_cast<float>(i);
  return vals[i * stride + component] * (1.0f - frac) +
         vals[(i + 1) * stride + component] * frac;
}

constexpr int kMaxNurbsDegree = 9;

// NURBS curve point at parameter u via de Boor's algorithm.
// `knots` must have ncv + degree + 1 non-decreasing entries; u should lie in
// [knots[degree], knots[ncv]].
bool DeBoorEval(const float* cvs, size_t ncv, const float* knots, int degree,
                float u, float out[3]) {
  if (degree < 1 || degree > kMaxNurbsDegree ||
      ncv < static_cast<size_t>(degree) + 1) {
    return false;
  }
  const int n = static_cast<int>(ncv) - 1;
  int k = degree;
  if (u >= knots[n + 1]) {
    k = n;
  } else if (u > knots[degree]) {
    while (k < n && !(u >= knots[k] && u < knots[k + 1])) ++k;
  }
  float d[kMaxNurbsDegree + 1][3];
  for (int j = 0; j <= degree; ++j) {
    const size_t idx = static_cast<size_t>(j + k - degree);
    d[j][0] = cvs[idx * 3 + 0];
    d[j][1] = cvs[idx * 3 + 1];
    d[j][2] = cvs[idx * 3 + 2];
  }
  for (int r = 1; r <= degree; ++r) {
    for (int j = degree; j >= r; --j) {
      const float tj = knots[j + k - degree];
      const float denom = knots[j + 1 + k - r] - tj;
      float alpha = 0.0f;
      if (denom > 0.0f) {
        alpha = (u - tj) / denom;
        alpha = std::min(std::max(alpha, 0.0f), 1.0f);
      }
      d[j][0] = (1.0f - alpha) * d[j - 1][0] + alpha * d[j][0];
      d[j][1] = (1.0f - alpha) * d[j - 1][1] + alpha * d[j][1];
      d[j][2] = (1.0f - alpha) * d[j - 1][2] + alpha * d[j][2];
    }
  }
  out[0] = d[degree][0];
  out[1] = d[degree][1];
  out[2] = d[degree][2];
  return true;
}

// Read a float-ish array attribute, converting double-backed data (e.g.
// NurbsCurves knots/ranges which are double[]/double2[]).
bool ReadFloatsFlexible(const UsdPrim& prim, const char* name, double time,
                        std::vector<float>* out) {
  ValueArrayRead<float> f;
  if (ReadFloatArray(prim, name, time, &f) && !f.empty()) {
    out->assign(f.begin(), f.end());
    return true;
  }
  const Value* v = GetAttribute(prim, name);
  if (!v) return false;
  ::lightusd::next::ArrayScratch<double> scratch;
  ::lightusd::next::ArrayView<double> view;
  if (!::lightusd::next::GetDoubleArrayView(*v, &scratch, &view) ||
      view.empty()) {
    return false;
  }
  out->clear();
  out->reserve(view.size);
  for (size_t i = 0; i < view.size; ++i) {
    out->push_back(static_cast<float>(view[i]));
  }
  return true;
}

// Per-curve tessellation plan.
struct CurveTessPlan {
  uint32_t n = 0;            // authored control point count
  uint32_t nsegs = 0;        // cubic/NURBS spans (unused for linear)
  bool linear = false;       // passthrough as polyline (also fallback mode)
  bool periodic = false;
  bool pinned = false;       // duplicate end CVs (bspline x2 / catmullRom x1)
  int degree = 0;            // NURBS only
  size_t knot_offset = 0;    // NURBS only, into the flattened knots array
  float u0 = 0.0f;           // NURBS eval domain
  float u1 = 0.0f;
  uint32_t varying_count = 0;  // varying-interp elements owned by this curve
};

}  // namespace

bool RenderSceneConverter::ConvertCurves(const UsdPrim& prim,
                                         RenderCurves* out) {
  const std::string type_name =
      prim.IsValid() ? prim.GetTypeName() : std::string();
  if (!out || (type_name != "BasisCurves" && type_name != "NurbsCurves" &&
               type_name != "HermiteCurves")) {
    SetLastError("Invalid curves prim");
    return false;
  }
  out->is_nurbs = (type_name == "NurbsCurves");
  out->is_hermite = (type_name == "HermiteCurves");
  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();

  ValueArrayRead<int32_t> counts;
  if (!ReadIntArray(prim, "curveVertexCounts", config_.time_code, &counts) ||
      counts.empty()) {
    SetLastError("Invalid curves.curveVertexCounts data");
    return false;
  }
  ValueArrayRead<float> points;
  bool have_points = ReadFloatArray(prim, "points", config_.time_code,
                                    &points);
  if (!have_points) {
    // Baked procedural curves commonly put clips metadata on their GEO
    // container while the sampled points live on descendant curves.
    for (UsdPrim owner = prim; owner.IsValid(); owner = owner.GetParent()) {
      const ::lightusd::next::PrimSpec* spec = owner.GetPrimSpec();
      if (!spec || !spec->meta().clips().is_dictionary()) continue;
      std::vector<::lightusd::next::ValueClipSet> sets;
      if (!::lightusd::next::ParseValueClipSets(owner, &sets, nullptr))
        break;
      for (auto& set : sets) set.prim_path = prim.GetPath().str();
      Value clipped;
      if (!config_.animation.clip_stage_loader ||
          !::lightusd::next::ResolveValueClipFromSets(
              sets, prim, "points", config_.time_code,
              config_.animation.clip_stage_loader, &clipped))
        break;
      points.scratch.materialized = std::move(clipped);
      have_points = ::lightusd::next::GetFloatArrayView(
          points.scratch.materialized, &points.scratch, &points.view);
      break;
    }
    // Some composed variant paths do not retain parent links in the compact
    // stage index. Walk the path index directly as a fallback.
    if (!have_points && prim.GetLayer()) {
      const ::lightusd::next::Layer* layer = prim.GetLayer();
      std::string parent_path = prim.GetPath().str();
      while (!have_points) {
        const size_t slash = parent_path.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        parent_path.resize(slash);
        const uint32_t index = layer->index_at_path(parent_path);
        if (index == UINT32_MAX) continue;
        const ::lightusd::next::PrimSpec* parent_spec =
            layer->prim_at_path(::lightusd::next::Path(parent_path));
        if (!parent_spec || !parent_spec->meta().clips().is_dictionary())
          continue;
        std::vector<::lightusd::next::ValueClipSet> sets;
        if (!::lightusd::next::ParseValueClipSets(
                ::lightusd::next::UsdPrim(parent_spec, layer, index), &sets,
                nullptr))
          break;
        for (auto& set : sets) set.prim_path = prim.GetPath().str();
        Value clipped;
        if (!config_.animation.clip_stage_loader ||
            !::lightusd::next::ResolveValueClipFromSets(
                sets, prim, "points", config_.time_code,
                config_.animation.clip_stage_loader, &clipped))
          break;
        points.scratch.materialized = std::move(clipped);
        have_points = ::lightusd::next::GetFloatArrayView(
            points.scratch.materialized, &points.scratch, &points.view);
      }
    }
  }
  if (!have_points || points.empty() || (points.view.size % 3) != 0) {
    SetLastError("Invalid curves.points data");
    return false;
  }

  size_t total_cp = 0;
  for (int32_t c : counts) {
    if (c <= 0) {
      SetLastError("Non-positive curveVertexCounts entry");
      return false;
    }
    total_cp += static_cast<size_t>(c);
  }
  if (total_cp != points.view.size / 3) {
    SetLastError("curveVertexCounts sum does not match points size");
    return false;
  }

  out->curve_vertex_counts.reserve(counts.size());
  for (int32_t c : counts) {
    out->curve_vertex_counts.push_back(static_cast<uint32_t>(c));
  }
  if (config_.curves.retain_control_points) {
    out->points.append(points.view.data, points.view.size);
  }

  // type / basis / wrap tokens (BasisCurves; NurbsCurves have order/knots).
  if (out->is_nurbs) {
    out->type = CurveType::Cubic;
  } else {
    std::string tok;
    if (GetToken(prim, "type", &tok) && tok == "linear") {
      out->type = CurveType::Linear;
    }
    tok.clear();
    if (GetToken(prim, "basis", &tok) && !tok.empty() && tok != "bezier") {
      if (tok == "bspline") {
        out->basis = CurveBasis::BSpline;
      } else if (tok == "catmullRom") {
        out->basis = CurveBasis::CatmullRom;
      } else {
        AddWarning("BasisCurves '" + out->prim_path +
                            "': unsupported basis '" + tok +
                            "', treating as bezier");
      }
    }
    tok.clear();
    if (GetToken(prim, "wrap", &tok)) {
      if (tok == "periodic") out->wrap = CurveWrap::Periodic;
      else if (tok == "pinned") out->wrap = CurveWrap::Pinned;
    }
  }

  // NURBS attributes.
  std::vector<int32_t> nurbs_order;
  std::vector<float> nurbs_knots;
  std::vector<float> nurbs_ranges;  // 2 floats per curve, optional
  std::vector<float> hermite_tangents;
  bool nurbs_data_ok = true;
  if (out->is_nurbs) {
    nurbs_order = ReadIntArrayCopy(prim, "order", config_.time_code);
    if (!ReadFloatsFlexible(prim, "knots", config_.time_code, &nurbs_knots)) {
      AddWarning("NurbsCurves '" + out->prim_path +
                          "': missing/unreadable knots; using control-polygon "
                          "passthrough");
      nurbs_data_ok = false;
    }
    ReadFloatsFlexible(prim, "ranges", config_.time_code, &nurbs_ranges);
  } else if (out->is_hermite) {
    if (!ReadFloatsFlexible(prim, "tangents", config_.time_code,
                            &hermite_tangents) ||
        hermite_tangents.size() != points.view.size) {
      AddWarning("HermiteCurves '" + out->prim_path +
                          "': tangents must match points; using control-polygon "
                          "passthrough");
      hermite_tangents.clear();
    }
  }

  const uint32_t segs = std::max(1u, config_.curves.tessellation_segments);
  const size_t ncurves = out->curve_vertex_counts.size();

  //
  // Build per-curve tessellation plans (validation + varying counts).
  //
  std::vector<CurveTessPlan> plans(ncurves);
  size_t knot_cursor = 0;
  for (size_t ci = 0; ci < ncurves; ++ci) {
    CurveTessPlan& plan = plans[ci];
    const uint32_t n = out->curve_vertex_counts[ci];
    plan.n = n;

    auto fall_back_linear = [&](const std::string& why) {
      plan.linear = true;
      plan.periodic = (!out->is_nurbs && out->wrap == CurveWrap::Periodic);
      plan.varying_count = n;
      AddWarning("Curves '" + out->prim_path + "' curve " +
                          std::to_string(ci) + ": " + why +
                          "; using control-polygon passthrough");
    };

    if (out->is_nurbs) {
      int order = 4;
      if (nurbs_order.size() == ncurves) order = nurbs_order[ci];
      else if (nurbs_order.size() == 1) order = nurbs_order[0];
      const size_t knot_count = static_cast<size_t>(n) + static_cast<size_t>(
          order > 0 ? order : 0);
      const size_t knot_offset = knot_cursor;
      if (order >= 2 && order <= kMaxNurbsDegree + 1) {
        knot_cursor += knot_count;  // advance even if this curve falls back
      }
      if (!nurbs_data_ok) {
        plan.linear = true;
        plan.varying_count = n;
        continue;
      }
      if (order < 2 || order > kMaxNurbsDegree + 1) {
        fall_back_linear("unsupported NURBS order " + std::to_string(order));
        continue;
      }
      if (n < static_cast<uint32_t>(order)) {
        fall_back_linear("fewer control points than NURBS order");
        continue;
      }
      if (knot_offset + knot_count > nurbs_knots.size()) {
        fall_back_linear("knot vector too short");
        continue;
      }
      const float* kn = nurbs_knots.data() + knot_offset;
      bool monotonic = true;
      for (size_t i = 1; i < knot_count; ++i) {
        if (kn[i] < kn[i - 1]) {
          monotonic = false;
          break;
        }
      }
      if (!monotonic) {
        fall_back_linear("decreasing knot vector");
        continue;
      }
      const int degree = order - 1;
      float u0 = kn[degree];
      float u1 = kn[n];
      if (nurbs_ranges.size() >= (ci + 1) * 2) {
        const float r0 = nurbs_ranges[ci * 2 + 0];
        const float r1 = nurbs_ranges[ci * 2 + 1];
        if (r0 < r1) {
          u0 = std::max(u0, r0);
          u1 = std::min(u1, r1);
        }
      }
      if (!(u1 > u0)) {
        fall_back_linear("degenerate NURBS parameter range");
        continue;
      }
      plan.degree = degree;
      plan.knot_offset = knot_offset;
      plan.u0 = u0;
      plan.u1 = u1;
      plan.nsegs = n - static_cast<uint32_t>(order) + 1;
      plan.varying_count = plan.nsegs + 1;
      continue;
    }

    if (out->is_hermite) {
      if (n < 2 || hermite_tangents.empty()) {
        fall_back_linear(n < 2 ? "too few Hermite control points"
                               : "missing or mismatched Hermite tangents");
        continue;
      }
      plan.nsegs = n - 1;
      plan.varying_count = n;
      continue;
    }

    // BasisCurves.
    if (out->type == CurveType::Linear) {
      plan.linear = true;
      plan.periodic = (out->wrap == CurveWrap::Periodic);
      if (plan.periodic && n < 3) plan.periodic = false;
      plan.varying_count = n;
      continue;
    }

    const bool bezier = (out->basis == CurveBasis::Bezier);
    // "pinned" only applies to cubic bspline/catmullRom.
    const bool pinned = (out->wrap == CurveWrap::Pinned) && !bezier;
    const bool periodic = (out->wrap == CurveWrap::Periodic);
    if (periodic) {
      if (n < 3 || (bezier && (n % 3) != 0)) {
        fall_back_linear("invalid periodic cubic control point count");
        continue;
      }
      plan.periodic = true;
      plan.nsegs = bezier ? (n / 3) : n;
      plan.varying_count = plan.nsegs;
      continue;
    }
    if (pinned) {
      if (n < 2) {
        fall_back_linear("too few control points for pinned cubic curve");
        continue;
      }
      plan.pinned = true;
      // bspline: endpoints tripled (dup x2); catmullRom: doubled (dup x1).
      const uint32_t dup = (out->basis == CurveBasis::BSpline) ? 2u : 1u;
      plan.nsegs = (n + 2 * dup) - 3;
      plan.varying_count = plan.nsegs + 1;
      continue;
    }
    // nonperiodic
    if (n < 4 || (bezier && ((n - 4) % 3) != 0)) {
      fall_back_linear("invalid cubic control point count");
      continue;
    }
    plan.nsegs = bezier ? ((n - 4) / 3 + 1) : (n - 3);
    plan.varying_count = plan.nsegs + 1;
  }

  size_t varying_total = 0;
  for (const CurveTessPlan& plan : plans) varying_total += plan.varying_count;

  //
  // widths (classified by element count; default schema interp is vertex).
  //
  ValueArrayRead<float> widths;
  if (ReadFloatArray(prim, "widths", config_.time_code, &widths) &&
      !widths.empty()) {
    const size_t m = widths.view.size;
    if (m == 1) {
      out->widths.append(widths.view.data, m);
      out->widths_interp = Interpolation::Constant;
    } else if (m == ncurves) {
      out->widths.append(widths.view.data, m);
      out->widths_interp = Interpolation::Uniform;
    } else if (m == total_cp) {
      out->widths.append(widths.view.data, m);
      out->widths_interp = Interpolation::Vertex;
    } else if (m == varying_total) {
      out->widths.append(widths.view.data, m);
      out->widths_interp = Interpolation::Varying;
    } else {
      AddWarning("Curves '" + out->prim_path +
                          "': ignoring widths with mismatched element count");
    }
  }

  //
  // displayColor (control data only; rgb).
  //
  ValueArrayRead<float> colors;
  if (ReadFloatArray(prim, "primvars:displayColor", config_.time_code,
                     &colors) &&
      !colors.empty() && (colors.view.size % 3) == 0) {
    std::string interp_tok = "constant";
    if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
      if (const ::lightusd::next::PropMeta* pm =
              spec->property_meta("primvars:displayColor")) {
        if (pm->authored & ::lightusd::next::PropMeta::kInterpolation) {
          interp_tok = pm->interpolation;
        }
      }
    }
    Interpolation interp = ParsePrimvarInterp(interp_tok);
    const size_t elems = colors.view.size / 3;
    auto expected = [&](Interpolation it) -> size_t {
      switch (it) {
        case Interpolation::Constant: return 1;
        case Interpolation::Uniform: return ncurves;
        case Interpolation::Varying: return varying_total;
        case Interpolation::Vertex:
        default: return total_cp;
      }
    };
    if (elems != expected(interp)) {
      // Authored interp does not match; classify by size instead.
      if (elems == 1) interp = Interpolation::Constant;
      else if (elems == total_cp) interp = Interpolation::Vertex;
      else if (elems == ncurves) interp = Interpolation::Uniform;
      else if (elems == varying_total) interp = Interpolation::Varying;
      else {
        AddWarning(
            "Curves '" + out->prim_path +
            "': ignoring displayColor with mismatched element count");
        interp = Interpolation::Constant;  // expected(Constant)==1 != elems
      }
    }
    if (elems == expected(interp)) {
      out->colors.append(colors.view.data, colors.view.size);
      out->colors_interp = interp;
    }
  }

  // displayOpacity follows the same constant/uniform/vertex/varying rules as
  // displayColor and is resampled onto the tessellated centerline below.
  ValueArrayRead<float> opacities;
  if (ReadFloatArray(prim, "primvars:displayOpacity", config_.time_code,
                     &opacities) && !opacities.empty()) {
    std::string interp_tok = "constant";
    if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
      if (const ::lightusd::next::PropMeta* pm =
              spec->property_meta("primvars:displayOpacity")) {
        if (pm->authored & ::lightusd::next::PropMeta::kInterpolation) {
          interp_tok = pm->interpolation;
        }
      }
    }
    Interpolation interp = ParsePrimvarInterp(interp_tok);
    const size_t elems = opacities.view.size;
    auto expected = [&](Interpolation it) -> size_t {
      switch (it) {
        case Interpolation::Constant: return 1;
        case Interpolation::Uniform: return ncurves;
        case Interpolation::Varying: return varying_total;
        case Interpolation::Vertex:
        default: return total_cp;
      }
    };
    if (elems != expected(interp)) {
      if (elems == 1) interp = Interpolation::Constant;
      else if (elems == total_cp) interp = Interpolation::Vertex;
      else if (elems == ncurves) interp = Interpolation::Uniform;
      else if (elems == varying_total) interp = Interpolation::Varying;
    }
    if (elems == expected(interp)) {
      out->opacities.append(opacities.view.data, elems);
      out->opacities_interp = interp;
    } else {
      AddWarning(
          "Curves '" + out->prim_path +
          "': ignoring displayOpacity with mismatched element count");
    }
  }

  //
  // Tessellate.
  //
  const bool emit_widths = out->has_widths() &&
                           out->widths_interp != Interpolation::Constant;
  const bool emit_colors = out->has_colors();
  const bool emit_opacities = !out->opacities.empty() &&
      out->opacities_interp != Interpolation::Constant;
  size_t cp_offset = 0;
  size_t var_offset = 0;
  std::vector<float> emitted;
  std::vector<float> pinned_cvs;
  for (size_t ci = 0; ci < ncurves; ++ci) {
    const CurveTessPlan& plan = plans[ci];
    const uint32_t n = plan.n;
    const float* cv = points.view.data + cp_offset * 3;
    emitted.clear();

    if (plan.linear) {
      emitted.assign(cv, cv + static_cast<size_t>(n) * 3);
      if (plan.periodic) {
        emitted.push_back(cv[0]);
        emitted.push_back(cv[1]);
        emitted.push_back(cv[2]);
      }
    } else if (out->is_hermite) {
      const float* tangent = hermite_tangents.data() + cp_offset * 3;
      emitted.reserve((static_cast<size_t>(plan.nsegs) * segs + 1) * 3);
      auto eval_hermite = [&](uint32_t span, float t, float p[3]) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        for (size_t component = 0; component < 3; ++component) {
          p[component] = h00 * cv[span * 3 + component] +
                         h10 * tangent[span * 3 + component] +
                         h01 * cv[(span + 1) * 3 + component] +
                         h11 * tangent[(span + 1) * 3 + component];
        }
      };
      for (uint32_t span = 0; span < plan.nsegs; ++span) {
        for (uint32_t sample = 0; sample < segs; ++sample) {
          float p[3];
          eval_hermite(span,
                       static_cast<float>(sample) / static_cast<float>(segs),
                       p);
          emitted.insert(emitted.end(), p, p + 3);
        }
      }
      float p[3];
      eval_hermite(plan.nsegs - 1, 1.0f, p);
      emitted.insert(emitted.end(), p, p + 3);
    } else if (out->is_nurbs) {
      const float* kn = nurbs_knots.data() + plan.knot_offset;
      const uint32_t nsamples = plan.nsegs * segs + 1;
      emitted.reserve(static_cast<size_t>(nsamples) * 3);
      for (uint32_t k = 0; k < nsamples; ++k) {
        const float u =
            plan.u0 + (plan.u1 - plan.u0) *
                          (static_cast<float>(k) /
                           static_cast<float>(nsamples - 1));
        float p[3] = {0.0f, 0.0f, 0.0f};
        DeBoorEval(cv, n, kn, plan.degree, u, p);
        emitted.push_back(p[0]);
        emitted.push_back(p[1]);
        emitted.push_back(p[2]);
      }
    } else {
      // Cubic BasisCurves.
      const float* ecv = cv;
      uint32_t en = n;
      if (plan.pinned) {
        const uint32_t dup = (out->basis == CurveBasis::BSpline) ? 2u : 1u;
        pinned_cvs.clear();
        pinned_cvs.reserve((static_cast<size_t>(n) + 2 * dup) * 3);
        for (uint32_t d = 0; d < dup; ++d) {
          pinned_cvs.insert(pinned_cvs.end(), cv, cv + 3);
        }
        pinned_cvs.insert(pinned_cvs.end(), cv, cv + static_cast<size_t>(n) * 3);
        const float* last = cv + (static_cast<size_t>(n) - 1) * 3;
        for (uint32_t d = 0; d < dup; ++d) {
          pinned_cvs.insert(pinned_cvs.end(), last, last + 3);
        }
        ecv = pinned_cvs.data();
        en = n + 2 * dup;
      }
      const uint32_t vstep = (out->basis == CurveBasis::Bezier) ? 3u : 1u;
      emitted.reserve((static_cast<size_t>(plan.nsegs) * segs + 1) * 3);
      float w[4];
      auto eval_span = [&](uint32_t span, float t, float p[3]) {
        EvalCubicBasisWeights(out->basis, t, w);
        p[0] = p[1] = p[2] = 0.0f;
        const uint32_t base = span * vstep;
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t idx = plan.periodic ? ((base + k) % en) : (base + k);
          p[0] += w[k] * ecv[idx * 3 + 0];
          p[1] += w[k] * ecv[idx * 3 + 1];
          p[2] += w[k] * ecv[idx * 3 + 2];
        }
      };
      for (uint32_t s = 0; s < plan.nsegs; ++s) {
        for (uint32_t j = 0; j < segs; ++j) {
          float p[3];
          eval_span(s, static_cast<float>(j) / static_cast<float>(segs), p);
          emitted.push_back(p[0]);
          emitted.push_back(p[1]);
          emitted.push_back(p[2]);
        }
      }
      if (plan.periodic) {
        // Close the loop with a copy of the first tessellated point.
        emitted.push_back(emitted[0]);
        emitted.push_back(emitted[1]);
        emitted.push_back(emitted[2]);
      } else {
        float p[3];
        eval_span(plan.nsegs - 1, 1.0f, p);
        emitted.push_back(p[0]);
        emitted.push_back(p[1]);
        emitted.push_back(p[2]);
      }
    }

    const size_t emit_count = emitted.size() / 3;
    out->tessellated_vertex_counts.push_back(
        static_cast<uint32_t>(emit_count));
    out->tessellated_points.append(emitted.data(), emitted.size());

    if (emit_widths) {
      const float* wvals = nullptr;
      size_t wcount = 0;
      switch (out->widths_interp) {
        case Interpolation::Uniform:
          wvals = widths.view.data + ci;
          wcount = 1;
          break;
        case Interpolation::Vertex:
          wvals = widths.view.data + cp_offset;
          wcount = n;
          break;
        case Interpolation::Varying:
          wvals = widths.view.data + var_offset;
          wcount = plan.varying_count;
          break;
        default: break;
      }
      for (size_t k = 0; k < emit_count; ++k) {
        const float u01 =
            emit_count > 1
                ? static_cast<float>(k) / static_cast<float>(emit_count - 1)
                : 0.0f;
        out->tessellated_widths.push_back(
            SampleChannelLinear(wvals, wcount, u01, plan.periodic));
      }
    }

    if (emit_colors) {
      const float* cvals = nullptr;
      size_t ccount = 0;
      switch (out->colors_interp) {
        case Interpolation::Constant:
          cvals = colors.view.data;
          ccount = 1;
          break;
        case Interpolation::Uniform:
          cvals = colors.view.data + ci * 3;
          ccount = 1;
          break;
        case Interpolation::Vertex:
          cvals = colors.view.data + cp_offset * 3;
          ccount = n;
          break;
        case Interpolation::Varying:
          cvals = colors.view.data + var_offset * 3;
          ccount = plan.varying_count;
          break;
        case Interpolation::FaceVarying:
          break;
      }
      if (cvals && ccount > 0) {
        for (size_t k = 0; k < emit_count; ++k) {
          const float u01 =
              emit_count > 1
                  ? static_cast<float>(k) / static_cast<float>(emit_count - 1)
                  : 0.0f;
          for (size_t component = 0; component < 3; ++component) {
            out->tessellated_colors.push_back(SampleChannelLinear(
                cvals, ccount, u01, plan.periodic, 3, component));
          }
        }
      }
    }

    if (emit_opacities) {
      const float* ovals = nullptr;
      size_t ocount = 0;
      switch (out->opacities_interp) {
        case Interpolation::Uniform:
          ovals = opacities.view.data + ci;
          ocount = 1;
          break;
        case Interpolation::Vertex:
          ovals = opacities.view.data + cp_offset;
          ocount = n;
          break;
        case Interpolation::Varying:
          ovals = opacities.view.data + var_offset;
          ocount = plan.varying_count;
          break;
        default: break;
      }
      if (ovals && ocount > 0) {
        for (size_t k = 0; k < emit_count; ++k) {
          const float u01 = emit_count > 1
                                ? static_cast<float>(k) /
                                      static_cast<float>(emit_count - 1)
                                : 0.0f;
          out->tessellated_opacities.push_back(
              SampleChannelLinear(ovals, ocount, u01, plan.periodic));
        }
      }
    }

    cp_offset += n;
    var_offset += plan.varying_count;
  }

  ComputePointBounds(out->tessellated_points, &out->bbox_min, &out->bbox_max,
                     &out->has_bbox);
  return true;
}


}  // namespace next
}  // namespace tydra
}  // namespace lightusd
