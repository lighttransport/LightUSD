/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * LightUSD C API — tydra-next render-scene extraction.
 *
 * Convert a loaded stage into GPU-friendly render data (meshes, materials,
 * textures, lights, cameras, node hierarchy) with zero-copy buffer access.
 *
 * Buffer contract: lightusd_buffer_view.data stays valid until the owning
 * lightusd_render_scene is destroyed. Multi-chunk arrays are flattened once on
 * first request into a cache owned by the scene (thread-safe); single-chunk
 * arrays are returned without copying.
 */

#ifndef LIGHTUSD_RENDER_C_H_
#define LIGHTUSD_RENDER_C_H_

#include "lightusd-c.h"
#include "lightusd-session-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity-based viewer budgets. Fixed-width records; zero capacities use the
 * existing library defaults. quality: 0 full, 1 adaptive, 2 proxy. */
typedef struct lightusd_resource_budget {
  uint64_t host_capacity, host_limit, stage_limit, cpu_geometry_limit, io_cache_limit;
  uint64_t vram_capacity, vram_limit, gpu_geometry_limit, gpu_texture_limit;
  uint64_t upload_staging_limit, proxy_geometry_threshold;
  uint32_t texture_max_edge;
  uint32_t quality;
} lightusd_resource_budget;

typedef enum lightusd_texture_fit_policy {
  LIGHTUSD_TEXTURE_FIT_MODEST = 0,
  LIGHTUSD_TEXTURE_FIT_DEFAULT = 1,
  LIGHTUSD_TEXTURE_FIT_AGGRESSIVE = 2,
  LIGHTUSD_TEXTURE_FIT_NEVER = 3,
  LIGHTUSD_TEXTURE_FIT_ALWAYS = 4,
  LIGHTUSD_TEXTURE_FIT_ABSOLUTE = 5
} lightusd_texture_fit_policy;
typedef struct lightusd_texture_fit {
  uint64_t absolute_bytes;
  uint32_t policy;
  uint32_t reserved;
} lightusd_texture_fit;

/* Invalid arguments leave outputs unchanged. Text accepts modest/default/
 * aggressive/never/always or a positive byte count with optional K/M/G suffix. */
LIGHTUSD_API lightusd_status lightusd_resource_budget_compute(
    uint64_t host_capacity, uint64_t vram_capacity, uint32_t quality,
    lightusd_resource_budget* out);
LIGHTUSD_API lightusd_status lightusd_texture_fit_parse(const char* text, lightusd_texture_fit* out);
LIGHTUSD_API lightusd_status lightusd_texture_fit_threshold(
    const lightusd_texture_fit* fit, uint64_t vram_capacity, uint64_t* out);
LIGHTUSD_API uint64_t lightusd_budget_percent(uint64_t value, uint64_t percent);
/* Static policy label ("invalid" for an unknown policy), and fractional percent
 * (zero for non-fractional/unknown policies). No allocation or ownership transfer. */
LIGHTUSD_API const char* lightusd_texture_fit_name(uint32_t policy);
LIGHTUSD_API uint32_t lightusd_texture_fit_percent(uint32_t policy);

typedef struct lightusd_render_scene lightusd_render_scene;
typedef struct lightusd_render_session lightusd_render_session;
typedef struct lightusd_render_prepared_update lightusd_render_prepared_update;
typedef struct lightusd_render_prim_catalog lightusd_render_prim_catalog;
typedef struct lightusd_render_float_array lightusd_render_float_array;

typedef struct lightusd_render_particle_field_info {
  uint32_t struct_size;
  uint32_t spherical_harmonics_degree;
  uint64_t particle_count;
  char positions_property[64];
  char orientations_property[64];
  char scales_property[64];
  char opacities_property[64];
  char spherical_harmonics_property[64];
} lightusd_render_particle_field_info;
LIGHTUSD_API lightusd_status lightusd_render_query_particle_field(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    lightusd_render_particle_field_info* out);

/* Inherited render-stage traversal result. The stage owner must outlive the
 * catalog; its string views and records remain valid until catalog destruction.
 * Matrices are row-major. */
typedef enum lightusd_render_prim_kind {
  LIGHTUSD_RENDER_PRIM_MESH = 0,
  LIGHTUSD_RENDER_PRIM_POINTS = 1,
  LIGHTUSD_RENDER_PRIM_POINT_INSTANCER = 2,
  LIGHTUSD_RENDER_PRIM_NATIVE_INSTANCE = 3,
  LIGHTUSD_RENDER_PRIM_LIGHT = 4,
  LIGHTUSD_RENDER_PRIM_CAMERA = 5,
  LIGHTUSD_RENDER_PRIM_MATERIAL = 6,
  LIGHTUSD_RENDER_PRIM_VOLUME = 7,
  LIGHTUSD_RENDER_PRIM_CURVE = 8,
  LIGHTUSD_RENDER_PRIM_SKELETON = 9
} lightusd_render_prim_kind;
typedef struct lightusd_render_prim_info {
  uint32_t struct_size;
  uint32_t kind;
  uint8_t animated_world;
  uint8_t reserved[7];
  lightusd_sv path, type_name, purpose, material_path, native_prototype;
  double local[16], world[16];
} lightusd_render_prim_info;
LIGHTUSD_API lightusd_status lightusd_render_prim_catalog_create(
    const lightusd_stage* stage, double time_code, uint8_t stop_at_point_instancers,
    uint8_t stop_at_native_instances, uint8_t collect_other,
    lightusd_render_prim_catalog** out);
LIGHTUSD_API void lightusd_render_prim_catalog_destroy(
    lightusd_render_prim_catalog* catalog);
LIGHTUSD_API size_t lightusd_render_prim_catalog_count(
    const lightusd_render_prim_catalog* catalog, uint8_t kind);
LIGHTUSD_API lightusd_status lightusd_render_prim_catalog_get(
    const lightusd_render_prim_catalog* catalog, uint8_t kind, size_t index,
    lightusd_render_prim_info* out);

/* Lazy float-compatible attribute array. Data is copied by bounded range into
 * caller storage; the decoded/lazy backing remains private to this handle.
 * The stage owner must outlive the array handle; a pointer returned by `data`
 * remains valid until the handle is destroyed. */
LIGHTUSD_API lightusd_status lightusd_render_float_array_create(
    const lightusd_stage* stage, lightusd_prim prim, const char* property,
    double time_code, lightusd_render_float_array** out);
LIGHTUSD_API size_t lightusd_render_float_array_size(
    const lightusd_render_float_array* array);
