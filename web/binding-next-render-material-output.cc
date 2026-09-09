// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
emscripten::val RenderStream::materialObjectForPrim_(
      const lightusd::next::UsdPrim &mat) {
    emscripten::val m = emscripten::val::object();
    if (!mat.IsValid()) return m;
    // Resolve the surface shader: prefer the material's outputs:surface (a
    // connection), but fall back to the first UsdPreviewSurface child shader —
    // the common case and robust when the output connection is not resolved.
    lightusd::next::UsdPrim shader;
    const std::string shaderPath = lightusd::next::GetSurfaceShader(stage_, mat);
    if (!shaderPath.empty()) shader = stage_.GetPrimAtPath(shaderPath);
    if (!shader.IsValid()) {
      for (const auto &ch : mat.GetChildren()) {
        if (lightusd::next::IsPreviewSurface(ch)) { shader = ch; break; }
      }
    }
    if (!shader.IsValid()) return m;
    lightusd::next::PreviewSurfaceData ps;
    if (!lightusd::next::GetPreviewSurfaceData(stage_, shader, &ps)) return m;
    m.set("baseColor", arr3_(ps.diffuse_color));
    m.set("metallic", ps.metallic);
    m.set("roughness", ps.roughness);
    m.set("opacity", ps.opacity);
    m.set("occlusion", ps.occlusion);
    m.set("emissive", arr3_(ps.emissive_color));
    if (ps.opacity_threshold > 0.0f) m.set("opacityThreshold", ps.opacity_threshold);
    // PreviewSurfaceData texture fields are connection paths to the UsdUVTexture
    // shader; resolve each to its inputs:file asset path for the JS caller.
    auto setTex = [&](const char *key, const std::string &connPath) {
      const std::string file = texFile_(connPath);
      if (!file.empty()) m.set(key, file);
    };
    setTex("baseColorTexture", ps.diffuse_texture);
    setTex("normalTexture", ps.normal_texture);
    setTex("roughnessTexture", ps.roughness_texture);
    setTex("metallicTexture", ps.metallic_texture);
    setTex("occlusionTexture", ps.occlusion_texture);
    setTex("emissiveTexture", ps.emissive_texture);
    setTex("opacityTexture", ps.opacity_texture);
    return m;
  }

