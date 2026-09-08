// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file render-data-converter.hh
/// @brief Converter classes and configs extracted from render-data.hh
///
/// Contains RenderSceneConverter, RenderSceneConverterEnv, and related
/// configuration structs (MeshConverterConfig, MaterialConverterConfig,
/// RenderSceneConverterConfig) plus vertex dedup helpers.
///

#pragma once

#include <array>
#include <map>
#include <mutex>
#include <string>

#include "render-data.hh"
#include "image-types.hh"

namespace lightusd {
namespace tydra {

struct MeshVisitorEnv;

//
// Thread-local diagnostic capture for parallel conversion phases.
//
// When a capture is installed (RenderSceneConverter::SetDiagCapture), the
// converter's PushError/PushWarn/AddWarning/PushInfo funnel their text into
// the capture buffers instead of the converter-wide strings. The parallel
// mesh-conversion driver installs one capture per worker slot and merges the
// buffers in original prim order afterwards, so warning/error CONTENT stays
// deterministic regardless of worker scheduling.
//
struct DiagCapture {
  std::string *err{nullptr};
  std::string *warn{nullptr};
  std::string *info{nullptr};

  bool active() const {
    return err || warn || info;
  }
};

namespace detail {

inline DiagCapture *&DiagCaptureTls() {
  thread_local DiagCapture *tls = nullptr;
  return tls;
}

}  // namespace detail

///
/// Texture image loader callback
///
/// The callback function should return TextureImage and Raw image data.
///
/// NOTE: TextureImage::buffer_id is filled in Tydra side after calling this
/// callback. NOTE: TextureImage::colorSpace will be overwritten if
/// `asset:sourceColorSpace` is authored in UsdUVTexture.
///
/// @param[in] asset Asset path
/// @param[in] assetInfo AssetInfo
/// @param[in] assetResolver AssetResolutionResolver context. Please pass
/// DefaultAssetResolutionResolver() if you don't have custom
/// AssetResolutionResolver.
/// @param[out] texImageOut TextureImage info.
/// @param[out] imageData Raw texture image data.
/// @param[inout] userdata User data.
/// @param[out] warn Optional. Warning message.
/// @param[out] error Optional. Error message.
///
/// @return true upon success.
/// termination of visiting Prims.
///
typedef bool (*TextureImageLoaderFunction)(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver, TextureImage *imageOut,
    std::vector<uint8_t> *imageData, void *userdata, std::string *warn,
    std::string *err);

bool DefaultTextureImageLoaderFunction(const value::AssetPath &assetPath,
                                       const AssetInfo &assetInfo,
                                       const AssetResolutionResolver &assetResolver,
                                       TextureImage *imageOut,
                                       std::vector<uint8_t> *imageData,
                                       void *userdata, std::string *warn,
                                       std::string *err);

///
/// UDIM texture support.
///

///
/// One resolved UDIM tile.
///
struct UDIMTile {
  uint32_t udim_id{1001};  // [1001, 1100]
  uint32_t u{0};           // 0-based column = (udim_id - 1001) % 10
  uint32_t v{0};           // 0-based row    = (udim_id - 1001) / 10
  std::string asset_path;  // un-resolved per-tile asset path (prefix+id+suffix)
};

///
/// Result of combining UDIM tiles into a single grid atlas.
///
struct UDIMAtlas {
  lightusd::Image image;   // combined atlas image (8-bit, RGBA)
  uint32_t cols{1};        // number of grid columns
  uint32_t rows{1};        // number of grid rows
  uint32_t min_u{0};       // grid origin (lowest tile column)
  uint32_t min_v{0};       // grid origin (lowest tile row)

  // UV remap so that `uv' = uv * scale + offset` places tile (u,v) into its
  // atlas cell. scale = (1/cols, 1/rows), offset = (-min_u/cols, -min_v/rows).
  vec2 uv_scale{1.0f, 1.0f};
  vec2 uv_offset{0.0f, 0.0f};
};

///
/// Discover the set of UDIM tiles that actually resolve for a UDIM asset path.
///
/// `udimAssetPath` must contain the `<UDIM>` token (see io::IsUDIMPath()).
/// Iterates ids 1001..1100 (capped by `max_tiles`), building
/// `prefix + to_string(id) + suffix` and keeping those that the resolver can
/// resolve. Mirrors OpenUSD `_FindUdimTiles`.
///
/// @return true if at least one tile resolved. `tilesOut` is filled in
/// ascending UDIM id order.
///
bool ExpandUDIMTiles(const std::string &udimAssetPath,
                     const AssetResolutionResolver &assetResolver,
                     int max_tiles, std::vector<UDIMTile> *tilesOut,
                     std::string *warn, std::string *err);

///
/// Combine resolved UDIM tiles into a single grid-atlas image.
///
/// Each tile is decoded, resized (sRGB-aware when `srgb` is true) to the
/// derived per-tile cell size and blitted into its grid cell. Empty cells are
/// left transparent. The longest atlas edge is bounded by `max_atlas_size`.
///
/// @return true on success. `atlasOut` is filled with the combined image and
/// the UV remap.
///
bool BuildUDIMAtlas(const std::vector<UDIMTile> &tiles,
                    const AssetResolutionResolver &assetResolver,
                    int max_atlas_size, bool srgb, UDIMAtlas *atlasOut,
                    std::string *warn, std::string *err);

struct MeshConverterConfig {
  bool triangulate{true};

  // Subdivision surface tessellation level. When > 0, meshes with authored
  // subdivisionScheme are tessellated before downstream Tydra mesh conversion.
  int32_t subdivision_level{0};
  // Per-mesh subdivision overrides keyed by absolute prim path. Values < 0 are
  // ignored; values >= 0 override subdivision_level for that mesh only.
  std::map<std::string, int32_t> subdivision_prim_levels;

  // Triangulation method for polygons with 5+ vertices
  enum class TriangulationMethod {
    Earcut,     // Use earcut algorithm (robust, handles complex polygons)
    TriangleFan // Use simple triangle fan (faster, only for convex polygons)
  };

  TriangulationMethod triangulation_method{TriangulationMethod::Earcut};

  bool validate_geomsubset{true};  // Validate GeomSubset.

  // We may want texcoord data even if the Mesh does not have bound Material.
  // But we don't know which primvar is used as a texture coordinate when no
  // Texture assigned to the mesh(no PrimVar Reader assigned to) Use
  // UsdPreviewSurface setting for it.
  //
  // https://openusd.org/release/spec_usdpreviewsurface.html#usd-sample
  //
  // Also for tangnents/binormals.
  //
  // 'primvars' namespace is omitted.
  //
  std::string default_texcoords_primvar_name{"st"};
  std::string default_texcoords1_primvar_name{
      "st1"};  // for multi texture(available from iOS 16/macOS 13)

  // When true, additionally extract EVERY authored `texCoord2f[]` primvar into a
  // RenderMesh.texcoords slot, not just the material-referenced ones. Lets tools
  // expose secondary UV sets (multi-UV) even when no shader reads them. Off by
  // default so the standard render path's slot assignment is unchanged.
  bool extract_all_texcoords{false};

  // When true, retain RenderMesh.triangulatedFaceCounts (per original face, the
  // number of triangles it produced) instead of freeing it after triangulation.
  // Lets tools map each output triangle back to its source USD face. Off by
  // default (the data is freed as before).
  bool keep_triangulation_intermediates{false};
  std::string default_tangents_primvar_name{"tangents"};
  std::string default_binormals_primvar_name{"binormals"};

  // TODO: tangents1/binormals1 for multi-frame normal mapping?

  // Upperlimit of the number of skin weights per vertex.
  // For realtime app, usually up to 64
  uint32_t max_skin_elementSize = 1024ull * 256ull;

  //
  // Bone reduction: limit the number of bone influences per vertex for GPU skinning.
  // When enabled, only the strongest N bone influences are kept and weights are renormalized.
  //
  bool enable_bone_reduction{false};

  //
  // Target number of bone influences per vertex after reduction.
  // Default is 4, which is standard for real-time GPU skinning (e.g., Three.js, Unity, Unreal).
  // Common values: 2, 4, 8
  //
  uint32_t target_bone_count{4};

  //
  // Round bone count up to standard GPU skinning values without reducing influences.
  // When enabled and enable_bone_reduction is false, the elementSize is rounded up
  // to one of: 4, 8, 16, 32, 48, 64, 80, 96, 128
  // This allows passing all bone influences while ensuring compatibility with
  // GPU skinning systems that expect specific bone counts.
  //
  bool round_bone_count{false};

