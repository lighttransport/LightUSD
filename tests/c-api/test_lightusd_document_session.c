/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lightusd-session-c.h"
#include "lightusd-render-c.h"

static int on_progress(void* userdata, const lightusd_document_progress* event) {
  assert(event);
  return !*(const int*)userdata;
}

struct cache_callback_state {
  lightusd_document_session* session;
  int check_busy;
  int calls;
};
static int on_cache_progress(void* userdata, const lightusd_document_progress* event) {
  struct cache_callback_state* state = (struct cache_callback_state*)userdata;
  assert(event);
  if (state->check_busy) {
    lightusd_document_open_config config;
    lightusd_document_open_config_init(&config);
    assert(lightusd_document_session_configure_open(state->session, &config) == LIGHTUSD_ERR_BUSY);
    assert(lightusd_document_session_set_payload_callbacks(state->session, NULL, NULL, NULL) == LIGHTUSD_ERR_BUSY);
    assert(lightusd_document_session_trim_caches(state->session) == LIGHTUSD_ERR_BUSY);
    assert(lightusd_document_session_release_cache(state->session) == LIGHTUSD_ERR_BUSY);
    lightusd_geometry_release_stats stats = {0};
    assert(lightusd_document_session_release_geometry(state->session, NULL, 1, &stats) == LIGHTUSD_ERR_BUSY);
    ++state->calls;
  }
  return 1;
}

static void write_change_fixture(const char* filename, int edited) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fprintf(file, "#usda 1.0\n(upAxis = \"%s\")\n"
      "def Shader \"S\" {\n"
      " float inputs:roughness = %s\n"
      " asset inputs:file = @%s.png@\n}\n",
      edited ? "Z" : "Y", edited ? "0.7" : "0.2", edited ? "second" : "first") > 0);
  assert(fclose(file) == 0);
}

static void test_change_records(const char* filename) {
  write_change_fixture(filename, 0);
  lightusd_document_session* session = NULL;
  lightusd_document_snapshot *first = NULL, *edited = NULL, *unchanged = NULL;
  assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_open_file(session, filename, &first) == LIGHTUSD_OK);
  lightusd_document_changes_info info = {0};
  info.struct_size = sizeof(info);
  assert(lightusd_document_snapshot_changes_copy(first, &info, NULL, 0, NULL, 0) == LIGHTUSD_OK);
  assert(info.flags == 1 && info.base_revision == 0 && info.revision > 0);
  assert(info.prim_count == 1 && info.property_count == 0);
  const uint64_t first_revision = info.revision;
  write_change_fixture(filename, 1);
  assert(lightusd_document_session_reload_layer(session, filename, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_changes_copy(edited, &info, NULL, 0, NULL, 0) == LIGHTUSD_OK);
  assert(info.flags == 2 && info.base_revision == first_revision && info.revision > first_revision);
  assert(info.prim_count == 1 && info.property_count == 2);
  lightusd_prim_change prim = {0};
  prim.flags = 123;
  lightusd_sv properties[2] = {{"sentinel", 8}, {"sentinel", 8}};
  assert(lightusd_document_snapshot_changes_copy(edited, &info, &prim, 0, properties, 2) == LIGHTUSD_ERR_INVALID_ARG);
  assert(prim.flags == 123 && properties[0].len == 8);
  assert(info.prim_count == 1 && info.property_count == 2);
  assert(lightusd_document_snapshot_changes_copy(edited, &info, &prim, 1, properties, 1) == LIGHTUSD_ERR_INVALID_ARG);
  assert(prim.flags == 123 && properties[0].len == 8 && properties[1].len == 8);
  assert(lightusd_document_snapshot_changes_copy(edited, &info, &prim, 1, properties, 2) == LIGHTUSD_OK);
  assert(strcmp(prim.prim_path, "/S") == 0);
  assert((prim.flags & (LIGHTUSD_CHANGE_TEXTURE | LIGHTUSD_CHANGE_MATERIAL)) ==
                      (LIGHTUSD_CHANGE_TEXTURE | LIGHTUSD_CHANGE_MATERIAL));
  assert(prim.property_count == 2 && prim.properties == properties);
  assert(properties[0].len == 11 && memcmp(properties[0].data, "inputs:file", 11) == 0);
  assert(properties[1].len == 16 && memcmp(properties[1].data, "inputs:roughness", 16) == 0);
  lightusd_document_changes_info saved = info;
  assert(lightusd_document_snapshot_changes_copy(NULL, &info, NULL, 0, NULL, 0) == LIGHTUSD_ERR_INVALID_ARG);
  assert(memcmp(&info, &saved, sizeof(info)) == 0);
  assert(lightusd_document_snapshot_changes_copy(edited, NULL, NULL, 0, NULL, 0) == LIGHTUSD_ERR_INVALID_ARG);
  info.struct_size = sizeof(info) - 1;
  assert(lightusd_document_snapshot_changes_copy(edited, &info, &prim, 1, properties, 2) == LIGHTUSD_ERR_INVALID_ARG);
  info = saved;
  assert(lightusd_document_snapshot_changes_copy(edited, &info, NULL, 1, properties, 2) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_snapshot_changes_copy(edited, &info, &prim, 1, NULL, 2) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_reload_layer(session, filename, &unchanged) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_changes_copy(unchanged, &info, NULL, 0, NULL, 0) == LIGHTUSD_OK);
  assert(info.flags == 0 && info.prim_count == 0 && info.property_count == 0);
  lightusd_document_session_destroy(session);
  lightusd_document_snapshot_destroy(first);
  lightusd_document_snapshot_destroy(unchanged);
  /* Strings remain backed by the edited snapshot after later publication and
   * destruction of the session; array storage belongs to this caller. */
  assert(strcmp(prim.prim_path, "/S") == 0);
  assert(memcmp(properties[0].data, "inputs:file", 11) == 0);
  assert(memcmp(properties[1].data, "inputs:roughness", 16) == 0);
  lightusd_document_snapshot_destroy(edited);
  assert(remove(filename) == 0);
}

