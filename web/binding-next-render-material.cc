// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
RenderStream::MaterialRecord RenderStream::materialRecordForPrim_(
      const lightusd::next::UsdPrim &mat) {
    MaterialRecord rec;
    if (!mat.IsValid()) {
      rec.prim_path = "__default";
      std::ostringstream ss;
      appendMaterialKey_(rec, &ss);
      rec.key = ss.str();
      return rec;
    }
    rec.prim_path = mat.GetPath().str();
    if (requiresRenderMaterial_(mat)) {
      (void)ensureRenderMaterial_(mat);
    }
    bool populated_from_render_scene = false;
    if (render_scene_valid_) {
      const auto material_it = render_scene_.material_by_path.find(rec.prim_path);
      if (material_it != render_scene_.material_by_path.end()) {
        const tr::RenderMaterial *render_mat =
            render_scene_.get_material(material_it->second);
        auto metaFromParam = [&](const tr::ShaderParam &param) {
          TextureMeta meta;
          const tr::RenderTexture *texture = TextureAt(render_scene_,
                                                        param.texture_id);
          if (!texture) return meta;
          meta.path = texture->asset_path;
          meta.source_color_space = texture->source_color_space;
          meta.color_transform_valid = texture->color_transform_valid;
          meta.color_transform_bypass = texture->color_transform_bypass;
          meta.source_color_is_data = texture->source_color_is_data;
          meta.source_gamma = texture->source_gamma;
          meta.source_linear_bias = texture->source_linear_bias;
          std::copy(texture->source_to_display_linear,
                    texture->source_to_display_linear + 9,
                    meta.source_to_display_linear.begin());
          auto wrapName = [](tr::WrapMode mode) {
            switch (mode) {
              case tr::WrapMode::Repeat: return std::string("repeat");
              case tr::WrapMode::Mirror: return std::string("mirror");
              case tr::WrapMode::Black: return std::string("black");
              case tr::WrapMode::Clamp:
              default: return std::string("clamp");
            }
          };
          meta.wrap_s = wrapName(texture->wrap_s);
          meta.wrap_t = wrapName(texture->wrap_t);
          meta.is_udim = isUdimPath_(meta.path);
          return meta;
        };
        if (render_mat && render_mat->preview_surface) {
          const tr::PreviewSurfaceShader &ps = *render_mat->preview_surface;
          rec.base_color[0] = ps.diffuse_color.value.x;
          rec.base_color[1] = ps.diffuse_color.value.y;
          rec.base_color[2] = ps.diffuse_color.value.z;
          rec.metallic = ps.metallic.value.x;
          rec.roughness = ps.roughness.value.x;
          rec.opacity = ps.opacity.value.x;
          rec.occlusion = ps.occlusion.value.x;
          rec.emissive[0] = ps.emissive_color.value.x;
          rec.emissive[1] = ps.emissive_color.value.y;
          rec.emissive[2] = ps.emissive_color.value.z;
          rec.opacity_threshold = ps.opacity_threshold.value.x > 0.0f
                                      ? ps.opacity_threshold.value.x
                                      : -1.0f;
          rec.base_color_meta = metaFromParam(ps.diffuse_color);
          rec.normal_meta = metaFromParam(ps.normal);
          rec.roughness_meta = metaFromParam(ps.roughness);
          rec.metallic_meta = metaFromParam(ps.metallic);
          rec.occlusion_meta = metaFromParam(ps.occlusion);
          rec.emissive_meta = metaFromParam(ps.emissive_color);
          rec.opacity_meta = metaFromParam(ps.opacity);
          populated_from_render_scene = true;
        } else if (render_mat && render_mat->openpbr) {
          const tr::OpenPBRSurfaceShader &op = *render_mat->openpbr;
          rec.base_color[0] = op.base_color.value.x;
          rec.base_color[1] = op.base_color.value.y;
          rec.base_color[2] = op.base_color.value.z;
          rec.metallic = op.base_metalness.value.x;
          rec.roughness = op.specular_roughness.value.x;
          rec.opacity = op.opacity.value.x;
          rec.emissive[0] = op.emission_color.value.x;
          rec.emissive[1] = op.emission_color.value.y;
          rec.emissive[2] = op.emission_color.value.z;
          rec.base_color_meta = metaFromParam(op.base_color);
          rec.normal_meta = metaFromParam(op.normal);
          rec.roughness_meta = metaFromParam(
              op.specular_roughness.is_texture() ? op.specular_roughness
                                                 : op.base_roughness);
          rec.metallic_meta = metaFromParam(op.base_metalness);
          rec.emissive_meta = metaFromParam(op.emission_color);
          rec.opacity_meta = metaFromParam(op.opacity);
          populated_from_render_scene = true;
        }
        if (populated_from_render_scene) {
          rec.base_color_texture = rec.base_color_meta.path;
          rec.normal_texture = rec.normal_meta.path;
          rec.roughness_texture = rec.roughness_meta.path;
          rec.metallic_texture = rec.metallic_meta.path;
          rec.occlusion_texture = rec.occlusion_meta.path;
          rec.emissive_texture = rec.emissive_meta.path;
          rec.opacity_texture = rec.opacity_meta.path;
        }
      }
    }
    lightusd::next::UsdPrim shader;
    const std::string shaderPath = lightusd::next::GetSurfaceShader(stage_, mat);
    if (!shaderPath.empty()) shader = stage_.GetPrimAtPath(shaderPath);
    if (!shader.IsValid()) {
      for (const auto &ch : mat.GetChildren()) {
        if (lightusd::next::IsPreviewSurface(ch)) { shader = ch; break; }
      }
    }
    if (!populated_from_render_scene && shader.IsValid()) {
      lightusd::next::PreviewSurfaceData ps;
      if (lightusd::next::GetPreviewSurfaceData(stage_, shader, &ps)) {
        rec.base_color[0] = ps.diffuse_color[0];
        rec.base_color[1] = ps.diffuse_color[1];
        rec.base_color[2] = ps.diffuse_color[2];
        rec.metallic = ps.metallic;
        rec.roughness = ps.roughness;
        rec.opacity = ps.opacity;
        rec.occlusion = ps.occlusion;
        rec.emissive[0] = ps.emissive_color[0];
        rec.emissive[1] = ps.emissive_color[1];
        rec.emissive[2] = ps.emissive_color[2];
        rec.opacity_threshold = ps.opacity_threshold > 0.0f
                                    ? ps.opacity_threshold
                                    : -1.0f;
        rec.base_color_meta = texMeta_(ps.diffuse_texture);
        rec.normal_meta = texMeta_(ps.normal_texture);
        rec.roughness_meta = texMeta_(ps.roughness_texture);
        rec.metallic_meta = texMeta_(ps.metallic_texture);
        rec.occlusion_meta = texMeta_(ps.occlusion_texture);
        rec.emissive_meta = texMeta_(ps.emissive_texture);
        rec.opacity_meta = texMeta_(ps.opacity_texture);
        rec.base_color_texture = rec.base_color_meta.path;
        rec.normal_texture = rec.normal_meta.path;
        rec.roughness_texture = rec.roughness_meta.path;
        rec.metallic_texture = rec.metallic_meta.path;
        rec.occlusion_texture = rec.occlusion_meta.path;
        rec.emissive_texture = rec.emissive_meta.path;
        rec.opacity_texture = rec.opacity_meta.path;
      }
    }
    // A dual-terminal material commonly carries its alpha cutoff only on the
    // PreviewSurface fallback while MaterialX supplies the actual shading
    // graph. Preserve that cutoff even when the render catalog correctly chose
    // outputs:mtlx:surface above.
    if (rec.opacity_threshold <= 0.0f && shader.IsValid()) {
      lightusd::next::PreviewSurfaceData ps;
      if (lightusd::next::GetPreviewSurfaceData(stage_, shader, &ps) &&
          ps.opacity_threshold > 0.0f) {
        rec.opacity_threshold = ps.opacity_threshold;
      }
    }
    // The schema helper above intentionally models PreviewSurface only.
    // Pull evaluated OpenPBR values from the next render converter so
    // constant MaterialX node networks drive the Three.js fallback material.
    const auto render_material_it =
        render_scene_.material_by_path.find(rec.prim_path);
    if (render_material_it != render_scene_.material_by_path.end()) {
      const tr::RenderMaterial* render_material =
          render_scene_.get_material(render_material_it->second);
      if (render_material && render_material->openpbr) {
        const tr::OpenPBRSurfaceShader& openpbr = *render_material->openpbr;
        rec.base_color[0] = openpbr.base_color.value.x;
        rec.base_color[1] = openpbr.base_color.value.y;
        rec.base_color[2] = openpbr.base_color.value.z;
        rec.metallic = openpbr.base_metalness.value.x;
        rec.roughness = openpbr.specular_roughness.value.x;
        rec.opacity = openpbr.opacity.value.x;
        const float emission = openpbr.emission_luminance.value.x;
        rec.emissive[0] = openpbr.emission_color.value.x * emission;
        rec.emissive[1] = openpbr.emission_color.value.y * emission;
        rec.emissive[2] = openpbr.emission_color.value.z * emission;
        rec.base_color_texture = TexturePath(render_scene_, openpbr.base_color);
        rec.normal_texture = TexturePath(render_scene_, openpbr.normal);
        rec.roughness_texture =
            TexturePath(render_scene_, openpbr.specular_roughness);
        rec.metallic_texture =
            TexturePath(render_scene_, openpbr.base_metalness);
        rec.emissive_texture =
            TexturePath(render_scene_, openpbr.emission_color);
        rec.opacity_texture = TexturePath(render_scene_, openpbr.opacity);
      }
    }
    (void)populateHairMaterial_(mat, &rec);
    std::ostringstream ss;
    appendMaterialKey_(rec, &ss);
    rec.key = ss.str();
    return rec;
  }

