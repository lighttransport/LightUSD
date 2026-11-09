// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.

#include "mcp-tools-usdz.hh"

#include <algorithm>
#include <climits>
#include <limits>
#include <map>
#include <vector>

#include "external/jsonhpp/nlohmann/json.hpp"

#include "../lightusd.hh"
#include "../usdz-convert.hh"
#include "image-loader.hh"
#include "image-writer.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tydra/texture-util.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

namespace {

image::PngEncoder ParsePngEncoder(const nlohmann::json &args) {
  std::string v = to_lower(args.value("pngEncoder", std::string("auto")));
  if (v == "fpng") return image::PngEncoder::Fpng;
  if (v == "fpnge") return image::PngEncoder::Fpnge;
  return image::PngEncoder::Auto;
}

// Encode an Image to base64 in the requested format ("png" default, or "jpeg").
// NOTE: this namespace (lightusd::tydra::mcp) has its own `Image` type, so the
// pixel-image type must be spelled `lightusd::Image`.
bool EncodeImageBase64(const lightusd::Image &img, const std::string &format,
                       image::PngEncoder enc, int jpeg_quality,
                       std::string *b64, std::string *mime, std::string &err) {
  image::WriteOption wopt;
  wopt.png_encoder = enc;
  wopt.jpeg_quality = jpeg_quality;
  std::string fmt = to_lower(format);
  if (fmt == "jpeg" || fmt == "jpg") {
    wopt.format = image::WriteImageFormat::JPEG;
    *mime = "image/jpeg";
  } else {
    wopt.format = image::WriteImageFormat::PNG;
    *mime = "image/png";
  }

  auto ret = image::WriteImageToMemory(img, wopt);
  if (!ret) {
    err = "Image encode failed: " + ret.error();
    return false;
  }
#if SIZE_MAX > UINT_MAX
  if (ret.value().size() > static_cast<size_t>(std::numeric_limits<unsigned int>::max())) {
    err = "Encoded image too large for base64 encoding.";
    return false;
  }
#endif
  *b64 = base64_encode(ret.value().data(), static_cast<unsigned int>(ret.value().size()));
  return true;
}

// Decode a base64 string into an Image.
bool DecodeImageBase64(const std::string &b64, lightusd::Image *img, std::string &err) {
  // Cap input size to avoid unbounded allocation.
  constexpr size_t kMaxBase64Input = size_t(64) * 1024 * 1024;  // 64 MiB base64
  if (b64.size() > kMaxBase64Input) {
    err = "Base64 input exceeds 64 MiB limit.";
    return false;
  }
  std::string bytes = base64_decode(b64);
  if (bytes.empty()) {
    err = "Empty/invalid base64 image data.";
    return false;
  }
  // Cap decoded size.
  constexpr size_t kMaxDecodedBytes = size_t(256) * 1024 * 1024;  // 256 MiB
  if (bytes.size() > kMaxDecodedBytes) {
    err = "Decoded image data exceeds 256 MiB limit.";
    return false;
  }
  auto ret = image::LoadImageFromMemory(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), "mem");
  if (!ret) {
    err = "Failed to decode image: " + ret.error();
    return false;
  }
  *img = std::move(ret.value().image);
  return true;
}

}  // namespace

