// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
/// @file prim-property-tables.hh
/// @brief Property descriptor tables for prim reconstruction
///
/// This file defines property tables using X-macros to reduce code duplication
/// in prim-reconstruct.cc. Each prim type has a property table that lists
/// all its typed attributes, relationships, and enum properties.
///
/// Usage in prim-reconstruct.cc:
///   for (auto& prop : properties) {
///     GEOM_MESH_PROPERTIES(TYPED_ATTR, RELATION, ENUM_PROP)
///     // special handling...
///     ADD_PROPERTY(...)
///   }

#pragma once

//
// X-macro format:
//   TYPED_ATTR(prop_name, member_name)
//   TYPED_ATTR_NOCONT(prop_name, member_name)  // no continue
//   UNIFORM_ENUM(prop_name, enum_type, handler, member_name)
//   TIMESAMPLED_ENUM(prop_name, enum_type, handler, member_name)
//   SINGLE_REL(prop_name, member_name)
//   MULTI_REL(prop_name, member_name)
//   EXTENT(prop_name, member_name)
//

// ============================================================================
// GeomMesh Properties
// ============================================================================
#define GEOM_MESH_TYPED_ATTRS(X) \
  X("points", points) \
  X("normals", normals) \
  X("faceVertexCounts", faceVertexCounts) \
  X("faceVertexIndices", faceVertexIndices) \
  X("cornerIndices", cornerIndices) \
  X("cornerSharpnesses", cornerSharpnesses) \
  X("creaseIndices", creaseIndices) \
  X("creaseLengths", creaseLengths) \
  X("creaseSharpnesses", creaseSharpnesses) \
  X("holeIndices", holeIndices) \
  X("velocities", velocities)

#define GEOM_MESH_SKEL_ATTRS(X) \
  X(kSkelBlendShapes, blendShapes)

#define GEOM_MESH_RELATIONS(SINGLE, MULTI) \
  SINGLE(kSkelSkeleton, skeleton) \
  MULTI(kSkelBlendShapeTargets, blendShapeTargets)

#define GEOM_MESH_UNIFORM_ENUMS(X) \
  X("subdivisionScheme", GeomMesh::SubdivisionScheme, SubdivisionSchemeHandler, subdivisionScheme) \
  X("triangleSubdivisionRule", GeomMesh::TriangleSubdivisionRule, TriangleSubdivisionRuleHandler, triangleSubdivisionRule)

#define GEOM_MESH_TIMESAMPLED_ENUMS(X) \
  X("interpolateBoundary", GeomMesh::InterpolateBoundary, InterpolateBoundaryHandler, interpolateBoundary) \
  X("faceVaryingLinearInterpolation", GeomMesh::FaceVaryingLinearInterpolation, FaceVaryingLinearInterpolationHandler, faceVaryingLinearInterpolation)

// ============================================================================
// GeomCamera Properties
// ============================================================================
#define GEOM_CAMERA_TYPED_ATTRS(X) \
  X("focalLength", focalLength) \
  X("focusDistance", focusDistance) \
  X("exposure", exposure) \
  X("fStop", fStop) \
  X("horizontalAperture", horizontalAperture) \
  X("horizontalApertureOffset", horizontalApertureOffset) \
  X("verticalAperture", verticalAperture) \
  X("verticalApertureOffset", verticalApertureOffset) \
  X("clippingRange", clippingRange) \
  X("clippingPlanes", clippingPlanes) \
  X("shutter:open", shutterOpen) \
  X("shutter:close", shutterClose)

#define GEOM_CAMERA_TIMESAMPLED_ENUMS(X) \
  X("projection", GeomCamera::Projection, ProjectionHandler, projection)

#define GEOM_CAMERA_UNIFORM_ENUMS(X) \
  X("stereoRole", GeomCamera::StereoRole, StereoRoleHandler, stereoRole)

// ============================================================================
// GeomSubset Properties
// ============================================================================
#define GEOM_SUBSET_TYPED_ATTRS(X) \
  X("familyName", familyName) \
  X("indices", indices)

#define GEOM_SUBSET_UNIFORM_ENUMS(X) \
  X("elementType", GeomSubset::ElementType, ElementTypeHandler, elementType)

// ============================================================================
// GeomPointInstancer Properties
// ============================================================================
#define GEOM_POINT_INSTANCER_TYPED_ATTRS(X) \
  X("protoIndices", protoIndices) \
  X("ids", ids) \
  X("positions", positions) \
  X("orientations", orientations) \
  X("scales", scales) \
  X("velocities", velocities) \
  X("accelerations", accelerations) \
  X("angularVelocities", angularVelocities) \
  X("invisibleIds", invisibleIds) \
  X("inactiveIds", inactiveIds)

