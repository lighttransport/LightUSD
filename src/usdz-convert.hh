// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.
//
// usdz-convert: high-level USD -> USDZ conversion pipeline.
//
// Capabilities:
//   - Load USD (USDA/USDC/USDZ), optionally compose/flatten multiple layers.
//   - Resize textures (cap the longest edge).
//   - Re-encode textures (fast PNG via fpnge when available).
//   - Normalize texture asset paths and pack everything into an ARKit-friendly
//     USDZ archive, then validate it (AOUSD Core Spec section 17).
//   - Standalone generic channel repacking (merge separate maps into a single
//     texture, e.g. R=gloss, G=roughness) via RepackTextureFiles().
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/prim-enums.hh"  // Axis
#include "image-writer.hh"     // image::PngEncoder
#include "udim-bake.hh"

namespace lightusd {
namespace usdz {

///
/// Output container format.
/// USDZ : ZIP archive with USDC root layer + embedded textures (default).
/// USDC : single flat binary Crate file (textures stay as external refs).
/// USDA : single flat ASCII file (textures stay as external refs).
///
enum class OutputFormat { USDZ, USDC, USDA };

///
/// Root layer format used inside USDZ output.
///
enum class USDZRootLayerFormat { USDC, USDA };

///
/// Output texture format policy.
/// KeepOriginal : re-encode in the source format (PNG -> PNG via fpnge, etc).
/// PNG          : transcode every texture to PNG (fpnge when available).
/// JPEG         : transcode every texture to JPEG.
///
enum class OutputTextureFormat { KeepOriginal, PNG, JPEG };

///
/// Lever used to shrink textures when fitting to a total-size budget.
/// Size    : reduce texture dimensions (preserve each texture's format).
/// Quality : transcode textures to JPEG and reduce JPEG quality.
///
enum class FitStrategy { Size, Quality };

/// Stage/layer-level shader material optimization for realtime delivery.
/// Dedupe : merge exact duplicate material/shader networks.
/// Preview: convert supported networks to UsdPreviewSurface, then dedupe.
/// Atlas  : build texture atlases for supported materials, then dedupe.
enum class MaterialOptimizationMode { Off, Dedupe, Preview, Atlas };

/// Stage/layer-level geometry optimization for realtime delivery.
/// MergeMeshes: merge eligible static Mesh prims by material binding.
enum class GeometryOptimizationMode { Off, MergeMeshes };

struct UsdzConvertOptions {
  // Input USD file(s). The first entry is the root layer; additional entries
  // are composed in as sublayers (weaker) when `flatten` is enabled.
  std::vector<std::string> inputs;

  // Output .usdz path.
  std::string output;

  // Compose/flatten the stage before writing. Recommended for USDZ delivery.
  bool flatten{true};

  // Apply ARKit-friendly stage metadata fixups (metersPerUnit/upAxis defaults).
  bool arkit_compatible{false};

  // Stage metadata overrides. metersPerUnit <= 0 => leave as-is.
  double metersPerUnit{0.0};
  // Axis::Invalid => leave as-is.
  Axis upAxis{Axis::Invalid};

  // Resize: cap the longest texture edge to this value (0 => no resize).
  int max_texture_size{0};

  // Texture budget fit (0 => disabled). When > 0, all textures are globally
  // shrunk so the sum of their encoded sizes fits this many bytes, using
  // `fit_strategy`. Takes precedence over `max_texture_size`/`texture_format`
  // (those become bounds/initial caps for the search).
  size_t target_texture_bytes{0};
  FitStrategy fit_strategy{FitStrategy::Size};
  int fit_min_texture_size{64};   // floor for the Size strategy
  int fit_min_jpeg_quality{30};   // floor for the Quality strategy

  // Texture output format policy.
  OutputTextureFormat texture_format{OutputTextureFormat::KeepOriginal};

  // PNG encoder backend (Auto prefers fpnge when compiled in).
  image::PngEncoder png_encoder{image::PngEncoder::Auto};

  // JPEG quality (1-100) when (re-)encoding JPEG.
  int jpeg_quality{90};

  // Re-encode textures even when no resize/transcode is required. When false,
  // unmodified textures are copied through byte-for-byte.
  bool reencode{true};

  // Also package image files found under the input layer directories even when
  // no authored UsdUVTexture inputs:file references them. Default false keeps
  // USDZ output limited to referenced texture assets.
  bool include_unused_textures{false};

  // Worker threads for texture decode/resize/encode. 0 = auto
  // (hardware_concurrency), 1 = sequential.
  int num_threads{0};

  // Best-effort cap for concurrent texture decode/resize/encode working memory.
  // 0 preserves the historical thread-count behavior. When non-zero, the
  // worker count is reduced using a conservative per-worker estimate and the
  // producer does not prefetch beyond that worker count. A single texture may
  // still exceed the budget on its own.
  size_t texture_memory_budget_bytes{0};

