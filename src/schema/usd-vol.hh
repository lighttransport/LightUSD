// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "stage/stage.hh"

#include <string>

namespace lightusd {
namespace next {

enum class ParticleKernel {
  None,
  GaussianEllipsoid,
  GaussianSurflet,
  ConstantSurflet,
};

// Resolves OpenUSD's float-over-half precedence without materializing large
// arrays. Property names can be passed to the caller's lazy/chunked reader.
struct ParticleFieldData {
  size_t particle_count = 0;
  std::string positions_property;
  std::string orientations_property;
  std::string scales_property;
  std::string opacities_property;
  std::string spherical_harmonics_property;
  bool positions_half = false;
  bool orientations_half = false;
  bool scales_half = false;
  bool opacities_half = false;
  bool spherical_harmonics_half = false;
  int32_t spherical_harmonics_degree = 3;
  ParticleKernel kernel = ParticleKernel::None;
  std::string projection_mode_hint = "perspective";
  std::string sorting_mode_hint = "zDepth";
};

bool IsParticleField(const UsdPrim& prim);
bool IsParticleField3DGaussianSplat(const UsdPrim& prim);
bool GetParticleFieldData(const Stage& stage, const UsdPrim& prim,
                          ParticleFieldData* out, double time = 0.0,
                          std::string* warning = nullptr);

}  // namespace next
}  // namespace lightusd