#define GEOM_POINT_INSTANCER_RELATIONS(SINGLE, MULTI) \
  MULTI("prototypes", prototypes)

// ============================================================================
// GeomBasisCurves Properties
// ============================================================================
#define GEOM_BASIS_CURVES_TYPED_ATTRS(X) \
  X("points", points) \
  X("normals", normals) \
  X("curveVertexCounts", curveVertexCounts) \
  X("widths", widths) \
  X("velocities", velocities) \
  X("accelerations", accelerations)

#define GEOM_BASIS_CURVES_UNIFORM_ENUMS(X) \
  X("type", GeomBasisCurves::Type, TypeHandler, type) \
  X("basis", GeomBasisCurves::Basis, BasisHandler, basis) \
  X("wrap", GeomBasisCurves::Wrap, WrapHandler, wrap)

// ============================================================================
// GeomPoints Properties
// ============================================================================
#define GEOM_POINTS_TYPED_ATTRS(X) \
  X("points", points) \
  X("normals", normals) \
  X("widths", widths) \
  X("ids", ids) \
  X("velocities", velocities) \
  X("accelerations", accelerations)

// ============================================================================
// GeomSphere Properties
// ============================================================================
#define GEOM_SPHERE_TYPED_ATTRS(X) \
  X("radius", radius)

// ============================================================================
// UsdVol field-asset Properties (FieldAsset / OpenVDBAsset / Field3DAsset)
// ============================================================================
#define FIELD_ASSET_TYPED_ATTRS(X) \
  X("filePath", filePath) \
  X("fieldName", fieldName) \
  X("fieldDataType", fieldDataType) \
  X("fieldIndex", fieldIndex) \
  X("vectorDataRoleHint", vectorDataRoleHint)

#define OPENVDB_ASSET_TYPED_ATTRS(X) \
  FIELD_ASSET_TYPED_ATTRS(X) \
  X("fieldClass", fieldClass)

#define FIELD3D_ASSET_TYPED_ATTRS(X) \
  FIELD_ASSET_TYPED_ATTRS(X) \
  X("fieldPurpose", fieldPurpose)

// ============================================================================
// GeomCube Properties
// ============================================================================
#define GEOM_CUBE_TYPED_ATTRS(X) \
  X("size", size)

// ============================================================================
// GeomCone Properties
// ============================================================================
#define GEOM_CONE_TYPED_ATTRS(X) \
  X("radius", radius) \
  X("height", height)

#define GEOM_CONE_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomCylinder Properties
// ============================================================================
#define GEOM_CYLINDER_TYPED_ATTRS(X) \
  X("radius", radius) \
  X("height", height)

#define GEOM_CYLINDER_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomCapsule Properties
// ============================================================================
#define GEOM_CAPSULE_TYPED_ATTRS(X) \
  X("radius", radius) \
  X("height", height)

#define GEOM_CAPSULE_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomPlane Properties
// ============================================================================
#define GEOM_PLANE_TYPED_ATTRS(X) \
  X("width", width) \
  X("length", length)

#define GEOM_PLANE_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomCylinder_1 Properties
// ============================================================================
#define GEOM_CYLINDER_1_TYPED_ATTRS(X) \
  X("height", height) \
  X("radiusTop", radiusTop) \
  X("radiusBottom", radiusBottom)

#define GEOM_CYLINDER_1_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomCapsule_1 Properties
// ============================================================================
#define GEOM_CAPSULE_1_TYPED_ATTRS(X) \
  X("height", height) \
  X("radiusTop", radiusTop) \
  X("radiusBottom", radiusBottom)

#define GEOM_CAPSULE_1_UNIFORM_ENUMS(X) \
  X("axis", Axis, AxisEnumHandler, axis)

// ============================================================================
// GeomTetMesh Properties
// ============================================================================
#define GEOM_TET_MESH_TYPED_ATTRS(X) \
  X("points", points) \
  X("velocities", velocities) \
  X("accelerations", accelerations) \
  X("normals", normals) \
  X("tetVertexIndices", tetVertexIndices) \
  X("surfaceFaceVertexIndices", surfaceFaceVertexIndices)

