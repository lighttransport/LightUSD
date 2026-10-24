// SPDX-License-Identifier: Apache-2.0
#include "volume-field.hh"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

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

bool ReadFileAsset(const std::string &path, std::vector<uint8_t> *bytes, std::string *err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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
      f.error = "volume file " + path + " exceeds the memory budget";
      f.bytes.clear();
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

const usdVol::VDBGrid *FieldCache::Dense(const ResolvedField &field,
                                         const usdVol::VDBReadOptions &options, std::string *warn,
                                         std::string *err) {
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

}  // namespace vol
}  // namespace tydra
}  // namespace lightusd
