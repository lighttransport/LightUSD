#include "lightusd-validation-c.h"
/* SPDX-License-Identifier: Apache-2.0
 * Smoke test for the lightusd_c C API (pure C11).
 * Authors a stage, round-trips it through USDA/USDC, and reads it back.
 */

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lightusd-c.h"

#define CHECK_OK(expr)                                                    \
  do {                                                                    \
    lightusd_status st_ = (expr);                                             \
    if (st_ != LIGHTUSD_OK) {                                                 \
      fprintf(stderr, "FAIL %s:%d: %s -> %d (%s)\n", __FILE__, __LINE__,  \
              #expr, (int)st_, lightusd_last_error());                        \
      return 1;                                                           \
    }                                                                     \
  } while (0)

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      return 1;                                                           \
    }                                                                     \
  } while (0)

static int test_empty_and_vector_arrays(void) {
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));
  lightusd_prim prim;
  CHECK_OK(lightusd_stage_define_prim(stage, "/P", "Scope", 0, &prim));
  const lightusd_type types[] = {LIGHTUSD_TYPE_INT, LIGHTUSD_TYPE_FLOAT,
      LIGHTUSD_TYPE_DOUBLE, LIGHTUSD_TYPE_INT2, LIGHTUSD_TYPE_INT3,
      LIGHTUSD_TYPE_INT4, LIGHTUSD_TYPE_FLOAT4};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    CHECK_OK(lightusd_attr_set(stage, "/P", "empty", types[i], 1, NULL, 0, LIGHTUSD_PROP_CUSTOM));
    lightusd_value_view view;
    CHECK_OK(lightusd_attr_get(prim, "empty", &view));
    CHECK(view.is_array && view.count == 0 && view.type == types[i] && !view.is_block);
  }
  const int32_t values[] = {1, -2, 3, 4};
  CHECK_OK(lightusd_attr_set(stage, "/P", "vector", LIGHTUSD_TYPE_INT2, 1, values, 2, LIGHTUSD_PROP_CUSTOM));
  lightusd_value_view view;
  CHECK_OK(lightusd_attr_get(prim, "vector", &view));
  CHECK(view.count == 2 && view.components == 2 && view.nbytes == sizeof(values));
  CHECK(memcmp(view.data, values, sizeof(values)) == 0);
  lightusd_string* text = NULL;
  CHECK_OK(lightusd_stage_export_usda(stage, &text));
  CHECK(strstr(lightusd_string_view(text).data, "custom int2[] vector") != NULL);
  lightusd_string_destroy(text);
  lightusd_stage_destroy(stage);
  return 0;
}

