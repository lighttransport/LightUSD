// SPDX-License-Identifier: Apache-2.0
#include "next_mesh_adapter.hh"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <vector>

#include "c-api/lightusd-render-cpp.hh"
#include "tydra/next/render-converter.hh"

namespace lusdview {
namespace tydn = ::lightusd::tydra::next;
namespace {
template <typename Chunked, typename Element>
bool AppendPublicBuffer(const lightusd::api::RenderScene& scene, int32_t meshId,
                        uint8_t kind, Chunked* output) {
  if (!output) return false;
  lightusd_buffer_view view{};
  if (lightusd::api::RenderMeshBuffer(
          const_cast<lightusd::api::RenderScene&>(scene), meshId, kind,
          &view) != LIGHTUSD_OK ||
      view.nbytes % sizeof(Element) != 0) {
    return false;
  }
  return output->append(static_cast<const Element*>(view.data),
                        view.nbytes / sizeof(Element));
}

template <typename Element>
bool CopyPublicVectorBuffer(const lightusd::api::RenderScene& scene,
                            int32_t meshId, uint8_t kind,
                            std::vector<Element>* output) {
  if (!output) return false;
  lightusd_buffer_view view{};
  if (lightusd::api::RenderMeshBuffer(
          const_cast<lightusd::api::RenderScene&>(scene), meshId, kind,
          &view) != LIGHTUSD_OK ||
      view.nbytes % sizeof(Element) != 0) {
    return false;
  }
  const Element* data = static_cast<const Element*>(view.data);
  const size_t count = view.nbytes / sizeof(Element);
  if (count == 0) output->clear();
  else output->assign(data, data + count);
  return true;
}

}  // namespace

bool ConvertMeshThroughPublicAPI(
    const lightusd_stage* stage, const std::string& path,
    const tydn::ConverterConfig& converterConfig, tydn::RenderMesh* out,
    uint8_t proxyMode) {
  if (!stage || !out) return false;
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage, path.c_str());
  if (!lightusd_prim_is_valid(prim)) return false;

  lightusd_render_config config;
  lightusd_render_config_init(&config);
  config.triangulate = converterConfig.mesh.triangulate ? 1 : 0;
  config.compute_normals = converterConfig.mesh.compute_normals ? 1 : 0;
  config.compute_tangents = converterConfig.mesh.compute_tangents ? 1 : 0;
  config.build_vertex_indices =
      converterConfig.mesh.build_vertex_indices ? 1 : 0;
  config.triangulation_method =
      converterConfig.mesh.triangulation_method ==
              tydn::MeshConfig::TriangulationMethod::Fan
          ? 1
          : 0;
  config.tangent_method =
      static_cast<uint8_t>(converterConfig.mesh.tangent_method);
  config.load_textures = converterConfig.material.load_textures ? 1 : 0;
  config.allow_missing_textures =
      converterConfig.material.allow_missing_textures ? 1 : 0;
  config.target_color_space =
      static_cast<uint8_t>(converterConfig.material.target_color_space);
  if (converterConfig.material.binding_purpose == "preview")
    config.material_binding_purpose = LIGHTUSD_MATERIAL_BINDING_PREVIEW;
  else if (converterConfig.material.binding_purpose == "full")
    config.material_binding_purpose = LIGHTUSD_MATERIAL_BINDING_FULL;
  config.time_code = converterConfig.time_code;
  config.max_threads = 1;
  config.use_default_asset_resolver = 0;
  const auto levelIt = converterConfig.mesh.subdivision_prim_levels.find(path);
  const int subdivisionLevel =
      levelIt == converterConfig.mesh.subdivision_prim_levels.end()
          ? converterConfig.mesh.subdivision_level
          : levelIt->second;

