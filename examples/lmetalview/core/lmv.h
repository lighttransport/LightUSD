/* SPDX-License-Identifier: Apache-2.0
 *
 * lmetalview - preview scene ABI used by the Swift + Metal viewer.
 *
 * The core converts a USD file with lightusd-next + tydra-next and publishes a
 * flat, GPU-ready preview scene: interleaved triangle meshes, per-draw world
 * matrices, packed OpenPBR materials, decoded textures, lights and cameras.
 *
 * Conventions
 * - All matrices are 16 floats, COLUMN-MAJOR (Metal / simd_float4x4 ready).
 * - Pointer members are borrowed from the owning lmv_scene and stay valid until
 *   lmv_scene_free() or the next lmv_scene_set_time() on that scene.
 * - Every function is safe to call from a worker thread as long as the scene is
 *   not shared between threads at the same time (the viewer calls load/set_time
 *   on a background queue and hand the scene to the UI thread afterwards).
 * - No C++ exceptions cross this boundary; errors are lmv_status + a message.
 */
#ifndef LMV_H_
#define LMV_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status / progress                                                         */
/* ------------------------------------------------------------------------- */

typedef enum lmv_status {
  LMV_OK = 0,
  LMV_ERR_INVALID_ARG = 1,
  LMV_ERR_NOT_FOUND = 2,
  LMV_ERR_IO = 3,
  LMV_ERR_PARSE = 4,
  LMV_ERR_UNSUPPORTED = 5,
  LMV_ERR_LIMIT = 6,
  LMV_ERR_CANCELLED = 7,
  LMV_ERR_INTERNAL = 8
} lmv_status;

typedef enum lmv_phase {
  LMV_PHASE_IDLE = 0,
  LMV_PHASE_OPEN = 1,
  LMV_PHASE_COMPOSE = 2,
  LMV_PHASE_CONVERT = 3,
  LMV_PHASE_TEXTURES = 4,
  LMV_PHASE_DONE = 5,
  LMV_PHASE_FAILED = 6,
  LMV_PHASE_CANCELLED = 7
} lmv_phase;

typedef struct lmv_progress {
  int32_t phase;      /* lmv_phase */
  float fraction;     /* 0..1 within the phase, when known */
  const char* message; /* borrowed; valid for the duration of the callback */
} lmv_progress;

/* Return 0 to cancel the load / conversion. Must be cheap and thread-safe. */
typedef int (*lmv_progress_fn)(void* user, const lmv_progress* progress);

/* ------------------------------------------------------------------------- */
/* Load options                                                              */
/* ------------------------------------------------------------------------- */

typedef struct lmv_load_options {
  uint32_t struct_size; /* = sizeof(lmv_load_options) */

  /* Memory ceilings. 0 selects the core default (1 GiB stage budget). */
  uint64_t max_memory_bytes;
  /* Texture decode budget. 0 = 512 MiB and a 2048-texel longest edge. */
  uint64_t max_texture_bytes;
  uint32_t max_texture_edge;
  /* Geometry cap. 0 = 20M triangles. Meshes past the cap are skipped. */
  uint32_t max_triangles;

  /* Initial time code for conversion. */
  double time_code;

  /* Optional variant override (both non-NULL). */
  const char* variant_set;
  const char* variant_name;

  uint8_t load_payloads;
  uint8_t _pad[7];

  lmv_progress_fn progress;
  void* progress_user;
} lmv_load_options;

/* Fills every field with the defaults. */
void lmv_load_options_init(lmv_load_options* opts);

/* ------------------------------------------------------------------------- */
/* Scene                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct lmv_scene lmv_scene;

/* Load, compose and convert `path`. On failure returns a non-OK status and
 * writes a human-readable message into `err` (when err/cap are non-zero). */
lmv_status lmv_load(const char* path, const lmv_load_options* opts,
                    lmv_scene** out, char* err, size_t err_cap);

void lmv_scene_free(lmv_scene* scene);

/* Re-convert the retained stage at `time_code` (animation scrubbing). Reuses
 * the composed stage and the decoded texture cache, so nothing is re-parsed.
 * On failure the previous scene contents are kept. */
lmv_status lmv_scene_set_time(lmv_scene* scene, double time_code, char* err,
                              size_t err_cap);

double lmv_scene_time(const lmv_scene* scene);
double lmv_scene_start_time(const lmv_scene* scene);
double lmv_scene_end_time(const lmv_scene* scene);
double lmv_scene_frames_per_second(const lmv_scene* scene);
int lmv_scene_has_animation(const lmv_scene* scene);

