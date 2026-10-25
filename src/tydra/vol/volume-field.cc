// SPDX-License-Identifier: Apache-2.0
#include "volume-field.hh"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <sys/stat.h>
#include <sys/types.h>

#include "external/tinyvdb/tinyvdb_io.h"
#include "external/tinyvdb/tinyvdb_sparse_tools.h"
#include "external/tinyvdb/tinyvdb_volume.h"

namespace lightusd {
namespace tydra {
namespace vol {

namespace {

// usdVol's index_to_world is column-vector (world = M * (i, j, k, 1)); USD
// stores row vectors. Transpose into the USD convention.
void ToRowVector(const double m[4][4], double out[4][4]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r][c] = m[c][r];
}

// Row-vector product a * b.
void Multiply(const double a[4][4], const double b[4][4], double out[4][4]) {
  double t[4][4];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
      t[r][c] = s;
    }
  std::memcpy(out, t, sizeof(t));
}

// World box of the voxel-corner box of [lo, hi] under a row-vector matrix.
void WorldBox(const double m[4][4], const int lo[3], const int hi[3], double wlo[3],
              double whi[3]) {
  for (int a = 0; a < 3; ++a) {
    wlo[a] = INFINITY;
    whi[a] = -INFINITY;
  }
  for (int c = 0; c < 8; ++c) {
    const double q[3] = {(c & 1) ? hi[0] + 0.5 : lo[0] - 0.5, (c & 2) ? hi[1] + 0.5 : lo[1] - 0.5,
                         (c & 4) ? hi[2] + 0.5 : lo[2] - 0.5};
    for (int a = 0; a < 3; ++a) {
      const double w = q[0] * m[0][a] + q[1] * m[1][a] + q[2] * m[2][a] + m[3][a];
      if (w < wlo[a]) wlo[a] = w;
      if (w > whi[a]) whi[a] = w;
    }
  }
}

std::string DenseKey(const ResolvedField &f, const usdVol::VDBReadOptions &o) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "|%zu|%zu|%d|%d", f.grid.source_index, o.max_voxels,
                int(o.vector_components), int(o.downsample));
  return f.request.file_path + buf;
}

}  // namespace

// One decoded sparse grid plus its world sampler. Declared in the header as
// an incomplete type so tinyvdb stays out of the public includes.
struct FieldCache::SparseGrid {
  tvdb_file_t file{};
  bool open = false;
  tvdb_volume_sampler_t *sampler = nullptr;
  ~SparseGrid() {
    if (sampler) tvdb_volume_sampler_destroy(sampler);
    if (open) tvdb_file_close(&file);
  }
};

FieldCache::~FieldCache() = default;

