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
    const uint32_t* face_indices, uint32_t index_count,
    const float* uv_values, uint32_t uv_value_count,
    const uint32_t* uv_indices, uint32_t uv_index_count,
    const lu_geometry_subdiv_options* options, uint32_t callback_id);
int32_t lu_geometry_subdiv_error(uint32_t handle, uint8_t* out,
                                  uint32_t cap);

#ifdef __cplusplus
}
#endif
