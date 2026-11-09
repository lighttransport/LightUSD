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
// Convert GeomCube to RenderMesh by generating tessellated geometry
//
bool RenderSceneConverter::ConvertCube(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomCube &cube, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  // Extract cube size
  double size;
  if (!cube.size.get_value().get_scalar(&size)) {
    size = 2.0;  // Use default value if not available
  }

  // Generate cube mesh geometry
  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  GenerateCubeMesh(size, points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);

  // Create temporary GeomMesh with generated data
  GeomMesh temp_mesh;

  // Convert points from float3 to point3f
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);

  // Copy properties from cube
  temp_mesh.orientation = cube.orientation;
  temp_mesh.doubleSided = cube.doubleSided;

  // Set normals as face-varying primvar
  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::FaceVarying);
  }

  // Set UVs as st primvar (face-varying)
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

  // Forward to ConvertMesh
  CopyDisplayPrimvarsToTempMesh(cube, &temp_mesh);
  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}

//
// Convert GeomSphere to RenderMesh by generating tessellated geometry
//
bool RenderSceneConverter::ConvertSphere(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomSphere &sphere, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>> &blendshapes,
    RenderMesh *dstMesh) {

  // Extract sphere radius
  double radius;
  if (!sphere.radius.get_value().get_scalar(&radius)) {
    radius = 1.0;  // UsdGeomSphere schema fallback
  }

  // Generate sphere mesh geometry
  // Default to icosphere with 2 subdivisions (4 divisions as per user request seems to mean subdivisions)
  std::vector<value::float3> points_f3;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<value::float3> normals_f3;
  std::vector<value::float2> uvs_f2;

  int subdivisions = env.mesh_config.sphere_subdivisions;
  if (env.mesh_config.sphere_tessellation == SphereTessellation::UV) {
    GenerateUVSphereMesh(radius, subdivisions, points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);
  } else {
    GenerateIcosphereMesh(radius, subdivisions, points_f3, faceVertexCounts, faceVertexIndices, normals_f3, uvs_f2);
  }

  // Create temporary GeomMesh with generated data
  GeomMesh temp_mesh;

  // Convert points from float3 to point3f
  std::vector<value::point3f> points;
  for (const auto &p : points_f3) {
    points.push_back(value::point3f{p[0], p[1], p[2]});
  }
  temp_mesh.points.set_value(points);
  temp_mesh.faceVertexCounts.set_value(faceVertexCounts);
  temp_mesh.faceVertexIndices.set_value(faceVertexIndices);

  // Copy properties from sphere
  temp_mesh.orientation = sphere.orientation;
  temp_mesh.doubleSided = sphere.doubleSided;

  // Set normals as face-varying primvar
  {
    std::vector<value::normal3f> normal3f_data;
    for (const auto &n : normals_f3) {
      normal3f_data.push_back(value::normal3f{n[0], n[1], n[2]});
    }
    temp_mesh.normals.set_value(normal3f_data);
    temp_mesh.normals.metas().set_interpolation_enum(Interpolation::FaceVarying);
  }

  // Set UVs as st primvar (face-varying)
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

  // Forward to ConvertMesh
  CopyDisplayPrimvarsToTempMesh(sphere, &temp_mesh);
  return ConvertMesh(env, abs_prim_path, temp_mesh, material_path,
                     subset_material_path_map, rmaterial_map,
                     material_subsets, blendshapes, dstMesh);
}



}  // namespace tydra
}  // namespace lightusd
