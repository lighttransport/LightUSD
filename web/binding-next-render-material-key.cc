// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
std::string RenderStream::normTexKey_(const std::string &path) {
    size_t first = 0;
    while (first < path.size() && (path[first] == '.' || path[first] == '/')) {
      ++first;
    }
    return path.substr(first);
  }

RenderStream::TextureMeta RenderStream::texMeta_(const std::string &connPath) {
    TextureMeta meta;
    if (connPath.empty()) return meta;
    const size_t slash = connPath.rfind('/');
    const size_t dot = connPath.find('.', slash == std::string::npos ? 0 : slash);
    const std::string primPath = (dot == std::string::npos)
                                     ? connPath
                                     : connPath.substr(0, dot);
    lightusd::next::UsdPrim tex = stage_.GetPrimAtPath(primPath);
    if (!tex.IsValid()) return meta;

    const lightusd::next::Value *file = tex.GetPropertyValue("inputs:file");
    if (file) {
      if (const std::string *a = file->as_asset_path()) {
        meta.path = *a;
      } else if (const std::string *s = file->as_string()) {
        meta.path = *s;
      }
    }
    auto read_tokenish = [&](const char *name, std::string *out) {
      if (!out) return;
      const lightusd::next::Value *v = tex.GetPropertyValue(name);
      if (!v) return;
      if (const std::string *tok = v->as_token()) {
        *out = *tok;
      } else if (const std::string *str = v->as_string()) {
        *out = *str;
      }
    };
    read_tokenish("inputs:sourceColorSpace", &meta.source_color_space);
    // colorSpace asset metadata on inputs:file wins over sourceColorSpace
    // (legacy tydra resolution order).
    if (const lightusd::next::PropMeta *file_meta =
            tex.GetPropertyMeta("inputs:file")) {
      if (!file_meta->colorSpace.empty()) {
        meta.source_color_space = file_meta->colorSpace;
      }
    }
    read_tokenish("inputs:wrapS", &meta.wrap_s);
    read_tokenish("inputs:wrapT", &meta.wrap_t);
    meta.is_udim = isUdimPath_(meta.path);
    return meta;
  }

void RenderStream::appendMaterialKey_(const MaterialRecord &m,
                                 std::ostringstream *ss) {
    *ss << "bc=" << fmtFloat_(m.base_color[0]) << "," << fmtFloat_(m.base_color[1])
        << "," << fmtFloat_(m.base_color[2]);
    *ss << "|metal=" << fmtFloat_(m.metallic);
    *ss << "|rough=" << fmtFloat_(m.roughness);
    *ss << "|opacity=" << fmtFloat_(m.opacity);
    *ss << "|occ=" << fmtFloat_(m.occlusion);
    *ss << "|emit=" << fmtFloat_(m.emissive[0]) << "," << fmtFloat_(m.emissive[1])
        << "," << fmtFloat_(m.emissive[2]);
    *ss << "|alpha=" << fmtFloat_(m.opacity_threshold);
    *ss << "|base=" << normTexKey_(m.base_color_texture);
    *ss << "|normal=" << normTexKey_(m.normal_texture);
    *ss << "|roughtex=" << normTexKey_(m.roughness_texture);
    *ss << "|metaltex=" << normTexKey_(m.metallic_texture);
    *ss << "|occtex=" << normTexKey_(m.occlusion_texture);
    *ss << "|emittex=" << normTexKey_(m.emissive_texture);
    *ss << "|opacitytex=" << normTexKey_(m.opacity_texture);
    if (m.has_hair) {
      *ss << "|hair=" << fmtFloat_(m.hair_tint_r[0]) << ","
          << fmtFloat_(m.hair_tint_r[1]) << ","
          << fmtFloat_(m.hair_tint_r[2]);
      *ss << "|hairtt=" << fmtFloat_(m.hair_tint_tt[0]) << ","
          << fmtFloat_(m.hair_tint_tt[1]) << ","
          << fmtFloat_(m.hair_tint_tt[2]);
      *ss << "|hairtrt=" << fmtFloat_(m.hair_tint_trt[0]) << ","
          << fmtFloat_(m.hair_tint_trt[1]) << ","
          << fmtFloat_(m.hair_tint_trt[2]);
      *ss << "|hairior=" << fmtFloat_(m.hair_ior)
          << "|haircuticle=" << fmtFloat_(m.hair_cuticle_angle);
    }
  }