/* Row-major 3x3 mapping working-space linear RGB to display-linear Rec.709.
 * Identity for Rec.709 scenes. Apply to material/light constants; the viewer's
 * environment sampling applies it on the GPU. */
void lmv_scene_working_to_display_linear(const lmv_scene* scene, float out[9]);

/* 0 = Y up, 1 = Z up (matches lightusd_render_scene_info::up_axis). */
int lmv_scene_up_axis(const lmv_scene* scene);
float lmv_scene_meters_per_unit(const lmv_scene* scene);

/* World-space bounds of the rendered geometry. Invalid scenes report a unit
 * cube around the origin. */
void lmv_scene_bounds(const lmv_scene* scene, float out_min[3],
                      float out_max[3]);

uint32_t lmv_scene_mesh_count(const lmv_scene* scene);
uint32_t lmv_scene_draw_count(const lmv_scene* scene);
uint32_t lmv_scene_material_count(const lmv_scene* scene);
uint32_t lmv_scene_texture_count(const lmv_scene* scene);
uint32_t lmv_scene_image_count(const lmv_scene* scene);
uint32_t lmv_scene_light_count(const lmv_scene* scene);
uint32_t lmv_scene_camera_count(const lmv_scene* scene);

/* Conversion / load diagnostics ("3 meshes skipped: ..."). */
uint32_t lmv_scene_warning_count(const lmv_scene* scene);
const char* lmv_scene_warning(const lmv_scene* scene, uint32_t index);

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

/* Interleaved preview vertex: position, normal, uv0, uv1, RGBA vertex color. */
typedef struct lmv_vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
  float u1, v1;
  float r, g, b, a;
} lmv_vertex;

/* One material range inside a mesh's triangle index buffer. */
typedef struct lmv_subset {
  uint32_t first_index;
  uint32_t index_count;
  int32_t material_id;
  uint32_t _pad;
} lmv_subset;

typedef struct lmv_ptex_rect {
  float x, y, w, h; /* normalized atlas rect */
} lmv_ptex_rect;

typedef struct lmv_mesh_view {
  const char* name;
  const char* prim_path;
  const lmv_vertex* vertices;
  /* Per-vertex authored face index (Ptex). Always present; zero when the mesh
   * has no faces or is not bound to a Ptex material. */
  const uint32_t* face_ids;
  uint32_t vertex_count;
  const uint32_t* indices; /* 3 per triangle */
  uint32_t index_count;
  const lmv_subset* subsets;
  uint32_t subset_count;
  int32_t material_id; /* mesh-level binding, -1 = unbound */
  uint32_t double_sided;
  /* UsdSkel: 4 influences per vertex when skinned. */
  uint32_t skinned;
  uint32_t influences;
  /* Number of blend-shape targets (position deltas) retained for playback. */
  uint32_t morph_targets;
  float bbox_min[3];
  float bbox_max[3];
} lmv_mesh_view;

lmv_status lmv_scene_mesh(const lmv_scene* scene, uint32_t index,
                          lmv_mesh_view* out);

/* Skinning buffers (4 influences per vertex; the same length as the mesh's
 * vertex_count). */
const uint16_t* lmv_scene_mesh_joint_indices(const lmv_scene* scene,
                                             uint32_t index, uint32_t* count);
const float* lmv_scene_mesh_joint_weights(const lmv_scene* scene,
                                          uint32_t index, uint32_t* count);
/* Per-joint 4x4 world-space skin palette for the current pose (16 floats per
 * joint, USD row-major). Skinned vertices are already in world space, so the
 * draw's world matrix must not be applied again. */
const float* lmv_scene_mesh_skin_matrices(const lmv_scene* scene, uint32_t index,
                                          uint32_t* matrix_count);
/* Blend-shape targets: 3 floats per vertex, per target. */
const float* lmv_scene_mesh_morph_deltas(const lmv_scene* scene, uint32_t index,
                                         uint32_t target, uint32_t* float_count);
/* Current blend-shape weights, one per target. */
const float* lmv_scene_mesh_morph_weights(const lmv_scene* scene, uint32_t index,
                                          uint32_t* count);

/* Re-evaluate node/light/camera transforms, light intensity/color/exposure,
 * camera focal/aperture/clipping/focus/exposure/f-stop, PointInstancer
 * instance transforms, `invisibleIds`, prototype indices and instance counts,
 * visibility, skeleton poses and blend-shape weights at `time` without
 * re-converting or re-uploading geometry. Returns UNSUPPORTED when the scene
 * has no animatable node/light/camera/instancer/visibility/rig/morph data. */