static void test_session_controls(const char* filename) {
  lightusd_document_options options;
  lightusd_document_options_init(&options);
  assert(offsetof(lightusd_document_options, max_resident_bytes) == 8);
  assert(options.skip_composition == 0 && options.cache_retention == 0);
  lightusd_document_session* session = NULL;
  uint8_t* controls[] = {&options.load_payloads, &options.skip_composition, &options.cache_retention};
  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
    const uint8_t previous = *controls[i];
    *controls[i] = 2;
    assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_ERR_INVALID_ARG);
    assert(!session);
    *controls[i] = previous;
  }
  const size_t filename_size = strlen(filename);
  const char suffix[] = ".payload.usda";
  char* payload = (char*)malloc(filename_size + sizeof(suffix));
  assert(payload);
  memcpy(payload, filename, filename_size);
  memcpy(payload + filename_size, suffix, sizeof(suffix));
  const char* basename = strrchr(payload, '/');
  basename = basename ? basename + 1 : payload;
  FILE* file = fopen(payload, "wb");
  assert(file);
  assert(fputs("#usda 1.0\ndef Xform \"Payload\" {\n"
               "def Xform \"Child\" { float[] values = [1,2,3,4] }\n}\n", file) >= 0);
  assert(fclose(file) == 0);
  file = fopen(filename, "wb");
  assert(file);
  assert(fprintf(file, "#usda 1.0\ndef Xform \"Root\" (prepend references = @%s@</Payload>) {}\n", basename) > 0);
  assert(fclose(file) == 0);
  uint64_t full_transient_bytes = 0;
  struct cache_callback_state callback_state = {0};
  options.progress_callback = on_cache_progress;
  options.progress_userdata = &callback_state;
  assert(!lightusd_document_session_is_open(NULL));
  assert(!lightusd_document_session_is_composed(NULL));
  assert(lightusd_document_session_trim_caches(NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_release_cache(NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_dependency_count(NULL) == 0);
  for (int mode = 0; mode < 3; ++mode) {
    options.skip_composition = mode == 2;
    options.cache_retention = mode == 1;
    assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
    callback_state.session = session;
    assert(!lightusd_document_session_is_open(session));
    assert(lightusd_document_session_trim_caches(session) == LIGHTUSD_ERR_NOT_FOUND);
    assert(lightusd_document_session_release_cache(session) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_document_memory_stats memory = {0};
    memory.struct_size = sizeof(memory);
    assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_OK);
    assert(memory.source_layer_bytes == 0 && memory.layer_count == 0);
    lightusd_stage* transferred = NULL;
    assert(lightusd_document_session_take_stage(session, &transferred) == LIGHTUSD_ERR_NOT_FOUND);
    assert(!transferred);
    lightusd_document_snapshot* snapshot = NULL;
    assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
    assert(lightusd_document_session_is_open(session));
    assert(lightusd_document_session_is_composed(session) == (mode != 2));
    assert(lightusd_document_snapshot_has_prim(snapshot, "/Root"));
    assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/Child") == (mode != 2));
    assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_OK);
    assert(memory.composed_stage_bytes > 0);
    assert(memory.estimated_total_bytes >= memory.composed_stage_bytes);
    assert(memory.peak_estimated_total_bytes >= memory.estimated_total_bytes);
    if (mode != 2) assert(memory.source_layer_bytes > 0 && memory.layer_count >= 2);
    if (mode == 0) full_transient_bytes = memory.transient_cache_bytes;
    if (mode == 1) {
      assert(memory.prim_index_count == 0 && memory.composed_prim_count == 0);
      assert(memory.transient_cache_bytes < full_transient_bytes);
    }
    if (mode != 2) {
      const size_t count = lightusd_document_session_dependency_count(session);
      assert(count >= 2);
      char** dependencies = (char**)calloc(count, sizeof(char*));
      assert(dependencies);
      for (size_t i = 0; i < count; ++i) {
        size_t required = 0;
        assert(lightusd_document_session_dependency_copy(session, i, NULL, 0, &required) == LIGHTUSD_OK);
        assert(required > 1);
        dependencies[i] = (char*)malloc(required + 1);
        assert(dependencies[i]);
        assert(lightusd_document_session_dependency_copy(session, i, dependencies[i], required, &required) == LIGHTUSD_OK);
        dependencies[i][required] = '\0';
        char short_buffer = 'x';
        assert(lightusd_document_session_dependency_copy(session, i, &short_buffer, 1, &required) != LIGHTUSD_OK);
      }
      size_t required = 123;
      assert(lightusd_document_session_dependency_copy(session, count, NULL, 0, &required) == LIGHTUSD_ERR_NOT_FOUND);
      assert(required == 0);
      assert(lightusd_document_session_dependency_copy(NULL, 0, NULL, 0, &required) == LIGHTUSD_ERR_INVALID_ARG);
      assert(lightusd_document_session_dependency_copy(session, 0, NULL, 0, NULL) == LIGHTUSD_ERR_INVALID_ARG);
      const uint64_t revision = lightusd_document_snapshot_revision(snapshot);
      assert(lightusd_document_session_trim_caches(session) == LIGHTUSD_OK);
      assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_OK);
      assert(memory.layer_count >= 2 && memory.composed_prim_count == 0);
      assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
      assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
      assert(lightusd_document_session_is_composed(session));
      assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_OK);
      assert(memory.source_layer_bytes == 0 && memory.layer_count == 0);
      assert(lightusd_document_session_dependency_count(session) == count);
      for (size_t i = 0; i < count; ++i) {
        size_t bytes = strlen(dependencies[i]) + 1;
        char* copied = (char*)malloc(bytes);
        assert(copied);
        assert(lightusd_document_session_dependency_copy(session, i, copied, bytes, &required) == LIGHTUSD_OK);
        copied[required] = '\0';
        assert(strcmp(copied, dependencies[i]) == 0);
        free(copied);
        free(dependencies[i]);
      }
      free(dependencies);
      lightusd_document_snapshot* current = NULL;
      assert(lightusd_document_session_snapshot(session, &current) == LIGHTUSD_OK);
      assert(lightusd_document_snapshot_revision(current) == revision);
      assert(lightusd_document_snapshot_has_prim(current, "/Root/Child"));
      lightusd_document_snapshot_destroy(current);
      callback_state.check_busy = 1;
      assert(lightusd_document_session_rebuild(session, &current) == LIGHTUSD_OK);
      callback_state.check_busy = 0;
      assert(callback_state.calls > 0);
      assert(lightusd_document_snapshot_has_prim(current, "/Root/Child"));
      assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/Child"));
      lightusd_document_snapshot_destroy(snapshot);
      snapshot = current;
      assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_OK);
      assert(memory.source_layer_bytes > 0);
    }
    memory.struct_size--;
    assert(lightusd_document_session_memory_stats(session, &memory) == LIGHTUSD_ERR_INVALID_ARG);
    memory.struct_size = sizeof(memory);
    assert(lightusd_document_session_memory_stats(NULL, &memory) == LIGHTUSD_ERR_INVALID_ARG);
    assert(lightusd_document_session_memory_stats(session, NULL) == LIGHTUSD_ERR_INVALID_ARG);
    assert(lightusd_document_session_take_stage(NULL, &transferred) == LIGHTUSD_ERR_INVALID_ARG);
    assert(lightusd_document_session_take_stage(session, NULL) == LIGHTUSD_ERR_INVALID_ARG);
    assert(lightusd_document_session_take_stage(session, &transferred) == LIGHTUSD_ERR_BUSY);
    assert(!transferred && lightusd_document_snapshot_has_prim(snapshot, "/Root"));
    lightusd_stage* view = NULL;
    assert(lightusd_document_snapshot_stage(NULL, &view) == LIGHTUSD_ERR_INVALID_ARG);
    assert(!view);
    assert(lightusd_document_snapshot_stage(snapshot, NULL) == LIGHTUSD_ERR_INVALID_ARG);
    assert(lightusd_document_snapshot_stage(snapshot, &view) == LIGHTUSD_OK);
    assert(lightusd_stage_is_read_only(view));
    assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(view, "/Root")));
    assert(lightusd_stage_define_prim(view, "/Forbidden", "Xform", 0, NULL) == LIGHTUSD_ERR_UNSUPPORTED);
    assert(lightusd_stage_remove_prim(view, "/Root") == LIGHTUSD_ERR_UNSUPPORTED);
    assert(lightusd_stage_set_default_prim(view, "Root") == LIGHTUSD_ERR_UNSUPPORTED);
    assert(lightusd_stage_add_sublayer_path(view, "other.usda") == LIGHTUSD_ERR_UNSUPPORTED);
    const float forbidden = 9;
    assert(lightusd_attr_set(view, "/Root", "forbidden", LIGHTUSD_TYPE_FLOAT, 0,
                            &forbidden, 1, 0) == LIGHTUSD_ERR_UNSUPPORTED);
    assert(lightusd_attr_remove(view, "/Root", "forbidden") == LIGHTUSD_ERR_UNSUPPORTED);
    lightusd_stage* editable = NULL;
    assert(lightusd_stage_flatten(view, &editable) == LIGHTUSD_OK);
    assert(!lightusd_stage_is_read_only(editable));
    assert(lightusd_stage_define_prim(editable, "/Independent", "Xform", 0, NULL) == LIGHTUSD_OK);
    assert(!lightusd_prim_is_valid(lightusd_stage_prim_at_path(view, "/Independent")));
    lightusd_stage_destroy(editable);
    lightusd_string* exported = NULL;
    assert(lightusd_stage_export_usdc(view, &exported) == LIGHTUSD_OK);
    lightusd_sv bytes = lightusd_string_view(exported);
    lightusd_stage* reloaded = NULL;
    assert(lightusd_stage_load_from_memory((const uint8_t*)bytes.data, bytes.len, NULL, &reloaded) == LIGHTUSD_OK);
    assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(reloaded, "/Root")));
    lightusd_stage_destroy(reloaded);
    lightusd_string_destroy(exported);
    lightusd_document_snapshot_destroy(snapshot);
    assert(lightusd_document_session_take_stage(session, &transferred) == LIGHTUSD_ERR_BUSY);
    assert(!transferred);
    lightusd_stage_destroy(view);
    assert(lightusd_document_session_take_stage(session, &transferred) == LIGHTUSD_OK);
    assert(transferred);
    assert(!lightusd_document_session_is_open(session));
    lightusd_stage* duplicate = NULL;
    assert(lightusd_document_session_take_stage(session, &duplicate) == LIGHTUSD_ERR_NOT_FOUND);
    assert(!duplicate);
    snapshot = NULL;
    assert(lightusd_document_session_snapshot(session, &snapshot) == LIGHTUSD_ERR_NOT_FOUND);
    assert(!snapshot);
    assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
    assert(lightusd_stage_define_prim(transferred, "/Independent", "Xform", 0, NULL) == LIGHTUSD_OK);
    assert(!lightusd_document_snapshot_has_prim(snapshot, "/Independent"));
    lightusd_document_snapshot_destroy(snapshot);
    lightusd_document_session_destroy(session);
    session = NULL;
    assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(transferred, "/Root")));
    if (mode != 2) {
      lightusd_value_view values = {0};
      assert(lightusd_attr_get(lightusd_stage_prim_at_path(transferred, "/Root/Child"),
                               "values", &values) == LIGHTUSD_OK);
      const float expected[] = {1,2,3,4};
      assert(values.nbytes == sizeof(expected) && memcmp(values.data, expected, sizeof(expected)) == 0);
    }
    lightusd_stage_destroy(transferred);
  }
  /* Without composition, unresolved external arcs must not be opened. */
  assert(remove(payload) == 0);
  options.skip_composition = 1;
  assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
  lightusd_document_snapshot* snapshot = NULL;
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  lightusd_document_snapshot_destroy(snapshot);
  lightusd_document_session_destroy(session);
  assert(remove(filename) == 0);
  free(payload);
}

