/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_WEB_LIGHTRT_WASM_API_H_
#define LIGHTUSD_WEB_LIGHTRT_WASM_API_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stateless next-WASM mesh simplification. Counts are element counts; output
 * has room for at least idx_count uint32 values. Optional normals, UVs, and
 * locks may be null only when their count is zero. Returns the output index
 * count, or -1 for invalid input. */
int32_t lightusd_meshopt_simplify(
    const float* pos, uint32_t pos_count, const uint32_t* idx,
    uint32_t idx_count, const float* nrm, uint32_t nrm_count,
    const float* tex, uint32_t tex_count, const uint8_t* lock,
    uint32_t lock_count, uint32_t target_index_count, float target_error,
    uint32_t options, uint32_t* out, uint32_t out_cap, float* result_error);

/* Generation-checked next-WASM path tracer. All lengths are element counts,
 * except string and scene-buffer copy capacities, which are bytes. Negative
 * results mean invalid handles/inputs; build/trace/query return 1 on success
 * and 0 on a valid-handle failure. */
uint32_t lightusd_lrt_create(void);
void lightusd_lrt_destroy(uint32_t handle);
int32_t lightusd_lrt_clear(uint32_t handle);
int32_t lightusd_lrt_build(
    uint32_t handle, const float* positions, uint32_t position_count,
    const float* normals, uint32_t normal_count,
    const float* colors, uint32_t color_count,
    const float* vertex_params, uint32_t param_count,
    const int32_t* material_ids, uint32_t id_count,
    const float* materials, uint32_t material_count);
int32_t lightusd_lrt_trace(
    uint32_t handle, const float* inverse_view_projection,
    uint32_t matrix_count, const float* camera_position,
    uint32_t camera_count, int32_t width, int32_t height,
    int32_t sample_start, int32_t sample_count, int32_t max_bounces,
    float exposure, float* pixels, uint32_t pixel_cap);
int32_t lightusd_lrt_occluded(
    uint32_t handle, const float* origins, uint32_t origin_count,
    const float* directions, uint32_t direction_count, float max_distance,
    uint8_t* out, uint32_t out_cap);
int32_t lightusd_lrt_raycast(
    uint32_t handle, const float* origins, uint32_t origin_count,
    const float* directions, uint32_t direction_count, float max_distance,
    float* distance, int32_t* triangle, float* barycentrics,
    uint32_t result_cap);
int32_t lightusd_lrt_error(uint32_t handle, uint8_t* out, uint32_t cap);
int32_t lightusd_lrt_triangle_count(uint32_t handle);

/* Scene buffer kinds: 0=BVH nodes (u32), 1=triangle blocks (u32),
 * 2=normals (f32), 3=colors (f32), 4=vertex parameters (f32),
 * 5=material IDs (i32), 6=materials (f32). lengths are element counts. */
typedef struct lightusd_lrt_scene_info {
  uint32_t struct_size;
  uint32_t root;
  uint32_t node_count;
  uint32_t block_count;
  uint32_t width;
  uint32_t lengths[7];
} lightusd_lrt_scene_info;
int32_t lightusd_lrt_scene_info_get(uint32_t handle,
                                   lightusd_lrt_scene_info* out);
int32_t lightusd_lrt_scene_buffer(uint32_t handle, uint8_t kind,
                                 uint8_t* out, uint32_t cap);

#ifdef __cplusplus
}
#endif
#endif