// ============================================================================
// GeomNurbsPatch Properties
// ============================================================================
#define GEOM_NURBS_PATCH_TYPED_ATTRS(X) \
  X("points", points) \
  X("velocities", velocities) \
  X("accelerations", accelerations) \
  X("normals", normals) \
  X("uVertexCount", uVertexCount) \
  X("vVertexCount", vVertexCount) \
  X("uOrder", uOrder) \
  X("vOrder", vOrder) \
  X("uKnots", uKnots) \
  X("vKnots", vKnots) \
  X("uForm", uForm) \
  X("vForm", vForm) \
  X("uRange", uRange) \
  X("vRange", vRange) \
  X("pointWeights", pointWeights) \
  X("trimCurve:counts", trimCurve_counts) \
  X("trimCurve:orders", trimCurve_orders) \
  X("trimCurve:vertexCounts", trimCurve_vertexCounts) \
  X("trimCurve:knots", trimCurve_knots) \
  X("trimCurve:ranges", trimCurve_ranges) \
  X("trimCurve:points", trimCurve_points)

// ============================================================================
// GeomHermiteCurves Properties
// ============================================================================
#define GEOM_HERMITE_CURVES_TYPED_ATTRS(X) \
  X("points", points) \
  X("velocities", velocities) \
  X("accelerations", accelerations) \
  X("normals", normals) \
  X("curveVertexCounts", curveVertexCounts) \
  X("widths", widths) \
  X("tangents", tangents)

// ============================================================================
// Skeleton Properties
// ============================================================================
#define SKELETON_TYPED_ATTRS(X) \
  X("joints", joints) \
  X("jointNames", jointNames) \
  X("bindTransforms", bindTransforms) \
  X("restTransforms", restTransforms)

// ============================================================================
// SkelAnimation Properties
// ============================================================================
#define SKEL_ANIMATION_TYPED_ATTRS(X) \
  X("joints", joints) \
  X("translations", translations) \
  X("rotations", rotations) \
  X("scales", scales) \
  X("blendShapes", blendShapes) \
  X("blendShapeWeights", blendShapeWeights)

// ============================================================================
// BlendShape Properties
// ============================================================================
#define BLEND_SHAPE_TYPED_ATTRS(X) \
  X("offsets", offsets) \
  X("normalOffsets", normalOffsets) \
  X("pointIndices", pointIndices)

// ============================================================================
// Light Common Properties
// ============================================================================

// Shadow attributes - shared by ALL light types
#define LIGHT_SHADOW_ATTRS(X) \
  X("inputs:shadow:enable", shadowEnable) \
  X("inputs:shadow:color", shadowColor) \
  X("inputs:shadow:distance", shadowDistance) \
  X("inputs:shadow:falloff", shadowFalloff) \
  X("inputs:shadow:falloffGamma", shadowFalloffGamma)

// Shaping attributes - shared by SphereLight, RectLight, DiskLight, CylinderLight
#define LIGHT_SHAPING_ATTRS(X) \
  X("inputs:shaping:focus", shapingFocus) \
  X("inputs:shaping:focusTint", shapingFocusTint) \
  X("inputs:shaping:cone:angle", shapingConeAngle) \
  X("inputs:shaping:cone:softness", shapingConeSoftness)

// Base LightAPI radiometric inputs - shared by ALL light types. These mirror the
// writer's EXTRACT_COMMON_LIGHT (sconv-light.cc) plus the per-light color/intensity
// /exposure it emits, so every light parses exactly what it writes. Keeping these
// here (instead of per-light) prevents the readers from silently dropping inputs.
#define LIGHT_BASE_ATTRS(X) \
  X("inputs:color", color) \
  X("inputs:intensity", intensity) \
  X("inputs:exposure", exposure) \
  X("inputs:diffuse", diffuse) \
  X("inputs:specular", specular) \
  X("inputs:normalize", normalize) \
  X("inputs:enableColorTemperature", enableColorTemperature) \
  X("inputs:colorTemperature", colorTemperature)

// Composite macros for light reconstruction
// Use these in ReconstructPrim to reduce boilerplate

// For lights WITH shaping (SphereLight, RectLight, DiskLight, CylinderLight)
#define LIGHT_COMMON_ATTRS_WITH_SHAPING(X) \
  LIGHT_BASE_ATTRS(X) \
  LIGHT_SHADOW_ATTRS(X) \
  LIGHT_SHAPING_ATTRS(X)

// For lights WITHOUT shaping (DistantLight, GeometryLight, DomeLight)
#define LIGHT_COMMON_ATTRS_NO_SHAPING(X) \
  LIGHT_BASE_ATTRS(X) \
  LIGHT_SHADOW_ATTRS(X)