bool RenderStream::populateHairMaterial_(const lightusd::next::UsdPrim &prim,
                                    MaterialRecord *rec) {
    if (!prim.IsValid() || !rec) return false;
    const lightusd::next::Value *id_value = prim.GetPropertyValue("info:id");
    std::string shader_id;
    if (id_value) {
      if (const std::string *token = id_value->as_token()) shader_id = *token;
      else if (const std::string *str = id_value->as_string()) shader_id = *str;
    }
    bool found = shader_id.find("chiang_hair_bsdf") != std::string::npos ||
                 shader_id.find("principled_hair") != std::string::npos;
    if (found) {
      rec->has_hair = true;
      auto color = [&](const char *name, float *out) {
        const lightusd::next::Value *value = prim.GetPropertyValue(name);
        if (value) (void)value->to_float3(out);
      };
      auto pair = [&](const char *name, float *out) {
        const lightusd::next::Value *value = prim.GetPropertyValue(name);
        if (value) (void)value->to_float2(out);
      };
      auto scalar = [&](const char *name, float *out) {
        const lightusd::next::Value *value = prim.GetPropertyValue(name);
        if (value) (void)value->to_float(out);
      };
      color("inputs:tint_R", rec->hair_tint_r);
      color("inputs:tint_TT", rec->hair_tint_tt);
      color("inputs:tint_TRT", rec->hair_tint_trt);
      pair("inputs:roughness_R", rec->hair_roughness_r);
      pair("inputs:roughness_TT", rec->hair_roughness_tt);
      pair("inputs:roughness_TRT", rec->hair_roughness_trt);
      color("inputs:absorption_coefficient", rec->hair_absorption);
      scalar("inputs:ior", &rec->hair_ior);
      scalar("inputs:cuticle_angle", &rec->hair_cuticle_angle);
    }
    for (const lightusd::next::UsdPrim &child : prim.GetChildren()) {
      found = populateHairMaterial_(child, rec) || found;
    }
    return found;
  }

bool RenderStream::ensureRenderMaterial_(const lightusd::next::UsdPrim &mat) {
    if (!mat.IsValid()) return false;
    const std::string path = mat.GetPath().str();
    if (render_scene_.material_by_path.find(path) !=
        render_scene_.material_by_path.end()) {
      return true;
    }

    // meshOnly intentionally skips the full RenderScene hierarchy and geometry
    // catalog. Material conversion is still required: otherwise getMesh()
    // falls back to the universal PreviewSurface terminal and loses an
    // authoritative outputs:mtlx:surface graph. Convert just this bound
    // material into the otherwise-empty scene so worker conversion retains
    // MaterialX values, node graphs, and texture metadata without rebuilding
    // the potentially very large node hierarchy.
    tr::ConverterConfig config;
    config.time_code = 0.0;
    config.material.load_textures = false;
    config.material.allow_missing_textures = true;
    tr::RenderSceneConverter converter(config);
    tr::RenderMaterial material;
    if (!converter.ConvertMaterial(stage_, mat, &material, &render_scene_)) {
      return false;
    }
    const int32_t id = static_cast<int32_t>(render_scene_.materials.size());
    render_scene_.material_by_path[material.prim_path] = id;
    render_scene_.materials.push_back(std::move(material));
    render_scene_valid_ = true;
    return true;
  }

bool RenderStream::requiresRenderMaterial_(const lightusd::next::UsdPrim &mat) const {
    if (!mat.IsValid()) return false;

    // PreviewSurface is decoded directly below and does not need the much
    // heavier RenderSceneConverter. Keep that converter for MaterialX and
    // other non-Preview terminals whose graph evaluation is authoritative.
    if (const lightusd::next::PrimSpec *spec = mat.GetPrimSpec()) {
      const std::vector<lightusd::next::Path> *mtlx =
          spec->connection("outputs:mtlx:surface");
      if (mtlx && !mtlx->empty()) return true;
    }
    if (const std::vector<lightusd::next::Path> *mtlx =
            mat.GetRelationship("outputs:mtlx:surface")) {
      if (!mtlx->empty()) return true;
    }
    if (const std::vector<lightusd::next::Path> *source =
            mat.GetRelationship("mtlx:surface:source")) {
      if (!source->empty()) return true;
    }

    const std::string surface_path =
        lightusd::next::GetSurfaceShader(stage_, mat);
    if (surface_path.empty()) return false;
    const lightusd::next::UsdPrim surface =
        stage_.GetPrimAtPath(surface_path);
    return surface.IsValid() && !lightusd::next::IsPreviewSurface(surface);
  }

