// SPDX-License-Identifier: Apache-2.0
//
// UsdVol field resolution shared by the legacy and next render converters.
//
// A Volume binds fields through `field:<name>` relationships to field prims
// (OpenVDBAsset, Field3DAsset). The converters read those prims (at a time
// code) into a FieldRequest; FieldCache resolves it against the referenced
// file the way the UsdVol schema describes:
//
//  - the grid named `fieldName`, the `fieldIndex`-th of the grids sharing
//    that name (OpenUSD's Storm ignores fieldIndex; LightUSD honors it); an
//    empty fieldName selects the `fieldIndex`-th grid of the file. No silent
//    first-grid fallback: a missing grid is an error.
//  - `fieldDataType`, when authored, must agree with the grid's type; a
//    mismatch is a warning and the file's type wins.
//  - `fieldClass` (OpenVDBAsset) falls back to the grid's class metadata.
//  - placement: world = field prim local-to-world * grid index-to-world
//    (UsdVol overview). Storm applies only the Volume's transform, ignoring
//    the field prim's; LightUSD follows the schema.
//
// Files are read once per cache (keyed by resolved path); grid headers are
// listed without decoding voxels, and dense data is decoded lazily per
// (file, grid, options). Field3D files are reported unsupported (no reader
// exists in OpenUSD either).
//
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "usdVol.hh"

namespace lightusd {
namespace tydra {
namespace vol {

// Field prim attributes, as authored at the evaluation time.
struct FieldRequest {
  std::string binding;            // relationship name without "field:" (e.g. "density")
  std::string field_prim;         // field prim path
  std::string prim_type;          // "OpenVDBAsset", "Field3DAsset", ...
  std::string file_path;          // resolved (or loader-resolvable) asset path
  std::string field_name;
  int field_index = 0;
  std::string field_data_type;    // authored fieldDataType token ("" when absent)
  std::string field_class;        // authored OpenVDBAsset fieldClass ("" when absent)
  std::string vector_role_hint = "None";
  // Field prim local-to-world, row-vector USD convention (translation in
  // row 3), as tydra's matrices are stored.
  double field_to_world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

struct ResolvedField {
  FieldRequest request;
  usdVol::VDBGridInfo grid;       // the selected grid's header facts
  std::string grid_class;         // UsdVol fieldClass token (authored, else the file's)
  // True when the field is a signed distance function: the effective class is
  // "levelSet" and the file does not contradict it (a grid whose own metadata
  // says fogVolume stays fog, like the fieldDataType rule, with a warning).
  // Renderers shade these as isosurfaces instead of as density fog.
  bool level_set = false;
  std::string value_type;         // the file's value type (usdVol names)
  int components = 1;
  // Grid index -> world in the USD row-vector convention:
  // world_row = (i, j, k, 1) * index_to_world.
  double index_to_world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  bool has_bounds = false;        // from file metadata (header) or a decoded grid
  double world_min[3] = {0, 0, 0}, world_max[3] = {0, 0, 0};
  std::vector<std::string> warnings;  // e.g. a fieldDataType mismatch
};

// Reads an asset into memory (USDZ members, resolver-backed paths, files).
using AssetReader =
    std::function<bool(const std::string &path, std::vector<uint8_t> *bytes, std::string *err)>;

// Reads plain files; the default AssetReader.
bool ReadFileAsset(const std::string &path, std::vector<uint8_t> *bytes, std::string *err);

class FieldCache {
 public:
  explicit FieldCache(AssetReader reader = ReadFileAsset, size_t max_bytes = 0);
  ~FieldCache();

  // Select and describe the grid a field names. False with *err (and no
  // output change) when the file cannot be read, the grid is missing, or the
  // prim type has no reader.
  bool Resolve(const FieldRequest &request, ResolvedField *out, std::string *err);

  // Dense voxels of a resolved field (decoded once per options). The
  // returned grid lives as long as the cache. `options.max_voxels` 0 uses
  // usdVol's default cap.
  const usdVol::VDBGrid *Dense(const ResolvedField &field, const usdVol::VDBReadOptions &options,
                               std::string *warn, std::string *err);