// Per-light macros below carry only the GEOMETRY-specific attributes; the shared
// LightAPI radiometric inputs (color/intensity/exposure/diffuse/specular/normalize
// /enableColorTemperature/colorTemperature) come from LIGHT_BASE_ATTRS via the
// LIGHT_COMMON_ATTRS_* macros, so no light drops them on read.

// ============================================================================
// SphereLight Properties
// ============================================================================
#define SPHERE_LIGHT_TYPED_ATTRS(X) \
  X("inputs:radius", radius)

// ============================================================================
// RectLight Properties
// ============================================================================
#define RECT_LIGHT_TYPED_ATTRS(X) \
  X("inputs:height", height) \
  X("inputs:width", width)

// RectLight has a special texture:file attr that uses UsdUVTexture type
// This needs special handling, not included in typed attrs

// ============================================================================
// DiskLight Properties
// ============================================================================
#define DISK_LIGHT_TYPED_ATTRS(X) \
  X("inputs:radius", radius)

// ============================================================================
// CylinderLight Properties
// ============================================================================
#define CYLINDER_LIGHT_TYPED_ATTRS(X) \
  X("inputs:length", length) \
  X("inputs:radius", radius)

// ============================================================================
// DistantLight Properties
// ============================================================================
#define DISTANT_LIGHT_TYPED_ATTRS(X) \
  X("inputs:angle", angle)

// ============================================================================
// GeometryLight Properties
// ============================================================================
// GeometryLight has no geometry-specific typed attrs beyond the shared base set.
#define GEOMETRY_LIGHT_TYPED_ATTRS(X)

// ============================================================================
// DomeLight Properties
// ============================================================================
#define DOME_LIGHT_TYPED_ATTRS(X) \
  X("guideRadius", guideRadius) \
  X("inputs:texture:file", file)

// ============================================================================
// Helper macros for property table expansion
// ============================================================================
// These macros require PRIM_CLASS_ and PRIM_PTR_ to be defined before use

// Expand typed attributes with PARSE_TYPED_ATTRIBUTE macro
#define EXPAND_TYPED_ATTR(name, member) \
  PARSE_TYPED_ATTRIBUTE(table, prop, name, PRIM_CLASS_, PRIM_PTR_->member)

// Expand typed attributes without continue
#define EXPAND_TYPED_ATTR_NOCONT(name, member) \
  PARSE_TYPED_ATTRIBUTE_NOCONTINUE(table, prop, name, PRIM_CLASS_, PRIM_PTR_->member)

// Expand single target relations
#define EXPAND_SINGLE_REL(name, member) \
  PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, name, PRIM_PTR_->member)

// Expand multi target relations
#define EXPAND_MULTI_REL(name, member) \
  PARSE_TARGET_PATHS_RELATION(table, prop, name, PRIM_PTR_->member)

// Expand uniform enum properties (handler must be a local variable)
#define EXPAND_UNIFORM_ENUM(name, enum_type, handler, member) \
  PARSE_UNIFORM_ENUM_PROPERTY(table, prop, name, enum_type, handler, PRIM_CLASS_, \
                              PRIM_PTR_->member, options.strict_allowedToken_check)

// Expand timesampled enum properties (handler must be a local variable)
#define EXPAND_TIMESAMPLED_ENUM(name, enum_type, handler, member) \
  PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, name, enum_type, handler, PRIM_CLASS_, \
                                  PRIM_PTR_->member, options.strict_allowedToken_check)

// Expand extent attribute
#define EXPAND_EXTENT(name, member) \
  PARSE_EXTENT_ATTRIBUTE(table, prop, name, PRIM_CLASS_, PRIM_PTR_->member)

// ============================================================================
// UsdPhysics Property Tables
// ============================================================================

#define PHYSICS_SCENE_TYPED_ATTRS(X) \
  X("physics:gravityDirection", gravityDirection) \
  X("physics:gravityMagnitude", gravityMagnitude)

// PhysicsJointBase attributes shared by all joint types
#define PHYSICS_JOINT_BASE_TYPED_ATTRS(X) \
  X("physics:localPos0", localPos0) \
  X("physics:localPos1", localPos1) \
  X("physics:localRot0", localRot0) \
  X("physics:localRot1", localRot1) \
  X("physics:jointEnabled", jointEnabled) \
  X("physics:collisionEnabled", collisionEnabled) \
  X("physics:breakForce", breakForce) \
  X("physics:breakTorque", breakTorque) \
  X("physics:excludeFromArticulation", excludeFromArticulation)