static void test_parent_path_config(const char* filename) {
  /* CMake passes an absolute fixture path. Insert .. at the filesystem
   * root, so the target stays valid even when the build directory is a symlink. */
  const char* root_slash = strchr(filename, '/');
  assert(root_slash);
  const size_t length = strlen(filename);
  char* payload = (char*)malloc(length + sizeof(".payload.usda"));
  assert(payload);
  memcpy(payload, filename, length);
  memcpy(payload + length, ".payload.usda", sizeof(".payload.usda"));
  FILE* file = fopen(payload, "wb");
  assert(file);
  assert(fputs("#usda 1.0\ndef Xform \"Source\" { def Xform \"Child\" {} }\n", file) >= 0);
  assert(fclose(file) == 0);
  file = fopen(filename, "wb");
  assert(file);
  assert(fprintf(file, "#usda 1.0\ndef Xform \"Root\" (prepend references = @%.*s/../%s.payload.usda@</Source>) {}\n",
                 (int)(root_slash - filename), filename, root_slash + 1) > 0);
  assert(fclose(file) == 0);
  for (uint32_t mode = 0; mode < 3; ++mode) {
    lightusd_document_session* session = NULL;
    lightusd_document_snapshot* snapshot = NULL;
    lightusd_document_open_config config;
    lightusd_document_open_config_init(&config);
    config.input_policy = mode == 0 ? LIGHTUSD_INPUT_UNTRUSTED : LIGHTUSD_INPUT_TRUSTED;
    config.flags = mode == 2 ? LIGHTUSD_DOCUMENT_REJECT_PARENT_PATHS : 0;
    assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
    assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_OK);
    assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_has_prim(snapshot, "/Root"));
    assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/Child") == (mode == 1));
    lightusd_document_snapshot_destroy(snapshot);
    assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
    assert(lightusd_document_session_rebuild(session, &snapshot) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/Child") == (mode == 1));
    lightusd_document_snapshot_destroy(snapshot);
    lightusd_document_session_destroy(session);
  }
  assert(remove(filename) == 0);
  assert(remove(payload) == 0);
  free(payload);
}

