/* SPDX-License-Identifier: Apache-2.0 */
/* UsdVol render C API: volume and field info, dense voxels, NanoVDB export and
 * world-space sampling.
 *
 * Needs the tinyvdbio reference corpus (ref_float.vdb / ref_vec3s.vdb /
 * ref_bool.vdb): set TINYVDBIO_DATA_DIR to the tinyvdbio data directory (the
 * files are expected under <dir>/reference/), or pass <dir> as argv[1]. Without
 * it the test prints SKIP and exits 0, like the C++ usdvol corpus test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lightusd-c.h"
#include "lightusd-render-c.h"

#define MAX_USDA 2048

/* Field records follow the converter's relationship order, not the authored
 * one: find a binding instead of assuming an index. */
static int32_t find_field(lightusd_render_scene* scene, int32_t volume,
                          const char* binding) {
  const size_t len = strlen(binding);
  lightusd_render_volume_info info;
  memset(&info, 0, sizeof(info));
  assert(lightusd_render_volume_get_info(scene, volume, &info) == LIGHTUSD_OK);
  for (uint32_t i = 0; i < info.field_count; ++i) {
    lightusd_render_volume_field_info field;
    memset(&field, 0, sizeof(field));
    assert(lightusd_render_volume_field_get_info(scene, volume, (int32_t)i,
                                                 &field) == LIGHTUSD_OK);
    if (field.binding.len == len &&
        memcmp(field.binding.data, binding, len) == 0)
      return (int32_t)i;
  }
  assert(0 && "field binding not found");
  return -1;
}

static void check_volume(lightusd_render_scene* scene, int32_t volume,
                         uint32_t expect_fields) {
  lightusd_render_volume_info info;
  memset(&info, 0, sizeof(info));
  assert(lightusd_render_volume_get_info(scene, volume, &info) == LIGHTUSD_OK);
  assert(info.field_count == expect_fields);
  assert(info.prim_path.len == 2 && memcmp(info.prim_path.data, "/V", 2) == 0);
}