#define PHYSICS_JOINT_BASE_RELS(X) \
  X("physics:body0", body0) \
  X("physics:body1", body1)

#define PHYSICS_REVOLUTE_JOINT_TYPED_ATTRS(X) \
  X("physics:axis", axis) \
  X("physics:lowerLimit", lowerLimit) \
  X("physics:upperLimit", upperLimit)

#define PHYSICS_PRISMATIC_JOINT_TYPED_ATTRS(X) \
  X("physics:axis", axis) \
  X("physics:lowerLimit", lowerLimit) \
  X("physics:upperLimit", upperLimit)

#define PHYSICS_SPHERICAL_JOINT_TYPED_ATTRS(X) \
  X("physics:axis", axis) \
  X("physics:coneAngle0Limit", coneAngle0Limit) \
  X("physics:coneAngle1Limit", coneAngle1Limit)

#define PHYSICS_DISTANCE_JOINT_TYPED_ATTRS(X) \
  X("physics:minDistance", minDistance) \
  X("physics:maxDistance", maxDistance)

// ============================================================================
// mjcPhysics Property Tables
// ============================================================================

// MjcSceneAPI option attributes (on PhysicsScene)
#define MJC_SCENE_OPTION_TYPED_ATTRS(X) \
  X("mjc:option:timestep", mjcScene->timestep) \
  X("mjc:option:impratio", mjcScene->impratio) \
  X("mjc:option:wind", mjcScene->wind) \
  X("mjc:option:magnetic", mjcScene->magnetic) \
  X("mjc:option:density", mjcScene->density) \
  X("mjc:option:viscosity", mjcScene->viscosity) \
  X("mjc:option:o_margin", mjcScene->o_margin) \
  X("mjc:option:o_solref", mjcScene->o_solref) \
  X("mjc:option:o_solimp", mjcScene->o_solimp) \
  X("mjc:option:o_friction", mjcScene->o_friction) \
  X("mjc:option:integrator", mjcScene->integrator) \
  X("mjc:option:cone", mjcScene->cone) \
  X("mjc:option:jacobian", mjcScene->jacobian) \
  X("mjc:option:solver", mjcScene->solver) \
  X("mjc:option:iterations", mjcScene->iterations) \
  X("mjc:option:tolerance", mjcScene->tolerance) \
  X("mjc:option:ls_iterations", mjcScene->ls_iterations) \
  X("mjc:option:ls_tolerance", mjcScene->ls_tolerance) \
  X("mjc:option:noslip_iterations", mjcScene->noslip_iterations) \
  X("mjc:option:noslip_tolerance", mjcScene->noslip_tolerance) \
  X("mjc:option:ccd_iterations", mjcScene->ccd_iterations) \
  X("mjc:option:ccd_tolerance", mjcScene->ccd_tolerance) \
  X("mjc:option:sdf_iterations", mjcScene->sdf_iterations) \
  X("mjc:option:sdf_initpoints", mjcScene->sdf_initpoints) \
  X("mjc:option:actuatorgroupdisable", mjcScene->actuatorgroupdisable)

// MjcSceneAPI flag attributes
#define MJC_SCENE_FLAG_TYPED_ATTRS(X) \
  X("mjc:flag:constraint", mjcScene->flag_constraint) \
  X("mjc:flag:equality", mjcScene->flag_equality) \
  X("mjc:flag:frictionloss", mjcScene->flag_frictionloss) \
  X("mjc:flag:limit", mjcScene->flag_limit) \
  X("mjc:flag:contact", mjcScene->flag_contact) \
  X("mjc:flag:spring", mjcScene->flag_spring) \
  X("mjc:flag:damper", mjcScene->flag_damper) \
  X("mjc:flag:gravity", mjcScene->flag_gravity) \
  X("mjc:flag:clampctrl", mjcScene->flag_clampctrl) \
  X("mjc:flag:warmstart", mjcScene->flag_warmstart) \
  X("mjc:flag:filterparent", mjcScene->flag_filterparent) \
  X("mjc:flag:actuation", mjcScene->flag_actuation) \
  X("mjc:flag:refsafe", mjcScene->flag_refsafe) \
  X("mjc:flag:sensor", mjcScene->flag_sensor) \
  X("mjc:flag:midphase", mjcScene->flag_midphase) \
  X("mjc:flag:nativeccd", mjcScene->flag_nativeccd) \
  X("mjc:flag:eulerdamp", mjcScene->flag_eulerdamp) \
  X("mjc:flag:autoreset", mjcScene->flag_autoreset) \
  X("mjc:flag:island", mjcScene->flag_island) \
  X("mjc:flag:override", mjcScene->flag_override) \
  X("mjc:flag:energy", mjcScene->flag_energy) \
  X("mjc:flag:fwdinv", mjcScene->flag_fwdinv) \
  X("mjc:flag:invdiscrete", mjcScene->flag_invdiscrete) \
  X("mjc:flag:multiccd", mjcScene->flag_multiccd)

