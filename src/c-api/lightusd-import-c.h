/* SPDX-License-Identifier: Apache-2.0
 * Native OBJ/URDF/MJCF import. Link lightusd_render_c (or the shared C ABI).
 * Returned stages have the ordinary lightusd_stage ownership contract.
 */
#ifndef LIGHTUSD_IMPORT_C_H_
#define LIGHTUSD_IMPORT_C_H_
#include "lightusd-c.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum lightusd_import_format {
  LIGHTUSD_IMPORT_AUTO = 0,
  LIGHTUSD_IMPORT_OBJ = 1,
  LIGHTUSD_IMPORT_URDF = 2,
  LIGHTUSD_IMPORT_MJCF = 3
} lightusd_import_format;
typedef struct lightusd_import_options {
  uint32_t struct_size;
  uint32_t format;
  uint64_t max_memory_bytes;
  uint64_t max_input_bytes;
  uint32_t max_depth;  /* XML/include depth in [1, 1024]; default 64. */
  uint32_t _reserved;
  lightusd_asset_resolver* resolver;
  /* Optional UTF-8 JSON object: up_axis (Z/Y), allow_missing (bool),
   * tessellate_collision_shapes (bool), unsupported_policy (warn/error/preserve),
   * asset_dirs (string[]),
   * packages (object mapping URDF package names to directories).
   * The importer copies these settings during the synchronous call. */
  const char* settings_json;
  /* Optional progress callback (fields added after settings_json; a smaller
   * struct_size from older callers leaves them unset). Called on the
   * importing thread with a phase ("import", "convert"), a monotonic count of
   * bytes accounted so far and the memory budget; return nonzero to cancel the import, which
   * then fails with LIGHTUSD_ERR_CANCELLED. */
  int (*progress)(void* user_data, const char* phase, uint64_t done, uint64_t total);
  void* progress_user_data;
} lightusd_import_options;
LIGHTUSD_API void lightusd_import_options_init(
    lightusd_import_options* options);
/* Failure clears *out. Warnings are available via stage_take_warnings(). */
LIGHTUSD_API lightusd_status lightusd_import_file(
    const char* filename, const lightusd_import_options* options,
    lightusd_stage** out);
/* source anchors dependencies and supplies the extension for AUTO. An empty
 * source permits self-contained inputs; external assets require a resolver. */
LIGHTUSD_API lightusd_status lightusd_import_memory(
    const uint8_t* data, size_t size, const char* source,
    const lightusd_import_options* options, lightusd_stage** out);
/* Owned JSON report (version 1). This query does not drain it. Stages loaded
 * through USD-only APIs return {}. Reports are not serialized into USD. */
LIGHTUSD_API lightusd_status lightusd_stage_import_report(
    const lightusd_stage* stage, lightusd_string** out);
/* Last native import attempt on the calling thread, including failures.
 * Overwritten by each import_file/import_memory call; invalid arguments/options
 * leave {}. Copy the owned result before another import on the same thread. */
LIGHTUSD_API lightusd_status lightusd_import_last_report(lightusd_string** out);
/* ---- Signed distance fields and volumetric meshes (tinyvdb) -------------
 * Narrow-band level sets are OpenVDB bytes (one float grid, class "level
 * set", negative inside). Byte outputs are owned lightusd_string buffers
 * (lightusd_string_view / lightusd_string_destroy); packed arrays are native
 * float32 / int32. The "device" setting picks "cpu" (default), "cuda", "hip"
 * or "auto" (CUDA, then HIP, then the CPU); CUDA/HIP need a
 * LIGHTUSD_TINYVDB_GPU build, and an explicit device that is unavailable is
 * LIGHTUSD_ERR_UNSUPPORTED. Results match the CPU's. */
/* Whether `device` can run here: LIGHTUSD_OK with the device it maps to
 * ("cpu", "cuda" or "hip") in *resolved, or LIGHTUSD_ERR_UNSUPPORTED with the
 * reason as the last error. `resolved` may be NULL. */
LIGHTUSD_API lightusd_status lightusd_volumetric_device_available(const char* device,
                                                                  lightusd_string** resolved);
/* Level set of a mesh: triangles in `indices`, or polygons when `counts` is
 * given (fan-triangulated). settings_json: {"voxel_size": h (> 0, required),
 * "exterior_width", "interior_width" (world, default 3 h), "exact_winding",
 * "max_voxels" (candidate budget, ~128 bytes each; default 8,000,000),
 * "threads", "name", "device"}. Signs come from generalized winding
 * numbers, so open and inside-out meshes work. */
LIGHTUSD_API lightusd_status lightusd_mesh_to_level_set_vdb(
    const float* points, size_t point_count, const int32_t* indices, size_t index_count,
    const int32_t* counts, size_t count_count, const char* settings_json,
    lightusd_string** vdb);
