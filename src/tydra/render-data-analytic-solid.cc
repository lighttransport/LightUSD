// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Analytic USD geometric primitives converted through the mesh pipeline.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/shape-to-mesh.hh"
#include "usdGeom.hh"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

// Copy authored displayColor / displayOpacity primvars from an analytic Gprim
// (Cube/Sphere/Cone/Cylinder/Capsule) onto the tessellated temp GeomMesh so
// ConvertMesh applies them. Without this, analytic primitives always render
// with the default material even when the prim authors primvars:displayColor.
static void CopyDisplayPrimvarsToTempMesh(const GPrim &src, GeomMesh *dst) {
  if (!dst) return;
  GeomPrimvar pv;
  if (src.get_primvar("displayColor", &pv)) dst->set_primvar(pv);
  GeomPrimvar po;
  if (src.get_primvar("displayOpacity", &po)) dst->set_primvar(po);
}

//
// Convert GeomCylinder to RenderMesh
//
bool RenderSceneConverter::ConvertCylinder(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomCylinder &cylinder, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  double radius = 1.0;
  cylinder.radius.get_value().get_scalar(&radius);
  double height = 2.0;
  cylinder.height.get_value().get_scalar(&height);

  int radialSegs = 24;
  int heightSegs = 1;

  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  GenerateCylinderMesh(radius, height, radialSegs, heightSegs,
                       points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);

  GeomMesh temp_mesh;
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);
  temp_mesh.orientation = cylinder.orientation;
  temp_mesh.doubleSided = cylinder.doubleSided;

  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::Vertex);
  }
  {
    GeomPrimvar primvar;
    primvar.set_name("st");
    primvar.set_interpolation(Interpolation::Vertex);
    std::vector<value::texcoord2f> uv_data;
    for (const auto &uv : uvs_f2) {
      uv_data.push_back(value::texcoord2f{uv[0], uv[1]});
    }
    primvar.set_value(uv_data);
    temp_mesh.set_primvar(primvar);
  }

  CopyDisplayPrimvarsToTempMesh(cylinder, &temp_mesh);
  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}

//
// Convert GeomCone to RenderMesh
//
bool RenderSceneConverter::ConvertCone(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomCone &cone, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  double radius = 1.0;
  cone.radius.get_value().get_scalar(&radius);
  double height = 2.0;
  cone.height.get_value().get_scalar(&height);

  int radialSegs = 24;

  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  GenerateConeMesh(radius, height, radialSegs,
                   points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);

  GeomMesh temp_mesh;
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);
  temp_mesh.orientation = cone.orientation;
  temp_mesh.doubleSided = cone.doubleSided;

  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::Vertex);
  }
  {
    GeomPrimvar primvar;
    primvar.set_name("st");
    primvar.set_interpolation(Interpolation::Vertex);
    std::vector<value::texcoord2f> uv_data;
    for (const auto &uv : uvs_f2) {
      uv_data.push_back(value::texcoord2f{uv[0], uv[1]});
    }
    primvar.set_value(uv_data);
    temp_mesh.set_primvar(primvar);
  }

  CopyDisplayPrimvarsToTempMesh(cone, &temp_mesh);
  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}

//
// Convert GeomCapsule to RenderMesh
//
bool RenderSceneConverter::ConvertCapsule(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomCapsule &capsule, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  double radius = 0.5;
  capsule.radius.get_value().get_scalar(&radius);
  double height = 2.0;
  capsule.height.get_value().get_scalar(&height);

  int radialSegs = 24;
  int heightSegs = 1;

  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  GenerateCapsuleMesh(radius, height, radialSegs, heightSegs,
                      points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);

  GeomMesh temp_mesh;
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);
  temp_mesh.orientation = capsule.orientation;
  temp_mesh.doubleSided = capsule.doubleSided;

  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::Vertex);
  }
  {
    GeomPrimvar primvar;
    primvar.set_name("st");
    primvar.set_interpolation(Interpolation::Vertex);
    std::vector<value::texcoord2f> uv_data;
    for (const auto &uv : uvs_f2) {
      uv_data.push_back(value::texcoord2f{uv[0], uv[1]});
    }
    primvar.set_value(uv_data);
    temp_mesh.set_primvar(primvar);
  }

  CopyDisplayPrimvarsToTempMesh(capsule, &temp_mesh);
  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}

//
// Convert GeomPlane to RenderMesh
//
bool RenderSceneConverter::ConvertPlane(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomPlane &plane, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  double width = 1.0;
  plane.width.get_value().get_scalar(&width);
  double length = 1.0;
  plane.length.get_value().get_scalar(&length);

  int widthSegs = 1;
  int lengthSegs = 1;

  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  GeneratePlaneMesh(width, length, widthSegs, lengthSegs,
                    points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);

  GeomMesh temp_mesh;
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);
  temp_mesh.orientation = plane.orientation;
  temp_mesh.doubleSided = plane.doubleSided;

  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::FaceVarying);
  }
  {
    GeomPrimvar primvar;
    primvar.set_name("st");
    primvar.set_interpolation(Interpolation::FaceVarying);
    std::vector<value::texcoord2f> uv_data;
    for (const auto &uv : uvs_f2) {
      uv_data.push_back(value::texcoord2f{uv[0], uv[1]});
    }
    primvar.set_value(uv_data);
    temp_mesh.set_primvar(primvar);
  }

  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}


}  // namespace tydra
}  // namespace lightusd