// MjcSceneAPI compiler attributes
#define MJC_SCENE_COMPILER_TYPED_ATTRS(X) \
  X("mjc:compiler:autoLimits", mjcScene->compiler_autoLimits) \
  X("mjc:compiler:boundMass", mjcScene->compiler_boundMass) \
  X("mjc:compiler:boundInertia", mjcScene->compiler_boundInertia) \
  X("mjc:compiler:setTotalMass", mjcScene->compiler_setTotalMass) \
  X("mjc:compiler:useThread", mjcScene->compiler_useThread) \
  X("mjc:compiler:balanceInertia", mjcScene->compiler_balanceInertia) \
  X("mjc:compiler:angle", mjcScene->compiler_angle) \
  X("mjc:compiler:fitAABB", mjcScene->compiler_fitAABB) \
  X("mjc:compiler:fuseStatic", mjcScene->compiler_fuseStatic) \
  X("mjc:compiler:inertiaFromGeom", mjcScene->compiler_inertiaFromGeom) \
  X("mjc:compiler:alignFree", mjcScene->compiler_alignFree) \
  X("mjc:compiler:inertiaGroupRange:min", mjcScene->compiler_inertiaGroupRangeMin) \
  X("mjc:compiler:inertiaGroupRange:max", mjcScene->compiler_inertiaGroupRangeMax) \
  X("mjc:compiler:saveInertial", mjcScene->compiler_saveInertial)

// MjcJointAPI attributes (on PhysicsJoint prims, accessed via mjcJoint->)
#define MJC_JOINT_TYPED_ATTRS(X) \
  X("mjc:group", mjcJoint->group) \
  X("mjc:stiffness", mjcJoint->stiffness) \
  X("mjc:damping", mjcJoint->damping) \
  X("mjc:armature", mjcJoint->armature) \
  X("mjc:frictionloss", mjcJoint->frictionloss) \
  X("mjc:springdamper", mjcJoint->springdamper) \
  X("mjc:springref", mjcJoint->springref) \
  X("mjc:ref", mjcJoint->ref) \
  X("mjc:margin", mjcJoint->margin) \
  X("mjc:solreflimit", mjcJoint->solreflimit) \
  X("mjc:solimplimit", mjcJoint->solimplimit) \
  X("mjc:solreffriction", mjcJoint->solreffriction) \
  X("mjc:solimpfriction", mjcJoint->solimpfriction) \
  X("mjc:actuatorfrcrange:min", mjcJoint->actuatorfrcrange_min) \
  X("mjc:actuatorfrcrange:max", mjcJoint->actuatorfrcrange_max) \
  X("mjc:actuatorfrclimited", mjcJoint->actuatorfrclimited) \
  X("mjc:actuatorgravcomp", mjcJoint->actuatorgravcomp)

// MjcActuator attributes
#define MJC_ACTUATOR_TYPED_ATTRS(X) \
  X("mjc:group", group) \
  X("mjc:ctrlLimited", ctrlLimited) \
  X("mjc:forceLimited", forceLimited) \
  X("mjc:actLimited", actLimited) \
  X("mjc:ctrlRange:min", ctrlRange_min) \
  X("mjc:ctrlRange:max", ctrlRange_max) \
  X("mjc:forceRange:min", forceRange_min) \
  X("mjc:forceRange:max", forceRange_max) \
  X("mjc:actRange:min", actRange_min) \
  X("mjc:actRange:max", actRange_max) \
  X("mjc:lengthRange:min", lengthRange_min) \
  X("mjc:lengthRange:max", lengthRange_max) \
  X("mjc:gear", gear) \
  X("mjc:crankLength", crankLength) \
  X("mjc:jointInParent", jointInParent) \
  X("mjc:actDim", actDim) \
  X("mjc:dynType", dynType) \
  X("mjc:gainType", gainType) \
  X("mjc:biasType", biasType) \
  X("mjc:dynPrm", dynPrm) \
  X("mjc:gainPrm", gainPrm) \
  X("mjc:biasPrm", biasPrm) \
  X("mjc:actEarly", actEarly) \
  X("mjc:inheritRange", inheritRange) \
  X("mjc:plugin", plugin) \
  X("mjc:instance", instance)