  //
  // Build vertex indices when vertex attributes are converted to `faceverying`?
  // Similar vertices are merged into single vertex index.
  // (convert vertex attributes from 'facevarying' to 'vertex' variability)
  //
  // Building indices is preferred for renderers which supports single
  // index-buffer only (e.g. OpenGL/Vulkan)
  //
  bool build_vertex_indices{true};

  //
  // When true, and mesh isn't single_indexable, skip BuildIndices for faster
  // and reduced temporary memory processing. Vertex attributes are all expanded
  // to facevertex varying. This option takes precedence over build_vertex_indices
  // when the mesh cannot be single-indexed.
  //
  bool prefer_non_indexed{false};

  //
  // When true, always use BuildVertexIndicesFastImpl (no vertex deduplication)
  // instead of BuildVertexIndicesImpl. This matches the WASM code path where
  // tangent computation is typically skipped. Useful for reproducing
  // WASM-specific behavior in native builds.
  //
  bool force_fast_index_build{false};

  //
  // Compute normals if not present in the mesh.
  // The algorithm computes smoothed normal for shared vertex.
  // Normals are also computed when `compute_tangents_and_binormals` is true
  // and normals primvar is not present in the mesh.
  //
  bool compute_normals{true};

  //
  // Compute tangents and binormals for tangent space normal mapping.
  // But when primary texcoords primvar is not present, tangents and binormals are not computed.
  //
  // NOTE: The algorithm is not robust to compute tangent/binormal for quad/polygons.
  // Set `triangulate` preferred when you want let Tydra compute tangent/binormal.
  //
  // NOTE: Computing tangent frame for multi-texcoord is not supported.
  //
  bool compute_tangents_and_binormals{true};

  //
  // When true, only compute tangents/binormals for meshes that have a bound
  // material with a normal map texture connected. Saves significant memory
  // and CPU for meshes that don't need tangent-space normal mapping.
  //
  bool compute_tangents_only_with_normal_map{true};

  //
  // When true, defer tangent computation — don't compute during ConvertMesh.
  // Instead, mark meshes with tangent_computation_deferred=true.
  // Call RenderSceneConverter::ComputeDeferredTangents() to compute on demand
  // (e.g. at first getTangents() call in WASM bindings).
  //
  bool defer_tangent_computation{false};

  //
  // Tangent computation method selection.
  //
  enum class TangentComputationMethod {
    Lengyel,        // Fast, lightweight (default). Good enough for most use cases.
    MikkTSpace,     // Industry standard (original C impl), higher quality but
                    // significantly slower and more memory-hungry for large meshes.
    FastMikkTSpace, // Optimized MikkTSpace reimplementation (~2x faster than
                    // original, same algorithm semantics at default 180° threshold).
    Hybrid,         // Lengyel base + MikkTSpace-quality welding/averaging.
                    // Near-MikkTSpace quality, ~5-10x faster than MikkTSpace.
  };

  TangentComputationMethod tangent_method{TangentComputationMethod::Lengyel};

  //
  // Tangent storage format selection.
  // Controls how computed tangents are stored in RenderMesh.
  // Packed formats store tangent direction + handedness sign in a single
  // attribute, eliminating separate binormal storage. Bitangent is
  // reconstructed in the shader: B = cross(N, T.xyz) * T.w
  //
  enum class TangentStorageFormat {
    Float3,        // float3 tangent + float3 binormal (24 bytes/vertex, baseline)
    Packed1010102, // INT_2_10_10_10_REV vec4 (4 bytes/vertex, WebGL2 native)
    PackedSNorm8,  // SNORM8x4 vec4 (4 bytes/vertex, widest compat)
    PackedFp16,    // FP16x4 vec4 (8 bytes/vertex, highest packed precision)
  };

#ifdef __EMSCRIPTEN__
  TangentStorageFormat tangent_storage{TangentStorageFormat::PackedSNorm8};
#else
  TangentStorageFormat tangent_storage{TangentStorageFormat::PackedFp16};
#endif

  //
  // Normal storage format selection.
  // PackedSNorm8 reduces normal storage from 12 to 3 bytes/vertex (75% savings).
  // ~1° angular resolution, sufficient for rendering.
  //
  enum class NormalStorageFormat {
    Float3,         // float3 normals (12 bytes/vertex, baseline)
    Packed1010102,  // INT_2_10_10_10_REV xyz (4 bytes/vertex)
    PackedSNorm8,   // SNorm8x3 (3 bytes/vertex, Three.js/glTF compatible)
    PackedSNorm16,  // SNorm16x3 (6 bytes/vertex, higher precision)
  };

#ifdef __EMSCRIPTEN__
  NormalStorageFormat normal_storage{NormalStorageFormat::PackedSNorm8};
#else
  NormalStorageFormat normal_storage{NormalStorageFormat::Float3};
#endif

  //
  // Allowed relative error to check if vertex data is the same.
  // Used for 'facevarying' variability to `vertex` variability conversion in
  // ConvertMesh. Only effective to floating-point vertex data.
  //
  float facevarying_to_vertex_eps = std::numeric_limits<float>::epsilon();

  //
  // Maximum vertex valence (number of face-vertices sharing a position) before
  // the position-bucketed vertex dedup falls back to flatten mode (no dedup).
  // This prevents O(N^2) worst case when a single position is referenced by
  // many faces. When any vertex's degree exceeds this threshold, dedup is
  // skipped entirely and each face-vertex becomes its own unique vertex.
  // Set to 0 to disable the safeguard (always dedup).
  //
  uint32_t max_vertex_valence{16};

  //
  // Sphere tessellation settings.
  // Controls how GeomSphere primitives are tessellated into triangle meshes.
  //
  SphereTessellation sphere_tessellation{SphereTessellation::Icosphere};
  int sphere_subdivisions{4};  // Icosphere subdivision level (0-6, default 4)

  // When true, free GeomMesh data after converting it to save memory usage.
  // For emscripten.
  bool lowmem{false};
};

struct MaterialConverterConfig {
  // purpose name for two-sided material mapping.
  // https://github.com/syoyo/lightusd/issues/120
  std::string default_backface_material_purpose_name{"back"};

  // Assign a generated RenderMaterial to meshes that have no authored material
  // binding. Disabled by default to preserve legacy `material_id == -1`
  // behavior for callers that distinguish unbound geometry.
  bool assign_default_material{false};
  std::string default_material_name{"defaultMaterial"};
  // Explicit UsdRenderSettings selection. Empty uses the stage-level
  // renderSettingsPrimPath and then the linear Rec.709 fallback.
  std::string render_settings_path;
  vec3 default_material_diffuse_color{0.18f, 0.18f, 0.18f};
  float default_material_roughness{0.5f};
  float default_material_metallic{0.0f};
  float default_material_opacity{1.0f};

  // DefaultTextureImageLoader will be used when nullptr;
  TextureImageLoaderFunction texture_image_loader_function{nullptr};
  void *texture_image_loader_function_userdata{nullptr};

  // For UsdUVTexture.
  //
  // Default configuration:
  //
  // - The converter converts 8bit texture to floating point image and texel
  // value is converted to linear space.
  // - Allow missing asset(texture) and asset load failure.
  //
  // Recommended configuration for mobile/WebGL
  //
  // - `preserve_texel_bitdepth` true
  //   - No floating-point image conversion.
  // - `linearize_color_space` true
  //   - Linearlize in CPU, and no sRGB -> Linear conversion in a shader
  //   required.

  // In the UsdUVTexture spec, 8bit texture image is converted to floating point
  // image of range `[0.0, 1.0]`. When this flag is set to false, 8bit and 16bit
  // texture image is converted to floating point image. When this flag is set
  // to true, 8bit and 16bit texture data is stored as-is to save memory.
  // Setting true is good if you want to render USD scene on mobile, WebGL, etc.
  bool preserve_texel_bitdepth{false};

  // Apply the inverse of a color space to make texture image in linear space.
  // When `preserve_texel_bitdepth` is set to true, linearization also preserse
  // texel bit depth (i.e, for 8bit sRGB image, 8bit linear-space image is
  // produced)
  bool linearize_color_space{false};

  //
  // Set scene(working space) colorspace. This space must be linear colorspace.
  // Possible choice is: Linear_sRGB(linear_srgb), Lin_ACEScg(ACEScg/AP1), Lin_DisplayP3(linear_displayp3)
  // W.I.P: Curently Lin_sRGB is only supported.
  //
  ColorSpace scene_color_space{ColorSpace::Lin_sRGB};

  // Allow asset(texture, shader, etc) path with Windows backslashes(e.g.
  // ".\textures\cat.png")? When true, convert it to forward slash('/') on
  // Posixish system(otherwise character is escaped(e.g. '\t' -> tab).
  bool allow_backslash_in_asset_path{true};

