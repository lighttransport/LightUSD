// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Render material and texture identity deduplication.

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tydra/render-data.hh"

namespace lightusd {
namespace tydra {
namespace {

static void AppendFloat(std::ostringstream &ss, const float v) {
  ss << std::setprecision(std::numeric_limits<float>::max_digits10) << v;
}

static void AppendVec2(std::ostringstream &ss, const vec2 &v) {
  AppendFloat(ss, v[0]);
  ss << ",";
  AppendFloat(ss, v[1]);
}

static void AppendVec3(std::ostringstream &ss, const vec3 &v) {
  AppendFloat(ss, v[0]);
  ss << ",";
  AppendFloat(ss, v[1]);
  ss << ",";
  AppendFloat(ss, v[2]);
}

static void AppendVec4(std::ostringstream &ss, const vec4 &v) {
  AppendFloat(ss, v[0]);
  ss << ",";
  AppendFloat(ss, v[1]);
  ss << ",";
  AppendFloat(ss, v[2]);
  ss << ",";
  AppendFloat(ss, v[3]);
}

static void AppendMat3(std::ostringstream &ss, const mat3 &m) {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      if (r || c) {
        ss << ",";
      }
      AppendFloat(ss, m.m[r][c]);
    }
  }
}

static void AppendValue(std::ostringstream &ss, const float v) {
  AppendFloat(ss, v);
}

static void AppendValue(std::ostringstream &ss, const vec3 &v) {
  AppendVec3(ss, v);
}

static void AppendTextureSignature(
    std::ostringstream &ss, const int32_t texture_id,
    const std::vector<UVTexture> &textures) {
  if (texture_id < 0 || size_t(texture_id) >= textures.size()) {
    ss << "tex:-1";
    return;
  }

  const UVTexture &t = textures[size_t(texture_id)];
  ss << "tex:image=" << t.texture_image_id
     << ",wrap=" << to_string(t.wrapS) << "/" << to_string(t.wrapT)
     << ",udim=" << (t.is_udim ? 1 : 0)
     << ",udimId=" << t.udim_texture_id
     << ",udimScale=";
  AppendVec2(ss, t.udim_uv_scale);
  ss << ",udimOffset=";
  AppendVec2(ss, t.udim_uv_offset);
  ss << ",scale=";
  AppendVec4(ss, t.scale);
  ss << ",bias=";
  AppendVec4(ss, t.bias);
  ss << ",channel=" << to_string(t.connectedOutputChannel)
     << ",varname=" << t.varname_uv
     << ",hasXform=" << (t.has_transform2d ? 1 : 0)
     << ",xform=";
  AppendMat3(ss, t.transform);
}

static std::string TextureSignature(const UVTexture &texture) {
  std::ostringstream ss;
  ss << "tex:image=" << texture.texture_image_id
     << ",wrap=" << to_string(texture.wrapS) << "/" << to_string(texture.wrapT)
     << ",udim=" << (texture.is_udim ? 1 : 0)
     << ",udimId=" << texture.udim_texture_id
     << ",udimScale=";
  AppendVec2(ss, texture.udim_uv_scale);
  ss << ",udimOffset=";
  AppendVec2(ss, texture.udim_uv_offset);
  ss << ",scale=";
  AppendVec4(ss, texture.scale);
  ss << ",bias=";
  AppendVec4(ss, texture.bias);
  ss << ",channel=" << to_string(texture.connectedOutputChannel)
     << ",varname=" << texture.varname_uv
     << ",hasXform=" << (texture.has_transform2d ? 1 : 0)
     << ",xform=";
  AppendMat3(ss, texture.transform);
  return ss.str();
}

template <typename T>
static void AppendShaderParam(std::ostringstream &ss, const char *name,
                              const ShaderParam<T> &param,
                              const std::vector<UVTexture> &textures) {
  ss << name << "=";
  AppendValue(ss, param.value);
  ss << ";";
  ss << name << "Texture=";
  AppendTextureSignature(ss, param.texture_id, textures);
  ss << ";";
}

static std::string MaterialSignature(
    const RenderMaterial &mat, const std::vector<UVTexture> &textures) {
  std::ostringstream ss;
  ss << "tag=" << int(mat.materialTag) << ";";
  ss << "disp=" << (mat.has_displacement ? 1 : 0) << ";";
  ss << "volume=" << (mat.has_volume ? 1 : 0) << ";";
  ss << "mtlxConfig=" << (mat.materialXConfig.authored ? 1 : 0) << ";"
     << mat.materialXConfig.version << ";"
     << mat.materialXConfig.name_space << ";"
     << mat.materialXConfig.colorspace << ";"
     << mat.materialXConfig.source_uri << ";";
  if (mat.surfaceShader.has_value()) {
    const PreviewSurfaceShader &s = *mat.surfaceShader;
    ss << "preview{";
    ss << "useSpecularWorkflow=" << (s.useSpecularWorkflow ? 1 : 0) << ";";
    AppendShaderParam(ss, "diffuseColor", s.diffuseColor, textures);
    AppendShaderParam(ss, "emissiveColor", s.emissiveColor, textures);
    AppendShaderParam(ss, "specularColor", s.specularColor, textures);
    AppendShaderParam(ss, "metallic", s.metallic, textures);
    AppendShaderParam(ss, "roughness", s.roughness, textures);
    AppendShaderParam(ss, "clearcoat", s.clearcoat, textures);
    AppendShaderParam(ss, "clearcoatRoughness", s.clearcoatRoughness,
                      textures);
    AppendShaderParam(ss, "opacity", s.opacity, textures);
    AppendShaderParam(ss, "opacityThreshold", s.opacityThreshold, textures);
    AppendShaderParam(ss, "ior", s.ior, textures);
    AppendShaderParam(ss, "normal", s.normal, textures);
    AppendShaderParam(ss, "displacement", s.displacement, textures);
    AppendShaderParam(ss, "occlusion", s.occlusion, textures);
    ss << "}";
  }
  if (mat.openPBRShader.has_value()) {
    const OpenPBRSurfaceShader &s = *mat.openPBRShader;
    ss << "openpbr{";
#define LIGHTUSD_APPEND_OPENPBR_PARAM(name) \
    AppendShaderParam(ss, #name, s.name, textures)
    LIGHTUSD_APPEND_OPENPBR_PARAM(base_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(base_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(base_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(base_metalness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(base_diffuse_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_ior);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_ior_level);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_rotation);
    LIGHTUSD_APPEND_OPENPBR_PARAM(specular_roughness_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_depth);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_scatter);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_scatter_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_dispersion);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_dispersion_abbe_number);
    LIGHTUSD_APPEND_OPENPBR_PARAM(transmission_dispersion_scale);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_radius);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_radius_scale);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_scale);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(subsurface_scatter_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(sheen_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(sheen_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(sheen_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(fuzz_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(fuzz_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(fuzz_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(thin_film_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(thin_film_thickness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(thin_film_ior);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_weight);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_rotation);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_ior);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_affect_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_affect_roughness);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_roughness_anisotropy);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_darkening);
    LIGHTUSD_APPEND_OPENPBR_PARAM(emission_luminance);
    LIGHTUSD_APPEND_OPENPBR_PARAM(emission_color);
    LIGHTUSD_APPEND_OPENPBR_PARAM(opacity);
    LIGHTUSD_APPEND_OPENPBR_PARAM(normal);
    LIGHTUSD_APPEND_OPENPBR_PARAM(tangent);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_normal);
    LIGHTUSD_APPEND_OPENPBR_PARAM(coat_tangent);
    LIGHTUSD_APPEND_OPENPBR_PARAM(displacement);
#undef LIGHTUSD_APPEND_OPENPBR_PARAM
    ss << "tangentRotation=";
    AppendFloat(ss, s.tangent_rotation);
    ss << ";normalMapScale=";
    AppendFloat(ss, s.normal_map_scale);
    ss << ";coatTangentRotation=";
    AppendFloat(ss, s.coat_tangent_rotation);
    ss << ";coatNormalMapScale=";
    AppendFloat(ss, s.coat_normal_map_scale);
    ss << ";nodeGraph=" << s.nodeGraphJson;
    ss << "}";
  }
  return ss.str();
}

