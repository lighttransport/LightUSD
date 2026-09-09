// SPDX-License-Identifier: Apache 2.0

// Simple image loader
// supported file format: PNG(use fpng), BMP/JPEG(use stb_image), HDR/RGBE(use stb_image), OpenEXR(use tinyexr), TIFF(use tinydng)
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "image-types.hh"
#include "security-policy.hh"

#include "nonstd/expected.hpp"

namespace lightusd {
namespace image {

struct ImageResult {
  Image image;
  std::string warning;
};

struct ImageInfoResult {
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  std::string warning;
};

///
/// User-defined Image asset loader
///
/// Applications that need a format outside the built-in set can register a
/// callback with SetImageLoader and SetImageInfoLoader.
///

///
/// Callback function to load an image from memory.
///
/// @param[in] addr Image data byte address.
/// @param[in] datasize Image data size in bytes.
/// @param[in] asset_name Corresponding asset/file name.
/// @param[inout] user_data User data pointer. Can be nullptr.
/// @param[out] warn Warning message. Can be nullptr.
/// @param[out] err Error message. Can be nullptr.
///
/// @return true upon success.

typedef bool (*LoadImageDataFunction)(ImageResult *image, const uint8_t *addr, const size_t datasize, const std::string &asset_name, void *user_data, std::string *warn, std::string *err);

///
/// Callback function to get info of an image from memory.
///
/// @param[in] addr Image data byte address.
/// @param[in] datasize Image data size in bytes.
/// @param[in] asset_name Corresponding asset/file name.
/// @param[inout] user_data User data pointer. Can be nullptr.
/// @param[out] warn Warning message. Can be nullptr.
/// @param[out] err Error message. Can be nullptr.
///
/// @return true upon success.

typedef bool (*GetImageInfoFunction)(ImageInfoResult *image, const uint8_t *addr, const size_t datasize, const std::string &asset_name, void *user_data);

/// Install an optional application image decoder. The callback is tried before
/// the built-in decoders and may handle formats unknown to LightUSD. Passing a
/// null callback disables the override.
void SetImageLoader(LoadImageDataFunction loader, void *user_data = nullptr);

/// Install an optional application image-info decoder. The callback is tried
/// before the built-in metadata readers. Passing a null callback disables it.
void SetImageInfoLoader(GetImageInfoFunction loader, void *user_data = nullptr);


///
/// Load image from a file.
///
/// @param[in] filename Input filename(or URI)
/// @param[in] max_memory_limit_in_mb Optional. Maximum image file size in [MB]. Default = 512 MB.
/// @return ImageResult or error message(std::string)
///
nonstd::expected<ImageResult, std::string> LoadImageFromFile(
    const std::string &filename,
    const size_t max_memory_limit_in_mb =
        security_policy::kDefaultInputLimitBytes / (1024ull * 1024ull));

///
/// Get Image info from file.
///
/// @param[in] filename Input filename(or URI)
/// @return ImageInfoResult or error message(std::string)
///
nonstd::expected<ImageInfoResult, std::string> GetImageInfoFromFile(const std::string &filename);

///
/// Load image from memory
///
/// @param[in] addr Memory address
/// @param[in] datasize Data size(in bytes)
/// @param[in] uri Input URI(or filename) as a hint. This is used only in error message.
/// @return ImageResult or error message(std::string)
///
nonstd::expected<ImageResult, std::string> LoadImageFromMemory(const uint8_t *addr, const size_t datasize, const std::string &uri);

/// Load every decoded TIFF/DNG directory as a separate image layer.
///
/// This is intentionally a TIFF/DNG-specific API. The regular single-image
/// loader selects the largest directory for compatibility; this API preserves
/// all directories returned by TinyDNG in file order.
nonstd::expected<std::vector<ImageResult>, std::string>
LoadImageLayersFromMemory(const uint8_t *addr, size_t datasize,
                          const std::string &uri);

///
/// Get Image info from a file.
///
/// @param[in] addr Memory address
/// @param[in] datasize Data size(in bytes)
/// @param[in] uri Input URI(or filename) as a hint. This is used only in error message.
/// @return ImageResult or error message(std::string)
///
nonstd::expected<ImageInfoResult, std::string> GetImageInfoFromMemory(const uint8_t *addr, const size_t datasize, const std::string &uri);

///
/// Decode an OpenEXR image as fp16 (half) WITHOUT widening to fp32. Succeeds
/// only when every channel is stored as HALF in the file (the common HDR-texture
/// case) and the image is scanline (non-tiled, single-part); returns false
/// otherwise so the caller can fall back to the fp32 LoadImageFromMemory path.
/// On success `image` is interleaved RGBA fp16 (PixelFormat::Float, bpp 16).
/// Always defined; returns false when built without LIGHTUSD_WITH_EXR.
///
bool DecodeImageEXRHalf(const uint8_t *bytes, size_t size, const std::string &uri,
                        Image *image, std::string *err);

} // namespace image
} // namespace lightusd