  // Allow texture load failure?
  bool allow_texture_load_failure{true};

  // Allow asset(e.g. texture file/shader file) which does not exit?
  bool allow_missing_asset{true};

  // When true, conversion fails fast on material authoring gaps
  // (e.g. `outputs:surface` not authored). When false (default), the
  // gaps are demoted to warnings and conversion proceeds with an
  // unshaded material. Set to true if a downstream renderer requires a
  // complete shading network and the asset is expected to provide one.
  bool strict_material_check{false};

  // --- UDIM texture support ---
  //
  // A UDIM asset path (e.g. `diffuse.<UDIM>.png`) expands to per-tile files
  // numbered 1001..1100, where tile `1000 + (u+1) + 10*v` covers UV region
  // `[u,u+1] x [v,v+1]`.
  //
  // When `combine_udim_tiles` is true (default), the converter loads the
  // resolved tiles through the asset resolver and packs them into a single
  // grid-atlas TextureImage. The referenced mesh UV set is rebaked so each
  // tile lands in its atlas cell, producing an ordinary single-texture
  // material suitable for WebGL/USDZ viewers.
  //
  // When false, tiles are loaded as-is and kept sparse: a
  // `tydra::UDIMTexture` is recorded in `RenderScene::udim_textures` and
  // referenced from the `UVTexture` via `udim_texture_id`. This mode is
  // intended for editing UDIM tiles in the web RenderScene/binding.
  bool combine_udim_tiles{true};

  // Max longest edge (in pixels) of the combined UDIM atlas. The per-tile
  // cell size is derived as the largest power-of-two <=
  // `udim_max_atlas_size / max(grid_cols, grid_rows)`.
  int udim_max_atlas_size{4096};

  // Safety cap on the number of UDIM tiles to load (UDIM is 1001..1100).
  int udim_max_tiles{100};

};

struct RenderSceneConverterConfig {
  // Load texture image data on convert.
  // false: no actual texture file/asset access.
  // App/User must setup TextureImage manually after the conversion.
  bool load_texture_assets{true};

  // Keep GPU-compressed KTX2 textures compressed instead of decoding them to
  // RGBA8. When a resolved `inputs:file` (or its `customData ktx2` companion)
  // is a `.ktx2`, the level-0 block payload is stored verbatim in the buffer
  // and `TextureImage::blockFormat` is set, so a GPU consumer (e.g. lusdview)
  // can upload/transcode the blocks directly instead of re-encoding decoded
  // texels. Requires LIGHTUSD_WITH_TEXTOOLS; ignored otherwise. Non-UDIM only.
  bool keep_compressed_textures{false};

  //
  // Merge meshes with the same material for performant rendering.
  //
  // When enabled, meshes that share the same material and have compatible
  // properties (static transforms, no per-face materials, no skeletal animation,
  // no blend shapes) will be merged into a single mesh.
  //
  // This optimization reduces draw calls in renderers like Three.js, WebGL,
  // and other GPU-based renderers where draw call overhead is significant.
  //
  // Merge criteria:
  // - Same material_id (whole mesh material, not per-face)
  // - No material_subsetMap (per-face materials prevent merging)
  // - Static mesh (no skeletal animation: skel_id == -1)
  // - No blend shapes (targets.empty())
  // - Same global transform matrix (meshes must be in the same world space,
  //   or transforms will be baked into vertex positions)
  //
  // When `bake_transform` is true:
  // - Meshes with different transforms can be merged by baking their
  //   global transforms into vertex positions/normals
  // - This allows more aggressive merging at the cost of losing
  //   individual mesh transforms
  //
  bool merge_meshes{false};

  //
  // Deduplicate RenderScene materials by full shader parameters before mesh
  // merging. Texture parameters are compared by their resolved image/wrap/UDIM
  // identity rather than transient texture object ids.
  //
  // Only effective for RenderScene conversion. Intended to make raw converted
  // scenes with many duplicated materials mergeable without changing the
  // source USD layer.
  //
  bool dedup_materials_by_texture_identity{false};

  //
  // When merging meshes, bake global transforms into vertex data.
  // This allows merging meshes with different transforms by transforming
  // their vertices into world space.
  //
  // Only effective when merge_meshes is true.
  //
  bool merge_meshes_bake_transform{true};

  // Replace the authored node hierarchy with a compact render-only tree after
  // native optimization. This breaks prim-tree fidelity and is intended for
  // realtime viewers that only need renderable objects, lights, and cameras.
  bool flatten_optimized_render_tree{false};

  // Bake USD value clip animations into animation clips.
  bool enable_value_clips{true};

  // If > 0, resample value clips to this sample rate (samples per second).
  // If <= 0, no resampling and metadata sample times are used.
  float value_clip_sample_rate{0.0f};

  // If true, value clip resampling uses value_clip_start_time/value_clip_end_time.
  // If false, range is inferred from clip metadata (`times`/`active`).
  bool value_clip_use_time_range{false};
  double value_clip_start_time{0.0};
  double value_clip_end_time{0.0};

  // Expand UsdGeomPointInstancer prims into RenderScene::instances
  // (one RenderInstance per visible instance; geometry is shared via mesh_id,
  // not duplicated). See RenderSceneConverter::ExpandPointInstancer.
  bool expand_point_instancers{true};

  //
  // Worker threads for parallel per-mesh geometry conversion during
  // ConvertToRenderScene (non-streaming).
  //
  //   0 = auto: use hardware_concurrency() capped at 16 on desktop/native
  //       builds; serial (1) on WASM/single-thread runtimes or when
  //       LIGHTUSD_NO_THREADS is defined.
  //   1 = force the fully serial path (identical to pre-parallel behavior;
  //       useful for deterministic tests and memory-constrained hosts).
  //   N = cap workers at N.
  //
  // Conversion output is byte-identical regardless of the thread count:
  // materials are converted in traversal order before geometry, geometry runs
  // into per-slot outputs, and meshes/nodes/ids are merged back in original
  // prim order. Only diagnostic (error/warning) TEXT interleaving may vary
  // with thread count; scene data does not.
  //
  // Skinned meshes (those authoring skel:* joint data or a skeleton
  // relationship) always convert on the calling thread to keep skeleton
  // registration order deterministic.
  //
  int num_threads{0};
};

//
// Simple packed vertex struct & comparator for dedup.
// https://github.com/huamulan/OpenGL-tutorial/blob/master/common/vboindexer.cpp
//
// Up to 2 texcoords.
// tangent and binormal is included in VertexData, considering the situation
// that tangent and binormal is supplied through user-defined primvar.
//
// TODO: Use spatial hash for robust dedup(consider floating-point eps)
// TODO: Polish interface to support arbitrary vertex configuration.
//
// When TYDRA_USE_INDEX is defined, use array indices instead of values
// to save memory. Index value of -1 (or ~0u for uint32_t) means no attribute.
//

// DEPRECATED: DefaultPackedVertexData, DefaultPackedVertexDataHasher,
// DefaultPackedVertexDataEqual, DefaultVertexInput, DefaultVertexOutput, and
// BuildIndices are kept for external code compatibility but are no longer used
// internally. Internal vertex dedup now uses position-bucketed linear scan.
struct DefaultPackedVertexData {
  //value::float3 position;
  uint32_t point_index;
#ifdef TYDRA_USE_INDEX
  // Use indices into attribute arrays instead of values
  // -1 (or ~0u) means no attribute
  uint32_t normal_index;
  uint32_t uv0_index;
  uint32_t uv1_index;
  uint32_t tangent_index;
  uint32_t binormal_index;
  uint32_t color_index;
  uint32_t opacity_index;
#else
  // Use values directly (original behavior)
  value::float3 normal;
  value::float2 uv0;
  value::float2 uv1;
  value::float3 tangent;
  value::float3 binormal;
  value::float3 color;
  float opacity;
#endif

  // Basic comparator for std::map (fallback to memcmp)
  // For epsilon-based comparison, use DefaultPackedVertexDataCompare with attribute arrays
  bool operator<(const DefaultPackedVertexData &rhs) const {
    return memcmp(reinterpret_cast<const void *>(this),
                  reinterpret_cast<const void *>(&rhs),
                  sizeof(DefaultPackedVertexData)) > 0;
  }
};

struct DefaultPackedVertexDataHasher {
  inline size_t operator()(const DefaultPackedVertexData &v) const {
    // Simple hasher using FNV1 32bit
    // TODO: Use 64bit FNV1?
    // TODO: Use spatial hash or LSH(LocallySensitiveHash) for position value.
    static constexpr uint32_t kFNV_Prime = 0x01000193;
    static constexpr uint32_t kFNV_Offset_Basis = 0x811c9dc5;

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&v);
    size_t n = sizeof(DefaultPackedVertexData);