static void test_open_config(const char* filename) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fputs("#usda 1.0\ndef Xform \"Root\" (\n"
      " variants = { string look = \"low\" }\n prepend variantSets = \"look\"\n) {\n"
      " variantSet \"look\" = {\n"
      "  \"low\" { def Xform \"Low\" {} }\n"
      "  \"high\" { def Xform \"High\" {} }\n }\n}\n", file) >= 0);
  assert(fclose(file) == 0);
  lightusd_document_session* session = NULL;
  lightusd_document_snapshot* snapshot = NULL;
  assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
  lightusd_document_open_config config;
  lightusd_document_open_config_init(&config);
  assert(config.flags == 0 && config.variant_count == 0 && config.opinion_batch_size == 0);
  assert(lightusd_document_session_configure_open(NULL, &config) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_configure_open(session, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  char choice[] = "high";
  lightusd_document_variant_selection variants[] = {{"/Root", "look", choice}, {"/Root", "look", "low"}};
  config.variants = variants;
  config.variant_count = 1;
  config.opinion_batch_size = 1;
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_OK);
  /* Copied configuration survives caller mutation and rejected replacement. */
  choice[0] = 'X';
  config.variant_count = 2;
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_ERR_INVALID_ARG);
  config.variant_count = SIZE_MAX;
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_ERR_OVERFLOW);
  config.variant_count = 0;
  config.flags = 4;
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_ERR_INVALID_ARG);
  config.flags = 0;
  config.input_policy = 2;
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_ERR_INVALID_ARG);
  config.input_policy = LIGHTUSD_INPUT_UNTRUSTED;
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/High"));
  assert(!lightusd_document_snapshot_has_prim(snapshot, "/Root/Low"));
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_ERR_BUSY);
  lightusd_document_snapshot_destroy(snapshot);
  assert(lightusd_document_session_reload_layer(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/High"));
  lightusd_document_snapshot_destroy(snapshot);
  lightusd_stage* stage = NULL;
  assert(lightusd_document_session_take_stage(session, &stage) == LIGHTUSD_OK);
  lightusd_stage_destroy(stage);
  /* A closed session can be reconfigured, including clearing initial variants. */
  assert(lightusd_document_session_configure_open(session, &config) == LIGHTUSD_OK);
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(snapshot, "/Root/Low"));
  assert(!lightusd_document_snapshot_has_prim(snapshot, "/Root/High"));
  lightusd_document_snapshot_destroy(snapshot);
  lightusd_document_session_destroy(session);
  assert(remove(filename) == 0);
}

struct payload_callback_state {
  const char* allowed;
  unsigned policy_calls;
  unsigned selected_calls;
};

static int select_payload(void* userdata, lightusd_sv path, lightusd_sv asset) {
  struct payload_callback_state* state = (struct payload_callback_state*)userdata;
  assert(path.data && path.len > 0);
  /* This fixture authors internal payloads. */
  assert(asset.len == 0);
  ++state->policy_calls;
  return strlen(state->allowed) == path.len &&
         memcmp(state->allowed, path.data, path.len) == 0;
}

static void payload_selected(void* userdata, lightusd_sv path) {
  struct payload_callback_state* state = (struct payload_callback_state*)userdata;
  assert(path.data && path.len > 0);
  ++state->selected_calls;
}

static void test_payload_callbacks(const char* filename) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fputs("#usda 1.0\n"
      "def Xform \"Source\" { def Xform \"Child\" {} }\n"
      "def Xform \"A\" (prepend payload = </Source>) {}\n"
      "def Xform \"B\" (prepend payload = </Source>) {}\n", file) >= 0);
  assert(fclose(file) == 0);
  assert(lightusd_document_session_set_payload_callbacks(NULL, NULL, NULL, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  for (int load_all = 0; load_all <= 1; ++load_all) {
    lightusd_document_options options;
    lightusd_document_options_init(&options);
    options.load_payloads = (uint8_t)load_all;
    options.max_threads = 1; /* Counters in this fixture are deliberately serial. */
    lightusd_document_session* session = NULL;
    lightusd_document_snapshot *first = NULL, *edited = NULL;
    struct payload_callback_state state = {"/A", 0, 0};
    assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
    assert(lightusd_document_session_set_payload_callbacks(session, select_payload, payload_selected, &state) == LIGHTUSD_OK);
    assert(lightusd_document_session_open_file(session, filename, &first) == LIGHTUSD_OK);
    assert(state.policy_calls > 0 && state.selected_calls > 0);
    assert(lightusd_document_snapshot_has_prim(first, "/A/Child"));
    assert(!lightusd_document_snapshot_has_prim(first, "/B/Child"));
    lightusd_strlist *dependencies = NULL, *deferred = NULL;
    assert(lightusd_document_session_dependencies(session, &dependencies) == LIGHTUSD_OK);
    assert(lightusd_strlist_size(dependencies) == lightusd_document_session_dependency_count(session));
    assert(lightusd_strlist_size(dependencies) > 0);
    assert(lightusd_document_session_deferred_payloads(session, &deferred) == LIGHTUSD_OK);
    assert(lightusd_strlist_size(deferred) == 1);
    struct payload_callback_state replacement = {"/B", 0, 0};
    const unsigned old_calls = state.policy_calls;
    assert(lightusd_document_session_set_payload_callbacks(session, select_payload, payload_selected, &replacement) == LIGHTUSD_OK);
    assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
    assert(state.policy_calls == old_calls);
    assert(replacement.policy_calls > 0 && replacement.selected_calls > 0);
    assert(!lightusd_document_snapshot_has_prim(edited, "/A/Child"));
    assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
    lightusd_document_snapshot_destroy(edited);
    assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
    assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
    assert(!lightusd_document_snapshot_has_prim(edited, "/A/Child"));
    assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
    lightusd_document_snapshot_destroy(edited);
    assert(lightusd_document_session_reload_layer(session, filename, &edited) == LIGHTUSD_OK);
    assert(!lightusd_document_snapshot_has_prim(edited, "/A/Child"));
    assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
    lightusd_document_snapshot_destroy(edited);
    /* Explicit load/unload rules override the callback. */
    assert(lightusd_document_session_load_payload(session, "/A", &edited) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_has_prim(edited, "/A/Child"));
    lightusd_document_snapshot_destroy(edited);
    assert(lightusd_document_session_unload_payload(session, "/B", &edited) == LIGHTUSD_OK);
    assert(!lightusd_document_snapshot_has_prim(edited, "/B/Child"));
    lightusd_document_snapshot_destroy(edited);
    /* Clear callbacks, reopen to clear explicit rules, and recover the default. */
    const unsigned replaced_calls = replacement.policy_calls;
    const unsigned replaced_selected = replacement.selected_calls;
    assert(lightusd_document_session_set_payload_callbacks(session, NULL, NULL, NULL) == LIGHTUSD_OK);
    assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_has_prim(edited, "/A/Child"));
    assert(!lightusd_document_snapshot_has_prim(edited, "/B/Child"));
    lightusd_document_snapshot_destroy(edited);
    assert(lightusd_document_session_open_file(session, filename, &edited) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_has_prim(edited, "/A/Child") == load_all);
    assert(lightusd_document_snapshot_has_prim(edited, "/B/Child") == load_all);
    assert(replacement.policy_calls == replaced_calls);
    assert(replacement.selected_calls == replaced_selected);
    assert(lightusd_document_snapshot_has_prim(first, "/A/Child"));
    lightusd_document_snapshot_destroy(edited);
    lightusd_document_snapshot_destroy(first);
    lightusd_document_session_destroy(session);
    /* List storage is independent of edits and session destruction. */
    assert(lightusd_strlist_size(dependencies) > 0);
    lightusd_sv deferred_path = lightusd_strlist_get(deferred, 0);
    assert(deferred_path.len == 2 && memcmp(deferred_path.data, "/B", 2) == 0);
    lightusd_strlist_destroy(dependencies);
    lightusd_strlist_destroy(deferred);
    assert(lightusd_document_session_dependencies(NULL, &dependencies) == LIGHTUSD_ERR_INVALID_ARG);
    assert(!dependencies);
    assert(lightusd_document_session_deferred_payloads(NULL, &deferred) == LIGHTUSD_ERR_INVALID_ARG);
    assert(!deferred);
  }
  assert(remove(filename) == 0);
}