LIGHTUSD_API lightusd_status lightusd_render_float_array_data(
    const lightusd_render_float_array* array, const float** out_data,
    size_t* out_count);
LIGHTUSD_API lightusd_status lightusd_render_float_array_copy(
    const lightusd_render_float_array* array, size_t first, size_t count,
    float* destination);
LIGHTUSD_API void lightusd_render_float_array_destroy(
    lightusd_render_float_array* array);

/* ============================================================
 * Conversion
 * ============================================================ */

typedef enum lightusd_material_binding_purpose {
  LIGHTUSD_MATERIAL_BINDING_DEFAULT = 0,
  LIGHTUSD_MATERIAL_BINDING_PREVIEW = 1,
  LIGHTUSD_MATERIAL_BINDING_FULL = 2
} lightusd_material_binding_purpose;

typedef struct lightusd_render_config {
  uint32_t struct_size;
  /* mesh */
  uint8_t triangulate;
  uint8_t compute_normals;
  uint8_t compute_tangents;
  uint8_t build_vertex_indices;
  /* material */
  uint8_t load_textures;
  uint8_t allow_missing_textures;
  uint8_t target_color_space; /* 0=srgb 1=linear 2=raw (tydra ColorSpace) */
  /* point instancer */
  uint8_t duplicate_instance_meshes;
  /* Formerly reserved bytes. Zero preserves the original C defaults. */
  uint8_t triangulation_method; /* 0=earcut, 1=fan */
  uint8_t tangent_method; /* 0=hybrid, 1=Lengyel, 2=MikkTSpace, 3=FastMikkTSpace */
  uint8_t discard_geometry; /* 1 releases source mesh geometry after conversion */
  uint8_t disable_animation;
  double time_code;
  /* execution/resource controls; zero limits are invalid */
  int32_t max_threads; /* 0=bounded auto, 1=serial, >1=fixed */
  uint8_t discard_instance_source_arrays; /* preference; draw expansion still retains required arrays */
  uint8_t use_default_asset_resolver; /* filesystem resolver anchored to source directory */
  uint8_t material_binding_purpose; /* lightusd_material_binding_purpose */
  uint8_t _pad1;
  uint64_t max_resident_bytes; /* estimated retained scene must fit; zero invalid */
  uint64_t max_render_records; /* 1..INT32_MAX or UNLIMITED (capped at INT32_MAX) */
  uint64_t max_render_depth;
  uint64_t max_value_clip_samples;
} lightusd_render_config;

LIGHTUSD_API void lightusd_render_config_init(lightusd_render_config* cfg);

typedef enum lightusd_render_light_type {
  LIGHTUSD_RENDER_LIGHT_POINT = 0,
  LIGHTUSD_RENDER_LIGHT_DIRECTIONAL = 1,
  LIGHTUSD_RENDER_LIGHT_SPOT = 2,
  LIGHTUSD_RENDER_LIGHT_RECT = 3,
  LIGHTUSD_RENDER_LIGHT_DISK = 4,
  LIGHTUSD_RENDER_LIGHT_DOME = 5,
  LIGHTUSD_RENDER_LIGHT_SPHERE = 6,
  LIGHTUSD_RENDER_LIGHT_CYLINDER = 7,
  LIGHTUSD_RENDER_LIGHT_GEOMETRY = 8
} lightusd_render_light_type;

/* Converter-backed light schema sample for one prim. `shape[0..1]` means
 * distant angle, spot angle in radians, rect width/height, cylinder
 * radius/length, or the applicable radius for disk/sphere. Flags: bit 0
 * normalize, bit 1 color temperature enabled, bit 2 IES normalize, bit 3
 * shadows enabled. IES asset and relationship targets remain available via
 * ordinary prim/property/relationship queries. */
typedef struct lightusd_render_light_query_info {
  uint32_t struct_size;
  uint32_t type;
  uint32_t flags;
  float color[3];
  float intensity, exposure, diffuse, specular;
  float color_temperature;
  float shaping_cone_angle, shaping_focus, shaping_focus_tint[3];
  float shaping_cone_softness, shaping_ies_angle_scale;
  float shadow_color[3], shadow_distance, shadow_falloff, shadow_falloff_gamma;
  float shape[2];
} lightusd_render_light_query_info;

LIGHTUSD_API void lightusd_render_light_query_info_init(
    lightusd_render_light_query_info* info);
LIGHTUSD_API lightusd_status lightusd_render_query_light(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    lightusd_render_light_query_info* out);
/* Convert one BasisCurves/NurbsCurves/HermiteCurves prim into an immutable
 * render scene containing that curve record. Clip assets resolve relative to
 * the source stage directory. A zero tessellation count selects one segment. */
LIGHTUSD_API lightusd_status lightusd_render_convert_curves(
    const lightusd_stage* stage, lightusd_prim prim, double time_code,
    uint32_t tessellation_segments, const char* const* asset_search_paths,
    size_t asset_search_path_count, uint8_t allow_parent_paths,
    lightusd_render_scene** out);
/* Convert one mesh prim into a scene containing one mesh record. This keeps
 * per-prim conversion bounded for streaming consumers. subdivision_level is
 * clamped to the converter's supported range. */
LIGHTUSD_API lightusd_status lightusd_render_convert_mesh(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* config, int32_t subdivision_level,
    lightusd_render_scene** out);
/* Bounded preflight for one Mesh prim; it does not materialize geometry. */
LIGHTUSD_API lightusd_status lightusd_render_estimate_mesh_bytes(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* config, int32_t subdivision_level,
    uint64_t* out_bytes);
/* Convert a deferred/unsupported mesh proxy. proxy_kind 0 derives the proxy
 * from authored extent; proxy_kind 1 uses the supplied bounds. */
LIGHTUSD_API lightusd_status lightusd_render_convert_mesh_proxy(
    const lightusd_stage* stage, lightusd_prim prim, uint8_t proxy_kind,
    const float bounds_min[3], const float bounds_max[3],
    lightusd_render_scene** out);
/* Convert one PointInstancer prim into an owning scene with its source arrays,
 * prototype paths, visibility and sampled instance transforms. */
LIGHTUSD_API lightusd_status lightusd_render_convert_instancer(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* config, lightusd_render_scene** out);
/* Convert one bound Material prim and its texture metadata into an owning
 * scene containing one material record. Image pixels are loaded only when
 * requested by config->load_textures. */
LIGHTUSD_API lightusd_status lightusd_render_convert_material(
    const lightusd_stage* stage, lightusd_prim prim,
    const lightusd_render_config* config, lightusd_render_scene** out);