#define MJC_ACTUATOR_RELS(X) \
  X("mjc:target", target) \
  X("mjc:refSite", refSite) \
  X("mjc:sliderSite", sliderSite)

// MjcTendon attributes
#define MJC_TENDON_TYPED_ATTRS(X) \
  X("mjc:type", type) \
  X("mjc:path:indices", path_indices) \
  X("mjc:sideSites:indices", sideSites_indices) \
  X("mjc:path:segments", path_segments) \
  X("mjc:path:divisors", path_divisors) \
  X("mjc:path:coef", path_coef) \
  X("mjc:group", group) \
  X("mjc:limited", limited) \
  X("mjc:actuatorfrclimited", actuatorfrclimited) \
  X("mjc:range:min", range_min) \
  X("mjc:range:max", range_max) \
  X("mjc:actuatorfrcrange:min", actuatorfrcrange_min) \
  X("mjc:actuatorfrcrange:max", actuatorfrcrange_max) \
  X("mjc:solreflimit", solreflimit) \
  X("mjc:solimplimit", solimplimit) \
  X("mjc:solreffriction", solreffriction) \
  X("mjc:solimpfriction", solimpfriction) \
  X("mjc:margin", margin) \
  X("mjc:frictionloss", frictionloss) \
  X("mjc:springlength", springlength) \
  X("mjc:stiffness", stiffness) \
  X("mjc:damping", damping) \
  X("mjc:armature", armature) \
  X("mjc:width", width) \
  X("mjc:rgba", rgba)

#define MJC_TENDON_RELS(X) \
  X("mjc:path", path) \
  X("mjc:sideSites", sideSites)

// MjcKeyframe attributes
#define MJC_KEYFRAME_TYPED_ATTRS(X) \
  X("mjc:qpos", qpos) \
  X("mjc:qvel", qvel) \
  X("mjc:act", act) \
  X("mjc:ctrl", ctrl) \
  X("mjc:mpos", mpos) \
  X("mjc:mquat", mquat)

// MjcSensor attributes
#define MJC_SENSOR_TYPED_ATTRS(X) \
  X("mjc:type", type) \
  X("mjc:objtype", objType) \
  X("mjc:objname", objName) \
  X("mjc:reftype", refType) \
  X("mjc:refname", refName) \
  X("mjc:group", group) \
  X("mjc:cutoff", cutoff) \
  X("mjc:noise", noise) \
  X("mjc:user", user)

// ============================================================================
// Newton Physics Property Tables
// ============================================================================

#define NEWTON_SCENE_TYPED_ATTRS(X) \
  X("newton:maxSolverIterations", newtonScene->maxSolverIterations) \
  X("newton:timeStepsPerSecond", newtonScene->timeStepsPerSecond) \
  X("newton:gravityEnabled", newtonScene->gravityEnabled)

#define NEWTON_XPBD_SCENE_TYPED_ATTRS(X) \
  X("newton:xpbd:softBodyRelaxation", newtonXpbdScene->softBodyRelaxation) \
  X("newton:xpbd:softContactRelaxation", newtonXpbdScene->softContactRelaxation) \
  X("newton:xpbd:jointLinearRelaxation", newtonXpbdScene->jointLinearRelaxation) \
  X("newton:xpbd:jointAngularRelaxation", newtonXpbdScene->jointAngularRelaxation) \
  X("newton:xpbd:jointLinearCompliance", newtonXpbdScene->jointLinearCompliance) \
  X("newton:xpbd:jointAngularCompliance", newtonXpbdScene->jointAngularCompliance) \
  X("newton:xpbd:rigidContactRelaxation", newtonXpbdScene->rigidContactRelaxation) \
  X("newton:xpbd:rigidContactConWeighting", newtonXpbdScene->rigidContactConWeighting) \
  X("newton:xpbd:angularDamping", newtonXpbdScene->angularDamping) \
  X("newton:xpbd:restitutionEnabled", newtonXpbdScene->restitutionEnabled)

