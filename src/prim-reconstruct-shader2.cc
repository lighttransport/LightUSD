// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Shader/Material/NodeGraph reconstruction specializations.
// Split from prim-reconstruct.cc
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdShade.hh"
#include "usdMtlx.hh"

#include "common-macros.inc"
#include "value-types.hh"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

// __VA_ARGS__ does not allow empty, thus # of args must be 2+
#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

namespace lightusd {
namespace prim {

[[maybe_unused]] constexpr auto kInputsVarname = "inputs:varname";
[[maybe_unused]] constexpr auto kPurpose = "purpose";

// MaterialX Validation Helpers
// ==========================================================================



template <typename T>
bool ReconstructShader(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options);

#include "prim-reconstruct-common.inc"

template <>
bool ReconstructShader<MtlxAutodeskStandardSurface>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    MtlxAutodeskStandardSurface *surface,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec;
  (void)references;
  (void)options;
  (void)warn;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  for (auto &prop : properties) {
    // Base properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base", MtlxAutodeskStandardSurface,
                         surface->base)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_color", MtlxAutodeskStandardSurface,
                         surface->base_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:diffuse_roughness", MtlxAutodeskStandardSurface,
                         surface->diffuse_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:metalness", MtlxAutodeskStandardSurface,
                         surface->metalness)