LIGHTUSD_API lightusd_status lightusd_render_convert(const lightusd_stage* stage,
                                         const lightusd_render_config* cfg,
                                         lightusd_render_scene** out);
LIGHTUSD_API void lightusd_render_scene_destroy(lightusd_render_scene* scene);
LIGHTUSD_API lightusd_status lightusd_render_scene_warnings(const lightusd_render_scene* scene,
                                                lightusd_strlist** out);
/* Export a self-contained GLB. `source_path` anchors relative texture assets;
 * for USDZ it also identifies the package root layer. An empty path uses the
 * paths already stored in the scene. `max_output_bytes` must be nonzero.
 * On success, `glb` owns binary bytes (including NULs) accessible through
 * lightusd_string_view. `losses` includes converter warnings and is returned
 * even for a strict-mode refusal. */
LIGHTUSD_API lightusd_status lightusd_render_export_glb(
    const lightusd_render_scene* scene, const char* source_path,
    uint8_t strict, uint64_t max_output_bytes, lightusd_string** glb,
    lightusd_strlist** losses);

/* ============================================================
 * Persistent conversion session
 * ============================================================
 *
 * A session preserves Tydra's published render revision between conversions.
 * It is bound to the source directory of the stage passed to create, so all
 * later stages must use that same asset-resolution root. Update publishes a
 * full-resync transaction. Apply accepts a change list and uses an incremental
 * path when Tydra supports the requested changes. Both operations clone mutable
 * stages, or retain an immutable document-snapshot stage without cloning.
 * The returned scene handle retains immutable
 * snapshot data and remains valid across later session updates or reset.
 */

typedef struct lightusd_render_update_info {
  uint32_t struct_size;
  uint8_t full_resync;
  uint8_t _pad[3];
  uint64_t revision;
  uint64_t converted_resource_count;
  uint64_t converted_scene_bytes;
  uint64_t upsert_count;
  uint64_t remove_count;
} lightusd_render_update_info;

LIGHTUSD_API void lightusd_render_update_info_init(
    lightusd_render_update_info* info);

typedef struct lightusd_render_change_set {
  uint32_t struct_size;
  uint8_t full_resync;
  uint8_t stage_metadata_changed;
  uint8_t _pad[2];
  uint64_t base_revision;
  const lightusd_prim_change* prims;
  size_t prim_count;
} lightusd_render_change_set;

LIGHTUSD_API void lightusd_render_change_set_init(
    lightusd_render_change_set* changes);

/* Initialize update-info and change-set structs to zero, then set struct_size
 * to sizeof(the struct) before passing them to this ABI. */

typedef enum lightusd_render_event_type {
  LIGHTUSD_RENDER_EVENT_BEGIN = 0,
  LIGHTUSD_RENDER_EVENT_UPSERT = 1,
  LIGHTUSD_RENDER_EVENT_REMOVE = 2,
  LIGHTUSD_RENDER_EVENT_END = 3,
  LIGHTUSD_RENDER_EVENT_ABORT = 4
} lightusd_render_event_type;

typedef struct lightusd_render_event {
  uint32_t struct_size;
  uint8_t type; /* lightusd_render_event_type */
  uint8_t kind; /* event kind: 1=node, 2=mesh, 3=points, 4=curves,
                 * 5=instancer, 6=material, 7=texture, 8=image, 9=light,
                 * 10=camera, 11=animation, 12=skeleton; differs from
                 * lightusd_render_kind */
  uint8_t full_resync;
  uint8_t _pad;
  int32_t record_index; /* upsert index in the prepared scene; -1 otherwise */
  uint64_t base_revision;
  uint64_t revision;
  uint64_t resource_id;
  lightusd_sv key; /* stable resource key; empty for begin/end/abort */
} lightusd_render_event;

typedef int (*lightusd_render_event_fn)(void* userdata,
                                        const lightusd_render_event* event);

typedef struct lightusd_render_event_sink {
  uint32_t struct_size;
  lightusd_render_event_fn callback;
  void* userdata;
} lightusd_render_event_sink;

LIGHTUSD_API void lightusd_render_event_sink_init(
    lightusd_render_event_sink* sink);

LIGHTUSD_API lightusd_status lightusd_render_session_create(
    const lightusd_stage* stage, const lightusd_render_config* cfg,
    lightusd_render_session** out);
/* Bind directly to a persistent document session. Snapshot updates retain
 * their immutable stage; they do not clone the document before conversion. */
LIGHTUSD_API lightusd_status lightusd_render_session_create_document(
    const lightusd_document_session* document,
    const lightusd_render_config* cfg, lightusd_render_session** out);
LIGHTUSD_API void lightusd_render_session_destroy(lightusd_render_session* session);
LIGHTUSD_API uint64_t lightusd_render_session_revision(
    const lightusd_render_session* session);
/* kind uses the event values: 1=node, 2=mesh, ..., 12=skeleton. */
LIGHTUSD_API uint64_t lightusd_render_session_resource_id(
    const lightusd_render_session* session, uint8_t kind, const char* key);
LIGHTUSD_API void lightusd_render_session_reset(lightusd_render_session* session);
LIGHTUSD_API lightusd_status lightusd_render_session_set_event_sink(
    lightusd_render_session* session, const lightusd_render_event_sink* sink);
LIGHTUSD_API lightusd_status lightusd_render_session_update(
    lightusd_render_session* session, const lightusd_stage* stage,
    lightusd_render_scene** out, lightusd_render_update_info* info);
LIGHTUSD_API lightusd_status lightusd_render_session_apply(
    lightusd_render_session* session, const lightusd_stage* stage,
    const lightusd_render_change_set* changes, lightusd_render_scene** out,
    lightusd_render_update_info* info);
LIGHTUSD_API lightusd_status lightusd_render_session_apply_document(
    lightusd_render_session* session,
    const lightusd_document_snapshot* snapshot,
    lightusd_render_scene** out, lightusd_render_update_info* info);
/* Prepare converts and allocates a candidate without invoking the event sink
 * or publishing a revision. Commit emits the candidate and publishes it only
 * after the sink accepts the transaction. A successful commit consumes the
 * prepared handle; callers must use abort to discard a candidate when commit
 * is not called or returns an error. The prepared handle belongs to the
 * creating session and must not be used after that session is destroyed. */
LIGHTUSD_API lightusd_status lightusd_render_session_prepare(
    lightusd_render_session* session, const lightusd_stage* stage,
    const lightusd_render_change_set* changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info);