std::string RenderStream::materialSourceIdentity_(
      const lightusd::next::UsdPrim &mat) const {
    if (!mat.IsValid()) return {};

    // MaterialX exports often repeat the same local graph under hundreds of
    // differently named Material prims. Canonicalize the connected graph
    // before conversion so exact semantic duplicates share one
    // RenderMaterial; absolute material paths are normalized by the encoder.
    bool has_mtlx_surface = false;
    if (const lightusd::next::PrimSpec *spec = mat.GetPrimSpec()) {
      const std::vector<lightusd::next::Path> *mtlx =
          spec->connection("outputs:mtlx:surface");
      has_mtlx_surface = mtlx && !mtlx->empty();
    }
    if (const std::vector<lightusd::next::Path> *mtlx =
            mat.GetRelationship("outputs:mtlx:surface")) {
      has_mtlx_surface = has_mtlx_surface || !mtlx->empty();
    }
    if (const std::vector<lightusd::next::Path> *source =
            mat.GetRelationship("mtlx:surface:source")) {
      has_mtlx_surface = has_mtlx_surface || !source->empty();
    }
    if (has_mtlx_surface) {
      const std::string canonical = CanonicalMaterialGraph(stage_, mat);
      return canonical.empty() ? std::string() : "mtlx:" + canonical;
    }

    // PreviewSurface networks can be repeated just as heavily as MaterialX
    // networks. Their connected graph is a complete semantic key, so reuse an
    // already-decoded material before walking every texture input again.
    const std::string canonical_preview = CanonicalMaterialGraph(stage_, mat);
    if (!canonical_preview.empty()) return "preview:" + canonical_preview;

    std::string source_asset;
    const std::vector<lightusd::next::UsdPrim> children = mat.GetChildren();
    for (const lightusd::next::UsdPrim &child : children) {
      const lightusd::next::Value *value =
          child.GetPropertyValue("info:unreal:sourceAsset");
      if (!value) continue;
      if (const std::string *asset = value->as_asset_path()) {
        source_asset = *asset;
      } else if (const std::string *value_string = value->as_string()) {
        source_asset = *value_string;
      }
      if (!source_asset.empty()) break;
    }
    // Ordinary PreviewSurface materials do not need an identity: their final
    // RenderMaterial key already performs exact deduplication after conversion.
    // Avoid walking and serializing every shader graph unless this is one of
    // the source-asset copies for which pre-conversion reuse is beneficial.
    if (source_asset.empty()) return {};

    bool has_preview_surface = false;
    const std::string material_path = mat.GetPath().str();
    std::string signature;
    signature.reserve(512);
    auto append_text = [&](const std::string &text) {
      signature.append(text);
      signature.push_back('\0');
    };
    auto append_float = [&](float value) {
      signature.append(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    for (const lightusd::next::UsdPrim &child : children) {
      if (lightusd::next::IsPreviewSurface(child)) {
        has_preview_surface = true;
        const char *scalar_names[] = {
            "inputs:metallic", "inputs:roughness", "inputs:opacity",
            "inputs:occlusion", "inputs:opacityThreshold"};
        for (const char *name : scalar_names) {
          const lightusd::next::Value *scalar_value =
              child.GetPropertyValue(name);
          float scalar = 0.0f;
          append_text(name);
          if (scalar_value && scalar_value->to_float(&scalar)) {
            signature.push_back('\1');
            append_float(scalar);
          } else {
            signature.push_back('\0');
          }
        }
        const char *color_names[] = {
            "inputs:diffuseColor", "inputs:emissiveColor"};
        for (const char *name : color_names) {
          const lightusd::next::Value *color_value =
              child.GetPropertyValue(name);
          float color[3] = {0.0f, 0.0f, 0.0f};
          append_text(name);
          if (color_value && color_value->to_float3(color)) {
            signature.push_back('\1');
            append_float(color[0]);
            append_float(color[1]);
            append_float(color[2]);
          } else {
            signature.push_back('\0');
          }
        }
        const char *connection_names[] = {
            "inputs:diffuseColor", "inputs:normal", "inputs:roughness",
            "inputs:metallic", "inputs:occlusion", "inputs:emissiveColor",
            "inputs:opacity"};
        const lightusd::next::PrimSpec *child_spec = child.GetPrimSpec();
        for (const char *name : connection_names) {
          append_text(name);
          const std::vector<lightusd::next::Path> *connections =
              child_spec ? child_spec->connection(name) : nullptr;
          if (!connections) {
            signature.push_back('\0');
            continue;
          }
          signature.push_back('\1');
          for (const lightusd::next::Path &connection : *connections) {
            std::string target = connection.str();
            if (target.rfind(material_path, 0) == 0) {
              target.erase(0, material_path.size());
            }
            append_text(target);
          }
        }
      }
      const lightusd::next::Value *file =
          child.GetPropertyValue("inputs:file");
      if (file) {
        const std::string *asset = file->as_asset_path();
        if (!asset) asset = file->as_string();
        if (asset) {
          append_text("tex");
          append_text(child.GetName());
          append_text(*asset);
        }
      }
    }
    if (!has_preview_surface) return {};
    std::string identity = std::string("unreal:") + source_asset;
    identity.push_back('\0');
    identity.append(signature);
    return identity;
  }
}  // namespace web_next
}  // namespace lightusd