  // Opt-in UDIM stitching. Caps reject the conversion instead of dropping tiles.
  udim::BakeMode udim_bake{udim::BakeMode::Off};
  size_t udim_max_tiles{100};
  int udim_max_atlas_size{8192};
  size_t udim_memory_budget_bytes{size_t(512) << 20};
  udim::CrossTilePolicy udim_cross_tile{udim::CrossTilePolicy::Reject};
  int udim_dense_padding{2};
  int udim_subdivision_level{2};

  // Optional material/shader optimization. Disabled by default to preserve
  // authored materials exactly.
  MaterialOptimizationMode material_optimization{
      MaterialOptimizationMode::Off};
  int material_atlas_size{4096};
  int material_atlas_tile_size{512};
  int material_atlas_padding{2};
  int material_atlas_min_group_size{2};

  // Optional geometry optimization. Disabled by default because it intentionally
  // breaks authored mesh prim paths and hierarchy.
  GeometryOptimizationMode geometry_optimization{
      GeometryOptimizationMode::Off};
  int mesh_merge_max_input_faces{2048};
  int mesh_merge_max_input_points{4096};
  int mesh_merge_max_aggregate_faces{65536};
  int mesh_merge_min_group_size{2};
  bool mesh_merge_prune_empty_xforms{true};

  // Output container format (USDZ, USDC, or USDA).
  OutputFormat output_format{OutputFormat::USDZ};

  // Root layer format when output_format is USDZ.
  USDZRootLayerFormat usdz_root_layer_format{USDZRootLayerFormat::USDC};

  // Verbose logging to stdout.
  bool verbose{false};

  // Optional metadata.
  std::string url;
  std::string copyright;
};

struct UsdzConvertStats {
  size_t num_udim_sets_baked{0};
  size_t num_udim_tiles_baked{0};
  size_t num_udim_atlases{0};
  size_t num_udim_meshes_remapped{0};
  size_t num_udim_faces_generated{0};
  size_t num_textures{0};
  size_t num_textures_resized{0};
  size_t num_textures_reencoded{0};
  size_t num_textures_passthrough{0};
  size_t num_unused_textures_included{0};
  size_t num_materials_before{0};
  size_t num_materials_after{0};
  size_t num_materials_deduped{0};
  size_t num_materials_preview_converted{0};
  size_t num_materials_skipped{0};
  size_t num_material_atlas_materials{0};
  size_t num_material_atlas_textures{0};
  size_t num_meshes_before{0};
  size_t num_meshes_after{0};
  size_t num_meshes_eligible{0};
  size_t num_meshes_merged{0};
  size_t num_meshes_skipped{0};
  size_t num_mesh_aggregates{0};
  size_t num_faces_merged{0};
  size_t num_points_merged{0};
  size_t output_size{0};
};

///
/// Run the conversion pipeline. Returns true on success.
///
bool Convert(const UsdzConvertOptions &options, UsdzConvertStats *stats,
             std::string *warn, std::string *err);

///
/// Rewrite `UsdUVTexture.inputs:file` asset paths on a Stage according to
/// `remap` (old asset path -> new asset path). Useful when textures are
/// renamed (e.g. transcoded PNG -> JPG) and the references must follow.
///
/// @return number of texture references rewritten.
///
size_t RemapTextureAssetPaths(Stage &stage,
                              const std::map<std::string, std::string> &remap);

///
/// Layer-level equivalent of RemapTextureAssetPaths: rewrite texture asset
/// paths authored in a (composed) Layer's PrimSpecs. Used by streaming USDZ
/// packers that export the flattened Layer directly and need renamed texture
/// references (e.g. transcoded PNG -> JPG) to follow.
///
/// @return number of asset references rewritten.
///
size_t RemapLayerTextureAssetPaths(
    Layer &layer, const std::map<std::string, std::string> &remap);

// ---------------------------------------------------------------------------
// Standalone generic channel repacking.
// ---------------------------------------------------------------------------

///
/// One output channel source for repacking. When `input_file` is empty, the
/// constant value is written into that output channel.
///
struct RepackChannel {
  std::string input_file;  // image file to sample; empty => use `constant`
  int channel{0};          // channel to sample (0=R,1=G,2=B,3=A)
  uint8_t constant{0};     // value used when input_file is empty
};

struct RepackSpec {
  int out_channels{4};  // number of channels in the packed output (1-4)
  int out_width{0};     // 0 => max width among referenced inputs
  int out_height{0};    // 0 => max height among referenced inputs
  RepackChannel r;
  RepackChannel g;
  RepackChannel b;
  RepackChannel a{/*input_file*/ "", /*channel*/ 0, /*constant*/ 255};
};

///
/// Load the referenced image files, pack channels per `spec`, and write the
/// packed image to `outputFile`. The output format is inferred from
/// `outputFile`'s extension (PNG uses fpnge when available).
///
bool RepackTextureFiles(const RepackSpec &spec, const std::string &outputFile,
                        image::PngEncoder png_encoder, std::string *warn,
                        std::string *err);

}  // namespace usdz
}  // namespace lightusd
