// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "enum-handlers.hh"

#include <array>
#include <algorithm>

#include "str-util.hh"
#include "tiny-format.hh"

namespace lightusd {
namespace enum_handler {

namespace {

// Helper template for enum token lookup
template <typename EnumT, size_t N>
nonstd::expected<EnumT, std::string> LookupEnum(
    const std::string &prop_name,
    const std::string &tok,
    const std::array<std::pair<EnumT, const char *>, N> &enums) {
  for (const auto &item : enums) {
    if (tok == item.second) {
      return item.first;
    }
  }

  // Build error message with allowed tokens
  std::vector<std::string> allowed;
  for (const auto &item : enums) {
    allowed.push_back(quote(item.second));
  }
  return nonstd::make_unexpected(
      fmt::format("Invalid token for `{}`. Allowed: [{}], got: {}",
                  prop_name, join(", ", allowed), quote(tok)));
}

}  // namespace

//
// Common/shared enum handlers
//

nonstd::expected<lightusd::Axis, std::string> Axis(const std::string &tok) {
  using E = lightusd::Axis;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::X, "X"},
      {E::Y, "Y"},
      {E::Z, "Z"},
  }};
  return LookupEnum("axis", tok, enums);
}

nonstd::expected<lightusd::Visibility, std::string> Visibility(const std::string &tok) {
  using E = lightusd::Visibility;
  constexpr std::array<std::pair<E, const char *>, 2> enums = {{
      {E::Inherited, "inherited"},
      {E::Invisible, "invisible"},
  }};
  return LookupEnum("visibility", tok, enums);
}

nonstd::expected<lightusd::Purpose, std::string> Purpose(const std::string &tok) {
  using E = lightusd::Purpose;
  constexpr std::array<std::pair<E, const char *>, 4> enums = {{
      {E::Default, "default"},
      {E::Proxy, "proxy"},
      {E::Render, "render"},
      {E::Guide, "guide"},
  }};
  return LookupEnum("purpose", tok, enums);
}

nonstd::expected<lightusd::Orientation, std::string> Orientation(const std::string &tok) {
  using E = lightusd::Orientation;
  constexpr std::array<std::pair<E, const char *>, 2> enums = {{
      {E::RightHanded, "rightHanded"},
      {E::LeftHanded, "leftHanded"},
  }};
  return LookupEnum("orientation", tok, enums);
}

//
// GeomMesh enum handlers
//

nonstd::expected<GeomMesh::SubdivisionScheme, std::string>
SubdivisionScheme(const std::string &tok) {
  using E = GeomMesh::SubdivisionScheme;
  constexpr std::array<std::pair<E, const char *>, 4> enums = {{
      {E::SubdivisionSchemeNone, "none"},
      {E::CatmullClark, "catmullClark"},
      {E::Loop, "loop"},
      {E::Bilinear, "bilinear"},
  }};
  return LookupEnum("subdivisionScheme", tok, enums);
}

nonstd::expected<GeomMesh::InterpolateBoundary, std::string>
InterpolateBoundary(const std::string &tok) {
  using E = GeomMesh::InterpolateBoundary;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::InterpolateBoundaryNone, "none"},
      {E::EdgeAndCorner, "edgeAndCorner"},
      {E::EdgeOnly, "edgeOnly"},
  }};
  return LookupEnum("interpolateBoundary", tok, enums);
}

nonstd::expected<GeomMesh::FaceVaryingLinearInterpolation, std::string>
FaceVaryingLinearInterpolation(const std::string &tok) {
  using E = GeomMesh::FaceVaryingLinearInterpolation;
  constexpr std::array<std::pair<E, const char *>, 6> enums = {{
      {E::CornersPlus1, "cornersPlus1"},
      {E::CornersPlus2, "cornersPlus2"},
      {E::CornersOnly, "cornersOnly"},
      {E::Boundaries, "boundaries"},
      {E::FaceVaryingLinearInterpolationNone, "none"},
      {E::All, "all"},
  }};
  return LookupEnum("faceVaryingLinearInterpolation", tok, enums);
}