LIGHTUSD_API lightusd_status lightusd_render_session_prepare_document(
    lightusd_render_session* session,
    const lightusd_document_snapshot* snapshot,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info);
/* Prepare a document snapshot with caller-aggregated changes spanning skipped
 * document revisions. The target revision and source anchor come from snapshot;
 * changes replaces its last-edit record. Strings are copied during this call.
 * Revision mismatches retain the usual full-resync fallback. Failure clears out. */
LIGHTUSD_API lightusd_status lightusd_render_session_prepare_document_changes(
    lightusd_render_session* session, const lightusd_document_snapshot* snapshot,
    const lightusd_render_change_set* changes,
    lightusd_render_prepared_update** out, lightusd_render_update_info* info);
/* Retain the immutable candidate scene before commit. The caller owns `out`
 * and may query it from event callbacks using each upsert's record_index. The
 * handle remains valid after commit, rejection, or abort. */
LIGHTUSD_API lightusd_status lightusd_render_prepared_scene_copy(
    const lightusd_render_prepared_update* prepared,
    lightusd_render_scene** out);
LIGHTUSD_API lightusd_status lightusd_render_session_commit(
    lightusd_render_session* session, lightusd_render_prepared_update* prepared,
    lightusd_render_scene** out, lightusd_render_update_info* info);
LIGHTUSD_API void lightusd_render_session_abort(
    lightusd_render_session* session, lightusd_render_prepared_update* prepared);

/* ============================================================
 * Scene-level info
 * ============================================================ */

typedef enum lightusd_render_kind {
  LIGHTUSD_RENDER_NODE = 0,
  LIGHTUSD_RENDER_MESH = 1,
  LIGHTUSD_RENDER_MATERIAL = 2,
  LIGHTUSD_RENDER_TEXTURE = 3,
  LIGHTUSD_RENDER_IMAGE = 4,
  LIGHTUSD_RENDER_LIGHT = 5,
  LIGHTUSD_RENDER_CAMERA = 6,
  LIGHTUSD_RENDER_SKELETON = 7,
  LIGHTUSD_RENDER_ANIMATION = 8,
  LIGHTUSD_RENDER_UNSUPPORTED = 9,
  LIGHTUSD_RENDER_INSTANCER = 10,
  LIGHTUSD_RENDER_ROOT_NODE = 11,
  LIGHTUSD_RENDER_POINTS = 12,
  LIGHTUSD_RENDER_CURVES = 13,
  LIGHTUSD_RENDER_POINT_INSTANCE_DRAW = 14
} lightusd_render_kind;

LIGHTUSD_API size_t lightusd_render_count(const lightusd_render_scene* scene,
                                  uint8_t kind);
/* Path-index lookup for path-bearing resource kinds; -1 if absent. Images use
 * their resolved filesystem path as the lookup key. Instance draws share their
 * instancer path, so lookup returns the first matching draw. */
LIGHTUSD_API int32_t lightusd_render_lookup(const lightusd_render_scene* scene,
                                    uint8_t kind, const char* prim_path);
LIGHTUSD_API int32_t lightusd_render_root_node(const lightusd_render_scene* scene,
                                       size_t index);

typedef struct lightusd_render_scene_info {
  lightusd_sv name;
  lightusd_sv default_prim;
  lightusd_sv render_settings_path;
  lightusd_sv working_color_space;
  float meters_per_unit;
  uint8_t up_axis; /* 0=Y 1=Z */
  double start_time;
  double end_time;
  double frames_per_second;
  float working_to_display_linear[9];
} lightusd_render_scene_info;

LIGHTUSD_API lightusd_status lightusd_render_scene_get_info(const lightusd_render_scene* scene,
                                                lightusd_render_scene_info* out);
LIGHTUSD_API size_t lightusd_render_scene_memory_bytes(
    const lightusd_render_scene* scene);

typedef struct lightusd_render_stats {
  uint64_t node_count;
  uint64_t mesh_count;
  uint64_t points_count;
  uint64_t point_cloud_point_count;
  uint64_t curves_count;
  uint64_t curve_count;
  uint64_t curve_tessellated_point_count;
  uint64_t point_instancer_count;
  uint64_t point_instance_count;
  uint64_t visible_point_instance_count;
  uint64_t point_instance_draw_count;
  uint64_t material_count;
  uint64_t texture_count;
  uint64_t image_count;
  uint64_t light_count;
  uint64_t camera_count;
  uint64_t animation_count;
  uint64_t skeleton_count;
  uint64_t total_vertices;
  uint64_t total_triangles;
  uint64_t memory_bytes;
} lightusd_render_stats;

LIGHTUSD_API lightusd_status lightusd_render_scene_get_stats(
    const lightusd_render_scene* scene, lightusd_render_stats* out);

/* Retained USD Physics annotations. These are descriptive records; Tydra does
 * not simulate physics. `flags` are kind-specific: rigid bodies use bits 0/1
 * for rigid-body/kinematic enabled, bit 2 for starts-asleep, and bit 3 for
 * authored mass; colliders use bits 0/1 for collision/mesh collision;
 * joints use bit 0 for collision enabled. vector0..2 and scalar0..5 carry the
 * kind-specific POD values documented by the corresponding next record. */
typedef enum lightusd_render_physics_kind {
  LIGHTUSD_RENDER_PHYSICS_SCENE = 0,
  LIGHTUSD_RENDER_PHYSICS_RIGID_BODY = 1,
  LIGHTUSD_RENDER_PHYSICS_COLLIDER = 2,
  LIGHTUSD_RENDER_PHYSICS_JOINT = 3,
  LIGHTUSD_RENDER_PHYSICS_MATERIAL = 4,
  LIGHTUSD_RENDER_PHYSICS_FILTERED_PAIR = 5,
  LIGHTUSD_RENDER_PHYSICS_ARTICULATION_ROOT = 6
} lightusd_render_physics_kind;

typedef struct lightusd_render_physics_info {
  uint8_t kind;
  uint8_t flags;
  uint8_t _pad[2];
  lightusd_sv prim_path;
  lightusd_sv type_name;
  lightusd_sv body0;
  lightusd_sv body1;
  lightusd_sv simulation_owner;
  lightusd_sv approximation;
  float vector0[4];
  float vector1[4];
  float vector2[4];
  float scalar0;
  float scalar1;
  float scalar2;
  float scalar3;
  float scalar4;
  float scalar5;
} lightusd_render_physics_info;

LIGHTUSD_API size_t lightusd_render_physics_count(
    const lightusd_render_scene* scene, uint8_t kind);
