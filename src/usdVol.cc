// SPDX-License-Identifier: Apache-2.0
#include "usdVol.hh"

#include <cmath>
#include <cstring>

// TinyVDB's C implementation is compiled separately (lightusd_tinyvdb). Keep
// this C++ translation unit limited to the public C declarations.
#include "external/tinyvdb/tinyvdb_io.h"
#include "external/tinyvdb/tinyvdb_volume.h"

namespace lightusd {
namespace usdVol {

namespace {

const size_t kDefaultMaxVoxels = size_t(640) * 640 * 640;

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
  tvdb_dense_volume_options_t o{};
  o.max_voxels = options.max_voxels ? options.max_voxels : kDefaultMaxVoxels;
  o.stride = options.downsample ? 0 : 1;
  o.inactive_as_background = 1;
  tvdb_dense_volume_t d{};
  const tvdb_status_t st = tvdb_grid_to_dense_volume(&source, &o, &d, &e);
  if (st != TVDB_OK) {
    if (warn)
      *warn += "Skipping VDB grid '" + name + "': " +
               (st == TVDB_ERROR_INVALID_ARGUMENT ? std::string("dense extent exceeds voxel limit")
                                                  : Message(e, st)) +
               ".\n";
    return false;
  }
  const bool vector = d.components == 3;
  const bool keep_vector = vector && options.vector_components;
  out->name = name;
  out->value_type = std::string(TypeName(info)) + (vector && !keep_vector ? "-magnitude" : "");
  out->grid_class = ClassToken(info.grid_class);
  out->components = keep_vector ? 3 : 1;
  out->stride = d.stride;
  out->source_index = index;
  size_t cells = 1;
  for (int a = 0; a < 3; ++a) {
    out->origin[a] = d.index_min[a];
    out->dim[a] = d.dims[a];
    cells *= size_t(d.dims[a]);
  }
  FillTransform(info.index_to_world, out->index_to_world, out->voxel_size,
                out->world_translation);
  const float *bg = d.background;
  out->background = vector ? std::sqrt(bg[0] * bg[0] + bg[1] * bg[1] + bg[2] * bg[2]) : bg[0];
  if (!vector || keep_vector) {
    out->data.assign(d.data, d.data + cells * size_t(d.components));
  } else {
    out->data.resize(cells);
    for (size_t i = 0; i < cells; ++i) {
      const float *v = d.data + 3 * i;
      out->data[i] = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
  }
  tvdb_dense_volume_free(&d);
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
  VDBGrid out;
  std::string why;
  if (!Convert(f.file.grids[pick], pick, options, &out, &why)) {
    if (err) *err += why.empty() ? "VDB grid (" + uri + ") has no active voxels.\n" : why;
    return false;
  }
  if (warn && !why.empty()) *warn += why;
  *grid = std::move(out);
  return true;
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

}  // namespace usdVol
}  // namespace lightusd