  // Dense voxels moved out of the cache into *out (the cached entry is
  // dropped and the budget released). Use when the caller keeps the data.
  bool TakeDense(const ResolvedField &field, const usdVol::VDBReadOptions &options,
                 usdVol::VDBGrid *out, std::string *warn, std::string *err);

  // Number of grids in a (cached) file, 0 when it cannot be read.
  size_t GridCount(const std::string &path);

  // The resolved grid as a NanoVDB grid buffer (raw GridData-first bytes,
  // no file header). Cached per grid, so the pointer lives as long as the
  // cache; nullptr on failure.
  const std::vector<uint8_t> *NanoVDB(const ResolvedField &field, usdVol::VDBNanoVDBInfo *info,
                                      std::string *err);

  // World-space samples of a resolved field. `points` holds `count` world
  // positions (3 doubles each); `values` receives `count * components`
  // floats (component-interleaved). Nearest unless `trilinear`.
  bool SampleWorld(const ResolvedField &field, const double *points, size_t count,
                   bool trilinear, std::vector<float> *values, int *components,
                   std::string *err);

  size_t bytes_used() const { return bytes_used_; }

 private:
  struct File {
    std::vector<uint8_t> bytes;
    std::vector<usdVol::VDBGridInfo> grids;
    std::string error;            // non-empty when reading failed (cached)
  };
  // One decoded sparse grid plus its world sampler (sampler is built on first
  // use and holds the grid, so both live here together).
  struct SparseGrid;
  const File *Load(const std::string &path, std::string *err);
  SparseGrid *LoadSparse(const std::string &path, size_t source_index, std::string *err);

  AssetReader reader_;
  size_t max_bytes_ = 0;
  size_t bytes_used_ = 0;
  std::map<std::string, File> files_;
  std::map<std::string, std::unique_ptr<usdVol::VDBGrid>> dense_;
  std::map<std::string, std::unique_ptr<std::vector<uint8_t>>> nanovdb_;
  std::map<std::string, std::unique_ptr<SparseGrid>> sparse_;
};

// UsdVol fieldDataType tokens a grid value type satisfies (e.g. "float" for
// float and half-stored float grids, "float3" for vec3f).
bool FieldDataTypeMatches(const std::string &field_data_type, const std::string &value_type,
                          bool saved_as_half);

// The UsdVol fieldDataType token for a grid value type ("half"/"half3" for
// half-stored float grids, "int" for int32, ...); "" when unknown.
std::string FieldDataTypeFor(const std::string &value_type, bool saved_as_half);

// Grid index -> world (column-vector usdVol layout) as a USD row-vector matrix.
void IndexToWorldRowVector(const usdVol::VDBGridInfo &grid, double out[4][4]);

// Union of the fields' world boxes (the Volume extent when not authored), in
// world space. False when no field has bounds.
bool FieldsWorldBounds(const std::vector<ResolvedField> &fields, double world_min[3],
                       double world_max[3]);

// Resolve a field, falling back to the file's first grid when the requested
// name has no match and `field_index` is 0 (the pre-fieldName authoring
// behaviour the render consumers kept). A non-zero fieldIndex never falls
// back. `*err` describes the primary failure when both attempts fail.
//
// `filter` (optional) is tried on each resolved candidate; when it returns
// false the candidate is skipped (e.g. its dense decode failed), which keeps
// the "first grid that can actually be used" behaviour of the old fallback.
using FieldFilter = std::function<bool(const ResolvedField &)>;
bool ResolveWithFirstGridFallback(FieldCache &cache, const FieldRequest &request,
                                  const FieldFilter &filter, ResolvedField *out,
                                  std::string *err);
inline bool ResolveWithFirstGridFallback(FieldCache &cache, const FieldRequest &request,
                                         ResolvedField *out, std::string *err) {
  return ResolveWithFirstGridFallback(cache, request, FieldFilter(), out, err);
}

}  // namespace vol
}  // namespace tydra
}  // namespace lightusd
