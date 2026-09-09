// SPDX-License-Identifier: Apache 2.0
// Copyright (c) 2019 - 2023, Syoyo Fujita.
// Copyright (c) 2023 - Present, Light Transport Entertainment Inc.

#ifndef LIGHTUSD_HH_
#define LIGHTUSD_HH_

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifdef LIGHTUSD_LOCAL_DEBUG_PRINT
#include <iostream>  // dbg
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// TODO: Use std:: version for C++17
#include "nonstd/expected.hpp"
#include "nonstd/optional.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "image-types.hh"
#include "core/prim.hh"       // Prim (transitively: value-types, path, prim-enums, prim-metas)
#include "core/prim-spec.hh"  // PrimSpec, Layer, FileFormatHandler
#include "texture-types.hh"
// NOTE: the concrete schema headers (usdGeom/usdLux/usdShade/usdSkel) are NOT
// included here — lightusd.hh's own API uses only Stage/Prim/Layer, so pulling the
// (template-heavy) schema definitions into every consumer of this umbrella header
// is unnecessary. Code that uses concrete schema types (GeomMesh, Material,
// SphereLight, SkelRoot, ...) must include the relevant "usd{Geom,Lux,Shade,Skel}.hh"
// directly.
#include "stage.hh"
#include "asset-resolution.hh"
#include "security-policy.hh"


namespace lightusd {

constexpr int version_major = 1;
constexpr int version_minor = 0;
constexpr int version_micro = 0;
constexpr auto version_rev = "rc4";  // extra revision suffix(e.g. "rc.1")

struct USDLoadOptions {
  ///
  /// Set the number of threads to use when parsing USD scene.
  /// -1 = use # of system threads(CPU cores/threads).
  ///
  int num_threads{-1};

  // Set the maximum memory limit advisorily(including image data).
  // This feature would be helpful if you want to load USDZ model in mobile
  // device.
  int32_t max_memory_limit_in_mb{static_cast<int32_t>(
      security_policy::kDefaultInputLimitBytes / (1024ull * 1024ull))};

  ///
  /// TODO: Deprecate
  /// Loads asset data(e.g. texture image, audio). Default is true.
  /// If you want to load asset data in your own way or don't need asset data to
  /// be loaded, Set this false.
  ///
  bool load_assets{true};

  ///
  /// (Deprecated. to be removed in the future release)
  /// Do composition on load(Load sublayers, references, etc)
  /// For USDZ model, this should be false.
  ///
  bool do_composition{false};

  ///
  /// (Deprecated. to be removed in the future release)
  /// Following load flags are valid when `do_composition` is set `true`.
  ///
  bool load_sublayers{false}; // true: Load `subLayers`
  bool load_references{false}; // true: Load `references`
  bool load_payloads{false}; // true: Load `paylod` at top USD loading(no lazy loading).

  ///
  /// Max MBs allowed for each asset file(e.g. jpeg)
  ///
  uint32_t max_allowed_asset_size_in_mb{static_cast<uint32_t>(
      security_policy::kDefaultAssetLimitBytes / (1024ull * 1024ull))};

  ///
  /// For texture size
  ///
  uint32_t max_image_width = 2048;
  uint32_t max_image_height = 2048;
  uint32_t max_image_channels = 4;

  ///
  /// For usdSkel
  ///
  bool strict_usdSkel_check{false}; // Strict usdSkel parsing check when true.

  ///
  /// allowedToken
  ///
  bool strict_allowedToken_check{false}; // Make parse error when token value is not in allowedToken list(when the schema defines allowedToken list)

  ///
  /// apiSchema
  ///
  bool strict_apiSchema_check{false}; // Make parse error when unknown apiSchema

  ///
  /// Shader property(input/output) type conformance.
  ///
  /// When false(default), a UsdPreviewSurface-family Shader property whose
  /// authored Sdf type does not match the canonical schema type(inferred from
  /// `info:id` / the UsdPreviewSurface spec) is accepted with a warning. This
  /// matches OpenUSD, which does not validate shader output types and resolves
  /// the real type from the Sdr registry. lightusd has no Sdr registry, so the
  /// hand-written shader schema type is used as the canonical type.
  ///
  /// When true, such a type mismatch is a hard parse error.
  ///
  /// Example: Unreal-exported `token outputs:result` on `UsdPrimvarReader_float2`
  /// (canonical type is `float2`).
  ///
  bool strict_shader_type_check{false};

  ///
  /// Treat recoverable reader/reconstruction errors as load failures.
  ///
  /// The default remains permissive for compatibility: readers may return a
  /// Stage/Layer while reporting diagnostics through `err`. Applications that
  /// require a clean load can opt into hard-failure semantics.
  ///
  bool strict_loading{false};

  // ==========================================================================
  // MaterialX Validation Options
  // ==========================================================================

