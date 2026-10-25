// SPDX-License-Identifier: Apache-2.0
//
// Built-in OpenVDB (.vdb) import plugin for UsdVol.
//
// Import only (decode .vdb grids into dense float voxels). Writing .vdb is
// not supported here. Uses the vendored tinyvdb (src/external/tinyvdb).
//
// Supported: bool, mask, int32, int64, half, float and double grids, and
// vec3i/vec3f/vec3d grids (magnitude by default, or three interleaved
// components), with ZIP/LZ4/Zstd payloads. Point-data/index grids are
// rejected explicitly. Any linear transform (uniform/nonuniform scale,
// translation, affine) is reported as an index-to-world matrix.
//
// Typical usage (a UsdVol OpenVDBAsset field prim references a .vdb file):
//
//   def Volume "volume" {
//     rel field:density = </volume/density>
//     def OpenVDBAsset "density" {
//       asset filePath = @density.vdb@
//       token fieldName = "density"
//     }
//   }
//
// ReadVDBGrid decodes just the grid a field names (fieldName, then fieldIndex
// among grids sharing that name); ReadVDBGridInfo lists the grids of a file
// from their headers without decoding voxel data.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace usdVol {

///
/// One OpenVDB grid decoded into a dense float voxel array.
///
struct VDBGrid {
  std::string name;             // grid / field name (e.g. "density")
  // Logical source type: "bool", "mask", "int32", "int64", "float" (also
  // half-stored floats), "double", or "vec3i"/"vec3f"/"vec3d" with a
  // "-magnitude" suffix when vectors are reduced to one channel.
  std::string value_type;
  // UsdVol OpenVDBAsset fieldClass token from the grid's class metadata:
  // "levelSet", "fogVolume", "staggered" or "unknown".
  std::string grid_class = "unknown";
  int components = 1;           // 1, or 3 for vector components

  int origin[3] = {0, 0, 0};    // index-space origin (min active voxel coord)
  int dim[3] = {0, 0, 0};       // dense cells per axis
  int stride = 1;               // index voxels per dense cell per axis (downsampled > 1)

  // Axis-aligned summary of the transform (column lengths and translation);
  // exact for scale/translate maps. index_to_world is the full linear map:
  // world = index_to_world * (i, j, k, 1), voxel centers at integer i, j, k.
  double voxel_size[3] = {1.0, 1.0, 1.0};
  double world_translation[3] = {0.0, 0.0, 0.0};
  double index_to_world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

  float background = 0.0f;      // inactive (background) value (component 0)
  size_t source_index = 0;      // position of the grid in its file

  // Dense voxels, length dim[0]*dim[1]*dim[2]*components.
  // Layout: index = c + components*(x + dim[0]*(y + dim[1]*z)) (x contiguous).
  // Cell (x, y, z) averages index voxels origin + stride*(x, y, z) ..
  // + stride - 1; inactive voxels read the background.
  std::vector<float> data;
};

///
/// Object-space box of a dense grid under the cell-centre sampling convention
/// renderers use (cell x is centred at bmin + (x + 0.5) * (bmax - bmin) / dim):
/// index box [origin - 0.5, origin + stride * dim - 0.5] through
/// index_to_world. Returns false when the map is not a positive axis-aligned
/// scale + translation, in which case the box is the axis-aligned bound of the
/// transformed index box and an axis-aligned sampler is only approximate.
///
bool DenseGridBox(const VDBGrid &grid, double bmin[3], double bmax[3]);