lmv_status lmv_scene_update_animation(lmv_scene* scene, double time, char* err,
                                      size_t err_cap);

/* One drawn instance: a mesh plus a world matrix. Instances sharing a mesh id
 * reuse the same vertex/index buffers. */
typedef struct lmv_draw_view {
  int32_t mesh_id;
  /* Per-draw material override (PointInstancer per-instance material), or -1
   * to use the mesh/subset material. */
  int32_t material_override;
  uint32_t visible;
  const float* world; /* 16 floats, column-major */
} lmv_draw_view;

lmv_status lmv_scene_draw(const lmv_scene* scene, uint32_t index,
                          lmv_draw_view* out);

/* ------------------------------------------------------------------------- */
/* Materials                                                                 */
/* ------------------------------------------------------------------------- */

/* Number of packed OpenPBR floats (matches tydra's
 * PackRealtimePbrMaterial / LIGHTUSD_GPU_OPENPBR_FLOATS). */
#define LMV_PBR_FLOATS 80

/* Indices into lmv_material::pbr. Keep in sync with
 * src/tydra/openpbr-params.hh (PackRealtimePbrMaterial). */
typedef enum lmv_pbr_index {
  LMV_PBR_BASE_COLOR = 0,      /* rgb */
  LMV_PBR_BASE_WEIGHT = 3,
  LMV_PBR_SPECULAR_COLOR = 4,  /* rgb */
  LMV_PBR_SPECULAR_WEIGHT = 7,
  LMV_PBR_TRANSMISSION_COLOR = 8, /* rgb */
  LMV_PBR_TRANSMISSION = 11,
  LMV_PBR_TRANSMISSION_DEPTH = 15,
  LMV_PBR_SUBSURFACE_COLOR = 16, /* rgb */
  LMV_PBR_SUBSURFACE = 19,
  LMV_PBR_COAT_COLOR = 24,     /* rgb */
  LMV_PBR_COAT_WEIGHT = 27,
  LMV_PBR_SHEEN_COLOR = 28,    /* rgb */
  LMV_PBR_SHEEN_WEIGHT = 31,
  LMV_PBR_EMISSION_COLOR = 32, /* rgb */
  LMV_PBR_EMISSION = 35,
  LMV_PBR_NORMAL = 36,         /* xyz */
  LMV_PBR_OPACITY = 39,
  LMV_PBR_METALNESS = 40,
  LMV_PBR_DIFFUSE_ROUGHNESS = 41,
  LMV_PBR_SPECULAR_ROUGHNESS = 42,
  LMV_PBR_SPECULAR_IOR = 43,
  LMV_PBR_COAT_ROUGHNESS = 44,
  LMV_PBR_COAT_IOR = 45,
  LMV_PBR_SHEEN_ROUGHNESS = 46,
  LMV_PBR_VALID = 51,
  LMV_PBR_HAS_TEXTURES = 52,
  LMV_PBR_HAS_NORMAL = 53,
  LMV_PBR_ALPHA_MODE = 54, /* 0 opaque, 1 mask, 2 blend */
  LMV_PBR_ALPHA_CUTOFF = 55,
  LMV_PBR_THIN_WALLED = 71
} lmv_pbr_index;

/* Material texture slots, in the order the Metal shader binds them. */
typedef enum lmv_material_tex_slot {
  LMV_TEX_BASE_COLOR = 0,
  LMV_TEX_METALLIC = 1,
  LMV_TEX_ROUGHNESS = 2,
  LMV_TEX_NORMAL = 3,
  LMV_TEX_EMISSION = 4,
  LMV_TEX_OPACITY = 5,
  LMV_TEX_COAT_NORMAL = 6,
  LMV_TEX_SLOT_COUNT = 7
} lmv_material_tex_slot;

typedef struct lmv_material_view {
  const char* name;
  const char* prim_path;
  uint32_t shader_type; /* 0 none, 1 preview surface, 2 OpenPBR/MaterialX */
  uint32_t alpha_mode;  /* 0 opaque, 1 mask, 2 blend */
  uint32_t double_sided;
  uint32_t default_fallback;
  float alpha_cutoff;
  /* Borrowed LMV_PBR_FLOATS array; see lmv_pbr_index. */
  const float* pbr;
  /* Texture ids per slot, -1 = constant. */
  int32_t tex_base_color;
  int32_t tex_metallic;
  int32_t tex_roughness;
  int32_t tex_normal;
  int32_t tex_emission;
  int32_t tex_opacity;
  int32_t tex_coat_normal;
  float normal_scale;
  float coat_normal_scale;
} lmv_material_view;