  ///
  /// Validate MaterialX connection types (e.g., color3f output to float input)
  ///
  bool validate_mtlx_connection_types{false};

  ///
  /// Validate MaterialX info:id against known shader definitions
  ///
  bool validate_mtlx_info_id{false};

  ///
  /// Validate MaterialX connection targets exist
  ///
  bool validate_mtlx_connection_targets{false};

  ///
  /// Check for duplicate prim names within a scope (e.g., NodeGraph)
  ///
  bool validate_mtlx_duplicate_names{false};

  ///
  /// Validate extract/combine index bounds (0-2 for color3/vector3)
  ///
  bool validate_mtlx_index_bounds{false};

  ///
  /// Enable all MaterialX validation options at once
  ///
  bool strict_mtlx_check{false};

  ///
  /// Show detailed USDA error reports without stack snipping or source-line
  /// truncation.
  ///
  bool error_detail{false};

  ///
  /// User-defined fileformat hander.
  /// key = file(asset) extension(`.` excluded. example: 'mtlx', 'obj').
  ///
  std::map<std::string, FileFormatHandler> fileformats;

  /// Enable mmap zero-copy for uncompressed USDC arrays.
  /// When true, defers reading eligible uncompressed USDC arrays and records
  /// their offsets instead. File-based loaders keep the mmap/file buffer alive
  /// through Stage ownership. Memory-based loaders still require the caller's
  /// input buffer to remain alive while the Stage uses zero-copy arrays.
  bool mmap_zero_copy{false};

  Axis upAxis{Axis::Y};

  ///
  /// Progress callback function type.
  /// @param[in] progress Progress value between 0.0 and 1.0
  /// @param[in] userptr User-provided pointer for custom data
  /// @return true to continue parsing, false to cancel
  ///
  std::function<bool(float progress, void *userptr)> progress_callback{nullptr};
  void *progress_userptr{nullptr};
};


// TODO: Provide profiles for loader option.
// e.g.
// - Embedded(e.g. web, tigh resource size limit for security)
// - Realtime(moderate resource size limit)
// - DCC(for data conversion. Unlimited resource size)

///
/// Options for writing USD files.
///
struct USDWriteOptions {
  ///
  /// Enable zstd compression for output file.
  /// When enabled, the entire USD file is wrapped with zstd compression.
  /// Also auto-detected when filename ends with ".zst" extension.
  ///
  bool use_zstd_compression{false};

  ///
  /// Zstd compression level (1-22).
  /// Higher values give better compression but slower speed.
  /// Default is 5 (good balance of speed and ratio).
  ///
  int zstd_compression_level{5};

  ///
  /// Enable OpenUSD-compatible tagged compression for float[]/double[] arrays
  /// in USDC output (code 'i' = integers, 't' = lookup table; see
  /// doc/crate-writer.md). Default OFF: such arrays are written uncompressed.
  ///
  bool compress_float_arrays{false};
};

//

///
/// Load USD(USDA/USDC/USDZ) from a file.
/// Automatically detect file format.
///
/// @param[in] filename USD filename(UTF-8)
/// @param[out] stage USD stage(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDFromFile(const std::string &filename, Stage *stage,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options = USDLoadOptions());

///
/// Load USD(USDA/USDC/USDZ) from memory.
/// Automatically detect file format.
///
/// @param[in] addr Memory address of USD data
/// @param[in] length Byte length of USD data
/// @param[in] filename Filename(can be empty).
/// @param[out] stage USD stage(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &filename, Stage *stage,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());

///
/// Load USDZ(zip) from a file.
/// It will load first USD file in USDZ container.
///
/// @param[in] filename USDZ filename(UTF-8)
/// @param[out] stage USD stage(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDZFromFile(const std::string &filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options = USDLoadOptions());

#ifdef _WIN32
// WideChar(Unicode filename) version
bool LoadUSDZFromFile(const std::wstring &filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options = USDLoadOptions());
#endif


///
/// Load USDZ(zip) from memory.
/// It will load first USD file in USDZ container.
///
/// @param[in] addr Memory address of USDZ data
/// @param[in] length Byte length of USDZ data
/// @param[in] filename Filename(can be empty).
/// @param[out] stage USD stage(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDZFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options = USDLoadOptions());

struct USDZAsset
{
  // USDZ root is the first archive entry. Retain its ordered name separately
  // because asset_map is sorted by key.
  std::string root_asset_name;

  // key: asset name(USD, Image, Audio, ...), value = byte begin/end in USDZ data.
  std::map<std::string, std::pair<size_t, size_t>> asset_map;

  // When mmapped, `data` is empty, and `addr`(Usually pointer to mmaped address) and `size`  are set.
  // When non-mmapped, `data` holds the copy of whole USDZ data.
  std::vector<uint8_t> data; // USDZ itself
  const uint8_t *addr{nullptr};
  size_t size{0}; // in bytes.

