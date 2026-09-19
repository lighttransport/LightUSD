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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lightusd_render_scene lightusd_render_scene;

/* ============================================================
 * Conversion
 * ============================================================ */

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
  uint8_t _pad0[4];
  double time_code;
  /* execution/resource controls; zero limits are invalid */
  int32_t max_threads; /* 0=bounded auto, 1=serial, >1=fixed */
  uint32_t _pad1;
  uint64_t max_resident_bytes;
  uint64_t max_render_records;
  uint64_t max_render_depth;
  uint64_t max_value_clip_samples;
} lightusd_render_config;

LIGHTUSD_API void lightusd_render_config_init(lightusd_render_config* cfg);

LIGHTUSD_API lightusd_status lightusd_render_convert(const lightusd_stage* stage,
                                         const lightusd_render_config* cfg,
                                         lightusd_render_scene** out);
LIGHTUSD_API void lightusd_render_scene_destroy(lightusd_render_scene* scene);
LIGHTUSD_API lightusd_status lightusd_render_scene_warnings(const lightusd_render_scene* scene,
                                                lightusd_strlist** out);

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
  LIGHTUSD_RENDER_INSTANCER = 9,
  LIGHTUSD_RENDER_ROOT_NODE = 10,
  LIGHTUSD_RENDER_UNSUPPORTED = 11
} lightusd_render_kind;

LIGHTUSD_API size_t lightusd_render_count(const lightusd_render_scene* scene,
                                  uint8_t kind);
/* Path-index lookup for NODE / MESH / MATERIAL / INSTANCER; -1 if absent. */
LIGHTUSD_API int32_t lightusd_render_lookup(const lightusd_render_scene* scene,
                                    uint8_t kind, const char* prim_path);
LIGHTUSD_API int32_t lightusd_render_root_node(const lightusd_render_scene* scene,
                                       size_t index);

typedef struct lightusd_render_scene_info {
  lightusd_sv name;
  lightusd_sv default_prim;
  float meters_per_unit;
  uint8_t up_axis; /* 0=Y 1=Z */
  double start_time;
  double end_time;
  double frames_per_second;
} lightusd_render_scene_info;

LIGHTUSD_API lightusd_status lightusd_render_scene_get_info(const lightusd_render_scene* scene,
                                                lightusd_render_scene_info* out);

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
                                                normals against the triangles) */
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

/* ============================================================
 * Materials
 * ============================================================ */

typedef struct lightusd_render_material_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint8_t shader_type; /* 0=none 1=preview_surface 2=openpbr */
  uint8_t double_sided;
  uint8_t alpha_mode; /* 0=opaque 1=mask 2=blend */
  uint8_t _pad;
  float alpha_cutoff;
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
} lightusd_render_light_info;

LIGHTUSD_API lightusd_status lightusd_render_light_get_info(const lightusd_render_scene* scene,
                                                int32_t id,
                                                lightusd_render_light_info* out);

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

typedef struct lightusd_render_instancer_info {
  lightusd_sv name;
  lightusd_sv prim_path;
  uint64_t instance_count;
  uint32_t prototype_count;
  uint8_t valid;
} lightusd_render_instancer_info;

LIGHTUSD_API lightusd_status lightusd_render_instancer_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_instancer_info* out);

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
  LIGHTUSD_INST_BUF_INACTIVE_IDS = 10  /* i64 x1 */
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