static int test_authoring_roundtrip(void) {
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));

  /* stage metadata */
  CHECK_OK(lightusd_stage_set_metadata(stage, "upAxis", LIGHTUSD_TYPE_TOKEN, "Z", 1));
  double mpu = 1.0;
  CHECK_OK(lightusd_stage_set_metadata(stage, "metersPerUnit", LIGHTUSD_TYPE_DOUBLE,
                                   &mpu, 1));

  /* prims (ancestor auto-creation) */
  lightusd_prim mesh;
  CHECK_OK(lightusd_stage_define_prim(stage, "/World", "Xform", 0, NULL));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Geo/Grid", "Mesh", 0, &mesh));
  CHECK(lightusd_prim_is_valid(mesh));
  uint64_t gen = lightusd_stage_generation(stage);
  CHECK(gen >= 2);
  CHECK_OK(lightusd_stage_set_default_prim(stage, "World"));

  /* attributes: float3 array, int array, scalar double, token, timesamples */
  const float points[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  CHECK_OK(lightusd_attr_set(stage, "/World/Geo/Grid", "points",
                         LIGHTUSD_TYPE_POINT3F, 1, points, 4, 0));
  const int32_t counts[1] = {4};
  CHECK_OK(lightusd_attr_set(stage, "/World/Geo/Grid", "faceVertexCounts",
                         LIGHTUSD_TYPE_INT, 1, counts, 1, 0));
  const int32_t indices[4] = {0, 1, 2, 3};
  CHECK_OK(lightusd_attr_set(stage, "/World/Geo/Grid", "faceVertexIndices",
                         LIGHTUSD_TYPE_INT, 1, indices, 4, 0));
  double radius = 2.5;
  CHECK_OK(lightusd_attr_set(stage, "/World/Geo/Grid", "radius", LIGHTUSD_TYPE_DOUBLE,
                         0, &radius, 1, LIGHTUSD_PROP_CUSTOM));
  CHECK_OK(lightusd_attr_set(stage, "/World/Geo/Grid", "purpose", LIGHTUSD_TYPE_TOKEN,
                         0, "render", 1, LIGHTUSD_PROP_UNIFORM));
  const char* order[1] = {"xformOp:translate"};
  CHECK_OK(lightusd_attr_set_token_array(stage, "/World/Geo/Grid", "xformOpOrder",
                                     LIGHTUSD_TYPE_TOKEN, order, 1,
                                     LIGHTUSD_PROP_UNIFORM));
  const double t0[3] = {0, 0, 0};
  const double t1[3] = {0, 5, 0};
  CHECK_OK(lightusd_attr_set_timesample(stage, "/World/Geo/Grid",
                                    "xformOp:translate", 0.0,
                                    LIGHTUSD_TYPE_DOUBLE3, 0, t0, 1));
  CHECK_OK(lightusd_attr_set_timesample(stage, "/World/Geo/Grid",
                                    "xformOp:translate", 24.0,
                                    LIGHTUSD_TYPE_DOUBLE3, 0, t1, 1));

  /* attribute metadata */
  CHECK_OK(lightusd_attr_set_metadata(stage, "/World/Geo/Grid", "points",
                                  "interpolation", LIGHTUSD_TYPE_TOKEN, "vertex",
                                  1));

  /* relationship + arc + variant */
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Looks/Red", "Material", 0,
                                  NULL));
  CHECK_OK(lightusd_rel_add_target(stage, "/World/Geo/Grid", "material:binding",
                               "/World/Looks/Red"));
  CHECK_OK(lightusd_prim_add_arc(stage, "/World", LIGHTUSD_ARC_REFERENCE,
                             "./library.usda", "/Proto"));
  CHECK_OK(lightusd_prim_add_variant_set(stage, "/World", "lod"));
  CHECK_OK(lightusd_prim_add_variant(stage, "/World", "lod", "high"));
  CHECK_OK(lightusd_prim_add_variant(stage, "/World", "lod", "low"));
  CHECK_OK(lightusd_prim_set_variant_selection(stage, "/World", "lod", "high"));

  /* prim metadata */
  CHECK_OK(lightusd_prim_set_metadata(stage, "/World", "kind", LIGHTUSD_TYPE_TOKEN,
                                  "assembly", 1));
  int metadata_authored = -1;
  lightusd_prim world_meta = lightusd_stage_prim_at_path(stage, "/World");
  CHECK_OK(lightusd_prim_metadata_is_authored(world_meta, "kind", &metadata_authored));
  CHECK(metadata_authored == 1);
  CHECK_OK(lightusd_prim_metadata_is_authored(world_meta, "hidden", &metadata_authored));
  CHECK(metadata_authored == 0);
  const uint8_t explicit_false = 0;
  CHECK_OK(lightusd_prim_set_metadata(stage, "/World", "active",
      LIGHTUSD_TYPE_BOOL, &explicit_false, 1));
  CHECK_OK(lightusd_prim_metadata_is_authored(world_meta, "active", &metadata_authored));
  CHECK(metadata_authored == 1);
  lightusd_value* empty_doc = NULL;
  CHECK_OK(lightusd_prim_set_metadata(stage, "/World", "doc",
      LIGHTUSD_TYPE_STRING, "", 1));
  CHECK_OK(lightusd_prim_get_metadata(world_meta, "doc", &empty_doc));
  lightusd_sv empty_doc_text = {0};
  CHECK_OK(lightusd_value_get_string(empty_doc, &empty_doc_text));
  CHECK(empty_doc_text.len == 0);
  lightusd_value_destroy(empty_doc);
  CHECK(lightusd_prim_metadata_is_authored(world_meta, "unknown", &metadata_authored) ==
      LIGHTUSD_ERR_NOT_FOUND);

  /* -------- read back before export -------- */
  lightusd_prim grid = lightusd_stage_prim_at_path(stage, "/World/Geo/Grid");
  CHECK(lightusd_prim_is_valid(grid));
  uint64_t gridResource = lightusd_prim_root_layer_resource_id(grid);
  CHECK(gridResource != 0);
  CHECK(lightusd_prim_root_layer_resource_id(
            lightusd_stage_prim_at_path(stage, "/World/Geo/Grid")) ==
        gridResource);
  CHECK(lightusd_prim_root_layer_resource_id((lightusd_prim){0}) == 0);

  lightusd_value_view view;
  CHECK_OK(lightusd_attr_get(grid, "points", &view));
  CHECK(view.is_array && view.count == 4 && view.components == 3);
  CHECK(view.storage == LIGHTUSD_COMP_FLOAT32);
  CHECK(view.nbytes == 12 * sizeof(float));
  CHECK(memcmp(view.data, points, sizeof(points)) == 0);

  CHECK_OK(lightusd_attr_get(grid, "radius", &view));
  CHECK(!view.is_array && view.storage == LIGHTUSD_COMP_FLOAT64);
  CHECK(fabs(*(const double*)view.data - 2.5) < 1e-12);

  lightusd_sv sv;
  CHECK_OK(lightusd_attr_get_string(grid, "purpose", &sv));
  CHECK(strncmp(sv.data, "render", sv.len) == 0);

  lightusd_string* trace_json = NULL;
  CHECK_OK(lightusd_stage_explain_property(stage, "/World/Geo/Grid", "radius",
                                           NAN, &trace_json));
  lightusd_sv trace_view = lightusd_string_view(trace_json);
  CHECK(trace_view.len != 0 && strstr(trace_view.data, "\"property\""));
  lightusd_string_destroy(trace_json);

  /* flags */
  CHECK(lightusd_prim_property_flags(grid, "radius") & LIGHTUSD_PROP_CUSTOM);
  CHECK(lightusd_prim_property_flags(grid, "purpose") & LIGHTUSD_PROP_UNIFORM);

  /* time samples */
  CHECK(lightusd_attr_has_timesamples(grid, "xformOp:translate"));
  CHECK(lightusd_attr_timesample_count(grid, "xformOp:translate") == 2);
  double times[2];
  CHECK(lightusd_attr_timesample_times(grid, "xformOp:translate", times, 2) == 2);
  CHECK(times[0] == 0.0 && times[1] == 24.0);
  lightusd_value* interp = NULL;
  CHECK_OK(lightusd_attr_interpolate(grid, "xformOp:translate", 12.0, 1, &interp));
  CHECK_OK(lightusd_value_get_view(interp, &view));
  CHECK(view.storage == LIGHTUSD_COMP_FLOAT64 && view.components == 3);
  CHECK(fabs(((const double*)view.data)[1] - 2.5) < 1e-12);
  lightusd_value_destroy(interp);
  lightusd_value* evaluated = NULL;
  CHECK_OK(lightusd_attr_eval_ex(stage, grid, "xformOp:translate", 12.0, 1,
                                 1, &evaluated));
  CHECK_OK(lightusd_value_get_view(evaluated, &view));
  CHECK(view.storage == LIGHTUSD_COMP_FLOAT64 && view.components == 3 &&
        fabs(((const double*)view.data)[1] - 2.5) < 1e-12);
  lightusd_value_destroy(evaluated);
  CHECK_OK(lightusd_attr_eval_ex(stage, grid, "radius", NAN, 0, 1,
                                 &evaluated));
  CHECK_OK(lightusd_value_get_view(evaluated, &view));
  CHECK(view.storage == LIGHTUSD_COMP_FLOAT64 &&
        fabs(*(const double*)view.data - 2.5) < 1e-12);
  lightusd_string* formatted = NULL;
  CHECK_OK(lightusd_value_to_usda(evaluated, &formatted));
  sv = lightusd_string_view(formatted);
  CHECK(sv.len == 3 && memcmp(sv.data, "2.5", 3) == 0);
  lightusd_string_destroy(formatted);
  lightusd_value_destroy(evaluated);
  size_t joint_count = 0;
  CHECK(lightusd_skel_animation_joint_count_at_time(stage, grid, 0.5,
                                                   &joint_count) ==
        LIGHTUSD_ERR_TYPE_MISMATCH);
  CHECK(lightusd_attr_eval_ex(stage, grid, "radius", 0.0, 2, 1,
                              &evaluated) == LIGHTUSD_ERR_INVALID_ARG);

  /* relationships */
  CHECK(lightusd_prim_has_relationship(grid, "material:binding"));
  CHECK(lightusd_rel_target_count(grid, "material:binding") == 1);
  sv = lightusd_rel_target(grid, "material:binding", 0);
  CHECK(strncmp(sv.data, "/World/Looks/Red", sv.len) == 0);

  /* Reading a normal USDA array does not need a lazy decode. */
  int would_materialize = 1;
  CHECK_OK(lightusd_attr_would_materialize_array(
      grid, "points", &would_materialize));
  CHECK(would_materialize == 0);
  CHECK(lightusd_attr_would_materialize_array(
            grid, "radius", &would_materialize) ==
        LIGHTUSD_ERR_TYPE_MISMATCH);

  /* variants */
  lightusd_prim world = lightusd_stage_prim_at_path(stage, "/World");
  CHECK(lightusd_prim_variant_set_count(world) == 1);
  CHECK(lightusd_variant_count(world, "lod") == 2);
  sv = lightusd_variant_selection(world, "lod");
  CHECK(strncmp(sv.data, "high", sv.len) == 0);

  /* traversal */
  CHECK(lightusd_stage_root_prim_count(stage) == 1);
  lightusd_prim root = lightusd_stage_root_prim(stage, 0);
  CHECK(lightusd_prim_child_count(root) == 2); /* Geo, Looks */
  lightusd_prim geo = lightusd_prim_child_by_name(root, "Geo");
  CHECK(lightusd_prim_is_valid(geo));
  lightusd_prim parent = lightusd_prim_parent(geo);
  sv = lightusd_prim_path(parent);
  CHECK(strncmp(sv.data, "/World", sv.len) == 0);

  /* prim metadata read */
  sv = lightusd_prim_kind(world);
  CHECK(strncmp(sv.data, "assembly", sv.len) == 0);

  /* -------- USDA round-trip -------- */
  lightusd_string* usda = NULL;
  CHECK_OK(lightusd_stage_export_usda(stage, &usda));
  lightusd_sv usda_sv = lightusd_string_view(usda);
  CHECK(usda_sv.len > 0);
  CHECK(strstr(usda_sv.data, "Grid") != NULL);

  lightusd_stage* re = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)usda_sv.data,
                                       usda_sv.len, NULL, &re));
  lightusd_string_destroy(usda);

  lightusd_prim regrid = lightusd_stage_prim_at_path(re, "/World/Geo/Grid");
  CHECK(lightusd_prim_is_valid(regrid));
  CHECK_OK(lightusd_attr_get(regrid, "points", &view));
  CHECK(view.is_array && view.count == 4);
  CHECK(memcmp(view.data, points, sizeof(points)) == 0);
  CHECK(lightusd_attr_has_timesamples(regrid, "xformOp:translate"));
  sv = lightusd_variant_selection(lightusd_stage_prim_at_path(re, "/World"), "lod");
  CHECK(strncmp(sv.data, "high", sv.len) == 0);
  lightusd_stage_destroy(re);

  /* -------- USDC round-trip -------- */
  lightusd_string* usdc = NULL;
  CHECK_OK(lightusd_stage_export_usdc(stage, &usdc));
  lightusd_sv usdc_sv = lightusd_string_view(usdc);
  CHECK(usdc_sv.len > 8);
  CHECK(memcmp(usdc_sv.data, "PXR-USDC", 8) == 0);
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)usdc_sv.data,
                                       usdc_sv.len, NULL, &re));
  {
    /* Large trusted crates can raise the structural-table caps explicitly. */
    lightusd_load_options limited;
    lightusd_load_options_init(&limited);
    limited.max_crate_elements = 1;
    lightusd_stage* re_limited = NULL;
    CHECK(lightusd_stage_load_from_memory((const uint8_t*)usdc_sv.data,
                                          usdc_sv.len, &limited,
                                          &re_limited) != LIGHTUSD_OK);
    CHECK(strstr(lightusd_last_error(), "Too many") != NULL);
    if (re_limited) lightusd_stage_destroy(re_limited);

    /* A struct that predates the field must ignore it (struct_size guard). */
    lightusd_load_options legacy;
    lightusd_load_options_init(&legacy);
    legacy.struct_size =
        (uint32_t)offsetof(lightusd_load_options, max_crate_elements);
    legacy.max_crate_elements = 1;
    lightusd_stage* re_test = NULL;
    CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)usdc_sv.data,
                                             usdc_sv.len, &legacy, &re_test));
    lightusd_stage_destroy(re_test);
    re_test = NULL;

    lightusd_load_options generous;
    lightusd_load_options_init(&generous);
    generous.max_crate_elements = 100000000;
    CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)usdc_sv.data,
                                             usdc_sv.len, &generous,
                                             &re_test));
    lightusd_stage_destroy(re_test);
  }
  lightusd_string_destroy(usdc);
  regrid = lightusd_stage_prim_at_path(re, "/World/Geo/Grid");
  CHECK(lightusd_prim_is_valid(regrid));
  CHECK_OK(lightusd_attr_get(regrid, "points", &view));
  CHECK(view.count == 4 && memcmp(view.data, points, sizeof(points)) == 0);
  lightusd_stage_destroy(re);

  /* remove property / prim */
  CHECK_OK(lightusd_attr_remove(stage, "/World/Geo/Grid", "radius"));
  CHECK(!lightusd_prim_has_property(lightusd_stage_prim_at_path(stage,
                                                        "/World/Geo/Grid"),
                                "radius"));
  CHECK_OK(lightusd_stage_remove_prim(stage, "/World/Looks"));
  CHECK(!lightusd_prim_is_valid(
      lightusd_stage_prim_at_path(stage, "/World/Looks/Red")));
  CHECK(lightusd_stage_generation(stage) > gen);

  lightusd_stage_destroy(stage);
  return 0;
}

static int test_error_handling(void) {
  lightusd_stage* stage = NULL;
  lightusd_status st = lightusd_stage_load("/nonexistent/file.usda", NULL, &stage);
  CHECK(st != LIGHTUSD_OK && stage == NULL);
  CHECK(lightusd_last_error()[0] != '\0');

  const uint8_t garbage[16] = {0xff, 0xfe, 1, 2, 3, 4, 5, 6,
                               7,    8,    9, 10, 11, 12, 13, 14};
  st = lightusd_stage_load_from_memory(garbage, sizeof(garbage), NULL, &stage);
  CHECK(st == LIGHTUSD_ERR_PARSE && stage == NULL);

  {
    static const char usda[] = "#usda 1.0\ndef Xform \"A\" {}\n";
    lightusd_load_options limited;
    lightusd_load_options_init(&limited);
    limited.max_input_bytes = 1;
    st = lightusd_stage_load_from_memory(
        (const uint8_t*)usda, sizeof(usda) - 1, &limited, &stage);
    CHECK(st == LIGHTUSD_ERR_RESOURCE_LIMIT && stage == NULL);
  }

  CHECK_OK(lightusd_stage_create(&stage));
  lightusd_prim invalid = lightusd_stage_prim_at_path(stage, "/Nope");
  CHECK(!lightusd_prim_is_valid(invalid));
  lightusd_value_view view;
  CHECK(lightusd_attr_get(invalid, "x", &view) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_stage_remove_prim(stage, "/Nope") == LIGHTUSD_ERR_NOT_FOUND);
  lightusd_stage_destroy(stage);
  return 0;
}

static int test_name_validation(void) {
  /* Non-identifier names are rejected at creation (pxr and the parser both
   * reject them), so they can't be authored into an unround-trippable scene. */
  const double one = 1.0;
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World", "Xform", 0, NULL));
  CHECK(lightusd_attr_set(stage, "/World", "0abc", LIGHTUSD_TYPE_DOUBLE, 0, &one, 1, 0)
         == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_attr_set(stage, "/World", "bad name", LIGHTUSD_TYPE_DOUBLE, 0, &one, 1, 0)
         == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_stage_define_prim(stage, "/World/0abc", "Mesh", 0, NULL)
         == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_rel_add_target(stage, "/World", "0rel", "/World")
         == LIGHTUSD_ERR_INVALID_ARG);
  /* Valid (possibly namespaced) names still work. */
  CHECK_OK(lightusd_attr_set(stage, "/World", "good", LIGHTUSD_TYPE_DOUBLE, 0, &one, 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/World", "xformOp:translate", LIGHTUSD_TYPE_DOUBLE,
                         0, &one, 1, 0));
  lightusd_stage_destroy(stage);
  return 0;
}

