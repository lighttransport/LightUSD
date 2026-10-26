// SPDX-License-Identifier: Apache-2.0
#include "usdVol.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

// TinyVDB's C implementation is compiled separately (lightusd_tinyvdb). Keep
// this C++ translation unit limited to the public C declarations.
#include "external/tinyvdb/tinyvdb_io.h"
#include "external/tinyvdb/tinyvdb_volume.h"

extern "C" {
// Defined by external/tinyvdb/tinyvdb_to_nanovdb.c (upstream declares it in
// tinyvdb_nanovdb.h, which the vendored subset does not carry). The buffer is
// malloc'd and must be released with free().
tvdb_status_t tvdb_grid_to_nanovdb(const tvdb_grid_t *grid, uint8_t **out_data,
                                   size_t *out_size, tvdb_error_t *err);
}

namespace lightusd {
namespace usdVol {

namespace {

// Dense cell cap when the caller gives none: 640^3 (1 GiB of floats), 384^3
// (216 MiB) where the address space is 32-bit (WASM).
#if defined(__EMSCRIPTEN__) || UINTPTR_MAX == 0xffffffffu
const size_t kDefaultMaxVoxels = size_t(384) * 384 * 384;
#else
const size_t kDefaultMaxVoxels = size_t(640) * 640 * 640;
#endif

// An open .vdb (file or borrowed memory) closed on scope exit.
struct OpenFile {
  tvdb_file_t file{};
  bool open = false;
  ~OpenFile() {
    if (open) tvdb_file_close(&file);
  }
};

std::string Message(const tvdb_error_t &e, tvdb_status_t st) {
  return e.message[0] ? e.message : tvdb_status_string(st);
}

bool Open(OpenFile *f, const uint8_t *data, size_t len, const std::string *path,
          const std::string &uri, std::string *err) {
  tvdb_error_t e{};
  const tvdb_status_t st = path ? tvdb_file_open(&f->file, path->c_str(), nullptr, &e)
                                : tvdb_file_open_memory(&f->file, data, len, nullptr, &e);
  if (st != TVDB_OK) {
    if (err) *err += "Failed to decode VDB (" + uri + "): " + Message(e, st) + "\n";
    return false;
  }
  f->open = true;
  return true;
}

bool IsPointGrid(const tvdb_grid_t &g) {
  const char *t = g.descriptor.grid_type;
  return g.tree.is_point_data_grid || g.tree.is_point_index_grid ||
         (t && (std::strstr(t, "ptidx") || std::strstr(t, "ptdataidx")));
}

const char *TypeName(const tvdb_grid_info_t &info) {
  if (info.is_mask) return "mask";
  switch (info.value_type) {
    case TVDB_VALUE_BOOL: return "bool";
    case TVDB_VALUE_INT32: return "int32";
    case TVDB_VALUE_INT64: return "int64";
    case TVDB_VALUE_FLOAT: return "float";
    case TVDB_VALUE_DOUBLE: return "double";
    case TVDB_VALUE_HALF: return "half";
    case TVDB_VALUE_VEC3I: return "vec3i";
    case TVDB_VALUE_VEC3F: return "vec3f";
    case TVDB_VALUE_VEC3D: return "vec3d";
    default: return "unsupported";
  }
}

const char *ClassToken(tvdb_grid_class_t c) {
  switch (c) {
    case TVDB_GRID_CLASS_LEVEL_SET: return "levelSet";
    case TVDB_GRID_CLASS_FOG_VOLUME: return "fogVolume";
    case TVDB_GRID_CLASS_STAGGERED: return "staggered";
    default: return "unknown";
  }
}

std::string GridName(const tvdb_grid_t &g) {
  return g.descriptor.grid_name ? g.descriptor.grid_name : "";
}

void FillTransform(const double m[4][4], double out[4][4], double voxel_size[3],
                   double translation[3]) {
  std::memcpy(out, m, 16 * sizeof(double));
  for (int a = 0; a < 3; ++a) {
    voxel_size[a] = std::sqrt(m[0][a] * m[0][a] + m[1][a] * m[1][a] + m[2][a] * m[2][a]);
    translation[a] = m[a][3];
  }
}

// Decoded grid -> dense VDBGrid. False (with a warning) skips the grid.
bool Convert(const tvdb_grid_t &source, size_t index, const VDBReadOptions &options,
             VDBGrid *out, std::string *warn) {
  const std::string name = GridName(source);
  if (IsPointGrid(source)) {
    if (warn) *warn += "Skipping OpenVDB point grid '" + name +
                       "'; UsdVol density rendering requires voxel-valued grids.\n";
    return false;
  }
  tvdb_grid_info_t info;
  tvdb_error_t e{};
  if (tvdb_grid_get_info(&source, &info, &e) != TVDB_OK ||
      std::strcmp(TypeName(info), "unsupported") == 0) {
    if (warn) *warn += "Skipping non-scalar VDB grid '" + name + "' of type '" +
                       std::string(source.descriptor.grid_type ? source.descriptor.grid_type : "") +
                       "'.\n";
    return false;
  }
  if (!info.has_bounds) return false;  // nothing active
  if (!info.has_transform) {
    if (warn) *warn += "Skipping VDB grid '" + name + "': unsupported (nonlinear) transform.\n";
    return false;
  }
  const bool vector = info.components == 3;
  const bool keep_vector = vector && options.vector_components;
  tvdb_dense_volume_options_t o{};
  o.max_voxels = options.max_voxels ? options.max_voxels : kDefaultMaxVoxels;
  o.stride = options.downsample ? 0 : 1;
  /* Density/fog volumes treat inactive space as empty (the background counts
   * as outside). A level set's interior is stored as inactive tiles holding
   * -band, so reading those as the (positive) background would turn the
   * interior into "outside" and leave only a thin shell: keep the stored
   * values there. */
  o.inactive_as_background = info.grid_class == TVDB_GRID_CLASS_LEVEL_SET ? 0 : 1;
  o.vector_magnitude = vector && !keep_vector;
  // Layout first, then decode straight into out->data (no second copy).
  tvdb_dense_volume_t d{};
  tvdb_status_t st = tvdb_grid_dense_volume_layout(&source, &o, &d, &e);
  if (st == TVDB_OK) {
    size_t cells = 1;
    for (int a = 0; a < 3; ++a) cells *= size_t(d.dims[a]);  // checked by tinyvdb
    out->data.resize(cells * size_t(d.components));
    st = tvdb_grid_to_dense_volume_buffer(&source, &o, out->data.data(), out->data.size(),
                                          nullptr, &e);
  }
  if (st != TVDB_OK) {
    std::vector<float>().swap(out->data);
    if (warn)
      *warn += "Skipping VDB grid '" + name + "': " +
               (st == TVDB_ERROR_INVALID_ARGUMENT ? std::string("dense extent exceeds voxel limit")
                                                  : Message(e, st)) +
               ".\n";
    return false;
  }
  out->name = name;
  out->value_type = std::string(TypeName(info)) + (vector && !keep_vector ? "-magnitude" : "");
  out->grid_class = ClassToken(info.grid_class);
  out->components = d.components;
  out->stride = d.stride;
  out->source_index = index;
  for (int a = 0; a < 3; ++a) {
    out->origin[a] = d.index_min[a];
    out->dim[a] = d.dims[a];
  }
  FillTransform(info.index_to_world, out->index_to_world, out->voxel_size,
                out->world_translation);
  const float *bg = d.background;
  out->background = vector ? std::sqrt(bg[0] * bg[0] + bg[1] * bg[1] + bg[2] * bg[2]) : bg[0];
  return true;
}

bool ReadAll(const uint8_t *data, size_t len, const std::string *path, const std::string &uri,
             std::vector<VDBGrid> *grids, std::string *warn, std::string *err,
             size_t max_voxels) {
  OpenFile f;
  if (!Open(&f, data, len, path, uri, err)) return false;
  VDBReadOptions options;
  options.max_voxels = max_voxels;
  for (size_t i = 0; i < f.file.num_grids; ++i) {
    tvdb_error_t e{};
    const tvdb_status_t st = tvdb_read_grid(&f.file, i, &e);
    if (st != TVDB_OK) {
      // A grid that cannot be decoded fails the file, as a whole-file read did.
      if (err) *err += "Failed to decode VDB (" + uri + "): " + Message(e, st) + "\n";
      return false;
    }
    VDBGrid grid;
    if (Convert(f.file.grids[i], i, options, &grid, warn)) grids->push_back(std::move(grid));
    tvdb_grid_destroy(&f.file.grids[i], nullptr);  // keep peak memory to one grid
  }
  return !grids->empty();
}

bool ReadInfo(const uint8_t *data, size_t len, const std::string *path, const std::string &uri,
              std::vector<VDBGridInfo> *grids, std::string *err) {
  if (!grids) return false;
  OpenFile f;
  if (!Open(&f, data, len, path, uri, err)) return false;
  for (size_t i = 0; i < f.file.num_grids; ++i) {
    tvdb_error_t e{};
    tvdb_status_t st = tvdb_read_grid_header(&f.file, i, &e);
    tvdb_grid_info_t info{};
    if (st == TVDB_OK) st = tvdb_grid_get_info(&f.file.grids[i], &info, &e);
    if (st != TVDB_OK) {
      if (err) *err += "Failed to read VDB grid header (" + uri + "): " + Message(e, st) + "\n";
      return false;
    }
    VDBGridInfo g;
    g.name = GridName(f.file.grids[i]);
    g.value_type = TypeName(info);
    g.grid_class = ClassToken(info.grid_class);
    g.components = info.components;
    g.source_index = i;
    for (const VDBGridInfo &prev : *grids) g.name_index += prev.name == g.name;
    g.saved_as_half = info.saved_as_half != 0;
    g.point_grid = IsPointGrid(f.file.grids[i]);
    g.has_transform = info.has_transform != 0;
    if (g.has_transform) std::memcpy(g.index_to_world, info.index_to_world, sizeof(g.index_to_world));
    g.has_bounds = info.has_bounds != 0;
    for (int a = 0; a < 3; ++a) {
      g.index_min[a] = info.index_min[a];
      g.index_max[a] = info.index_max[a];
      g.world_min[a] = info.world_min[a];
      g.world_max[a] = info.world_max[a];
    }
    g.active_voxels = info.active_voxels;
    grids->push_back(std::move(g));
  }
  return true;
}

bool ReadOne(const uint8_t *data, size_t len, const std::string *path, const std::string &uri,
             const std::string &field_name, int field_index, const VDBReadOptions &options,
             VDBGrid *grid, std::string *warn, std::string *err) {
  if (!grid) return false;
  if (field_index < 0) {
    if (err) *err += "VDB field index must be nonnegative (" + uri + ").\n";
    return false;
  }
  OpenFile f;
  if (!Open(&f, data, len, path, uri, err)) return false;
  size_t pick = f.file.num_grids;
  int seen = 0;
  for (size_t i = 0; i < f.file.num_grids && pick == f.file.num_grids; ++i) {
    const char *n = tvdb_grid_name(&f.file, i);
    if (!field_name.empty() && field_name != (n ? n : "")) continue;
    if (seen++ == field_index) pick = i;
  }
  if (pick == f.file.num_grids) {
    if (err)
      *err += "VDB (" + uri + ") has no grid " +
              (field_name.empty() ? std::string("#") + std::to_string(field_index)
                                  : "'" + field_name + "'" +
                                        (field_index ? " #" + std::to_string(field_index) : "")) +
              ".\n";
    return false;
  }
  tvdb_error_t e{};
  const tvdb_status_t st = tvdb_read_grid(&f.file, pick, &e);
  if (st != TVDB_OK) {
    if (err) *err += "Failed to decode VDB grid (" + uri + "): " + Message(e, st) + "\n";
    return false;
  }
  return DenseFromGrid(&f.file.grids[pick], pick, uri, options, grid, warn, err);
}

}  // namespace

bool ReadVDBFromMemory(const uint8_t *data, size_t len, const std::string &uri,
                       std::vector<VDBGrid> *grids, std::string *warn,
                       std::string *err, size_t max_voxels) {
  if (!grids) {
    if (err) (*err) += "usdVol::ReadVDBFromMemory: `grids` output is null.\n";
    return false;
  }
  if (!data || len == 0) {
    if (err) (*err) += "usdVol::ReadVDBFromMemory: empty buffer (" + uri + ").\n";
    return false;
  }
  return ReadAll(data, len, nullptr, uri, grids, warn, err, max_voxels);
}

bool ReadVDBFromFile(const std::string &filepath, std::vector<VDBGrid> *grids,
                     std::string *warn, std::string *err, size_t max_voxels) {
  if (!grids) return false;
  return ReadAll(nullptr, 0, &filepath, filepath, grids, warn, err, max_voxels);
}

bool DenseGridBox(const VDBGrid &grid, double bmin[3], double bmax[3]) {
  const double s = double(grid.stride > 0 ? grid.stride : 1);
  double lo[3], hi[3];
  for (int a = 0; a < 3; ++a) {
    lo[a] = double(grid.origin[a]) - 0.5;
    hi[a] = double(grid.origin[a]) + s * double(grid.dim[a]) - 0.5;
    bmin[a] = std::numeric_limits<double>::infinity();
    bmax[a] = -std::numeric_limits<double>::infinity();
  }
  const double (*m)[4] = grid.index_to_world;  // column vectors
  for (int corner = 0; corner < 8; ++corner) {
    const double p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1],
                         (corner & 4) ? hi[2] : lo[2]};
    for (int a = 0; a < 3; ++a) {
      const double w = m[a][0] * p[0] + m[a][1] * p[1] + m[a][2] * p[2] + m[a][3];
      bmin[a] = std::min(bmin[a], w);
      bmax[a] = std::max(bmax[a], w);
    }
  }
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (r == c ? !(m[r][c] > 0.0) : m[r][c] != 0.0) return false;
  return true;
}

