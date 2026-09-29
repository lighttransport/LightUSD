// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "binding-next-scene.hh"
#include "next/schema/usd-shade.hh"
#include "tydra/material-serializer.hh"
namespace lightusd {
namespace web_next {
namespace {
int CopyOutputString(const std::string& value, uint8_t* out, uint32_t cap) {
  if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
  const int required = static_cast<int>(value.size());
  if (out && cap >= value.size() && !value.empty())
    std::memcpy(out, value.data(), value.size());
  return required;
}

using NextRenderMaterial = lightusd::tydra::next::RenderMaterial;
using NextShaderParam = lightusd::tydra::next::ShaderParam;

static void CopyMaterialParam(lightusd::tydra::ShaderParam<float>& dst,
                              const NextShaderParam& src) {
  dst.value = src.value.x;
  dst.texture_id = src.texture_id;
}

static void CopyMaterialParam(lightusd::tydra::ShaderParam<lightusd::tydra::vec3>& dst,
                              const NextShaderParam& src) {
  dst.value[0] = src.value.x;
  dst.value[1] = src.value.y;
  dst.value[2] = src.value.z;
  dst.texture_id = src.texture_id;
}

static lightusd::tydra::RenderMaterial ToLegacyMaterial(
    const NextRenderMaterial& src, const lightusd::next::Stage& stage) {
  lightusd::tydra::RenderMaterial dst;
  dst.name = src.name;
  dst.abs_path = src.prim_path;
  const lightusd::next::UsdPrim prim = stage.GetPrimAtPath(src.prim_path);
  const lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  if (spec && !spec->meta().displayName().empty())
    dst.display_name = spec->meta().displayName();
  dst.materialXConfig.authored = src.mtlx_config.authored;
  dst.materialXConfig.version = src.mtlx_config.version;
  dst.materialXConfig.name_space = src.mtlx_config.name_space;
  dst.materialXConfig.colorspace = src.mtlx_config.colorspace;
  dst.materialXConfig.source_uri = src.mtlx_config.source_uri;

  if (src.preview_surface) {
    lightusd::tydra::PreviewSurfaceShader shader;
    shader.useSpecularWorkflow = src.preview_surface->use_specular_workflow;
#define COPY_PREVIEW_PARAM(field, next_field) \
    CopyMaterialParam(shader.field, src.preview_surface->next_field)
    COPY_PREVIEW_PARAM(diffuseColor, diffuse_color);
    COPY_PREVIEW_PARAM(emissiveColor, emissive_color);
    COPY_PREVIEW_PARAM(specularColor, specular_color);
    COPY_PREVIEW_PARAM(metallic, metallic);
    COPY_PREVIEW_PARAM(roughness, roughness);
    COPY_PREVIEW_PARAM(clearcoat, clearcoat);
    COPY_PREVIEW_PARAM(clearcoatRoughness, clearcoat_roughness);
    COPY_PREVIEW_PARAM(opacity, opacity);
    COPY_PREVIEW_PARAM(opacityThreshold, opacity_threshold);
    COPY_PREVIEW_PARAM(ior, ior);
    COPY_PREVIEW_PARAM(normal, normal);
    COPY_PREVIEW_PARAM(displacement, displacement);
    COPY_PREVIEW_PARAM(occlusion, occlusion);
#undef COPY_PREVIEW_PARAM
    dst.surfaceShader = std::move(shader);
  }
  if (src.openpbr) {
    lightusd::tydra::OpenPBRSurfaceShader shader;
#define COPY_OPENPBR_PARAM(field) CopyMaterialParam(shader.field, src.openpbr->field)
#define COPY_OPENPBR_VEC3(field) CopyMaterialParam(shader.field, src.openpbr->field)
    COPY_OPENPBR_PARAM(base_weight); COPY_OPENPBR_VEC3(base_color);
    // The legacy converter exposes Standard Surface diffuse roughness through
    // both fields in its serialized OpenPBR payload.
    CopyMaterialParam(shader.base_roughness, src.openpbr->base_diffuse_roughness);
    COPY_OPENPBR_PARAM(base_diffuse_roughness);
    COPY_OPENPBR_PARAM(base_metalness);
    COPY_OPENPBR_PARAM(specular_weight);
    COPY_OPENPBR_VEC3(specular_color); COPY_OPENPBR_PARAM(specular_roughness);
    COPY_OPENPBR_PARAM(specular_ior); COPY_OPENPBR_PARAM(specular_anisotropy);
    COPY_OPENPBR_PARAM(specular_rotation); COPY_OPENPBR_PARAM(transmission_weight);
    COPY_OPENPBR_VEC3(transmission_color); COPY_OPENPBR_PARAM(transmission_depth);
    COPY_OPENPBR_VEC3(transmission_scatter);
    COPY_OPENPBR_PARAM(transmission_scatter_anisotropy);
    COPY_OPENPBR_PARAM(transmission_dispersion); COPY_OPENPBR_PARAM(subsurface_weight);
    COPY_OPENPBR_VEC3(subsurface_color); COPY_OPENPBR_PARAM(subsurface_scale);
    COPY_OPENPBR_PARAM(subsurface_anisotropy);
    if (!src.openpbr->fuzz_authored) {
      COPY_OPENPBR_PARAM(sheen_weight);
      COPY_OPENPBR_VEC3(sheen_color); COPY_OPENPBR_PARAM(sheen_roughness);
    }
    COPY_OPENPBR_PARAM(fuzz_weight); COPY_OPENPBR_VEC3(fuzz_color);
    COPY_OPENPBR_PARAM(fuzz_roughness); COPY_OPENPBR_PARAM(thin_film_weight);
    COPY_OPENPBR_PARAM(thin_film_thickness); COPY_OPENPBR_PARAM(thin_film_ior);
    COPY_OPENPBR_PARAM(coat_weight); COPY_OPENPBR_VEC3(coat_color);
    COPY_OPENPBR_PARAM(coat_roughness); COPY_OPENPBR_PARAM(coat_rotation);
    COPY_OPENPBR_PARAM(coat_anisotropy);
    COPY_OPENPBR_PARAM(coat_ior); COPY_OPENPBR_PARAM(coat_affect_color);
    COPY_OPENPBR_PARAM(coat_affect_roughness);
    COPY_OPENPBR_PARAM(emission_luminance); COPY_OPENPBR_VEC3(emission_color);
    COPY_OPENPBR_PARAM(opacity); COPY_OPENPBR_VEC3(normal); COPY_OPENPBR_VEC3(tangent);
    shader.tangent_rotation = src.openpbr->tangent_rotation;
    shader.normal_map_scale = src.openpbr->normal_map_scale;
    COPY_OPENPBR_VEC3(coat_normal); COPY_OPENPBR_VEC3(coat_tangent);
    shader.coat_tangent_rotation = src.openpbr->coat_tangent_rotation;
    shader.coat_normal_map_scale = src.openpbr->coat_normal_map_scale;
    shader.nodeGraphJson = src.openpbr->nodegraph_json;
#undef COPY_OPENPBR_PARAM
#undef COPY_OPENPBR_VEC3
    dst.openPBRShader = std::move(shader);
  }
  return dst;
}

static std::string LegacyPreviewMaterialJSON(
    const lightusd::tydra::RenderMaterial& material) {
  minijson::Value result = minijson::Value::object();
  minijson::Value config = minijson::Value::object();
  config["authored"] = material.materialXConfig.authored;
  config["version"] = material.materialXConfig.version.empty()
      ? "1.38" : material.materialXConfig.version;
  config["namespace"] = material.materialXConfig.name_space;
  config["colorspace"] = material.materialXConfig.colorspace.empty()
      ? "lin_rec709" : material.materialXConfig.colorspace;
  config["sourceUri"] = material.materialXConfig.source_uri;
  result["materialXConfig"] = std::move(config);
  if (!material.hasUsdPreviewSurface()) {
    result["error"] = "Material does not have UsdPreviewSurface shader";
    return result.dump();
  }
  const auto& shader = *material.surfaceShader;
  result["useSpecularWorkflow"] = shader.useSpecularWorkflow;
  const auto add_scalar = [&result](const char* name, const auto& param) {
    result[name] = param.value;
    if (param.is_texture())
      result[std::string(name) + "TextureId"] = param.texture_id;
  };
  const auto add_vector = [&result](const char* name, const auto& param) {
    minijson::Value value = minijson::Value::array();
    for (size_t i = 0; i < 3; ++i) value.push_back(param.value[i]);
    result[name] = std::move(value);
    if (param.is_texture())
      result[std::string(name) + "TextureId"] = param.texture_id;
  };
  add_vector("diffuseColor", shader.diffuseColor);
  add_vector("emissiveColor", shader.emissiveColor);
  if (shader.useSpecularWorkflow) add_vector("specularColor", shader.specularColor);
  else add_scalar("metallic", shader.metallic);
  add_scalar("roughness", shader.roughness);
  add_scalar("clearcoat", shader.clearcoat);
  add_scalar("clearcoatRoughness", shader.clearcoatRoughness);
  add_scalar("opacity", shader.opacity);
  add_scalar("opacityThreshold", shader.opacityThreshold);
  add_scalar("ior", shader.ior);
  add_vector("normal", shader.normal);
  add_scalar("displacement", shader.displacement);
  add_scalar("occlusion", shader.occlusion);
  return result.dump();
}

static lightusd::tydra::RenderScene ToLegacyMaterialScene(
    const lightusd::tydra::next::RenderScene& src) {
  lightusd::tydra::RenderScene dst;
  const auto legacy_color_space = [](const std::string& name,
                                     lightusd::tydra::ColorSpace fallback) {
    if (name == "sRGB" || name == "srgb") return lightusd::tydra::ColorSpace::sRGB;
    if (name == "raw" || name == "Raw") return lightusd::tydra::ColorSpace::Raw;
    if (name == "rec709" || name == "Rec709") return lightusd::tydra::ColorSpace::Rec709;
    if (name == "lin_rec709" || name == "lin_rec709_scene")
      return lightusd::tydra::ColorSpace::Lin_Rec709;
    if (name == "lin_acescg" || name == "acescg")
      return lightusd::tydra::ColorSpace::Lin_ACEScg;
    if (name == "lin_rec2020" || name == "rec2020")
      return lightusd::tydra::ColorSpace::Lin_Rec2020;
    if (name == "lin_displayp3" || name == "displayp3")
      return lightusd::tydra::ColorSpace::Lin_DisplayP3;
    if (name == "srgb_texture") return lightusd::tydra::ColorSpace::sRGB_Texture;
    return fallback;
  };
  for (const auto& image : src.images) {
    lightusd::tydra::TextureImage converted;
    converted.asset_identifier = image.resolved_path;
    converted.width = static_cast<int32_t>(image.width);
    converted.height = static_cast<int32_t>(image.height);
    converted.channels = image.channels;
    converted.decoded = image.is_loaded();
    switch (image.color_space) {
      case lightusd::tydra::next::ColorSpace::sRGB:
        converted.colorSpace = lightusd::tydra::ColorSpace::sRGB; break;
      case lightusd::tydra::next::ColorSpace::Linear:
        converted.colorSpace = lightusd::tydra::ColorSpace::Lin_Rec709; break;
      case lightusd::tydra::next::ColorSpace::Raw:
        converted.colorSpace = lightusd::tydra::ColorSpace::Raw; break;
      case lightusd::tydra::next::ColorSpace::ACEScg:
        converted.colorSpace = lightusd::tydra::ColorSpace::Lin_ACEScg; break;
      case lightusd::tydra::next::ColorSpace::Rec709:
        converted.colorSpace = lightusd::tydra::ColorSpace::Rec709; break;
      case lightusd::tydra::next::ColorSpace::Rec2020:
        converted.colorSpace = lightusd::tydra::ColorSpace::Lin_Rec2020; break;
      case lightusd::tydra::next::ColorSpace::DisplayP3:
        converted.colorSpace = lightusd::tydra::ColorSpace::Lin_DisplayP3; break;
      default: converted.colorSpace = lightusd::tydra::ColorSpace::Unknown; break;
    }
    converted.usdColorSpace = converted.colorSpace;
    dst.images.push_back(std::move(converted));
  }
  for (const auto& texture : src.textures) {
    lightusd::tydra::UVTexture converted;
    converted.texture_image_id = texture.image_id;
    dst.textures.push_back(std::move(converted));
    if (texture.image_id >= 0 &&
        static_cast<size_t>(texture.image_id) < dst.images.size()) {
      auto& image = dst.images[static_cast<size_t>(texture.image_id)];
      image.sourceColorSpaceName = texture.source_color_space;
      image.usdColorSpace = legacy_color_space(texture.source_color_space,
                                               image.colorSpace);
    }
  }
  return dst;
}

static lightusd::tydra::RenderLight ToLegacyLight(
    const lightusd::tydra::next::RenderLight& src,
    const lightusd::tydra::next::RenderScene& scene) {
  using NextType = lightusd::tydra::next::LightType;
  using Legacy = lightusd::tydra::RenderLight;
  Legacy dst;
  dst.name = src.name;
  dst.abs_path = src.prim_path;
  switch (src.type) {
    case NextType::Directional: dst.type = Legacy::Type::Distant; break;
    case NextType::Spot: dst.type = Legacy::Type::Sphere; break;
    case NextType::Rect: dst.type = Legacy::Type::Rect; break;
    case NextType::Disk: dst.type = Legacy::Type::Disk; break;
    case NextType::Dome: dst.type = Legacy::Type::Dome; break;
    case NextType::Sphere: dst.type = Legacy::Type::Sphere; break;
    case NextType::Cylinder: dst.type = Legacy::Type::Cylinder; break;
    case NextType::Geometry: dst.type = Legacy::Type::Geometry; break;
    default: dst.type = Legacy::Type::Point; break;
  }
  dst.color = {src.color.x, src.color.y, src.color.z};
  dst.intensity = src.intensity;
  dst.exposure = src.exposure;
  dst.normalize = src.normalize;
  dst.enableColorTemperature = src.enable_color_temperature;
  dst.colorTemperature = src.color_temperature;
  dst.diffuse = src.diffuse;
  dst.specular = src.specular;
  // Distant lights do not use the shaping API in the legacy render record;
  // its serializer therefore reports the RenderLight default.
  dst.shapingConeAngle = src.type == NextType::Directional
      ? 90.0f : src.shaping_cone_angle;
  dst.shapingFocus = src.shaping_focus;
  dst.shapingFocusTint = {src.shaping_focus_tint.x, src.shaping_focus_tint.y,
                          src.shaping_focus_tint.z};
  dst.shapingConeSoftness = src.shaping_cone_softness;
  dst.shapingIesFile = src.shaping_ies_file;
  dst.shapingIesAngleScale = src.shaping_ies_angle_scale;
  dst.shapingIesNormalize = src.shaping_ies_normalize;
  dst.textureFile = src.texture_file;
  dst.shadowEnable = src.enable_shadow;
  dst.shadowColor = {src.shadow_color.x, src.shadow_color.y, src.shadow_color.z};
  dst.shadowDistance = src.shadow_distance;
  dst.shadowFalloff = src.shadow_falloff;
  dst.shadowFalloffGamma = src.shadow_falloff_gamma;
  switch (src.type) {
    case NextType::Directional:
      dst.angle = src.params.distant.angle;
      break;
    case NextType::Rect:
      dst.width = src.params.rect.width;
      dst.height = src.params.rect.height;
      break;
    case NextType::Disk:
      dst.radius = src.params.disk.radius;
      break;
    case NextType::Dome:
      dst.domeTextureFormat = static_cast<Legacy::DomeTextureFormat>(
          src.params.dome.texture_format);
      dst.guideRadius = src.guide_radius;
      if (src.params.dome.texture_id >= 0 &&
          static_cast<size_t>(src.params.dome.texture_id) < scene.images.size()) {
        dst.textureFile = scene.images[static_cast<size_t>(
            src.params.dome.texture_id)].resolved_path;
      }
      break;
    case NextType::Sphere:
      dst.radius = src.params.sphere.radius;
      break;
    case NextType::Cylinder:
      dst.radius = src.params.cylinder.radius;
      dst.length = src.params.cylinder.length;
      break;
    case NextType::Spot:
      dst.radius = src.spot_source_radius;
      break;
    default:
      break;
  }
  return dst;
}
}

const tr::RenderMaterial* RenderStream::outputRenderMaterial_(int material_id) const {
  if (material_id < 0 || static_cast<size_t>(material_id) >= materials_.size())
    return nullptr;
  const auto it = render_scene_.material_by_path.find(
      materials_[static_cast<size_t>(material_id)].prim_path);
  return it == render_scene_.material_by_path.end()
             ? nullptr : render_scene_.get_material(it->second);
}

const RenderStream::TextureMeta* RenderStream::outputTextureMeta_(
    int material_id, uint8_t slot) const {
  if (material_id < 0 || static_cast<size_t>(material_id) >= materials_.size())
    return nullptr;
  const MaterialRecord& rec = materials_[static_cast<size_t>(material_id)];
  switch (slot) {
    case 0: return &rec.base_color_meta;
    case 1: return &rec.normal_meta;
    case 2: return &rec.roughness_meta;
    case 3: return &rec.metallic_meta;
    case 4: return &rec.occlusion_meta;
    case 5: return &rec.emissive_meta;
    case 6: return &rec.opacity_meta;
    default: return nullptr;
  }
}

int RenderStream::outputMaterialInfo(
    int material_id, lightusd_next_output_material_info* out) const {
  if (!out || out->struct_size < sizeof(*out) || material_id < 0 ||
      static_cast<size_t>(material_id) >= materials_.size()) return -1;
  const uint32_t requested_size = out->struct_size;
  std::memset(out, 0, sizeof(*out));
  out->struct_size = requested_size;
  const MaterialRecord& rec = materials_[static_cast<size_t>(material_id)];
  out->id = rec.id;
  const tr::RenderMaterial* render_mat = outputRenderMaterial_(material_id);
  if (render_mat) {
    out->flags |= 1u;
    if (render_mat->mtlx_config.authored) out->flags |= 2u;
    std::copy(render_scene_.working_to_display_linear,
              render_scene_.working_to_display_linear + 9, out->value + 11);
  }
  if (rec.has_hair) out->flags |= 4u;
  std::copy(rec.base_color, rec.base_color + 3, out->value);
  out->value[3] = rec.metallic;
  out->value[4] = rec.roughness;
  out->value[5] = rec.opacity;
  out->value[6] = rec.occlusion;
  std::copy(rec.emissive, rec.emissive + 3, out->value + 7);
  out->value[10] = rec.opacity_threshold;
  std::copy(rec.hair_tint_r, rec.hair_tint_r + 3, out->value + 20);
  std::copy(rec.hair_tint_tt, rec.hair_tint_tt + 3, out->value + 23);
  std::copy(rec.hair_tint_trt, rec.hair_tint_trt + 3, out->value + 26);
  std::copy(rec.hair_roughness_r, rec.hair_roughness_r + 2, out->value + 29);
  std::copy(rec.hair_roughness_tt, rec.hair_roughness_tt + 2, out->value + 31);
  std::copy(rec.hair_roughness_trt, rec.hair_roughness_trt + 2, out->value + 33);
  std::copy(rec.hair_absorption, rec.hair_absorption + 3, out->value + 35);
  out->value[38] = rec.hair_ior;
  out->value[39] = rec.hair_cuticle_angle;
  return 0;
}

int RenderStream::outputMaterialStringCopy(int material_id, uint8_t kind,
                                            uint8_t* out, uint32_t cap) const {
  if (material_id < 0 || static_cast<size_t>(material_id) >= materials_.size() ||
      kind > 11) return -1;
  const MaterialRecord& rec = materials_[static_cast<size_t>(material_id)];
  const tr::RenderMaterial* render_mat = outputRenderMaterial_(material_id);
  std::string value;
  switch (kind) {
    case 0: value = rec.key; break;
    case 1: value = rec.prim_path; break;
    case 2:
      if (render_mat) value = RenderMaterialShaderTypeName(render_mat->shader_type);
      break;
    case 3:
      if (render_mat) value = render_scene_.working_color_space;
      break;
    case 4:
      if (render_mat) value = RenderMaterialJson(render_scene_, *render_mat);
      break;
    case 5:
      if (render_mat) {
        value = render_mat->openpbr ? render_mat->openpbr->nodegraph_json
                                    : std::string();
        if (value.empty()) {
          value = render_mat->preview_surface_nodegraph_json;
        }
        if (value.empty()) {
          const lightusd::next::UsdPrim mat = stage_.GetPrimAtPath(rec.prim_path);
          lightusd::next::UsdPrim shader;
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
          value = BuildNextNodeGraphJson(
              mat, shader, render_mat->mtlx_config.version);
        }
      }
      break;
    case 6: if (render_mat) value = render_mat->mtlx_config.version; break;
    case 7: if (render_mat) value = render_mat->mtlx_config.name_space; break;
    case 8: if (render_mat) value = render_mat->mtlx_config.colorspace; break;
    case 9: if (render_mat) value = render_mat->mtlx_config.source_uri; break;
    case 10:
      if (render_mat) value = render_mat->volume_nodegraph_json;
      break;
    case 11:
      if (render_mat) value = render_mat->preview_surface_nodegraph_json;
      break;
  }
  return CopyOutputString(value, out, cap);
}

int RenderStream::prepareMaterialFormat_(int material_id, uint8_t format) const {
  if (formatted_material_id_ == material_id &&
      formatted_material_format_ == format) {
    return formatted_material_status_;
  }
  formatted_material_id_ = material_id;
  formatted_material_format_ = format;
  formatted_material_status_ = 0;
  formatted_material_text_.clear();
  if (!render_scene_valid_) {
    formatted_material_text_ = "Scene not loaded";
    return 0;
  }
  const tydra::next::RenderMaterial* found = outputRenderMaterial_(material_id);
  if (!found) {
    formatted_material_text_ = "Invalid material ID";
    return 0;
  }
  if (format > 2) {
    formatted_material_text_ = "Unsupported format. Use 'json' or 'xml'";
    return 0;
  }
  constexpr size_t kMaxMaterialFormatBytes = size_t{512} << 20;
  const tydra::next::RenderMaterial& material = *found;
  size_t escaped_bytes = 0;
  const auto add_escaped_source = [&escaped_bytes](size_t size) {
    if (size > (std::numeric_limits<size_t>::max)() - escaped_bytes) return false;
    escaped_bytes += size;
    return true;
  };
  if (!add_escaped_source(material.name.size()) ||
      !add_escaped_source(material.prim_path.size()) ||
      !add_escaped_source(material.mtlx_config.version.size()) ||
      !add_escaped_source(material.mtlx_config.name_space.size()) ||
      !add_escaped_source(material.mtlx_config.colorspace.size()) ||
      !add_escaped_source(material.mtlx_config.source_uri.size())) {
    formatted_material_text_ = "Serialized material size estimate overflow";
    return 0;
  }
  if (format != 2 && material.openpbr) {
    if (!add_escaped_source(material.openpbr->nodegraph_json.size())) {
      formatted_material_text_ = "Serialized material size estimate overflow";
      return 0;
    }
  }
  size_t adapter_bytes = 0;
  const auto add_adapter_bytes = [&adapter_bytes](size_t size) {
    if (size > (std::numeric_limits<size_t>::max)() - adapter_bytes) return false;
    adapter_bytes += size;
    return true;
  };
  if (format != 2 && (render_scene_.images.size() >
          (std::numeric_limits<size_t>::max)() /
              sizeof(lightusd::tydra::TextureImage) ||
      render_scene_.textures.size() >
          (std::numeric_limits<size_t>::max)() /
              sizeof(lightusd::tydra::UVTexture) ||
      !add_adapter_bytes(render_scene_.images.size() *
                         sizeof(lightusd::tydra::TextureImage)) ||
      !add_adapter_bytes(render_scene_.textures.size() *
                         sizeof(lightusd::tydra::UVTexture)))) {
    formatted_material_text_ = "Material serialization adapter size overflow";
    return 0;
  }
  if (format != 2) {
    for (const auto& image : render_scene_.images) {
      if (!add_adapter_bytes(image.resolved_path.capacity())) {
        formatted_material_text_ = "Material serialization adapter size overflow";
        return 0;
      }
    }
  }
  if (escaped_bytes > ((std::numeric_limits<size_t>::max)() - 4096) / 6) {
    formatted_material_text_ = "Serialized material size estimate overflow";
    return 0;
  }
  const size_t serialized_estimate = 4096 + escaped_bytes * 6;
  if (adapter_bytes > (std::numeric_limits<size_t>::max)() - serialized_estimate) {
    formatted_material_text_ = "Material serialization adapter size overflow";
    return 0;
  }
  const size_t estimate = serialized_estimate + adapter_bytes;
  if (estimate > kMaxMaterialFormatBytes ||
      estimate > remainingMemoryLimitBytes() / 3) {
    formatted_material_text_ = "Serialized material exceeds available memory limit";
    return 0;
  }
  lightusd::tydra::RenderMaterial legacy_material =
      ToLegacyMaterial(material, stage_);
  if (format == 2) {
    formatted_material_text_ = LegacyPreviewMaterialJSON(legacy_material);
    if (formatted_material_text_.size() > kMaxMaterialFormatBytes) {
      formatted_material_text_ = "Serialized material exceeds 512 MiB limit";
      return 0;
    }
    formatted_material_status_ = 1;
    return 1;
  }
  lightusd::tydra::RenderScene legacy_scene = ToLegacyMaterialScene(render_scene_);
  const lightusd::tydra::SerializationFormat serialization = format == 0
      ? lightusd::tydra::SerializationFormat::JSON
      : lightusd::tydra::SerializationFormat::XML;
  auto serialization_result = lightusd::tydra::serializeMaterial(
      legacy_material, serialization, &legacy_scene);
  if (!serialization_result) {
    formatted_material_text_ = serialization_result.error();
    return 0;
  }
  std::string serialized = std::move(*serialization_result);
  if (serialized.size() > kMaxMaterialFormatBytes) {
    formatted_material_text_ = "Serialized material exceeds 512 MiB limit";
    return 0;
  }
  formatted_material_text_ = std::move(serialized);
  formatted_material_status_ = 1;
  return 1;
}

int RenderStream::materialFormatStatus(int material_id, uint8_t format) const {
  return prepareMaterialFormat_(material_id, format);
}

int RenderStream::materialFormatStringCopy(int material_id, uint8_t format,
                                            uint8_t* out, uint32_t cap) const {
  (void)prepareMaterialFormat_(material_id, format);
  const int required = CopyOutputString(formatted_material_text_, out, cap);
  if (out && required >= 0 && cap >= static_cast<uint32_t>(required)) {
    formatted_material_text_.clear();
    formatted_material_id_ = -1;
    formatted_material_format_ = 0xff;
    formatted_material_status_ = 0;
  }
  return required;
}

int RenderStream::lightFormatStatus(int light_id, uint8_t format) const {
  if (formatted_light_cached_ && formatted_light_id_ == light_id &&
      formatted_light_format_ == format)
    return formatted_light_status_;
  formatted_light_id_ = light_id;
  formatted_light_format_ = format;
  formatted_light_cached_ = true;
  formatted_light_status_ = 0;
  formatted_light_text_.clear();
  if (!render_scene_valid_) {
    formatted_light_text_ = "Scene not loaded";
    return 0;
  }
  if (light_id < 0 || static_cast<size_t>(light_id) >= render_scene_.lights.size()) {
    formatted_light_text_ = "Invalid light ID";
    return 0;
  }
  if (format > 1) {
    formatted_light_text_ = "Unsupported format. Use 'json' or 'xml'";
    return 0;
  }
  constexpr size_t kMaxLightFormatBytes = size_t{512} << 20;
  const auto& light = render_scene_.lights[static_cast<size_t>(light_id)];
  size_t source_bytes = 0;
  bool estimate_ok = true;
  const auto add_source = [&source_bytes, &estimate_ok](size_t bytes) {
    if (bytes > (std::numeric_limits<size_t>::max)() - source_bytes) {
      estimate_ok = false;
      return;
    }
    source_bytes += bytes;
  };
  add_source(light.name.size());
  add_source(light.prim_path.size());
  add_source(light.shaping_ies_file.size());
  add_source(light.texture_file.size());
  for (const auto& s : light.light_link_targets) add_source(s.size());
  for (const auto& s : light.shadow_link_targets) add_source(s.size());
  for (const auto& s : light.filter_targets) add_source(s.size());
  // The shared scene adapter creates lightweight image/texture records even
  // though it does not copy decoded image bytes.
  if (render_scene_.images.size() >
      ((std::numeric_limits<size_t>::max)() - source_bytes) / 256u) {
    estimate_ok = false;
  } else {
    source_bytes += render_scene_.images.size() * 256u;
    for (const auto& image : render_scene_.images)
      add_source(image.resolved_path.size());
  }
  if (render_scene_.textures.size() >
      ((std::numeric_limits<size_t>::max)() - source_bytes) / 128u) {
    estimate_ok = false;
  } else {
    source_bytes += render_scene_.textures.size() * 128u;
  }
  if (!estimate_ok || source_bytes >
          ((std::numeric_limits<size_t>::max)() - 4096u) / 6u) {
    formatted_light_text_ = "Serialized light exceeds available memory limit";
    return 0;
  }
  const size_t estimate = 4096u + source_bytes * 6u;
  if (estimate > kMaxLightFormatBytes ||
      estimate > remainingMemoryLimitBytes() / 3) {
    formatted_light_text_ = "Serialized light exceeds available memory limit";
    return 0;
  }
  const auto legacy_light = ToLegacyLight(light, render_scene_);
  const auto legacy_scene = ToLegacyMaterialScene(render_scene_);
  const auto serialization = format == 0
      ? lightusd::tydra::SerializationFormat::JSON
      : lightusd::tydra::SerializationFormat::XML;
  auto result = lightusd::tydra::serializeLight(legacy_light, serialization,
                                                 &legacy_scene);
  if (!result) {
    formatted_light_text_ = result.error();
    return 0;
  }
  if (result->size() > kMaxLightFormatBytes) {
    formatted_light_text_ = "Serialized light exceeds 512 MiB limit";
    return 0;
  }
  formatted_light_text_ = std::move(*result);
  formatted_light_status_ = 1;
  return 1;
}

int RenderStream::lightFormatStringCopy(int light_id, uint8_t format,
                                        uint8_t* out, uint32_t cap) const {
  (void)lightFormatStatus(light_id, format);
  const int required = CopyOutputString(formatted_light_text_, out, cap);
  if (out && required >= 0 && cap >= static_cast<uint32_t>(required)) {
    formatted_light_text_.clear();
    formatted_light_id_ = -1;
    formatted_light_format_ = 0xff;
    formatted_light_status_ = 0;
    formatted_light_cached_ = false;
  }
  return required;
}

int RenderStream::outputTextureMeta(
    int material_id, uint8_t slot, lightusd_next_output_texture_meta* out) const {
  if (!out || out->struct_size < sizeof(*out)) return -1;
  const TextureMeta* meta = outputTextureMeta_(material_id, slot);
  if (!meta) return -1;
  const uint32_t requested_size = out->struct_size;
  std::memset(out, 0, sizeof(*out));
  out->struct_size = requested_size;
  if (!meta->path.empty()) out->flags |= 1u;
  if (meta->is_udim) out->flags |= 2u;
  if (meta->color_transform_valid) out->flags |= 4u;
  if (meta->color_transform_bypass) out->flags |= 8u;
  if (meta->source_color_is_data) out->flags |= 16u;
  out->source_gamma = meta->source_gamma;
  out->source_linear_bias = meta->source_linear_bias;
  std::copy(meta->source_to_display_linear.begin(),
            meta->source_to_display_linear.end(), out->source_to_display_linear);
  return 0;
}

int RenderStream::outputTextureStringCopy(int material_id, uint8_t slot,
                                           uint8_t kind, uint8_t* out,
                                           uint32_t cap) const {
  const TextureMeta* meta = outputTextureMeta_(material_id, slot);
  if (!meta || kind > 4) return -1;
  const MaterialRecord& rec = materials_[static_cast<size_t>(material_id)];
  const std::string* texture = nullptr;
  switch (slot) {
    case 0: texture = &rec.base_color_texture; break;
    case 1: texture = &rec.normal_texture; break;
    case 2: texture = &rec.roughness_texture; break;
    case 3: texture = &rec.metallic_texture; break;
    case 4: texture = &rec.occlusion_texture; break;
    case 5: texture = &rec.emissive_texture; break;
    case 6: texture = &rec.opacity_texture; break;
  }
  const std::string& value = kind == 0 ? *texture
                             : kind == 1 ? meta->path
                             : kind == 2 ? meta->source_color_space
                             : kind == 3 ? meta->wrap_s : meta->wrap_t;
  return CopyOutputString(value, out, cap);
}

int RenderStream::meshSubsetOutputCopy(int mesh_id, uint8_t* out,
                                       uint32_t cap) {
  if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return -1;
  int source_index = -1;
  if (mesh_merge_) {
    if (static_cast<size_t>(mesh_id) < outputs_.size()) {
      const OutputMesh& record = outputs_[static_cast<size_t>(mesh_id)];
      if (!record.merged) source_index = record.source_index;
    }
  } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
    source_index = mesh_id;
  }
  if (source_index < 0 || static_cast<size_t>(source_index) >= meshes_.size())
    return 0;
  const lightusd::next::UsdPrim& prim =
      meshes_[static_cast<size_t>(source_index)].GetPrim();
  std::vector<int32_t> materials;
  std::vector<std::array<int32_t, 3>> groups;
  auto copy_result = [&]() -> int {
    if (materials.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()) ||
        groups.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
      return -1;
    const size_t required = 8 + materials.size() * sizeof(int32_t) +
                            groups.size() * sizeof(std::array<int32_t, 3>);
    if (required > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
      return -1;
    if (out && cap >= required) {
      const int32_t counts[2] = {static_cast<int32_t>(materials.size()),
                                 static_cast<int32_t>(groups.size())};
      std::memcpy(out, counts, sizeof(counts));
      uint8_t* cursor = out + sizeof(counts);
      if (!materials.empty()) {
        std::memcpy(cursor, materials.data(), materials.size() * sizeof(int32_t));
        cursor += materials.size() * sizeof(int32_t);
      }
      if (!groups.empty()) {
        std::memcpy(cursor, groups.data(),
                    groups.size() * sizeof(std::array<int32_t, 3>));
      }
    }
    return static_cast<int>(required);
  };

  // Converter ranges account for holes and sanitized topology.
  if (render_scene_valid_) {
    const auto mit = render_scene_.mesh_by_path.find(prim.GetPath().str());
    if (mit != render_scene_.mesh_by_path.end() && mit->second >= 0 &&
        static_cast<size_t>(mit->second) < render_scene_.meshes.size()) {
      const tr::RenderMesh& rmesh =
          render_scene_.meshes[static_cast<size_t>(mit->second)];
      if (!rmesh.material_subsets.empty() &&
          !rmesh.face_triangle_offsets.empty()) {
        std::map<int32_t, int32_t> material_indices;
        for (const tr::RenderMesh::MaterialSubset& subset :
             rmesh.material_subsets) {
          if (subset.material_id < 0 ||
              static_cast<size_t>(subset.material_id) >=
                  render_scene_.materials.size()) continue;
          auto found = material_indices.find(subset.material_id);
          int32_t material_index;
          if (found == material_indices.end()) {
            const std::string& path = render_scene_.materials[
                static_cast<size_t>(subset.material_id)].prim_path;
            material_index = static_cast<int32_t>(materials.size());
            material_indices.emplace(subset.material_id, material_index);
            materials.push_back(registerMaterial_(stage_.GetPrimAtPath(path)));
          } else {
            material_index = found->second;
          }
          groups.push_back({static_cast<int32_t>(subset.face_start * 3u),
                            static_cast<int32_t>(subset.face_count * 3u),
                            material_index});
        }
        return groups.empty() ? 0 : copy_result();
      }
    }
  }

  const std::vector<int32_t> face_counts = matIntStatic_(prim, "faceVertexCounts");
  if (face_counts.empty()) return 0;
  struct SubsetInfo {
    lightusd::next::UsdPrim prim;
    std::vector<int32_t> faces;
  };
  std::vector<SubsetInfo> subsets;
  for (const lightusd::next::UsdPrim& child : prim.GetChildren()) {
    if (!child.IsValid() || child.GetTypeName() != "GeomSubset") continue;
    const lightusd::next::Value* family = child.GetPropertyValue("familyName");
    if (family) {
      const std::string* token = family->as_token();
      if (token && *token != "materialBind") continue;
    }
    std::vector<int32_t> faces = matIntStatic_(child, "indices");
    if (faces.empty() ||
        !lightusd::next::GetBoundMaterial(stage_, child).IsValid()) continue;
    subsets.push_back({child, std::move(faces)});
  }
  if (subsets.empty()) return 0;

  std::vector<int32_t> face_material(face_counts.size(), -1);
  for (size_t i = 0; i < subsets.size(); ++i) {
    const int32_t material_index = static_cast<int32_t>(i);
    for (int32_t face : subsets[i].faces) {
      if (face >= 0 && static_cast<size_t>(face) < face_material.size())
        face_material[static_cast<size_t>(face)] = material_index;
    }
    const lightusd::next::UsdPrim mat =
        lightusd::next::GetBoundMaterial(stage_, subsets[i].prim);
    materials.push_back(registerMaterial_(mat));
  }
  const std::vector<uint32_t> triangle_starts = faceTriangleStarts_(face_counts);
  size_t face_begin = 0;
  while (face_begin < face_material.size()) {
    const int32_t material_index = face_material[face_begin];
    size_t face_end = face_begin + 1;
    while (face_end < face_material.size() &&
           face_material[face_end] == material_index) ++face_end;
    if (material_index >= 0 && face_begin < triangle_starts.size() &&
        face_end < triangle_starts.size()) {
      const uint32_t start = triangle_starts[face_begin] * 3u;
      const uint32_t count =
          (triangle_starts[face_end] - triangle_starts[face_begin]) * 3u;
      if (count > 0) {
        groups.push_back({static_cast<int32_t>(start),
                          static_cast<int32_t>(count), material_index});
      }
    }
    face_begin = face_end;
  }
  return copy_result();
}
}  // namespace web_next
}  // namespace lightusd
