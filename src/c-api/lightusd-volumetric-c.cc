// SPDX-License-Identifier: Apache-2.0
// C API over tydra/asset-import/volumetric (vendored tinyvdb).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "c-internal.hh"
#include "lightusd-import-c.h"
#include "tydra/asset-import/import.hh"
#include "tydra/asset-import/volumetric.hh"

namespace ia = lightusd::tydra::import_asset;
using lightusd_internal::Fail;

namespace {

bool Settings(const char* text, ia::Json* out, std::string* err) {
  *out = ia::Json::object();
  if (!text || !*text) return true;
  lightusd::minijson::Error error;
  if (!lightusd::minijson::Parse(text, out, &error) || !out->is_object()) {
    *err = "settings must be a JSON object";
    return false;
  }
  return true;
}
double Number(const ia::Json& j, const char* key, double fallback) {
  return j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}
template <class T>
lightusd_string* Pack(const std::vector<T>& v) {
  auto* s = new (std::nothrow) lightusd_string();
  if (!s) return nullptr;
  s->s.assign(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
  return s;
}
lightusd_status Read(const uint8_t* vdb, size_t size, ia::LevelSet* out) {
  std::string err;
  if (!ia::ReadLevelSetVDB(vdb, size, out, &err)) return Fail(LIGHTUSD_ERR_PARSE, err);
  return LIGHTUSD_OK;
}

}  // namespace

extern "C" {

lightusd_status lightusd_mesh_to_level_set_vdb(const float* points, size_t point_count,
                                               const int32_t* indices, size_t index_count,
                                               const int32_t* counts, size_t count_count,
                                               const char* settings_json, lightusd_string** vdb) {
  if (!vdb || !points || !indices || !point_count || !index_count || (count_count && !counts))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: points, indices and output are required");
  *vdb = nullptr;
  ia::Json settings;
  std::string err;
  if (!Settings(settings_json, &settings, &err)) return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  ia::LevelSetOptions o;
  o.voxel_size = Number(settings, "voxel_size", 0);
  o.exterior_width = Number(settings, "exterior_width", 0);
  o.interior_width = Number(settings, "interior_width", 0);
  o.max_voxels = size_t(std::max(0.0, Number(settings, "max_voxels", 0)));
  o.threads = int(Number(settings, "threads", 0));
  o.exact_winding = settings.contains("exact_winding") && settings["exact_winding"].is_boolean() &&
                    settings["exact_winding"].get<bool>();
  if (settings.contains("name") && settings["name"].is_string())
    o.name = settings["name"].get<std::string>();
  if (!(o.voxel_size > 0) || o.threads < 0 || o.threads > 256)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: voxel_size > 0 and threads in [0, 256]");
  ia::Mesh mesh;
  mesh.positions.assign(points, points + 3 * point_count);
  mesh.indices.assign(indices, indices + index_count);
  if (count_count) mesh.counts.assign(counts, counts + count_count);
  ia::LevelSet level_set;
  std::vector<uint8_t> bytes;
  if (!ia::MeshToLevelSet(mesh, o, &level_set, &err) ||
      !ia::WriteLevelSetVDB(level_set, &bytes, &err))
    return Fail(err.find("budget") != std::string::npos ? LIGHTUSD_ERR_RESOURCE_LIMIT
                                                         : LIGHTUSD_ERR_INVALID_ARG,
                err);
  *vdb = Pack(bytes);
  return *vdb ? LIGHTUSD_OK : Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "level set: out of memory");
}

lightusd_status lightusd_level_set_vdb_info(const uint8_t* vdb, size_t size,
                                            lightusd_level_set_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set info: output is required");
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  if (st != LIGHTUSD_OK) return st;
  std::memset(out, 0, sizeof(*out));
  out->voxel_size = level_set.voxel_size();
  out->background = level_set.background();
  out->active_voxels = level_set.active_voxels();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_vdb_sample(const uint8_t* vdb, size_t size,
                                              const double* points, size_t count, float* values) {
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  if (st != LIGHTUSD_OK) return st;
  std::string err;
  if (!ia::SampleLevelSet(level_set, points, count, values, &err))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_vdb_to_mesh(const uint8_t* vdb, size_t size,
                                               uint64_t max_cells, lightusd_string** points,
                                               lightusd_string** indices) {
  if (!points || !indices) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set mesh: outputs are required");
  *points = *indices = nullptr;
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  if (st != LIGHTUSD_OK) return st;
  ia::Mesh mesh;
  std::string err;
  if (!ia::LevelSetToMesh(level_set, size_t(max_cells), &mesh, &err))
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT, err);
  *points = Pack(mesh.positions);
  *indices = Pack(mesh.indices);
  if (!*points || !*indices) {
    lightusd_string_destroy(*points);
    lightusd_string_destroy(*indices);
    *points = *indices = nullptr;
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "level set mesh: out of memory");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_vdb_to_tet_mesh(const uint8_t* vdb, size_t size,
                                                   const char* settings_json,
                                                   lightusd_string** points,
                                                   lightusd_string** tets,
                                                   lightusd_string** surface) {
  if (!points || !tets || !surface)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: outputs are required");
  *points = *tets = *surface = nullptr;
  ia::Json settings;
  std::string err;
  if (!Settings(settings_json, &settings, &err)) return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  ia::StuffingOptions o;
  o.spacing = Number(settings, "spacing", 0);
  o.max_tets = size_t(std::max(0.0, Number(settings, "max_tets", 0)));
  o.threads = int(Number(settings, "threads", 0));
  if (!(o.spacing > 0) || o.threads < 0 || o.threads > 256)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: spacing > 0 and threads in [0, 256]");
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  if (st != LIGHTUSD_OK) return st;
  ia::TetMeshData mesh;
  if (!ia::LevelSetToTetMesh(level_set, o, &mesh, &err))
    return Fail(err.find("budget") != std::string::npos ? LIGHTUSD_ERR_RESOURCE_LIMIT
                                                         : LIGHTUSD_ERR_INVALID_ARG,
                err);
  *points = Pack(mesh.points);
  *tets = Pack(mesh.tets);
  *surface = Pack(mesh.surface);
  if (!*points || !*tets || !*surface) {
    lightusd_string_destroy(*points);
    lightusd_string_destroy(*tets);
    lightusd_string_destroy(*surface);
    *points = *tets = *surface = nullptr;
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "tet mesh: out of memory");
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_stage_add_generated_asset(lightusd_stage* stage, const char* path,
                                                   const uint8_t* data, size_t size) {
  if (!stage || !path || !*path || (size && !data))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "generated asset: stage, path and data are required");
  if (stage->snapshot_stage)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "generated asset: stage is an immutable snapshot");
  const std::string name(path);
  if (name.find("..") != std::string::npos || name[0] == '/' || name.find(':') != std::string::npos)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "generated asset: path must be relative and stay below the layer");
  std::shared_ptr<const std::vector<uint8_t>> bytes(
      new (std::nothrow) std::vector<uint8_t>(data, data + size));
  if (!bytes) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "generated asset: out of memory");
  stage->generated_assets[name] = bytes;
  stage->import_assets[name] = bytes;
  stage->import_asset_paths[name] = name;
  return LIGHTUSD_OK;
}

}  // extern "C"