bool ReadVDBGridInfo(const uint8_t *data, size_t len, const std::string &uri,
                     std::vector<VDBGridInfo> *grids, std::string *err) {
  if (!data || !len) {
    if (err) *err += "usdVol::ReadVDBGridInfo: empty buffer (" + uri + ").\n";
    return false;
  }
  return ReadInfo(data, len, nullptr, uri, grids, err);
}

bool ReadVDBGridInfoFromFile(const std::string &filepath, std::vector<VDBGridInfo> *grids,
                             std::string *err) {
  return ReadInfo(nullptr, 0, &filepath, filepath, grids, err);
}

bool DenseFromGrid(const tvdb_grid *grid, size_t source_index, const std::string &uri,
                   const VDBReadOptions &options, VDBGrid *out, std::string *warn,
                   std::string *err) {
  if (!grid || !out) {
    if (err) *err += "usdVol::DenseFromGrid: null grid or output.\n";
    return false;
  }
  VDBGrid dense;
  std::string why;
  if (!Convert(*grid, source_index, options, &dense, &why)) {
    if (err) *err += why.empty() ? "VDB grid (" + uri + ") has no active voxels.\n" : why;
    return false;
  }
  if (warn && !why.empty()) *warn += why;
  *out = std::move(dense);
  return true;
}