    uint32_t hash = kFNV_Offset_Basis;
    for (size_t i = 0; i < n; i++) {
      hash = (kFNV_Prime * hash) ^ (ptr[i]);
    }

    return size_t(hash);
  }
};

struct DefaultPackedVertexDataEqual {
  bool operator()(const DefaultPackedVertexData &lhs,
                  const DefaultPackedVertexData &rhs) const {
    return memcmp(reinterpret_cast<const void *>(&lhs),
                  reinterpret_cast<const void *>(&rhs),
                  sizeof(DefaultPackedVertexData)) == 0;
  }
};

template <class PackedVert>
struct DefaultVertexInput {
  //std::vector<value::float3> positions;
  std::vector<uint32_t> point_indices; // Keep int array as std::vector
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<value::float3> normals;
  ChunkedVectorArray<value::float2> uv0s;
  ChunkedVectorArray<value::float2> uv1s;
  ChunkedVectorArray<value::float3> tangents;
  ChunkedVectorArray<value::float3> binormals;
  ChunkedVectorArray<value::float3> colors;
  ChunkedVectorArray<float> opacities;
#else
  std::vector<value::float3> normals;
  std::vector<value::float2> uv0s;
  std::vector<value::float2> uv1s;
  std::vector<value::float3> tangents;
  std::vector<value::float3> binormals;
  std::vector<value::float3> colors;
  std::vector<float> opacities;
#endif

#ifdef TYDRA_USE_INDEX
  // Unique attribute arrays for indexed mode
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<value::float3> unique_normals;
  ChunkedVectorArray<value::float2> unique_uv0s;
  ChunkedVectorArray<value::float2> unique_uv1s;
  ChunkedVectorArray<value::float3> unique_tangents;
  ChunkedVectorArray<value::float3> unique_binormals;
  ChunkedVectorArray<value::float3> unique_colors;
  ChunkedVectorArray<float> unique_opacities;
#else
  std::vector<value::float3> unique_normals;
  std::vector<value::float2> unique_uv0s;
  std::vector<value::float2> unique_uv1s;
  std::vector<value::float3> unique_tangents;
  std::vector<value::float3> unique_binormals;
  std::vector<value::float3> unique_colors;
  std::vector<float> unique_opacities;
#endif
#endif

  size_t size() const { return point_indices.size(); }

  void get(size_t idx, PackedVert &output) const {
    if (idx < point_indices.size()) {
      output.point_index = point_indices[idx];
    } else {
      output.point_index = ~0u; // this case should not happen though
    }
#ifdef TYDRA_USE_INDEX
    // In index mode, store indices to unique attribute arrays
    // The indices will be set by the conversion process
    // For now, we just mark them as not present if no data
    output.normal_index = (idx < normals.size()) ? idx : ~0u;
    output.uv0_index = (idx < uv0s.size()) ? idx : ~0u;
    output.uv1_index = (idx < uv1s.size()) ? idx : ~0u;
    output.tangent_index = (idx < tangents.size()) ? idx : ~0u;
    output.binormal_index = (idx < binormals.size()) ? idx : ~0u;
    output.color_index = (idx < colors.size()) ? idx : ~0u;
    output.opacity_index = (idx < opacities.size()) ? idx : ~0u;
#else
    // Original behavior: store values directly
    if (idx < normals.size()) {
      output.normal = normals[idx];
    } else {
      output.normal = {0.0f, 0.0f, 0.0f};
    }
    if (idx < uv0s.size()) {
      output.uv0 = uv0s[idx];
    } else {
      output.uv0 = {0.0f, 0.0f};
    }
    if (idx < uv1s.size()) {
      output.uv1 = uv1s[idx];
    } else {
      output.uv1 = {0.0f, 0.0f};
    }
    if (idx < tangents.size()) {
      output.tangent = tangents[idx];
    } else {
      output.tangent = {0.0f, 0.0f, 0.0f};
    }
    if (idx < binormals.size()) {
      output.binormal = binormals[idx];
    } else {
      output.binormal = {0.0f, 0.0f, 0.0f};
    }
    if (idx < colors.size()) {
      output.color = colors[idx];
    } else {
      output.color = {0.0f, 0.0f, 0.0f};
    }
    if (idx < opacities.size()) {
      output.opacity = opacities[idx];
    } else {
      // Missing opacity means opaque (USD displayOpacity default is 1.0, and
      // RenderMesh::displayOpacity defaults to 1.0 as well).
      output.opacity = 1.0f;
    }
#endif
  }
};

template <class PackedVert>
struct DefaultVertexOutput {
  //std::vector<value::float3> positions;
  std::vector<uint32_t> point_indices; // Keep int array as std::vector
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<value::float3> normals;
  ChunkedVectorArray<value::float2> uv0s;
  ChunkedVectorArray<value::float2> uv1s;
  ChunkedVectorArray<value::float3> tangents;
  ChunkedVectorArray<value::float3> binormals;
  ChunkedVectorArray<value::float3> colors;
  ChunkedVectorArray<float> opacities;
#else
  std::vector<value::float3> normals;
  std::vector<value::float2> uv0s;
  std::vector<value::float2> uv1s;
  std::vector<value::float3> tangents;
  std::vector<value::float3> binormals;
  std::vector<value::float3> colors;
  std::vector<float> opacities;
#endif

#ifdef TYDRA_USE_INDEX
  // Unique attribute arrays for indexed mode
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<value::float3> unique_normals;
  ChunkedVectorArray<value::float2> unique_uv0s;
  ChunkedVectorArray<value::float2> unique_uv1s;
  ChunkedVectorArray<value::float3> unique_tangents;
  ChunkedVectorArray<value::float3> unique_binormals;
  ChunkedVectorArray<value::float3> unique_colors;
  ChunkedVectorArray<float> unique_opacities;
#else
  std::vector<value::float3> unique_normals;
  std::vector<value::float2> unique_uv0s;
  std::vector<value::float2> unique_uv1s;
  std::vector<value::float3> unique_tangents;
  std::vector<value::float3> unique_binormals;
  std::vector<value::float3> unique_colors;
  std::vector<float> unique_opacities;
#endif
#endif

  size_t size() const { return point_indices.size(); }

  void push_back(const PackedVert &v) {
    point_indices.push_back(v.point_index);
#ifdef TYDRA_USE_INDEX
    // In index mode, we would resolve indices to actual values
    // from the unique arrays when needed for rendering
    // For now, we keep the existing interface but store indices
    // This would need additional logic to resolve indices to values
    normals.push_back({0.0f, 0.0f, 0.0f}); // placeholder
    uv0s.push_back({0.0f, 0.0f});
    uv1s.push_back({0.0f, 0.0f});
    tangents.push_back({0.0f, 0.0f, 0.0f});
    binormals.push_back({0.0f, 0.0f, 0.0f});
    colors.push_back({0.0f, 0.0f, 0.0f});
    opacities.push_back(0.0f);
#else
    normals.push_back(v.normal);
    uv0s.push_back(v.uv0);
    uv1s.push_back(v.uv1);
    tangents.push_back(v.tangent);
    binormals.push_back(v.binormal);
    colors.push_back(v.color);
    opacities.push_back(v.opacity);
#endif
  }
};

// BuildIndices and BuildIndicesWithSpatialHash declarations.
// Implementations are in render-data.cc with explicit instantiations.

//
// out_vertex_indices_remap: corresponding vertexIndex in input.
//
template <class VertexInput, class VertexOutput, class PackedVert,
          class PackedVertHasher, class PackedVertEqual>
void BuildIndices(const VertexInput &input, VertexOutput &output,
                  std::vector<uint32_t> &out_indices, std::vector<uint32_t> &out_point_indices);

class RenderSceneConverterEnv {
 public:
  RenderSceneConverterEnv(const Stage &_stage) : stage(_stage) {}

  RenderSceneConverterConfig scene_config;
  MeshConverterConfig mesh_config;
  MaterialConverterConfig material_config;

  AssetResolutionResolver asset_resolver;

  std::string usd_filename; // Corresponding USD filename to Stage.

  void set_search_paths(const std::vector<std::string> &paths) {
    asset_resolver.set_search_paths(paths);
  }

  const Stage &stage;  // Point to valid Stage object at constructor

