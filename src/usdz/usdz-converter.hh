// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// USD -> USDZ / flat USDC / flat USDA converter on the core library.
//
// Loads a USD file, optionally composes and flattens it, applies stage
// metadata fixups, collects referenced textures, optionally resizes /
// re-encodes / fits them to a byte budget, and writes the package.
//
// Material optimization, geometry (mesh merge) optimization and UDIM baking
// are implemented by the JavaScript converter (web/js usdzconvert) only; the
// native converter rejects those options with an explicit error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image-writer.hh"  // image::PngEncoder

namespace lightusd {
namespace usdz {

enum class OutputFormat { USDZ, USDC, USDA };
enum class USDZRootLayerFormat { USDC, USDA };
enum class OutputTextureFormat { KeepOriginal, PNG, JPEG };
enum class FitStrategy { Size, Quality };
enum class UpAxis { Unset, X, Y, Z };
enum class MaterialOptimizationMode { Off, Dedupe, Preview, Atlas };
enum class GeometryOptimizationMode { Off, MergeMeshes };
enum class UDIMBakeMode { Off, Grid, Dense };

struct UsdzConvertOptions {
  std::string input;
  std::string output;
  OutputFormat output_format{OutputFormat::USDZ};
  USDZRootLayerFormat usdz_root_layer_format{USDZRootLayerFormat::USDC};

  bool flatten{true};
  bool arkit_compatible{false};
  double metersPerUnit{0.0};  // <= 0: keep
  UpAxis upAxis{UpAxis::Unset};
  std::string url;        // written into the layer `doc`
  std::string copyright;  // written into the layer `doc`

  int max_texture_size{0};  // longest-edge cap; 0 = keep
  OutputTextureFormat texture_format{OutputTextureFormat::KeepOriginal};
  image::PngEncoder png_encoder{image::PngEncoder::Auto};
  int jpeg_quality{90};
  bool reencode{true};
  bool include_unused_textures{false};

  size_t target_texture_bytes{0};  // 0 = no budget fit
  FitStrategy fit_strategy{FitStrategy::Size};
  int fit_min_texture_size{64};
  int fit_min_jpeg_quality{30};

  // Not implemented natively (see the file comment); non-default values fail.
  MaterialOptimizationMode material_optimization{MaterialOptimizationMode::Off};
  GeometryOptimizationMode geometry_optimization{GeometryOptimizationMode::Off};
  UDIMBakeMode udim_bake{UDIMBakeMode::Off};
  // Tuning for the modes above; accepted (and range-checked by the CLI) so
  // command lines stay compatible with the JavaScript converter.
  size_t udim_max_tiles{100};
  int udim_max_atlas_size{8192};
  size_t udim_memory_budget_bytes{size_t(512) << 20};
  bool udim_cross_tile_split{false};
  int udim_dense_padding{2};
  int udim_subdivision_level{2};
  int material_atlas_size{4096};
  int material_atlas_tile_size{512};
  int material_atlas_padding{2};
  int material_atlas_min_group_size{2};
  int mesh_merge_max_input_faces{2048};
  int mesh_merge_max_input_points{4096};
  int mesh_merge_max_aggregate_faces{65536};
  int mesh_merge_min_group_size{2};

  // Textures are processed serially; accepted for CLI compatibility.
  int num_threads{0};
  size_t texture_memory_budget_bytes{0};

  bool verbose{false};
};

struct UsdzConvertStats {
  size_t num_textures{0};
  size_t num_textures_resized{0};
  size_t num_textures_reencoded{0};
  size_t num_textures_passthrough{0};
  size_t num_unused_textures_included{0};
  size_t num_layers_packaged{0};  // -noFlatten dependency layers
  size_t output_size{0};
};

bool Convert(const UsdzConvertOptions& options, UsdzConvertStats* stats,
             std::string* warn, std::string* err);

// Standalone channel repack (e.g. R=occlusion, G=roughness, B=metallic).
struct RepackChannel {
  std::string input_file;  // image file to sample; empty => use `constant`
  int channel{0};          // 0=R,1=G,2=B,3=A
  uint8_t constant{0};
};

struct RepackSpec {
  int out_channels{4};
  int out_width{0};   // 0 => max width among referenced inputs
  int out_height{0};  // 0 => max height among referenced inputs
  RepackChannel r;
  RepackChannel g;
  RepackChannel b;
  RepackChannel a{"", 0, 255};
};

bool RepackTextureFiles(const RepackSpec& spec, const std::string& output_file,
                        image::PngEncoder png_encoder, std::string* warn,
                        std::string* err);

}  // namespace usdz
}  // namespace lightusd