static void test_batch_edits(const char* filename) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fputs("#usda 1.0\n"
      "def Xform \"Source\" { def Xform \"Child\" {} }\n"
      "def Xform \"A\" (\n prepend payload = </Source>\n"
      " variants = { string look = \"low\" }\n prepend variantSets = \"look\"\n) {\n"
      " variantSet \"look\" = {\n"
      "  \"low\" { def Xform \"Low\" {} }\n"
      "  \"high\" { def Xform \"High\" {} }\n }\n}\n"
      "def Xform \"B\" (\n prepend payload = </Source>\n"
      " variants = { string look = \"low\" }\n prepend variantSets = \"look\"\n) {\n"
      " variantSet \"look\" = {\n"
      "  \"low\" { def Xform \"Low\" {} }\n"
      "  \"high\" { def Xform \"High\" {} }\n }\n}\n", file) >= 0);
  assert(fclose(file) == 0);
  int cancel = 0;
  lightusd_document_options options;
  lightusd_document_options_init(&options);
  options.load_payloads = 0;
  options.progress_callback = on_progress;
  options.progress_userdata = &cancel;
  lightusd_document_session* session = NULL;
  assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
  lightusd_document_snapshot* first = NULL;
  assert(lightusd_document_session_open_file(session, filename, &first) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(first, "/A/Low"));
  assert(lightusd_document_snapshot_has_prim(first, "/B/Low"));
  assert(!lightusd_document_snapshot_has_prim(first, "/A/Child"));
  assert(!lightusd_document_snapshot_has_prim(first, "/B/Child"));
  const uint64_t initial_revision = lightusd_document_snapshot_revision(first);
  lightusd_stage* retained_view = NULL;
  assert(lightusd_document_snapshot_stage(first, &retained_view) == LIGHTUSD_OK);
  const lightusd_prim retained_low = lightusd_stage_prim_at_path(retained_view, "/A/Low");
  assert(lightusd_prim_is_valid(retained_low));
  lightusd_document_variant_selection selections[] = {
      {"/A", "look", "high"}, {"/B", "look", "high"}};
  lightusd_document_snapshot* edited = NULL;
  assert(lightusd_document_session_set_variants(session, NULL, 1, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_set_variants(session, selections, SIZE_MAX, &edited) == LIGHTUSD_ERR_OVERFLOW);
  selections[1].prim_path = "/A";
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  selections[1].prim_path = "relative";
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  selections[1].prim_path = "/B";
  selections[1].selection = "";
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  selections[1].selection = "high";
  cancel = 1;
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_ERR_CANCELLED);
  assert(!edited);
  cancel = 0;
  assert(lightusd_document_session_snapshot(session, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(edited) == initial_revision);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Low"));
  lightusd_document_snapshot_destroy(edited);
  /* Rebuild checks rollback of override state as well as publication. */
  assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Low"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Low"));
  uint64_t revision = lightusd_document_snapshot_revision(edited);
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(edited) == revision + 1);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/High"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/High"));
  assert(!lightusd_document_snapshot_has_prim(edited, "/A/Low"));
  lightusd_document_snapshot_destroy(edited);
  /* Replacement drops B's override; an empty batch drops A's too. */
  assert(lightusd_document_session_set_variants(session, selections, 1, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/High"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Low"));
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_set_variants(session, NULL, 0, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Low"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Low"));
  revision = lightusd_document_snapshot_revision(edited);
  lightusd_document_snapshot_destroy(edited);
  const char* paths[] = {"/A", "/B"};
  assert(lightusd_document_session_load_payloads(session, NULL, 1, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_load_payloads(session, paths, SIZE_MAX, &edited) == LIGHTUSD_ERR_OVERFLOW);
  paths[1] = "/B.attr";
  assert(lightusd_document_session_load_payloads(session, paths, 2, &edited) == LIGHTUSD_ERR_INVALID_ARG);
  paths[1] = "/B";
  cancel = 1;
  assert(lightusd_document_session_load_payloads(session, paths, 2, &edited) == LIGHTUSD_ERR_CANCELLED);
  assert(!edited);
  cancel = 0;
  assert(lightusd_document_session_snapshot(session, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(edited) == revision);
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
  assert(!lightusd_document_snapshot_has_prim(edited, "/A/Child"));
  assert(!lightusd_document_snapshot_has_prim(edited, "/B/Child"));
  revision = lightusd_document_snapshot_revision(edited);
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_load_payloads(session, paths, 2, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(edited) == revision + 1);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Child"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_load_payloads(session, NULL, 0, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Child"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
  lightusd_document_snapshot_destroy(edited);
  /* Cache retirement must retain both the payload rules and variant overrides. */
  assert(lightusd_document_session_set_variants(session, selections, 2, &edited) == LIGHTUSD_OK);
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
  assert(lightusd_document_session_rebuild(session, &edited) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_has_prim(edited, "/A/High"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/High"));
  assert(lightusd_document_snapshot_has_prim(edited, "/A/Child"));
  assert(lightusd_document_snapshot_has_prim(edited, "/B/Child"));
  lightusd_document_snapshot_destroy(edited);
  assert(lightusd_document_snapshot_has_prim(first, "/A/Low"));
  assert(!lightusd_document_snapshot_has_prim(first, "/A/Child"));
  lightusd_document_snapshot_destroy(first);
  lightusd_document_session_destroy(session);
  assert(lightusd_prim_is_valid(retained_low));
  assert(!lightusd_prim_is_valid(lightusd_stage_prim_at_path(retained_view, "/A/High")));
  lightusd_stage_retain(retained_view);
  lightusd_stage_destroy(retained_view);
  assert(lightusd_stage_prim_count(retained_view) > 0);
  lightusd_stage_destroy(retained_view);
  assert(remove(filename) == 0);
}

struct preview_state {
  lightusd_stage* stages[2];
  int calls[2];
  int cancel_phase;
};

static int on_preview(void* userdata, const lightusd_document_preview* preview) {
  struct preview_state* state = (struct preview_state*)userdata;
  assert(preview && preview->phase < 2 && preview->stage);
  assert(lightusd_stage_is_read_only(preview->stage));
  assert(preview->flags == (preview->phase == 0 ? 0u : 2u));
  lightusd_prim mesh = lightusd_stage_prim_at_path(preview->stage, "/M");
  assert(lightusd_prim_is_valid(mesh));
  lightusd_value_view extent = {0};
  assert(lightusd_attr_get(mesh, "extent", &extent) == LIGHTUSD_OK);
  assert(extent.nbytes == 6 * sizeof(float));
  lightusd_value_view expensive = {0};
  assert((lightusd_attr_get(mesh, "expensive", &expensive) == LIGHTUSD_OK) == (preview->phase == 0));
  assert(lightusd_stage_remove_prim(preview->stage, "/M") == LIGHTUSD_ERR_UNSUPPORTED);
  lightusd_stage_destroy(state->stages[preview->phase]);
  lightusd_stage_retain(preview->stage);
  state->stages[preview->phase] = preview->stage;
  ++state->calls[preview->phase];
  return state->cancel_phase != (int)preview->phase;
}

static void test_previews(const char* filename) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fputs("#usda 1.0\ndef Xform \"Source\" {}\n"
               "def Mesh \"M\" (prepend references = </Source>) {\n"
               "float3[] extent = [(-1,-2,-3), (1,2,3)]\n int expensive = 7\n}\n", file) >= 0);
  assert(fclose(file) == 0);
  struct preview_state state = {{NULL, NULL}, {0, 0}, -1};
  assert(lightusd_document_session_set_preview_callback(NULL, on_preview, &state) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_document_session* session = NULL;
  assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_set_preview_callback(session, on_preview, &state) == LIGHTUSD_OK);
  lightusd_document_snapshot* snapshot = NULL;
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(state.calls[0] == 1 && state.calls[1] == 1);
  uint64_t revision = lightusd_document_snapshot_revision(snapshot);
  lightusd_document_snapshot_destroy(snapshot);
  for (int phase = 0; phase < 2; ++phase) {
    state.cancel_phase = phase;
    snapshot = NULL;
    assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_ERR_CANCELLED);
    assert(!snapshot);
    assert(lightusd_document_session_snapshot(session, &snapshot) == LIGHTUSD_OK);
    assert(lightusd_document_snapshot_revision(snapshot) == revision);
    assert(lightusd_document_snapshot_has_prim(snapshot, "/M"));
    lightusd_document_snapshot_destroy(snapshot);
  }
  assert(state.calls[0] == 3 && state.calls[1] == 2);
  state.cancel_phase = -1;
  assert(lightusd_document_session_reload_layer(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(state.calls[0] == 4 && state.calls[1] == 3);
  lightusd_document_snapshot_destroy(snapshot);
  assert(lightusd_document_session_set_preview_callback(session, NULL, NULL) == LIGHTUSD_OK);
  assert(lightusd_document_session_reload_layer(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(state.calls[0] == 4 && state.calls[1] == 3);
  lightusd_document_snapshot_destroy(snapshot);
  /* Provisional stages are separate from the published stage and do not block transfer. */
  lightusd_stage* published = NULL;
  assert(lightusd_document_session_take_stage(session, &published) == LIGHTUSD_OK);
  lightusd_stage_destroy(published);
  lightusd_document_session_destroy(session);
  for (int phase = 0; phase < 2; ++phase) {
    lightusd_prim mesh = lightusd_stage_prim_at_path(state.stages[phase], "/M");
    assert(lightusd_prim_is_valid(mesh));
    lightusd_value_view extent = {0};
    assert(lightusd_attr_get(mesh, "extent", &extent) == LIGHTUSD_OK);
    const float expected[] = {-1,-2,-3,1,2,3};
    assert(extent.nbytes == sizeof(expected) && memcmp(extent.data, expected, sizeof(expected)) == 0);
    lightusd_stage_destroy(state.stages[phase]);
    state.stages[phase] = NULL;
  }
  state.calls[0] = state.calls[1] = 0;
  lightusd_document_options options;
  lightusd_document_options_init(&options);
  options.skip_composition = 1;
  assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_set_preview_callback(session, on_preview, &state) == LIGHTUSD_OK);
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(state.calls[0] == 1 && state.calls[1] == 0);
  lightusd_document_snapshot_destroy(snapshot);
  lightusd_document_session_destroy(session);
  lightusd_stage_destroy(state.stages[0]);
  assert(remove(filename) == 0);
}

static void test_geometry_release(const char* filename) {
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fputs("#usda 1.0\ndef Xform \"Source\" {}\n"
      "def Mesh \"M\" (prepend references = </Source>) {\n"
      " point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\n"
      " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0,1,2]\n"
      " string label = \"retained\"\n}\n"
      "def Mesh \"N\" { point3f[] points = [(0,0,0),(1,0,0),(0,1,0)] }\n"
      "def Mesh \"A\" {\n point3f[] points = [(0,0,0)]\n"
      " point3f[] points.timeSamples = {1: [(0,0,0)], 2: [(1,0,0)]}\n}\n", file) >= 0);
  assert(fclose(file) == 0);
  lightusd_document_session* session = NULL;
  lightusd_geometry_release_stats stats = {0};
  assert(sizeof(stats) == 40);
  assert(lightusd_document_session_release_geometry(NULL, NULL, 1, &stats) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_release_geometry(session, NULL, 1, &stats) == LIGHTUSD_ERR_NOT_FOUND);
  lightusd_document_snapshot* first = NULL;
  assert(lightusd_document_session_open_file(session, filename, &first) == LIGHTUSD_OK);
  const uint64_t revision = lightusd_document_snapshot_revision(first);
  lightusd_stage* retained = NULL;
  assert(lightusd_document_snapshot_stage(first, &retained) == LIGHTUSD_OK);
  assert(lightusd_document_session_release_geometry(session, "/M", 1, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  stats.property_count = 123;
  const lightusd_geometry_release_stats saved = stats;
  const char* invalid[] = {"/Missing", "/M.points", "relative", "/"};
  for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
    assert(lightusd_document_session_release_geometry(session, invalid[i], 1, &stats) == LIGHTUSD_ERR_INVALID_ARG);
    assert(memcmp(&stats, &saved, sizeof(stats)) == 0);
  }
  assert(lightusd_document_session_release_geometry(session, "/M", 4, &stats) == LIGHTUSD_OK);
  assert(stats.property_count == 0);
  assert(lightusd_document_session_release_geometry(session, "/M", 1, &stats) == LIGHTUSD_OK);
  assert(stats.property_count == 3 && stats.element_count == 7 && stats.estimated_payload_bytes > 0);
  assert(stats.stage_bytes_before == 0 && stats.stage_bytes_after == 0);
  lightusd_value_view value = {0};
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(retained, "/M"), "points", &value) == LIGHTUSD_OK);
  assert(value.nbytes == 9 * sizeof(float));
  lightusd_document_snapshot* compact = NULL;
  assert(lightusd_document_session_snapshot(session, &compact) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(compact) == revision);
  lightusd_stage* current = NULL;
  assert(lightusd_document_snapshot_stage(compact, &current) == LIGHTUSD_OK);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(current, "/M"), "points", &value) == LIGHTUSD_ERR_NOT_FOUND);
  lightusd_sv label = {0};
  assert(lightusd_attr_get_string(lightusd_stage_prim_at_path(current, "/M"), "label", &label) == LIGHTUSD_OK);
  assert(label.len == 8 && memcmp(label.data, "retained", 8) == 0);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(current, "/N"), "points", &value) == LIGHTUSD_OK);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(current, "/A"), "points", &value) == LIGHTUSD_OK);
  lightusd_stage_destroy(current);
  lightusd_document_snapshot_destroy(compact);
  assert(lightusd_document_session_release_geometry(session, "/M", 1, &stats) == LIGHTUSD_OK);
  assert(stats.property_count == 0);
  assert(lightusd_document_session_release_geometry(session, NULL, 1, &stats) == LIGHTUSD_OK);
  assert(stats.property_count == 1 && stats.element_count == 3);
  assert(stats.stage_bytes_after < stats.stage_bytes_before);
  assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
  assert(lightusd_document_session_rebuild(session, &compact) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_stage(compact, &current) == LIGHTUSD_OK);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(current, "/M"), "points", &value) == LIGHTUSD_OK);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(current, "/N"), "points", &value) == LIGHTUSD_OK);
  lightusd_stage_destroy(current);
  lightusd_document_snapshot_destroy(compact);
  lightusd_document_snapshot_destroy(first);
  lightusd_document_session_destroy(session);
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(retained, "/M"), "points", &value) == LIGHTUSD_OK);
  lightusd_stage_destroy(retained);
  lightusd_document_options options;
  lightusd_document_options_init(&options);
  options.skip_composition = 1;
  assert(lightusd_document_session_create(&options, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_open_file(session, filename, &first) == LIGHTUSD_OK);
  assert(lightusd_document_session_release_geometry(session, NULL, 1, &stats) == LIGHTUSD_ERR_UNSUPPORTED);
  lightusd_document_snapshot_destroy(first);
  lightusd_document_session_destroy(session);
  assert(remove(filename) == 0);
}

