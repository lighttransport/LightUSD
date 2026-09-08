// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Points conversion

#include "render-converter.hh"

#include "next/schema/usd-vol.hh"

#include <algorithm>
#include <cmath>
#include <string>

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

bool RenderSceneConverter::ConvertPoints(const Stage& stage,
                                         const UsdPrim& prim,
                                         RenderPoints* out) {
  const bool gaussian = prim.GetTypeName() == "ParticleField3DGaussianSplat";
  if (!out || !prim.IsValid() ||
      (prim.GetTypeName() != "Points" && !gaussian)) {
    SetLastError("Invalid Points prim");
    return false;
  }

  ::lightusd::next::ParticleFieldData particle_field;
  if (gaussian) {
    std::string particle_warning;
    if (!::lightusd::next::GetParticleFieldData(
            stage, prim, &particle_field, config_.time_code,
            &particle_warning)) {
      SetLastError("Invalid ParticleField data");
      return false;
    }
    if (!particle_warning.empty()) AddWarning(particle_warning);
  }
  ValueArrayRead<float> points;
  const char* point_property = gaussian
      ? particle_field.positions_property.c_str()
      : "points";
  if (!ReadFloatArray(prim, point_property, config_.time_code, &points) ||
      points.empty() || (points.view.size % 3) != 0) {
    SetLastError("Invalid Points.points data");
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  out->points.append(points.view.data, points.view.size);

  if (!gaussian) {
    ValueArrayRead<float> normals;
    if (ReadFloatArray(prim, "normals", config_.time_code, &normals) &&
        normals.view.size == out->point_count() * 3) {
      out->normals.append(normals.view.data, normals.view.size);
    } else if (normals.view.size != 0) {
      AddWarning("Points '" + out->prim_path +
                          "': ignoring normals with mismatched element count");
    }
  }

  ValueArrayRead<float> widths;
  const bool have_widths = gaussian
      ? (!particle_field.scales_property.empty() &&
         ReadFloatArray(prim, particle_field.scales_property.c_str(),
                        config_.time_code, &widths))
      : ReadFloatArray(prim, "widths", config_.time_code, &widths);
  if (have_widths &&
      !widths.empty()) {
    const size_t n = out->point_count();
    if (gaussian && widths.view.size == n * 3) {
      // Gaussian scales are one standard deviation per local axis. The
      // raster carrier is isotropic, so retain a conservative diameter using
      // the largest authored axis; RT backends consume the full ellipse data.
      out->widths.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        const float* s = widths.view.data + i * 3;
        out->widths.push_back(2.0f * std::max(std::fabs(s[0]),
                                               std::max(std::fabs(s[1]),
                                                        std::fabs(s[2]))));
      }
    } else if (!gaussian && (widths.view.size == 1 || widths.view.size == n)) {
      out->widths.append(widths.view.data, widths.view.size);
    } else {
      AddWarning("Points '" + out->prim_path +
                          "': ignoring widths with mismatched element count");
    }
  }

  ValueArrayRead<float> colors;
  if (ReadFloatArray(prim, "primvars:displayColor", config_.time_code,
                     &colors) &&
      !colors.empty()) {
    std::string interp_tok;
    if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
      if (const ::lightusd::next::PropMeta* pm =
              spec->property_meta("primvars:displayColor")) {
        if (pm->authored & ::lightusd::next::PropMeta::kInterpolation) {
          interp_tok = pm->interpolation;
        }
      }
    }
    const size_t elems = colors.view.size / 3;
    Interpolation interp;
    if (interp_tok.empty()) {
      // Unauthored: spec default is constant; per-point arrays are common in
      // the wild, so classify by element count.
      interp = (elems == out->point_count() && elems != 1)
                   ? Interpolation::Vertex
                   : Interpolation::Constant;
    } else {
      interp = ParsePrimvarInterp(interp_tok);
    }
    const size_t expected = (interp == Interpolation::Constant)
                                ? 1
                                : out->point_count();
    if ((colors.view.size % 3) == 0 && elems == expected) {
      out->colors.append(colors.view.data, colors.view.size);
      out->colors_interp = interp;
    } else {
      AddWarning("Points '" + out->prim_path +
                          "': ignoring displayColor with mismatched element count");
    }
  }

  ValueArrayRead<float> opacities;
  if (ReadFloatArray(prim, "primvars:displayOpacity", config_.time_code,
                     &opacities) &&
      !opacities.empty()) {
    std::string interp_tok;
    if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
      if (const ::lightusd::next::PropMeta* pm =
              spec->property_meta("primvars:displayOpacity")) {
        if (pm->authored & ::lightusd::next::PropMeta::kInterpolation) {
          interp_tok = pm->interpolation;
        }
      }
    }
    const size_t elems = opacities.view.size;
    Interpolation interp;
    if (interp_tok.empty()) {
      interp = (elems == out->point_count() && elems != 1)
                   ? Interpolation::Vertex
                   : Interpolation::Constant;
    } else {
      interp = ParsePrimvarInterp(interp_tok);
    }
    const size_t expected = (interp == Interpolation::Constant)
                                ? 1
                                : out->point_count();
    if (elems == expected) {
      out->opacities.append(opacities.view.data, elems);
      out->opacities_interp = interp;
    } else {
      AddWarning(
          "Points '" + out->prim_path +
          "': ignoring displayOpacity with mismatched element count");
    }
  }

  ComputePointBounds(out->points, &out->bbox_min, &out->bbox_max,
                     &out->has_bbox);
  return true;
}


}  // namespace next
}  // namespace tydra
}  // namespace lightusd