bool ReadFileAsset(const std::string &path, std::vector<uint8_t> *bytes, std::string *err) {
  // Asset paths come from the stage: read only regular files (opening a FIFO
  // blocks; devices such as /dev/zero never end) up to kMaxVolumeFileBytes,
  // checked before opening.
#if defined(_WIN32)
  struct _stat64 st;
  const bool regular = _stat64(path.c_str(), &st) == 0 && (st.st_mode & _S_IFREG);
#else
  struct stat st;
  const bool regular = ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
  if (!regular) {
    if (err) *err = "cannot open " + path + " (not a regular file)";
    return false;
  }
  if (st.st_size < 0 || uint64_t(st.st_size) > kMaxVolumeFileBytes) {
    if (err) *err = path + " exceeds the volume file size limit";
    return false;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  const std::streamoff size = in ? std::streamoff(in.tellg()) : std::streamoff(-1);
  if (size < 0) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  if (uint64_t(size) > kMaxVolumeFileBytes) {
    if (err) *err = path + " exceeds the volume file size limit";
    return false;
  }
  bytes->resize(size_t(size));
  in.seekg(0);
  if (size && !in.read(reinterpret_cast<char *>(bytes->data()), size)) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  return true;
}

bool FieldDataTypeMatches(const std::string &t, const std::string &vt, bool saved_as_half) {
  if (t.empty()) return true;
  if (vt == "float") return t == "float" || (saved_as_half && t == "half");
  if (vt == "double") return t == "double" || (saved_as_half && t == "half");
  if (vt == "int32") return t == "int" || t == "uint";
  if (vt == "int64") return t == "int64";
  if (vt == "bool") return t == "bool";
  if (vt == "mask") return t == "mask" || t == "bool";
  if (vt == "vec3f") return t == "float3" || (saved_as_half && t == "half3");
  if (vt == "vec3d") return t == "double3" || (saved_as_half && t == "half3");
  if (vt == "vec3i") return t == "int3";
  return false;
}

std::string FieldDataTypeFor(const std::string &vt, bool saved_as_half) {
  if (vt == "float") return saved_as_half ? "half" : "float";
  if (vt == "double") return "double";
  if (vt == "int32") return "int";
  if (vt == "int64") return "int64";
  if (vt == "bool") return "bool";
  if (vt == "mask") return "mask";
  if (vt == "vec3f") return saved_as_half ? "half3" : "float3";
  if (vt == "vec3d") return "double3";
  if (vt == "vec3i") return "int3";
  return std::string();
}

void IndexToWorldRowVector(const usdVol::VDBGridInfo &grid, double out[4][4]) {
  ToRowVector(grid.index_to_world, out);
}

FieldCache::FieldCache(AssetReader reader, size_t max_bytes)
    : reader_(reader ? std::move(reader) : AssetReader(ReadFileAsset)), max_bytes_(max_bytes) {}

const FieldCache::File *FieldCache::Load(const std::string &path, std::string *err) {
  auto it = files_.find(path);
  if (it == files_.end()) {
    File f;
    std::string why;
    if (!reader_(path, &f.bytes, &why)) {
      f.error = "cannot read volume file " + path + (why.empty() ? "" : ": " + why);
    } else if (max_bytes_ && bytes_used_ + f.bytes.size() > max_bytes_) {
      // Not cached: TakeDense may release budget before the next request.
      if (err) *err = "volume file " + path + " exceeds the memory budget";
      return nullptr;
    } else {
      bytes_used_ += f.bytes.size();
      if (!usdVol::ReadVDBGridInfo(f.bytes.data(), f.bytes.size(), path, &f.grids, &why))
        f.error = why.empty() ? "not a VDB file: " + path : why;
    }
    it = files_.emplace(path, std::move(f)).first;
  }
  if (!it->second.error.empty()) {
    if (err) *err = it->second.error;
    return nullptr;
  }
  return &it->second;
}

bool FieldCache::Resolve(const FieldRequest &request, ResolvedField *out, std::string *err) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!out) return false;
  if (request.prim_type == "Field3DAsset") {
    if (err) *err = request.field_prim + ": Field3D files are not supported (no reader)";
    return false;
  }
  if (request.file_path.empty()) {
    if (err) *err = request.field_prim + ": field has no filePath";
    return false;
  }
  if (request.field_index < 0) {
    if (err) *err = request.field_prim + ": negative fieldIndex";
    return false;
  }
  const File *file = Load(request.file_path, err);
  if (!file) return false;
  const usdVol::VDBGridInfo *grid = nullptr;
  int seen = 0;
  for (const usdVol::VDBGridInfo &g : file->grids) {
    if (!request.field_name.empty() && g.name != request.field_name) continue;
    if (seen++ == request.field_index) {
      grid = &g;
      break;
    }
  }
  if (!grid) {
    if (err)
      *err = request.field_prim + ": " + request.file_path + " has no grid " +
             (request.field_name.empty() ? "#" + std::to_string(request.field_index)
                                         : "'" + request.field_name + "'" +
                                               (request.field_index
                                                    ? " #" + std::to_string(request.field_index)
                                                    : ""));
    return false;
  }
  if (grid->point_grid) {
    if (err) *err = request.field_prim + ": grid '" + grid->name + "' is a point grid, not a field";
    return false;
  }
  if (!grid->has_transform) {
    if (err) *err = request.field_prim + ": grid '" + grid->name + "' has an unsupported transform";
    return false;
  }
  ResolvedField r;
  r.request = request;
  r.grid = *grid;
  r.value_type = grid->value_type;
  r.components = grid->components;
  r.grid_class = request.field_class.empty() ? grid->grid_class : request.field_class;
  if (!request.field_class.empty() && grid->grid_class != "unknown" &&
      request.field_class != grid->grid_class)
    r.warnings.push_back(request.field_prim + ": fieldClass '" + request.field_class +
                         "' but the grid is '" + grid->grid_class + "'");
  // The file's own class wins when the authored hint contradicts it, matching
  // the fieldDataType rule above: a density grid labelled levelSet would
  // otherwise be rendered as an isosurface of its densities (nothing visible).
  r.level_set = r.grid_class == "levelSet" &&
                (grid->grid_class == "levelSet" || grid->grid_class == "unknown");
  if (!FieldDataTypeMatches(request.field_data_type, grid->value_type, grid->saved_as_half))
    r.warnings.push_back(request.field_prim + ": fieldDataType '" + request.field_data_type +
                         "' but the grid holds " + grid->value_type + " (the file wins)");
  double grid_row[4][4];
  ToRowVector(grid->index_to_world, grid_row);
  Multiply(grid_row, request.field_to_world, r.index_to_world);
  if (grid->has_bounds) {
    r.has_bounds = true;
    WorldBox(r.index_to_world, grid->index_min, grid->index_max, r.world_min, r.world_max);
  }
  *out = std::move(r);
  return true;
}