bool ReadVDBGrid(const uint8_t *data, size_t len, const std::string &uri,
                 const std::string &field_name, int field_index,
                 const VDBReadOptions &options, VDBGrid *grid, std::string *warn,
                 std::string *err) {
  if (!data || !len) {
    if (err) *err += "usdVol::ReadVDBGrid: empty buffer (" + uri + ").\n";
    return false;
  }
  return ReadOne(data, len, nullptr, uri, field_name, field_index, options, grid, warn, err);
}

bool ReadVDBGridFromFile(const std::string &filepath, const std::string &field_name,
                         int field_index, const VDBReadOptions &options, VDBGrid *grid,
                         std::string *warn, std::string *err) {
  return ReadOne(nullptr, 0, &filepath, filepath, field_name, field_index, options, grid, warn,
                 err);
}

namespace {

// NanoVDB conversion lives in tinyvdb_to_nanovdb.c (the vendored subset does
// not ship upstream's tinyvdb_nanovdb.h; the C file declares the same
// prototype itself).
uint32_t NanoVDBGridType(const std::string &value_type) {
  // PNANOVDB_GRID_TYPE_* ids.
  if (value_type == "float") return 1;
  if (value_type == "double") return 2;
  if (value_type == "int32") return 4;
  if (value_type == "int64") return 5;
  if (value_type == "vec3f") return 6;
  if (value_type == "vec3d") return 7;
  if (value_type == "mask") return 8;
  if (value_type == "half") return 9;
  return 0;
}

}  // namespace