    // Specular properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular", MtlxAutodeskStandardSurface,
                         surface->specular)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_color", MtlxAutodeskStandardSurface,
                         surface->specular_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_roughness", MtlxAutodeskStandardSurface,
                         surface->specular_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_IOR", MtlxAutodeskStandardSurface,
                         surface->specular_IOR)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_anisotropy", MtlxAutodeskStandardSurface,
                         surface->specular_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_rotation", MtlxAutodeskStandardSurface,
                         surface->specular_rotation)

    // Transmission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission", MtlxAutodeskStandardSurface,
                         surface->transmission)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_color", MtlxAutodeskStandardSurface,
                         surface->transmission_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_depth", MtlxAutodeskStandardSurface,
                         surface->transmission_depth)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter", MtlxAutodeskStandardSurface,
                         surface->transmission_scatter)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter_anisotropy", MtlxAutodeskStandardSurface,
                         surface->transmission_scatter_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion", MtlxAutodeskStandardSurface,
                         surface->transmission_dispersion)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_extra_roughness", MtlxAutodeskStandardSurface,
                         surface->transmission_extra_roughness)

    // Subsurface properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface", MtlxAutodeskStandardSurface,
                         surface->subsurface)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_color", MtlxAutodeskStandardSurface,
                         surface->subsurface_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_radius", MtlxAutodeskStandardSurface,
                         surface->subsurface_radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_scale", MtlxAutodeskStandardSurface,
                         surface->subsurface_scale)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_anisotropy", MtlxAutodeskStandardSurface,
                         surface->subsurface_anisotropy)

    // Sheen properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen", MtlxAutodeskStandardSurface,
                         surface->sheen)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen_color", MtlxAutodeskStandardSurface,
                         surface->sheen_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen_roughness", MtlxAutodeskStandardSurface,
                         surface->sheen_roughness)

    // Coat properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat", MtlxAutodeskStandardSurface,
                         surface->coat)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_color", MtlxAutodeskStandardSurface,
                         surface->coat_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_roughness", MtlxAutodeskStandardSurface,
                         surface->coat_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_anisotropy", MtlxAutodeskStandardSurface,
                         surface->coat_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_rotation", MtlxAutodeskStandardSurface,
                         surface->coat_rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_IOR", MtlxAutodeskStandardSurface,
                         surface->coat_IOR)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_color", MtlxAutodeskStandardSurface,
                         surface->coat_affect_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_roughness", MtlxAutodeskStandardSurface,
                         surface->coat_affect_roughness)

    // Thin film properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_thickness", MtlxAutodeskStandardSurface,
                         surface->thin_film_thickness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_IOR", MtlxAutodeskStandardSurface,
                         surface->thin_film_IOR)

    // Emission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission", MtlxAutodeskStandardSurface,
                         surface->emission)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission_color", MtlxAutodeskStandardSurface,
                         surface->emission_color)

    // Other properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:opacity", MtlxAutodeskStandardSurface,
                         surface->opacity)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_walled", MtlxAutodeskStandardSurface,
                         surface->thin_walled)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:normal", MtlxAutodeskStandardSurface,
                         surface->normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:tangent", MtlxAutodeskStandardSurface,
                         surface->tangent)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_normal", MtlxAutodeskStandardSurface,
                         surface->coat_normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:displacement", MtlxAutodeskStandardSurface,
                         surface->displacement)

    // Output
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:out", MtlxAutodeskStandardSurface,
                   surface->out)

    ADD_PROPERTY(table, prop, MtlxAutodeskStandardSurface, surface->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<MtlxOpenPBRSurface>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    MtlxOpenPBRSurface *surface,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec;
  (void)references;
  (void)options;
  (void)warn;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  for (auto &prop : properties) {
    // Base properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_weight", MtlxOpenPBRSurface,
                         surface->base_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_color", MtlxOpenPBRSurface,
                         surface->base_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_metalness", MtlxOpenPBRSurface,
                         surface->base_metalness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_diffuse_roughness", MtlxOpenPBRSurface,
                         surface->base_diffuse_roughness)

    // Specular properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_weight", MtlxOpenPBRSurface,
                         surface->specular_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_color", MtlxOpenPBRSurface,
                         surface->specular_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_roughness", MtlxOpenPBRSurface,
                         surface->specular_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_ior", MtlxOpenPBRSurface,
                         surface->specular_ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_anisotropy", MtlxOpenPBRSurface,
                         surface->specular_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_rotation", MtlxOpenPBRSurface,
                         surface->specular_rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_roughness_anisotropy", MtlxOpenPBRSurface,
                         surface->specular_roughness_anisotropy)

    // Transmission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_weight", MtlxOpenPBRSurface,
                         surface->transmission_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_color", MtlxOpenPBRSurface,
                         surface->transmission_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_depth", MtlxOpenPBRSurface,
                         surface->transmission_depth)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter", MtlxOpenPBRSurface,
                         surface->transmission_scatter)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter_anisotropy", MtlxOpenPBRSurface,
                         surface->transmission_scatter_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion", MtlxOpenPBRSurface,
                         surface->transmission_dispersion)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion_abbe_number", MtlxOpenPBRSurface,
                         surface->transmission_dispersion_abbe_number)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion_scale", MtlxOpenPBRSurface,
                         surface->transmission_dispersion_scale)

    // Subsurface properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_weight", MtlxOpenPBRSurface,
                         surface->subsurface_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_color", MtlxOpenPBRSurface,
                         surface->subsurface_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_radius", MtlxOpenPBRSurface,
                         surface->subsurface_radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_radius_scale", MtlxOpenPBRSurface,
                         surface->subsurface_radius_scale)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_scale", MtlxOpenPBRSurface,
                         surface->subsurface_scale)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_anisotropy", MtlxOpenPBRSurface,
                         surface->subsurface_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_scatter_anisotropy", MtlxOpenPBRSurface,
                         surface->subsurface_scatter_anisotropy)

    // Coat properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_weight", MtlxOpenPBRSurface,
                         surface->coat_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_color", MtlxOpenPBRSurface,
                         surface->coat_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_roughness", MtlxOpenPBRSurface,
                         surface->coat_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_anisotropy", MtlxOpenPBRSurface,
                         surface->coat_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_rotation", MtlxOpenPBRSurface,
                         surface->coat_rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_roughness_anisotropy", MtlxOpenPBRSurface,
                         surface->coat_roughness_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_ior", MtlxOpenPBRSurface,
                         surface->coat_ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_darkening", MtlxOpenPBRSurface,
                         surface->coat_darkening)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_color", MtlxOpenPBRSurface,
                         surface->coat_affect_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_roughness", MtlxOpenPBRSurface,
                         surface->coat_affect_roughness)

    // Fuzz properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_weight", MtlxOpenPBRSurface,
                         surface->fuzz_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_color", MtlxOpenPBRSurface,
                         surface->fuzz_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_roughness", MtlxOpenPBRSurface,
                         surface->fuzz_roughness)

    // Thin film properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_thickness", MtlxOpenPBRSurface,
                         surface->thin_film_thickness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_ior", MtlxOpenPBRSurface,
                         surface->thin_film_ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_weight", MtlxOpenPBRSurface,
                         surface->thin_film_weight)

    // Emission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission_luminance", MtlxOpenPBRSurface,
                         surface->emission_luminance)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission_color", MtlxOpenPBRSurface,
                         surface->emission_color)

    // Geometry properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_opacity", MtlxOpenPBRSurface,
                         surface->geometry_opacity)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_thin_walled", MtlxOpenPBRSurface,
                         surface->geometry_thin_walled)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_normal", MtlxOpenPBRSurface,
                         surface->geometry_normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_tangent", MtlxOpenPBRSurface,
                         surface->geometry_tangent)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_coat_normal", MtlxOpenPBRSurface,
                         surface->geometry_coat_normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_coat_tangent", MtlxOpenPBRSurface,
                         surface->geometry_coat_tangent)

    // Output
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:surface", MtlxOpenPBRSurface,
                   surface->surface)

    ADD_PROPERTY(table, prop, MtlxOpenPBRSurface, surface->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<OpenPBRSurface>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    OpenPBRSurface *surface,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec;
  (void)references;
  (void)options;
  (void)warn;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  // OpenPBR's canonical opacity input is `geometry_opacity`, but many USD assets
  // use the UsdPreviewSurface-style `opacity`. Both map to the single `opacity`
  // field. Parse the legacy `opacity` into a side holder (a copy of the field so
  // type + fallback match) so that `geometry_opacity` stays authoritative
  // regardless of property iteration order; reconcile after the loop and warn if
  // both are authored.
  auto legacy_opacity = surface->opacity;
  auto legacy_normal = surface->normal;
  auto legacy_tangent = surface->tangent;

  for (auto &prop : properties) {
    // Base layer properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_weight", OpenPBRSurface,
                         surface->base_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_color", OpenPBRSurface,
                         surface->base_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_roughness", OpenPBRSurface,
                         surface->base_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_metalness", OpenPBRSurface,
                         surface->base_metalness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:base_diffuse_roughness", OpenPBRSurface,
                         surface->base_diffuse_roughness)

    // Specular layer properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_weight", OpenPBRSurface,
                         surface->specular_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_color", OpenPBRSurface,
                         surface->specular_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_roughness", OpenPBRSurface,
                         surface->specular_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_ior", OpenPBRSurface,
                         surface->specular_ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_ior_level", OpenPBRSurface,
                         surface->specular_ior_level)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_anisotropy", OpenPBRSurface,
                         surface->specular_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_rotation", OpenPBRSurface,
                         surface->specular_rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular_roughness_anisotropy", OpenPBRSurface,
                         surface->specular_roughness_anisotropy)

    // Transmission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_weight", OpenPBRSurface,
                         surface->transmission_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_color", OpenPBRSurface,
                         surface->transmission_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_depth", OpenPBRSurface,
                         surface->transmission_depth)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter", OpenPBRSurface,
                         surface->transmission_scatter)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_scatter_anisotropy", OpenPBRSurface,
                         surface->transmission_scatter_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion", OpenPBRSurface,
                         surface->transmission_dispersion)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion_abbe_number", OpenPBRSurface,
                         surface->transmission_dispersion_abbe_number)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:transmission_dispersion_scale", OpenPBRSurface,
                         surface->transmission_dispersion_scale)

    // Subsurface properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_weight", OpenPBRSurface,
                         surface->subsurface_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_color", OpenPBRSurface,
                         surface->subsurface_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_radius", OpenPBRSurface,
                         surface->subsurface_radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_scale", OpenPBRSurface,
                         surface->subsurface_scale)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_anisotropy", OpenPBRSurface,
                         surface->subsurface_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:subsurface_scatter_anisotropy", OpenPBRSurface,
                         surface->subsurface_scatter_anisotropy)

    // Sheen properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen_weight", OpenPBRSurface,
                         surface->sheen_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen_color", OpenPBRSurface,
                         surface->sheen_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:sheen_roughness", OpenPBRSurface,
                         surface->sheen_roughness)

    // Fuzz properties (velvet/fabric-like appearance)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_weight", OpenPBRSurface,
                         surface->fuzz_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_color", OpenPBRSurface,
                         surface->fuzz_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fuzz_roughness", OpenPBRSurface,
                         surface->fuzz_roughness)

    // Thin film properties (iridescence from thin film interference)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_weight", OpenPBRSurface,
                         surface->thin_film_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_thickness", OpenPBRSurface,
                         surface->thin_film_thickness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:thin_film_ior", OpenPBRSurface,
                         surface->thin_film_ior)

    // Coat properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_weight", OpenPBRSurface,
                         surface->coat_weight)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_color", OpenPBRSurface,
                         surface->coat_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_roughness", OpenPBRSurface,
                         surface->coat_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_anisotropy", OpenPBRSurface,
                         surface->coat_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_rotation", OpenPBRSurface,
                         surface->coat_rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_ior", OpenPBRSurface,
                         surface->coat_ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_color", OpenPBRSurface,
                         surface->coat_affect_color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_affect_roughness", OpenPBRSurface,
                         surface->coat_affect_roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_roughness_anisotropy", OpenPBRSurface,
                         surface->coat_roughness_anisotropy)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:coat_darkening", OpenPBRSurface,
                         surface->coat_darkening)

    // Emission properties
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission_luminance", OpenPBRSurface,
                         surface->emission_luminance)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emission_color", OpenPBRSurface,
                         surface->emission_color)

    // Geometry properties.
    // `inputs:opacity` (UsdPreviewSurface-style legacy name) parses into a side
    // holder; `inputs:geometry_opacity` (OpenPBR canonical) parses into the
    // field directly. Reconciled after the loop so geometry_opacity wins.
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:opacity", OpenPBRSurface,
                         legacy_opacity)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_opacity", OpenPBRSurface,
                         surface->opacity)  // OpenPBR standard name, maps to same opacity field
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:normal", OpenPBRSurface,
                         legacy_normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_normal", OpenPBRSurface,
                         surface->normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:tangent", OpenPBRSurface,
                         legacy_tangent)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:geometry_tangent", OpenPBRSurface,
                         surface->tangent)

    // Outputs
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:surface", OpenPBRSurface,
                   surface->surface)

    ADD_PROPERTY(table, prop, OpenPBRSurface, surface->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  // Reconcile the two opacity spellings (see note above the loop).
  if (surface->opacity.authored() && legacy_opacity.authored()) {
    PUSH_WARN(
        "OpenPBRSurface: both `inputs:opacity` and `inputs:geometry_opacity` are "
        "authored; using the OpenPBR-canonical `inputs:geometry_opacity` and "
        "ignoring `inputs:opacity`.");
  } else if (!surface->opacity.authored() && legacy_opacity.authored()) {
    surface->opacity = legacy_opacity;
  }
  if (!surface->normal.authored() && legacy_normal.authored()) {
    surface->normal = legacy_normal;
  }
  if (!surface->tangent.authored() && legacy_tangent.authored()) {
    surface->tangent = legacy_tangent;
  }

  return true;
}


}  // namespace prim
}  // namespace lightusd