static int test_file_load(const char* path) {
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  lightusd_stage* stage = NULL;
  lightusd_status st = lightusd_stage_load(path, &opts, &stage);
  if (st != LIGHTUSD_OK) {
    fprintf(stderr, "FAIL: load %s: %s\n", path, lightusd_last_error());
    return 1;
  }
  lightusd_stage_stats stats;
  CHECK_OK(lightusd_stage_get_stats(stage, &stats));
  CHECK(stats.prim_count > 0);
  printf("  loaded %s: %llu prims\n", path,
         (unsigned long long)stats.prim_count);
  lightusd_stage_destroy(stage);
  return 0;
}

static int test_usda_lazy_load_options(void) {
  const size_t count = 25000;
  const char* prefix = "#usda 1.0\n\ndef Mesh \"Mesh\" {\n    int[] points = [";
  const char* suffix = "]\n}\n";
  const size_t max_chars_per_digit = 8; /* "-2147483648" */
  size_t buf_cap = 64 + strlen(prefix) + strlen(suffix) + (count * max_chars_per_digit);
  char* data = (char*)malloc(buf_cap);
  if (!data) {
    fprintf(stderr, "FAIL: OOM building lazy parse fixture\n");
    return 1;
  }

  size_t len = strlen(prefix);
  memcpy(data, prefix, len);
  for (size_t i = 0; i < count; ++i) {
    const int written = snprintf(
        data + len,
        buf_cap - len,
        i == 0 ? "%zu" : ",%zu",
        i);
    if (written <= 0 || written >= (int)(buf_cap - len)) {
      free(data);
      fprintf(stderr, "FAIL: fixture build overflow\n");
      return 1;
    }
    len += (size_t)written;
  }
  memcpy(data + len, suffix, strlen(suffix) + 1);
  len += strlen(suffix);

  printf("  built USDA fixture: %zu values (%zu bytes)\n", count, len);

  /* Force eager parsing by shrinking the lazy cap to 1 element. */
  lightusd_load_options eager;
  lightusd_load_options_init(&eager);
  eager.format = LIGHTUSD_FORMAT_USDA;
  eager.enable_usda_lazy_arrays = 1;
  eager.max_array_elements = 1;
  eager.max_threads = 1;
  lightusd_stage* eager_stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)data, len, &eager, &eager_stage));
  lightusd_stage_stats eager_before = {0}, eager_after = {0};
  CHECK_OK(lightusd_stage_get_stats(eager_stage, &eager_before));
  {
    lightusd_prim mesh = lightusd_stage_prim_at_path(eager_stage, "/Mesh");
    CHECK(lightusd_prim_is_valid(mesh));
    lightusd_value_view view = {0};
    CHECK_OK(lightusd_attr_get(mesh, "points", &view));
    CHECK(view.is_array && view.count == count);
  }
  CHECK_OK(lightusd_stage_get_stats(eager_stage, &eager_after));
  lightusd_stage_destroy(eager_stage);

  /* Enable lazy path with a high per-array cap; should keep the array lazy until
   * first materialization. */
  lightusd_load_options lazy;
  lightusd_load_options_init(&lazy);
  lazy.format = LIGHTUSD_FORMAT_USDA;
  lazy.enable_usda_lazy_arrays = 1;
  lazy.max_array_elements = (1ull << 60);
  lightusd_stage* lazy_stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)data, len, &lazy, &lazy_stage));
  lightusd_stage_stats lazy_before = {0}, lazy_after = {0};
  CHECK_OK(lightusd_stage_get_stats(lazy_stage, &lazy_before));
  {
    lightusd_prim mesh = lightusd_stage_prim_at_path(lazy_stage, "/Mesh");
    CHECK(lightusd_prim_is_valid(mesh));
    lightusd_value_view view = {0};
    lightusd_value* copy = NULL;
    CHECK_OK(lightusd_attr_copy_default(mesh, "points", &copy));
    CHECK_OK(lightusd_value_get_view(copy, &view));
    CHECK(view.is_array && view.count == count && view.components == 1);
    CHECK(view.storage == LIGHTUSD_COMP_INT32 && view.nbytes == count * sizeof(int32_t));
    CHECK(((const int32_t*)view.data)[count - 1] == (int32_t)(count - 1));
    lightusd_stage_stats after_copy = {0};
    CHECK_OK(lightusd_stage_get_stats(lazy_stage, &after_copy));
    CHECK(after_copy.memory_bytes == lazy_before.memory_bytes);
    lightusd_value_destroy(copy);
    CHECK_OK(lightusd_attr_get(mesh, "points", &view));
    CHECK(view.is_array && view.count == count);
  }
  CHECK_OK(lightusd_stage_get_stats(lazy_stage, &lazy_after));
  lightusd_stage_destroy(lazy_stage);

  /* If lazy parsing is threaded to the path, materializing from memory should grow
   * peak memory after attribute read, while eager parsing allocates upfront. */
  CHECK(lazy_before.memory_bytes <= eager_before.memory_bytes);
  CHECK(lazy_after.memory_bytes >= lazy_before.memory_bytes);
  CHECK(eager_after.memory_bytes <= eager_before.memory_bytes + 1024);

  free(data);
  return 0;
}