static void RemapMaterialId(int &id, const std::vector<int> &remap) {
  if (id >= 0 && size_t(id) < remap.size()) {
    id = remap[size_t(id)];
  }
}

static void RemapMaterialSubsetIds(MaterialSubset &subset,
                                   const std::vector<int> &remap) {
  RemapMaterialId(subset.material_id, remap);
  RemapMaterialId(subset.backface_material_id, remap);
}

template <typename T>
static void RemapShaderParamTexture(ShaderParam<T> &param,
                                    const std::vector<int> &remap) {
  if (param.texture_id >= 0 && size_t(param.texture_id) < remap.size()) {
    param.texture_id = remap[size_t(param.texture_id)];
  }
}

static void RemapMaterialTextureIds(RenderMaterial &mat,
                                    const std::vector<int> &remap) {
  if (mat.surfaceShader.has_value()) {
    PreviewSurfaceShader &s = *mat.surfaceShader;
    RemapShaderParamTexture(s.diffuseColor, remap);
    RemapShaderParamTexture(s.emissiveColor, remap);
    RemapShaderParamTexture(s.specularColor, remap);
    RemapShaderParamTexture(s.metallic, remap);
    RemapShaderParamTexture(s.roughness, remap);
    RemapShaderParamTexture(s.clearcoat, remap);
    RemapShaderParamTexture(s.clearcoatRoughness, remap);
    RemapShaderParamTexture(s.opacity, remap);
    RemapShaderParamTexture(s.opacityThreshold, remap);
    RemapShaderParamTexture(s.ior, remap);
    RemapShaderParamTexture(s.normal, remap);
    RemapShaderParamTexture(s.displacement, remap);
    RemapShaderParamTexture(s.occlusion, remap);
  }

  if (mat.openPBRShader.has_value()) {
    OpenPBRSurfaceShader &s = *mat.openPBRShader;
#define LIGHTUSD_REMAP_OPENPBR_PARAM(name) RemapShaderParamTexture(s.name, remap)
    LIGHTUSD_REMAP_OPENPBR_PARAM(base_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(base_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(base_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(base_metalness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(base_diffuse_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_ior);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_ior_level);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_rotation);
    LIGHTUSD_REMAP_OPENPBR_PARAM(specular_roughness_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_depth);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_scatter);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_scatter_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_dispersion);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_dispersion_abbe_number);
    LIGHTUSD_REMAP_OPENPBR_PARAM(transmission_dispersion_scale);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_radius);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_radius_scale);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_scale);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(subsurface_scatter_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(sheen_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(sheen_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(sheen_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(fuzz_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(fuzz_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(fuzz_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(thin_film_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(thin_film_thickness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(thin_film_ior);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_weight);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_rotation);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_ior);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_affect_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_affect_roughness);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_roughness_anisotropy);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_darkening);
    LIGHTUSD_REMAP_OPENPBR_PARAM(emission_luminance);
    LIGHTUSD_REMAP_OPENPBR_PARAM(emission_color);
    LIGHTUSD_REMAP_OPENPBR_PARAM(opacity);
    LIGHTUSD_REMAP_OPENPBR_PARAM(normal);
    LIGHTUSD_REMAP_OPENPBR_PARAM(tangent);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_normal);
    LIGHTUSD_REMAP_OPENPBR_PARAM(coat_tangent);
    LIGHTUSD_REMAP_OPENPBR_PARAM(displacement);
#undef LIGHTUSD_REMAP_OPENPBR_PARAM
  }
}

}  // namespace