bool USDZConvert(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err) {
  (void)ctx;
  if (!args.contains("input") || !args["input"].is_string()) {
    err = "Missing 'input' argument";
    return false;
  }
  if (!args.contains("output") || !args["output"].is_string()) {
    err = "Missing 'output' argument";
    return false;
  }

  usdz::UsdzConvertOptions opts;
  std::string input_path = args["input"].get<std::string>();
  std::string output_path = args["output"].get<std::string>();
  // Reject path traversal and unsafe paths.
  auto isUnsafePath = [](const std::string &p) -> bool {
    if (p.empty()) return true;
    if (p.find("..") != std::string::npos) return true;
    if (p.find('\0') != std::string::npos) return true;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() >= 2 && p[1] == ':') return true;
    return false;
  };
  if (isUnsafePath(input_path) || isUnsafePath(output_path)) {
    err = "Path contains unsafe characters or traversal sequences.";
    return false;
  }
  opts.inputs.push_back(input_path);
  opts.output = output_path;
  opts.flatten = args.value("flatten", true);
  opts.arkit_compatible = args.value("arkitCompatible", false);
  opts.reencode = args.value("reencode", true);

  double mpu = args.value("metersPerUnit", 0.0);
  opts.metersPerUnit = (mpu > 0.0 && mpu < 1e6) ? mpu : 0.0;

  int rsz = args.value("resizeTextures", 0);
  opts.max_texture_size = (rsz > 0) ? rsz : 0;

  int jq = args.value("jpegQuality", 90);
  if (jq < 1 || jq > 100) jq = 90;
  opts.jpeg_quality = jq;

  opts.png_encoder = ParsePngEncoder(args);

  // Texture budget fit.
  double ttb = args.value("targetTextureBytes", 0.0);
  opts.target_texture_bytes = (ttb > 0.0) ? size_t(ttb) : 0;
  std::string fs = to_lower(args.value("fitStrategy", std::string("size")));
  opts.fit_strategy = (fs == "quality") ? usdz::FitStrategy::Quality
                                         : usdz::FitStrategy::Size;

  int fmts = args.value("fitMinTextureSize", 64);
  opts.fit_min_texture_size = (fmts >= 1 && fmts <= 16384) ? fmts : 64;
  int fmtq = args.value("fitMinQuality", 30);
  opts.fit_min_jpeg_quality = (fmtq >= 1 && fmtq <= 100) ? fmtq : 30;

  std::string tf = to_lower(args.value("textureFormat", std::string("keep")));
  if (tf == "png") opts.texture_format = usdz::OutputTextureFormat::PNG;
  else if (tf == "jpeg" || tf == "jpg") opts.texture_format = usdz::OutputTextureFormat::JPEG;
  else opts.texture_format = usdz::OutputTextureFormat::KeepOriginal;

  std::string ax = to_lower(args.value("upAxis", std::string("")));
  if (ax == "x") opts.upAxis = Axis::X;
  else if (ax == "y") opts.upAxis = Axis::Y;
  else if (ax == "z") opts.upAxis = Axis::Z;

  usdz::UsdzConvertStats stats;
  std::string warn;
  if (!usdz::Convert(opts, &stats, &warn, &err)) {
    return false;
  }

  result["success"] = true;
  result["output"] = opts.output;
  result["warn"] = warn;
  result["stats"] = {
      {"textures", stats.num_textures},
      {"resized", stats.num_textures_resized},
      {"reencoded", stats.num_textures_reencoded},
      {"passthrough", stats.num_textures_passthrough},
      {"output_size", stats.output_size},
  };
  return true;
}

bool USDZPack(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  if (args.value("arkitCompatible", false)) {
    ctx.stage->metas().upAxis.set_value(Axis::Y);
  }

  if (args.contains("metersPerUnit") && args["metersPerUnit"].is_number()) {
    double mpu = args["metersPerUnit"].get<double>();
    if (mpu > 0.0) {
      ctx.stage->metas().metersPerUnit.set_value(mpu);
    }
  }

  // NOTE: assets map is empty -- no texture reprocessing during pack.
  // Textures remain as file-path references in the USDC layer.
  // For texture resize/reencode, use the full USDZConvert tool instead.
  const std::map<std::string, std::vector<uint8_t>> assets;
  std::string warn;

  if (args.contains("uri") && args["uri"].is_string()) {
    const std::string uri = args["uri"].get<std::string>();
    // Reject unsafe paths (same checks as USDZConvert).
    if (uri.empty() || uri.find("..") != std::string::npos ||
        uri.find('\0') != std::string::npos ||
        uri[0] == '/' || uri[0] == '\\' ||
        (uri.size() >= 2 && uri[1] == ':')) {
      err = "Path contains unsafe characters or traversal sequences.";
      return false;
    }
    if (!lightusd::SaveAsUSDZToFile(uri, *ctx.stage, assets, &warn, &err)) {
      return false;
    }
    result["success"] = true;
    result["uri"] = uri;
    return true;
  }

  std::vector<uint8_t> out;
  if (!lightusd::SaveAsUSDZToMemory(*ctx.stage, assets, &out, &warn, &err)) {
    return false;
  }
  // Cap in-memory archive size before base64 encoding.
  constexpr size_t kMaxUSDZArchiveBytes = size_t(256) * 1024 * 1024;  // 256 MiB
  if (out.size() > kMaxUSDZArchiveBytes) {
    err = "USDZ archive exceeds 256 MiB limit for in-memory encoding.";
    return false;
  }
#if SIZE_MAX > UINT_MAX
  if (out.size() > static_cast<size_t>(std::numeric_limits<unsigned int>::max())) {
    err = "USDZ archive too large for base64 encoding.";
    return false;
  }
#endif
  result["success"] = true;
  result["data"] =
      base64_encode(out.data(), static_cast<unsigned int>(out.size()));
  result["size"] = out.size();
  return true;
}