static int test_core_transforms_and_ownership(void) {
  lightusd_stage* stage = NULL;
  lightusd_stage* other = NULL;
  lightusd_prim parent, child;
  lightusd_value_view view;
  double matrix[16];
  const char* order[] = {"xformOp:translate"};
  const char* reset_order[] = {"!resetXformStack!", "xformOp:translate"};
  const double parent_translation[] = {5, 0, 0};
  const double start[] = {2, 0, 0};
  const double end[] = {4, 0, 0};
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_create(&other));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World", "Xform", 0, &parent));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Child", "Xform", 0, &child));
  CHECK(!lightusd_prim_is_valid(parent));
  CHECK(lightusd_prim_name(parent).len == 0);
  CHECK(lightusd_attr_get(parent, "xformOp:translate", &view) == LIGHTUSD_ERR_INVALID_ARG);

  CHECK_OK(lightusd_attr_set_token_array(stage, "/World", "xformOpOrder",
      LIGHTUSD_TYPE_TOKEN, order, 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/World", "xformOp:translate",
      LIGHTUSD_TYPE_DOUBLE3, 0, parent_translation, 1, 0));
  CHECK_OK(lightusd_attr_set_token_array(stage, "/World/Child", "xformOpOrder",
      LIGHTUSD_TYPE_TOKEN, order, 1, 0));
  CHECK_OK(lightusd_attr_set_timesample(stage, "/World/Child", "xformOp:translate",
      0, LIGHTUSD_TYPE_DOUBLE3, 0, start, 1));
  CHECK_OK(lightusd_attr_set_timesample(stage, "/World/Child", "xformOp:translate",
      2, LIGHTUSD_TYPE_DOUBLE3, 0, end, 1));
  CHECK_OK(lightusd_prim_local_transform(child, 1, matrix));
  CHECK(fabs(matrix[12] - 3) < 1e-12);
  CHECK_OK(lightusd_prim_world_transform(stage, child, 1, matrix));
  CHECK(fabs(matrix[12] - 8) < 1e-12);
  CHECK(lightusd_prim_world_transform(other, child, 1, matrix) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK_OK(lightusd_attr_set_token_array(stage, "/World/Child", "xformOpOrder",
      LIGHTUSD_TYPE_TOKEN, reset_order, 2, 0));
  CHECK_OK(lightusd_prim_world_transform(stage, child, 1, matrix));
  CHECK(fabs(matrix[12] - 3) < 1e-12);

  lightusd_stage_retain(stage);
  lightusd_stage_destroy(stage);
  CHECK(lightusd_prim_is_valid(child));
  CHECK_OK(lightusd_stage_remove_prim(stage, "/World/Child"));
  CHECK(!lightusd_prim_is_valid(child));
  CHECK(lightusd_prim_child_count(child) == 0);
  CHECK(lightusd_prim_local_transform(child, 1, matrix) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_stage_destroy(stage);
  lightusd_stage_destroy(other);
  return 0;
}

typedef struct {
  int resolve_calls;
  int read_calls;
  int destroy_calls;
  char context[64];
} TestSchemeState;

static int test_scheme_resolver(void* userdata, const char* identifier,
                                const char* anchor_path,
                                const char* scheme_context,
                                const char** resolved) {
  TestSchemeState* state = (TestSchemeState*)userdata;
  (void)anchor_path;
  if (state) {
    ++state->resolve_calls;
    snprintf(state->context, sizeof(state->context), "%s", scheme_context);
  }
  *resolved = identifier;
  return 1;
}

static void test_scheme_destroy(void* userdata) {
  TestSchemeState* state = (TestSchemeState*)userdata;
  if (state) ++state->destroy_calls;
}

static int test_scheme_reader(void* userdata, const char* identifier,
                              uint8_t** data, size_t* size) {
  static const char payload[] = "#usda 1.0\n";
  TestSchemeState* state = (TestSchemeState*)userdata;
  (void)identifier;
  if (state) ++state->read_calls;
  *size = sizeof(payload) - 1;
  *data = (uint8_t*)malloc(*size);
  if (!*data) return 0;
  memcpy(*data, payload, *size);
  return 1;
}

static int test_owned_package_callback(void* userdata,
                                       const char* package_path,
                                       const char* asset_path,
                                       const char** resolved) {
  (void)userdata;
  (void)package_path;
  (void)asset_path;
  (void)resolved;
  return 0;
}

static void test_owned_package_destroy(void* userdata) {
  if (userdata) ++*(int*)userdata;
}

static int test_package_resolver_callback(void* userdata,
                                          const char* package_path,
                                          const char* asset_path,
                                          const char** resolved) {
  int* calls = (int*)userdata;
  static char buffer[512];
  if (calls) ++*calls;
  snprintf(buffer, sizeof(buffer), "virtual:%s|%s", package_path, asset_path);
  *resolved = buffer;
  return 1;
}

static int test_dependency_boundary(void) {
  static const char source[] =
      "#usda 1.0\n(defaultPrim = \"World\")\n"
      "def Xform \"World\" { asset info:readme = @readme.txt@ }\n";
  static const char missing_source[] =
      "#usda 1.0\n(defaultPrim = \"World\")\n"
      "def Xform \"World\" { asset info:readme = @missing.txt@ }\n";
  static const uint8_t note[] = {'a', 0, 'b'};
  lightusd_asset_resolver* resolver = NULL;
  CHECK_OK(lightusd_asset_resolver_create(&resolver));

  lightusd_resolver_context* context = NULL;
  void* context_binding = NULL;
  CHECK_OK(lightusd_asset_resolver_create_context(resolver, "/tmp", &context));
  lightusd_sv context_text = lightusd_resolver_context_string(context);
  CHECK(context_text.len == 4 && memcmp(context_text.data, "/tmp", 4) == 0);
  CHECK_OK(lightusd_resolver_context_set_scheme(context, "StUdIo:", "ctx"));
  context_text = lightusd_resolver_context_get_scheme(context, "studio");
  CHECK(context_text.len == 3 && memcmp(context_text.data, "ctx", 3) == 0);
  CHECK(lightusd_asset_resolver_is_context_dependent(resolver, "rel.usda") == 1);
  CHECK(lightusd_asset_resolver_is_context_dependent(resolver, "/abs.usda") == 0);
  CHECK_OK(lightusd_asset_resolver_bind_context(resolver, context,
                                                &context_binding));
  lightusd_resolver_context* current_context = NULL;
  CHECK_OK(lightusd_asset_resolver_get_context(resolver, &current_context));
  context_text = lightusd_resolver_context_string(current_context);
  CHECK(context_text.len == 4 && memcmp(context_text.data, "/tmp", 4) == 0);
  lightusd_resolver_context_destroy(current_context);
  CHECK_OK(lightusd_asset_resolver_unbind_context(resolver, context,
                                                  &context_binding));
  CHECK(lightusd_asset_resolver_unbind_context(resolver, context,
                                               &context_binding) ==
            LIGHTUSD_ERR_NOT_FOUND);
  lightusd_resolver_context_destroy(context);

  TestSchemeState scheme_state;
  memset(&scheme_state, 0, sizeof(scheme_state));
  CHECK_OK(lightusd_asset_resolver_register_scheme(
      resolver, "studio", test_scheme_resolver, test_scheme_reader,
      &scheme_state));
  lightusd_resolver_context* scheme_context = NULL;
  void* scheme_binding = NULL;
  CHECK_OK(lightusd_asset_resolver_create_context(resolver, "", &scheme_context));
  CHECK_OK(lightusd_resolver_context_set_scheme(scheme_context, "studio",
                                                 "scheme-ctx"));
  CHECK_OK(lightusd_asset_resolver_bind_context(resolver, scheme_context,
                                                 &scheme_binding));
  lightusd_string* resolved = NULL;
  uint8_t scheme_exists = 0, scheme_is_package = 0;
  CHECK_OK(lightusd_asset_resolver_resolve(
      resolver, "studio:note.usda", NULL, &resolved, &scheme_exists,
      &scheme_is_package));
  CHECK(scheme_exists && !scheme_is_package);
  CHECK(scheme_state.resolve_calls == 1 &&
        strcmp(scheme_state.context, "scheme-ctx") == 0);
  lightusd_string_destroy(resolved);
  resolved = NULL;
  CHECK_OK(lightusd_asset_resolver_read(resolver, "studio:note.usda", &resolved));
  {
    const lightusd_sv text = lightusd_string_view(resolved);
    CHECK(scheme_state.read_calls == 1 && text.len == 10 &&
          memcmp(text.data, "#usda 1.0\n", 10) == 0);
  }
  lightusd_string_destroy(resolved);
  CHECK_OK(lightusd_asset_resolver_unbind_context(resolver, scheme_context,
                                                   &scheme_binding));
  lightusd_resolver_context_destroy(scheme_context);
  CHECK(lightusd_asset_resolver_has_scheme(resolver, "STUDIO") == 1);
  CHECK_OK(lightusd_asset_resolver_unregister_scheme(resolver, "studio"));
  CHECK(lightusd_asset_resolver_unregister_scheme(resolver, "studio") ==
        LIGHTUSD_ERR_NOT_FOUND);
  CHECK(lightusd_asset_resolver_has_scheme(resolver, "studio") == 0);

  TestSchemeState owned_state;
  memset(&owned_state, 0, sizeof(owned_state));
  CHECK_OK(lightusd_asset_resolver_register_scheme_owned(
      resolver, "owned", test_scheme_resolver, test_scheme_reader,
      &owned_state, test_scheme_destroy));
  CHECK(owned_state.destroy_calls == 0);
  CHECK_OK(lightusd_asset_resolver_unregister_scheme(resolver, "owned"));
  CHECK(owned_state.destroy_calls == 1);
  CHECK(lightusd_asset_resolver_has_scheme(resolver, "OWNED") == 0);
  CHECK(lightusd_asset_resolver_has_scheme(resolver, "owned") == 0);

  resolved = NULL;
  uint8_t exists = 0, is_package = 0;
  CHECK_OK(lightusd_asset_resolver_resolve_for_new_asset(
      resolver, "sub/new.usda", "/tmp/look/layer.usda", &resolved, &exists,
      &is_package));
  {
    lightusd_sv text = lightusd_string_view(resolved);
    CHECK(!exists && !is_package && text.len == strlen("/tmp/look/sub/new.usda") &&
          memcmp(text.data, "/tmp/look/sub/new.usda", text.len) == 0);
  }
  lightusd_string_destroy(resolved);

  int package_calls = 0;
  CHECK_OK(lightusd_asset_resolver_register_package(
      resolver, ".pkg", test_package_resolver_callback, &package_calls));
  lightusd_resolver_cache_scope* cache = NULL;
  CHECK_OK(lightusd_asset_resolver_create_cache_scope(resolver, &cache));
  CHECK_OK(lightusd_asset_resolver_resolve(resolver, "bundle.pkg[entry.usda]",
                                           NULL, &resolved, &exists,
                                           &is_package));
  {
    lightusd_sv text = lightusd_string_view(resolved);
    CHECK(exists && is_package && text.len != 0 &&
          strstr(text.data, "virtual:bundle.pkg|entry.usda") != NULL);
  }
  lightusd_string_destroy(resolved);
  CHECK_OK(lightusd_asset_resolver_resolve(resolver, "bundle.pkg[entry.usda]",
                                           NULL, &resolved, NULL, NULL));
  lightusd_string_destroy(resolved);
  CHECK(package_calls == 1);
  lightusd_resolver_cache_scope_destroy(cache);
  CHECK_OK(lightusd_asset_resolver_resolve(resolver, "bundle.pkg[entry.usda]",
                                           NULL, &resolved, NULL, NULL));
  lightusd_string_destroy(resolved);
  CHECK(package_calls == 2);
  CHECK_OK(lightusd_asset_resolver_unregister_package(resolver, "pkg"));
  CHECK(lightusd_asset_resolver_unregister_package(resolver, "pkg") ==
        LIGHTUSD_ERR_NOT_FOUND);

  int owned_package_destroyed = 0;
  CHECK_OK(lightusd_asset_resolver_register_package_owned(
      resolver, "ownedpkg", test_owned_package_callback,
      &owned_package_destroyed, test_owned_package_destroy));
  CHECK(owned_package_destroyed == 0);
  CHECK_OK(lightusd_asset_resolver_unregister_package(resolver, "ownedpkg"));
  CHECK(owned_package_destroyed == 1);

  /* Composed loading must use the caller's resolver, not the default one. */
  {
    static const char dep_source[] =
        "#usda 1.0\n(defaultPrim = \"Base\")\n"
        "def Xform \"Base\" { def Xform \"Child\" {} }\n";
    static const char root_source[] =
        "#usda 1.0\ndef Xform \"Root\" (references = @dep.usda@) {}\n";
    const char* root_path = "resolver-load-root.usda";
    CHECK_OK(lightusd_asset_resolver_register_memory(
        resolver, "demo:dep.usda", (const uint8_t*)dep_source,
        sizeof(dep_source) - 1));
    CHECK_OK(lightusd_asset_resolver_set_alias(resolver, "dep.usda",
                                                "demo:dep.usda"));
    FILE* f = fopen(root_path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(root_source, 1, sizeof(root_source) - 1, f) ==
          sizeof(root_source) - 1);
    fclose(f);

    lightusd_load_options options;
    lightusd_load_options_init(&options);
    options.resolver = resolver;
    lightusd_stage* loaded = NULL;
    CHECK_OK(lightusd_stage_load(root_path, &options, &loaded));
    const lightusd_prim child =
        lightusd_stage_prim_at_path(loaded, "/Root/Child");
    CHECK(lightusd_prim_is_valid(child));
    lightusd_stage_destroy(loaded);
    remove(root_path);
  }

  CHECK_OK(lightusd_asset_resolver_register_memory(
      resolver, "demo:root.usda", (const uint8_t*)source, sizeof(source) - 1));
  /* Reading a package entry identifier returns the package file's bytes. */
  {
    const char* package_path = "resolver-read-fake.usdz";
    FILE* f = fopen(package_path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite("package-bytes", 1, 13, f) == 13);
    fclose(f);
    lightusd_string* bytes = NULL;
    CHECK_OK(lightusd_asset_resolver_read(resolver,
                                          "resolver-read-fake.usdz[entry.usda]",
                                          &bytes));
    lightusd_sv view = lightusd_string_view(bytes);
    CHECK(view.len == 13 && memcmp(view.data, "package-bytes", 13) == 0);
    lightusd_string_destroy(bytes);
    remove(package_path);
  }

  /* Reading a registered memory asset returns its bytes. */
  {
    lightusd_string* bytes = NULL;
    CHECK_OK(lightusd_asset_resolver_read(resolver, "demo:dep.usda", &bytes));
    CHECK(bytes != NULL && lightusd_string_view(bytes).len > 0);
    lightusd_string_destroy(bytes);
  }

  /* Aliases can be removed again. */
  {
    CHECK_OK(lightusd_asset_resolver_set_alias(resolver, "alias.usda",
                                               "demo:root.usda"));
    CHECK_OK(lightusd_asset_resolver_unset_alias(resolver, "alias.usda"));
    CHECK(lightusd_asset_resolver_unset_alias(resolver, "alias.usda") ==
              LIGHTUSD_ERR_NOT_FOUND);
  }

  /* Stable identifiers stay context-dependent for unresolved paths. */
  {
    lightusd_string* identifier = NULL;
    CHECK_OK(lightusd_asset_resolver_create_identifier(
        resolver, "future.usda", "/tmp/look/layer.usda", &identifier));
    lightusd_sv view = lightusd_string_view(identifier);
    CHECK(view.len == strlen("/tmp/look/future.usda") &&
              memcmp(view.data, "/tmp/look/future.usda", view.len) == 0);
    lightusd_string_destroy(identifier);
  }

  /* Asset info and timestamps are exposed for resolved assets. */
  {
    lightusd_asset_info info;
    CHECK_OK(lightusd_asset_resolver_get_asset_info(resolver, "demo:root.usda",
                                                    NULL, &info));
    lightusd_sv name = lightusd_string_view(info.asset_name);
    CHECK(name.len > 0);
    CHECK(info.exists == 1);

    /* An unresolved asset has no known repository. */
    lightusd_asset_info missing;
    CHECK_OK(lightusd_asset_resolver_get_asset_info(resolver, "missing.usda",
                                                    NULL, &missing));
    CHECK(missing.exists == 0);
    CHECK(lightusd_string_view(missing.repository_path).len == 0);
    lightusd_asset_info_destroy(&missing);
    lightusd_string_destroy(info.resolved_path);
    lightusd_string_destroy(info.asset_name);
    lightusd_string_destroy(info.repository_path);

    uint64_t seconds = 0;
    uint32_t nanoseconds = 0;
    uint8_t valid = 0;
    CHECK_OK(lightusd_asset_resolver_get_modification_timestamp(
        resolver, "demo:root.usda", NULL, &seconds, &nanoseconds, &valid));
    CHECK(valid == 0 || seconds > 0);
  }

  CHECK_OK(lightusd_asset_resolver_register_memory(
      resolver, "demo:readme.txt", note, sizeof(note)));
  CHECK_OK(lightusd_asset_resolver_set_alias(
      resolver, "readme.txt", "demo:readme.txt"));

  lightusd_string* report = NULL;
  uint8_t complete = 0;
  CHECK_OK(lightusd_dependency_report_json(resolver, "demo:root.usda",
                                            &report, &complete));
  CHECK(complete == 1);
  lightusd_sv text = lightusd_string_view(report);
  CHECK(text.len != 0 && strstr(text.data, "demo:readme.txt") != NULL);
  lightusd_string_destroy(report);

  lightusd_string* asset = NULL;
  CHECK_OK(lightusd_asset_resolver_read(resolver, "demo:readme.txt", &asset));
  text = lightusd_string_view(asset);
  CHECK(text.len == sizeof(note) && memcmp(text.data, note, sizeof(note)) == 0);
  lightusd_string_destroy(asset);

  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)source,
                                           sizeof(source) - 1, NULL, &stage));
  size_t errors = 0, warnings = 0;
  CHECK_OK(lightusd_stage_validate_core(stage, &errors, &warnings));
  CHECK(errors == 0);
  lightusd_stage_destroy(stage);

  CHECK_OK(lightusd_asset_resolver_register_memory(
      resolver, "demo:missing-root.usda", (const uint8_t*)missing_source,
      sizeof(missing_source) - 1));
  CHECK_OK(lightusd_dependency_report_json(resolver, "demo:missing-root.usda",
                                            &report, &complete));
  CHECK(complete == 0);
  lightusd_string_destroy(report);
  CHECK_OK(lightusd_asset_resolver_unregister_memory(resolver,
                                                      "demo:readme.txt"));
  CHECK(lightusd_asset_resolver_unregister_memory(resolver,
                                                  "demo:readme.txt") ==
        LIGHTUSD_ERR_NOT_FOUND);
  CHECK(lightusd_asset_resolver_unregister_memory(resolver, "") ==
        LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_asset_resolver_read(resolver, "demo:readme.txt", &asset) ==
        LIGHTUSD_ERR_IO);
  CHECK(asset == NULL);
  lightusd_asset_resolver_destroy(resolver);

  CHECK_OK(lightusd_asset_resolver_create(&resolver));
  const uint8_t small[] = {1, 2, 3};
  const uint8_t replacement[] = {4, 5, 6, 7, 8};
  CHECK_OK(lightusd_asset_resolver_register_memory(resolver, "budget:a",
                                                    small, sizeof(small)));
  size_t asset_count = 0, bytes_used = 0, limit_bytes = 0;
  CHECK_OK(lightusd_asset_resolver_get_memory_stats(
      resolver, &asset_count, &bytes_used, &limit_bytes));
  CHECK(asset_count == 1 && bytes_used == sizeof(small) && limit_bytes == 0);
  CHECK_OK(lightusd_asset_resolver_set_memory_limit(resolver, 5));
  CHECK_OK(lightusd_asset_resolver_register_memory(
      resolver, "budget:a", replacement, sizeof(replacement)));
  CHECK(lightusd_asset_resolver_register_memory(resolver, "budget:b", small,
                                                 1) ==
        LIGHTUSD_ERR_RESOURCE_LIMIT);
  CHECK(lightusd_asset_resolver_set_memory_limit(resolver, 4) ==
        LIGHTUSD_ERR_RESOURCE_LIMIT);
  CHECK_OK(lightusd_asset_resolver_get_memory_stats(
      resolver, &asset_count, &bytes_used, &limit_bytes));
  CHECK(asset_count == 1 && bytes_used == 5 && limit_bytes == 5);
  CHECK_OK(lightusd_asset_resolver_unregister_memory(resolver, "budget:a"));
  CHECK_OK(lightusd_asset_resolver_get_memory_stats(
      resolver, &asset_count, &bytes_used, &limit_bytes));
  CHECK(asset_count == 0 && bytes_used == 0 && limit_bytes == 5);
  CHECK_OK(lightusd_asset_resolver_set_memory_limit(resolver, 4));
  CHECK_OK(lightusd_asset_resolver_register_memory(resolver, "budget:b", small,
                                                    sizeof(small)));
  CHECK_OK(lightusd_asset_resolver_register_memory(resolver, "budget:z", small,
                                                    1));
  CHECK_OK(lightusd_asset_resolver_get_memory_stats(
      resolver, &asset_count, &bytes_used, &limit_bytes));
  CHECK(asset_count == 2 && bytes_used == 4 && limit_bytes == 4);
  size_t required_size = 0;
  CHECK_OK(lightusd_asset_resolver_memory_identifier(resolver, 0, NULL, 0,
                                                      &required_size));
  CHECK(required_size == strlen("budget:b"));
  char identifier[16];
  memset(identifier, 'x', sizeof(identifier));
  CHECK(lightusd_asset_resolver_memory_identifier(
            resolver, 0, identifier, required_size - 1, &required_size) ==
        LIGHTUSD_ERR_RESOURCE_LIMIT);
  CHECK(identifier[0] == 'x');
  CHECK_OK(lightusd_asset_resolver_memory_identifier(
      resolver, 0, identifier, sizeof(identifier), &required_size));
  CHECK(required_size == strlen("budget:b") &&
        memcmp(identifier, "budget:b", required_size) == 0);
  CHECK_OK(lightusd_asset_resolver_memory_identifier(
      resolver, 1, identifier, sizeof(identifier), &required_size));
  CHECK(required_size == strlen("budget:z") &&
        memcmp(identifier, "budget:z", required_size) == 0);
  CHECK(lightusd_asset_resolver_memory_identifier(
            resolver, 2, identifier, sizeof(identifier), &required_size) ==
        LIGHTUSD_ERR_NOT_FOUND);
  CHECK(lightusd_asset_resolver_get_memory_stats(
      resolver, NULL, &bytes_used, &limit_bytes) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_asset_resolver_destroy(resolver);
  return 0;
}

