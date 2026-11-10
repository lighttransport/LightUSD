/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* SubdivStreamer inputs are typed, counted arrays. A nonzero callback_id
 * identifies a synchronous JS batch callback. Returns 0 on success, 1 on a
 * refinement error (query subdiv_error), and -1 for invalid arguments. */
typedef struct lu_geometry_subdiv_options {
  uint32_t struct_size;
  int32_t uv_interpolation;
  int32_t scheme;
  int32_t boundary;
  int32_t level;
  int32_t batch_faces;
  int32_t block_faces;
  int32_t halo_rings;
  uint32_t want_normals;
} lu_geometry_subdiv_options;
int32_t lu_geometry_subdiv_refine(
    uint32_t handle, const float* points, uint32_t point_values,
    const uint32_t* face_counts, uint32_t face_count,
    const uint32_t* face_indices, uint32_t index_count, const float* uv_values,
    uint32_t uv_value_count, const uint32_t* uv_indices,
    uint32_t uv_index_count, const lu_geometry_subdiv_options* options,
    uint32_t callback_id);
int32_t lu_geometry_subdiv_error(uint32_t handle, uint8_t* out, uint32_t cap);

/* xatlas input options (32-byte POD). flags: rotate-to-axis, rotate,
 * block-align, brute-force. The owned result blob starts with ten uint32 words:
 * byte count, width, height, atlas count, chart count, vertex count, index
 * count, source vertex count, reserved, reserved. It then contains float3
 * positions, float2 UVs, uint32 indices/xref, int32 chart/atlas IDs. Release
 * with lu_feature_free. A null result indicates invalid input or generation
 * failure. Only available when LIGHTUSD_WITH_XATLAS is enabled. */
typedef struct lu_geometry_xatlas_options {
  uint32_t iterations, resolution, padding, max_chart_size, flags;
  float max_cost, seam_weight, texels_per_unit;
} lu_geometry_xatlas_options;
uint8_t* lu_geometry_xatlas(const float* positions, uint32_t position_values,
                            const uint32_t* indices, uint32_t index_count,
                            const lu_geometry_xatlas_options* options);

#ifdef __cplusplus
}
#endif