int32_t RenderStream::registerMaterial_(const lightusd::next::UsdPrim &mat) {
    const std::string mat_path = mat.IsValid() ? mat.GetPath().str()
                                               : std::string("__default");
    const auto path_it = material_path_to_id_.find(mat_path);
    if (path_it != material_path_to_id_.end()) return path_it->second;

    const double identity_start_ms = emscripten_get_now();
    const std::string source_identity =
        material_dedup_ ? materialSourceIdentity_(mat) : std::string();
    stats_.material_identity_ms += emscripten_get_now() - identity_start_ms;
    const bool graph_identity = source_identity.rfind("mtlx:", 0) == 0 ||
                                source_identity.rfind("preview:", 0) == 0;
    const auto identity_it = material_identity_to_id_.find(source_identity);
    if (!source_identity.empty() &&
        identity_it != material_identity_to_id_.end()) {
      stats_.material_identity_hits++;
      if (graph_identity) stats_.material_graph_cache_hits++;
      source_material_keys_.insert(mat_path);
      material_path_to_id_[mat_path] = identity_it->second;
      return identity_it->second;
    }
    if (!source_identity.empty()) {
      stats_.material_identity_misses++;
      if (graph_identity) stats_.material_graph_cache_misses++;
    }

    const double conversion_start_ms = emscripten_get_now();
    MaterialRecord rec = materialRecordForPrim_(mat);
    stats_.material_conversion_ms += emscripten_get_now() - conversion_start_ms;
    source_material_keys_.insert(mat_path);
    addTextureKey_("color", rec.base_color_texture, &source_texture_keys_);
    addTextureKey_("data", rec.normal_texture, &source_texture_keys_);
    addTextureKey_("data", rec.roughness_texture, &source_texture_keys_);
    addTextureKey_("data", rec.metallic_texture, &source_texture_keys_);
    addTextureKey_("data", rec.occlusion_texture, &source_texture_keys_);
    addTextureKey_("color", rec.emissive_texture, &source_texture_keys_);
    if (rec.opacity_texture != rec.base_color_texture) {
      addTextureKey_("data", rec.opacity_texture, &source_texture_keys_);
    }

    const std::string key = material_dedup_ ? rec.key : mat_path;
    auto it = material_key_to_id_.find(key);
    if (it != material_key_to_id_.end()) {
      material_path_to_id_[mat_path] = it->second;
      if (!source_identity.empty()) {
        material_identity_to_id_[source_identity] = it->second;
      }
      return it->second;
    }
    rec.id = static_cast<int32_t>(materials_.size());
    rec.key = key;
    materials_.push_back(rec);
    material_key_to_id_[key] = rec.id;
    material_path_to_id_[mat_path] = rec.id;
    if (!source_identity.empty()) {
      material_identity_to_id_[source_identity] = rec.id;
    }
    addTextureKey_("color", rec.base_color_texture, &texture_keys_);
    addTextureKey_("data", rec.normal_texture, &texture_keys_);
    addTextureKey_("data", rec.roughness_texture, &texture_keys_);
    addTextureKey_("data", rec.metallic_texture, &texture_keys_);
    addTextureKey_("data", rec.occlusion_texture, &texture_keys_);
    addTextureKey_("color", rec.emissive_texture, &texture_keys_);
    if (rec.opacity_texture != rec.base_color_texture) {
      addTextureKey_("data", rec.opacity_texture, &texture_keys_);
    }
    return rec.id;
  }

std::string RenderStream::texFile_(const std::string &connPath) {
    if (connPath.empty()) return "";
    const size_t slash = connPath.rfind('/');
    const size_t dot = connPath.find('.', slash == std::string::npos ? 0 : slash);
    const std::string primPath = (dot == std::string::npos) ? connPath : connPath.substr(0, dot);
    lightusd::next::UsdPrim tex = stage_.GetPrimAtPath(primPath);
    if (!tex.IsValid()) return "";
    const lightusd::next::Value *v = tex.GetPropertyValue("inputs:file");
    if (!v) return "";
    if (const std::string *a = v->as_asset_path()) return *a;
    if (const std::string *s = v->as_string()) return *s;
    return "";
  }
}  // namespace web_next
}  // namespace lightusd