static int test_native_instance_load(const char* path) {
  lightusd_load_options options;
  lightusd_load_options_init(&options);
  lightusd_stage* holder = NULL;
  CHECK_OK(lightusd_stage_load(path, &options, &holder));
  options.preserve_native_instances = 1;
  lightusd_stage* native = NULL;
  CHECK_OK(lightusd_stage_load(path, &options, &native));
  const char* instances[] = {"Tree1", "Tree2", "Tree3", "Tree4"};
  int holder_enabled = 0;
  int native_enabled = 0;
  for (size_t i = 0; i < 4; ++i) {
    char prim_path[128];
    snprintf(prim_path, sizeof(prim_path),
             "/InstancingTests/Forest/%s", instances[i]);
    lightusd_prim hp = lightusd_stage_prim_at_path(holder, prim_path);
    lightusd_prim np = lightusd_stage_prim_at_path(native, prim_path);
    CHECK(lightusd_prim_is_valid(hp) && lightusd_prim_is_valid(np));
    lightusd_value* hv = NULL;
    lightusd_value* nv = NULL;
    CHECK_OK(lightusd_prim_get_metadata(hp, "instanceable", &hv));
    CHECK_OK(lightusd_prim_get_metadata(np, "instanceable", &nv));
    lightusd_value_view hview = {0}, nview = {0};
    CHECK_OK(lightusd_value_get_view(hv, &hview));
    CHECK_OK(lightusd_value_get_view(nv, &nview));
    CHECK(hview.data && nview.data);
    holder_enabled += *(const uint8_t*)hview.data != 0;
    native_enabled += *(const uint8_t*)nview.data != 0;
    lightusd_value_destroy(hv);
    lightusd_value_destroy(nv);
  }
  CHECK(native_enabled == 4 && holder_enabled < native_enabled);
  lightusd_prim secondary = lightusd_stage_prim_at_path(
      native, "/InstancingTests/Forest/Tree2");
  lightusd_sv prototype = lightusd_prim_instance_prototype_path(secondary);
  CHECK(prototype.data && prototype.len > 0);
  CHECK(prototype.len == sizeof("/InstancingTests/Forest/Tree1") - 1);
  CHECK(memcmp(prototype.data, "/InstancingTests/Forest/Tree1",
               sizeof("/InstancingTests/Forest/Tree1") - 1) == 0);
  CHECK(lightusd_prim_instance_prototype_path(
            lightusd_stage_prim_at_path(
                native, "/InstancingTests/Forest/Tree1"))
            .len == 0);
  CHECK(lightusd_prim_instance_prototype_path((lightusd_prim){0}).len == 0);
  lightusd_stage_destroy(holder);
  lightusd_stage_destroy(native);
  return 0;
}

