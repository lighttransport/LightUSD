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
#include "tydra/vol/volume-field.hh"
#include "tinyvdb_tetmesh.h"

#include <sstream>

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
std::string Device(const ia::Json& j) {
  return j.contains("device") && j["device"].is_string() ? j["device"].get<std::string>() : "cpu";
}
// Explicit devices that cannot run are unsupported rather than invalid.
lightusd_status DeviceStatus(const std::string& device, std::string* err) {
  std::string resolved;
  if (ia::VolumetricDeviceAvailable(device, &resolved, err)) return LIGHTUSD_OK;
  return device == "cuda" || device == "hip" ? LIGHTUSD_ERR_UNSUPPORTED : LIGHTUSD_ERR_INVALID_ARG;
}
double Number(const ia::Json& j, const char* key, double fallback) {
  return j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}
// A whole-number setting in [0, max] (absent selects 0, the default). Large,
// negative or non-finite JSON numbers are rejected before any cast.
constexpr double kMaxCountSetting = double(size_t(1) << 28);
bool Count(const ia::Json& j, const char* key, double max, size_t* out, std::string* err) {
  *out = 0;
  if (!j.contains(key)) return true;
  const double v = j[key].is_number() ? j[key].get<double>() : -1.0;
  if (!(v >= 0 && v <= max) || v != std::floor(v)) {
    if (err) *err = std::string(key) + " must be a whole number in [0, " + std::to_string(size_t(max)) + "]";
    return false;
  }
  *out = size_t(v);
  return true;
}
lightusd_status Failure(const std::string& err) {
  return Fail(ia::IsResourceLimitError(err) ? LIGHTUSD_ERR_RESOURCE_LIMIT : LIGHTUSD_ERR_INVALID_ARG, err);
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

lightusd_status FromMesh(const float* points, size_t point_count, const int32_t* indices,
                         size_t index_count, const int32_t* counts, size_t count_count,
                         const char* settings_json, ia::LevelSet* out) {
  if (!points || !indices || !point_count || !index_count || (count_count && !counts))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: points, indices and output are required");
  ia::Json settings;
  std::string err;
  if (!Settings(settings_json, &settings, &err)) return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  ia::LevelSetOptions o;
  o.voxel_size = Number(settings, "voxel_size", 0);
  o.exterior_width = Number(settings, "exterior_width", 0);
  o.interior_width = Number(settings, "interior_width", 0);
  size_t threads = 0;
  if (!Count(settings, "max_voxels", kMaxCountSetting, &o.max_voxels, &err) ||
      !Count(settings, "threads", 256, &threads, &err))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: " + err);
  o.threads = int(threads);
  if (!std::isfinite(o.voxel_size) || !(o.exterior_width >= 0) || !(o.interior_width >= 0) ||
      !std::isfinite(o.exterior_width) || !std::isfinite(o.interior_width))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: voxel size and widths must be finite");
  o.exact_winding = settings.contains("exact_winding") && settings["exact_winding"].is_boolean() &&
                    settings["exact_winding"].get<bool>();
  if (settings.contains("name") && settings["name"].is_string())
    o.name = settings["name"].get<std::string>();
  o.device = Device(settings);
  if (!(o.voxel_size > 0) || o.threads < 0 || o.threads > 256)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: voxel_size > 0 and threads in [0, 256]");
  const lightusd_status ds = DeviceStatus(o.device, &err);
  if (ds != LIGHTUSD_OK) return Fail(ds, err);
  ia::Mesh mesh;
  mesh.positions.assign(points, points + 3 * point_count);
  mesh.indices.assign(indices, indices + index_count);
  if (count_count) mesh.counts.assign(counts, counts + count_count);
  if (!ia::MeshToLevelSet(mesh, o, out, &err)) return Failure(err);
  return LIGHTUSD_OK;
}

lightusd_status WriteVDB(const ia::LevelSet& level_set, lightusd_string** vdb) {
  auto* s = new (std::nothrow) lightusd_string();
  if (!s) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "level set: out of memory");
  std::string err;
  if (!ia::WriteLevelSetVDB(level_set, &s->s, &err)) {
    delete s;
    return Failure(err);
  }
  *vdb = s;
  return LIGHTUSD_OK;
}

void Info(const ia::LevelSet& level_set, lightusd_level_set_info* out) {
  std::memset(out, 0, sizeof(*out));
  out->voxel_size = level_set.voxel_size();
  out->background = level_set.background();
  out->active_voxels = level_set.active_voxels();
}