static void test_aggregated_render_prepare(const char* filename) {
  lightusd_document_session* document = NULL;
  lightusd_document_snapshot *first = NULL, *middle = NULL, *last = NULL;
  lightusd_render_session* renderer = NULL;
  lightusd_render_scene* scene = NULL;
  assert(lightusd_document_session_create(NULL, &document) == LIGHTUSD_OK);
  assert(lightusd_document_session_open_file(document, filename, &first) == LIGHTUSD_OK);
  assert(lightusd_render_session_create_document(document, NULL, &renderer) == LIGHTUSD_OK);
  assert(lightusd_render_session_apply_document(renderer, first, &scene, NULL) == LIGHTUSD_OK);
  lightusd_render_scene_destroy(scene);
  const uint64_t first_revision = lightusd_document_snapshot_revision(first);
  assert(lightusd_document_session_reload_layer(document, filename, &middle) == LIGHTUSD_OK);
  assert(lightusd_document_session_reload_layer(document, filename, &last) == LIGHTUSD_OK);
  const uint64_t target_revision = lightusd_document_snapshot_revision(last);
  assert(target_revision > first_revision + 1);
  lightusd_render_change_set changes;
  lightusd_render_change_set_init(&changes);
  changes.base_revision = first_revision;
  changes.full_resync = 1;
  lightusd_render_prepared_update* prepared = NULL;
  lightusd_render_update_info info;
  lightusd_render_update_info_init(&info);
  assert(lightusd_render_session_prepare_document_changes(renderer, last, NULL, &prepared, &info) == LIGHTUSD_ERR_INVALID_ARG);
  assert(!prepared);
  lightusd_prim_change prim = {0};
  prim.prim_path = "/cube0";
  changes.prims = &prim;
  changes.prim_count = SIZE_MAX;
  assert(lightusd_render_session_prepare_document_changes(renderer, last, &changes, &prepared, &info) == LIGHTUSD_ERR_OVERFLOW);
  assert(!prepared);
  changes.prim_count = 1;
  lightusd_sv property = {"points", 6};
  prim.properties = &property;
  prim.property_count = SIZE_MAX;
  assert(lightusd_render_session_prepare_document_changes(renderer, last, &changes, &prepared, &info) == LIGHTUSD_ERR_OVERFLOW);
  assert(!prepared);
  changes.prims = NULL;
  changes.prim_count = 0;
  assert(lightusd_render_session_prepare_document_changes(renderer, last, &changes, &prepared, &info) == LIGHTUSD_OK);
  assert(prepared && info.revision == target_revision && info.full_resync);
  assert(lightusd_render_session_revision(renderer) == first_revision);
  lightusd_render_session_abort(renderer, prepared);
  prepared = NULL;
  assert(lightusd_render_session_prepare_document_changes(renderer, last, &changes, &prepared, &info) == LIGHTUSD_OK);
  assert(lightusd_render_session_commit(renderer, prepared, &scene, &info) == LIGHTUSD_OK);
  assert(info.revision == target_revision && lightusd_render_session_revision(renderer) == target_revision);
  assert(lightusd_render_count(scene, LIGHTUSD_RENDER_MESH) > 0);
  lightusd_render_scene_destroy(scene);
  lightusd_render_session_destroy(renderer);
  lightusd_document_snapshot_destroy(first);
  lightusd_document_snapshot_destroy(middle);
  lightusd_document_snapshot_destroy(last);
  lightusd_document_session_destroy(document);
}

