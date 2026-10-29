// SPDX-License-Identifier: Apache 2.0
// Copyright 2020-2023 Syoyo Fujita.
// Copyright 2023-Present Light Transport Entertainment Inc.
//
// Common Prim reconstruction modules both for USDA and USDC.
//
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/prim-spec.hh"  // PrimSpec, PropertyMap, ReferenceList, Specifier (transitively: property, composition-types, prim-enums)
#include "prim-reconstruct-common.hh"  // ParsePropertyTable
#include "core/xform-op.hh"   // XformOp

namespace lightusd {
namespace prim {

struct PrimReconstructOptions
{
  bool strict_allowedToken_check{false};

  // When true, a Shader prim that declares `info:id` without a value
  // is an error. When false (default), it is demoted to a warning and
  // the Shader is reconstructed as a generic ShaderNode — matching the
  // pattern that Omniverse Kit emits for MDL shaders (where `info:id`
  // is a placeholder and the real shader identity comes from
  // `info:implementationSource` + `info:mdl:sourceAsset`).
  bool strict_shader_check{false};

  // When true, a shader property(input/output) whose authored Sdf type does
  // not match the canonical schema type(from `info:id` / UsdPreviewSurface
  // spec) is a parse error. When false(default), it is accepted with a
  // warning: the canonical schema type is kept for connection/render
  // semantics(matching OpenUSD, which does not validate shader output types).
  bool strict_shader_type_check{false};

  // MaterialX validation options
  bool validate_mtlx_connection_types{false};
  bool validate_mtlx_info_id{false};
  bool validate_mtlx_connection_targets{false};
  bool validate_mtlx_duplicate_names{false};
  bool validate_mtlx_index_bounds{false};
  bool strict_mtlx_check{false};  // Enable all above

  std::function<std::string(const std::string &property_name)>
      format_property_source_diagnostic;
  std::function<std::string()> format_prim_source_diagnostic;
  std::function<std::string(const std::string &property_name)>
      format_property_path;
  std::function<std::string()> format_prim_path;
};


///
/// Reconstruct property with `xformOp:***` namespace in `properties` to `XformOp` class.
/// Corresponding property are looked up from names in `xformOpOrder`(`token[]`) property.
/// Name of processed xformOp properties are added to `table`
/// TODO: Move to prim-reconstruct.cc?
///
bool ReconstructXformOpsFromProperties(
      const Specifier &spec,
      ParsePropertyTable &table, /* inout */
      PropertyMap &properties,
      std::vector<XformOp> *xformOps,
      std::string *err);

///
/// Reconstruct concrete Prim(e.g. Xform, GeomMesh) from `properties`.
///
template <typename T>
bool ReconstructPrim(
    const Specifier &spec,
    PropertyMap &properties, // modified
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options = PrimReconstructOptions());

///
/// Reconstruct concrete Prim(e.g. Xform, GeomMesh) from PrimSpec.
///
template <typename T>
bool ReconstructPrim(
    PrimSpec &primspec,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options = PrimReconstructOptions());


} // namespace prim
} // namespace lightusd
