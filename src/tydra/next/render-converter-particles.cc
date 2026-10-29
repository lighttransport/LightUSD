// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - UsdVol ParticleField conversion (see render-data-particles.hh).

#include "render-converter-internal.hh"

#include "next/schema/usd-vol.hh"

#include <cstring>
#include <string>

namespace lightusd {
namespace tydra {
namespace next {

namespace {

// `width` floats per particle: truncate, or pad with `fill` per particle.
void FitPerParticle(std::vector<float>* v, size_t count, size_t width, const float* fill,
                    const char* what, RenderParticleField* out) {
  const size_t want = count * width;
  if (v->size() == want) return;
  if (v->size() > want) {
    v->resize(want);
    return;
  }
  if (!v->empty())
    out->warnings.push_back(out->prim_path + ": " + what + " has " + std::to_string(v->size() / width) +
                            " entries for " + std::to_string(count) + " particles; using defaults");
  v->clear();
  v->reserve(want);
  for (size_t i = 0; i < count; ++i) v->insert(v->end(), fill, fill + width);
}

}  // namespace

bool ConvertParticleField(const ::lightusd::next::Stage& stage, const UsdPrim& prim, double time,
                          RenderParticleField* out) {
  if (!out || !::lightusd::next::IsParticleField(prim)) return false;
  ::lightusd::next::ParticleFieldData data;
  std::string warning;
  if (!::lightusd::next::GetParticleFieldData(stage, prim, &data, time, &warning)) return false;
  *out = RenderParticleField{};
  out->prim_path = prim.GetPath().str();
  out->type_name = prim.GetTypeName();
  // ParticleField3DGaussianSplat also converts to RenderPoints, which reports
  // the schema warnings already.
  if (!warning.empty() && out->type_name != "ParticleField3DGaussianSplat")
    out->warnings.push_back(warning);
  double m[16];
  if (ComputeWorldTransform(stage, prim, m, time)) std::memcpy(out->world_matrix, m, sizeof(m));
  const size_t n = data.particle_count;
  out->count = n;
  if (!n) return true;
  out->positions = ReadFloatArrayCopy(prim, data.positions_property.c_str(), time);
  if (out->positions.size() < n * 3) {
    out->warnings.push_back(out->prim_path + ": positions unreadable");
    out->count = 0;
    out->positions.clear();
    return true;
  }
  out->positions.resize(n * 3);
  static const float kIdentity[4] = {1, 0, 0, 0}, kOne[3] = {1, 1, 1}, kOpaque[1] = {1},
                     kGray[3] = {0.5f, 0.5f, 0.5f};
  if (!data.orientations_property.empty())
    out->orientations = ReadFloatArrayCopy(prim, data.orientations_property.c_str(), time);
  FitPerParticle(&out->orientations, n, 4, kIdentity, "orientations", out);
  if (!data.scales_property.empty())
    out->scales = ReadFloatArrayCopy(prim, data.scales_property.c_str(), time);
  FitPerParticle(&out->scales, n, 3, kOne, "scales", out);
  if (!data.opacities_property.empty())
    out->opacities = ReadFloatArrayCopy(prim, data.opacities_property.c_str(), time);
  FitPerParticle(&out->opacities, n, 1, kOpaque, "opacities", out);
  // Spherical harmonics: (degree + 1)^2 rgb coefficients per particle; a
  // short array falls back to the degree-0 DC default.
  const int degree = data.spherical_harmonics_degree < 0 ? 0 : data.spherical_harmonics_degree;
  const size_t per = size_t(degree + 1) * size_t(degree + 1) * 3;
  if (!data.spherical_harmonics_property.empty())
    out->sh_coefficients = ReadFloatArrayCopy(prim, data.spherical_harmonics_property.c_str(), time);
  if (out->sh_coefficients.size() >= n * per) {
    out->sh_degree = degree;
    out->sh_coefficients.resize(n * per);
  } else {
    if (!out->sh_coefficients.empty())
      out->warnings.push_back(out->prim_path + ": spherical harmonics shorter than degree " +
                              std::to_string(degree) + " needs; using the DC default");
    out->sh_degree = 0;
    out->sh_coefficients.clear();
    FitPerParticle(&out->sh_coefficients, n, 3, kGray, "spherical harmonics", out);
  }
  switch (data.kernel) {
    case ::lightusd::next::ParticleKernel::GaussianEllipsoid: out->kernel = "gaussianEllipsoid"; break;
    case ::lightusd::next::ParticleKernel::GaussianSurflet: out->kernel = "gaussianSurflet"; break;
    case ::lightusd::next::ParticleKernel::ConstantSurflet: out->kernel = "constantSurflet"; break;
    default: break;
  }
  out->projection_mode_hint = data.projection_mode_hint;
  out->sorting_mode_hint = data.sorting_mode_hint;
  return true;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