bool ReadVDBGridNanoVDB(const uint8_t *data, size_t len, const std::string &uri,
                        const std::string &field_name, int field_index,
                        std::vector<uint8_t> *nano, VDBNanoVDBInfo *info, std::string *err) {
  if (!nano) {
    if (err) *err += "usdVol::ReadVDBGridNanoVDB: `nano` output is null.\n";
    return false;
  }
  if (!data || !len) {
    if (err) *err += "usdVol::ReadVDBGridNanoVDB: empty buffer (" + uri + ").\n";
    return false;
  }
  if (field_index < 0) {
    if (err) *err += "VDB field index must be nonnegative (" + uri + ").\n";
    return false;
  }
  OpenFile f;
  if (!Open(&f, data, len, nullptr, uri, err)) return false;
  size_t pick = f.file.num_grids;
  int seen = 0;
  for (size_t i = 0; i < f.file.num_grids && pick == f.file.num_grids; ++i) {
    const char *n = tvdb_grid_name(&f.file, i);
    if (!field_name.empty() && field_name != (n ? n : "")) continue;
    if (seen++ == field_index) pick = i;
  }
  if (pick == f.file.num_grids) {
    if (err)
      *err += "VDB (" + uri + ") has no grid " +
              (field_name.empty() ? std::string("#") + std::to_string(field_index)
                                  : "'" + field_name + "'" +
                                        (field_index ? " #" + std::to_string(field_index) : "")) +
              ".\n";
    return false;
  }
  tvdb_error_t e{};
  const tvdb_status_t st = tvdb_read_grid(&f.file, pick, &e);
  if (st != TVDB_OK) {
    if (err) *err += "Failed to decode VDB grid (" + uri + "): " + Message(e, st) + "\n";
    return false;
  }
  return NanoVDBFromGrid(&f.file.grids[pick], uri, nano, info, err);
}

