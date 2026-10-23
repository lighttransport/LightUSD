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
#ifdef __cplusplus
}
#endif
#endif
