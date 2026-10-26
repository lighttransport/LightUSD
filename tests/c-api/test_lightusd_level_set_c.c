/* SPDX-License-Identifier: Apache-2.0 */
/* Level-set handle C API: a handle answers exactly as the byte-based
 * lightusd_level_set_vdb_* calls do, outlives the bytes it was read from,
 * and rejects bad arguments. Self-contained (builds its own cube). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lightusd-c.h"
#include "lightusd-import-c.h"

static const float kCube[] = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                              -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1};
static const int32_t kCubeTris[] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                    1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
static const char kSettings[] = "{\"voxel_size\": 0.1, \"threads\": 1}";
static const char kTetSettings[] = "{\"spacing\": 0.4, \"threads\": 1}";

static int same(lightusd_string* a, lightusd_string* b) {
  const lightusd_sv x = lightusd_string_view(a), y = lightusd_string_view(b);
  return x.len == y.len && x.len > 0 && memcmp(x.data, y.data, x.len) == 0;
}

int main(void) {
  /* Handle from the mesh; its VDB bytes equal the one-shot call's. */
  lightusd_level_set* from_mesh = NULL;
  assert(lightusd_level_set_from_mesh(kCube, 8, kCubeTris, 36, NULL, 0, kSettings, &from_mesh) ==
         LIGHTUSD_OK);
  lightusd_string *vdb = NULL, *vdb_once = NULL;
  assert(lightusd_level_set_write_vdb(from_mesh, &vdb) == LIGHTUSD_OK);
  assert(lightusd_mesh_to_level_set_vdb(kCube, 8, kCubeTris, 36, NULL, 0, kSettings, &vdb_once) ==
         LIGHTUSD_OK);
  assert(same(vdb, vdb_once));
  lightusd_string_destroy(vdb_once);

  /* Handle from a private copy of the bytes, which is then wiped and freed. */
  const lightusd_sv view = lightusd_string_view(vdb);
  uint8_t* copy = (uint8_t*)malloc(view.len);
  assert(copy);
  memcpy(copy, view.data, view.len);
  lightusd_level_set* from_vdb = NULL;
  assert(lightusd_level_set_from_vdb(copy, view.len, &from_vdb) == LIGHTUSD_OK);
  memset(copy, 0, view.len);
  free(copy);

  const uint8_t* bytes = (const uint8_t*)view.data;
  lightusd_level_set_info a, b, c;
  assert(lightusd_level_set_get_info(from_mesh, &a) == LIGHTUSD_OK);
  assert(lightusd_level_set_get_info(from_vdb, &b) == LIGHTUSD_OK);
  assert(lightusd_level_set_vdb_info(bytes, view.len, &c) == LIGHTUSD_OK);
  assert(a.voxel_size > 0.099 && a.voxel_size < 0.101 && a.background > 0 && a.active_voxels > 0);
  assert(!memcmp(&a, &b, sizeof(a)) && !memcmp(&a, &c, sizeof(a)));

  /* Samples: centre inside, a face point near zero, far away the background. */
  const double points[] = {0, 0, 0, 1, 0, 0, 0, 0, 5};
  float s1[3], s2[3], s3[3];
  assert(lightusd_level_set_sample(from_mesh, points, 3, s1) == LIGHTUSD_OK);
  assert(lightusd_level_set_sample(from_vdb, points, 3, s2) == LIGHTUSD_OK);
  assert(lightusd_level_set_vdb_sample(bytes, view.len, points, 3, s3) == LIGHTUSD_OK);
  assert(!memcmp(s1, s2, sizeof(s1)) && !memcmp(s1, s3, sizeof(s1)));
  assert(s1[0] < 0 && s1[1] > -0.11f && s1[1] < 0.11f && s1[2] == a.background);

  /* Surface and tetrahedra: identical to the byte-based calls. */
  lightusd_string *p1 = NULL, *i1 = NULL, *p2 = NULL, *i2 = NULL;
  assert(lightusd_level_set_to_mesh(from_vdb, 0, &p1, &i1) == LIGHTUSD_OK);
  assert(lightusd_level_set_vdb_to_mesh(bytes, view.len, 0, &p2, &i2) == LIGHTUSD_OK);
  assert(same(p1, p2) && same(i1, i2));
  lightusd_string_destroy(p1);
  lightusd_string_destroy(i1);
  lightusd_string_destroy(p2);
  lightusd_string_destroy(i2);
  lightusd_string *tp1 = NULL, *tt1 = NULL, *ts1 = NULL, *tp2 = NULL, *tt2 = NULL, *ts2 = NULL;
  assert(lightusd_level_set_to_tet_mesh(from_vdb, kTetSettings, &tp1, &tt1, &ts1) == LIGHTUSD_OK);
  assert(lightusd_level_set_vdb_to_tet_mesh(bytes, view.len, kTetSettings, &tp2, &tt2, &ts2) ==
         LIGHTUSD_OK);
  assert(same(tp1, tp2) && same(tt1, tt2) && same(ts1, ts2));
  lightusd_string_destroy(tp1);
  lightusd_string_destroy(tt1);
  lightusd_string_destroy(ts1);
  lightusd_string_destroy(tp2);
  lightusd_string_destroy(tt2);
  lightusd_string_destroy(ts2);

  /* Errors leave outputs NULL. */
  lightusd_level_set* bad = (lightusd_level_set*)&a;
  const uint8_t junk[] = "not a vdb";
  assert(lightusd_level_set_from_vdb(junk, sizeof(junk), &bad) == LIGHTUSD_ERR_PARSE && !bad);
  bad = (lightusd_level_set*)&a;
  assert(lightusd_level_set_from_mesh(kCube, 8, kCubeTris, 36, NULL, 0, "{\"voxel_size\": -1}",
                                      &bad) == LIGHTUSD_ERR_INVALID_ARG &&
         !bad);
  assert(lightusd_level_set_from_mesh(kCube, 8, kCubeTris, 36, NULL, 0, kSettings, NULL) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_level_set_get_info(NULL, &a) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_level_set_sample(NULL, points, 1, s1) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_string *tp = (lightusd_string*)&a, *tt = (lightusd_string*)&a, *ts = (lightusd_string*)&a;
  assert(lightusd_level_set_to_tet_mesh(from_vdb, "{\"spacing\": 0}", &tp, &tt, &ts) ==
             LIGHTUSD_ERR_INVALID_ARG &&
         !tp && !tt && !ts);
  lightusd_string* w = (lightusd_string*)&a;
  assert(lightusd_level_set_write_vdb(NULL, &w) == LIGHTUSD_ERR_INVALID_ARG);

  lightusd_string_destroy(vdb);
  lightusd_level_set_destroy(from_mesh);
  lightusd_level_set_destroy(from_vdb);
  lightusd_level_set_destroy(NULL);
  printf("level set handle C API: OK\n");
  return 0;
}