static void test_composition_issues(const char* filename) {
  const char* basename = strrchr(filename, '/');
  basename = basename ? basename + 1 : filename;
  FILE* file = fopen(filename, "wb");
  assert(file);
  assert(fprintf(file, "#usda 1.0\n(subLayers = [@%s@])\ndef Xform \"Root\" {}\n",
                 basename) > 0);
  assert(fclose(file) == 0);
  lightusd_document_session* session = NULL;
  assert(lightusd_document_session_create(NULL, &session) == LIGHTUSD_OK);
  assert(lightusd_document_session_composition_issue_count(NULL) == 0);
  assert(lightusd_document_session_composition_issue_count(session) == 0);
  lightusd_document_snapshot* snapshot = NULL;
  assert(lightusd_document_session_open_file(session, filename, &snapshot) == LIGHTUSD_OK);
  assert(snapshot);
  const size_t count = lightusd_document_session_composition_issue_count(session);
  assert(count > 0);
  int saw_cycle = 0;
  for (size_t i = 0; i < count; ++i) {
    lightusd_document_composition_issue issue = {0};
    issue.struct_size = sizeof(issue);
    assert(lightusd_document_session_composition_issue_copy(
        session, i, &issue, NULL, 0, NULL, 0) == LIGHTUSD_OK);
    assert(issue.site_bytes > 0 && issue.message_bytes > 0);
    char* site = (char*)malloc(issue.site_bytes + 1);
    char* message = (char*)malloc(issue.message_bytes + 1);
    assert(site && message);
    const size_t site_bytes = issue.site_bytes;
    const size_t message_bytes = issue.message_bytes;
    char untouched = 'x';
    assert(lightusd_document_session_composition_issue_copy(
        session, i, &issue, &untouched, 0, NULL, 0) == LIGHTUSD_ERR_INVALID_ARG);
    assert(untouched == 'x');
    assert(lightusd_document_session_composition_issue_copy(
        session, i, &issue, site, site_bytes, message, message_bytes) == LIGHTUSD_OK);
    site[site_bytes] = '\0';
    message[message_bytes] = '\0';
    if (issue.code == LIGHTUSD_COMPOSITION_SUBLAYER_CYCLE) {
      assert(strstr(message, "cycle") || strstr(message, "Cycle"));
      saw_cycle = 1;
    }
    free(site);
    free(message);
  }
  assert(saw_cycle);
  lightusd_document_composition_issue issue = {0};
  issue.struct_size = sizeof(issue);
  assert(lightusd_document_session_composition_issue_copy(
      session, count, &issue, NULL, 0, NULL, 0) == LIGHTUSD_ERR_NOT_FOUND);
  assert(lightusd_document_session_release_cache(session) == LIGHTUSD_OK);
  assert(lightusd_document_session_composition_issue_count(session) == count);
  assert(lightusd_document_session_composition_issue_copy(
      session, 0, &issue, NULL, 0, NULL, 0) == LIGHTUSD_OK);
  assert(issue.site_bytes > 0 && issue.message_bytes > 0);
  lightusd_document_snapshot_destroy(snapshot);
  lightusd_document_session_destroy(session);
  assert(remove(filename) == 0);
}