  double timecode{value::TimeCode::Default()};
  value::TimeSampleInterpolationType tinterp{
      value::TimeSampleInterpolationType::Linear};

};

//
// Convert USD scenegraph at specified time
// TODO: Use RenderSceneConverterEnv(RenderSceneConverterEnv::timecode)
//
class RenderSceneConverter {
 public:
  RenderSceneConverter() = default;
  RenderSceneConverter(const RenderSceneConverter &rhs) = delete;
  RenderSceneConverter(RenderSceneConverter &&rhs) = delete;

  ///
  /// Set progress callback for monitoring conversion progress.
  ///
  /// @param[in] callback Function to call during conversion to report progress
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetProgressCallback(ProgressCallback callback, void *userptr = nullptr);

  ///
  /// Set detailed progress callback for fine-grained progress monitoring.
  /// This callback provides mesh/material/texture counts during conversion.
  ///
  /// @param[in] callback Function to call during conversion with detailed info
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetDetailedProgressCallback(DetailedProgressCallback callback, void *userptr = nullptr);

  ///
  /// Report mesh conversion progress (for use by MeshVisitor).
  /// Updates internal progress info and calls the detailed callback if set.
  ///
  /// @param[in] meshes_processed Number of meshes processed so far
  /// @param[in] meshes_total Total number of meshes to process
  /// @param[in] mesh_name Name of the current mesh being processed
  /// @param[in] message Progress message
  /// @return true to continue, false to cancel
  ///
  bool ReportMeshProgress(size_t meshes_processed, size_t meshes_total,
                          const std::string& mesh_name, const std::string& message);

  ///
  /// Streaming emit helpers (for use by MeshVisitor and the node builders during
  /// ConvertToRenderSceneStreaming). Each returns true (continue) when no sink is
  /// set or the corresponding slot is unset; otherwise it calls the slot and
  /// returns its bool (false == cancel). They read the just-appended element from
  /// this converter's member array by index.
  ///
  /// True while a streaming sink is active (inside ConvertToRenderSceneStreaming).
  bool HasStreamingSink() const { return _sink != nullptr; }
  bool EmitPhase(StreamPhase phase);
  bool EmitImage(size_t index);
  bool EmitBuffer(size_t index);
  bool EmitTexture(size_t index, const std::string &abs_path);
  bool EmitUdimTexture(size_t index);
  bool EmitMaterial(size_t index, const std::string &abs_path);
  bool EmitMesh(size_t index, const std::string &abs_path);
  bool EmitLight(size_t index, const std::string &abs_path);
  bool EmitCamera(size_t index, const std::string &abs_path);
  bool EmitRootNode(size_t index);
  bool EmitSkeleton(size_t index, const std::string &abs_path);
  bool EmitAnimation(size_t index, const std::string &abs_path);
  bool EmitInstance(size_t index, const std::string &abs_path);
  bool EmitComplete(const RenderScene &scene);

  ///
  /// All-in-one Stage to RenderScene conversion.
  ///
  /// Convert Stage to RenderScene.
  /// Must be called after SetStage, SetMaterialConverterConfig(optional)
  ///
  bool ConvertToRenderScene(const RenderSceneConverterEnv &env, RenderScene *scene);

  ///
  /// Streaming (progressive) variant of ConvertToRenderScene.
  ///
  /// Runs the SAME phases as ConvertToRenderScene and ALSO fully populates
  /// `scene` (so callers get the complete result), while invoking `sink`
  /// callbacks at each production point so a consumer can upload / display
  /// meshes, materials and textures as they are produced.
  ///
  /// Any sink callback returning false cancels conversion: this function then
  /// returns false and `*scene` is left unpopulated (the elements already
  /// delivered via callbacks are the partial result). On success `*scene` is
  /// populated identically to ConvertToRenderScene.
  ///
  /// All callbacks fire synchronously on the calling thread; keep them fast
  /// (enqueue-only) if conversion runs on a worker thread. See `RenderSceneSink`.
  ///
  bool ConvertToRenderSceneStreaming(const RenderSceneConverterEnv &env,
                                     const RenderSceneSink &sink,
                                     RenderScene *scene);

  const std::string &GetInfo() const { return _info; }
  const std::string &GetWarning() const { return _warn; }
  const std::string &GetError() const { return _err; }
  const std::string &GetTimingInfo() const { return _timing_info; }

  // Append a warning line. Public so free-function prim visitors (which hold a
  // RenderSceneConverter*) can record non-fatal degradations.
  void AddWarning(const std::string &msg) {
    const DiagCapture *cap = detail::DiagCaptureTls();
    if (cap && cap->warn) {
      (*cap->warn) += msg + "\n";
    } else {
      _warn += msg + "\n";
    }
  }
  void ClearError() { _err.clear(); }

  void PushWarn(const std::string &msg) { AddWarning(msg); }

  // Prim path <-> index for corresponding array
  // e.g. meshMap: primPath/index to `meshes`.

  // TODO: Move to private?
  StringAndIdMap root_nodeMap;
  StringAndIdMap meshMap;
  StringAndIdMap materialMap;
  StringAndIdMap cameraMap;
  StringAndIdMap lightMap;
  StringAndIdMap textureMap;
  StringAndIdMap imageMap;
  StringAndIdMap bufferMap;
  StringAndIdMap animationMap;
  StringAndIdMap volumeMap;  ///< UsdVol Volume prim path -> volumes index

  // UDIM info cache, keyed by the (un-resolved) `<UDIM>` asset path. Used to
  // restore UDIM remap / sparse-texture linkage when a UDIM texture image is
  // reused from `imageMap`.
  struct UDIMInfo {
    bool is_udim{false};
    vec2 uv_scale{1.0f, 1.0f};
    vec2 uv_offset{0.0f, 0.0f};
    int64_t udim_texture_id{-1};
  };
  std::map<std::string, UDIMInfo> udimInfoMap;

  int default_node{-1};

#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<Node> root_nodes;
  ChunkedVectorArray<RenderMesh> meshes;
  ChunkedVectorArray<RenderMaterial> materials;
  ChunkedVectorArray<RenderCamera> cameras;
  ChunkedVectorArray<RenderLight> lights;
  ChunkedVectorArray<UVTexture> textures;
  ChunkedVectorArray<UDIMTexture> udim_textures;
  ChunkedVectorArray<TextureImage> images;
  ChunkedVectorArray<BufferData> buffers;
  ChunkedVectorArray<SkelHierarchy> skeletons;
  ChunkedVectorArray<AnimationClip> animations;
  ChunkedVectorArray<RenderInstance> instances;  ///< USD instancing (Spec 11.3.3)
  ChunkedVectorArray<RenderVolume> volumes;      ///< UsdVol volumes (OpenVDB)
#else
  std::vector<Node> root_nodes;
  std::vector<RenderMesh> meshes;
  std::vector<RenderMaterial> materials;
  std::vector<RenderCamera> cameras;
  std::vector<RenderLight> lights;
  std::vector<UVTexture> textures;
  std::vector<UDIMTexture> udim_textures;
  std::vector<TextureImage> images;
  std::vector<BufferData> buffers;
  std::vector<SkelHierarchy> skeletons;
  std::vector<AnimationClip> animations;
  std::vector<RenderInstance> instances;  ///< USD instancing (Spec 11.3.3)
  std::vector<RenderVolume> volumes;      ///< UsdVol volumes (OpenVDB)
#endif

  // Rendering working space selected for the current conversion.
  std::string _working_color_space{"lin_rec709_scene"};

  // Pre-discovered skeleton/animation prims for ancestor-based discovery
  // These are set temporarily during ConvertToRenderScene
  const PathPrimMap<Skeleton> *_allSkeletons{nullptr};
  const PathPrimMap<SkelRoot> *_allSkelRoots{nullptr};
  const PathPrimMap<SkelAnimation> *_allAnimations{nullptr};