bool NanoVDBFromGrid(const tvdb_grid *grid, const std::string &uri, std::vector<uint8_t> *nano,
                     VDBNanoVDBInfo *info, std::string *err) {
  if (!grid || !nano) {
    if (err) *err += "usdVol::NanoVDBFromGrid: null grid or output.\n";
    return false;
  }
  tvdb_error_t e{};
  tvdb_grid_info_t gi{};
  tvdb_status_t st = tvdb_grid_get_info(grid, &gi, &e);
  if (st != TVDB_OK) {
    if (err) *err += "Failed to inspect VDB grid (" + uri + "): " + Message(e, st) + "\n";
    return false;
  }
  const std::string value_type = TypeName(gi);
  const uint32_t grid_type = NanoVDBGridType(value_type);
  if (grid_type == 0) {
    if (err)
      *err += "VDB grid '" + GridName(*grid) + "' of type '" +
              (grid->descriptor.grid_type ? grid->descriptor.grid_type : "") +
              "' has no NanoVDB representation (" + uri + ").\n";
    return false;
  }
  uint8_t *raw = nullptr;
  size_t raw_size = 0;
  st = tvdb_grid_to_nanovdb(grid, &raw, &raw_size, &e);
  if (st != TVDB_OK || !raw || raw_size == 0) {
    if (raw) std::free(raw);
    if (err)
      *err += "Failed to convert VDB grid to NanoVDB (" + uri + "): " + Message(e, st) + "\n";
    return false;
  }
  nano->assign(raw, raw + raw_size);
  std::free(raw);
  if (info) {
    info->grid_type = grid_type;
    info->components = gi.components > 0 ? gi.components : 1;
    info->value_type = value_type;
    info->grid_class = ClassToken(gi.grid_class);
    std::memcpy(info->index_to_world, gi.index_to_world, sizeof(info->index_to_world));
  }
  return true;
}

}  // namespace usdVol
}  // namespace lightusd