bool TextureResize(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  (void)ctx;
  if (!args.contains("data") || !args["data"].is_string()) {
    err = "Missing 'data' (base64 image) argument";
    return false;
  }

  lightusd::Image img;
  if (!DecodeImageBase64(args["data"].get<std::string>(), &img, err)) {
    return false;
  }

  // Validate decoded image dimensions.
  const int kMaxDimension = 32768;
  if (img.width <= 0 || img.height <= 0 || img.channels < 1 || img.channels > 4) {
    err = "Invalid decoded image dimensions.";
    return false;
  }
  if (img.width > kMaxDimension || img.height > kMaxDimension) {
    err = "Image dimensions exceed maximum allowed size.";
    return false;
  }

  int target_w = args.value("width", 0);
  int target_h = args.value("height", 0);
  const int max_size = args.value("max_size", 0);

  if (target_w <= 0 || target_h <= 0) {
    if (max_size > 0) {
      const int longest = (std::max)(img.width, img.height);
      if (longest > max_size) {
        const double scale = double(max_size) / double(longest);
        target_w = (std::max)(1, int(img.width * scale + 0.5));
        target_h = (std::max)(1, int(img.height * scale + 0.5));
      } else {
        target_w = img.width;
        target_h = img.height;
      }
    } else {
      err = "Provide either width+height or max_size";
      return false;
    }
  }

  lightusd::Image resized;
  if ((target_w == img.width) && (target_h == img.height)) {
    resized = img;
  } else if (!tydra::ResizeImage(img, target_w, target_h, &resized,
                                 tydra::ResizeFilter::Auto, &err)) {
    return false;
  }

  std::string b64, mime;
  if (!EncodeImageBase64(resized, args.value("format", std::string("png")),
                         ParsePngEncoder(args), args.value("jpegQuality", 90),
                         &b64, &mime, err)) {
    return false;
  }

  result["success"] = true;
  result["data"] = b64;
  result["mimeType"] = mime;
  result["width"] = resized.width;
  result["height"] = resized.height;
  return true;
}

bool TextureRepack(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  (void)ctx;

  const char *slot_names[4] = {"r", "g", "b", "a"};

  std::vector<lightusd::Image> images;
  // Map a base64 payload string -> image index (dedupe identical inputs).
  std::map<std::string, int> data_to_index;

  tydra::ChannelPackSpec spec;
  spec.out_channels = args.value("channels", 0);
  spec.out_width = args.value("width", 0);
  spec.out_height = args.value("height", 0);
  tydra::ChannelSource *dst[4] = {&spec.r, &spec.g, &spec.b, &spec.a};

  int inferred_channels = 0;
  for (int c = 0; c < 4; c++) {
    if (!args.contains(slot_names[c])) {
      // default: opaque alpha, zero rgb (only used if out_channels covers it)
      dst[c]->input_index = -1;
      dst[c]->constant = (c == 3) ? 255 : 0;
      continue;
    }
    inferred_channels = (std::max)(inferred_channels, c + 1);
    const nlohmann::json &slot = args[slot_names[c]];

    if (slot.contains("data") && slot["data"].is_string()) {
      const std::string b64 = slot["data"].get<std::string>();
      int idx;
      auto it = data_to_index.find(b64);
      if (it != data_to_index.end()) {
        idx = it->second;
      } else {
        lightusd::Image im;
        if (!DecodeImageBase64(b64, &im, err)) {
          return false;
        }
        idx = int(images.size());
        images.push_back(std::move(im));
        data_to_index[b64] = idx;
      }
      dst[c]->input_index = idx;
      dst[c]->channel = slot.value("channel", 0);
    } else {
      dst[c]->input_index = -1;
      dst[c]->constant = uint8_t(slot.value("const", 0) & 0xff);
    }
  }

  if (spec.out_channels < 1 || spec.out_channels > 4) {
    spec.out_channels = inferred_channels > 0 ? inferred_channels : 4;
  }

  lightusd::Image packed;
  if (!tydra::PackChannels(images, spec, &packed, &err)) {
    return false;
  }

  std::string b64, mime;
  if (!EncodeImageBase64(packed, args.value("format", std::string("png")),
                         ParsePngEncoder(args), args.value("jpegQuality", 90),
                         &b64, &mime, err)) {
    return false;
  }

  result["success"] = true;
  result["data"] = b64;
  result["mimeType"] = mime;
  result["width"] = packed.width;
  result["height"] = packed.height;
  result["channels"] = packed.channels;
  return true;
}

}  // namespace mcp
}  // namespace tydra
}  // namespace lightusd
