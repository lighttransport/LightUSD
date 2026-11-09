// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lightusd {
namespace web_next {
namespace {
const tr::ShaderParam* CommonMaterialParam(const tr::RenderMaterial& material,
                                           uint8_t param) {
  if (material.shader_type == tr::RenderMaterial::ShaderType::PreviewSurface &&
      material.preview_surface) {
    const auto& s = *material.preview_surface;
    switch (param) {
      case 0: return &s.diffuse_color;
      case 1: return &s.emissive_color;
      case 2: return &s.metallic;
      case 3: return &s.roughness;
      case 4: return &s.opacity;
    }
  } else if (material.shader_type == tr::RenderMaterial::ShaderType::OpenPBR &&
             material.openpbr) {
    const auto& s = *material.openpbr;
    switch (param) {
      case 0: return &s.base_color;
      case 1: return &s.emission_color;
      case 2: return &s.base_metalness;
      case 3: return &s.base_roughness;
      case 4: return &s.opacity;
    }
  }
  return nullptr;
}

}  // namespace

int RenderStream::materialField(int material_id, uint8_t field) const {
    const tr::RenderMaterial* found = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !found) return -1;
    const tr::RenderMaterial& material = *found;
    switch (field) {
      case 0: return static_cast<int>(material.shader_type);
      case 1: return static_cast<int>(material.alpha_mode);
      case 2: return material.double_sided ? 1 : 0;
      case 3: {
        float opacity = 1.0f;
        if (material.preview_surface) opacity = material.preview_surface->opacity.as_float();
        else if (material.openpbr) opacity = material.openpbr->opacity.as_float();
        if (!std::isfinite(opacity)) return 0;
        const double scaled = static_cast<double>(opacity) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 4: {
        float roughness = 0.0f;
        if (material.preview_surface) roughness = material.preview_surface->roughness.as_float();
        else if (material.openpbr) roughness = material.openpbr->specular_roughness.as_float();
        if (!std::isfinite(roughness)) return 0;
        const double scaled = static_cast<double>(roughness) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 5: {
        float clearcoat = 0.0f;
        if (material.preview_surface) clearcoat = material.preview_surface->clearcoat.as_float();
        if (!std::isfinite(clearcoat)) return 0;
        const double scaled = static_cast<double>(clearcoat) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 6: {
        float clearcoat_roughness = 0.0f;
        if (material.preview_surface)
          clearcoat_roughness = material.preview_surface->clearcoat_roughness.as_float();
        if (!std::isfinite(clearcoat_roughness)) return 0;
        const double scaled = static_cast<double>(clearcoat_roughness) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 7: return material.default_fallback ? 1 : 0;
      default: return -1;
    }
}

int RenderStream::materialDiagnosticCount(int material_id) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material) return -1;
    const size_t count = material->diagnostics.size();
    return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(count)
               : -1;
}

int RenderStream::materialDiagnosticKind(int material_id,
                                         int diagnostic_id) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || diagnostic_id < 0) return -1;
    const auto& diagnostics = material->diagnostics;
    if (static_cast<size_t>(diagnostic_id) >= diagnostics.size()) return -1;
    return static_cast<int>(diagnostics[static_cast<size_t>(diagnostic_id)].kind);
}

int RenderStream::materialDiagnosticStringCopy(int material_id,
                                                int diagnostic_id,
                                                uint8_t kind, uint8_t* out,
                                                uint32_t cap) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || diagnostic_id < 0 || kind > 3) return -1;
    const auto& diagnostics = material->diagnostics;
    if (static_cast<size_t>(diagnostic_id) >= diagnostics.size()) return -1;
    const tr::MaterialDiagnostic& diagnostic =
        diagnostics[static_cast<size_t>(diagnostic_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &diagnostic.material_path; break;
      case 1: value = &diagnostic.node_path; break;
      case 2: value = &diagnostic.shader_id; break;
      case 3: value = &diagnostic.message; break;
      default: return -1;
    }
    return CopyOutputString(*value, out, cap);
}