size_t FieldCache::GridCount(const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const File *file = Load(path, nullptr);
  return file ? file->grids.size() : 0;
}

bool FieldCache::TakeDense(const ResolvedField &field, const usdVol::VDBReadOptions &options,
                           usdVol::VDBGrid *out, std::string *warn, std::string *err) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!out) return false;
  const std::string key = DenseKey(field, options);
  if (!Dense(field, options, warn, err)) return false;
  auto it = dense_.find(key);
  if (it == dense_.end() || !it->second) return false;
  *out = std::move(*it->second);
  const size_t bytes = out->data.size() * sizeof(float);
  bytes_used_ = bytes_used_ > bytes ? bytes_used_ - bytes : 0;
  dense_.erase(it);
  return true;
}

const usdVol::VDBGrid *FieldCache::Dense(const ResolvedField &field,
                                         const usdVol::VDBReadOptions &options, std::string *warn,
                                         std::string *err) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const std::string key = DenseKey(field, options);
  auto it = dense_.find(key);
  if (it != dense_.end()) return it->second.get();
  const File *file = Load(field.request.file_path, err);
  if (!file) return nullptr;
  std::unique_ptr<usdVol::VDBGrid> grid(new usdVol::VDBGrid);
  // Select by position (the resolved grid), not by name again.
  if (!usdVol::ReadVDBGrid(file->bytes.data(), file->bytes.size(), field.request.file_path, "",
                           int(field.grid.source_index), options, grid.get(), warn, err))
    return nullptr;
  const size_t bytes = grid->data.size() * sizeof(float);
  if (max_bytes_ && bytes_used_ + bytes > max_bytes_) {
    if (err) *err = field.request.field_prim + ": dense voxels exceed the memory budget";
    return nullptr;
  }
  bytes_used_ += bytes;
  return (dense_[key] = std::move(grid)).get();
}

bool FieldCache::SampleWorld(const ResolvedField &field, const double *points, size_t count,
                             bool trilinear, std::vector<float> *values, int *components,
                             std::string *err) {
  if (!values || (count && !points)) {
    if (err) *err = "volume sampling: null points or values";
    return false;
  }
  SparseGrid *sparse = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    sparse = LoadSparse(field.request.file_path, field.grid.source_index, err);
    if (!sparse) return false;
    if (!sparse->sampler) {
      tvdb_error_t e{};
      const tvdb_status_t st =
          tvdb_volume_sampler_create(&sparse->file.grids[field.grid.source_index], &sparse->sampler, &e);
      if (st != TVDB_OK || !sparse->sampler) {
        if (err)
          *err = field.request.field_prim + ": cannot sample the grid: " +
                 (e.message[0] ? e.message : tvdb_status_string(st));
        return false;
      }
    }
  }
  const int comps = tvdb_volume_sampler_components(sparse->sampler);
  const size_t output_components = size_t(comps > 0 ? comps : 1);
  if (count && output_components > SIZE_MAX / count) {
    if (err) *err = field.request.field_prim + ": sample count overflow";
    return false;
  }
  values->resize(count * output_components);
  if (components) *components = int(output_components);
  if (count == 0) return true;
  tvdb_error_t e{};
  const tvdb_status_t st =
      tvdb_volume_sample_world(sparse->sampler, points, count,
                               trilinear ? TVDB_VOLUME_FILTER_TRILINEAR : TVDB_VOLUME_FILTER_NEAREST,
                               values->data(), &e);
  if (st != TVDB_OK) {
    if (err)
      *err = field.request.field_prim + ": volume sampling failed: " +
             (e.message[0] ? e.message : tvdb_status_string(st));
    values->clear();
    return false;
  }
  return true;
}