  lightusd::api::RenderScene scene;
  lightusd_status convertStatus = LIGHTUSD_OK;
  if (proxyMode == 0) {
    convertStatus = lightusd_render_convert_mesh(
        stage, prim, &config, subdivisionLevel, scene.put());
  } else {
    static const float boundsMin[3] = {-1.0f, -1.0f, -1.0f};
    static const float boundsMax[3] = {1.0f, 1.0f, 1.0f};
    convertStatus = lightusd_render_convert_mesh_proxy(
        stage, prim, proxyMode == 1 ? 0 : 1,
        proxyMode == 1 ? nullptr : boundsMin,
        proxyMode == 1 ? nullptr : boundsMax, scene.put());
  }
  if (convertStatus != LIGHTUSD_OK) {
    return false;
  }
  lightusd_render_mesh_info info{};
  lightusd_render_mesh_extra_info extra{};
  if (lightusd::api::RenderMeshInfo(scene, 0, &info) != LIGHTUSD_OK ||
      lightusd::api::RenderMeshExtraInfo(scene, 0, &extra) != LIGHTUSD_OK) {
    return false;
  }

  tydn::RenderMesh mesh;
  mesh.name.assign(info.name.data ? info.name.data : "", info.name.len);
  mesh.prim_path.assign(info.prim_path.data ? info.prim_path.data : "",
                        info.prim_path.len);
  mesh.material_id = info.material_id;
  mesh.is_triangulated = info.is_triangulated != 0;
  mesh.has_bbox = info.has_bbox != 0;
  mesh.bbox_min = {info.bbox_min[0], info.bbox_min[1], info.bbox_min[2]};
  mesh.bbox_max = {info.bbox_max[0], info.bbox_max[1], info.bbox_max[2]};
  mesh.double_sided = extra.double_sided != 0;
  mesh.normals_interp = static_cast<tydn::Interpolation>(info.normals_interp);
  mesh.texcoords_0_interp =
      static_cast<tydn::Interpolation>(info.texcoords0_interp);
  mesh.texcoords_1_interp =
      static_cast<tydn::Interpolation>(info.texcoords1_interp);
  mesh.colors_interp = static_cast<tydn::Interpolation>(info.colors_interp);
  mesh.tangents_interp =
      static_cast<tydn::Interpolation>(extra.tangents_interp);
  mesh.opacities_interp =
      static_cast<tydn::Interpolation>(extra.opacities_interp);
  mesh.texcoords_0_name.assign(
      extra.texcoords0_name.data ? extra.texcoords0_name.data : "",
      extra.texcoords0_name.len);
  mesh.texcoords_1_name.assign(
      extra.texcoords1_name.data ? extra.texcoords1_name.data : "",
      extra.texcoords1_name.len);

