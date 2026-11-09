// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
/// @file enum-handlers.hh
/// @brief Centralized enum handlers for USD token-to-enum conversion
///
/// This file provides type-safe handlers for converting USD token strings
/// to their corresponding C++ enum values. Used primarily during prim
/// reconstruction from parsed USD data.
///

#pragma once

#include <string>
#include <vector>
#include <utility>

#include "nonstd/expected.hpp"

#include "core/prim-enums.hh"     // Axis, Visibility, Purpose, Orientation, Kind, etc.
#include "core/collection-api.hh" // CollectionInstance
#include "core/composition-types.hh" // APISchemas
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdSkel.hh"

namespace lightusd {

/// Template type alias for enum handler functions
template <typename EnumTy>
using EnumHandlerFun = std::function<nonstd::expected<EnumTy, std::string>(
    const std::string &)>;

namespace enum_handler {

//
// Common/shared enum handlers
//

/// Handle Axis enum (X, Y, Z)
nonstd::expected<Axis, std::string> Axis(const std::string &tok);

/// Handle Visibility enum (inherited, invisible)
nonstd::expected<Visibility, std::string> Visibility(const std::string &tok);

/// Handle Purpose enum (default, proxy, render, guide)
nonstd::expected<Purpose, std::string> Purpose(const std::string &tok);

/// Handle Orientation enum (rightHanded, leftHanded)
nonstd::expected<Orientation, std::string> Orientation(const std::string &tok);

//
// GeomMesh enum handlers
//

/// Handle GeomMesh::SubdivisionScheme enum
nonstd::expected<GeomMesh::SubdivisionScheme, std::string>
SubdivisionScheme(const std::string &tok);

/// Handle GeomMesh::InterpolateBoundary enum
nonstd::expected<GeomMesh::InterpolateBoundary, std::string>
InterpolateBoundary(const std::string &tok);

/// Handle GeomMesh::FaceVaryingLinearInterpolation enum
nonstd::expected<GeomMesh::FaceVaryingLinearInterpolation, std::string>
FaceVaryingLinearInterpolation(const std::string &tok);

/// Handle GeomMesh::TriangleSubdivisionRule enum
nonstd::expected<GeomMesh::TriangleSubdivisionRule, std::string>
TriangleSubdivisionRule(const std::string &tok);

//
// GeomSubset enum handlers
//

/// Handle GeomSubset::ElementType enum (face, point)
nonstd::expected<GeomSubset::ElementType, std::string>
ElementType(const std::string &tok);

/// Handle GeomSubset::FamilyType enum (partition, nonOverlapping, unrestricted)
nonstd::expected<GeomSubset::FamilyType, std::string>
FamilyType(const std::string &tok);

//
// GeomBasisCurves enum handlers
//

/// Handle GeomBasisCurves::Basis enum (bezier, bspline, catmullRom)
nonstd::expected<GeomBasisCurves::Basis, std::string>
BasisCurvesBasis(const std::string &tok);

/// Handle GeomBasisCurves::Type enum (cubic, linear)
nonstd::expected<GeomBasisCurves::Type, std::string>
BasisCurvesType(const std::string &tok);

/// Handle GeomBasisCurves::Wrap enum (nonperiodic, periodic, pinned)
nonstd::expected<GeomBasisCurves::Wrap, std::string>
BasisCurvesWrap(const std::string &tok);

//
// GeomCamera enum handlers
//

/// Handle GeomCamera::Projection enum (perspective, orthographic)
nonstd::expected<GeomCamera::Projection, std::string>
CameraProjection(const std::string &tok);

/// Handle GeomCamera::StereoRole enum (mono, left, right)
nonstd::expected<GeomCamera::StereoRole, std::string>
CameraStereoRole(const std::string &tok);

//
// UsdPreviewSurface enum handlers
//

/// Handle UsdPreviewSurface::OpacityMode enum (transparent, presence)
nonstd::expected<UsdPreviewSurface::OpacityMode, std::string>
OpacityMode(const std::string &tok);

//
// UsdUVTexture enum handlers
//

/// Handle UsdUVTexture::SourceColorSpace enum (auto, raw, sRGB)
nonstd::expected<UsdUVTexture::SourceColorSpace, std::string>
SourceColorSpace(const std::string &tok);

/// Handle UsdUVTexture::Wrap enum (useMetadata, black, clamp, repeat, mirror)
nonstd::expected<UsdUVTexture::Wrap, std::string>
TextureWrap(const std::string &tok);

//
// Collection enum handlers
//

/// Handle CollectionInstance::ExpansionRule enum
nonstd::expected<CollectionInstance::ExpansionRule, std::string>
ExpansionRule(const std::string &tok);

//
// APISchemas enum handlers
//

/// Handle APISchemas::APIName enum
/// Returns error for unknown schema names
nonstd::expected<APISchemas::APIName, std::string>
APISchemaName(const std::string &tok);

/// Handle APISchemas::APIName enum (optional version)
/// Returns nullopt for unknown schema names (useful for ignore_unknown mode)
nonstd::optional<APISchemas::APIName>
APISchemaNameOpt(const std::string &tok);

/// Resolve an apiSchemas token that may carry a multi-apply instance name.
/// USD writes multi-apply instances as `SchemaName:instanceName` (split on the
/// first ':'). Looks up the base schema name; on success returns
/// `(APIName, instanceName)` where `instanceName` is empty for single-apply.
/// Returns nullopt when the base name is not a known schema.
nonstd::optional<std::pair<APISchemas::APIName, std::string>>
APISchemaNameWithInstanceOpt(const std::string &tok);

}  // namespace enum_handler
}  // namespace lightusd
