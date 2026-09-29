// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "usd-vol.hh"
#include "../layer/prim-spec.hh"

#include "../eval/attribute-eval.hh"

namespace lightusd {
namespace next {
namespace {

bool HasApplied(const UsdPrim& prim, const char* schema) {
  for (const std::string& entry : prim.GetMeta().apiSchemas()) {
    if (entry == schema) return true;
  }
  return false;
}

void Append(std::string* warning, const std::string& message) {
  if (!warning) return;
  if (!warning->empty() && warning->back() != '\n') *warning += '\n';
  *warning += message;
}

bool SelectArray(const AttributeEval& eval, const UsdPrim& prim,
                 const char* full_name, const char* half_name,
                 TypeId full_type, TypeId half_type,
                 size_t required_count, bool position_array,
                 std::string* selected, bool* half, size_t* source_count,
                 std::string* warning) {
  selected->clear();
  *half = false;
  *source_count = 0;
  EvalResult value = eval.Eval(prim, full_name);
  if (!value.success || !value.value.is_array() ||
      value.value.type_id() != full_type) {
    value = eval.Eval(prim, half_name);
    if (!value.success || !value.value.is_array() ||
        value.value.type_id() != half_type) {
      return false;
    }
    *half = true;
    *selected = half_name;
  } else {
    *selected = full_name;
  }
  *source_count = value.value.array_size();
  if (!position_array && *source_count < required_count) {
    Append(warning, "ParticleField '" + prim.GetPath().str() + "': " +
                        *selected + " has fewer elements than positions and "
                        "will be ignored");
    selected->clear();
    *source_count = 0;
    return false;
  }
  if (!position_array && *source_count > required_count) {
    Append(warning, "ParticleField '" + prim.GetPath().str() + "': " +
                        *selected + " has extra elements and will be truncated");
  }
  return true;
}

}  // namespace

bool IsParticleField(const UsdPrim& prim) {
  return prim.IsValid() &&
         (prim.GetTypeName() == "ParticleField" ||
          prim.GetTypeName() == "ParticleField3DGaussianSplat");
}

bool IsParticleField3DGaussianSplat(const UsdPrim& prim) {
  return prim.IsValid() &&
         prim.GetTypeName() == "ParticleField3DGaussianSplat";
}

bool GetParticleFieldData(const Stage& stage, const UsdPrim& prim,
                          ParticleFieldData* out, double time,
                          std::string* warning) {
  if (!out || !IsParticleField(prim)) return false;
  *out = ParticleFieldData{};
  AttributeEval eval(&stage);
  eval.SetTime(time);

  size_t position_count = 0;
  if (!SelectArray(eval, prim, "positions", "positionsh", TypeId::Point3f,
                   TypeId::Point3h, 0, true,
                   &out->positions_property, &out->positions_half,
                   &position_count, warning)) {
    return true;  // A ParticleField without positions contains zero particles.
  }
  out->particle_count = position_count;

  size_t ignored_count = 0;
  (void)SelectArray(eval, prim, "orientations", "orientationsh",
                    TypeId::Quatf, TypeId::Quath,
                    position_count, false, &out->orientations_property,
                    &out->orientations_half, &ignored_count, warning);
  (void)SelectArray(eval, prim, "scales", "scalesh", TypeId::Float3,
                    TypeId::Half3, position_count, false,
                    &out->scales_property, &out->scales_half, &ignored_count,
                    warning);
  (void)SelectArray(eval, prim, "opacities", "opacitiesh", TypeId::Float,
                    TypeId::Half, position_count, false,
                    &out->opacities_property, &out->opacities_half,
                    &ignored_count, warning);

  // SH has multiple coefficients per particle, so only select precision here;
  // consumers validate degree-derived coefficient counts.
  std::string sh_property;
  bool sh_half = false;
  size_t sh_count = 0;
  (void)SelectArray(eval, prim, "radiance:sphericalHarmonicsCoefficients",
                    "radiance:sphericalHarmonicsCoefficientsh", TypeId::Float3,
                    TypeId::Half3, 0, true,
                    &sh_property, &sh_half, &sh_count, warning);
  out->spherical_harmonics_property = std::move(sh_property);
  out->spherical_harmonics_half = sh_half;
  out->spherical_harmonics_degree =
      eval.EvalInt(prim, "radiance:sphericalHarmonicsDegree").value_or(3);

  if (IsParticleField3DGaussianSplat(prim) ||
      HasApplied(prim, "ParticleFieldKernelGaussianEllipsoidAPI")) {
    out->kernel = ParticleKernel::GaussianEllipsoid;
  } else if (HasApplied(prim, "ParticleFieldKernelGaussianSurfletAPI")) {
    out->kernel = ParticleKernel::GaussianSurflet;
  } else if (HasApplied(prim, "ParticleFieldKernelConstantSurfletAPI")) {
    out->kernel = ParticleKernel::ConstantSurflet;
  }
  out->projection_mode_hint =
      eval.EvalToken(prim, "projectionModeHint").value_or("perspective");
  out->sorting_mode_hint =
      eval.EvalToken(prim, "sortingModeHint").value_or("zDepth");
  return true;
}

}  // namespace next
}  // namespace lightusd