lmv_status lmv_scene_material(const lmv_scene* scene, uint32_t index,
                              lmv_material_view* out);

/* ------------------------------------------------------------------------- */
/* Textures / images                                                         */
/* ------------------------------------------------------------------------- */

typedef enum lmv_wrap_mode {
  LMV_WRAP_REPEAT = 0,
  LMV_WRAP_CLAMP = 1,
  LMV_WRAP_MIRROR = 2,
  LMV_WRAP_BLACK = 3
} lmv_wrap_mode;

/* One resolved UDIM tile: `udim` is 1001..1100, `image_id` indexes
 * lmv_scene_image(). */
typedef struct lmv_udim_tile {
  uint32_t udim;
  int32_t image_id;
} lmv_udim_tile;

typedef struct lmv_texture_view {
  const char* asset_path;
  /* GPU-compressed companion authored through `customData = { asset ktx2 = ... }`;
   * empty when the texture has no KTX2 hint. */
  const char* ktx2_hint;
  int32_t image_id; /* -1 = no decoded image */
  uint32_t wrap_s;  /* lmv_wrap_mode */
  uint32_t wrap_t;
  /* Which channel a single-channel map carries (0=R 1=G 2=B 3=A, 4=RGB,
   * 5=RGBA). */
  uint32_t channel;
  float uv_offset[2];
  float uv_scale[2];
  float uv_rotation; /* radians */
  /* UsdUVTexture post-sample value scale/bias. */
  float bias[4];
  float scale[4];
  /* UDIM: number of resolved tiles and their (udim, image) table. Empty for a
   * regular 2D texture. The second UV set of a UDIM texture is the 10x10 tile
   * grid (u in [0,10)). */
  /* Which mesh UV set this texture samples: 0 = texcoords_0, 1 = texcoords_1. */
  uint32_t uv_set;
  uint32_t udim_tile_count;
  const lmv_udim_tile* udim_tiles;
  /* Ptex: non-zero when `image_id` is a face atlas built from the .ptx file.
   * `ptex_rects` has one normalized rect per Ptex face (zero-size = undecoded),
   * indexed by the mesh's per-vertex face id. */
  uint32_t is_ptex;
  uint32_t ptex_face_count;
  const lmv_ptex_rect* ptex_rects;
} lmv_texture_view;

lmv_status lmv_scene_texture(const lmv_scene* scene, uint32_t index,
                             lmv_texture_view* out);

/* One mip level of a block-compressed image (KTX2). */
typedef struct lmv_image_level {
  const void* data;
  size_t nbytes;
  uint32_t width;
  uint32_t height;
} lmv_image_level;

typedef enum lmv_compressed_format {
  LMV_COMPRESSED_NONE = 0,
  LMV_COMPRESSED_BC1 = 1,
  LMV_COMPRESSED_BC3 = 2,
  LMV_COMPRESSED_BC5 = 3,
  LMV_COMPRESSED_BC6H = 4,
  LMV_COMPRESSED_BC7 = 5
} lmv_compressed_format;

typedef struct lmv_image_view {
  const char* resolved_path;
  uint32_t width;
  uint32_t height;
  /* 4 for uint8 RGBA, 3 for float RGB (HDR environment maps). */
  uint32_t channels;
  uint32_t is_float;
  /* 1 when the pixels are sRGB-encoded and should be decoded while sampling. */
  uint32_t srgb;
  const void* pixels;
  size_t nbytes;
  /* Block-compressed KTX2 images: format (lmv_compressed_format), mip count and
   * per-level blocks. When format != NONE, `pixels` is null. */
  uint32_t compressed_format;
  uint32_t mip_levels;
  const lmv_image_level* levels;
} lmv_image_view;

lmv_status lmv_scene_image(const lmv_scene* scene, uint32_t index,
                           lmv_image_view* out);

/* ------------------------------------------------------------------------- */
/* Point splats (UsdGeomPoints / particles) and curve line segments           */
/* ------------------------------------------------------------------------- */

/* World-space camera-facing splat. `radius` is a world-space radius (the
 * viewer converts it to pixels). */
typedef struct lmv_splat {
  float x, y, z;
  float radius;
  float r, g, b, a;
} lmv_splat;

typedef struct lmv_line_segment {
  float p0[3];
  float p1[3];
  float width; /* world-space ribbon width */
  float r, g, b, a;
} lmv_line_segment;