lightusd_status Sample(const ia::LevelSet& level_set, const double* points, size_t count,
                       float* values) {
  std::string err;
  if (!ia::SampleLevelSet(level_set, points, count, values, &err))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  return LIGHTUSD_OK;
}

lightusd_status ToMesh(const ia::LevelSet& level_set, uint64_t max_cells, lightusd_string** points,
                       lightusd_string** indices) {
  ia::Mesh mesh;
  std::string err;
  if (!ia::LevelSetToMesh(level_set, max_cells > uint64_t(SIZE_MAX) ? SIZE_MAX : size_t(max_cells),
                          &mesh, &err))
    return Failure(err);
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

lightusd_status StuffingSettings(const char* settings_json, ia::StuffingOptions* o) {
  ia::Json settings;
  std::string err;
  if (!Settings(settings_json, &settings, &err)) return Fail(LIGHTUSD_ERR_INVALID_ARG, err);
  o->spacing = Number(settings, "spacing", 0);
  o->graded_coarse_spacing = Number(settings, "graded_coarse_spacing", 0);
  o->graded_growth = Number(settings, "graded_growth", 0.8);
  if (settings.contains("graded_surface_guard")) {
    if (!settings["graded_surface_guard"].is_boolean())
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: graded_surface_guard must be boolean");
    o->graded_surface_guard = settings["graded_surface_guard"].get<bool>();
  }
  size_t threads = 0;
  if (!Count(settings, "max_tets", kMaxCountSetting, &o->max_tets, &err) ||
      !Count(settings, "max_lattice_vertices", kMaxCountSetting, &o->max_lattice_vertices, &err) ||
      !Count(settings, "max_background_tets", kMaxCountSetting, &o->max_background_tets, &err) ||
      !Count(settings, "threads", 256, &threads, &err))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: " + err);
  o->threads = int(threads);
  if (!std::isfinite(o->spacing) || !std::isfinite(o->graded_coarse_spacing) ||
      !std::isfinite(o->graded_growth) || o->graded_coarse_spacing < 0 || o->graded_growth <= 0 ||
      (o->graded_coarse_spacing > 0 && o->graded_coarse_spacing < o->spacing) ||
      (o->graded_surface_guard && o->graded_coarse_spacing <= 0))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: invalid graded spacing or growth");
  o->device = Device(settings);
  if (o->graded_coarse_spacing > 0 && o->device != "cpu")
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: graded stuffing requires the cpu device");
  if (!(o->spacing > 0) || o->threads < 0 || o->threads > 256)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: spacing > 0 and threads in [0, 256]");
  const lightusd_status ds = DeviceStatus(o->device, &err);
  if (ds != LIGHTUSD_OK) return Fail(ds, err);
  return LIGHTUSD_OK;
}

lightusd_status ToTetMesh(const ia::LevelSet& level_set, const ia::StuffingOptions& o,
                          lightusd_string** points, lightusd_string** tets,
                          lightusd_string** surface) {
  ia::TetMeshData mesh;
  std::string err;
  if (!ia::LevelSetToTetMesh(level_set, o, &mesh, &err)) return Failure(err);
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

}  // namespace

struct lightusd_level_set {
  ia::LevelSet level_set;
};

extern "C" {

lightusd_status lightusd_volumetric_device_available(const char* device,
                                                     lightusd_string** resolved) {
  if (resolved) *resolved = nullptr;
  std::string name, err;
  if (!ia::VolumetricDeviceAvailable(device ? device : "cpu", &name, &err))
    return Fail(DeviceStatus(device ? device : "cpu", &err), err);
  if (resolved) {
    auto* s = new (std::nothrow) lightusd_string();
    if (!s) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "out of memory");
    s->s = name;
    *resolved = s;
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_mesh_to_level_set_vdb(const float* points, size_t point_count,
                                               const int32_t* indices, size_t index_count,
                                               const int32_t* counts, size_t count_count,
                                               const char* settings_json, lightusd_string** vdb) {
  if (!vdb) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: points, indices and output are required");
  *vdb = nullptr;
  ia::LevelSet level_set;
  const lightusd_status st = FromMesh(points, point_count, indices, index_count, counts,
                                      count_count, settings_json, &level_set);
  return st != LIGHTUSD_OK ? st : WriteVDB(level_set, vdb);
}

lightusd_status lightusd_level_set_vdb_info(const uint8_t* vdb, size_t size,
                                            lightusd_level_set_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set info: output is required");
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  if (st != LIGHTUSD_OK) return st;
  Info(level_set, out);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_vdb_inspect(const uint8_t* vdb, size_t size, lightusd_string** json) {
  if (!vdb || !json) return Fail(LIGHTUSD_ERR_INVALID_ARG, "vdb inspect: data and output are required");
  *json = nullptr;
  std::vector<lightusd::usdVol::VDBGridInfo> grids;
  std::string err;
  if (!lightusd::usdVol::ReadVDBGridInfo(vdb, size, "<memory>", &grids, &err))
    return Fail(LIGHTUSD_ERR_PARSE, err.empty() ? "not a VDB file" : err);
  auto str = [](std::ostringstream& o, const std::string& s) {
    o << '"';
    for (unsigned char c : s) {
      if (c == '"' || c == '\\') o << '\\' << c;
      else if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        o << buf;
      } else o << c;
    }
    o << '"';
  };
  std::ostringstream o;
  o.precision(17);
  o << "{\"grids\":[";
  for (size_t i = 0; i < grids.size(); ++i) {
    const lightusd::usdVol::VDBGridInfo& g = grids[i];
    o << (i ? "," : "") << "{\"name\":";
    str(o, g.name);
    o << ",\"index\":" << g.source_index << ",\"nameIndex\":" << g.name_index << ",\"valueType\":";
    str(o, g.value_type);
    o << ",\"fieldDataType\":";
    str(o, lightusd::tydra::vol::FieldDataTypeFor(g.value_type, g.saved_as_half));
    o << ",\"gridClass\":";
    str(o, g.grid_class);
    o << ",\"components\":" << g.components << ",\"savedAsHalf\":" << (g.saved_as_half ? "true" : "false")
      << ",\"pointGrid\":" << (g.point_grid ? "true" : "false") << ",\"activeVoxels\":" << g.active_voxels;
    if (g.has_transform) {
      double m[4][4];
      lightusd::tydra::vol::IndexToWorldRowVector(g, m);
      o << ",\"indexToWorld\":[";
      for (int k = 0; k < 16; ++k) o << (k ? "," : "") << m[k / 4][k % 4];
      o << ']';
    }
    if (g.has_bounds) {
      o << ",\"indexMin\":[" << g.index_min[0] << ',' << g.index_min[1] << ',' << g.index_min[2]
        << "],\"indexMax\":[" << g.index_max[0] << ',' << g.index_max[1] << ',' << g.index_max[2]
        << "],\"worldMin\":[" << g.world_min[0] << ',' << g.world_min[1] << ',' << g.world_min[2]
        << "],\"worldMax\":[" << g.world_max[0] << ',' << g.world_max[1] << ',' << g.world_max[2] << ']';
    }
    o << '}';
  }
  o << "]}";
  auto* s = new (std::nothrow) lightusd_string();
  if (!s) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "vdb inspect: allocation failed");
  s->s = o.str();
  *json = s;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_vdb_sample(const uint8_t* vdb, size_t size,
                                              const double* points, size_t count, float* values) {
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  return st != LIGHTUSD_OK ? st : Sample(level_set, points, count, values);
}

lightusd_status lightusd_level_set_vdb_to_mesh(const uint8_t* vdb, size_t size,
                                               uint64_t max_cells, lightusd_string** points,
                                               lightusd_string** indices) {
  if (!points || !indices) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set mesh: outputs are required");
  *points = *indices = nullptr;
  ia::LevelSet level_set;
  const lightusd_status st = Read(vdb, size, &level_set);
  return st != LIGHTUSD_OK ? st : ToMesh(level_set, max_cells, points, indices);
}

lightusd_status lightusd_level_set_vdb_to_tet_mesh(const uint8_t* vdb, size_t size,
                                                   const char* settings_json,
                                                   lightusd_string** points,
                                                   lightusd_string** tets,
                                                   lightusd_string** surface) {
  if (!points || !tets || !surface)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: outputs are required");
  *points = *tets = *surface = nullptr;
  ia::StuffingOptions o;
  lightusd_status st = StuffingSettings(settings_json, &o);
  if (st != LIGHTUSD_OK) return st;
  ia::LevelSet level_set;
  st = Read(vdb, size, &level_set);
  return st != LIGHTUSD_OK ? st : ToTetMesh(level_set, o, points, tets, surface);
}

lightusd_status lightusd_level_set_from_mesh(const float* points, size_t point_count,
                                             const int32_t* indices, size_t index_count,
                                             const int32_t* counts, size_t count_count,
                                             const char* settings_json, lightusd_level_set** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: points, indices and output are required");
  *out = nullptr;
  auto* h = new (std::nothrow) lightusd_level_set();
  if (!h) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "level set: out of memory");
  const lightusd_status st = FromMesh(points, point_count, indices, index_count, counts,
                                      count_count, settings_json, &h->level_set);
  if (st != LIGHTUSD_OK) {
    delete h;
    return st;
  }
  *out = h;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_from_vdb(const uint8_t* vdb, size_t size,
                                            lightusd_level_set** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: output is required");
  *out = nullptr;
  auto* h = new (std::nothrow) lightusd_level_set();
  if (!h) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "level set: out of memory");
  const lightusd_status st = Read(vdb, size, &h->level_set);
  if (st != LIGHTUSD_OK) {
    delete h;
    return st;
  }
  *out = h;
  return LIGHTUSD_OK;
}

void lightusd_level_set_destroy(lightusd_level_set* level_set) { delete level_set; }

lightusd_status lightusd_level_set_get_info(const lightusd_level_set* level_set,
                                            lightusd_level_set_info* out) {
  if (!level_set || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set info: handle and output are required");
  Info(level_set->level_set, out);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_level_set_write_vdb(const lightusd_level_set* level_set,
                                             lightusd_string** vdb) {
  if (!level_set || !vdb) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set: handle and output are required");
  *vdb = nullptr;
  return WriteVDB(level_set->level_set, vdb);
}

lightusd_status lightusd_level_set_sample(const lightusd_level_set* level_set,
                                          const double* points, size_t count, float* values) {
  if (!level_set) return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set sample: handle is required");
  return Sample(level_set->level_set, points, count, values);
}

lightusd_status lightusd_level_set_to_mesh(const lightusd_level_set* level_set, uint64_t max_cells,
                                           lightusd_string** points, lightusd_string** indices) {
  if (!level_set || !points || !indices)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "level set mesh: handle and outputs are required");
  *points = *indices = nullptr;
  return ToMesh(level_set->level_set, max_cells, points, indices);
}

lightusd_status lightusd_level_set_to_tet_mesh(const lightusd_level_set* level_set,
                                               const char* settings_json, lightusd_string** points,
                                               lightusd_string** tets, lightusd_string** surface) {
  if (!level_set || !points || !tets || !surface)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh: handle and outputs are required");
  *points = *tets = *surface = nullptr;
  ia::StuffingOptions o;
  const lightusd_status st = StuffingSettings(settings_json, &o);
  return st != LIGHTUSD_OK ? st : ToTetMesh(level_set->level_set, o, points, tets, surface);
}

lightusd_status lightusd_tet_mesh_get_quality(const float* positions,
                                              size_t point_count,
                                              const int32_t* tets,
                                              size_t tet_count,
                                              lightusd_tet_mesh_quality* out) {
  if (!out || (point_count && !positions) || (tet_count && !tets) ||
      point_count > static_cast<size_t>(INT32_MAX) + 1 ||
      point_count > SIZE_MAX / (3 * sizeof(float)) ||
      tet_count > SIZE_MAX / (4 * sizeof(int32_t)) ||
      (tet_count && !point_count))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "tet mesh quality: invalid buffers or counts");

  tvdb_tet_mesh mesh = {};
  mesh.positions = const_cast<float*>(positions);
  mesh.vertex_count = point_count;
  mesh.tets = const_cast<int32_t*>(tets);
  mesh.tet_count = tet_count;
  tvdb_tet_mesh_quality_t measured = {};
  tvdb_error_t err = {};
  const tvdb_status_t status = tvdb_tet_mesh_quality(&mesh, &measured, &err);
  if (status != TVDB_OK)
    return Fail(LIGHTUSD_ERR_INVALID_ARG,
                err.message[0] ? err.message : "tet mesh quality: invalid mesh");

  lightusd_tet_mesh_quality result = {};
  result.min_dihedral_degrees = measured.min_dihedral;
  result.max_dihedral_degrees = measured.max_dihedral;
  result.volume = measured.volume;
  result.min_tet_volume = measured.min_volume;
  result.inverted_tets = static_cast<uint64_t>(measured.inverted);
  *out = result;
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