int RenderStream::materialParamBufferCopy(int material_id, uint8_t param,
                                          uint8_t* out, uint32_t cap) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || param > 4) return -1;
    const tr::ShaderParam* value = CommonMaterialParam(*material, param);
    if (!value) return -1;
    constexpr size_t kBytes = 4 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, &value->value, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::materialParamTextureId(int material_id, uint8_t param) const {
    const tr::RenderMaterial* material = outputRenderMaterial_(material_id);
    if (!render_scene_valid_ || !material || param > 4) return -1;
    const tr::ShaderParam* value = CommonMaterialParam(*material, param);
    return value ? value->texture_id : -1;
}

int RenderStream::textureField(int texture_id, uint8_t field) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const tr::TextureImage* image =
        (texture.image_id >= 0 &&
         static_cast<size_t>(texture.image_id) < render_scene_.images.size())
            ? &render_scene_.images[static_cast<size_t>(texture.image_id)]
            : nullptr;
    switch (field) {
      case 0: return texture.image_id;
      case 1: return image ? static_cast<int>(image->width) : 0;
      case 2: return image ? static_cast<int>(image->height) : 0;
      case 3: return image ? image->channels : 0;
      case 4: return image ? image->mip_levels : 0;
      case 5: return image && image->is_loaded() ? 1 : 0;
      case 6: return static_cast<int>(texture.wrap_s);
      case 7: return static_cast<int>(texture.wrap_t);
      case 8: return static_cast<int>(texture.output_channel);
      case 9: {
        const double scaled = static_cast<double>(texture.rotation) * 1000000.0;
        return scaled <= static_cast<double>((std::numeric_limits<int>::min)())
                   ? (std::numeric_limits<int>::min)()
                   : scaled >= static_cast<double>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(scaled);
      }
      case 10: return texture.has_transform2d ? 1 : 0;
      case 11: return texture.is_udim ? 1 : 0;
      case 12: return texture.udim_texture_id;
      default: return -1;
    }
}

int RenderStream::textureStringCopy(int texture_id, uint8_t kind,
                                    uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 || kind > 2 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &texture.uv_primvar; break;
      case 1: value = &texture.source_color_space; break;
      case 2: value = &texture.target_color_space; break;
      default: return -1;
    }
    return CopyOutputString(*value, out, cap);
}