#define NEWTON_KAMINO_SCENE_TYPED_ATTRS(X) \
  X("newton:kamino:padmm:primalTolerance", newtonKaminoScene->padmmPrimalTolerance) \
  X("newton:kamino:padmm:dualTolerance", newtonKaminoScene->padmmDualTolerance) \
  X("newton:kamino:padmm:complementarityTolerance", newtonKaminoScene->padmmComplementarityTolerance) \
  X("newton:kamino:padmm:warmstarting", newtonKaminoScene->padmmWarmstarting) \
  X("newton:kamino:padmm:useAcceleration", newtonKaminoScene->padmmUseAcceleration) \
  X("newton:kamino:constraints:usePreconditioning", newtonKaminoScene->constraintsUsePreconditioning) \
  X("newton:kamino:constraints:alpha", newtonKaminoScene->constraintsAlpha) \
  X("newton:kamino:constraints:beta", newtonKaminoScene->constraintsBeta) \
  X("newton:kamino:constraints:gamma", newtonKaminoScene->constraintsGamma) \
  X("newton:kamino:jointCorrection", newtonKaminoScene->jointCorrection)

#define NEWTON_MIMIC_TYPED_ATTRS(X) \
  X("newton:mimicEnabled", newtonMimic->mimicEnabled) \
  X("newton:mimicCoef0", newtonMimic->mimicCoef0) \
  X("newton:mimicCoef1", newtonMimic->mimicCoef1)

#define NEWTON_MIMIC_RELS(X) \
  X("newton:mimicJoint", newtonMimic->mimicJoint)

#define NEWTON_ACTUATOR_TYPED_ATTRS(X) \
  X("newton:delaySteps", delaySteps) \
  X("newton:constEffort", constEffort) \
  X("newton:kp", kp) \
  X("newton:kd", kd) \
  X("newton:ki", ki) \
  X("newton:integralMax", integralMax) \
  X("newton:modelPath", modelPath) \
  X("newton:maxEffort", maxEffort) \
  X("newton:maxMotorEffort", maxMotorEffort) \
  X("newton:saturationEffort", saturationEffort) \
  X("newton:velocityLimit", velocityLimit) \
  X("newton:lookupPositions", lookupPositions) \
  X("newton:lookupEfforts", lookupEfforts)

#define NEWTON_ACTUATOR_RELS(X) \
  X("newton:targets", targets)

// PhysicsCollisionGroup
#define PHYSICS_COLLISION_GROUP_TYPED_ATTRS(X) \
  X("physics:mergeGroup", mergeGroup) \
  X("physics:invertFilteredGroups", invertFilteredGroups)

#define PHYSICS_COLLISION_GROUP_RELS(X) \
  X("physics:filteredGroups", filteredGroups)

// ============================================================================
// AR/Interactive Property Tables (Apple Preliminary_* schemas)
// ============================================================================

#define PRELIMINARY_GRAVITATIONAL_FORCE_TYPED_ATTRS(X) \
  X("physics:gravitationalForce:acceleration", acceleration)

#define PRELIMINARY_INFINITE_COLLIDER_PLANE_TYPED_ATTRS(X) \
  X("position", position) \
  X("normal", normal)

#define PRELIMINARY_REFERENCE_IMAGE_TYPED_ATTRS(X) \
  X("image", image) \
  X("physicalWidth", physicalWidth)

#define PRELIMINARY_BEHAVIOR_TYPED_ATTRS(X) \
  X("exclusive", exclusive)

#define PRELIMINARY_BEHAVIOR_RELS(X) \
  X("triggers", triggers) \
  X("actions", actions)

#define PRELIMINARY_TRIGGER_TYPED_ATTRS(X) \
  X("info:id", info_id)

#define PRELIMINARY_ACTION_TYPED_ATTRS(X) \
  X("info:id", info_id) \
  X("multiplePerformOperation", multiplePerformOperation)

#define PRELIMINARY_TEXT_TYPED_ATTRS(X) \
  X("content", content) \
  X("font", font) \
  X("pointSize", pointSize) \
  X("width", width) \
  X("height", height) \
  X("depth", depth) \
  X("wrapMode", wrapMode) \
  X("horizontalAlignment", horizontalAlignment) \
  X("verticalAlignment", verticalAlignment)

// ============================================================================
// usdMedia Property Tables
// ============================================================================

#define SPATIAL_AUDIO_TYPED_ATTRS(X) \
  X("filePath", filePath) \
  X("auralMode", auralMode) \
  X("playbackMode", playbackMode) \
  X("startTime", startTime) \
  X("endTime", endTime) \
  X("mediaOffset", mediaOffset) \
  X("gain", gain)