int main(int argc, char** argv) {
  assert(argc == 3);
  test_aggregated_render_prepare(argv[1]);
  test_change_records(argv[2]);
  test_session_controls(argv[2]);
  test_composition_issues(argv[2]);
  test_batch_edits(argv[2]);
  test_payload_callbacks(argv[2]);
  test_open_config(argv[2]);
  test_parent_path_config(argv[2]);
  test_previews(argv[2]);
  test_geometry_release(argv[2]);
  int cancel = 0;
  lightusd_document_options options;
  lightusd_document_options_init(&options);
  options.progress_callback = on_progress;
  options.progress_userdata = &cancel;
  lightusd_document_session* document = NULL;
  assert(lightusd_document_session_create(&options, &document) == LIGHTUSD_OK);
  lightusd_render_session* unopened_renderer = NULL;
  assert(lightusd_render_session_create_document(document, NULL,
                                                 &unopened_renderer) ==
         LIGHTUSD_ERR_NOT_FOUND);
  assert(!unopened_renderer);

  lightusd_document_snapshot* first = NULL;
  assert(lightusd_document_session_open_file(document, argv[1], &first) ==
         LIGHTUSD_OK);
  assert(first && lightusd_document_snapshot_revision(first) != 0);
  lightusd_stage* first_view = NULL;
  assert(lightusd_document_snapshot_stage(first, &first_view) == LIGHTUSD_OK);
  lightusd_render_scene* view_scene = NULL;
  assert(lightusd_render_convert(first_view, NULL, &view_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(view_scene, LIGHTUSD_RENDER_MESH) > 0);
  lightusd_render_scene_destroy(view_scene);
  lightusd_render_session* view_renderer = NULL;
  assert(lightusd_render_session_create(first_view, NULL, &view_renderer) == LIGHTUSD_OK);
  view_scene = NULL;
  assert(lightusd_render_session_update(view_renderer, first_view, &view_scene, NULL) == LIGHTUSD_OK);
  assert(lightusd_render_count(view_scene, LIGHTUSD_RENDER_MESH) > 0);
  lightusd_stage_destroy(first_view);
  assert(lightusd_render_count(view_scene, LIGHTUSD_RENDER_MESH) > 0);
  lightusd_render_scene_destroy(view_scene);
  lightusd_render_session_destroy(view_renderer);
  assert(lightusd_document_snapshot_has_prim(first, "/cube0"));
  assert(lightusd_document_snapshot_full_resync(first));
  assert(lightusd_document_snapshot_change_count(first) != 0);
  lightusd_sv changed_path = {0};
  uint32_t flags = 0;
  assert(lightusd_document_snapshot_change(first, 0, &changed_path, &flags) ==
         LIGHTUSD_OK);
  assert(changed_path.data && changed_path.len && flags);

  size_t root_bytes = 0;
  assert(lightusd_document_session_root_identifier_copy(document, NULL, 0,
                                                       &root_bytes) == LIGHTUSD_OK);
  assert(root_bytes != 0);
  char* root = (char*)malloc(root_bytes + 1);
  assert(root);
  assert(lightusd_document_session_root_identifier_copy(
             document, root, root_bytes, &root_bytes) == LIGHTUSD_OK);
  root[root_bytes] = '\0';

  lightusd_render_session* renderer = NULL;
  assert(lightusd_render_session_create_document(document, NULL, &renderer) ==
         LIGHTUSD_OK);
  lightusd_render_scene* scene = NULL;
  lightusd_render_update_info info;
  lightusd_render_update_info_init(&info);
  assert(lightusd_render_session_apply_document(renderer, first, &scene,
                                                &info) == LIGHTUSD_OK);
  assert(info.revision == lightusd_document_snapshot_revision(first));
  lightusd_render_scene_destroy(scene);

  lightusd_document_snapshot* second = NULL;
  assert(lightusd_document_session_reload_layer(document, root, &second) ==
         LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(second) >
         lightusd_document_snapshot_revision(first));
  scene = NULL;
  lightusd_render_update_info_init(&info);
  lightusd_render_prepared_update* prepared = NULL;
  assert(lightusd_render_session_prepare_document(renderer, second, &prepared,
                                                  &info) == LIGHTUSD_OK);
  assert(prepared);
  lightusd_render_session_abort(renderer, prepared);
  assert(lightusd_render_session_revision(renderer) ==
         lightusd_document_snapshot_revision(first));
  prepared = NULL;
  assert(lightusd_render_session_prepare_document(renderer, second, &prepared,
                                                  &info) == LIGHTUSD_OK);
  assert(lightusd_render_session_commit(renderer, prepared, &scene,
                                        &info) == LIGHTUSD_OK);
  assert(lightusd_render_session_revision(renderer) ==
         lightusd_document_snapshot_revision(second));
  lightusd_render_scene_destroy(scene);
  scene = NULL;
  assert(lightusd_render_session_apply_document(renderer, first, &scene,
                                                NULL) == LIGHTUSD_ERR_STALE_REVISION);
  assert(!scene);

  cancel = 1;
  lightusd_document_session* cancelled_document = NULL;
  assert(lightusd_document_session_create(&options, &cancelled_document) ==
         LIGHTUSD_OK);
  lightusd_document_snapshot* cancelled = NULL;
  assert(lightusd_document_session_open_file(cancelled_document, argv[1],
                                             &cancelled) == LIGHTUSD_ERR_CANCELLED);
  assert(!cancelled);
  lightusd_document_session_destroy(cancelled_document);
  lightusd_document_snapshot* current = NULL;
  assert(lightusd_document_session_snapshot(document, &current) == LIGHTUSD_OK);
  assert(lightusd_document_snapshot_revision(current) ==
         lightusd_document_snapshot_revision(second));
  lightusd_document_snapshot_destroy(current);
  lightusd_render_session* retained_renderer = NULL;
  assert(lightusd_render_session_create_document(document, NULL,
                                                 &retained_renderer) ==
         LIGHTUSD_OK);
  lightusd_document_session_destroy(document);
  assert(lightusd_document_snapshot_has_prim(first, "/cube0"));
  assert(lightusd_document_snapshot_has_prim(second, "/cube0"));
  scene = NULL;
  assert(lightusd_render_session_apply_document(retained_renderer, first,
                                                &scene, NULL) == LIGHTUSD_OK);
  lightusd_render_scene_destroy(scene);
  lightusd_render_session_destroy(retained_renderer);

  lightusd_render_session_destroy(renderer);
  lightusd_document_snapshot_destroy(second);
  lightusd_document_snapshot_destroy(first);
  free(root);
  return 0;
}