LIGHTUSD_API lightusd_status lightusd_render_physics_get_info(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    lightusd_render_physics_info* out);
LIGHTUSD_API lightusd_status lightusd_render_physics_extension_property(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    size_t property_index, lightusd_sv* name, lightusd_sv* value);
/* Copy a filtered-pair or articulation-root path. `which` is currently zero
 * and follows the usual size-query semantics. */
LIGHTUSD_API lightusd_status lightusd_render_physics_string_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    uint8_t which, size_t string_index, char* out, size_t cap,
    size_t* required);

/* Stable, allocation-free enumeration of scene records. The returned key is
 * borrowed from the immutable scene and is suitable for resource-event
 * correlation. Records are ordered by kind and then source vector index.
 * For LIGHTUSD_RENDER_ROOT_NODE, `id` is the underlying node ID and `key` is
 * the node's prim path; lookup returns the root-list index. */
typedef struct lightusd_render_record {
  uint8_t kind;
  uint8_t _pad[3];
  int32_t id;
  lightusd_sv key;
} lightusd_render_record;

LIGHTUSD_API lightusd_status lightusd_render_record_get(
    const lightusd_render_scene* scene, uint8_t kind, size_t index,
    lightusd_render_record* out);
/* Copy a record key into caller-owned UTF-8 storage. `required` receives the
 * byte count excluding a NUL terminator. Pass out=NULL/cap=0 to query size. */
LIGHTUSD_API lightusd_status lightusd_render_record_key_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index, char* out,
    size_t cap, size_t* required);
/* Copy a named record's display name. Root-node records use the underlying
 * node name. Unsupported diagnostics and instance draws have no independent
 * name and return NOT_FOUND. Pass out=NULL/cap=0 to query size. */
LIGHTUSD_API lightusd_status lightusd_render_record_name_copy(
    const lightusd_render_scene* scene, uint8_t kind, size_t index, char* out,
    size_t cap, size_t* required);
/* Copy an instancer prototype path into caller-owned UTF-8 storage. */
LIGHTUSD_API lightusd_status lightusd_render_instancer_prototype_path_copy(
    const lightusd_render_scene* scene, int32_t instancer_id,
    size_t prototype_index, char* out, size_t cap, size_t* required);

/* ============================================================
 * Per-object info (one call copies the whole POD block)
 * ============================================================ */

typedef struct lightusd_render_node_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint8_t type; /* tydra NodeType */
  uint8_t visible;
  int32_t data_id;
  int32_t parent_id;
  uint32_t child_count;
  float local_transform[16];
  float world_transform[16];
} lightusd_render_node_info;

LIGHTUSD_API lightusd_status lightusd_render_node_get_info(const lightusd_render_scene* scene,
                                               int32_t id,
                                               lightusd_render_node_info* out);
/* Copies up to cap child ids; returns total child count. */
LIGHTUSD_API size_t lightusd_render_node_children(const lightusd_render_scene* scene,
                                          int32_t id, int32_t* out,
                                          size_t cap);

typedef struct lightusd_render_mesh_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint64_t point_count;
  uint64_t face_count;
  int32_t material_id;
  uint8_t is_triangulated;
  uint8_t has_normals;
  uint8_t has_tangents;
  uint8_t has_texcoords0;
  uint8_t has_texcoords1;
  uint8_t has_colors;
  uint8_t has_skin;
  uint8_t has_bbox;
  uint8_t normals_interp;    /* tydra Interpolation */
  uint8_t texcoords0_interp;
  uint8_t colors_interp;
  uint8_t _pad;
  uint32_t subset_count;
  uint32_t primvar_count;
  uint32_t blend_shape_count;
  int32_t skeleton_id; /* -1 when unskinned */
  float bbox_min[3];
  float bbox_max[3];
} lightusd_render_mesh_info;

LIGHTUSD_API lightusd_status lightusd_render_mesh_get_info(const lightusd_render_scene* scene,
                                               int32_t id,
                                               lightusd_render_mesh_info* out);

typedef struct lightusd_render_mesh_extra_info {
  uint8_t tangents_interp;  /* tydra Interpolation */
  uint8_t opacities_interp; /* tydra Interpolation */
  uint8_t colors_components; /* 0 absent, otherwise 3 or 4 */
  uint8_t double_sided;
  lightusd_sv texcoords0_name;
  lightusd_sv texcoords1_name;
} lightusd_render_mesh_extra_info;
LIGHTUSD_API lightusd_status lightusd_render_mesh_get_extra_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_mesh_extra_info* out);

LIGHTUSD_API lightusd_status lightusd_render_mesh_subset(const lightusd_render_scene* scene,
                                             int32_t mesh_id, size_t index,
                                             uint32_t* face_start,
                                             uint32_t* face_count,
                                             int32_t* material_id);

typedef struct lightusd_render_primvar_info {
  lightusd_sv name;
  uint8_t format;        /* tydra VertexFormat */
  uint8_t interpolation; /* tydra Interpolation */
  uint8_t has_indices;
  uint8_t _pad;
  uint64_t element_count;
} lightusd_render_primvar_info;

LIGHTUSD_API lightusd_status lightusd_render_mesh_primvar_info(
    const lightusd_render_scene* scene, int32_t mesh_id, size_t index,
    lightusd_render_primvar_info* out);

/* ============================================================
 * Buffers (zero-copy or flatten-once-cached)
 * ============================================================ */

typedef struct lightusd_buffer_view {
  uint8_t component_type; /* lightusd_component_type */
  uint8_t components;     /* scalars per logical element */
  uint16_t _pad;
  uint32_t _pad2;
  size_t count; /* logical elements */
  const void* data;
  size_t nbytes;
} lightusd_buffer_view;

/* Copy a borrowed buffer view into caller-owned storage. `required` is always
 * written on success and on a short-buffer query. Pass out=NULL/cap=0 to
 * query the byte count. The view remains borrowed; this helper never changes
 * its lifetime or ownership. Source and destination may overlap. */
LIGHTUSD_API lightusd_status lightusd_buffer_copy(
    const lightusd_buffer_view* view, void* out, size_t cap,
    size_t* required);

