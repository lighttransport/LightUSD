// SPDX-License-Identifier: Apache-2.0
#include "lusdr_generated.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include "tydra/render-converter.hh"
#include "tydra/render-extract.hh"

namespace lusdr {

void ExpandGeneratedMeshCornersNext(const GeneratedMeshNext &mesh,
                                    std::vector<float> *points,
                                    std::vector<float> *normals,
                                    std::vector<float> *uvs,
                                    std::vector<uint32_t> *indices) {
  points->clear(); normals->clear(); uvs->clear(); indices->clear();
  points->reserve(mesh.indices.size() * 3);
  normals->reserve(mesh.indices.size() * 3);
  uvs->reserve(mesh.indices.size() * 2);
  for (size_t corner = 0; corner < mesh.indices.size(); ++corner) {
    const size_t index = size_t(mesh.indices[corner]);
    for (size_t c = 0; c < 3; ++c) points->push_back(mesh.points[index * 3 + c]);
    const size_t ni = mesh.normals_facevarying ? corner : index;
    for (size_t c = 0; c < 3; ++c)
      normals->push_back(ni * 3 + c < mesh.normals.size()
                            ? mesh.normals[ni * 3 + c] : 0.0f);
    const size_t ui = mesh.uvs_facevarying ? corner : index;
    for (size_t c = 0; c < 2; ++c)
      uvs->push_back(ui * 2 + c < mesh.uvs.size()
                        ? mesh.uvs[ui * 2 + c] : 0.0f);
  }
  size_t offset = 0;
  for (int32_t count : mesh.counts) {
    for (int32_t k = 1; k + 1 < count; ++k) {
      indices->push_back(uint32_t(offset));
      indices->push_back(uint32_t(offset + size_t(k)));
      indices->push_back(uint32_t(offset + size_t(k + 1)));
    }
    offset += size_t(count);
  }
}

bool IsTriangleMeshNext(const lightusd::next::UsdPrim &prim) {
  const std::string &type = prim.GetTypeName();
  return type == "Mesh" ||
         lightusd::tydra::next::IsAnalyticGeomTypeName(type);
}

bool GenerateAnalyticMeshNext(const lightusd::next::Stage &stage,
                              const lightusd::next::UsdPrim &prim, double time,
                              GeneratedMeshNext *out) {
  if (!out || !lightusd::tydra::next::IsAnalyticGeomTypeName(
                  prim.GetTypeName())) return false;
  *out = GeneratedMeshNext{};
  lightusd::tydra::next::ConverterConfig config;
  config.time_code = time;
  // Keep polygon corners so generated face-varying normals/UVs stay aligned.
  // The consumers perform the same fan triangulation as for authored Meshes.
  config.mesh.triangulate = false;
  lightusd::tydra::next::RenderSceneConverter converter(config);
  lightusd::tydra::next::RenderMesh mesh;
  if (!converter.ConvertRenderableMesh(stage, prim, &mesh) ||
      mesh.has_alloc_failure()) {
    std::cerr << "Failed to tessellate " << prim.GetPath().str() << ": "
              << converter.GetLastError() << "\n";
    return false;
  }
  out->points = mesh.points.flatten();
  for (float point : out->points) {
    if (!std::isfinite(point)) {
      std::cerr << "Non-finite generated geometry: " << prim.GetPath().str()
                << "\n";
      *out = GeneratedMeshNext{};
      return false;
    }
  }
  for (uint32_t count : mesh.face_vertex_counts) {
    if (count > uint32_t(std::numeric_limits<int32_t>::max())) return false;
    out->counts.push_back(static_cast<int32_t>(count));
  }
  for (uint32_t index : mesh.face_vertex_indices) {
    if (index > uint32_t(std::numeric_limits<int32_t>::max())) return false;
    out->indices.push_back(static_cast<int32_t>(index));
  }
  out->normals = mesh.normals.flatten();
  out->uvs = mesh.texcoords_0.flatten();
  out->normals_facevarying = mesh.normals_interp ==
      lightusd::tydra::next::Interpolation::FaceVarying;
  out->uvs_facevarying = mesh.texcoords_0_interp ==
      lightusd::tydra::next::Interpolation::FaceVarying;
  return !out->points.empty() && !out->indices.empty();
}

}  // namespace lusdr