nonstd::expected<GeomMesh::TriangleSubdivisionRule, std::string>
TriangleSubdivisionRule(const std::string &tok) {
  using E = GeomMesh::TriangleSubdivisionRule;
  constexpr std::array<std::pair<E, const char *>, 2> enums = {{
      {E::CatmullClark, "catmullClark"},
      {E::Smooth, "smooth"},
  }};
  return LookupEnum("triangleSubdivisionRule", tok, enums);
}

//
// GeomSubset enum handlers
//

nonstd::expected<GeomSubset::ElementType, std::string>
ElementType(const std::string &tok) {
  using E = GeomSubset::ElementType;
  constexpr std::array<std::pair<E, const char *>, 4> enums = {{
      {E::Face, "face"},
      {E::Point, "point"},
      {E::Edge, "edge"},
      {E::Tetrahedron, "tetrahedron"},
  }};
  return LookupEnum("elementType", tok, enums);
}

nonstd::expected<GeomSubset::FamilyType, std::string>
FamilyType(const std::string &tok) {
  using E = GeomSubset::FamilyType;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Partition, "partition"},
      {E::NonOverlapping, "nonOverlapping"},
      {E::Unrestricted, "unrestricted"},
  }};
  return LookupEnum("familyType", tok, enums);
}

//
// GeomBasisCurves enum handlers
//

nonstd::expected<GeomBasisCurves::Basis, std::string>
BasisCurvesBasis(const std::string &tok) {
  using E = GeomBasisCurves::Basis;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Bezier, "bezier"},
      {E::Bspline, "bspline"},
      {E::CatmullRom, "catmullRom"},
  }};
  return LookupEnum("basis", tok, enums);
}

nonstd::expected<GeomBasisCurves::Type, std::string>
BasisCurvesType(const std::string &tok) {
  using E = GeomBasisCurves::Type;
  constexpr std::array<std::pair<E, const char *>, 2> enums = {{
      {E::Cubic, "cubic"},
      {E::Linear, "linear"},
  }};
  return LookupEnum("type", tok, enums);
}

nonstd::expected<GeomBasisCurves::Wrap, std::string>
BasisCurvesWrap(const std::string &tok) {
  using E = GeomBasisCurves::Wrap;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Nonperiodic, "nonperiodic"},
      {E::Periodic, "periodic"},
      {E::Pinned, "pinned"},
  }};
  return LookupEnum("wrap", tok, enums);
}

//
// GeomCamera enum handlers
//

nonstd::expected<GeomCamera::Projection, std::string>
CameraProjection(const std::string &tok) {
  using E = GeomCamera::Projection;
  constexpr std::array<std::pair<E, const char *>, 2> enums = {{
      {E::Perspective, "perspective"},
      {E::Orthographic, "orthographic"},
  }};
  return LookupEnum("projection", tok, enums);
}

nonstd::expected<GeomCamera::StereoRole, std::string>
CameraStereoRole(const std::string &tok) {
  using E = GeomCamera::StereoRole;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Mono, "mono"},
      {E::Left, "left"},
      {E::Right, "right"},
  }};
  return LookupEnum("stereoRole", tok, enums);
}

//
// UsdPreviewSurface enum handlers
//

nonstd::expected<UsdPreviewSurface::OpacityMode, std::string>
OpacityMode(const std::string &tok) {
  using E = UsdPreviewSurface::OpacityMode;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Opacity, "opacity"},
      {E::Transparent, "transparent"},
      {E::Presence, "presence"},
  }};
  return LookupEnum("inputs:opacityMode", tok, enums);
}

//
// UsdUVTexture enum handlers
//

nonstd::expected<UsdUVTexture::SourceColorSpace, std::string>
SourceColorSpace(const std::string &tok) {
  using E = UsdUVTexture::SourceColorSpace;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::Auto, "auto"},
      {E::Raw, "raw"},
      {E::SRGB, "sRGB"},
  }};
  return LookupEnum("inputs:sourceColorSpace", tok, enums);
}