static int test_authored_stage_metadata(void) {
  const char* source = "#usda 1.0\n(startTimeCode = 0\n endTimeCode = 0\n"
      " upAxis = \"Y\"\n metersPerUnit = 0.01\n framesPerSecond = 24\n"
      " timeCodesPerSecond = 24\n defaultPrim = \"\"\n documentation = \"\"\n comment = \"\")\n";
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)source, strlen(source), NULL, &stage));
  const char* keys[] = {"startTimeCode", "endTimeCode", "upAxis", "metersPerUnit",
      "framesPerSecond", "timeCodesPerSecond", "defaultPrim", "doc", "comment"};
  for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
    CHECK(lightusd_stage_metadata_is_authored(stage, keys[i]));
  CHECK(!lightusd_stage_metadata_is_authored(stage, "kilogramsPerUnit"));
  CHECK(!lightusd_stage_metadata_is_authored(stage, "missing"));
  CHECK(!lightusd_stage_metadata_is_authored(stage, NULL));
  CHECK(!lightusd_stage_metadata_is_authored(NULL, "upAxis"));
  lightusd_stage_destroy(stage);
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK(!lightusd_stage_metadata_is_authored(stage, "startTimeCode"));
  const double zero = 0;
  CHECK_OK(lightusd_stage_set_metadata(stage, "startTimeCode", LIGHTUSD_TYPE_DOUBLE, &zero, 1));
  CHECK(lightusd_stage_metadata_is_authored(stage, "startTimeCode"));
  lightusd_stage_destroy(stage);
  return 0;
}

static int test_composition_metadata_queries(void) {
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_define_prim(stage, "/Root", "Xform", 0, NULL));
  CHECK_OK(lightusd_prim_add_arc(stage, "/Root", LIGHTUSD_ARC_REFERENCE, "ref.usda", "/Asset"));
  CHECK_OK(lightusd_prim_add_arc(stage, "/Root", LIGHTUSD_ARC_PAYLOAD, "payload.usda", "/Asset"));
  CHECK_OK(lightusd_prim_add_arc(stage, "/Root", LIGHTUSD_ARC_INHERIT, NULL, "/Base"));
  CHECK_OK(lightusd_prim_add_arc(stage, "/Root", LIGHTUSD_ARC_SPECIALIZE, NULL, "/Special"));
  CHECK_OK(lightusd_prim_add_variant_set(stage, "/Root", "look"));
  CHECK_OK(lightusd_prim_add_variant(stage, "/Root", "look", "red"));
  CHECK_OK(lightusd_prim_set_variant_selection(stage, "/Root", "look", "red"));
  CHECK_OK(lightusd_prim_set_variant_selection(stage, "/Root", "external", "blue"));
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, "/Root");
  for (uint8_t type = 0; type < 4; ++type) CHECK(lightusd_prim_arc_count(prim, type) == 1);
  CHECK(lightusd_prim_arc_count(prim, 255) == 0);
  const char* arcs[] = {"@ref.usda@</Asset>", "@payload.usda@</Asset>", "</Base>", "</Special>"};
  for (uint8_t type = 0; type < 4; ++type) {
    lightusd_sv text = lightusd_prim_arc_text(prim, type, 0);
    CHECK(text.len == strlen(arcs[type]) && memcmp(text.data, arcs[type], text.len) == 0);
    CHECK(lightusd_prim_arc_text(prim, type, 1).len == 0);
  }
  CHECK(lightusd_prim_arc_text(prim, 255, 0).len == 0);
  CHECK(lightusd_prim_arc_text((lightusd_prim){0}, LIGHTUSD_ARC_PAYLOAD, 0).len == 0);
  CHECK(lightusd_prim_arc_count((lightusd_prim){0}, LIGHTUSD_ARC_REFERENCE) == 0);
  CHECK(lightusd_prim_variant_set_count(prim) == 1);
  lightusd_strlist* names = NULL;
  CHECK_OK(lightusd_prim_variant_selection_names(prim, &names));
  CHECK(lightusd_strlist_size(names) == 2);
  lightusd_sv key = lightusd_strlist_get(names, 1);
  CHECK(key.len == 8 && memcmp(key.data, "external", 8) == 0);
  lightusd_sv selection = lightusd_variant_selection(prim, "external");
  CHECK(selection.len == 4 && memcmp(selection.data, "blue", 4) == 0);
  CHECK(lightusd_prim_variant_selection_names(prim, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_stage_destroy(stage);
  CHECK(lightusd_strlist_get(names, 1).len == 8);
  lightusd_strlist_destroy(names);
  CHECK(lightusd_prim_variant_selection_names((lightusd_prim){0}, &names) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!names);
  return 0;
}

static int test_inherited_relationships(const char* path) {
  lightusd_load_options options;
  lightusd_load_options_init(&options);
  options.composed = 1;
  options.preserve_native_instances = 1;
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load(path, &options, &stage));
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, "/Instance");
  CHECK(lightusd_prim_is_valid(prim));
  CHECK(lightusd_prim_relationship_count(prim) == 2);
  CHECK(lightusd_prim_has_relationship(prim, "link"));
  CHECK(lightusd_prim_has_relationship(prim, "empty"));
  CHECK(lightusd_rel_target_count(prim, "empty") == 0);
  CHECK(lightusd_rel_target_count(prim, "link") == 1);
  lightusd_sv target = lightusd_rel_target(prim, "link", 0);
  CHECK(target.len == 16 && memcmp(target.data, "/Instance/Target", 16) == 0);
  CHECK(lightusd_rel_target(prim, "link", 1).len == 0);
  CHECK(lightusd_rel_target_count(lightusd_stage_prim_at_path(stage, "/Override"), "link") == 2);
  // Remove the local opinion from the instance so lookup must use its source.
  CHECK_OK(lightusd_rel_remove(stage, "/Override", "link"));
  prim = lightusd_stage_prim_at_path(stage, "/Override");
  CHECK(lightusd_prim_relationship_count(prim) == 2);
  CHECK(lightusd_prim_has_relationship(prim, "link"));
  CHECK(lightusd_rel_target_count(prim, "link") == 1);
  target = lightusd_rel_target(prim, "link", 0);
  CHECK(target.len == 16 && memcmp(target.data, "/Instance/Target", 16) == 0);
  lightusd_strlist* names = NULL;
  CHECK_OK(lightusd_prim_relationship_names(prim, &names));
  CHECK(lightusd_strlist_size(names) == 2);
  lightusd_stage_destroy(stage);
  CHECK(lightusd_strlist_get(names, 0).len > 0);
  lightusd_strlist_destroy(names);
  CHECK(lightusd_prim_relationship_names((lightusd_prim){0}, &names) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!names);
  CHECK(lightusd_prim_relationship_count((lightusd_prim){0}) == 0);
  CHECK(!lightusd_prim_has_relationship((lightusd_prim){0}, "link"));
  CHECK(lightusd_rel_target_count((lightusd_prim){0}, "link") == 0);
  return 0;
}

static int test_value_clip_metadata(const char* path) {
  lightusd_load_options options;
  lightusd_load_options_init(&options);
  options.composed = 0;
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load(path, &options, &stage));
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, "/GEO");
  CHECK(lightusd_prim_is_valid(prim));
  CHECK(lightusd_prim_has_value_clips(prim));
  CHECK(!lightusd_prim_has_value_clips(
      lightusd_stage_prim_at_path(stage, "/missing")));
  CHECK(!lightusd_prim_has_value_clips((lightusd_prim){0}));
  lightusd_stage_destroy(stage);
  return 0;
}

static int inspect_array_queries(lightusd_prim prim, size_t* arrays) {
  const size_t count = lightusd_prim_property_count(prim);
  for (size_t i = 0; i < count; ++i) {
    lightusd_sv name = lightusd_prim_property_name(prim, i);
    char buffer[512];
    if (!name.data || name.len == 0 || name.len >= sizeof(buffer)) continue;
    memcpy(buffer, name.data, name.len);
    buffer[name.len] = '\0';
    lightusd_value_view view = {0};
    if (lightusd_attr_inspect_default(prim, buffer, &view, NULL) !=
            LIGHTUSD_OK || !view.is_array)
      continue;
    int would_materialize = -1;
    CHECK_OK(lightusd_attr_would_materialize_array(
        prim, buffer, &would_materialize));
    CHECK(would_materialize == 0 || would_materialize == 1);
    ++*arrays;
  }
  const size_t children = lightusd_prim_child_count(prim);
  for (size_t i = 0; i < children; ++i)
    CHECK(inspect_array_queries(lightusd_prim_child(prim, i), arrays) == 0);
  return 0;
}

static int test_lazy_array_materialize_query(const char* path) {
  lightusd_load_options options;
  lightusd_load_options_init(&options);
  options.composed = 0;
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load(path, &options, &stage));
  size_t arrays = 0;
  for (size_t i = 0; i < lightusd_stage_root_prim_count(stage); ++i)
    CHECK(inspect_array_queries(lightusd_stage_root_prim(stage, i),
                                &arrays) == 0);
  CHECK(arrays > 0);
  lightusd_stage_destroy(stage);
  return 0;
}