typedef enum lightusd_mesh_buffer_kind {
  LIGHTUSD_MESH_BUF_POINTS = 0,          /* f32 x3 */
  LIGHTUSD_MESH_BUF_FACE_COUNTS = 1,     /* u32 x1 */
  LIGHTUSD_MESH_BUF_FACE_INDICES = 2,    /* u32 x1 */
  LIGHTUSD_MESH_BUF_TRI_INDICES = 3,     /* u32 x1 (triangulated) */
  LIGHTUSD_MESH_BUF_NORMALS = 4,         /* f32 x3 */
  LIGHTUSD_MESH_BUF_TANGENTS = 5,        /* f32 x4 */
  LIGHTUSD_MESH_BUF_TEXCOORDS0 = 6,      /* f32 x2 */
  LIGHTUSD_MESH_BUF_TEXCOORDS1 = 7,      /* f32 x2 */
  LIGHTUSD_MESH_BUF_COLORS = 8,          /* f32 x3 */
  LIGHTUSD_MESH_BUF_JOINT_INDICES = 9,   /* u16 x4 (skin) */
  LIGHTUSD_MESH_BUF_JOINT_WEIGHTS = 10,  /* f32 x4 (skin) */
  LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES = 11 /* u32 x1: per triangulated corner,
                                                the original faceVarying corner
                                                index (index faceVarying uv/
                                                normals against the triangles) */,
  LIGHTUSD_MESH_BUF_OPACITIES = 12,      /* f32 x1 */
  LIGHTUSD_MESH_BUF_SUBDIVISION_FACE_SOURCE = 13, /* u32 x1 */
  LIGHTUSD_MESH_BUF_FACE_TRIANGLE_OFFSETS = 14 /* u32 x1, face_count + 1 */
} lightusd_mesh_buffer_kind;

LIGHTUSD_API lightusd_status lightusd_render_mesh_buffer(lightusd_render_scene* scene,
                                             int32_t mesh_id, uint8_t kind,
                                             lightusd_buffer_view* out);
/* which: 0=data, 1=indices */
LIGHTUSD_API lightusd_status lightusd_render_mesh_primvar_buffer(lightusd_render_scene* scene,
                                                     int32_t mesh_id,
                                                     size_t primvar_index,
                                                     uint8_t which,
                                                     lightusd_buffer_view* out);
/* which: 0=point_offsets, 1=normal_offsets. name/weight optional. */
LIGHTUSD_API lightusd_status lightusd_render_mesh_blendshape(
    lightusd_render_scene* scene, int32_t mesh_id, size_t bs_index, uint8_t which,
    lightusd_sv* name, float* weight, lightusd_buffer_view* out);

typedef enum lightusd_points_buffer_kind {
  LIGHTUSD_POINTS_BUF_POSITIONS = 0, /* f32 x3 */
  LIGHTUSD_POINTS_BUF_WIDTHS = 1,    /* f32 x1 */
  LIGHTUSD_POINTS_BUF_COLORS = 2     /* f32 x3 */
} lightusd_points_buffer_kind;
LIGHTUSD_API lightusd_status lightusd_render_points_buffer(
    lightusd_render_scene* scene, int32_t points_id, uint8_t kind,
    lightusd_buffer_view* out);

typedef enum lightusd_curves_buffer_kind {
  LIGHTUSD_CURVES_BUF_POINTS = 0,            /* f32 x3 */
  LIGHTUSD_CURVES_BUF_TESSELLATED_POINTS = 1,/* f32 x3 */
  LIGHTUSD_CURVES_BUF_WIDTHS = 2,            /* f32 x1 */
  LIGHTUSD_CURVES_BUF_COLORS = 3,            /* f32 x3 */
  LIGHTUSD_CURVES_BUF_VERTEX_COUNTS = 4,     /* u32 x1 */
  LIGHTUSD_CURVES_BUF_TESSELLATED_COUNTS = 5,/* u32 x1 */
  LIGHTUSD_CURVES_BUF_OPACITIES = 6,          /* f32 x1 */
  LIGHTUSD_CURVES_BUF_TESSELLATED_WIDTHS = 7,/* f32 x1 */
  LIGHTUSD_CURVES_BUF_TESSELLATED_COLORS = 8,/* f32 x3 */
  LIGHTUSD_CURVES_BUF_TESSELLATED_OPACITIES = 9 /* f32 x1 */
} lightusd_curves_buffer_kind;
LIGHTUSD_API lightusd_status lightusd_render_curves_buffer(
    lightusd_render_scene* scene, int32_t curves_id, uint8_t kind,
    lightusd_buffer_view* out);
typedef struct lightusd_render_curves_info {
  uint64_t curve_count;
  uint64_t control_point_count;
  uint64_t tessellated_point_count;
  uint8_t type;
  uint8_t basis;
  uint8_t wrap;
  uint8_t is_nurbs;
  uint8_t is_hermite;
  uint8_t widths_interpolation;
  uint8_t colors_interpolation;
  uint8_t opacities_interpolation;
  uint8_t _pad[4];
} lightusd_render_curves_info;
LIGHTUSD_API lightusd_status lightusd_render_curves_get_info(
    const lightusd_render_scene* scene, int32_t curves_id,
    lightusd_render_curves_info* out);

/* ============================================================
 * Materials
 * ============================================================ */

typedef struct lightusd_render_material_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint8_t shader_type; /* 0=none 1=preview_surface 2=openpbr */
  uint8_t double_sided;
  uint8_t alpha_mode; /* 0=opaque 1=mask 2=blend */
  uint8_t default_fallback;
  float alpha_cutoff;
  uint8_t has_displacement;
  uint8_t has_volume;
  uint8_t use_specular_workflow;
  uint8_t _pad;
} lightusd_render_material_info;

LIGHTUSD_API lightusd_status lightusd_render_material_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_material_info* out);

typedef struct lightusd_render_materialx_config_info {
  uint8_t authored;
  uint8_t _pad[7];
  lightusd_sv version;
  lightusd_sv name_space;
  lightusd_sv colorspace;
  lightusd_sv source_uri;
} lightusd_render_materialx_config_info;

LIGHTUSD_API lightusd_status lightusd_render_material_mtlx_config(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_materialx_config_info* out);

/* Copy retained displacement/volume terminal paths. `which` is 0=displacement
 * and 1=volume. Uses lightusd_sv_copy size-query semantics. */
LIGHTUSD_API lightusd_status lightusd_render_material_terminal_path_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which, char* out,
    size_t cap, size_t* required);

typedef struct lightusd_render_material_diagnostic {
  uint8_t kind; /* MaterialDiagnosticKind */
  uint8_t _pad[7];
  lightusd_sv material_path;
  lightusd_sv node_path;
  lightusd_sv shader_id;
  lightusd_sv message;
} lightusd_render_material_diagnostic;

LIGHTUSD_API size_t lightusd_render_material_diagnostic_count(
    const lightusd_render_scene* scene, int32_t id);
