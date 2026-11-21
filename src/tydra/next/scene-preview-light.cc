// SPDX-License-Identifier: Apache-2.0
// Portable bounded linear-HDR light emission sidecars.
#include <algorithm>
#include <cmath>
#include <fstream>

#include "scene-preview-internal.hh"
#include "scene-preview-photometry.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
float Linear(float x) {
  return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
bool Hdr(const fs::path& path, const DecodedImage& image, bool srgb) {
  std::ofstream file(path, std::ios::binary);
  file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << image.height << " +X "
       << image.width << "\n";
  std::vector<unsigned char> row(size_t(image.width) * 4);
  for (size_t y = 0; y < image.height; ++y) {
    for (size_t x = 0; x < image.width; ++x) {
      const size_t i = y * image.width + x;
      float rgb[3];
      for (size_t k = 0; k < 3; ++k)
        rgb[k] =
            image.hdr
                ? image.float_pixels[i * 3 + k]
                : (srgb
                       ? Linear(image.pixels[i * image.channels +
                                             std::min(k, size_t(image.channels -
                                                                1))] /
                                255.f)
                       : image.pixels[i * image.channels +
                                      std::min(k, size_t(image.channels - 1))] /
                             255.f);
      float value = std::max(rgb[0], std::max(rgb[1], rgb[2]));
      unsigned char pixel[4] = {};
      if (std::isfinite(value) && value > 1e-32f) {
        value = std::min(value, 1e30f);
        int exponent = 0;
        const float factor = std::frexp(value, &exponent) * 256 / value;
        for (size_t k = 0; k < 3; ++k)
          pixel[k] = static_cast<unsigned char>(
              std::max(0.f, std::min(255.f, rgb[k] * factor)));
        pixel[3] = static_cast<unsigned char>(exponent + 128);
      }
      std::copy(pixel, pixel + 4, row.data() + x * 4);
    }
    if (image.width >= 8 && image.width < 32768) {
      const unsigned char header[] = {
          2, 2, static_cast<unsigned char>(image.width >> 8),
          static_cast<unsigned char>(image.width & 255)};
      file.write(reinterpret_cast<const char*>(header), 4);
      for (size_t k = 0; k < 4; ++k)
        for (size_t x = 0; x < image.width;) {
          const size_t count = std::min(size_t(128), size_t(image.width) - x);
          file.put(static_cast<char>(count));
          for (size_t j = 0; j < count; ++j)
            file.put(static_cast<char>(row[(x + j) * 4 + k]));
          x += count;
        }
    } else
      file.write(reinterpret_cast<const char*>(row.data()),
                 static_cast<std::streamsize>(row.size()));
  }
  return bool(file);
}
}  // namespace
bool Build::bake_lights() {
  TextureDecodeOptions decode;
  decode.max_edge = 256;
  decode.budget_bytes = uint64_t(32) << 20;
  decode.max_source_bytes = uint64_t(256) << 20;
  TextureDecoder textures(decode);
  std::map<std::string, std::string> baked;
  for (auto& extra : extras) {
    const auto* value = extra.property_value("inputs:texture:file");
    if (!value || !value->as_asset_path()) continue;
    const std::string input = value->as_asset_path();
    dependencies.insert(input);
    bool srgb = true;
    if (const auto* meta = extra.property_meta("inputs:texture:file"))
      srgb = meta->colorSpace != "raw" && meta->colorSpace != "linear" &&
             meta->colorSpace != "data" &&
             meta->colorSpace != "lin_rec709_scene";
    const std::string key = input + (srgb ? "#srgb" : "#linear");
    auto it = baked.find(key);
    if (it == baked.end()) {
      DecodedImage image;
      if (!textures.Decode(input, srgb, &image) || image.empty()) {
        extra.remove_property("inputs:texture:file");
        if (result.warning.size() < 16384)
          result.warning +=
              "Preview light texture unavailable: " + input + "\n";
        continue;
      }
      const std::string name = "light-" + std::to_string(Hash(key)) + ".hdr";
      if (!Hdr(directory / name, image, srgb)) {
        result.error = "cannot write baked light emission";
        return false;
      }
      it = baked.emplace(key, name).first;
    }
    extra.upsert_property("inputs:texture:file",
                          usd::Value::MakeAssetPath(it->second));
    auto& metadata = extra.ensure_property_meta("inputs:texture:file");
    metadata.colorSpace = "linear";
    metadata.authored |= usd::PropMeta::kColorSpace;
    if (!check("light-textures", baked.size())) return false;
  }
  // Photometric profiles become self-contained sidecars so an exported cache
  // does not point back into the source asset tree. A profile that cannot be
  // read leaves the light unshaped, exactly like a missing emission texture.
  size_t profiles = 0;
  for (auto& extra : extras) {
    const auto* value = extra.property_value("inputs:shaping:ies:file");
    if (!value || !value->as_asset_path()) continue;
    const std::string input = value->as_asset_path();
    dependencies.insert(input);
    PreviewIesProfile profile;
    std::string error;
    if (!LoadPreviewIes(input, &profile, &error)) {
      extra.remove_property("inputs:shaping:ies:file");
      if (result.warning.size() < 16384)
        result.warning += "Preview light IES unavailable: " + input + " (" +
                          error + ")\n";
      continue;
    }
    const std::string name = "light-" + std::to_string(Hash(input)) + ".ies";
    if (!WritePreviewIes((directory / name).string(), profile, &error)) {
      result.error = "cannot write baked light IES profile";
      return false;
    }
    extra.upsert_property("inputs:shaping:ies:file",
                          usd::Value::MakeAssetPath(name));
    if (!check("light-ies", ++profiles)) return false;
  }
  return true;
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
