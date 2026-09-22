// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Geometry prim to_string (extracted from pprinter.cc).
//
#include "pprinter.hh"       // for all declarations
#include "pprint-detail.hh"  // for templates

#include "common-macros.inc"

namespace lightusd {

std::string to_string(lightusd::GeomMesh::InterpolateBoundary v) {
  std::string s;

  switch (v) {
    case lightusd::GeomMesh::InterpolateBoundary::InterpolateBoundaryNone: {
      s = "none";
      break;
    }
    case lightusd::GeomMesh::InterpolateBoundary::EdgeAndCorner: {
      s = "edgeAndCorner";
      break;
    }
    case lightusd::GeomMesh::InterpolateBoundary::EdgeOnly: {
      s = "edgeOnly";
      break;
    }
  }

  return s;
}

std::string to_string(lightusd::GeomMesh::SubdivisionScheme v) {
  std::string s;

  switch (v) {
    case lightusd::GeomMesh::SubdivisionScheme::CatmullClark: {
      s = "catmullClark";
      break;
    }
    case lightusd::GeomMesh::SubdivisionScheme::Loop: {
      s = "loop";
      break;
    }
    case lightusd::GeomMesh::SubdivisionScheme::Bilinear: {
      s = "bilinear";
      break;
    }
    case lightusd::GeomMesh::SubdivisionScheme::SubdivisionSchemeNone: {
      s = "none";
      break;
    }
  }

  return s;
}

std::string to_string(lightusd::GeomMesh::TriangleSubdivisionRule v) {
  switch (v) {
    case lightusd::GeomMesh::TriangleSubdivisionRule::CatmullClark:
      return "catmullClark";
    case lightusd::GeomMesh::TriangleSubdivisionRule::Smooth:
      return "smooth";
  }
  return "catmullClark";
}

std::string to_string(lightusd::GeomMesh::FaceVaryingLinearInterpolation v) {
  std::string s;

  switch (v) {
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::CornersPlus1: {
      s = "cornersPlus1";
      break;
    }
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::CornersPlus2: {
      s = "cornersPlus2";
      break;
    }
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::CornersOnly: {
      s = "cornersOnly";
      break;
    }
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::Boundaries: {
      s = "boundaries";
      break;
    }
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::
        FaceVaryingLinearInterpolationNone: {
      s = "none";
      break;
    }
    case lightusd::GeomMesh::FaceVaryingLinearInterpolation::All: {
      s = "all";
      break;
    }
  }

  return s;
}

std::string to_string(lightusd::GeomSubset::ElementType v) {
  std::string s;

  switch (v) {
    case lightusd::GeomSubset::ElementType::Face: {
      s = "face";
      break;
    }
    case lightusd::GeomSubset::ElementType::Point: {
      s = "point";
      break;
    }
    case lightusd::GeomSubset::ElementType::Edge: {
      s = "edge";
      break;
    }
    case lightusd::GeomSubset::ElementType::Tetrahedron: {
      s = "tetrahedron";
      break;
    }
  }

  return s;
}

std::string to_string(lightusd::GeomSubset::FamilyType v) {
  std::string s;

  switch (v) {
    case lightusd::GeomSubset::FamilyType::Partition: {
      s = "partition";
      break;
    }
    case lightusd::GeomSubset::FamilyType::NonOverlapping: {
      s = "nonOverlapping";
      break;
    }
    case lightusd::GeomSubset::FamilyType::Unrestricted: {
      s = "unrestricted";
      break;
    }
  }

  return s;
}

std::string to_string(const GeomBasisCurves::Type &ty) {
  std::string s;

  switch (ty) {
    case GeomBasisCurves::Type::Cubic: {
      s = "cubic";
      break;
    }
    case GeomBasisCurves::Type::Linear: {
      s = "linear";
      break;
    }
  }

  return s;
}

std::string to_string(const GeomBasisCurves::Basis &ty) {
  std::string s;

  switch (ty) {
    case GeomBasisCurves::Basis::Bezier: {
      s = "bezier";
      break;
    }
    case GeomBasisCurves::Basis::Bspline: {
      s = "bspline";
      break;
    }
    case GeomBasisCurves::Basis::CatmullRom: {
      s = "catmullRom";
      break;
    }
  }

  return s;
}

std::string to_string(const GeomBasisCurves::Wrap &ty) {
  std::string s;

  switch (ty) {
    case GeomBasisCurves::Wrap::Nonperiodic: {
      s = "nonperiodic";
      break;
    }
    case GeomBasisCurves::Wrap::Periodic: {
      s = "periodic";
      break;
    }
    case GeomBasisCurves::Wrap::Pinned: {
      s = "pinned";
      break;
    }
  }

  return s;
}

std::string to_string(const GeomCamera::Projection &proj) {
  if (proj == GeomCamera::Projection::Orthographic) {
    return "orthographic";
  } else {
    return "perspective";
  }
}

std::string to_string(const GeomCamera::StereoRole &role) {
  if (role == GeomCamera::StereoRole::Mono) {
    return "mono";
  } else if (role == GeomCamera::StereoRole::Right) {
    return "right";
  } else {
    return "left";
  }
}

std::string to_string(const Model &model, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(model.spec);
  if (model.prim_type_name.size()) {
    ss << " " << model.prim_type_name;
  }
  ss << " \"" << model.name << "\"\n";

  if (model.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(model.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  std::set<std::string> tokset;
  ss << print_material_binding(&model, indent + 1);
  ss << print_collection(&model, indent + 1);
  ss << print_props(model.props, tokset, model.propertyNames(), indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const Scope &scope, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(scope.spec) << " Scope \""
     << scope.name << "\"\n";
  if (scope.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(scope.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // Scope is imageable: `visibility` and `purpose` are parsed into typed fields,
  // so they are NOT in `props` and have to be printed from there -- otherwise
  // they are silently dropped on write.
  ss << print_typed_token_attr(scope.purpose, "purpose", indent + 1);
  ss << print_typed_token_attr(scope.visibility, "visibility", indent + 1);
  ss << print_material_binding(&scope, indent + 1);
  ss << print_collection(&scope, indent + 1);

  std::set<std::string> tokset;
  ss << print_props(scope.props, tokset, scope.propertyNames(), indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

// UsdRender / UsdProc placeholder prim types: emit `<spec> <TypeName> "name"`
// with generic props (no typed schema fields yet).
#define DEFINE_PLACEHOLDER_PRIM_TO_STRING(__cls, __tyname)                 \
  std::string to_string(const __cls &p, const uint32_t indent,            \
                        bool closing_brace) {                             \
    std::stringstream ss;                                                 \
    ss << pprint::Indent(indent) << to_string(p.spec) << " " __tyname     \
       << " \"" << p.name << "\"\n";                                      \
    if (p.meta.authored()) {                                              \
      ss << pprint::Indent(indent) << "(\n";                              \
      ss << print_prim_metas(p.meta, indent + 1);                         \
      ss << pprint::Indent(indent) << ")\n";                              \
    }                                                                     \
    ss << pprint::Indent(indent) << "{\n";                                \
    std::set<std::string> tokset;                                         \
    ss << print_props(p.props, tokset, p.propertyNames(), indent + 1);    \
    if (closing_brace) {                                                  \
      ss << pprint::Indent(indent) << "}\n";                              \
    }                                                                     \
    return ss.str();                                                     \
  }

DEFINE_PLACEHOLDER_PRIM_TO_STRING(RenderSettings, "RenderSettings")
DEFINE_PLACEHOLDER_PRIM_TO_STRING(RenderProduct, "RenderProduct")
DEFINE_PLACEHOLDER_PRIM_TO_STRING(RenderVar, "RenderVar")
DEFINE_PLACEHOLDER_PRIM_TO_STRING(GenerativeProcedural, "GenerativeProcedural")
#undef DEFINE_PLACEHOLDER_PRIM_TO_STRING

std::string to_string(const GPrim &gprim, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(gprim.spec) << " GPrim \""
     << gprim.name << "\"\n";
  ss << pprint::Indent(indent) << "(\n";
  // args
  ss << pprint::Indent(indent) << ")\n";
  ss << pprint::Indent(indent) << "{\n";

  ss << print_gprim_predefined(gprim, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const Xform &xform, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(xform.spec) << " Xform \""
     << xform.name << "\"\n";
  if (xform.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(xform.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_gprim_predefined(xform, indent + 1);

  ss << print_props(xform.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCamera &camera, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(camera.spec) << " Camera \""
     << camera.name << "\"\n";
  if (camera.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(camera.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_attr(camera.clippingRange, "clippingRange", indent + 1);
  ss << print_typed_attr(camera.clippingPlanes, "clippingPlanes", indent + 1);
  ss << print_typed_attr(camera.focalLength, "focalLength", indent + 1);
  ss << print_typed_attr(camera.horizontalAperture, "horizontalAperture",
                         indent + 1);
  ss << print_typed_attr(camera.horizontalApertureOffset,
                         "horizontalApertureOffset", indent + 1);
  ss << print_typed_attr(camera.verticalAperture, "verticalAperture",
                         indent + 1);
  ss << print_typed_attr(camera.verticalApertureOffset,
                         "verticalApertureOffset", indent + 1);

  ss << print_typed_attr(camera.exposure, "exposure", indent + 1);
  ss << print_typed_attr(camera.focusDistance, "focusDistance", indent + 1);
  ss << print_typed_attr(camera.fStop, "fStop", indent + 1);

  ss << print_typed_token_attr(camera.projection, "projection", indent + 1);
  ss << print_typed_token_attr(camera.stereoRole, "stereoRole", indent + 1);

  ss << print_typed_attr(camera.shutterOpen, "shutter:open", indent + 1);
  ss << print_typed_attr(camera.shutterClose, "shutter:close", indent + 1);

  ss << print_gprim_predefined(camera, indent + 1);

  ss << print_props(camera.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}


std::string to_string(const GeomSphere &sphere, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(sphere.spec) << " Sphere \""
     << sphere.name << "\"\n";
  if (sphere.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(sphere.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_attr(sphere.radius, "radius", indent + 1);

  ss << print_gprim_predefined(sphere, indent + 1);

  ss << print_props(sphere.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

// --- UsdVol ---

// Shared printer for FieldAsset-derived field-asset attributes.
static void print_field_asset_attrs(std::stringstream &ss,
                                    const FieldAsset &field,
                                    const uint32_t indent) {
  ss << print_typed_attr(field.filePath, "filePath", indent);
  ss << print_typed_attr(field.fieldName, "fieldName", indent);
  ss << print_typed_attr(field.fieldDataType, "fieldDataType", indent);
  ss << print_typed_attr(field.fieldIndex, "fieldIndex", indent);
}

std::string to_string(const FieldAsset &field, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;
  ss << pprint::Indent(indent) << to_string(field.spec) << " FieldAsset \""
     << field.name << "\"\n";
  if (field.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(field.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";
  print_field_asset_attrs(ss, field, indent + 1);
  ss << print_gprim_predefined(field, indent + 1);
  ss << print_props(field.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return ss.str();
}

std::string to_string(const OpenVDBAsset &asset, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;
  ss << pprint::Indent(indent) << to_string(asset.spec) << " OpenVDBAsset \""
     << asset.name << "\"\n";
  if (asset.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(asset.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";
  print_field_asset_attrs(ss, asset, indent + 1);
  ss << print_typed_attr(asset.fieldClass, "fieldClass", indent + 1);
  ss << print_gprim_predefined(asset, indent + 1);
  ss << print_props(asset.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return ss.str();
}

std::string to_string(const Field3DAsset &asset, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;
  ss << pprint::Indent(indent) << to_string(asset.spec) << " Field3DAsset \""
     << asset.name << "\"\n";
  if (asset.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(asset.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";
  print_field_asset_attrs(ss, asset, indent + 1);
  ss << print_typed_attr(asset.fieldPurpose, "fieldPurpose", indent + 1);
  ss << print_gprim_predefined(asset, indent + 1);
  ss << print_props(asset.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return ss.str();
}

std::string to_string(const Volume &volume, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;
  ss << pprint::Indent(indent) << to_string(volume.spec) << " Volume \""
     << volume.name << "\"\n";
  if (volume.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(volume.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // `field:<name>` relationships -> field-asset prim paths.
  for (const auto &item : volume.fieldRelationships) {
    ss << print_relationship(item.second, item.second.get_listedit_qual(),
                             /* custom */ false, "field:" + item.first,
                             indent + 1);
  }

  ss << print_gprim_predefined(volume, indent + 1);
  ss << print_props(volume.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return ss.str();
}

std::string to_string(const GeomMesh &mesh, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(mesh.spec) << " Mesh \""
     << mesh.name << "\"\n";
  if (mesh.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(mesh.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_attr(mesh.points, "points", indent + 1);
  ss << print_typed_attr(mesh.normals, "normals", indent + 1);
  ss << print_typed_attr(mesh.velocities, "velocities", indent + 1);
  ss << print_typed_attr(mesh.faceVertexIndices, "faceVertexIndices",
                         indent + 1);
  ss << print_typed_attr(mesh.faceVertexCounts, "faceVertexCounts", indent + 1);

  if (mesh.skeleton) {
    ss << print_relationship(mesh.skeleton.value(),
                             mesh.skeleton.value().get_listedit_qual(),
                             /* custom */ false, "skel:skeleton", indent + 1);
  }

  ss << print_typed_attr(mesh.blendShapes, "skel:blendShapes", indent + 1);
  if (mesh.blendShapeTargets) {
    ss << print_relationship(mesh.blendShapeTargets.value(),
                             mesh.blendShapeTargets.value().get_listedit_qual(),
                             /* custom */ false, "skel:blendShapeTargets",
                             indent + 1);
  }

  for (const auto &item : mesh.subsetFamilyTypeMap) {
    std::string attr_name = "subsetFamily:" + item.first.str() + ":familyType";
    // TODO: Support connection attr?
    ss << pprint::Indent(indent+1) << "uniform token " << attr_name << " = " << quote(to_string(item.second)) << "\n";
  }

  // subdiv
  ss << print_typed_attr(mesh.cornerIndices, "cornerIndices", indent + 1);
  ss << print_typed_attr(mesh.cornerSharpnesses, "cornerSharpnesses",
                         indent + 1);
  ss << print_typed_attr(mesh.creaseIndices, "creaseIndices", indent + 1);
  ss << print_typed_attr(mesh.creaseLengths, "creaseLengths", indent + 1);
  ss << print_typed_attr(mesh.creaseSharpnesses, "creaseSharpnesses",
                         indent + 1);
  ss << print_typed_attr(mesh.holeIndices, "holeIndices", indent + 1);

  ss << print_typed_token_attr(mesh.subdivisionScheme, "subdivisionScheme",
                               indent + 1);
  ss << print_typed_token_attr(mesh.interpolateBoundary, "interpolateBoundary",
                               indent + 1);
  ss << print_typed_token_attr(mesh.faceVaryingLinearInterpolation,
                               "faceVaryingLinearInterpolation", indent + 1);
  ss << print_typed_token_attr(mesh.triangleSubdivisionRule,
                               "triangleSubdivisionRule", indent + 1);

  ss << print_gprim_predefined(mesh, indent + 1);


  ss << print_props(mesh.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomSubset &subset, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(subset.spec) << " GeomSubset \""
     << subset.name << "\"\n";
  ss << pprint::Indent(indent) << "(\n";
  ss << print_prim_metas(subset.meta, indent + 1);
  ss << pprint::Indent(indent) << ")\n";
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_token_attr(subset.elementType, "elementType", indent + 1);
  ss << print_typed_attr(subset.familyName, "familyName", indent + 1);
  ss << print_typed_attr(subset.indices, "indices", indent + 1);

  ss << print_material_binding(&subset, indent + 1);
  ss << print_collection(&subset, indent + 1);

  ss << print_props(subset.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomPoints &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(geom.spec) << " Points \""
     << geom.name << "\"\n";
  if (geom.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(geom.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);
  ss << print_typed_attr(geom.widths, "widths", indent + 1);
  ss << print_typed_attr(geom.ids, "ids", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);

  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomBasisCurves &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(geom.spec) << " BasisCurves \""
     << geom.name << "\"\n";
  if (geom.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(geom.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_token_attr(geom.type, "type", indent + 1);
  ss << print_typed_token_attr(geom.basis, "basis", indent + 1);
  ss << print_typed_token_attr(geom.wrap, "wrap", indent + 1);

  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);
  ss << print_typed_attr(geom.widths, "widths", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(geom.curveVertexCounts, "curveVertexCounts",
                         indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);

  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomNurbsCurves &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(geom.spec) << " NurbsCurves \""
     << geom.name << "\"\n";
  if (geom.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(geom.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);
  ss << print_typed_attr(geom.widths, "widths", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(geom.curveVertexCounts, "curveVertexCounts",
                         indent + 1);

  //
  ss << print_typed_attr(geom.order, "order", indent + 1);
  ss << print_typed_attr(geom.knots, "knots", indent + 1);
  ss << print_typed_attr(geom.ranges, "ranges", indent + 1);
  ss << print_typed_attr(geom.pointWeights, "pointWeights", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);

  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCube &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(geom.spec) << " Cube \""
     << geom.name << "\"\n";
  if (geom.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(geom.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  ss << print_typed_attr(geom.size, "size", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);

  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCone &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Cone", indent);

  // members
  ss << print_typed_attr(geom.radius, "radius", indent + 1);
  ss << print_typed_attr(geom.height, "height", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCylinder &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Cylinder", indent);

  // members
  ss << print_typed_attr(geom.radius, "radius", indent + 1);
  ss << print_typed_attr(geom.height, "height", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCapsule &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Capsule", indent);

  // members
  ss << print_typed_attr(geom.radius, "radius", indent + 1);
  ss << print_typed_attr(geom.height, "height", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomPointInstancer &instancer, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(instancer.spec)
     << " PointInstancer \"" << instancer.name << "\"\n";
  if (instancer.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(instancer.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  if (instancer.prototypes) {
    ss << print_relationship(instancer.prototypes.value(),
                             instancer.prototypes.value().get_listedit_qual(),
                             /* custom */ false, "prototypes", indent + 1);
  }
  ss << print_typed_attr(instancer.protoIndices, "protoIndices", indent + 1);
  ss << print_typed_attr(instancer.ids, "ids", indent + 1);
  ss << print_typed_attr(instancer.invisibleIds, "invisibleIds", indent + 1);
  ss << print_typed_attr(instancer.inactiveIds, "inactiveIds", indent + 1);
  ss << print_typed_attr(instancer.positions, "positions", indent + 1);
  ss << print_typed_attr(instancer.orientations, "orientations", indent + 1);
  ss << print_typed_attr(instancer.scales, "scales", indent + 1);
  ss << print_typed_attr(instancer.velocities, "velocities", indent + 1);
  ss << print_typed_attr(instancer.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(instancer.angularVelocities, "angularVelocities",
                         indent + 1);

  ss << print_gprim_predefined(instancer, indent + 1);

  ss << print_props(instancer.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomPlane &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Plane", indent);

  // members
  ss << print_typed_attr(geom.width, "width", indent + 1);
  ss << print_typed_attr(geom.length, "length", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCylinder_1 &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Cylinder_1", indent);

  // members
  ss << print_typed_attr(geom.height, "height", indent + 1);
  ss << print_typed_attr(geom.radiusTop, "radiusTop", indent + 1);
  ss << print_typed_attr(geom.radiusBottom, "radiusBottom", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomCapsule_1 &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "Capsule_1", indent);

  // members
  ss << print_typed_attr(geom.height, "height", indent + 1);
  ss << print_typed_attr(geom.radiusTop, "radiusTop", indent + 1);
  ss << print_typed_attr(geom.radiusBottom, "radiusBottom", indent + 1);
  ss << print_axis_attr(geom, indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomTetMesh &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "TetMesh", indent);

  // members
  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);
  ss << print_typed_attr(geom.tetVertexIndices, "tetVertexIndices", indent + 1);
  ss << print_typed_attr(geom.surfaceFaceVertexIndices, "surfaceFaceVertexIndices", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomNurbsPatch &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "NurbsPatch", indent);

  // PointBased attributes
  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);

  // NurbsPatch-specific
  ss << print_typed_attr(geom.uVertexCount, "uVertexCount", indent + 1);
  ss << print_typed_attr(geom.vVertexCount, "vVertexCount", indent + 1);
  ss << print_typed_attr(geom.uOrder, "uOrder", indent + 1);
  ss << print_typed_attr(geom.vOrder, "vOrder", indent + 1);
  ss << print_typed_attr(geom.uKnots, "uKnots", indent + 1);
  ss << print_typed_attr(geom.vKnots, "vKnots", indent + 1);
  ss << print_typed_token_attr(geom.uForm, "uForm", indent + 1);
  ss << print_typed_token_attr(geom.vForm, "vForm", indent + 1);
  ss << print_typed_attr(geom.uRange, "uRange", indent + 1);
  ss << print_typed_attr(geom.vRange, "vRange", indent + 1);
  ss << print_typed_attr(geom.pointWeights, "pointWeights", indent + 1);

  // Trim curves
  ss << print_typed_attr(geom.trimCurve_counts, "trimCurve:counts", indent + 1);
  ss << print_typed_attr(geom.trimCurve_orders, "trimCurve:orders", indent + 1);
  ss << print_typed_attr(geom.trimCurve_vertexCounts, "trimCurve:vertexCounts", indent + 1);
  ss << print_typed_attr(geom.trimCurve_knots, "trimCurve:knots", indent + 1);
  ss << print_typed_attr(geom.trimCurve_ranges, "trimCurve:ranges", indent + 1);
  ss << print_typed_attr(geom.trimCurve_points, "trimCurve:points", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const GeomHermiteCurves &geom, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << print_prim_header(geom, "HermiteCurves", indent);

  // members
  ss << print_typed_attr(geom.points, "points", indent + 1);
  ss << print_typed_attr(geom.velocities, "velocities", indent + 1);
  ss << print_typed_attr(geom.accelerations, "accelerations", indent + 1);
  ss << print_typed_attr(geom.normals, "normals", indent + 1);
  ss << print_typed_attr(geom.curveVertexCounts, "curveVertexCounts", indent + 1);
  ss << print_typed_attr(geom.widths, "widths", indent + 1);
  ss << print_typed_attr(geom.tangents, "tangents", indent + 1);

  ss << print_gprim_predefined(geom, indent + 1);
  ss << print_props(geom.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

}  // namespace lightusd