static int test_resolved_property_inspection(void) {
  const char* source = "#usda 1.0\ndef Cube \"Root\" {\n"
      " custom float weight = 0.25\n custom float[] weights = [1,2,3]\n"
      " custom string label = \"hello\"\n token visibility = None\n}\n";
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)source, strlen(source), NULL, &stage));
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, "/Root");
  lightusd_strlist* names = NULL;
  CHECK_OK(lightusd_prim_property_names(prim, &names));
  CHECK(lightusd_strlist_size(names) > lightusd_prim_property_count(prim));
  int found_size = 0, found_weight = 0;
  for (size_t i = 0; i < lightusd_strlist_size(names); ++i) {
    lightusd_sv name = lightusd_strlist_get(names, i);
    if (name.len == 4 && memcmp(name.data, "size", 4) == 0) found_size = 1;
    if (name.len == 6 && memcmp(name.data, "weight", 6) == 0) found_weight = 1;
  }
  CHECK(found_size && found_weight);
  lightusd_value_view value = {0};
  lightusd_sv text = {0};
  CHECK_OK(lightusd_attr_inspect_default(prim, "size", &value, &text));
  CHECK(value.type == LIGHTUSD_TYPE_DOUBLE && !value.is_array);
  CHECK(*(const double*)value.data == 2.0);
  CHECK_OK(lightusd_attr_inspect_default(prim, "weight", &value, &text));
  CHECK(value.type == LIGHTUSD_TYPE_FLOAT && *(const float*)value.data == 0.25f);
  CHECK_OK(lightusd_attr_inspect_default(prim, "weights", &value, &text));
  CHECK(value.is_array && value.count == 3 && !value.data && value.nbytes == 0);
  CHECK_OK(lightusd_attr_inspect_default(prim, "label", &value, &text));
  CHECK(text.len == 5 && memcmp(text.data, "hello", 5) == 0);
  CHECK(lightusd_attr_inspect_default(prim, "visibility", &value, &text) == LIGHTUSD_ERR_NOT_FOUND);
  CHECK(value.type == LIGHTUSD_TYPE_INVALID && !value.data && text.len == 0);
  CHECK(lightusd_attr_inspect_default(prim, "absent", &value, NULL) == LIGHTUSD_ERR_NOT_FOUND);
  CHECK(lightusd_attr_inspect_default((lightusd_prim){0}, "size", &value, &text) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_value *copied = NULL, *schema_default = NULL;
  CHECK_OK(lightusd_attr_copy_default(prim, "weights", &copied));
  CHECK_OK(lightusd_attr_copy_default(prim, "size", &schema_default));
  CHECK_OK(lightusd_value_get_view(schema_default, &value));
  CHECK(value.type == LIGHTUSD_TYPE_DOUBLE && *(const double*)value.data == 2.0);
  lightusd_value_destroy(schema_default);
  schema_default = copied;
  CHECK(lightusd_attr_copy_default(prim, "visibility", &schema_default) == LIGHTUSD_ERR_NOT_FOUND);
  CHECK(!schema_default);
  CHECK(lightusd_attr_copy_default(prim, "absent", &schema_default) == LIGHTUSD_ERR_NOT_FOUND);
  CHECK(lightusd_attr_copy_default((lightusd_prim){0}, "weights", &schema_default) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!schema_default);
  CHECK(lightusd_attr_copy_default(prim, NULL, &schema_default) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_attr_copy_default(prim, "weights", NULL) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!lightusd_prim_has_payload(prim));
  CHECK(!lightusd_prim_has_payload((lightusd_prim){0}));
  CHECK_OK(lightusd_prim_add_arc(stage, "/Root", LIGHTUSD_ARC_PAYLOAD, "payload.usda", "/Root"));
  CHECK(lightusd_prim_has_payload(lightusd_stage_prim_at_path(stage, "/Root")));
  lightusd_stage_destroy(stage);
  CHECK_OK(lightusd_value_get_view(copied, &value));
  CHECK(value.is_array && value.count == 3 && value.nbytes == 3 * sizeof(float));
  CHECK(((const float*)value.data)[0] == 1 && ((const float*)value.data)[2] == 3);
  lightusd_value_destroy(copied);
  CHECK(lightusd_strlist_size(names) > 0);
  lightusd_strlist_destroy(names);
  CHECK(lightusd_prim_property_names((lightusd_prim){0}, &names) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!names);
  return 0;
}

