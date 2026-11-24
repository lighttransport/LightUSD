/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_WEB_BINDING_LEGACY_API_H_
#define LIGHTUSD_WEB_BINDING_LEGACY_API_H_
#include <stdint.h>

// Internal records and constants for the opt-in legacy embind module.
typedef struct lightusd_legacy_export_info {
  uint32_t struct_size;
  uint32_t reserved;
  double size;
  double data_ptr;
} lightusd_legacy_export_info;

typedef struct lightusd_legacy_mesh_pointer_info {
  uint32_t struct_size, flags; /* subsets=1, single index=2, triangles=4, display color=8, double sided=16 */
  int32_t material_id;
  uint32_t reserved;
  uint64_t vertex_count;
  uint32_t attribute_count, submesh_count;
  double display_color[3];
  uint64_t result;
} lightusd_legacy_mesh_pointer_info;

typedef struct lightusd_legacy_mesh_attribute {
  uint32_t struct_size, key, slot, dtype, components, reserved;
  uint64_t address, count;
} lightusd_legacy_mesh_attribute;

typedef struct lightusd_legacy_mesh_submesh {
  uint32_t struct_size;
  int32_t start, count, material_id;
} lightusd_legacy_mesh_submesh;

typedef struct lightusd_legacy_mesh_value_info {
  uint32_t struct_size, flags; /* double sided=1, area light=2, normalize=4,
                               geom bind authored=8, display color=16, subsets=32 */
  int32_t material_id, element_size, skeleton_id;
  uint32_t attribute_count, submesh_count, reserved;
  double display_color[3], light_intensity, light_exposure;
  uint64_t result;
} lightusd_legacy_mesh_value_info;

typedef struct lightusd_legacy_material_info {
  uint32_t struct_size, flags; /* MaterialX authored=1, PreviewSurface=2, specular workflow=4 */
  uint32_t texture_mask, reserved;
  int32_t texture_ids[13];
  uint32_t padding;
  double values[21];
} lightusd_legacy_material_info;

enum {
  LIGHTUSD_LEGACY_HAS_SUBLAYERS = 0,
  LIGHTUSD_LEGACY_HAS_REFERENCES = 1,
  LIGHTUSD_LEGACY_HAS_PAYLOAD = 2,
  LIGHTUSD_LEGACY_HAS_INHERITS = 3,
  LIGHTUSD_LEGACY_HAS_VARIANTS = 4,
  LIGHTUSD_LEGACY_COMPOSE_SUBLAYERS = 5,
  LIGHTUSD_LEGACY_COMPOSE_REFERENCES = 6,
  LIGHTUSD_LEGACY_COMPOSE_PAYLOAD = 7,
  LIGHTUSD_LEGACY_COMPOSE_INHERITS = 8,
  LIGHTUSD_LEGACY_COMPOSE_VARIANTS = 9,
  LIGHTUSD_LEGACY_LOD_VARIANT_COUNT = 10
};

enum {
  LIGHTUSD_LEGACY_SUBLAYER_ASSET_PATHS = 0,
  LIGHTUSD_LEGACY_REFERENCE_ASSET_PATHS = 1,
  LIGHTUSD_LEGACY_PAYLOAD_ASSET_PATHS = 2,
  LIGHTUSD_LEGACY_VARIANT_INFO = 3
};

enum {
  LIGHTUSD_LEGACY_STREAM_START = 0,
  LIGHTUSD_LEGACY_STREAM_APPEND = 1,
  LIGHTUSD_LEGACY_STREAM_FINALIZE = 2,
  LIGHTUSD_LEGACY_STREAM_COMPLETE = 3,
  LIGHTUSD_LEGACY_ZERO_PTR = 4,
  LIGHTUSD_LEGACY_ZERO_PTR_OFFSET = 5,
  LIGHTUSD_LEGACY_ZERO_MARK = 6,
  LIGHTUSD_LEGACY_ZERO_FINALIZE = 7,
  LIGHTUSD_LEGACY_ZERO_CANCEL = 8,
  LIGHTUSD_LEGACY_MMAP_SET = 9,
  LIGHTUSD_LEGACY_MMAP_GET = 10,
  LIGHTUSD_LEGACY_ZERO_KEYS = 11,
  LIGHTUSD_LEGACY_STREAM_SIZE_MAX = 12
};

enum {
  LIGHTUSD_LEGACY_ASSET_SET = 0,
  LIGHTUSD_LEGACY_ASSET_HAS = 1,
  LIGHTUSD_LEGACY_ASSET_DELETE = 2,
  LIGHTUSD_LEGACY_ASSET_DELETE_UUID = 3,
  LIGHTUSD_LEGACY_ASSET_DELETE_NAME = 4,
  LIGHTUSD_LEGACY_ASSET_COUNT = 5,
  LIGHTUSD_LEGACY_ASSET_CACHE_SIZE = 6,
  LIGHTUSD_LEGACY_ASSET_CACHE_LIMIT_SET = 7,
  LIGHTUSD_LEGACY_ASSET_CACHE_LIMIT_GET = 8,
  LIGHTUSD_LEGACY_ASSET_EXISTS = 9,
  LIGHTUSD_LEGACY_ASSET_CLEAR = 10,
  LIGHTUSD_LEGACY_ASSET_PARENT_PATHS_SET = 11,
  LIGHTUSD_LEGACY_ASSET_PARENT_PATHS_GET = 12,
  LIGHTUSD_LEGACY_ASSET_BASE_PATH_SET = 13,
  LIGHTUSD_LEGACY_ASSET_SEARCH_PATHS_CLEAR = 14,
  LIGHTUSD_LEGACY_ASSET_SEARCH_PATH_ADD = 15,
  LIGHTUSD_LEGACY_ASSET_VERIFY_HASH = 16
};