LIGHTUSD_API lightusd_status lightusd_render_material_diagnostic_get(
    const lightusd_render_scene* scene, int32_t id, size_t index,
    lightusd_render_material_diagnostic* out);

/* Copy retained shader/node-graph JSON. `which`: 0=OpenPBR surface graph,
 * 1=MaterialX volume graph, 2=utility graph feeding a PreviewSurface.
 * Uses lightusd_sv_copy size-query semantics. */
LIGHTUSD_API lightusd_status lightusd_render_material_nodegraph_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which, char* out,
    size_t cap, size_t* required);

/* Named shader parameter. PreviewSurface names: diffuse_color,
 * emissive_color, specular_color, metallic, roughness, clearcoat,
 * clearcoat_roughness, opacity, opacity_threshold, ior, normal,
 * displacement, occlusion. OpenPBR names: base_weight, base_color,
 * base_roughness, base_metalness, specular_weight, specular_color,
 * specular_roughness, specular_ior, transmission_weight, transmission_color,
 * subsurface_weight, subsurface_color, coat_weight, coat_color,
 * coat_roughness, sheen_weight, sheen_color, sheen_roughness,
 * emission_luminance, emission_color, opacity, normal.
 * On success: *texture_id >= 0 means a texture drives the param. */
LIGHTUSD_API lightusd_status lightusd_render_material_param(
    const lightusd_render_scene* scene, int32_t id, const char* param,
    int32_t* texture_id, float value[4]);

typedef struct lightusd_render_material_retained_param {
  lightusd_sv shader;
  lightusd_sv name;
  int32_t texture_id;
  float value[4];
} lightusd_render_material_retained_param;

LIGHTUSD_API size_t lightusd_render_material_retained_param_count(
    const lightusd_render_scene* scene, int32_t id);
LIGHTUSD_API lightusd_status lightusd_render_material_retained_param_get(
    const lightusd_render_scene* scene, int32_t id, size_t index,
    lightusd_render_material_retained_param* out);

typedef struct lightusd_render_texture_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  lightusd_sv asset_path;
  float uv_offset[2];
  float uv_scale[2];
  float uv_rotation;
  uint8_t wrap_s; /* tydra WrapMode */
  uint8_t wrap_t;
  uint8_t output_channel; /* tydra RenderTexture::Channel */
  uint8_t _pad;
  float bias[4];
  float scale[4];
  int32_t image_id;
  lightusd_sv ktx2_hint;
  lightusd_sv uv_primvar;
  lightusd_sv source_color_space;
  lightusd_sv target_color_space;
} lightusd_render_texture_info;

LIGHTUSD_API lightusd_status lightusd_render_texture_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_texture_info* out);

typedef struct lightusd_render_image_info {
  lightusd_sv name;
  lightusd_sv resolved_path;
  uint32_t width;
  uint32_t height;
  uint8_t channels;
  uint8_t component_type; /* tydra ComponentType */
  uint8_t color_space;    /* tydra ColorSpace */
  uint8_t is_loaded;
  uint64_t nbytes;
} lightusd_render_image_info;

LIGHTUSD_API lightusd_status lightusd_render_image_get_info(const lightusd_render_scene* scene,
                                                int32_t id,
                                                lightusd_render_image_info* out);
LIGHTUSD_API lightusd_status lightusd_render_image_buffer(lightusd_render_scene* scene,
                                              int32_t id,
                                              lightusd_buffer_view* out);

/* ============================================================
 * Lights / cameras
 * ============================================================ */

typedef struct lightusd_render_light_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint8_t type; /* tydra LightType */
  uint8_t normalize;
  uint8_t enable_shadow;
  uint8_t _pad;
  float color[3];
  float intensity;
  float exposure;
  float transform[16];
  /* type-specific scalars: sphere/disk radius, rect w/h, spot angle */
  float param0;
  float param1;
  lightusd_sv shaping_ies_file;
} lightusd_render_light_info;

LIGHTUSD_API lightusd_status lightusd_render_light_get_info(const lightusd_render_scene* scene,
                                                int32_t id,
                                                lightusd_render_light_info* out);
/* `which`: 0=light links, 1=shadow links, 2=filter targets. */
LIGHTUSD_API size_t lightusd_render_light_link_count(
    const lightusd_render_scene* scene, int32_t id, uint8_t which);
LIGHTUSD_API lightusd_status lightusd_render_light_link_copy(
    const lightusd_render_scene* scene, int32_t id, uint8_t which,
    size_t index, char* out, size_t cap, size_t* required);

typedef struct lightusd_render_camera_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint8_t type; /* 0=perspective 1=orthographic */
  uint8_t _pad[3];
  float focal_length;
  float horizontal_aperture;
  float vertical_aperture;
  float ortho_width;
  float near_clip;
  float far_clip;
  float fov_x;
  float fov_y;
  float transform[16];
  float focus_distance;
  float fstop;
  float aspect;
  float horizontal_aperture_offset;
  float vertical_aperture_offset;
  float exposure;
  uint8_t stereo_role; /* 0=mono 1=left 2=right */
  uint8_t _pad2[3];
  double shutter_open;
  double shutter_close;
} lightusd_render_camera_info;

LIGHTUSD_API lightusd_status lightusd_render_camera_get_info(
    const lightusd_render_scene* scene, int32_t id, lightusd_render_camera_info* out);

/* ============================================================
 * Skeletons / instancers (basic access)
 * ============================================================ */

typedef struct lightusd_render_skeleton_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint32_t joint_count;
  int32_t root_joint;
  int32_t animation_id;
  lightusd_sv animation_source_path;
} lightusd_render_skeleton_info;

LIGHTUSD_API lightusd_status lightusd_render_skeleton_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_skeleton_info* out);

typedef struct lightusd_render_joint_info {
  lightusd_sv name;
  lightusd_sv path;
  int32_t parent_id;
  float bind_transform[16];
  float rest_transform[16];
} lightusd_render_joint_info;

LIGHTUSD_API lightusd_status lightusd_render_skeleton_joint(const lightusd_render_scene* scene,
                                                int32_t skeleton_id,
                                                size_t joint_index,
                                                lightusd_render_joint_info* out);
/* Copy one joint's ordered child IDs. `required` receives an element count;
 * out=NULL/cap=0 queries the count without copying. */
LIGHTUSD_API lightusd_status lightusd_render_skeleton_joint_children_copy(
    const lightusd_render_scene* scene, int32_t skeleton_id,
    size_t joint_index, int32_t* out, size_t cap, size_t* required);