  bool is_mmaped() const {
    return (addr != nullptr) && (size > 0);
  }
};

///
/// Read USDZ(zip) asset info from a file.
///
/// Whole file content(USDZ) is copied into USDZAsset::data.
/// To avoid copying USDZ asset data, keep the source buffer (or an
/// io-util.hh::MMapFile mapping) alive and call ReadUSDZAssetInfoFromMemory()
/// with asset_on_memory set to true.
///
/// @param[in] filename USDZ filename(UTF-8)
/// @param[out] asset USDZ asset info.
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] max_file_size_in_mb Maximum file size
///
/// @return true upon success
///
bool ReadUSDZAssetInfoFromFile(const std::string &filename, USDZAsset *asset,
  std::string *warn, std::string *err, size_t max_file_size_in_mb = 16384ull);

///
/// Read USDZ(zip) asset info from memory.
///
/// @param[in] addr Memory address
/// @param[in] asset_on_memory When true, do not copy USDZ data(`length` bytes from `addr` address) to USDZAsset. Instead just retain `addr` and `length` in USDZAsset. Memory address `addr` must be retained during any asset data in USDZAsset is accessed. When false, USDZ data is copied into USDZAsset.
///
/// @param[out] asset USDZ asset info.
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
///
/// @return true upon success
///
bool ReadUSDZAssetInfoFromMemory(const uint8_t *addr, const size_t length, const bool asset_on_memory, USDZAsset *asset,
  std::string *warn, std::string *err);

///
/// Handy utility API to setup AssetResolutionResolver to load asset data from USDZ data.
///
/// @param[inout] resolver Add asset resolution to the resolver. The resolver retains the pointer to USDZAsset.
/// @param[in] pusdzAsset Pointer to data struct(USDZAsset struct). Must be retained until there is (potential) access to any asset, since AssetResolutionResolver and FileSystemHandler loads an asset from this struct.
///
/// @return upon success and setup `resolver` and `fsHandler`.
///
bool SetupUSDZAssetResolution(
  AssetResolutionResolver &resolver,
  const USDZAsset *pusdzAsset);

///
/// Default AssetResolution handler for USDZ(read asset from USDZ container)
///
int USDZResolveAsset(const char *asset_name, const std::vector<std::string> &search_paths, std::string *resolved_asset_name, std::string *err, void *userdata);
int USDZSizeAsset(const char *resolved_asset_name, uint64_t *nbytes, std::string *err, void *userdata);
int USDZReadAsset(const char *resolved_asset_name, uint64_t req_bytes, uint8_t *out_buf, uint64_t *nbytes, std::string *err, void *userdata);

///
/// Load USDC(binary) from a file.
///
/// @param[in] filename USDC filename(UTF-8)
/// @param[out] stage USD stage(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDCFromFile(const std::string &filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options = USDLoadOptions());

///
/// Load USDC(binary) from a memory.
///
/// @param[in] addr Memory address of USDC data
/// @param[in] length Byte length of USDC data
/// @param[in] filename Filename(can be empty).
/// @param[out] stage USD stage.
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDCFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options = USDLoadOptions());

///
/// Load USDA(ascii) from a file.
///
/// @param[in] filename USDA filename(UTF-8)
/// @param[out] stage USD stage.
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDAFromFile(const std::string &filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options = USDLoadOptions());

///
/// Load USDA(ascii) from a memory.
///
/// @param[in] addr Memory address of USDA data
/// @param[in] length Byte length of USDA data
/// @param[in[ base_dir Base directory(can be empty)
/// @param[out] stage USD stage.
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadUSDAFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &base_dir, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options = USDLoadOptions());

///
/// For composition
///

///
/// Load USD(USDA/USDC/USDZ) from a file and return it as Layer.
/// Automatically detect file format.
///
/// @param[in] filename USD filename(UTF-8)
/// @param[out] layer USD layer(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadLayerFromFile(const std::string &filename, Layer *layer,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options = USDLoadOptions());

///
/// Load USD(USDA/USDC/USDZ) from memory and return it as Layer.
/// Automatically detect file format.
///
/// @param[in] addr Memory address of USD data
/// @param[in] length Byte length of USD data
/// @param[in] filename Corresponding Filename(can be empty).
/// @param[out] layer USD layer(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadLayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &filename, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());


bool LoadUSDALayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &filename, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());

bool LoadUSDCLayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &filename, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());

bool LoadUSDZLayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &filename, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());