  ///
  /// Convert GeomMesh to renderer-friendly mesh.
  /// Also apply triangulation when MeshConverterConfig::triangulate is set to
  /// true.
  ///
  /// normals, texcoords, vertexcolors/opacities vertex attributes(built-in
  /// primvars) are converterd to either `vertex` variability(i.e. can be drawn
  /// with single vertex indices) or `facevarying` variability(any of primvars
  /// is `facevarying`. It can be drawn with no indices, but less
  /// efficient(especially vertex has skin weights and blendshapes)).
  ///
  /// Since preferred variability for OpenGL/Vulkan renderer is `vertex`,
  /// ConvertMesh tries to convert `facevarying` attribute to `vertex` attribute
  /// when all shared vertex data is the same. If it fails, but
  /// `MeshConverterConfig.build_indices` is set to true, ConvertMesh builds
  /// vertex indices from `facevarying` and convert variability to 'vertex'.
  ///
  /// Note that `points`, skin weights and BlendShape attributes are remains
  /// with `vertex` variability. (so that we can apply some processing per
  /// point-wise)
  ///
  /// Thus, if you want to render a mesh whose normal/texcoord/etc variability
  /// is `facevarying`, `points`, skin weights and BlendShape attributes would
  /// also need to be converted to `facevarying` to draw.
  ///
  /// Other user defined primvars are not touched by ConvertMesh.
  /// The app need to manually triangulate, change variability of user-defined
  /// primvar if required.
  ///
  /// It is recommended first convert Materials assigned(bounded) to this
  /// GeomMesh(and GeomSubsets) or create your own Materials, and supply
  /// material info with `material_path` and `rmaterial_map`. You may supply
  /// empty material info and assign Material after ConvertMesh manually, but it
  /// will need some steps(Need to find texcoord primvar, triangulate texcoord,
  /// etc). See the implementation of ConvertMesh for details)
  ///
  ///
  /// @param[in] mesh_abs_path USD prim path to this GeomMesh
  /// @param[in] mesh Input GeomMesh
  /// @param[in] material_path USD Material Prim path assigned(bound) to this
  /// GeomMesh. Use tydra::GetBoundPath to get Material path actually assigned
  /// to the mesh.
  /// @param[in] subset_material_path_map USD Material Prim path assigned(bound)
  /// to GeomSubsets in this GeomMesh. key = GeomSubset Prim name.
  /// @param[in] rmaterial_map USD Material Prim path <-> RenderMaterial index
  /// map. Use empty map if no material assigned to this Mesh. If the mesh has
  /// bounded material(including material from GeomSubset), RenderMaterial index
  /// must be obrained using ConvertMaterial method before calling ConvertMesh.
  /// @param[in] material_subsets GeomSubset assigned to this Mesh. Can be empty
  /// when no materialBind GeomSuset assigned to this mesh.
  /// @param[in] blendshapes BlendShape Prims assigned to this Mesh. Can be
  /// empty when no BlendShape assigned to this mesh.
  /// @param[out] dst RenderMesh output
  ///
  /// @return true when success.
  ///
  ///