int RenderStream::textureImageBufferCopy(int texture_id, uint8_t* out,
                                         uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    if (texture.image_id < 0 ||
        static_cast<size_t>(texture.image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image =
        render_scene_.images[static_cast<size_t>(texture.image_id)];
    const size_t bytes = image.data.size();
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (image.data.is_contiguous()) {
      std::memcpy(out, image.data.chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < bytes; ++i) out[i] = image.data[i];
    }
    return required;
}

int RenderStream::imageField(int image_id, uint8_t field) const {
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    switch (field) {
      case 0: return static_cast<int>(image.width);
      case 1: return static_cast<int>(image.height);
      case 2: return image.channels;
      case 3: return image.mip_levels;
      case 4: return image.is_loaded() ? 1 : 0;
      case 5: return static_cast<int>(image.component_type);
      case 6: return static_cast<int>(image.color_space);
      case 7:
        return image.data.size() <= (size_t{1} << 29)
            ? static_cast<int>(image.data.size()) : -1;
      default: return -1;
    }
}

int RenderStream::imageAssetIdentifierCopy(int image_id, uint8_t* out,
                                           uint32_t cap) const {
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const std::string& value = render_scene_.images[static_cast<size_t>(image_id)].asset_identifier;
    return CopyOutputString(value, out, cap);
}

int RenderStream::imageBufferCopy(int image_id, uint8_t* out,
                                  uint32_t cap) const {
    constexpr size_t kMaxImageCopyBytes = size_t{1} << 29;
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return -1;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    const size_t bytes = image.data.size();
    if (bytes > kMaxImageCopyBytes ||
        bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (image.data.is_contiguous()) {
      std::memcpy(out, image.data.chunk_data(0), bytes);
    } else {
      for (size_t i = 0; i < bytes; ++i) out[i] = image.data[i];
    }
    return required;
}

uintptr_t RenderStream::imageBufferData(int image_id) const {
    constexpr size_t kMaxImageCopyBytes = size_t{1} << 29;
    if (!render_scene_valid_ || image_id < 0 ||
        static_cast<size_t>(image_id) >= render_scene_.images.size()) return 0;
    const tr::TextureImage& image = render_scene_.images[static_cast<size_t>(image_id)];
    if (image.data.empty() || !image.data.is_contiguous() ||
        image.data.size() > kMaxImageCopyBytes) return 0;
    return reinterpret_cast<uintptr_t>(image.data.chunk_data(0));
}

int RenderStream::textureSamplingBufferCopy(int texture_id, uint8_t* out,
                                            uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 13> values = {
        texture.offset.x, texture.offset.y, texture.scale.x, texture.scale.y,
        texture.rotation,
        texture.bias.x, texture.bias.y, texture.bias.z, texture.bias.w,
        texture.scale_value.x, texture.scale_value.y,
        texture.scale_value.z, texture.scale_value.w};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::textureTransformBufferCopy(int texture_id, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 5> values = {
        texture.tx_rotation, texture.tx_scale.x, texture.tx_scale.y,
        texture.tx_translation.x, texture.tx_translation.y};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
  return static_cast<int>(kBytes);
}

int RenderStream::textureUDIMRemapBufferCopy(int texture_id, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 4> values = {
        texture.udim_uv_scale.x, texture.udim_uv_scale.y,
        texture.udim_uv_offset.x, texture.udim_uv_offset.y};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::udimTileCount(int udim_id) const {
    if (!render_scene_valid_ || udim_id < 0 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const size_t count = render_scene_.udim_textures[static_cast<size_t>(udim_id)].tiles.size();
    return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(count) : -1;
}

int RenderStream::udimTextureCount() const {
    if (!render_scene_valid_) return 0;
    return render_scene_.udim_textures.size() <=
                   static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(render_scene_.udim_textures.size()) : -1;
}

int RenderStream::udimTileBufferCopy(int udim_id, uint8_t* out,
                                     uint32_t cap) const {
    if (!render_scene_valid_ || udim_id < 0 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const auto& tiles = render_scene_.udim_textures[static_cast<size_t>(udim_id)].tiles;
    constexpr size_t kRecordBytes = 4 * sizeof(int32_t);
    if (tiles.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) / kRecordBytes) return -1;
    const size_t required = tiles.size() * kRecordBytes;
    if (!out || cap < required || required == 0) return static_cast<int>(required);
    for (size_t i = 0; i < tiles.size(); ++i) {
      const int32_t record[4] = {
          static_cast<int32_t>(tiles[i].udim),
          static_cast<int32_t>((tiles[i].udim - 1001u) % 10u),
          static_cast<int32_t>((tiles[i].udim - 1001u) / 10u),
          tiles[i].image_id};
      std::memcpy(out + i * kRecordBytes, record, kRecordBytes);
    }
    return static_cast<int>(required);
}

int RenderStream::udimStringCopy(int udim_id, uint8_t kind, uint8_t* out,
                                 uint32_t cap) const {
    if (!render_scene_valid_ || udim_id < 0 || kind > 3 ||
        static_cast<size_t>(udim_id) >= render_scene_.udim_textures.size()) return -1;
    const auto& udim = render_scene_.udim_textures[static_cast<size_t>(udim_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &udim.prim_name; break;
      case 1: value = &udim.abs_path; break;
      case 2: value = &udim.display_name; break;
      case 3: value = &udim.asset_identifier; break;
      default: return -1;
    }
    return CopyOutputString(*value, out, cap);
}

int RenderStream::textureColorTransformBufferCopy(int texture_id,
                                                  uint8_t* out,
                                                  uint32_t cap) const {
    if (!render_scene_valid_ || texture_id < 0 ||
        static_cast<size_t>(texture_id) >= render_scene_.textures.size()) return -1;
    const tr::RenderTexture& texture =
        render_scene_.textures[static_cast<size_t>(texture_id)];
    const std::array<float, 14> values = {
        texture.color_transform_valid ? 1.0f : 0.0f,
        texture.color_transform_bypass ? 1.0f : 0.0f,
        texture.source_color_is_data ? 1.0f : 0.0f,
        texture.source_gamma,
        texture.source_linear_bias,
        texture.source_to_display_linear[0],
        texture.source_to_display_linear[1],
        texture.source_to_display_linear[2],
        texture.source_to_display_linear[3],
        texture.source_to_display_linear[4],
        texture.source_to_display_linear[5],
        texture.source_to_display_linear[6],
        texture.source_to_display_linear[7],
        texture.source_to_display_linear[8]};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

}  // namespace web_next
}  // namespace lightusd