size_t RenderSceneConverter::DeduplicateMaterialsByTextureIdentityImpl() {
  if (materials.empty()) {
    return 0;
  }

  const size_t before = materials.size();
  std::unordered_map<std::string, int> signature_to_new_id;
  signature_to_new_id.reserve(before);
  std::vector<int> old_to_new(before, -1);
  std::vector<RenderMaterial> deduped;
  deduped.reserve(before);

  for (size_t i = 0; i < before; i++) {
    const RenderMaterial &mat = materials[i];
    const std::string signature = MaterialSignature(mat, textures);
    auto it = signature_to_new_id.find(signature);
    if (it != signature_to_new_id.end()) {
      old_to_new[i] = it->second;
      continue;
    }

    const int new_id = int(deduped.size());
    signature_to_new_id.emplace(signature, new_id);
    old_to_new[i] = new_id;
    deduped.push_back(mat);
  }

  if (deduped.size() == before) {
    return 0;
  }

  for (RenderMesh &mesh : meshes) {
    RemapMaterialId(mesh.material_id, old_to_new);
    RemapMaterialId(mesh.backface_material_id, old_to_new);
    for (auto &subset : mesh.material_subsetMap) {
      RemapMaterialSubsetIds(subset.second, old_to_new);
    }
  }

  for (RenderInstance &inst : instances) {
    RemapMaterialId(inst.material_id, old_to_new);
  }

  materials = std::move(deduped);
  // Note: materialMap (path -> material index) is left stale after dedup. It is
  // a converter-internal cache consulted only during material conversion (which
  // completes before this pass) and is not exported into RenderScene, so the
  // stale entries are never read. All live references (mesh/instance material
  // ids, subsets) are remapped above.
  return before - materials.size();
}

size_t RenderSceneConverter::DeduplicateTexturesByIdentityImpl() {
  if (textures.empty()) {
    return 0;
  }

  const size_t before = textures.size();
  std::unordered_map<std::string, int> signature_to_new_id;
  signature_to_new_id.reserve(before);
  std::vector<int> old_to_new(before, -1);
  std::vector<UVTexture> deduped;
  deduped.reserve(before);

  for (size_t i = 0; i < before; i++) {
    const UVTexture &texture = textures[i];
    const std::string signature = TextureSignature(texture);
    auto it = signature_to_new_id.find(signature);
    if (it != signature_to_new_id.end()) {
      old_to_new[i] = it->second;
      continue;
    }

    const int new_id = int(deduped.size());
    signature_to_new_id.emplace(signature, new_id);
    old_to_new[i] = new_id;
    deduped.push_back(texture);
  }

  if (deduped.size() == before) {
    return 0;
  }

  for (RenderMaterial &mat : materials) {
    RemapMaterialTextureIds(mat, old_to_new);
  }

  textures = std::move(deduped);
  return before - textures.size();
}


}  // namespace tydra
}  // namespace lightusd