emscripten::val RenderStream::materialObject_(int32_t material_id) const {
    emscripten::val m = emscripten::val::object();
    if (material_id < 0 ||
        static_cast<size_t>(material_id) >= materials_.size()) {
      return m;
    }
    const MaterialRecord &rec = materials_[static_cast<size_t>(material_id)];
    m.set("id", rec.id);
    m.set("key", rec.key);
    m.set("primPath", rec.prim_path);
    const tr::RenderMaterial* render_mat = nullptr;
    auto mat_it = render_scene_.material_by_path.find(rec.prim_path);
    if (mat_it != render_scene_.material_by_path.end()) {
      render_mat = render_scene_.get_material(mat_it->second);
    }
    if (render_mat) {
      m.set("shaderType", RenderMaterialShaderTypeName(render_mat->shader_type));
      m.set("workingColorSpace", render_scene_.working_color_space);
      m.set("workingToDisplayLinear",
            Matrix3Value(render_scene_.working_to_display_linear));
      emscripten::val mtlx = emscripten::val::object();
      mtlx.set("authored", render_mat->mtlx_config.authored);
      mtlx.set("version", render_mat->mtlx_config.version);
      mtlx.set("namespace", render_mat->mtlx_config.name_space);
      mtlx.set("colorspace", render_mat->mtlx_config.colorspace);
      mtlx.set("sourceUri", render_mat->mtlx_config.source_uri);
      m.set("materialXConfig", mtlx);
      m.set("materialXJson", RenderMaterialJson(render_scene_, *render_mat));
      std::string nodegraph_json = render_mat->openpbr
                                       ? render_mat->openpbr->nodegraph_json
                                       : std::string();
      if (nodegraph_json.empty()) {
        const lightusd::next::UsdPrim mat =
            stage_.GetPrimAtPath(rec.prim_path);
        lightusd::next::UsdPrim shader;
        // A material may author both PreviewSurface and MaterialX terminals.
        // Graph reconstruction must prefer outputs:mtlx:surface even when the
        // renderer intentionally chose the generic outputs:surface fallback.
        const std::vector<lightusd::next::Path>* mtlx_connections =
            NextPropertyConnections(mat, "outputs:mtlx:surface");
        if (mtlx_connections && !mtlx_connections->empty()) {
          shader = stage_.GetPrimAtPath(
              NextConnectionPrimPath((*mtlx_connections)[0].str()));
        }
        if (!shader.IsValid()) {
          const std::string shader_path =
              lightusd::next::GetSurfaceShader(stage_, mat);
          if (!shader_path.empty()) shader = stage_.GetPrimAtPath(shader_path);
        }
        nodegraph_json = BuildNextNodeGraphJson(
            mat, shader, render_mat->mtlx_config.version);
      }
      if (!nodegraph_json.empty()) {
        m.set("openPBRNodeGraphJson", nodegraph_json);
      }
    }
    m.set("baseColor", arr3_(rec.base_color));
    m.set("metallic", rec.metallic);
    m.set("roughness", rec.roughness);
    m.set("opacity", rec.opacity);
    m.set("occlusion", rec.occlusion);
    m.set("emissive", arr3_(rec.emissive));
    if (rec.has_hair) {
      emscripten::val hair = emscripten::val::object();
      hair.set("model", std::string("chiang_hair_bsdf"));
      hair.set("tintR", arr3_(rec.hair_tint_r));
      hair.set("tintTT", arr3_(rec.hair_tint_tt));
      hair.set("tintTRT", arr3_(rec.hair_tint_trt));
      emscripten::val roughness_r = emscripten::val::array();
      emscripten::val roughness_tt = emscripten::val::array();
      emscripten::val roughness_trt = emscripten::val::array();
      for (size_t i = 0; i < 2; ++i) {
        roughness_r.set(i, rec.hair_roughness_r[i]);
        roughness_tt.set(i, rec.hair_roughness_tt[i]);
        roughness_trt.set(i, rec.hair_roughness_trt[i]);
      }
      hair.set("roughnessR", roughness_r);
      hair.set("roughnessTT", roughness_tt);
      hair.set("roughnessTRT", roughness_trt);
      hair.set("absorptionCoefficient", arr3_(rec.hair_absorption));
      hair.set("ior", rec.hair_ior);
      hair.set("cuticleAngle", rec.hair_cuticle_angle);
      m.set("hair", hair);
    }
    if (rec.opacity_threshold > 0.0f) {
      m.set("opacityThreshold", rec.opacity_threshold);
    }
    if (!rec.base_color_texture.empty()) {
      m.set("baseColorTexture", rec.base_color_texture);
    }
    if (!rec.normal_texture.empty()) {
      m.set("normalTexture", rec.normal_texture);
    }
    if (!rec.roughness_texture.empty()) {
      m.set("roughnessTexture", rec.roughness_texture);
    }
    if (!rec.metallic_texture.empty()) {
      m.set("metallicTexture", rec.metallic_texture);
    }
    if (!rec.occlusion_texture.empty()) {
      m.set("occlusionTexture", rec.occlusion_texture);
    }
    if (!rec.emissive_texture.empty()) {
      m.set("emissiveTexture", rec.emissive_texture);
    }
    if (!rec.opacity_texture.empty()) {
      m.set("opacityTexture", rec.opacity_texture);
    }
    auto metaObject = [](const TextureMeta &meta) {
      emscripten::val out = emscripten::val::object();
      out.set("path", meta.path);
      out.set("sourceColorSpace", meta.source_color_space);
      out.set("wrapS", meta.wrap_s);
      out.set("wrapT", meta.wrap_t);
      out.set("isUdim", meta.is_udim);
      out.set("colorTransformValid", meta.color_transform_valid);
      out.set("colorTransformBypass", meta.color_transform_bypass);
      out.set("sourceColorIsData", meta.source_color_is_data);
      out.set("sourceGamma", meta.source_gamma);
      out.set("sourceLinearBias", meta.source_linear_bias);
      emscripten::val matrix = emscripten::val::array();
      for (size_t i = 0; i < meta.source_to_display_linear.size(); ++i) {
        matrix.set(i, meta.source_to_display_linear[i]);
      }
      out.set("sourceToDisplayLinear", matrix);
      return out;
    };
    emscripten::val texture_meta = emscripten::val::object();
    if (!rec.base_color_meta.path.empty()) {
      texture_meta.set("baseColor", metaObject(rec.base_color_meta));
    }
    if (!rec.normal_meta.path.empty()) {
      texture_meta.set("normal", metaObject(rec.normal_meta));
    }
    if (!rec.roughness_meta.path.empty()) {
      texture_meta.set("roughness", metaObject(rec.roughness_meta));
    }
    if (!rec.metallic_meta.path.empty()) {
      texture_meta.set("metallic", metaObject(rec.metallic_meta));
    }
    if (!rec.occlusion_meta.path.empty()) {
      texture_meta.set("occlusion", metaObject(rec.occlusion_meta));
    }
    if (!rec.emissive_meta.path.empty()) {
      texture_meta.set("emissive", metaObject(rec.emissive_meta));
    }
    if (!rec.opacity_meta.path.empty()) {
      texture_meta.set("opacity", metaObject(rec.opacity_meta));
    }
    m.set("textureMetadata", texture_meta);
    return m;
  }