static int test_backplate_queries(void) {
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_define_prim(stage, "/Camera", "Camera", 0, NULL));
  const char* schemas[] = {"BackPlateAPI:main", "BackPlateAPI:defaults"};
  CHECK_OK(lightusd_prim_set_metadata_token_array(stage, "/Camera", "apiSchemas", schemas, 2));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:image",
      LIGHTUSD_TYPE_ASSET_PATH, 0, "plate.png", 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:alpha:image",
      LIGHTUSD_TYPE_ASSET_PATH, 0, "alpha.png", 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:depth:image",
      LIGHTUSD_TYPE_ASSET_PATH, 0, "depth.png", 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:plateVisibility",
      LIGHTUSD_TYPE_TOKEN, 0, "invisible", 1, 0));
  const float start = 2, end = 6, scale[] = {0.5f, 0.75f};
  const float gain[] = {1.5f, 2, 3};
  CHECK_OK(lightusd_attr_set_timesample(stage, "/Camera", "backPlate:main:depth:minOffset",
      0, LIGHTUSD_TYPE_FLOAT, 0, &start, 1));
  CHECK_OK(lightusd_attr_set_timesample(stage, "/Camera", "backPlate:main:depth:minOffset",
      2, LIGHTUSD_TYPE_FLOAT, 0, &end, 1));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:scale:tweak",
      LIGHTUSD_TYPE_FLOAT2, 0, scale, 1, 0));
  CHECK_OK(lightusd_attr_set(stage, "/Camera", "backPlate:main:luma:gain",
      LIGHTUSD_TYPE_COLOR3F, 0, gain, 1, 0));
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, "/Camera");
  lightusd_backplate *plate = NULL, *defaults = NULL;
  lightusd_backplate_info info;
  CHECK_OK(lightusd_backplate_eval(stage, prim, "main", 1, &plate));
  CHECK_OK(lightusd_backplate_get_info(plate, &info));
  CHECK(info.depth_min_offset == 4);
  CHECK(info.scale_tweak[0] == 0.5f && info.scale_tweak[1] == 0.75f);
  CHECK(info.luma_gain[0] == 1.5f && info.luma_gain[2] == 3);
  CHECK(info.plate_visibility.len == 9 && !memcmp(info.plate_visibility.data, "invisible", 9));
  CHECK_OK(lightusd_backplate_eval(stage, prim, "defaults", 0, &defaults));
  CHECK_OK(lightusd_backplate_get_info(defaults, &info));
  CHECK(!info.image.len && !info.alpha_image.len && !info.depth_image.len);
  CHECK(info.depth_min_offset == 0 && info.depth_normalizing_factor == 1);
  CHECK(info.depth_camera_space_offset == 0);
  CHECK(info.scale_tweak[0] == 1 && info.scale_tweak[1] == 1);
  for (int i = 0; i < 3; ++i) {
    CHECK(info.rotate_xyz_tweak[i] == 0 && info.translate_tweak[i] == 0);
    CHECK(info.luma_gain[i] == 1 && info.luma_lift[i] == 0 && info.luma_gamma[i] == 1);
  }
  lightusd_backplate_destroy(defaults);
  defaults = plate;
  CHECK(lightusd_backplate_eval(stage, prim, "absent", 0, &defaults) == LIGHTUSD_ERR_NOT_FOUND);
  CHECK(!defaults);
  CHECK(lightusd_backplate_eval(stage, prim, "bad/name", 0, &defaults) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_backplate_eval(stage, prim, "main", NAN, &defaults) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_backplate_eval(stage, prim, "main", INFINITY, &defaults) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(lightusd_backplate_eval(stage, prim, "main", 0, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_stage* other = NULL;
  CHECK_OK(lightusd_stage_create(&other));
  CHECK(lightusd_backplate_eval(other, prim, "main", 0, &defaults) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_stage_destroy(other);
  CHECK_OK(lightusd_stage_define_prim(stage, "/Other", "Scope", 0, NULL));
  CHECK(lightusd_backplate_eval(stage, prim, "main", 0, &defaults) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_stage_destroy(stage);
  /* The result and all its strings outlive both mutation and stage destruction. */
  CHECK_OK(lightusd_backplate_get_info(plate, &info));
  CHECK(info.image.len == 9 && !memcmp(info.image.data, "plate.png", 9));
  CHECK(info.alpha_image.len == 9 && !memcmp(info.alpha_image.data, "alpha.png", 9));
  CHECK(info.depth_image.len == 9 && !memcmp(info.depth_image.data, "depth.png", 9));
  CHECK(info.depth_min_offset == 4);
  lightusd_backplate_destroy(plate);
  CHECK(lightusd_backplate_get_info(NULL, &info) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!info.image.data && !info.image.len && info.depth_min_offset == 0);
  lightusd_backplate_destroy(NULL);
  return 0;
}

static int test_skeleton_sample(void) {
  static const char usda[] =
      "#usda 1.0\n"
      "def Skeleton \"Skel\" {\n"
      " uniform token[] joints = [\"Root\", \"Root/Child\"]\n"
      " uniform matrix4d[] restTransforms = [((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,2,0,1))]\n"
      " def SkelAnimation \"Anim\" {\n"
      "  uniform token[] joints = [\"Root\", \"Root/Child\"]\n"
      "  float3[] translations.timeSamples = {0: [(0,0,0),(0,1,0)], 2: [(0,0,0),(0,3,0)]}\n"
      "  quatf[] rotations = [(1,0,0,0),(1,0,0,0)]\n"
      " }\n"
      "}\n";
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_load_from_memory((const uint8_t*)usda,
      strlen(usda), NULL, &stage));
  lightusd_prim skel = lightusd_stage_prim_at_path(stage, "/Skel");
  lightusd_prim anim = lightusd_stage_prim_at_path(stage, "/Skel/Anim");
  lightusd_skel_sample* sample = NULL;
  CHECK_OK(lightusd_skel_sample_create(stage, skel, anim, 1, &sample));
  lightusd_skel_sample_info info;
  CHECK_OK(lightusd_skel_sample_get_info(sample, &info));
  CHECK(info.joint_count == 2 && info.animation_joint_count == 2);
  CHECK(info.parent_indices[0] == -1 && info.parent_indices[1] == 0);
  CHECK(info.rest_transform_count == 32 && info.rest_transforms[29] == 2);
  CHECK(info.has_translations && info.translation_count == 6);
  CHECK(info.translations[4] == 2 && info.has_rotations && info.rotation_count == 8);
  lightusd_sv name;
  CHECK_OK(lightusd_skel_sample_joint_name(sample, 1, &name));
  CHECK(name.len == 10 && !memcmp(name.data, "Root/Child", 10));
  CHECK_OK(lightusd_skel_sample_animation_joint_name(sample, 0, &name));
  CHECK(name.len == 4 && !memcmp(name.data, "Root", 4));
  CHECK(lightusd_skel_sample_joint_name(sample, 2, &name) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!name.data && !name.len);
  lightusd_skel_sample_destroy(sample);
  sample = NULL;
  CHECK(lightusd_skel_sample_create(stage, anim, skel, 0, &sample) == LIGHTUSD_ERR_TYPE_MISMATCH);
  CHECK(!sample);
  lightusd_stage* other = NULL;
  CHECK_OK(lightusd_stage_create(&other));
  CHECK(lightusd_skel_sample_create(other, skel, anim, 0, &sample) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!sample);
  CHECK_OK(lightusd_stage_define_prim(other, "/Other", "SkelAnimation", 0, NULL));
  lightusd_prim foreign_anim = lightusd_stage_prim_at_path(other, "/Other");
  CHECK(lightusd_skel_sample_create(stage, skel, foreign_anim, 0, &sample) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!sample);
  lightusd_stage_destroy(other);
  CHECK(lightusd_skel_sample_create(stage, skel, anim, NAN, &sample) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!sample);
  CHECK_OK(lightusd_skel_sample_create(stage, skel, (lightusd_prim){0}, 0, &sample));
  CHECK_OK(lightusd_skel_sample_get_info(sample, &info));
  CHECK(info.animation_joint_count == 0 && !info.has_translations);
  CHECK_OK(lightusd_stage_define_prim(stage, "/Other", "Scope", 0, NULL));
  lightusd_skel_sample* stale = NULL;
  CHECK(lightusd_skel_sample_create(stage, skel, anim, 0, &stale) == LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!stale);
  lightusd_stage_destroy(stage);
  CHECK_OK(lightusd_skel_sample_get_info(sample, &info));
  CHECK(info.rest_transforms[29] == 2 && info.parent_indices[1] == 0);
  lightusd_skel_sample_destroy(sample);
  lightusd_skel_sample_destroy(NULL);
  return 0;
}

static int test_material_binding_query(void) {
  lightusd_stage* stage = NULL;
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World", "Xform", 0, NULL));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Mat", "Material", 0, NULL));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Mat/Shader", "Shader", 0, NULL));
  CHECK_OK(lightusd_stage_define_prim(stage, "/World/Mesh", "Mesh", 0, NULL));
  const float roughness = 0.375f;
  CHECK_OK(lightusd_attr_set(stage, "/World/Mat/Shader", "inputs:roughness",
                             LIGHTUSD_TYPE_FLOAT, 0, &roughness, 1, 0));
  CHECK_OK(lightusd_rel_add_target(stage, "/World/Mat", "outputs:surface",
                                   "/World/Mat/Shader"));
  CHECK_OK(lightusd_rel_add_target(stage, "/World", "material:binding",
                                   "/World/Mat"));
  CHECK_OK(lightusd_rel_add_target(stage, "/World", "material:binding:back",
                                   "/World/Mat"));
  lightusd_prim mesh = lightusd_stage_prim_at_path(stage, "/World/Mesh");
  lightusd_string* result = NULL;
  CHECK_OK(lightusd_prim_bound_material_path(stage, mesh, NULL, &result));
  lightusd_sv path = lightusd_string_view(result);
  CHECK(path.len == strlen("/World/Mat") && !memcmp(path.data, "/World/Mat", path.len));
  lightusd_string_destroy(result);
  result = NULL;
  CHECK_OK(lightusd_material_shader_path(stage,
      lightusd_stage_prim_at_path(stage, "/World/Mat"),
      LIGHTUSD_MATERIAL_SHADER_SURFACE, &result));
  path = lightusd_string_view(result);
  CHECK(path.len == strlen("/World/Mat/Shader") &&
        !memcmp(path.data, "/World/Mat/Shader", path.len));
  lightusd_string_destroy(result);
  result = NULL;
  lightusd_value* port = NULL;
  CHECK_OK(lightusd_shader_port_value(stage,
      lightusd_stage_prim_at_path(stage, "/World/Mat/Shader"),
      "inputs:roughness", 0, &port));
  lightusd_value_view portView;
  CHECK_OK(lightusd_value_get_view(port, &portView));
  CHECK(portView.type == LIGHTUSD_TYPE_FLOAT && !portView.is_array &&
        *(const float*)portView.data == roughness);
  lightusd_value_destroy(port);
  CHECK(lightusd_material_shader_path(stage, mesh, LIGHTUSD_MATERIAL_SHADER_VOLUME,
                                      &result) == LIGHTUSD_ERR_TYPE_MISMATCH);
  CHECK(!result);
  CHECK(lightusd_material_shader_path(stage,
      lightusd_stage_prim_at_path(stage, "/World/Mat"), 255, &result) ==
      LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!result);
  result = NULL;
  CHECK_OK(lightusd_prim_bound_material_path(stage, mesh, "back", &result));
  path = lightusd_string_view(result);
  CHECK(path.len == strlen("/World/Mat") && !memcmp(path.data, "/World/Mat", path.len));
  lightusd_string_destroy(result);
  result = NULL;
  CHECK_OK(lightusd_prim_bound_material_path(stage, mesh, "preview", &result));
  path = lightusd_string_view(result);
  CHECK(path.len == strlen("/World/Mat") && !memcmp(path.data, "/World/Mat", path.len));
  lightusd_string_destroy(result);
  result = NULL;
  CHECK(lightusd_prim_bound_material_path(stage, mesh, "bad purpose", &result) ==
        LIGHTUSD_OK);
  path = lightusd_string_view(result);
  CHECK(path.len == 0);
  lightusd_string_destroy(result);
  CHECK(lightusd_prim_bound_material_path(NULL, mesh, NULL, &result) ==
        LIGHTUSD_ERR_INVALID_ARG);
  CHECK(!result);
  lightusd_stage_destroy(stage);
  return 0;
}

static int read_pin_number(void* context, size_t index, lu_number* out) {
  (void)context;
  out->kind = LU_NUMBER_SIGNED;
  out->value.signed_value = (int64_t)index + 7;
  return 1;
}

static int test_buffer_pins(void) {
  lightusd_stage* stage = NULL;
  lightusd_prim prim;
  lightusd_value* value = NULL;
  lightusd_value_view borrowed, pinned;
  lightusd_buffer* buffer = NULL;
  const lightusd_abi_info abi = lightusd_abi_get();
  float replacement[] = {90, 91, 92};
  CHECK(abi.major == 5 && abi.core.major == 2);
  CHECK(abi.core.pointer_bits == sizeof(void*) * 8);
  CHECK(abi.value_view_size == sizeof(lightusd_value_view));
  CHECK(abi.value_view_data_offset == offsetof(lightusd_value_view, data));
  CHECK_OK(lightusd_stage_create(&stage));
  CHECK_OK(lightusd_stage_define_prim(stage, "/P", "Xform", 0, &prim));
  CHECK_OK(lightusd_value_from_numbers(LIGHTUSD_TYPE_FLOAT, 1, 3,
                                      read_pin_number, NULL, &value));
  CHECK_OK(lightusd_attr_set_value(stage, "/P", "values", value, 0));
  CHECK_OK(lightusd_value_get_view(value, &borrowed));
  CHECK_OK(lightusd_buffer_pin(borrowed, &buffer));
  pinned = lightusd_buffer_get_view(buffer);
  CHECK(pinned.data == borrowed.data); /* C11 payload retained without copying. */
  lightusd_value_destroy(value);
  CHECK_OK(lightusd_attr_set(stage, "/P", "values", LIGHTUSD_TYPE_FLOAT,
                             1, replacement, 3, 0));
  lightusd_stage_destroy(stage);
  CHECK(pinned.nbytes == 3 * sizeof(float));
  CHECK(((const float*)pinned.data)[0] == 7 && ((const float*)pinned.data)[2] == 9);
  lightusd_buffer_destroy(buffer);
  CHECK_OK(lightusd_value_from_numbers(LIGHTUSD_TYPE_INT, 0, 1,
                                      read_pin_number, NULL, &value));
  CHECK_OK(lightusd_value_get_view(value, &borrowed));
  CHECK_OK(lightusd_buffer_pin(borrowed, &buffer));
  lightusd_value_destroy(value);
  pinned = lightusd_buffer_get_view(buffer);
  CHECK(*(const int32_t*)pinned.data == 7); /* Inline scalar copied into the pin. */
  lightusd_buffer_destroy(buffer);
  return 0;
}

int main(int argc, char** argv) {
  if (test_buffer_pins()) return 1;
  if (test_skeleton_sample()) return 1;
  if (test_material_binding_query()) return 1;
  if (test_backplate_queries()) return 1;
  if (test_authored_stage_metadata()) return 1;
  if (test_composition_metadata_queries()) return 1;
  if (argc > 3 && test_inherited_relationships(argv[3])) return 1;
  if (argc > 4 && test_value_clip_metadata(argv[4])) return 1;
  if (argc > 5 && test_lazy_array_materialize_query(argv[5])) return 1;
  if (test_resolved_property_inspection()) return 1;
  CHECK(lightusd_api_version() == ((LIGHTUSD_API_VERSION_MAJOR << 16) |
      (LIGHTUSD_API_VERSION_MINOR << 8) | LIGHTUSD_API_VERSION_PATCH));
  CHECK(strcmp(lightusd_version_string(), "1.0.0-rc4") == 0);
  CHECK(strcmp(lightusd_type_name(LIGHTUSD_TYPE_POINT3F), "point3f") == 0);
  CHECK(lightusd_type_from_name("float3") == LIGHTUSD_TYPE_FLOAT3);
  CHECK(lightusd_type_component_count(LIGHTUSD_TYPE_MATRIX4D) == 16);

  if (test_empty_and_vector_arrays()) return 1;
  if (test_authoring_roundtrip()) return 1;
  printf("  authoring round-trip: PASSED\n");
  if (test_error_handling()) return 1;
  printf("  error handling: PASSED\n");
  if (test_core_transforms_and_ownership()) return 1;
  printf("  core transforms and ownership: PASSED\n");
  if (test_dependency_boundary()) return 1;
  printf("  dependency boundary: PASSED\n");
  if (test_name_validation()) return 1;
  printf("  name validation: PASSED\n");
  if (argc > 1) {
    if (test_file_load(argv[1])) return 1;
  }
  if (argc > 2) {
    if (test_native_instance_load(argv[2])) return 1;
    printf("  native instance load option: PASSED\n");
  }
  if (test_usda_lazy_load_options()) return 1;
  printf("  usda lazy parse options: PASSED\n");
  printf("All C API tests PASSED\n");
  return 0;
}