typedef struct lightusd_level_set_info {
  double voxel_size;
  float background;
  uint32_t _reserved;
  uint64_t active_voxels;
} lightusd_level_set_info;
LIGHTUSD_API lightusd_status lightusd_level_set_vdb_info(const uint8_t* vdb, size_t size,
                                                        lightusd_level_set_info* out);
/* UsdVol: describe every grid of a .vdb without decoding voxels, as JSON
 * {"grids": [{"name", "index" (file position), "nameIndex" (among equal
 * names: the UsdVol fieldIndex), "valueType", "fieldDataType", "gridClass",
 * "components", "savedAsHalf", "pointGrid", "activeVoxels",
 * "indexToWorld" (16, USD row-vector), "indexMin"/"indexMax" (inclusive),
 * "worldMin"/"worldMax"}]}. Bounds are omitted when the file lacks them. */
LIGHTUSD_API lightusd_status lightusd_vdb_inspect(const uint8_t* vdb, size_t size,
                                                  lightusd_string** json);
/* Trilinear world-space samples (3 doubles per point). */
LIGHTUSD_API lightusd_status lightusd_level_set_vdb_sample(const uint8_t* vdb, size_t size,
                                                          const double* points, size_t count,
                                                          float* values);
/* Zero isosurface: float32 xyz points and int32 triangle indices. */
LIGHTUSD_API lightusd_status lightusd_level_set_vdb_to_mesh(const uint8_t* vdb, size_t size,
                                                           uint64_t max_cells,
                                                           lightusd_string** points,
                                                           lightusd_string** indices);
/* Isosurface-stuffing tetrahedra: settings_json {"spacing" (> 0, required),
 * "max_tets", "max_lattice_vertices", "threads", "device"}. The optional
 * "graded_coarse_spacing" (>= spacing) selects experimental serial CPU
 * grading; "graded_growth" (default 0.8) and "max_background_tets" (0 uses
 * the default) control its sizing and budget. "graded_surface_guard" enables
 * extra refinement of cells that may contain the surface. It assumes a
 * 1-Lipschitz SDF and can use substantially more memory. Small features may be missed,
 * and element-quality bounds are not guaranteed. Outputs float32 xyz points,
 * int32x4 tets (positive orientation) and int32x3 outward boundary triangles. */
LIGHTUSD_API lightusd_status lightusd_level_set_vdb_to_tet_mesh(
    const uint8_t* vdb, size_t size, const char* settings_json, lightusd_string** points,
    lightusd_string** tets, lightusd_string** surface);
/* A decoded level set, for several queries without re-reading VDB bytes
 * (the lightusd_level_set_vdb_* calls above decode on every call). A handle
 * does not reference the bytes or mesh it was made from. Queries only read
 * the handle and may run concurrently; destroy must not race them. Settings
 * and outputs are as for the matching *_vdb_* call. From VDB bytes, only the
 * grid that is chosen (and grids examined before it) is decoded. */
typedef struct lightusd_level_set lightusd_level_set;
LIGHTUSD_API lightusd_status lightusd_level_set_from_mesh(
    const float* points, size_t point_count, const int32_t* indices, size_t index_count,
    const int32_t* counts, size_t count_count, const char* settings_json,
    lightusd_level_set** out);
LIGHTUSD_API lightusd_status lightusd_level_set_from_vdb(const uint8_t* vdb, size_t size,
                                                        lightusd_level_set** out);
LIGHTUSD_API void lightusd_level_set_destroy(lightusd_level_set* level_set);
LIGHTUSD_API lightusd_status lightusd_level_set_get_info(const lightusd_level_set* level_set,
                                                        lightusd_level_set_info* out);
/* OpenVDB bytes (ZIP + active-mask compression). */
LIGHTUSD_API lightusd_status lightusd_level_set_write_vdb(const lightusd_level_set* level_set,
                                                         lightusd_string** vdb);
LIGHTUSD_API lightusd_status lightusd_level_set_sample(const lightusd_level_set* level_set,
                                                      const double* points, size_t count,
                                                      float* values);
LIGHTUSD_API lightusd_status lightusd_level_set_to_mesh(const lightusd_level_set* level_set,
                                                       uint64_t max_cells,
                                                       lightusd_string** points,
                                                       lightusd_string** indices);
LIGHTUSD_API lightusd_status lightusd_level_set_to_tet_mesh(
    const lightusd_level_set* level_set, const char* settings_json, lightusd_string** points,
    lightusd_string** tets, lightusd_string** surface);
/* Attach a generated file (for example a level-set .vdb a stage references
 * by the relative path): written beside the layer by USDA/USDC saves and
 * packaged under that path in USDZ saves. Replaces an existing entry. */
LIGHTUSD_API lightusd_status lightusd_stage_add_generated_asset(lightusd_stage* stage,
                                                               const char* path,
                                                               const uint8_t* data,
                                                               size_t size);
#ifdef __cplusplus
}
#endif
#endif