bool FieldCache::MeshLevelSet(const ResolvedField &field, size_t max_cells,
                              std::vector<float> *positions, std::vector<int32_t> *indices,
                              std::string *err) {
  if (!positions || !indices) {
    if (err) *err = "level-set mesh: outputs are required";
    return false;
  }
  positions->clear();
  indices->clear();
  if (!field.level_set) {
    if (err) *err = field.request.field_prim + ": not a level-set field";
    return false;
  }
  SparseGrid *sparse = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    sparse = LoadSparse(field.request.file_path, field.grid.source_index, err);
  }
  if (!sparse) return false;
  tvdb_triangle_mesh mesh{};
  tvdb_error_t e{};
  const size_t budget = max_cells ? max_cells : size_t(4) * 1024 * 1024;
  const tvdb_status_t st =
      tvdb_grid_volume_to_mesh(&sparse->file.grids[field.grid.source_index], 0.0f, 0.0, budget,
                               &mesh, &e);
  if (st != TVDB_OK) {
    if (err)
      *err = field.request.field_prim + ": cannot mesh the level set: " +
             (e.message[0] ? e.message : tvdb_status_string(st));
    return false;
  }
  if (!mesh.vertex_count || !mesh.face_count) {
    tvdb_triangle_mesh_free(&mesh);
    if (err) *err = field.request.field_prim + ": level set has no surface";
    return false;
  }
  positions->resize(size_t(mesh.vertex_count) * 3);
  indices->resize(size_t(mesh.face_count) * 3);
  const double(*m)[4] = field.request.field_to_world;  // USD row-vector
  double volume6 = 0.0;  // signed volume, to detect inward winding
  for (size_t i = 0; i < size_t(mesh.vertex_count); ++i) {
    const tvdb_vec3f &v = mesh.vertices[i];
    for (int a = 0; a < 3; ++a) {
      const double p[3] = {double(v.x), double(v.y), double(v.z)};
      (*positions)[3 * i + size_t(a)] = float(p[0] * m[0][a] + p[1] * m[1][a] +
                                             p[2] * m[2][a] + m[3][a]);
    }
  }
  for (size_t f = 0; f < size_t(mesh.face_count); ++f) {
    int32_t v0 = int32_t(mesh.faces[f].v0);
    int32_t v1 = int32_t(mesh.faces[f].v1);
    int32_t v2 = int32_t(mesh.faces[f].v2);
    (*indices)[3 * f + 0] = v0;
    (*indices)[3 * f + 1] = v1;
    (*indices)[3 * f + 2] = v2;
    const float *p0 = &(*positions)[size_t(v0) * 3];
    const float *p1 = &(*positions)[size_t(v1) * 3];
    const float *p2 = &(*positions)[size_t(v2) * 3];
    volume6 += double(p0[0]) * (double(p1[1]) * double(p2[2]) - double(p1[2]) * double(p2[1])) -
               double(p0[1]) * (double(p1[0]) * double(p2[2]) - double(p1[2]) * double(p2[0])) +
               double(p0[2]) * (double(p1[0]) * double(p2[1]) - double(p1[1]) * double(p2[0]));
  }
  tvdb_triangle_mesh_free(&mesh);
  // OpenVDB levels sets are negative inside, so the mesher already winds
  // outward; a positive-inside file would come out inverted.
  if (volume6 < 0.0) {
    for (size_t f = 0; f + 2 < indices->size(); f += 3) std::swap((*indices)[f + 1], (*indices)[f + 2]);
  }
  return true;
}

// NanoVDB grid type id (PNANOVDB_GRID_TYPE_*) for a usdVol value type.
uint32_t NanoVDBTypeFor(const std::string &value_type) {
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

namespace {
// The resolved field's index -> world (row vectors, field prim included) in
// the column-vector layout NanoVDB / VDBNanoVDBInfo use.
void FieldMapColumnVector(const ResolvedField &field, double out[4][4]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r][c] = field.index_to_world[c][r];
}
}  // namespace