typedef enum lightusd_skeleton_buffer_kind {
  LIGHTUSD_SKELETON_BUF_BIND_TRANSFORMS = 0, /* f32 x16 */
  LIGHTUSD_SKELETON_BUF_REST_TRANSFORMS = 1, /* f32 x16 */
  LIGHTUSD_SKELETON_BUF_PARENT_IDS = 2       /* i32 x1 */
} lightusd_skeleton_buffer_kind;

LIGHTUSD_API lightusd_status lightusd_render_skeleton_buffer(
    lightusd_render_scene* scene, int32_t skeleton_id, uint8_t kind,
    lightusd_buffer_view* out);

/* ============================================================
 * Animation clips and channels
 * ============================================================ */

typedef struct lightusd_render_animation_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  double start_time;
  double end_time;
  uint32_t channel_count;
  uint32_t clip_asset_count;
  uint8_t value_clip_baked;
  uint8_t _pad[7];
} lightusd_render_animation_info;

LIGHTUSD_API lightusd_status lightusd_render_animation_get_info(
    const lightusd_render_scene* scene, int32_t animation_id,
    lightusd_render_animation_info* out);

typedef struct lightusd_render_animation_channel_info {
  uint8_t target_path; /* AnimationChannel::TargetPath */
  uint8_t interpolation; /* AnimationChannel::Interpolation */
  uint8_t is_skeletal;
  uint8_t _pad;
  int32_t target_node;
  int32_t target_skeleton;
  uint32_t keyframe_count;
  uint32_t joint_order_count;
  uint32_t blend_shape_order_count;
  uint32_t joint_remap_count;
  uint32_t element_count;
  uint32_t value_stride;
  lightusd_sv target_prim_path;
  lightusd_sv property_name;
  lightusd_sv target_skeleton_path;
} lightusd_render_animation_channel_info;

LIGHTUSD_API lightusd_status lightusd_render_animation_channel_get_info(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t channel_index, lightusd_render_animation_channel_info* out);

typedef enum lightusd_animation_buffer_kind {
  LIGHTUSD_ANIMATION_BUF_TIMES = 0,       /* f64 x1 */
  LIGHTUSD_ANIMATION_BUF_VALUES = 1,      /* f32 x component count */
  LIGHTUSD_ANIMATION_BUF_ARRAY_VALUES = 2,/* f32 x value_stride */
  LIGHTUSD_ANIMATION_BUF_JOINT_REMAP = 3  /* i32 x1 */
} lightusd_animation_buffer_kind;

LIGHTUSD_API lightusd_status lightusd_render_animation_channel_buffer(
    lightusd_render_scene* scene, int32_t animation_id, size_t channel_index,
    uint8_t kind, lightusd_buffer_view* out);

/* Copy one indexed channel string. `which` is 0=joint order, 1=blend-shape
 * order. Use lightusd_sv_copy semantics for size queries. */
LIGHTUSD_API lightusd_status lightusd_render_animation_channel_string_copy(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t channel_index, uint8_t which, size_t string_index, char* out,
    size_t cap, size_t* required);

/* Copy one source value-clip asset path. */
LIGHTUSD_API lightusd_status lightusd_render_animation_clip_asset_copy(
    const lightusd_render_scene* scene, int32_t animation_id,
    size_t asset_index, char* out, size_t cap, size_t* required);

typedef struct lightusd_render_instancer_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  lightusd_sv validation_error;
  uint64_t instance_count;
  uint64_t visible_instance_count;
  uint64_t draw_start;
  uint64_t draw_count;
  uint32_t prototype_count;
  uint8_t valid;
  uint8_t has_transforms;
  uint8_t has_orientations;
  uint8_t has_scales;
  uint8_t has_velocities;
  uint8_t has_angular_velocities;
  uint8_t _pad[2];
} lightusd_render_instancer_info;

LIGHTUSD_API lightusd_status lightusd_render_instancer_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_instancer_info* out);

typedef struct lightusd_render_point_instance_draw_info {
  int32_t point_instancer_id;
  uint32_t instance_index;
  uint32_t prototype_index;
  int32_t mesh_id;
  int32_t material_id;
  int32_t expanded_mesh_id;
  float transform[16];
} lightusd_render_point_instance_draw_info;

LIGHTUSD_API lightusd_status lightusd_render_point_instance_draw_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_point_instance_draw_info* out);

typedef enum lightusd_instancer_buffer_kind {
  LIGHTUSD_INST_BUF_PROTO_INDICES = 0, /* i32 x1 */
  LIGHTUSD_INST_BUF_POSITIONS = 1,     /* f32 x3 */
  LIGHTUSD_INST_BUF_ORIENTATIONS = 2,  /* f32 x4 */
  LIGHTUSD_INST_BUF_SCALES = 3,        /* f32 x3 */
  LIGHTUSD_INST_BUF_TRANSFORMS = 4,    /* f32 x16 */
  LIGHTUSD_INST_BUF_VISIBLE = 5,       /* u8 x1 */
  LIGHTUSD_INST_BUF_VELOCITIES = 6,    /* f32 x3 */
  LIGHTUSD_INST_BUF_ANGULAR_VELOCITIES = 7, /* f32 x3 */
  LIGHTUSD_INST_BUF_IDS = 8,           /* i64 x1 */
  LIGHTUSD_INST_BUF_INVISIBLE_IDS = 9, /* i64 x1 */
  LIGHTUSD_INST_BUF_INACTIVE_IDS = 10, /* i64 x1 */
  LIGHTUSD_INST_BUF_COMPACT = 11,      /* u8 x32, one packed record */
  LIGHTUSD_INST_BUF_PROTO_NODE_IDS = 12, /* i32 x1 */
  LIGHTUSD_INST_BUF_PROTO_MESH_OFFSETS = 13, /* u32 x1 */
  LIGHTUSD_INST_BUF_PROTO_MESH_IDS = 14, /* i32 x1 */
  LIGHTUSD_INST_BUF_PROTO_TRANSFORMS = 15 /* f32 x16 */
} lightusd_instancer_buffer_kind;

LIGHTUSD_API lightusd_status lightusd_render_instancer_buffer(lightusd_render_scene* scene,
                                                  int32_t id, uint8_t kind,
                                                  lightusd_buffer_view* out);

typedef struct lightusd_render_unsupported_info {
  lightusd_sv prim_path;
  lightusd_sv type_name;
  lightusd_sv reason;
} lightusd_render_unsupported_info;

LIGHTUSD_API lightusd_status lightusd_render_unsupported_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_unsupported_info* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUSD_RENDER_C_H_ */