  if (!AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_POINTS, &mesh.points) ||
      !AppendPublicBuffer<tydn::UInt32Chunked, uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_FACE_COUNTS,
          &mesh.face_vertex_counts) ||
      !AppendPublicBuffer<tydn::UInt32Chunked, uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_FACE_INDICES,
          &mesh.face_vertex_indices) ||
      !AppendPublicBuffer<tydn::UInt32Chunked, uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_TRI_INDICES,
          &mesh.triangulated_indices) ||
      !AppendPublicBuffer<tydn::UInt32Chunked, uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES,
          &mesh.triangulated_face_vertex_indices) ||
      !CopyPublicVectorBuffer<uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_SUBDIVISION_FACE_SOURCE,
          &mesh.subdivision_face_source) ||
      !CopyPublicVectorBuffer<uint32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_FACE_TRIANGLE_OFFSETS,
          &mesh.face_triangle_offsets) ||
      !CopyPublicVectorBuffer<int32_t>(
          scene, 0, LIGHTUSD_MESH_BUF_SANITIZE_FACE_REMAP,
          &mesh.sanitize_face_remap) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_NORMALS, &mesh.normals) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_TANGENTS, &mesh.tangents) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_TEXCOORDS0, &mesh.texcoords_0) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_TEXCOORDS1, &mesh.texcoords_1) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_COLORS, &mesh.colors) ||
      !AppendPublicBuffer<tydn::FloatChunked, float>(
          scene, 0, LIGHTUSD_MESH_BUF_OPACITIES, &mesh.opacities)) {
    return false;
  }
  mesh.sanitize_dropped_faces = static_cast<uint32_t>(std::count(
      mesh.sanitize_face_remap.begin(), mesh.sanitize_face_remap.end(), -1));
  if (info.has_skin) {
    mesh.skin = std::make_shared<tydn::RenderMesh::SkinBinding>();
    mesh.skin->skeleton_id = info.skeleton_id;
    mesh.skin->influences_per_vertex = 4;
    if (!AppendPublicBuffer<tydn::UInt16Chunked, uint16_t>(
            scene, 0, LIGHTUSD_MESH_BUF_JOINT_INDICES,
            &mesh.skin->joint_indices) ||
        !AppendPublicBuffer<tydn::FloatChunked, float>(
            scene, 0, LIGHTUSD_MESH_BUF_JOINT_WEIGHTS,
            &mesh.skin->joint_weights)) {
      return false;
    }
  }
  mesh.blend_shapes.resize(info.blend_shape_count);
  for (size_t i = 0; i < info.primvar_count; ++i) {
    lightusd_render_primvar_info primvarInfo{};
    if (lightusd::api::RenderMeshPrimvarInfo(scene, 0, i, &primvarInfo) !=
        LIGHTUSD_OK) {
      return false;
    }
    tydn::VertexAttribute& primvar = mesh.primvars.emplace_back();
    primvar.name.assign(primvarInfo.name.data ? primvarInfo.name.data : "",
                        primvarInfo.name.len);
    primvar.format = static_cast<tydn::VertexFormat>(primvarInfo.format);
    primvar.interpolation =
        static_cast<tydn::Interpolation>(primvarInfo.interpolation);
    lightusd_buffer_view dataView{};
    lightusd_buffer_view indexView{};
    if (lightusd::api::RenderMeshPrimvarBuffer(scene, 0, i, 0, &dataView) !=
            LIGHTUSD_OK ||
        lightusd::api::RenderMeshPrimvarBuffer(scene, 0, i, 1, &indexView) !=
            LIGHTUSD_OK) {
      return false;
    }
    const auto appendTyped = [&](auto* destination, const auto* source,
                                 size_t byteCount) {
      using Element = typename std::remove_pointer<decltype(source)>::type;
      return byteCount % sizeof(Element) == 0 &&
             destination->append(source, byteCount / sizeof(Element));
    };
    switch (primvar.format) {
      case tydn::VertexFormat::Float:
      case tydn::VertexFormat::Vec2:
      case tydn::VertexFormat::Vec3:
      case tydn::VertexFormat::Vec4:
      case tydn::VertexFormat::Matrix33:
      case tydn::VertexFormat::Matrix44:
        if (!appendTyped(&primvar.float_data,
                         static_cast<const float*>(dataView.data),
                         dataView.nbytes)) return false;
        break;
      case tydn::VertexFormat::Int:
      case tydn::VertexFormat::IVec2:
      case tydn::VertexFormat::IVec3:
      case tydn::VertexFormat::IVec4:
        if (!appendTyped(&primvar.int_data,
                         static_cast<const int32_t*>(dataView.data),
                         dataView.nbytes)) return false;
        break;
      case tydn::VertexFormat::UInt:
      case tydn::VertexFormat::UVec2:
      case tydn::VertexFormat::UVec3:
      case tydn::VertexFormat::UVec4:
        if (!appendTyped(&primvar.uint_data,
                         static_cast<const uint32_t*>(dataView.data),
                         dataView.nbytes)) return false;
        break;
    }
    if (primvarInfo.has_indices &&
        !primvar.indices.append(static_cast<const uint32_t*>(indexView.data),
                                indexView.nbytes / sizeof(uint32_t))) {
      return false;
    }
  }
  *out = std::move(mesh);
  return true;
}

}  // namespace lusdview