int main(int argc, char** argv) {
  const char* root = argc > 1 ? argv[1] : getenv("TINYVDBIO_DATA_DIR");
  if (!root || !root[0]) {
    printf("SKIP: set TINYVDBIO_DATA_DIR (tinyvdbio data directory) or pass it "
           "as argv[1]\n");
    return 0;
  }

  char usda[MAX_USDA];
  const int n = snprintf(
      usda, sizeof(usda),
      "#usda 1.0\n"
      "def Volume \"V\"\n"
      "{\n"
      "    rel field:density = </V/density>\n"
      "    rel field:vec = </V/vec>\n"
      "    rel field:broken = </V/broken>\n"
      "    def OpenVDBAsset \"density\"\n"
      "    {\n"
      "        asset filePath = @%s/reference/ref_float.vdb@\n"
      "        token fieldName = \"float_test\"\n"
      "        token fieldDataType = \"float\"\n"
      "    }\n"
      "    def OpenVDBAsset \"vec\"\n"
      "    {\n"
      "        asset filePath = @%s/reference/ref_vec3s.vdb@\n"
      "        token fieldName = \"vec3s_test\"\n"
      "        token fieldDataType = \"float3\"\n"
      "        token vectorDataRoleHint = \"Vector\"\n"
      "    }\n"
      "    def OpenVDBAsset \"broken\"\n"
      "    {\n"
      "        asset filePath = @%s/reference/ref_bool.vdb@\n"
      "        token fieldName = \"bool_test\"\n"
      "    }\n"
      "}\n",
      root, root, root);
  assert(n > 0 && (size_t)n < sizeof(usda));

  lightusd_stage* stage = NULL;
  assert(lightusd_stage_load_from_memory((const uint8_t*)usda, (size_t)n, NULL,
                                         &stage) == LIGHTUSD_OK);
  assert(stage != NULL);

  lightusd_render_config config;
  lightusd_render_config_init(&config);
  lightusd_render_scene* scene = NULL;
  assert(lightusd_render_convert(stage, &config, &scene) == LIGHTUSD_OK);
  assert(scene != NULL);

  assert(lightusd_render_volume_count(scene) == 1);
  check_volume(scene, 0, 3);

  /* Scalar field: float, one component, float NanoVDB grid type, samples. */
  const int32_t density = find_field(scene, 0, "density");
  {
    lightusd_render_volume_field_info field;
    memset(&field, 0, sizeof(field));
    assert(lightusd_render_volume_field_get_info(scene, 0, density, &field) ==
           LIGHTUSD_OK);
    assert(field.binding.len == 7 &&
           memcmp(field.binding.data, "density", 7) == 0);
    assert(field.grid_name.len == 10 &&
           memcmp(field.grid_name.data, "float_test", 10) == 0);
    assert(field.value_type.len == 5 &&
           memcmp(field.value_type.data, "float", 5) == 0);
    assert(field.components == 1);
    assert(field.has_bounds);

    lightusd_render_volume_dense dense;
    memset(&dense, 0, sizeof(dense));
    assert(lightusd_render_volume_field_dense(scene, 0, density, 0, 0, 0, &dense) ==
           LIGHTUSD_OK);
    assert(dense.data != NULL);
    assert(dense.dims[0] == 4 && dense.dims[1] == 4 && dense.dims[2] == 4);
    assert(dense.components == 1 && dense.stride == 1);

    lightusd_render_volume_nanovdb nano;
    memset(&nano, 0, sizeof(nano));
    assert(lightusd_render_volume_field_nanovdb(scene, 0, density, &nano) ==
           LIGHTUSD_OK);
    assert(nano.data != NULL && nano.size >= 736);
    assert(nano.grid_type == LIGHTUSD_NANOVDB_GRID_TYPE_FLOAT);
    assert(nano.components == 1);
    /* GridData magic ("NanoVDB0") and the grid type field (offset 636). */
    uint64_t magic = 0;
    memcpy(&magic, nano.data, sizeof(magic));
    assert(magic == 0x304244566f6e614eULL);
    uint32_t header_type = 0;
    memcpy(&header_type, nano.data + 636, sizeof(header_type));
    assert(header_type == LIGHTUSD_NANOVDB_GRID_TYPE_FLOAT);
    /* Cached: a second call returns the same buffer. */
    lightusd_render_volume_nanovdb again;
    memset(&again, 0, sizeof(again));
    assert(lightusd_render_volume_field_nanovdb(scene, 0, density, &again) ==
           LIGHTUSD_OK);
    assert(again.data == nano.data && again.size == nano.size);

    /* Sample the centre of voxel (0,0,0): nearest must be that voxel's stored
     * value and trilinear must stay within the grid's value range. */
    const double point[3] = {0.5, 0.5, 0.5};
    lightusd_render_volume_samples nearest;
    memset(&nearest, 0, sizeof(nearest));
    assert(lightusd_render_volume_field_sample(scene, 0, density, point, 1, 0,
                                               &nearest) == LIGHTUSD_OK);
    assert(nearest.count == 1 && nearest.components == 1);
    assert(nearest.values != NULL);
    lightusd_render_volume_samples trilinear;
    memset(&trilinear, 0, sizeof(trilinear));
    assert(lightusd_render_volume_field_sample(scene, 0, density, point, 1, 1,
                                               &trilinear) == LIGHTUSD_OK);
    float lo = dense.data[0], hi = dense.data[0];
    for (size_t i = 1; i < 64; ++i) {
      if (dense.data[i] < lo) lo = dense.data[i];
      if (dense.data[i] > hi) hi = dense.data[i];
    }
    assert(nearest.values[0] >= lo && nearest.values[0] <= hi);
    assert(trilinear.values[0] >= lo - 1e-5f && trilinear.values[0] <= hi + 1e-5f);
  }

  /* Vec3 field: three components, vec3f NanoVDB grid, three values per point. */
  const int32_t vec = find_field(scene, 0, "vec");
  {
    lightusd_render_volume_field_info field;
    memset(&field, 0, sizeof(field));
    assert(lightusd_render_volume_field_get_info(scene, 0, vec, &field) ==
           LIGHTUSD_OK);
    assert(field.binding.len == 3 && memcmp(field.binding.data, "vec", 3) == 0);
    assert(field.value_type.len == 5 &&
           memcmp(field.value_type.data, "vec3f", 5) == 0);
    assert(field.components == 3);

    lightusd_render_volume_dense dense;
    memset(&dense, 0, sizeof(dense));
    assert(lightusd_render_volume_field_dense(scene, 0, vec, 0, 0, 1, &dense) ==
           LIGHTUSD_OK);
    assert(dense.components == 3 && dense.data != NULL);

    lightusd_render_volume_nanovdb nano;
    memset(&nano, 0, sizeof(nano));
    assert(lightusd_render_volume_field_nanovdb(scene, 0, vec, &nano) ==
           LIGHTUSD_OK);
    assert(nano.grid_type == LIGHTUSD_NANOVDB_GRID_TYPE_VEC3F);
    assert(nano.components == 3);

    const double points[6] = {0.5, 0.5, 0.5, 1.5, 1.5, 1.5};
    lightusd_render_volume_samples samples;
    memset(&samples, 0, sizeof(samples));
    assert(lightusd_render_volume_field_sample(scene, 0, vec, points, 2, 1,
                                               &samples) == LIGHTUSD_OK);
    assert(samples.count == 2 && samples.components == 3);
    assert(samples.values != NULL);
  }

  /* Bool grids have no NanoVDB type: dense works, NanoVDB reports it. */
  const int32_t broken = find_field(scene, 0, "broken");
  {
    lightusd_render_volume_dense dense;
    memset(&dense, 0, sizeof(dense));
    assert(lightusd_render_volume_field_dense(scene, 0, broken, 0, 0, 0, &dense) ==
           LIGHTUSD_OK);
    lightusd_render_volume_nanovdb nano;
    memset(&nano, 0, sizeof(nano));
    assert(lightusd_render_volume_field_nanovdb(scene, 0, broken, &nano) ==
           LIGHTUSD_ERR_UNSUPPORTED);
  }

  /* Out-of-range ids are rejected. */
  {
    lightusd_render_volume_info info;
    assert(lightusd_render_volume_get_info(scene, 7, &info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_volume_field_info field;
    assert(lightusd_render_volume_field_get_info(scene, 0, 99, &field) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_volume_samples samples;
    assert(lightusd_render_volume_field_sample(scene, 0, 99, NULL, 0, 0,
                                               &samples) ==
           LIGHTUSD_ERR_NOT_FOUND);
  }

  lightusd_render_scene_destroy(scene);
  lightusd_stage_destroy(stage);
  printf("volume C API: OK\n");
  return 0;
}