const std::vector<uint8_t> *FieldCache::NanoVDB(const ResolvedField &field,
                                                usdVol::VDBNanoVDBInfo *info, std::string *err) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const std::string key = field.request.file_path + "|" + std::to_string(field.grid.source_index);
  auto it = nanovdb_.find(key);
  if (it != nanovdb_.end()) {
    if (info) {
      // Header facts only: the converted buffer is already cached.
      info->grid_type = NanoVDBTypeFor(field.grid.value_type);
      info->components = field.grid.components > 0 ? field.grid.components : 1;
      info->value_type = field.grid.value_type;
      info->grid_class = field.grid_class;
      FieldMapColumnVector(field, info->index_to_world);
    }
    return it->second.get();
  }
  const File *file = Load(field.request.file_path, err);
  if (!file) return nullptr;
  std::unique_ptr<std::vector<uint8_t>> buffer(new std::vector<uint8_t>);
  usdVol::VDBNanoVDBInfo converted;
  if (!usdVol::ReadVDBGridNanoVDB(file->bytes.data(), file->bytes.size(),
                                  field.request.file_path, "", int(field.grid.source_index),
                                  buffer.get(), &converted, err))
    return nullptr;
  // The field's authored fieldClass wins over the file's, as in Resolve(),
  // and the map includes the field prim's transform: the buffer's own map is
  // only the file's index -> grid-world.
  converted.grid_class = field.grid_class;
  FieldMapColumnVector(field, converted.index_to_world);
  const size_t bytes = buffer->size();
  if (max_bytes_ && bytes_used_ + bytes > max_bytes_) {
    if (err) *err = field.request.field_prim + ": NanoVDB grid exceeds the memory budget";
    return nullptr;
  }
  bytes_used_ += bytes;
  if (info) *info = converted;
  return (nanovdb_[key] = std::move(buffer)).get();
}

FieldCache::SparseGrid *FieldCache::LoadSparse(const std::string &path, size_t source_index,
                                               std::string *err) {
  const std::string key = path + "|" + std::to_string(source_index);
  auto it = sparse_.find(key);
  if (it != sparse_.end()) return it->second.get();
  const File *file = Load(path, err);
  if (!file) return nullptr;
  if (source_index >= file->grids.size()) {
    if (err) *err = path + ": grid index out of range";
    return nullptr;
  }
  std::unique_ptr<SparseGrid> sparse(new SparseGrid);
  tvdb_error_t e{};
  tvdb_status_t st = tvdb_file_open_memory(&sparse->file, file->bytes.data(), file->bytes.size(),
                                           nullptr, &e);
  if (st != TVDB_OK) {
    if (err) *err = "cannot open " + path + ": " + (e.message[0] ? e.message : tvdb_status_string(st));
    return nullptr;
  }
  sparse->open = true;
  st = tvdb_read_grid(&sparse->file, source_index, &e);
  if (st != TVDB_OK) {
    if (err)
      *err = "cannot decode " + path + " grid #" + std::to_string(source_index) + ": " +
             (e.message[0] ? e.message : tvdb_status_string(st));
    return nullptr;
  }
  return (sparse_[key] = std::move(sparse)).get();
}

bool FieldsWorldBounds(const std::vector<ResolvedField> &fields, double world_min[3],
                       double world_max[3]) {
  bool any = false;
  for (const ResolvedField &f : fields) {
    if (!f.has_bounds) continue;
    for (int a = 0; a < 3; ++a) {
      if (!any || f.world_min[a] < world_min[a]) world_min[a] = f.world_min[a];
      if (!any || f.world_max[a] > world_max[a]) world_max[a] = f.world_max[a];
    }
    any = true;
  }
  return any;
}

bool ResolveWithFirstGridFallback(FieldCache &cache, const FieldRequest &request,
                                  const FieldFilter &filter, ResolvedField *out,
                                  std::string *err) {
  if (!out) return false;
  std::string primary_err;
  {
    ResolvedField primary;
    if (cache.Resolve(request, &primary, &primary_err)) {
      if (!filter || filter(primary)) {
        *out = std::move(primary);
        return true;
      }
      // The selected grid exists but cannot be used (e.g. it exceeds the
      // dense cap): report it rather than render another grid in its place.
      if (err)
        *err = request.field_prim + ": grid '" + primary.grid.name + "' of " +
               request.file_path + " could not be loaded";
      return false;
    }
  }
  if (!request.field_name_defaulted || request.field_index != 0 ||
      request.prim_type == "Field3DAsset") {
    if (err) *err = primary_err;
    return false;
  }
  FieldRequest fallback = request;
  fallback.field_name.clear();
  const size_t count = cache.GridCount(request.file_path);
  for (size_t i = 0; i < count; ++i) {
    fallback.field_index = int(i);
    ResolvedField candidate;
    if (!cache.Resolve(fallback, &candidate, nullptr)) continue;
    if (filter && !filter(candidate)) continue;
    candidate.warnings.push_back(request.field_prim + ": no grid '" + request.field_name +
                                 "' (fieldName not authored); using grid '" +
                                 candidate.grid.name + "'");
    *out = std::move(candidate);
    return true;
  }
  if (err) *err = primary_err;
  return false;
}

}  // namespace vol
}  // namespace tydra
}  // namespace lightusd