///
/// Load USD(USDA/USDC/USDZ) layer using AssetResolution resolver.
/// This API would be useful if you want to load USD from custom storage(e.g, on Android), URI(web), DB, etc.
/// Automatically detect file format.
///
/// resolved_asset_name must be resolved asset name using AssetResolutionResolver::resolve()
///
/// @param[in] resolver AssetResolution resolver.
/// @param[in] resolved_asset_name Resolved asset name.
/// @param[out] layer USD layer(scene graph).
/// @param[out] warn Warning message.
/// @param[out] err Error message(filled when the function returns false)
/// @param[in] options Load options(optional)
///
/// @return true upon success
///
bool LoadLayerFromAsset(AssetResolutionResolver &resolver,
                       const std::string &resolved_asset_name, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options = USDLoadOptions());

enum class USDZRootLayerFormat {
  USDC,
  USDA,
};

struct USDZWriteOptions {
  USDZRootLayerFormat root_layer_format{USDZRootLayerFormat::USDC};

  // Optional overrides for the USDC root-layer writer resource limits (bytes).
  // 0 keeps the built-in default (intentionally small on WASM to guard against
  // resource exhaustion on untrusted input). Raise these to write large scenes
  // (e.g. dense meshes / roundtrip testing).
  int64_t max_file_size_bytes{0};
  int64_t max_memory_bytes{0};
};

///
/// Save Stage as USDZ (uncompressed ZIP package) to a file.
///
/// The root layer is written as USDC (binary Crate format) by default as the
/// first entry in the ZIP archive. Use USDZWriteOptions to request a USDA root
/// layer. Additional assets (images, audio) can be included via the `assets`
/// parameter.
///
/// Conforms to AOUSD Core Spec section 17 (USDZ Package Format):
/// - Uncompressed ZIP (store method)
/// - 64-byte data alignment for all entries
/// - First entry is the root .usdc or .usda layer
/// - Allowed types: .usd, .usda, .usdc, .png, .jpg, .jpeg, .exr, .avif, .m4a, .mp3, .wav
///
/// @param[in] filename Output USDZ filename
/// @param[in] stage USD Stage to write
/// @param[in] assets Additional assets to include: key=archive name, value=file data
/// @param[out] warn Warning messages
/// @param[out] err Error messages
/// @return true upon success
///
bool SaveAsUSDZToFile(const std::string &filename, const Stage &stage,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      std::string *warn, std::string *err);

bool SaveAsUSDZToFile(const std::string &filename, const Stage &stage,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      const USDZWriteOptions &options,
                      std::string *warn, std::string *err);

bool SaveAsUSDZToFile(const std::string &filename, const Layer &layer,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      const USDZWriteOptions &options,
                      std::string *warn, std::string *err);

///
/// Save Stage as USDZ to memory.
///
/// @param[in] stage USD Stage to write
/// @param[in] assets Additional assets to include
/// @param[out] output Output USDZ data
/// @param[out] warn Warning messages
/// @param[out] err Error messages
/// @return true upon success
///
bool SaveAsUSDZToMemory(const Stage &stage,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        std::string *warn, std::string *err);

bool SaveAsUSDZToMemory(const Stage &stage,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        const USDZWriteOptions &options,
                        std::string *warn, std::string *err);

bool SaveAsUSDZToMemory(const Layer &layer,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        const USDZWriteOptions &options,
                        std::string *warn, std::string *err);

///
/// Validate a USDZ archive against AOUSD Core Spec section 17.
///
/// Checks: ZIP magic, uncompressed storage, 64-byte alignment,
/// root layer is first entry, allowed file extensions.
///
/// @param[in] addr Memory address of USDZ data
/// @param[in] length Byte length
/// @param[out] warn Warning messages (non-fatal issues)
/// @param[out] err Error messages (spec violations)
/// @return true if valid USDZ
///
bool ValidateUSDZ(const uint8_t *addr, size_t length,
                  std::string *warn, std::string *err);

// Test if input is any of USDA/USDC/USDZ format.
// Optionally returns detected format("usda", "usdc", or "usdz") to
// `detected_format` when a given file/binary is a USD format.
bool IsUSD(const std::string &filename, std::string *detected_format = nullptr);
bool IsUSD(const uint8_t *addr, const size_t length,
            std::string *detected_format = nullptr);

// Test if input is USDA format.
bool IsUSDA(const std::string &filename);
bool IsUSDA(const uint8_t *addr, const size_t length);

// Test if input is USDC(Crate binary) format.
bool IsUSDC(const std::string &filename);
bool IsUSDC(const uint8_t *addr, const size_t length);

// Test if input is USDZ(Uncompressed ZIP) format.
bool IsUSDZ(const std::string &filename);
bool IsUSDZ(const uint8_t *addr, const size_t length);

// Test if input is zstd-compressed data (by magic number).
// This is for file-level compression, not internal USDC LZ4 compression.
bool IsZstdCompressed(const uint8_t *addr, const size_t length);

}  // namespace lightusd

#endif  // LIGHTUSD_HH_