///
/// Header-only facts about a grid (no voxel data decoded).
///
struct VDBGridInfo {
  std::string name;
  std::string value_type;       // as VDBGrid::value_type, without "-magnitude"
  std::string grid_class = "unknown";
  int components = 1;
  size_t source_index = 0;      // position in the file
  int name_index = 0;           // occurrence among grids of the same name (fieldIndex)
  bool saved_as_half = false;
  bool point_grid = false;      // point-index/point-data: not a voxel field
  bool has_transform = false;
  double index_to_world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  // From the file's bounding-box metadata, when the writer stored it.
  bool has_bounds = false;
  int index_min[3] = {0, 0, 0}, index_max[3] = {0, 0, 0};  // inclusive
  double world_min[3] = {0, 0, 0}, world_max[3] = {0, 0, 0};
  uint64_t active_voxels = 0;
};

struct VDBReadOptions {
  size_t max_voxels = 0;          // dense cell cap (0 = default: 640^3, 384^3 on 32-bit/WASM)
  bool vector_components = false; // vec3 grids as three channels (else magnitude)
  bool downsample = false;        // over the cap: box-filter by the smallest stride that fits
};

///
/// Decode every grid of a .vdb file into dense float grids.
///
/// `max_voxels` caps the dense voxel count of any single grid (0 = built-in
/// default); larger grids are skipped with a warning. Returns true when at
/// least one grid decoded; on failure `err` holds a message and `grids` may
/// be partially filled.
///
bool ReadVDBFromFile(const std::string &filepath, std::vector<VDBGrid> *grids,
                     std::string *warn = nullptr, std::string *err = nullptr,
                     size_t max_voxels = 0);

///
/// Decode a .vdb from an in-memory buffer (e.g. via an asset resolver).
/// `uri` is used only for diagnostic messages.
///
bool ReadVDBFromMemory(const uint8_t *data, size_t len, const std::string &uri,
                       std::vector<VDBGrid> *grids, std::string *warn = nullptr,
                       std::string *err = nullptr, size_t max_voxels = 0);

///
/// List the grids of a .vdb from their headers (cheap: no voxel decoding).
///
bool ReadVDBGridInfo(const uint8_t *data, size_t len, const std::string &uri,
                     std::vector<VDBGridInfo> *grids, std::string *err = nullptr);
bool ReadVDBGridInfoFromFile(const std::string &filepath, std::vector<VDBGridInfo> *grids,
                             std::string *err = nullptr);

///
/// Decode the one grid a UsdVol field selects: the `field_index`-th grid
/// (0-based) named `field_name`, or the `field_index`-th grid of the file
/// when `field_name` is empty. Other grids are not decoded.
///
bool ReadVDBGrid(const uint8_t *data, size_t len, const std::string &uri,
                 const std::string &field_name, int field_index,
                 const VDBReadOptions &options, VDBGrid *grid,
                 std::string *warn = nullptr, std::string *err = nullptr);
bool ReadVDBGridFromFile(const std::string &filepath, const std::string &field_name,
                         int field_index, const VDBReadOptions &options, VDBGrid *grid,
                         std::string *warn = nullptr, std::string *err = nullptr);

///
/// Facts about a grid converted to NanoVDB (the GPU-ready sparse form).
///
struct VDBNanoVDBInfo {
  // NanoVDB grid type id (PNANOVDB_GRID_TYPE_*: 1 float, 2 double, 4 int32,
  // 5 int64, 6 vec3f, 7 vec3d, 8 mask, 9 half, 11 bool).
  uint32_t grid_type = 0;
  int components = 1;
  std::string value_type;         // as VDBGrid::value_type
  std::string grid_class = "unknown";
  double index_to_world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

///
/// Convert the grid a field selects into an in-memory NanoVDB grid: the raw
/// GridData-first byte stream (no NanoVDB file header), with the grid's full
/// affine index-to-world map and class metadata. Lower-case bool and vec3i
/// grids have no NanoVDB type and fail with an error.
///
bool ReadVDBGridNanoVDB(const uint8_t *data, size_t len, const std::string &uri,
                        const std::string &field_name, int field_index,
                        std::vector<uint8_t> *nano, VDBNanoVDBInfo *info = nullptr,
                        std::string *err = nullptr);

}  // namespace usdVol
}  // namespace lightusd