nonstd::expected<UsdUVTexture::Wrap, std::string>
TextureWrap(const std::string &tok) {
  using E = UsdUVTexture::Wrap;
  constexpr std::array<std::pair<E, const char *>, 5> enums = {{
      {E::UseMetadata, "useMetadata"},
      {E::Black, "black"},
      {E::Clamp, "clamp"},
      {E::Repeat, "repeat"},
      {E::Mirror, "mirror"},
  }};
  return LookupEnum("wrap", tok, enums);
}

//
// Collection enum handlers
//

nonstd::expected<CollectionInstance::ExpansionRule, std::string>
ExpansionRule(const std::string &tok) {
  using E = CollectionInstance::ExpansionRule;
  constexpr std::array<std::pair<E, const char *>, 3> enums = {{
      {E::ExplicitOnly, "explicitOnly"},
      {E::ExpandPrims, "expandPrims"},
      {E::ExpandPrimsAndProperties, "expandPrimsAndProperties"},
  }};
  return LookupEnum("expansionRule", tok, enums);
}

//
// APISchemas enum handlers
//

nonstd::expected<APISchemas::APIName, std::string>
APISchemaName(const std::string &tok) {
  using E = APISchemas::APIName;
  constexpr std::array<std::pair<E, const char *>, 65> enums = {{
      {E::SkelBindingAPI, "SkelBindingAPI"},
      {E::CollectionAPI, "CollectionAPI"},
      {E::ColorSpaceAPI, "ColorSpaceAPI"},
      {E::ColorSpaceDefinitionAPI, "ColorSpaceDefinitionAPI"},
      {E::MaterialBindingAPI, "MaterialBindingAPI"},
      {E::ShapingAPI, "ShapingAPI"},
      {E::ShadowAPI, "ShadowAPI"},
      {E::VolumeLightAPI, "VolumeLightAPI"},
      {E::Preliminary_PhysicsMaterialAPI, "Preliminary_PhysicsMaterialAPI"},
      {E::Preliminary_PhysicsRigidBodyAPI, "Preliminary_PhysicsRigidBodyAPI"},
      {E::Preliminary_PhysicsColliderAPI, "Preliminary_PhysicsColliderAPI"},
      {E::Preliminary_AnchoringAPI, "Preliminary_AnchoringAPI"},
      {E::LightAPI, "LightAPI"},
      {E::MeshLightAPI, "MeshLightAPI"},
      {E::LightListAPI, "LightListAPI"},
      {E::ListAPI, "ListAPI"},
      {E::MotionAPI, "MotionAPI"},
      {E::PrimvarsAPI, "PrimvarsAPI"},
      {E::GeomModelAPI, "GeomModelAPI"},
      {E::VisibilityAPI, "VisibilityAPI"},
      {E::XformCommonAPI, "XformCommonAPI"},
      {E::NodeDefAPI, "NodeDefAPI"},
      {E::CoordSysAPI, "CoordSysAPI"},
      {E::ConnectableAPI, "ConnectableAPI"},
      {E::MaterialXConfigAPI, "MaterialXConfigAPI"},
      {E::MaterialUEConfigAPI, "MaterialUEConfigAPI"},
      // UsdPhysics
      {E::PhysicsRigidBodyAPI, "PhysicsRigidBodyAPI"},
      {E::PhysicsCollisionAPI, "PhysicsCollisionAPI"},
      {E::PhysicsMaterialAPI, "PhysicsMaterialAPI"},
      {E::PhysicsMeshCollisionAPI, "PhysicsMeshCollisionAPI"},
      // MuJoCo (mjcPhysics)
      {E::MjcSceneAPI, "MjcSceneAPI"},
      {E::MjcJointAPI, "MjcJointAPI"},
      {E::MjcCollisionAPI, "MjcCollisionAPI"},
      {E::MjcMeshCollisionAPI, "MjcMeshCollisionAPI"},
      {E::MjcMaterialAPI, "MjcMaterialAPI"},
      {E::MjcSiteAPI, "MjcSiteAPI"},
      {E::MjcImageableAPI, "MjcImageableAPI"},
      {E::MjcEqualityAPI, "MjcEqualityAPI"},
      {E::MjcEqualityConnectAPI, "MjcEqualityConnectAPI"},
      {E::MjcEqualityWeldAPI, "MjcEqualityWeldAPI"},
      {E::MjcEqualityJointAPI, "MjcEqualityJointAPI"},
      // Newton physics
      {E::NewtonSceneAPI, "NewtonSceneAPI"},
      {E::NewtonXpbdSceneAPI, "NewtonXpbdSceneAPI"},
      {E::NewtonKaminoSceneAPI, "NewtonKaminoSceneAPI"},
      {E::NewtonArticulationRootAPI, "NewtonArticulationRootAPI"},
      {E::NewtonCollisionAPI, "NewtonCollisionAPI"},
      {E::NewtonMeshCollisionAPI, "NewtonMeshCollisionAPI"},
      {E::NewtonMaterialAPI, "NewtonMaterialAPI"},
      {E::NewtonMimicAPI, "NewtonMimicAPI"},
      {E::NewtonActuatorDelayAPI, "NewtonActuatorDelayAPI"},
      {E::NewtonActuatorControlBaseAPI, "NewtonActuatorControlBaseAPI"},
      {E::NewtonPDControlAPI, "NewtonPDControlAPI"},
      {E::NewtonPIDControlAPI, "NewtonPIDControlAPI"},
      {E::NewtonNeuralControlAPI, "NewtonNeuralControlAPI"},
      {E::NewtonActuatorClampingBaseAPI, "NewtonActuatorClampingBaseAPI"},
      {E::NewtonMaxEffortClampingAPI, "NewtonMaxEffortClampingAPI"},
      {E::NewtonDCMotorClampingAPI, "NewtonDCMotorClampingAPI"},
      {E::NewtonPositionBasedClampingAPI, "NewtonPositionBasedClampingAPI"},
      // Additional UsdPhysics
      {E::PhysicsMassAPI, "PhysicsMassAPI"},
      {E::PhysicsFilteredPairsAPI, "PhysicsFilteredPairsAPI"},
      {E::PhysicsArticulationRootAPI, "PhysicsArticulationRootAPI"},
      // PhysX (Omniverse)
      {E::PhysxJointAPI, "PhysxJointAPI"},
      // UsdMedia
      {E::AssetPreviewsAPI, "AssetPreviewsAPI"},
      // Multi-apply
      {E::PhysicsDriveAPI, "PhysicsDriveAPI"},
      {E::PhysicsLimitAPI, "PhysicsLimitAPI"},
  }};
  return LookupEnum("apiSchemas", tok, enums);
}

nonstd::optional<APISchemas::APIName>
APISchemaNameOpt(const std::string &tok) {
  auto result = APISchemaName(tok);
  if (result) {
    return result.value();
  }
  return nonstd::nullopt;
}

nonstd::optional<std::pair<APISchemas::APIName, std::string>>
APISchemaNameWithInstanceOpt(const std::string &tok) {
  // Multi-apply instances are authored as `SchemaName:instanceName`; the
  // instance name follows the first ':'. (Schema names are bare identifiers, so
  // any ':' separates the base schema from its instance.)
  std::string base = tok;
  std::string instance;
  const auto pos = tok.find(':');
  if (pos != std::string::npos) {
    base = tok.substr(0, pos);
    instance = tok.substr(pos + 1);
  }
  if (auto e = APISchemaNameOpt(base)) {
    return std::make_pair(e.value(), instance);
  }
  return nonstd::nullopt;
}

}  // namespace enum_handler
}  // namespace lightusd
