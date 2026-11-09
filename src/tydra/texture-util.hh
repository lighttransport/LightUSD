// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Light Transport Entertainment, Inc.

///
/// @file texture-util.hh
/// @brief Texture processing utilities for material workflows
///
/// Provides utilities for combining and processing textures commonly used
/// in physically-based rendering workflows, particularly for glTF and
/// USD material conversion.
///
/// Key functions:
/// - BuildOcclusionRoughnessMetallicTexture(): Combines separate texture
///   channels into a single ORM (Occlusion-Roughness-Metallic) texture
///   following glTF conventions
///
/// Channel packing follows glTF 2.0 specification:
/// - R channel: Occlusion
/// - G channel: Roughness
/// - B channel: Metallic
/// - A channel: Not used (set to 1.0)
///
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib>

#include "image-types.hh"
#include "image-writer.hh"  // image::PngEncoder / WriteOption
// Resize filter + ResizeImage live in their own header so consumers that do not
// need the PNG encoder can skip image-writer.hh (and the legacy value-type
// layer it reaches) entirely.
#include "resize-image.hh"

namespace lightusd {
namespace tydra {

///
/// Source for one output channel when repacking textures.
/// When `input_index` < 0, the constant value is written instead of sampling.
///
struct ChannelSource {
  int input_index{-1};  // index into the `inputs` vector; <0 => use `constant`
  int channel{0};       // channel to sample from inputs[input_index] (0=R,1=G,..)
  uint8_t constant{0};  // value written when input_index < 0
};

///
/// Generic channel pack specification: each output channel is sourced from a
/// (input image, channel) pair or a constant. Example: out.R = gloss.R,
/// out.G = roughness.R packs two grayscale maps into one image.
///
struct ChannelPackSpec {
  int out_channels{4};  // number of channels in the output image (1-4)
  int out_width{0};     // 0 => max width among referenced inputs
  int out_height{0};    // 0 => max height among referenced inputs
  ChannelSource r;
  ChannelSource g;
  ChannelSource b;
  ChannelSource a{/*input_index*/ -1, /*channel*/ 0, /*constant*/ 255};
};

///
/// Pack channels from multiple 8-bit input images into a single 8-bit output
/// image following `spec`. Referenced inputs are resized (linear) to the output
/// dimensions before sampling.
///
/// @return true on success. `err` is filled on failure.
///
bool PackChannels(const std::vector<Image> &inputs, const ChannelPackSpec &spec,
                  Image *dst, std::string *err = nullptr);

// ---------------------------------------------------------------------------
// Fit textures to a total byte budget.
// ---------------------------------------------------------------------------

///
/// Lever used to shrink textures to meet a total-size budget.
/// Size    : reduce texture dimensions (preserve each texture's format).
/// Quality : transcode to JPEG and reduce JPEG quality (smaller than PNG).
///
enum class FitStrategy { Size, Quality };

///
/// One input texture for FitTexturesToBudget(). When `reencodable` is false
/// (e.g. EXR / non 8-bit), the original bytes are kept and counted as fixed
/// overhead against the budget.
///
struct FitTextureInput {
  Image image;                         // decoded 8-bit image (used when reencodable)
  std::vector<uint8_t> original_bytes; // kept as-is when not reencodable
  std::string ext;                     // lowercase source extension ("png","jpg",...)
  bool reencodable{true};
};

struct FitTextureOutput {
  std::vector<uint8_t> bytes;
  std::string ext;        // final extension ("png"/"jpg" or the original)
  int width{0};
  int height{0};
  bool changed{false};    // true if resized/re-encoded vs the original
};

struct FitTextureOptions {
  size_t target_total_bytes{0};  // budget for the sum of all texture bytes
  FitStrategy strategy{FitStrategy::Size};
  int start_max_size{0};         // initial longest-edge cap (0 = original size)
  int min_texture_size{64};      // floor for the Size search
  int min_jpeg_quality{30};      // floor for the Quality search
  int jpeg_quality{90};          // quality used when not searching quality
  image::PngEncoder png_encoder{image::PngEncoder::Auto};
};

///
/// Shrink a set of textures so the sum of their encoded sizes fits
/// `target_total_bytes`, using the chosen `strategy`. Performs a search over
/// the lever (dimension cap for Size, JPEG quality for Quality) and returns the
/// encoded result per input. If the budget cannot be met even at the lever's
/// floor, the floor is used (best effort) and a message is appended to `warn`.
///
/// @return true on success (outputs produced). `err` is filled on hard failure.
///
bool FitTexturesToBudget(const std::vector<FitTextureInput> &inputs,
                         const FitTextureOptions &opts,
                         std::vector<FitTextureOutput> *out, std::string *warn,
                         std::string *err = nullptr);

///
/// Build combined ORM (Occlusion-Roughness-Metallic) texture for glTF workflow.
///
/// Combines separate occlusion, roughness, and metallic texture channels
/// into a single RGB texture following glTF 2.0 specification:
/// - R channel: Occlusion factor and texture
/// - G channel: Roughness factor and texture
/// - B channel: Metallic factor and texture
/// - A channel: Set to 1.0 (opaque)
///
/// @param[in] occlusionFactor Occlusion multiplier (typically 1.0)
/// @param[in] roughnessFactor Roughness multiplier (typically 1.0)
/// @param[in] metallicFactor Metallic multiplier (typically 1.0)
/// @param[in] occlusionImageData Source occlusion texture data
/// @param[in] occlusionImageWidth Width of occlusion texture
/// @param[in] occlusionImageHeight Height of occlusion texture
/// @param[in] occlusionImageChannels Channels in occlusion texture
/// @param[in] occlusionChannel Which channel to use from occlusion texture
/// @param[in] roughnessImageData Source roughness texture data
/// @param[in] roughnessImageWidth Width of roughness texture
/// @param[in] roughnessImageHeight Height of roughness texture
/// @param[in] roughnessImageChannels Channels in roughness texture
/// @param[in] roughnessChannel Which channel to use from roughness texture
/// @param[in] metallicImageData Source metallic texture data
/// @param[in] metallicImageWidth Width of metallic texture
/// @param[in] metallicImageHeight Height of metallic texture
/// @param[in] metallicImageChannels Channels in metallic texture
/// @param[in] metallicChannel Which channel to use from metallic texture
/// @param[out] dst Combined RGBA texture data
/// @param[out] dstWidth Width of output texture
/// @param[out] dstHeight Height of output texture
/// @return true on success, false on error
///
bool BuildOcclusionRoughnessMetallicTexture(
	const float occlusionFactor,
	const float roughnessFactor,
	const float metallicFactor,
	const std::vector<uint8_t> &occlusionImageData,
	const size_t occlusionImageWidth,
	const size_t occlusionImageHeight,
	const size_t occlusionImageChannels,
	const size_t occlusionChannel,
	const std::vector<uint8_t> &roughnessImageData,
	const size_t roughnessImageWidth,
	const size_t roughnessImageHeight,
	const size_t roughnessImageChannels,
	const size_t roughnessChannel,
	const std::vector<uint8_t> &metallicImageData,
	const size_t metallicImageWidth,
	const size_t metallicImageHeight,
	const size_t metallicImageChannels,
	const size_t metallicChannel,
	std::vector<uint8_t> &dst, // RGBA
  size_t &dstWidth,
  size_t &dstHeight);


} // namespace tydra
} // namespace lightusd