uint32_t lmv_scene_splat_count(const lmv_scene* scene);
/* Borrowed array; valid until the next lmv_scene_set_time/free. */
const lmv_splat* lmv_scene_splats(const lmv_scene* scene);
uint32_t lmv_scene_line_count(const lmv_scene* scene);
const lmv_line_segment* lmv_scene_lines(const lmv_scene* scene);

/* ------------------------------------------------------------------------- */
/* Lights / cameras                                                          */
/* ------------------------------------------------------------------------- */

typedef enum lmv_light_type {
  LMV_LIGHT_POINT = 0,
  LMV_LIGHT_DIRECTIONAL = 1,
  LMV_LIGHT_SPOT = 2,
  LMV_LIGHT_RECT = 3,
  LMV_LIGHT_DISK = 4,
  LMV_LIGHT_DOME = 5,
  LMV_LIGHT_SPHERE = 6,
  LMV_LIGHT_OTHER = 7
} lmv_light_type;

typedef struct lmv_light_view {
  const char* name;
  const char* prim_path;
  uint32_t type; /* lmv_light_type */
  float color[3];
  float intensity;
  float exposure;
  /* 16 floats, column-major; light-space -Z is the emitted direction. */
  const float* transform;
  /* sphere/disk radius, rect width/height, spot cone angle (radians). */
  float param0;
  float param1;
  /* Dome lights: texture id of the environment map, -1 = none. */
  int32_t dome_texture_id;
} lmv_light_view;

lmv_status lmv_scene_light(const lmv_scene* scene, uint32_t index,
                           lmv_light_view* out);

typedef struct lmv_camera_view {
  const char* name;
  const char* prim_path;
  uint32_t orthographic;
  float focal_length;
  float horizontal_aperture;
  float vertical_aperture;
  float ortho_width;
  float near_clip;
  float far_clip;
  float focus_distance;
  float exposure; /* authored stops */
  float fstop;    /* 0 = no depth of field */
  /* 16 floats, column-major; -Z is the view direction. */
  const float* transform;
} lmv_camera_view;

lmv_status lmv_scene_camera(const lmv_scene* scene, uint32_t index,
                            lmv_camera_view* out);

/* ------------------------------------------------------------------------- */
/* Camera controller (shared with lusdview: examples/common/light3d)         */
/* ------------------------------------------------------------------------- */

typedef struct lmv_camera lmv_camera;

lmv_camera* lmv_camera_create(void);
void lmv_camera_destroy(lmv_camera* camera);

void lmv_camera_set_aspect(lmv_camera* camera, float aspect);
void lmv_camera_set_up_axis(lmv_camera* camera, int up_axis); /* 0=Y 1=Z */
void lmv_camera_set_fov_y(lmv_camera* camera, float degrees);
void lmv_camera_set_exposure(lmv_camera* camera, float stops);
void lmv_camera_set_orthographic(lmv_camera* camera, int on);
void lmv_camera_set_orthographic_height(lmv_camera* camera, float height);
void lmv_camera_set_preset(lmv_camera* camera, int preset);

/* Frame the scene bounds and record them for automatic near/far clipping. */
void lmv_camera_frame_bounds(lmv_camera* camera, const float bbox_min[3],
                             const float bbox_max[3]);
/* Orbit/pan/dolly deltas are in pixels / wheel notches, matching lusdview. */
void lmv_camera_orbit(lmv_camera* camera, float dx, float dy);
void lmv_camera_pan(lmv_camera* camera, float dx, float dy);
void lmv_camera_dolly(lmv_camera* camera, float amount);
void lmv_camera_set_orbit(lmv_camera* camera, const float target[3], float yaw,
                          float pitch, float distance);

/* Read the current orbit placement (target, yaw, pitch in radians, distance). */
void lmv_camera_get_orbit(const lmv_camera* camera, float out_target[3],
                          float* yaw, float* pitch, float* distance);

/* Position the controller from an authored USD camera. */
void lmv_camera_set_from_camera(lmv_camera* camera,
                                const lmv_camera_view* authored);

typedef struct lmv_camera_frame {
  float view[16];  /* column-major */
  float proj[16];  /* column-major, 0..1 depth (Metal) */
  float eye[3];
  float target[3];
  float near_clip;
  float far_clip;
  float exposure;
  float fov_y; /* radians */
  int32_t orthographic;
  int32_t _pad;
} lmv_camera_frame;

void lmv_camera_get_frame(const lmv_camera* camera, lmv_camera_frame* out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LMV_H_ */