enum {
  LIGHTUSD_LEGACY_ASSET_HASH = 0,
  LIGHTUSD_LEGACY_ASSET_UUID = 1,
  LIGHTUSD_LEGACY_ASSET_STREAM_UUID = 2,
  LIGHTUSD_LEGACY_ASSET_FIND_UUID = 3,
  LIGHTUSD_LEGACY_ASSET_BASE_PATH = 4,
  LIGHTUSD_LEGACY_ASSET_SEARCH_PATHS = 5,
  LIGHTUSD_LEGACY_ASSET_UUIDS = 6
};

enum {
  LIGHTUSD_LEGACY_LOAD_BINARY = 0,
  LIGHTUSD_LEGACY_LOAD_LAYER = 1,
  LIGHTUSD_LEGACY_LOAD_PROGRESS = 2,
  LIGHTUSD_LEGACY_LOAD_LAYER_PROGRESS = 3,
  LIGHTUSD_LEGACY_LOAD_CACHED = 4,
  LIGHTUSD_LEGACY_LOAD_LAYER_CACHED = 5,
  LIGHTUSD_LEGACY_LOAD_JSON = 6,
  LIGHTUSD_LEGACY_LOAD_TEST = 7,
  LIGHTUSD_LEGACY_LOAD_CANCEL = 8,
  LIGHTUSD_LEGACY_LOAD_WAS_CANCELLED = 9,
  LIGHTUSD_LEGACY_LOAD_IN_PROGRESS = 10,
  LIGHTUSD_LEGACY_LOAD_RESET_PROGRESS = 11,
  LIGHTUSD_LEGACY_LOAD_RELEASE_SOURCE = 12,
  LIGHTUSD_LEGACY_LOAD_RESET = 13,
  LIGHTUSD_LEGACY_LOAD_OK = 14,
  LIGHTUSD_LEGACY_LOAD_ERROR = 15,
  LIGHTUSD_LEGACY_LOAD_WARNING = 16,
  LIGHTUSD_LEGACY_LOAD_VALIDATE_BINARY = 17,
  LIGHTUSD_LEGACY_LOAD_VALIDATE_LAYER = 18
};

enum {
  LIGHTUSD_LEGACY_MCP_CREATE_CONTEXT = 0,
  LIGHTUSD_LEGACY_MCP_SELECT_CONTEXT = 1,
  LIGHTUSD_LEGACY_MCP_TOOLS_LIST = 2,
  LIGHTUSD_LEGACY_MCP_TOOLS_CALL = 3,
  LIGHTUSD_LEGACY_MCP_RESOURCES_LIST = 4,
  LIGHTUSD_LEGACY_MCP_RESOURCES_READ = 5
};

enum {
  LIGHTUSD_LEGACY_CONFIG_COMBINE_UDIM_TILES = 0,          /* bool */
  LIGHTUSD_LEGACY_CONFIG_DEFER_TANGENT_COMPUTATION = 1,   /* bool */
  LIGHTUSD_LEGACY_CONFIG_ENABLE_BONE_REDUCTION = 2,       /* bool */
  LIGHTUSD_LEGACY_CONFIG_ENABLE_VALUE_CLIPS = 3,          /* bool */
  LIGHTUSD_LEGACY_CONFIG_MAX_MEMORY_LIMIT_MB = 4,         /* int32 */
  LIGHTUSD_LEGACY_CONFIG_ROUND_BONE_COUNT = 5,            /* bool */
  LIGHTUSD_LEGACY_CONFIG_SPHERE_SUBDIVISIONS = 6,         /* int32, 0-6 */
  LIGHTUSD_LEGACY_CONFIG_TARGET_BONE_COUNT = 7,           /* uint32, 1-128 */
  LIGHTUSD_LEGACY_CONFIG_VALUE_CLIP_SAMPLE_RATE = 8,      /* float */
  LIGHTUSD_LEGACY_CONFIG_VALUE_CLIP_USE_TIME_RANGE = 9,   /* bool */
  LIGHTUSD_LEGACY_CONFIG_VALUE_CLIP_START_TIME = 10,      /* get only */
  LIGHTUSD_LEGACY_CONFIG_VALUE_CLIP_END_TIME = 11,        /* get only */
  LIGHTUSD_LEGACY_CONFIG_VALUE_CLIP_TIME_RANGE = 12,      /* set only: a, b */
  LIGHTUSD_LEGACY_CONFIG_ENABLE_COMPOSITION = 13,         /* set only */
  LIGHTUSD_LEGACY_CONFIG_LOAD_TEXTURE_IN_NATIVE = 14,     /* set only */
  LIGHTUSD_LEGACY_CONFIG_NATIVE_FLATTEN_RENDER_TREE = 15, /* bool */
  LIGHTUSD_LEGACY_CONFIG_NATIVE_MATERIAL_DEDUP = 16,      /* bool */
  LIGHTUSD_LEGACY_CONFIG_NATIVE_MESH_MERGE = 17,          /* bool */
  LIGHTUSD_LEGACY_CONFIG_NATIVE_MESH_MERGE_BAKE_TRANSFORM = 18, /* bool */
  LIGHTUSD_LEGACY_CONFIG_USDC_EXPORT_LIMIT_MB = 19        /* set only: int32 a, b */
};
#endif
