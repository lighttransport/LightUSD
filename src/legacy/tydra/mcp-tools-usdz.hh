// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.
//
// MCP tools for USDZ conversion and texture processing.
//
#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// usdz_convert: run the full USD->USDZ conversion pipeline on a file.
/// Args: { input: string, output: string, flatten?: bool,
///         arkitCompatible?: bool, metersPerUnit?: number, upAxis?: "X"|"Y"|"Z",
///         resizeTextures?: int, textureFormat?: "keep"|"png"|"jpeg",
///         pngEncoder?: "fpnge"|"fpng", jpegQuality?: int, reencode?: bool }
bool USDZConvert(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err);

/// usdz_pack: pack the current session stage into a USDZ.
/// Args: { uri?: string, arkitCompatible?: bool }
/// When `uri` is omitted, returns base64-encoded USDZ data in `data`.
bool USDZPack(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

/// texture_resize: decode a base64 image, resize, re-encode.
/// Args: { data: base64, max_size?: int, width?: int, height?: int,
///         format?: "png"|"jpeg", pngEncoder?: "fpnge"|"fpng",
///         jpegQuality?: int }
bool TextureResize(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

/// texture_repack: merge channels from base64 images into one image.
/// Args: { channels?: int(1-4), width?: int, height?: int,
///         format?: "png"|"jpeg", pngEncoder?: "fpnge"|"fpng",
///         r/g/b/a: { data?: base64, channel?: int, const?: int } }
bool TextureRepack(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err);

}  // namespace mcp
}  // namespace tydra
}  // namespace lightusd