void RenderStream::addGeomSubsetMaterials_(const lightusd::next::UsdPrim &prim,
                               emscripten::val &out) {
    // Prefer the converter's triangle-space subset ranges: they account for
    // holes, degenerate faces, earcut splits and topology sanitization,
    // which the stage-side re-derivation below cannot.
    if (render_scene_valid_) {
      const auto mit = render_scene_.mesh_by_path.find(prim.GetPath().str());
      if (mit != render_scene_.mesh_by_path.end() &&
          static_cast<size_t>(mit->second) < render_scene_.meshes.size()) {
        const tr::RenderMesh &rmesh =
            render_scene_.meshes[static_cast<size_t>(mit->second)];
        if (!rmesh.material_subsets.empty() &&
            !rmesh.face_triangle_offsets.empty()) {
          emscripten::val materials = emscripten::val::array();
          emscripten::val groups = emscripten::val::array();
          std::map<int32_t, int> mat_index_by_scene_id;
          int group_index = 0;
          for (const tr::RenderMesh::MaterialSubset &ms :
               rmesh.material_subsets) {
            if (ms.material_id < 0 ||
                static_cast<size_t>(ms.material_id) >=
                    render_scene_.materials.size()) {
              continue;
            }
            int mat_index = -1;
            const auto found = mat_index_by_scene_id.find(ms.material_id);
            if (found == mat_index_by_scene_id.end()) {
              const std::string &mat_path =
                  render_scene_.materials[static_cast<size_t>(ms.material_id)]
                      .prim_path;
              lightusd::next::UsdPrim mat_prim = stage_.GetPrimAtPath(mat_path);
              const int32_t record_id = registerMaterial_(mat_prim);
              mat_index = static_cast<int>(mat_index_by_scene_id.size());
              mat_index_by_scene_id.emplace(ms.material_id, mat_index);
              materials.set(mat_index, materialObject_(record_id));
            } else {
              mat_index = found->second;
            }
            emscripten::val g = emscripten::val::object();
            g.set("start", static_cast<int>(ms.face_start * 3u));
            g.set("count", static_cast<int>(ms.face_count * 3u));
            g.set("materialIndex", mat_index);
            groups.set(group_index++, g);
          }
          if (group_index > 0) {
            out.set("materials", materials);
            out.set("submeshes", groups);
          }
          return;
        }
      }
    }

    std::vector<int32_t> fvc = matIntStatic_(prim, "faceVertexCounts");
    if (fvc.empty()) return;

    struct SubsetInfo {
      lightusd::next::UsdPrim prim;
      std::vector<int32_t> faces;
    };
    std::vector<SubsetInfo> subsets;
    for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
      if (!child.IsValid() || child.GetTypeName() != "GeomSubset") continue;
      const lightusd::next::Value *family = child.GetPropertyValue("familyName");
      if (family) {
        const std::string *tok = family->as_token();
        if (tok && *tok != "materialBind") continue;
      }
      std::vector<int32_t> faces = matIntStatic_(child, "indices");
      if (faces.empty()) continue;
      lightusd::next::UsdPrim mat = lightusd::next::GetBoundMaterial(stage_, child);
      if (!mat.IsValid()) continue;
      subsets.push_back({child, std::move(faces)});
    }
    if (subsets.empty()) return;

    std::vector<int> face_material(fvc.size(), -1);
    emscripten::val materials = emscripten::val::array();
    for (size_t i = 0; i < subsets.size(); ++i) {
      const int mat_index = static_cast<int>(i);
      for (int32_t face : subsets[i].faces) {
        if (face >= 0 && static_cast<size_t>(face) < face_material.size()) {
          face_material[static_cast<size_t>(face)] = mat_index;
        }
      }
      lightusd::next::UsdPrim mat =
          lightusd::next::GetBoundMaterial(stage_, subsets[i].prim);
      const int32_t material_id = registerMaterial_(mat);
      materials.set(mat_index, materialObject_(material_id));
    }

    const std::vector<uint32_t> tri_starts = faceTriangleStarts_(fvc);
    emscripten::val groups = emscripten::val::array();
    int group_index = 0;
    size_t face_begin = 0;
    while (face_begin < face_material.size()) {
      const int mat_index = face_material[face_begin];
      size_t face_end = face_begin + 1;
      while (face_end < face_material.size() &&
             face_material[face_end] == mat_index) {
        face_end++;
      }
      if (mat_index >= 0 && face_begin < tri_starts.size() &&
          face_end < tri_starts.size()) {
        const uint32_t start = tri_starts[face_begin] * 3u;
        const uint32_t count = (tri_starts[face_end] - tri_starts[face_begin]) * 3u;
        if (count > 0) {
          emscripten::val g = emscripten::val::object();
          g.set("start", static_cast<int>(start));
          g.set("count", static_cast<int>(count));
          g.set("materialIndex", mat_index);
          groups.set(group_index++, g);
        }
      }
      face_begin = face_end;
    }

    out.set("materials", materials);
    out.set("submeshes", groups);
  }
}  // namespace web_next
}  // namespace lightusd