  ///
  /// Convert GeomCube to RenderMesh by generating tessellated geometry
  ///
  /// @param[in] env Converter environment
  /// @param[in] cube_abs_path Absolute path to the cube primitive
  /// @param[in] cube GeomCube primitive
  /// @param[in] material_path Material path for the cube
  /// @param[in] subset_material_path_map Material subset map
  /// @param[in] rmaterial_map Material ID map
  /// @param[in] material_subsets GeomSubset array
  /// @param[in] blendshapes BlendShape array
  /// @param[out] dst RenderMesh output
  ///
  /// @return true when success.
  ///
  bool ConvertCube(
      const RenderSceneConverterEnv &env, const lightusd::Path &cube_abs_path,
      const lightusd::GeomCube &cube, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  ///
  /// Convert GeomSphere to RenderMesh by generating tessellated geometry
  ///
  /// @param[in] env Converter environment
  /// @param[in] sphere_abs_path Absolute path to the sphere primitive
  /// @param[in] sphere GeomSphere primitive
  /// @param[in] material_path Material path for the sphere
  /// @param[in] subset_material_path_map Material subset map
  /// @param[in] rmaterial_map Material ID map
  /// @param[in] material_subsets GeomSubset array
  /// @param[in] blendshapes BlendShape array
  /// @param[out] dst RenderMesh output
  ///
  /// @return true when success.
  ///
  bool ConvertSphere(
      const RenderSceneConverterEnv &env, const lightusd::Path &sphere_abs_path,
      const lightusd::GeomSphere &sphere, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  bool ConvertCylinder(
      const RenderSceneConverterEnv &env, const lightusd::Path &abs_path,
      const lightusd::GeomCylinder &cylinder, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  bool ConvertCone(
      const RenderSceneConverterEnv &env, const lightusd::Path &abs_path,
      const lightusd::GeomCone &cone, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  bool ConvertCapsule(
      const RenderSceneConverterEnv &env, const lightusd::Path &abs_path,
      const lightusd::GeomCapsule &capsule, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  bool ConvertPlane(
      const RenderSceneConverterEnv &env, const lightusd::Path &abs_path,
      const lightusd::GeomPlane &plane, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  bool ConvertMesh(
      const RenderSceneConverterEnv &env, const lightusd::Path &mesh_abs_path,
      const lightusd::GeomMesh &mesh, const MaterialPath &material_path,
      const std::map<std::string, MaterialPath> &subset_material_path_map,
      //const std::map<std::string, int64_t> &rmaterial_map,
      const StringAndIdMap &rmaterial_map,
      const std::vector<const lightusd::GeomSubset *> &material_subsets,
      const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
          &blendshapes,
      RenderMesh *dst);

  ///
  /// Convert a UsdVol Volume prim into a RenderVolume: resolve each `field:*`
  /// relationship to its field-asset prim, read that prim's `filePath` via the
  /// env asset resolver, decode the referenced `.vdb` grid (usdVol loader) into
  /// a dense float buffer (appended to `buffers`), and fill the RenderVolume.
  ///
  bool ConvertVolume(
      const RenderSceneConverterEnv &env, const std::string &volume_abs_path,
      const lightusd::Volume &volume, RenderVolume *dst);

  ///
  /// Compute tangents/binormals for a RenderMesh that had deferred tangent
  /// computation (tangent_computation_deferred == true).
  /// Call this on demand, e.g. from WASM getTangents() binding.
  /// After successful computation, tangent_computation_deferred is set to false.
  ///
  /// @return true on success, false if normals or texcoords are missing.
  ///
  static bool ComputeDeferredTangents(
      RenderMesh *mesh,
      MeshConverterConfig::TangentComputationMethod method =
          MeshConverterConfig::TangentComputationMethod::Lengyel,
      MeshConverterConfig::TangentStorageFormat storage =
#ifdef __EMSCRIPTEN__
          MeshConverterConfig::TangentStorageFormat::Packed1010102,
#else
          MeshConverterConfig::TangentStorageFormat::PackedFp16,
#endif
      std::string *err = nullptr);

  ///
  /// Convert USD Material/Shader to renderer-friendly Material
  ///
  /// @return true when success.
  ///
  bool ConvertMaterial(const RenderSceneConverterEnv &env,
                       const lightusd::Path &abs_mat_path,
                       const lightusd::Material &material,
                       RenderMaterial *rmat_out);

  ///
  /// Convert UsdPreviewSurface Shader to renderer-friendly PreviewSurfaceShader
  ///
  /// @param[in] shader_abs_path USD Path to Shader Prim with UsdPreviewSurface
  /// info:id.
  /// @param[in] shader UsdPreviewSurface
  /// @param[in] pss_put PreviewSurfaceShader
  ///
  /// @return true when success.
  ///
  bool ConvertPreviewSurfaceShader(const RenderSceneConverterEnv &env,
                                   const lightusd::Path &shader_abs_path,
                                   const lightusd::UsdPreviewSurface &shader,
                                   PreviewSurfaceShader *pss_out,
                                   bool is_materialx = false);

  ///
  /// Convert MaterialX OpenPBR Surface Shader to renderer-friendly OpenPBRSurfaceShader
  ///
  /// @param[in] env Conversion environment
  /// @param[in] shader_abs_path USD Path to Shader Prim with OpenPBRSurface info:id
  /// @param[in] shader OpenPBRSurface
  /// @param[out] openpbr_out OpenPBRSurfaceShader
  ///
  /// @return true when success.
  ///
  bool ConvertOpenPBRSurfaceShader(const RenderSceneConverterEnv &env,
                                    const lightusd::Path &shader_abs_path,
                                    const lightusd::OpenPBRSurface &shader,
                                    OpenPBRSurfaceShader *openpbr_out,
                                    bool is_materialx = false,
                                    bool standard_surface_source = false);

  ///
  /// Convert UsdUvTexture to renderer-friendly UVTexture
  ///
  /// @param[in] tex_abs_path USD Path to Shader Prim with UsdUVTexture info:id.
  /// @param[in] assetInfo assetInfo Prim metadata of given Shader Prim
  /// @param[in] texture UsdUVTexture
  /// @param[in] tex_out UVTexture
  ///
  /// TODO: Retrieve assetInfo from `tex_abs_path`?
  ///
  /// @return true when success.
  ///
  bool ConvertUVTexture(const RenderSceneConverterEnv &env,
                        const Path &tex_abs_path, const AssetInfo &assetInfo,
                        const UsdUVTexture &texture, UVTexture *tex_out);

  ///
  /// Convert SkelAnimation to Tydra Animation.
  ///
  /// @param[in] abs_path USD Path to SkelAnimation Prim
  /// @param[in] skelAnim SkelAnimation
  /// @param[in] anim_out AnimationClip
  ///
  bool ConvertSkelAnimation(const RenderSceneConverterEnv &env,
                        const Path &abs_path, const SkelAnimation &skelAnim,
                        int32_t skeleton_id,
                        AnimationClip *anim_out);

  ///
  /// Extract animation data from xformOps time samples and convert to AnimationClip
  ///
  /// @param[in] env Converter environment
  /// @param[in] abs_path Absolute path to the prim
  /// @param[in] prim_name Prim name
  /// @param[in] xformable Xformable object containing xformOps
  /// @param[in] target_node_index Index of the target node in RenderScene
  /// @param[out] anim_out Output AnimationClip
  /// @return true if animation was extracted, false otherwise
  ///
  bool ExtractXformOpAnimation(const RenderSceneConverterEnv &env,
                        const Path &abs_path,
                        const std::string &prim_name,
                        const Xformable &xformable,
                        int32_t target_node_index,
                        AnimationClip *anim_out);

  /// Extract numeric non-xform attribute time samples from any render node prim
  /// into CustomProperty channels. This covers camera, light, material/shader,
  /// and schema-specific parameter animation when a node exists for the prim.
  bool ExtractPrimPropertyAnimation(const RenderSceneConverterEnv &env,
                        const Prim &prim,
                        const Path &abs_path,
                        int32_t target_node_index,
                        AnimationClip *anim_out);

  ///
  /// Bake USD value-clip animation into AnimationClip by loading clip layers,
  /// resolving active clip selection at sampled stage times,
  /// and decomposing clip xform matrices into TRS tracks.
  ///
  bool ConvertValueClipAnimation(const RenderSceneConverterEnv &env,
                               const Prim &prim,
                               const Path &abs_path,
                               int32_t target_node_index,
                               AnimationClip *anim_out);

  ///
  /// @param[in] env
  /// @param[in] root XformNode
  ///
  bool BuildNodeHierarchy(const RenderSceneConverterEnv &env, const XformNode &node);

  ///
  /// Expand a UsdGeomPointInstancer into RenderScene::instances.
  ///
  /// Emits one RenderInstance per visible instance (mask applied). Geometry is
  /// shared: each RenderInstance references a prototype RenderMesh via mesh_id
  /// (looked up from `meshMap`). Instance transforms are computed via
  /// ComputeInstanceTransformsAtTime() at `env.timecode`.
  ///
  /// @param[in] env Conversion env (provides timecode).
  /// @param[in] instancer_abs_path Absolute path of the PointInstancer prim.
  /// @param[in] pi The PointInstancer.
  /// @param[in] instancer_world World transform of the PointInstancer prim.
  /// @param[in] path_to_global abs_path -> world matrix map (from node tree),
  ///            used to resolve prototype-relative mesh transforms.
  ///
  bool ExpandPointInstancer(
      const RenderSceneConverterEnv &env,
      const std::string &instancer_abs_path, const GeomPointInstancer &pi,
      const value::matrix4d &instancer_world,
      const std::unordered_map<std::string, value::matrix4d> &path_to_global);

  ///
  /// Convert UsdLux lights to renderer-friendly RenderLight
  ///

  bool ConvertSphereLight(const RenderSceneConverterEnv &env,
                          const Path &light_abs_path,
                          const SphereLight &light,
                          RenderLight *rlight_out);

  bool ConvertDistantLight(const RenderSceneConverterEnv &env,
                           const Path &light_abs_path,
                           const DistantLight &light,
                           RenderLight *rlight_out);

  bool ConvertDomeLight(const RenderSceneConverterEnv &env,
                        const Path &light_abs_path,
                        const DomeLight &light,
                        RenderLight *rlight_out);

  bool ConvertRectLight(const RenderSceneConverterEnv &env,
                        const Path &light_abs_path,
                        const RectLight &light,
                        RenderLight *rlight_out);

  bool ConvertDiskLight(const RenderSceneConverterEnv &env,
                        const Path &light_abs_path,
                        const DiskLight &light,
                        RenderLight *rlight_out);

  bool ConvertCylinderLight(const RenderSceneConverterEnv &env,
                            const Path &light_abs_path,
                            const CylinderLight &light,
                            RenderLight *rlight_out);

  bool ConvertGeometryLight(const RenderSceneConverterEnv &env,
                            const Path &light_abs_path,
                            const GeometryLight &light,
                            RenderLight *rlight_out);

  ///
  /// Merge two RenderMesh instances into a single mesh.
  /// The source mesh data is appended to the destination mesh.
  /// Returns false with `err` filled when the meshes are not merge-compatible.
  ///
  bool MergeMeshData(const RenderMesh &src, const value::matrix4d &src_transform,
                     RenderMesh &dst, std::string *err = nullptr);

  ///
  /// Merge meshes with the same material for performant rendering.
  ///
  /// This function merges meshes that share the same material and have
  /// compatible properties into a single mesh. It reduces draw calls
  /// for GPU-based renderers.
  ///
  /// @param[in] env Converter environment containing configuration
  /// @return true upon success
  ///
  bool MergeMeshesImpl(const RenderSceneConverterEnv &env);

  ///
  /// Deduplicate RenderMaterial entries and remap mesh/subset/instance material
  /// ids to the canonical material. Returns the number of removed materials.
  ///
  size_t DeduplicateMaterialsByTextureIdentityImpl();

  ///
  /// Deduplicate UVTexture entries by resolved image/wrap/UDIM identity and
  /// remap all material shader texture references. Returns removed textures.
  ///
  size_t DeduplicateTexturesByIdentityImpl();

  ///
  /// Replace root_nodes with a synthetic render-only root containing renderable
  /// nodes in world space. Intended for viewer optimization only.
  ///
  size_t FlattenOptimizedRenderTreeImpl();

  ///
  /// Build a cheap source-graph signature for duplicate material detection
  /// before full RenderMaterial conversion.
  ///
  bool BuildMaterialSourceSignature(const RenderSceneConverterEnv &env,
                                    const Path &mat_abs_path,
                                    const Material &material,
                                    std::string *signature);

  bool FindMaterialSourceSignature(const std::string &signature,
                                   int64_t *material_id) const;
  void RememberMaterialSourceSignature(const std::string &signature,
                                       int64_t material_id);
  bool GetOrCreateDefaultMaterial(const RenderSceneConverterEnv &env,
                                  int *material_id);

  // Wrapper around GetBoundMaterial with caching.
  // Avoids repeated ancestor walks for sibling prims.
  bool GetBoundMaterialCached(
      const Stage &stage, const Path &abs_path,
      const std::string &purpose, Path *materialPath,
      const Material **material, std::string *err);

 private:
  //
  // Parallel mesh-conversion support.
  //

  // Install a thread-local diagnostic capture (see DiagCapture). Returns the
  // previously installed capture so callers can nest/restore.
  DiagCapture *SetDiagCapture(DiagCapture *capture) {
    DiagCapture *&tls = detail::DiagCaptureTls();
    DiagCapture *prev = tls;
    tls = capture;
    return prev;
  }

  // Mutex guarding the memoization caches below and the skeletons array
  // during parallel geometry conversion. Contention is negligible: the
  // guarded sections are O(1) lookups plus rare first-time inserts.
  mutable std::mutex _cache_mu;

  /// Convert one deferred geometry work item into `dst`. Dispatches to
  /// ConvertMesh / ConvertCube / ... based on item.kind. Errors/warnings are
  /// captured via the TLS diag capture installed by the caller.
  bool ConvertMeshWorkItem(const RenderSceneConverterEnv &env,
                           const struct MeshWorkItem &item, RenderMesh *dst);

  /// Run deferred geometry items (filled by MeshVisitor in collect mode) on a
  /// TaskArena, then merge results into meshes/meshMap bookkeeping in
  /// original traversal order. Items tagged convert_serially run inline
  /// during the merge. Serial-safe: output is byte-identical to converting
  /// every item inline.
  bool ConvertDeferredMeshes(const RenderSceneConverterEnv &env,
                             const std::vector<struct MeshWorkItem> &items,
                             size_t num_workers, MeshVisitorEnv *menv,
                             std::string *err);

  void ExtractXformAnimations(const RenderSceneConverterEnv &env,
                              const XformNode &xform_node);

  double BuildStageInstances(
      const RenderSceneConverterEnv &env,
      const PathPrimMap<GeomPointInstancer> &point_instancers);

  ///
  /// Convert variability of vertex data to 'vertex' or 'facevarying'.
  ///
  /// @param[inout] vattr Input/Output VertexAttribute
  /// @param[in] to_vertex_varing Convert to `vertex` varying when true.
  /// `facevarying` when false.
  /// @param[in] faceVertexCounts faceVertexCounts
  /// @param[in] faceVertexIndices faceVertexIndices
  ///
  /// @return true upon success.
  ///
  bool ConvertVertexVariabilityImpl(
      VertexAttribute &vattr, const bool to_vertex_varying,
      const std::vector<uint32_t> &faceVertexCounts,
      const std::vector<uint32_t> &faceVertexIndices);

  template <typename T, typename Dty>
  bool ConvertPreviewSurfaceShaderParam(
      const RenderSceneConverterEnv &env, const Path &shader_abs_path,
      const TypedAttributeWithFallback<Animatable<T>> &param,
      const std::string &param_name, ShaderParam<Dty> &dst_param,
      bool is_materialx = false);

  ///
  /// Build (single) vertex indices for RenderMesh.
  /// existing `RenderMesh::faceVertexIndices` will be replaced with built indices.
  /// All vertex attributes are converted to 'vertex' variability.
  ///
  /// Limitation: Currently we only supports texcoords up to two(primary(0) and secondary(1)).
  ///
  /// @param[inout] mesh
  ///
  bool BuildVertexIndicesImpl(RenderMesh &mesh, uint32_t max_vertex_valence = 16,
                               float dedup_eps = 0.0f);

  ///
  /// Build (single) vertex indices for RenderMesh.
  /// Skip similarity search for faster processing.
  /// existing `RenderMesh::faceVertexIndices` will be replaced with built indices.
  /// All vertex attributes are converted to 'vertex' variability.
  ///
  /// Limitation: Currently we only supports texcoords up to two(primary(0) and secondary(1)).
  ///
  /// @param[inout] mesh
  ///
  bool BuildVertexIndicesFastImpl(RenderMesh &mesh);

  //
  // Convert skeleton from explicit path (for ancestor-discovered skeletons)
  bool ConvertSkeletonImplWithPath(const RenderSceneConverterEnv &env, const Path &skelPath,
                       SkelHierarchy *out_skel);

  // Convert skeleton from Skeleton pointer directly (more efficient for pre-discovered skeletons)
  bool ConvertSkeletonFromPtr(const RenderSceneConverterEnv &env,
                       const Path &skelPath,
                       const Skeleton &skel,
                       const std::string &primName,
                       SkelHierarchy *out_skel);

  // Convert all SkelAnimation prims after skeleton conversion is complete.
  // Supports multiple animations per skeleton by processing all discovered SkelAnimation prims
  // and finding which Skeleton(s) reference each via their skel:animationSource relationship.
  bool ConvertAllSkelAnimations(const RenderSceneConverterEnv &env);

  // Resolve SkelAnimation blendShapeWeights channels to mesh nodes after the
  // render node hierarchy has stable node indices.
  bool ResolveBlendShapeAnimationTargets();

  /// Process a single XformNode's data into a Node (no children).
  bool BuildSingleNode(
    const RenderSceneConverterEnv &env,
    const std::string &primPath,
    const XformNode &node,
    Node &out_rnode);

  /// Iterative depth-first build of Node hierarchy from XformNode tree.
  bool BuildNodeHierarchyIterative(
    const RenderSceneConverterEnv &env,
    const std::string &parentPrimPath,
    const XformNode &node,
    Node &out_rnode);

  ///
  /// Helper to check if a mesh can be merged with others.
  /// Returns true if the mesh has no skeletal animation, no blend shapes,
  /// and no per-face materials.
  ///
  bool IsMeshMergeable(const RenderMesh &mesh) const;

  void PushInfo(const std::string &msg) {
    const DiagCapture *cap = detail::DiagCaptureTls();
    if (cap && cap->info) {
      (*cap->info) += msg + "\n";
    } else {
      _info += msg + "\n";
    }
  }
  void PushError(const std::string &msg) {
    const DiagCapture *cap = detail::DiagCaptureTls();
    if (cap && cap->err) {
      (*cap->err) += msg + "\n";
    } else {
      _err += msg + "\n";
    }
  }

  ///
  /// Call progress callback if set.
  /// @param[in] progress Progress value between 0.0 and 1.0
  /// @return true to continue, false to cancel
  ///
  bool CallProgressCallback(float progress);

  ///
  /// Call detailed progress callback if set.
  /// @param[in] info Detailed progress information
  /// @return true to continue, false to cancel
  ///
  bool CallDetailedProgressCallback(const DetailedProgressInfo &info);

  ///
  /// Shared implementation behind ConvertToRenderScene (sink == nullptr) and
  /// ConvertToRenderSceneStreaming (sink != nullptr). With a null sink the
  /// Emit* helpers are no-ops, so the code path and output are identical to the
  /// legacy ConvertToRenderScene.
  ///
  bool ConvertToRenderSceneImpl(const RenderSceneConverterEnv &env,
                                RenderScene *scene,
                                const RenderSceneSink *sink);

  std::string _info;
  std::string _err;
  std::string _warn;

  // Progress callback
  ProgressCallback _progress_callback{nullptr};
  void *_progress_userptr{nullptr};

  // Detailed progress callback
  DetailedProgressCallback _detailed_progress_callback{nullptr};
  void *_detailed_progress_userptr{nullptr};

  // Progress state for detailed tracking
  mutable DetailedProgressInfo _progress_info;

  // Streaming sink for ConvertToRenderSceneStreaming. Set for the duration of
  // ConvertToRenderSceneImpl when streaming, nullptr otherwise. Not owned.
  const RenderSceneSink *_sink{nullptr};

  // Reusable buffers for mesh conversion to avoid repeated allocation
  mutable std::vector<value::float3> _tmp_points_buffer;

  // Lookup caches for O(1) skeleton/animation dedup (populated during ConvertToRenderScene)
  std::unordered_map<std::string, int32_t, FNV1StringHash> _skelPathToIndex;
  std::unordered_map<std::string, int32_t, FNV1StringHash> _animPathToIndex;

  // Cached BuildSkelNameToIndexMap results per skeleton ID
  std::unordered_map<int32_t, SkelNameToIndexMap> _skelNameToIndexCache;

  // Precomputed SkelRoot -> Skeleton mapping for fast ancestor discovery.
  std::unordered_map<std::string, std::pair<Path, const Skeleton *>,
                     FNV1StringHash>
      _skelRootToSkeleton;

  // Cached ListUVNames results per material ID
  std::unordered_map<int64_t, StringAndIdMap> _uvNameCache;

  // Cached combined-UDIM UV remaps per material ID, keyed by texcoord primvar
  // name: {scale.x, scale.y, offset.x, offset.y}. Used to rebake mesh UV sets
  // into the UDIM atlas layout.
  std::unordered_map<int64_t, std::map<std::string, std::array<float, 4>>>
      _udimRemapCache;

  // Cached material binding results: key = "prim_path\0purpose" → (found, materialPath, material_ptr).
  // Avoids repeated ancestor walks for sibling prims.
  struct MaterialBindingCacheEntry {
    bool found{false};
    Path materialPath;
    const Material *material{nullptr};
    std::string error;
  };
  std::unordered_map<std::string, MaterialBindingCacheEntry> _materialBindingCache;

  std::unordered_map<std::string, int64_t, FNV1StringHash>
      _materialSourceSignatureCache;

  std::string _timing_info;

  // Cache frequently-referenced value clip assets/stages while converting.
  std::unordered_map<std::string, std::shared_ptr<Layer>> _value_clip_layer_cache;
  std::unordered_map<std::string, std::shared_ptr<Stage>> _value_clip_stage_cache;

  bool LoadValueClipLayer(const RenderSceneConverterEnv &env,
                         const std::string &assetPath,
                         std::shared_ptr<Layer> *layer_out);
  bool LoadValueClipStage(const RenderSceneConverterEnv &env,
                         const std::string &assetPath,
                         std::shared_ptr<Stage> *stage_out);

};

// Defined once in render-data-material.cc. Material assembly calls these
// forms across translation units, so suppress implicit instantiation there.
extern template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<
    value::normal3f, value::color3f>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<value::normal3f>> &,
    const std::string &, ShaderParam<value::color3f> &, bool);
extern template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<
    value::normal3f, value::float3>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<value::normal3f>> &,
    const std::string &, ShaderParam<value::float3> &, bool);
extern template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<float, float>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<float>> &, const std::string &,
    ShaderParam<float> &, bool);

} // namespace tydra
} // namespace lightusd
