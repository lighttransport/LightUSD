/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lightusd-c.h"
#include "lightusd-render-c.h"

struct event_state {
  int begin;
  int upsert;
  int remove;
  int end;
  int abort;
  int reject_begin;
  uint64_t last_id;
  uint64_t mesh_id;
  lightusd_render_scene* candidate_scene;
  const float* expected_mesh_points;
  size_t expected_mesh_bytes;
  int candidate_mesh_reads;
  uint8_t first_sequence[16];
  size_t first_sequence_count;
};

static int on_event(void* userdata, const lightusd_render_event* event) {
  struct event_state* state = (struct event_state*)userdata;
  assert(event->struct_size == sizeof(*event));
  if (event->type != LIGHTUSD_RENDER_EVENT_UPSERT) {
    assert(event->record_index == -1);
  }
  if (state->end == 0 && state->abort == 0 &&
      state->first_sequence_count < sizeof(state->first_sequence)) {
    state->first_sequence[state->first_sequence_count++] = event->type;
  }
  if (event->type == LIGHTUSD_RENDER_EVENT_BEGIN) state->begin++;
  if (event->type == LIGHTUSD_RENDER_EVENT_UPSERT) {
    assert(event->record_index >= 0);
    state->upsert++;
    state->last_id = event->resource_id;
    if (event->kind == 2 && event->key.len == 2 &&
        memcmp(event->key.data, "/M", 2) == 0) {
      state->mesh_id = event->resource_id;
      if (state->candidate_scene) {
        lightusd_buffer_view points_view = {0};
        assert(lightusd_render_mesh_buffer(
                   state->candidate_scene, event->record_index,
                   LIGHTUSD_MESH_BUF_POINTS, &points_view) == LIGHTUSD_OK);
        assert(points_view.nbytes == state->expected_mesh_bytes);
        assert(memcmp(points_view.data, state->expected_mesh_points,
                      state->expected_mesh_bytes) == 0);
        state->candidate_mesh_reads++;
      }
    }
  }
  if (event->type == LIGHTUSD_RENDER_EVENT_REMOVE) state->remove++;
  if (event->type == LIGHTUSD_RENDER_EVENT_END) state->end++;
  if (event->type == LIGHTUSD_RENDER_EVENT_ABORT) state->abort++;
  if (event->type == LIGHTUSD_RENDER_EVENT_BEGIN && state->reject_begin) return 0;
  return 1;
}

static void test_converter_controls(void) {
  /* The new controls consume reserved bytes without changing the v4 layout. */
  assert(sizeof(lightusd_render_config) == 64);
  assert(offsetof(lightusd_render_config, time_code) == 16);
  assert(offsetof(lightusd_render_config, max_resident_bytes) == 32);
  /* Secondary-UV interpolation consumes a reserved byte in the existing ABI. */
  assert(offsetof(lightusd_render_mesh_info, texcoords1_interp) ==
         offsetof(lightusd_render_mesh_info, colors_interp) + 1);
  if (sizeof(void*) == 8) {
    assert(sizeof(lightusd_render_mesh_info) == 104);
    assert(offsetof(lightusd_render_mesh_info, subset_count) == 64);
    assert(offsetof(lightusd_render_mesh_info, bbox_min) == 80);
  }
  const char source[] =
      "#usda 1.0\n"
      "def Xform \"Root\" {\n"
      " double3 xformOp:translate.timeSamples = {0: (0,0,0), 1: (1,0,0)}\n"
      " uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
      " def Mesh \"Quad\" {\n"
      "  point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]\n"
      "  int[] faceVertexCounts = [4]\n"
      "  int[] faceVertexIndices = [0,1,2,3]\n"
      "  float3[] extent = [(-1,-1,-1),(1,1,1)]\n"
      "  float[] primvars:displayOpacity = [1,0.5,0.75,1] (interpolation = \"vertex\")\n"
      "  rel material:binding = </Root/Material>\n"
      "  texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = \"vertex\")\n"
      "  texCoord2f[] primvars:st1 = [(0.75,0.5)] (interpolation = \"constant\")\n"
      " }\n"
      " def Material \"Material\" {\n"
      "  token outputs:surface.connect = </Root/Material/Surface.outputs:surface>\n"
      "  def Shader \"Surface\" {\n"
      "   uniform token info:id = \"UsdPreviewSurface\"\n"
      "   color3f inputs:diffuseColor.connect = </Root/Material/Texture.outputs:rgb>\n"
      "   token outputs:surface\n"
      "  }\n"
      "  def Shader \"Texture\" {\n"
      "   uniform token info:id = \"UsdUVTexture\"\n"
      "   asset inputs:file = @missing-render-control-texture.png@\n"
      "   float3 outputs:rgb\n"
      "  }\n"
      " }\n"
      " def PointInstancer \"Copies\" {\n"
      "  rel prototypes = [</Root/Quad>]\n"
      "  int[] protoIndices = [0]\n"
      "  point3f[] positions = [(2,0,0)]\n"
      " }\n"
      " def DistantLight \"Key\" {\n"
      "  color3f inputs:color = (0.2, 0.4, 0.8)\n"
      "  float inputs:intensity = 3.5\n"
      "  float inputs:angle = 1.25\n"
      "  bool inputs:normalize = true\n"
      " }\n"
      " def ParticleField3DGaussianSplat \"Splat\" {\n"
      "  point3f[] positions = [(0,0,0)]\n"
      "  float3[] scales = [(1,1,1)]\n"
      " }\n"
      " def BasisCurves \"Guide\" {\n"
      "  uniform token type = \"linear\"\n"
      "  uniform token wrap = \"nonperiodic\"\n"
      "  int[] curveVertexCounts = [3]\n"
      "  point3f[] points = [(0,0,0),(1,0,0),(2,1,0)]\n"
      "  float[] widths = [0.1,0.2,0.3] (interpolation = \"vertex\")\n"
      "  color3f[] primvars:displayColor = [(1,0,0),(0,1,0),(0,0,1)] (interpolation = \"vertex\")\n"
      "  float[] primvars:displayOpacity = [1,0.5,0.25] (interpolation = \"vertex\")\n"
      " }\n"
      "}\n";
  lightusd_stage* stage = NULL;
  assert(lightusd_stage_load_from_memory((const uint8_t*)source,
      sizeof(source) - 1, NULL, &stage) == LIGHTUSD_OK);
  lightusd_render_prim_catalog* catalog = NULL;
  assert(lightusd_render_prim_catalog_create(stage, 0.0, 1, 1, 1,
                                              &catalog) == LIGHTUSD_OK);
  assert(catalog != NULL);
  assert(lightusd_render_prim_catalog_count(
             catalog, LIGHTUSD_RENDER_PRIM_MESH) == 1);
  assert(lightusd_render_prim_catalog_count(
             catalog, LIGHTUSD_RENDER_PRIM_POINT_INSTANCER) == 1);
  lightusd_render_prim_info prim_info = {0};
  prim_info.struct_size = sizeof(prim_info);
  assert(lightusd_render_prim_catalog_get(
             catalog, LIGHTUSD_RENDER_PRIM_MESH, 0, &prim_info) ==
         LIGHTUSD_OK);
  assert(prim_info.path.len == strlen("/Root/Quad") &&
         memcmp(prim_info.path.data, "/Root/Quad", prim_info.path.len) == 0);
  assert(prim_info.material_path.len == strlen("/Root/Material"));
  lightusd_render_prim_catalog_destroy(catalog);

  lightusd_render_float_array* float_array = NULL;
  assert(lightusd_render_float_array_create(
             stage, lightusd_stage_prim_at_path(stage, "/Root/Quad"),
             "points", 0.0, &float_array) == LIGHTUSD_OK);
  assert(lightusd_render_float_array_size(float_array) == 12);
  const float* array_data = NULL;
  size_t array_count = 0;
  assert(lightusd_render_float_array_data(float_array, &array_data,
                                           &array_count) == LIGHTUSD_OK);
  assert(array_data != NULL && array_count == 12 && array_data[6] == 1.0f);
  lightusd_render_float_array_destroy(float_array);
  lightusd_prim key = lightusd_stage_prim_at_path(stage, "/Root/Key");
  lightusd_render_light_query_info light_info;
  lightusd_render_light_query_info_init(&light_info);
  assert(lightusd_render_query_light(stage, key, 0.0, &light_info) ==
         LIGHTUSD_OK);
  assert(light_info.type == LIGHTUSD_RENDER_LIGHT_DIRECTIONAL);
  assert((light_info.flags & 1u) != 0);
  assert(light_info.color[0] == 0.2f && light_info.color[1] == 0.4f &&
         light_info.color[2] == 0.8f);
  assert(light_info.intensity == 3.5f && light_info.shape[0] == 1.25f);
  assert(lightusd_render_query_light(stage,
             lightusd_stage_prim_at_path(stage, "/Root/Quad"), 0.0,
             &light_info) == LIGHTUSD_ERR_TYPE_MISMATCH);
  lightusd_render_particle_field_info particle_info = {0};
  particle_info.struct_size = sizeof(particle_info);
  assert(lightusd_render_query_particle_field(
             stage, lightusd_stage_prim_at_path(stage, "/Root/Splat"), 0.0,
             &particle_info) == LIGHTUSD_OK);
  assert(particle_info.particle_count == 1);
  assert(strcmp(particle_info.positions_property, "positions") == 0);
  assert(strcmp(particle_info.scales_property, "scales") == 0);
  lightusd_render_config material_config;
  lightusd_render_config_init(&material_config);
  material_config.load_textures = 0;
  lightusd_render_scene* material_scene = NULL;
  lightusd_prim material_prim =
      lightusd_stage_prim_at_path(stage, "/Root/Material");
  assert(lightusd_render_convert_material(stage, material_prim,
                                          &material_config,
                                          &material_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(material_scene, LIGHTUSD_RENDER_MATERIAL) == 1);
  lightusd_render_material_info material_info;
  assert(lightusd_render_material_get_info(material_scene, 0, &material_info) ==
         LIGHTUSD_OK);
  assert(material_info.shader_type == 1);
  assert(material_info.prim_path.len == strlen("/Root/Material") &&
         memcmp(material_info.prim_path.data, "/Root/Material",
                strlen("/Root/Material")) == 0);
  int32_t material_texture = -1;
  float material_value[4] = {0};
  assert(lightusd_render_material_param(material_scene, 0, "diffuse_color",
                                        &material_texture, material_value) ==
         LIGHTUSD_OK);
  assert(material_texture >= 0);
  assert(lightusd_render_count(material_scene, LIGHTUSD_RENDER_TEXTURE) > 0);
  lightusd_render_texture_info texture_info;
  assert(lightusd_render_texture_get_info(material_scene, material_texture,
                                          &texture_info) == LIGHTUSD_OK);
  assert(texture_info.asset_path.len ==
         strlen("missing-render-control-texture.png"));
  lightusd_render_scene_destroy(material_scene);
  lightusd_render_scene* instancer_scene = NULL;
  lightusd_prim copies = lightusd_stage_prim_at_path(stage, "/Root/Copies");
  assert(lightusd_render_convert_instancer(stage, copies, &material_config,
                                           &instancer_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(instancer_scene, LIGHTUSD_RENDER_INSTANCER) == 1);
  lightusd_render_instancer_info instancer_info;
  assert(lightusd_render_instancer_get_info(instancer_scene, 0,
                                            &instancer_info) == LIGHTUSD_OK);
  assert(instancer_info.instance_count == 1 &&
         instancer_info.prototype_count == 1 && instancer_info.valid);
  lightusd_buffer_view instancer_buffer = {0};
  assert(lightusd_render_instancer_buffer(
             instancer_scene, 0, LIGHTUSD_INST_BUF_POSITIONS,
             &instancer_buffer) == LIGHTUSD_OK);
  assert(instancer_buffer.nbytes == 3 * sizeof(float));
  assert(((const float*)instancer_buffer.data)[0] == 2.0f);
  char prototype_path[32] = {0};
  size_t prototype_path_size = 0;
  assert(lightusd_render_instancer_prototype_path_copy(
             instancer_scene, 0, 0, prototype_path, sizeof(prototype_path),
             &prototype_path_size) == LIGHTUSD_OK);
  assert(prototype_path_size == strlen("/Root/Quad") &&
         memcmp(prototype_path, "/Root/Quad", prototype_path_size) == 0);
  lightusd_render_scene* invalid_instancer_scene = NULL;
  assert(lightusd_render_convert_instancer(
             stage, material_prim, &material_config,
             &invalid_instancer_scene) ==
         LIGHTUSD_ERR_TYPE_MISMATCH);
  assert(invalid_instancer_scene == NULL);
  lightusd_render_scene_destroy(instancer_scene);
  lightusd_render_scene* curves_scene = NULL;
  lightusd_prim guide = lightusd_stage_prim_at_path(stage, "/Root/Guide");
  assert(lightusd_render_convert_curves(stage, guide, 0.0, 2, NULL, 0, 0,
                                        &curves_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(curves_scene, LIGHTUSD_RENDER_CURVES) == 1);
  lightusd_render_curves_info curves_info;
  assert(lightusd_render_curves_get_info(curves_scene, 0, &curves_info) ==
         LIGHTUSD_OK);
  assert(curves_info.curve_count == 1 &&
         curves_info.tessellated_point_count >= 3);
  lightusd_buffer_view curve_buffer = {0};
  assert(lightusd_render_curves_buffer(
             curves_scene, 0, LIGHTUSD_CURVES_BUF_TESSELLATED_POINTS,
             &curve_buffer) == LIGHTUSD_OK);
  assert(curve_buffer.nbytes == curves_info.tessellated_point_count *
                                    3 * sizeof(float));
  assert(lightusd_render_curves_buffer(
             curves_scene, 0, LIGHTUSD_CURVES_BUF_TESSELLATED_WIDTHS,
             &curve_buffer) == LIGHTUSD_OK);
  assert(curve_buffer.nbytes > 0);
  assert(lightusd_render_curves_buffer(
             curves_scene, 0, LIGHTUSD_CURVES_BUF_TESSELLATED_COLORS,
             &curve_buffer) == LIGHTUSD_OK);
  assert(curve_buffer.nbytes > 0);
  assert(lightusd_render_curves_buffer(
             curves_scene, 0, LIGHTUSD_CURVES_BUF_TESSELLATED_OPACITIES,
             &curve_buffer) == LIGHTUSD_OK);
  assert(curve_buffer.nbytes > 0);
  assert(lightusd_render_curves_buffer(
             curves_scene, 0, LIGHTUSD_CURVES_BUF_OPACITIES,
             &curve_buffer) == LIGHTUSD_OK);
  assert(curve_buffer.nbytes == 3 * sizeof(float));
  lightusd_render_scene_destroy(curves_scene);
  lightusd_render_config config;
  lightusd_render_config_init(&config);
  assert(config.triangulation_method == 0 && config.tangent_method == 0 &&
         config.discard_geometry == 0 && config.disable_animation == 0 &&
         config.discard_instance_source_arrays == 0 && config.use_default_asset_resolver == 0);
  lightusd_render_scene* mesh_scene = NULL;
  lightusd_prim quad = lightusd_stage_prim_at_path(stage, "/Root/Quad");
  uint64_t estimated_mesh_bytes = 0;
  assert(lightusd_render_estimate_mesh_bytes(stage, quad, &config, 0,
                                             &estimated_mesh_bytes) ==
         LIGHTUSD_OK);
  assert(estimated_mesh_bytes > 0);
  assert(lightusd_render_convert_mesh(stage, quad, &config, 0,
                                      &mesh_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(mesh_scene, LIGHTUSD_RENDER_MESH) == 1);
  lightusd_render_mesh_info mesh_info;
  assert(lightusd_render_mesh_get_info(mesh_scene, 0, &mesh_info) ==
         LIGHTUSD_OK);
  assert(mesh_info.prim_path.len == strlen("/Root/Quad") &&
         memcmp(mesh_info.prim_path.data, "/Root/Quad", strlen("/Root/Quad")) == 0);
  assert(mesh_info.point_count == 4 && mesh_info.face_count == 1);
  assert(mesh_info.has_texcoords1 && mesh_info.texcoords0_interp == 2 &&
         mesh_info.texcoords1_interp == 0);
  lightusd_render_mesh_extra_info mesh_extra;
  assert(lightusd_render_mesh_get_extra_info(mesh_scene, 0, &mesh_extra) ==
         LIGHTUSD_OK);
  assert(mesh_extra.opacities_interp == 2 &&
         mesh_extra.texcoords0_name.len == 2 &&
         memcmp(mesh_extra.texcoords0_name.data, "st", 2) == 0);
  lightusd_buffer_view mesh_buffer = {0};
  assert(lightusd_render_mesh_buffer(mesh_scene, 0, LIGHTUSD_MESH_BUF_POINTS,
                                     &mesh_buffer) == LIGHTUSD_OK);
  assert(mesh_buffer.nbytes == 12 * sizeof(float));
  assert(lightusd_render_mesh_buffer(mesh_scene, 0,
                                     LIGHTUSD_MESH_BUF_TRI_INDICES,
                                     &mesh_buffer) == LIGHTUSD_OK);
  assert(mesh_buffer.nbytes == 6 * sizeof(uint32_t));
  assert(lightusd_render_mesh_buffer(mesh_scene, 0,
                                     LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES,
                                     &mesh_buffer) == LIGHTUSD_OK);
  assert(mesh_buffer.nbytes == 6 * sizeof(uint32_t));
  assert(lightusd_render_mesh_buffer(mesh_scene, 0,
                                     LIGHTUSD_MESH_BUF_FACE_TRIANGLE_OFFSETS,
                                     &mesh_buffer) == LIGHTUSD_OK);
  assert(mesh_buffer.nbytes == 2 * sizeof(uint32_t));
  assert(lightusd_render_mesh_buffer(mesh_scene, 0,
                                     LIGHTUSD_MESH_BUF_OPACITIES,
                                     &mesh_buffer) == LIGHTUSD_OK);
  assert(mesh_buffer.nbytes == 4 * sizeof(float));
  lightusd_render_scene_destroy(mesh_scene);
  lightusd_render_scene* proxy_scene = NULL;
  assert(lightusd_render_convert_mesh_proxy(stage, quad, 0, NULL, NULL,
                                            &proxy_scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(proxy_scene, LIGHTUSD_RENDER_MESH) == 1);
  assert(lightusd_render_mesh_get_info(proxy_scene, 0, &mesh_info) ==
         LIGHTUSD_OK);
  assert(mesh_info.point_count == 8 && mesh_info.face_count == 6);
  lightusd_render_scene_destroy(proxy_scene);
  proxy_scene = NULL;
  const float proxy_min[3] = {-2.0f, -1.0f, -1.0f};
  const float proxy_max[3] = {2.0f, 1.0f, 1.0f};
  assert(lightusd_render_convert_mesh_proxy(stage, quad, 1, proxy_min,
                                            proxy_max, &proxy_scene) ==
         LIGHTUSD_OK);
  assert(lightusd_render_mesh_get_info(proxy_scene, 0, &mesh_info) ==
         LIGHTUSD_OK);
  assert(mesh_info.point_count == 8 && mesh_info.face_count == 6);
  lightusd_render_scene_destroy(proxy_scene);
  config.compute_tangents = 1;
  lightusd_render_scene* scene = NULL;
  assert(lightusd_render_convert(stage, &config, &scene) == LIGHTUSD_OK);
  assert(lightusd_render_count(scene, LIGHTUSD_RENDER_ANIMATION) > 0);
  int32_t mesh = lightusd_render_lookup(scene, LIGHTUSD_RENDER_MESH, "/Root/Quad");
  assert(mesh >= 0);
  lightusd_buffer_view view = {0};
  assert(lightusd_render_mesh_buffer(scene, mesh, LIGHTUSD_MESH_BUF_POINTS, &view) == LIGHTUSD_OK);
  assert(view.nbytes == 12 * sizeof(float));
  int32_t instancer = lightusd_render_lookup(scene, LIGHTUSD_RENDER_INSTANCER, "/Root/Copies");
  assert(instancer >= 0);
  assert(lightusd_render_instancer_buffer(scene, instancer, LIGHTUSD_INST_BUF_POSITIONS, &view) == LIGHTUSD_OK);
  assert(view.nbytes == 3 * sizeof(float));
  lightusd_render_scene_destroy(scene);
  scene = NULL;
  config.disable_animation = 1;
  config.triangulation_method = 1;
  config.discard_instance_source_arrays = 1;
  config.use_default_asset_resolver = 1;
  for (uint8_t method = 0; method < 4; ++method) {
    config.tangent_method = method;
    assert(lightusd_render_convert(stage, &config, &scene) == LIGHTUSD_OK);
    assert(lightusd_render_count(scene, LIGHTUSD_RENDER_ANIMATION) == 0);
    mesh = lightusd_render_lookup(scene, LIGHTUSD_RENDER_MESH, "/Root/Quad");
    assert(lightusd_render_mesh_buffer(scene, mesh, LIGHTUSD_MESH_BUF_TRI_INDICES, &view) == LIGHTUSD_OK);
    const uint32_t fan[] = {0,1,2,0,2,3};
    assert(view.nbytes == sizeof(fan) && memcmp(view.data, fan, sizeof(fan)) == 0);
    assert(lightusd_render_mesh_buffer(scene, mesh, LIGHTUSD_MESH_BUF_TANGENTS, &view) == LIGHTUSD_OK);
    assert(view.nbytes > 0);
    instancer = lightusd_render_lookup(scene, LIGHTUSD_RENDER_INSTANCER, "/Root/Copies");
    assert(lightusd_render_instancer_buffer(scene, instancer, LIGHTUSD_INST_BUF_POSITIONS, &view) == LIGHTUSD_OK);
    /* Draw expansion still needs these arrays; the discard preference cannot
     * drop required data in the default non-compact conversion mode. */
    assert(view.nbytes == 3 * sizeof(float));
    lightusd_render_scene_destroy(scene);
    scene = NULL;
  }
  config.discard_geometry = 1;
  lightusd_render_session* session = NULL;
  assert(lightusd_render_session_create(stage, &config, &session) == LIGHTUSD_OK);
  for (int update = 0; update < 2; ++update) {
    assert(lightusd_render_session_update(session, stage, &scene, NULL) == LIGHTUSD_OK);
    assert(lightusd_render_count(scene, LIGHTUSD_RENDER_ANIMATION) == 0);
    mesh = lightusd_render_lookup(scene, LIGHTUSD_RENDER_MESH, "/Root/Quad");
    assert(lightusd_render_mesh_buffer(scene, mesh, LIGHTUSD_MESH_BUF_POINTS, &view) == LIGHTUSD_OK);
    assert(view.nbytes == 0);
    lightusd_render_scene_destroy(scene);
    scene = NULL;
  }
  lightusd_render_session_destroy(session);
  session = NULL;
  uint8_t* fields[] = {&config.triangulation_method, &config.tangent_method,
      &config.discard_geometry, &config.disable_animation,
      &config.discard_instance_source_arrays, &config.use_default_asset_resolver};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const uint8_t saved = *fields[i];
    *fields[i] = 255;
    assert(lightusd_render_convert(stage, &config, &scene) == LIGHTUSD_ERR_INVALID_ARG);
    assert(scene == NULL);
    assert(lightusd_render_session_create(stage, &config, &session) == LIGHTUSD_ERR_INVALID_ARG);
    assert(session == NULL);
    *fields[i] = saved;
  }
  lightusd_stage_destroy(stage);
}

static void test_clip_curve_query(void) {
  const char* root_path = "./lightusd-curve-clip-query-root.usda";
  const char* clip_path = "./lightusd-curve-clip-query-data.usda";
  const char* root_source =
      "#usda 1.0\n"
      "def Xform \"World\" {\n"
      " def BasisCurves \"Hair\" (\n"
      "  clips = { dictionary default_clip = {\n"
      "   double2[] active = [(0,0),(1,0)]\n"
      "   asset[] assetPaths = [@lightusd-curve-clip-query-data.usda@]\n"
      "   string primPath = \"/World/Hair\"\n"
      "   double2[] times = [(0,0),(1,1)]\n"
      "  } }\n"
      " ) {\n"
      "  token type = \"linear\"\n"
      "  token wrap = \"nonperiodic\"\n"
      "  int[] curveVertexCounts = [2]\n"
      " }\n"
      "}\n";
  const char* clip_source =
      "#usda 1.0\n"
      "def Xform \"World\" {\n"
      " def BasisCurves \"Hair\" {\n"
      "  token type = \"linear\"\n"
      "  token wrap = \"nonperiodic\"\n"
      "  int[] curveVertexCounts = [2]\n"
      "  point3f[] points.timeSamples = {\n"
      "   0: [(0,0,0),(1,0,0)],\n"
      "   1: [(0,0,0),(2,0,0)]\n"
      "  }\n"
      " }\n"
      "}\n";
  FILE* file = fopen(root_path, "wb");
  assert(file);
  assert(fwrite(root_source, 1, strlen(root_source), file) ==
         strlen(root_source));
  assert(fclose(file) == 0);
  file = fopen(clip_path, "wb");
  assert(file);
  assert(fwrite(clip_source, 1, strlen(clip_source), file) ==
         strlen(clip_source));
  assert(fclose(file) == 0);

  lightusd_stage* stage = NULL;
  assert(lightusd_stage_load(root_path, NULL, &stage) == LIGHTUSD_OK);
  lightusd_prim hair = lightusd_stage_prim_at_path(stage, "/World/Hair");
  lightusd_render_scene* scene = NULL;
  assert(lightusd_render_convert_curves(stage, hair, 1.0, 1, NULL, 0, 0,
                                        &scene) == LIGHTUSD_OK);
  lightusd_render_curves_info info;
  assert(lightusd_render_curves_get_info(scene, 0, &info) == LIGHTUSD_OK);
  assert(info.tessellated_point_count == 2);
  lightusd_buffer_view points = {0};
  assert(lightusd_render_curves_buffer(
             scene, 0, LIGHTUSD_CURVES_BUF_TESSELLATED_POINTS,
             &points) == LIGHTUSD_OK);
  assert(points.nbytes == 6 * sizeof(float));
  const float* values = (const float*)points.data;
  assert(values[3] == 2.0f);

  lightusd_render_scene_destroy(scene);
  lightusd_stage_destroy(stage);
  assert(remove(root_path) == 0);
  assert(remove(clip_path) == 0);
}

static void test_resource_budget(void) {
  const uint64_t gib = UINT64_C(1) << 30;
  lightusd_resource_budget budget = {0};
  assert(sizeof(budget) == 96);
  assert(sizeof(lightusd_texture_fit) == 16);
  assert(lightusd_resource_budget_compute(32 * gib, 16 * gib, 1, &budget) == LIGHTUSD_OK);
  assert(budget.host_limit == 30 * gib && budget.vram_limit == 8 * gib);
  assert(budget.host_capacity == 32 * gib && budget.vram_capacity == 16 * gib);
  assert(budget.stage_limit == 17716740096ULL);
  assert(budget.cpu_geometry_limit == 8053063680ULL);
  assert(budget.io_cache_limit == 3221225472ULL);
  assert(budget.upload_staging_limit == gib / 2);
  assert(budget.gpu_geometry_limit == 5234491392ULL);
  assert(budget.gpu_texture_limit == 2013265920ULL);
  assert(budget.proxy_geometry_threshold == gib / 4);
  assert(budget.quality == 1 && budget.texture_max_edge == 2048);
  lightusd_resource_budget saved = budget;
  assert(lightusd_resource_budget_compute(0, 0, 3, &budget) == LIGHTUSD_ERR_INVALID_ARG);
  assert(memcmp(&budget, &saved, sizeof(budget)) == 0);
  assert(lightusd_resource_budget_compute(0, 0, 0, NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_resource_budget_compute(0, 0, 0, &budget) == LIGHTUSD_OK);
  assert(budget.host_limit == 0 && budget.vram_limit == 8 * gib && budget.texture_max_edge == 0);
  assert(lightusd_resource_budget_compute(gib, gib, 2, &budget) == LIGHTUSD_OK);
  assert(budget.host_limit == 3 * gib / 4 && budget.vram_limit == 3 * gib / 4);
  assert(budget.texture_max_edge == 1024);
  assert(lightusd_resource_budget_compute(UINT64_MAX, UINT64_MAX, 1, &budget) == LIGHTUSD_OK);
  assert(budget.host_limit < UINT64_MAX && budget.vram_limit == UINT64_MAX / 2);
  assert(budget.stage_limit <= budget.host_limit && budget.gpu_geometry_limit <= budget.vram_limit);
  assert(lightusd_budget_percent(UINT64_MAX, 100) == UINT64_MAX);
  assert(lightusd_budget_percent(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
  assert(lightusd_budget_percent(UINT64_MAX, 0) == 0);
  const char* names[] = {"modest", "default", "aggressive", "never", "always", "2G"};
  const uint64_t expected[] = {330, 660, 900, UINT64_MAX, 0, UINT64_C(2) << 30};
  lightusd_texture_fit fit = {0};
  for (uint32_t i = 0; i < 6; ++i) {
    assert(lightusd_texture_fit_parse(names[i], &fit) == LIGHTUSD_OK);
    assert(fit.policy == i && fit.reserved == 0);
    uint64_t threshold = 123;
    assert(lightusd_texture_fit_threshold(&fit, 1000, &threshold) == LIGHTUSD_OK);
    assert(threshold == expected[i]);
    assert(strcmp(lightusd_texture_fit_name(i), i == 5 ? "absolute" : names[i]) == 0);
    assert(lightusd_texture_fit_percent(i) == (i < 3 ? expected[i] / 10 : 0));
  }
  const char* bad[] = {"", "0", "-1", "1.5G", "2GB", " 2G", "18446744073709551616", "18446744073709551615G", "unknown"};
  const lightusd_texture_fit old_fit = fit;
  for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
    assert(lightusd_texture_fit_parse(bad[i], &fit) == LIGHTUSD_ERR_INVALID_ARG);
    assert(memcmp(&fit, &old_fit, sizeof(fit)) == 0);
  }
  assert(lightusd_texture_fit_parse(NULL, &fit) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_texture_fit_parse("default", NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_texture_fit_parse("18446744073709551615", &fit) == LIGHTUSD_OK);
  assert(fit.absolute_bytes == UINT64_MAX);
  assert(lightusd_texture_fit_parse("32k", &fit) == LIGHTUSD_OK && fit.absolute_bytes == 32768);
  assert(lightusd_texture_fit_parse("2m", &fit) == LIGHTUSD_OK && fit.absolute_bytes == 2097152);
  uint64_t threshold = 123;
  fit.policy = 6;
  assert(strcmp(lightusd_texture_fit_name(fit.policy), "invalid") == 0);
  assert(lightusd_texture_fit_percent(fit.policy) == 0);
  assert(lightusd_texture_fit_threshold(&fit, gib, &threshold) == LIGHTUSD_ERR_INVALID_ARG);
  assert(threshold == 123);
  assert(lightusd_texture_fit_threshold(NULL, gib, &threshold) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_texture_fit_threshold(&old_fit, gib, NULL) == LIGHTUSD_ERR_INVALID_ARG);
}

int main(void) {
  test_resource_budget();
  test_converter_controls();
  test_clip_curve_query();
  char overlapping_text[8] = "abcdefg";
  size_t copied_size = 0;
  lightusd_sv text_view = {overlapping_text, 6};
  assert(lightusd_sv_copy(text_view, overlapping_text + 1, 6,
                          &copied_size) == LIGHTUSD_OK);
  assert(copied_size == 6 && memcmp(overlapping_text, "aabcdef", 7) == 0);
  unsigned char overlapping_bytes[] = {0, 1, 2, 3, 4, 5, 0};
  lightusd_buffer_view byte_view = {0, 0, 0, 0, 6, overlapping_bytes, 6};
  assert(lightusd_buffer_copy(&byte_view, overlapping_bytes + 1, 5,
                              &copied_size) == LIGHTUSD_ERR_INVALID_ARG);
  assert(copied_size == byte_view.nbytes);
  assert(lightusd_buffer_copy(&byte_view, overlapping_bytes + 1, 6,
                              &copied_size) == LIGHTUSD_OK);
  assert(copied_size == byte_view.nbytes);
  assert(memcmp(overlapping_bytes, (unsigned char[]){0, 0, 1, 2, 3, 4, 5},
                sizeof(overlapping_bytes)) == 0);

  assert(LIGHTUSD_API_VERSION_MAJOR == 4);
  assert(LIGHTUSD_RENDER_UNSUPPORTED == 9 &&
         LIGHTUSD_RENDER_INSTANCER == 10 &&
         LIGHTUSD_RENDER_ROOT_NODE == 11);
  lightusd_stage* stage = NULL;
  assert(lightusd_stage_create(&stage) == LIGHTUSD_OK);
  lightusd_render_config tiny_config;
  lightusd_render_config_init(&tiny_config);
  tiny_config.max_resident_bytes = 1;
  lightusd_render_scene* over_budget_scene = NULL;
  assert(lightusd_render_convert(stage, &tiny_config, &over_budget_scene) ==
         LIGHTUSD_ERR_RESOURCE_LIMIT);
  assert(over_budget_scene == NULL);
  lightusd_render_session* tiny_session = NULL;
  assert(lightusd_render_session_create(stage, &tiny_config, &tiny_session) ==
         LIGHTUSD_OK);
  assert(lightusd_render_session_update(tiny_session, stage,
                                        &over_budget_scene, NULL) ==
         LIGHTUSD_ERR_RESOURCE_LIMIT);
  assert(over_budget_scene == NULL &&
         lightusd_render_session_revision(tiny_session) == 0);
  lightusd_render_session_destroy(tiny_session);
  assert(lightusd_stage_define_prim(stage, "/M", "Mesh", 0, NULL) ==
         LIGHTUSD_OK);
  const float points[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const int32_t counts[] = {3};
  const int32_t indices[] = {0, 1, 2};
  assert(lightusd_attr_set(stage, "/M", "points", LIGHTUSD_TYPE_POINT3F, 1,
                           points, 3, 0) == LIGHTUSD_OK);
  assert(lightusd_attr_set(stage, "/M", "faceVertexCounts", LIGHTUSD_TYPE_INT,
                           1, counts, 1, 0) == LIGHTUSD_OK);
  assert(lightusd_attr_set(stage, "/M", "faceVertexIndices", LIGHTUSD_TYPE_INT,
                           1, indices, 3, 0) == LIGHTUSD_OK);

  lightusd_render_config oversized_config;
  lightusd_render_config_init(&oversized_config);
  oversized_config.max_render_records = (uint64_t)INT32_MAX + 1u;
  lightusd_render_session* oversized_session = NULL;
  assert(lightusd_render_session_create(stage, &oversized_config,
                                        &oversized_session) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(oversized_session == NULL);
  oversized_config.max_render_records = LIGHTUSD_LIMIT_UNLIMITED;
  assert(lightusd_render_session_create(stage, &oversized_config,
                                        &oversized_session) == LIGHTUSD_OK);
  lightusd_render_session_destroy(oversized_session);

  lightusd_render_session* session = NULL;
  assert(lightusd_render_session_create(stage, NULL, &session) == LIGHTUSD_OK);
  assert(session != NULL && lightusd_render_session_revision(session) == 0);
  lightusd_render_scene* rejected = NULL;
  assert(lightusd_render_session_update(NULL, stage, &rejected, NULL) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(rejected == NULL);
  assert(lightusd_render_session_update(session, stage, NULL, NULL) ==
         LIGHTUSD_ERR_INVALID_ARG);
  lightusd_render_change_set empty_changes;
  lightusd_render_change_set_init(&empty_changes);
  assert(lightusd_render_session_apply(session, stage, &empty_changes, NULL,
                                       NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_render_session_prepare(session, stage, &empty_changes, NULL,
                                         NULL) == LIGHTUSD_ERR_INVALID_ARG);
  struct event_state events = {0};
  lightusd_render_event_sink sink;
  lightusd_render_event_sink_init(&sink);
  sink.callback = on_event;
  sink.userdata = &events;
  assert(lightusd_render_session_set_event_sink(session, &sink) == LIGHTUSD_OK);

  lightusd_render_update_info info;
  lightusd_render_update_info_init(&info);
  lightusd_render_scene* first = NULL;
  assert(lightusd_render_session_update(session, stage, &first, &info) ==
         LIGHTUSD_OK);
  assert(first != NULL && info.revision == 1 && info.full_resync == 1);
  assert(lightusd_render_count(first, LIGHTUSD_RENDER_MESH) == 1);
  assert(lightusd_render_scene_memory_bytes(first) != 0);
  lightusd_render_stats first_stats = {0};
  assert(lightusd_render_scene_get_stats(first, &first_stats) == LIGHTUSD_OK);
  assert(first_stats.mesh_count == 1 && first_stats.node_count >= 1);
  assert(first_stats.memory_bytes == lightusd_render_scene_memory_bytes(first));
  lightusd_render_scene_info scene_info = {0};
  assert(lightusd_render_scene_get_info(first, &scene_info) == LIGHTUSD_OK);
  assert(scene_info.working_color_space.len != 0);
  size_t scene_name_bytes = 0;
  assert(lightusd_sv_copy(scene_info.working_color_space, NULL, 0,
                          &scene_name_bytes) == LIGHTUSD_OK);
  assert(scene_name_bytes == scene_info.working_color_space.len);
  char scene_name[32] = {0};
  assert(scene_name_bytes < sizeof(scene_name));
  assert(lightusd_sv_copy(scene_info.working_color_space, scene_name,
                          sizeof(scene_name), &scene_name_bytes) == LIGHTUSD_OK);
  assert(scene_name_bytes == scene_info.working_color_space.len);
  assert(lightusd_render_lookup(first, LIGHTUSD_RENDER_MESH, "/M") == 0);
  assert(lightusd_render_lookup(first, LIGHTUSD_RENDER_POINTS, "/M") == -1);
  lightusd_render_record record = {0};
  assert(lightusd_render_count(first, LIGHTUSD_RENDER_ROOT_NODE) >= 1);
  assert(lightusd_render_record_get(first, LIGHTUSD_RENDER_ROOT_NODE, 0,
                                    &record) == LIGHTUSD_OK);
  assert(record.key.len == 2 && memcmp(record.key.data, "/M", 2) == 0);
  assert(lightusd_render_lookup(first, LIGHTUSD_RENDER_ROOT_NODE, "/M") == 0);
  assert(lightusd_render_record_get(first, LIGHTUSD_RENDER_MESH, 0, &record) ==
         LIGHTUSD_OK);
  assert(record.id == 0 && record.key.len == 2 &&
         memcmp(record.key.data, "/M", 2) == 0);
  size_t key_bytes = 0;
  assert(lightusd_render_record_key_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                         NULL, 0, &key_bytes) ==
         LIGHTUSD_OK);
  assert(key_bytes == 2);
  char key_copy[2] = {0};
  assert(lightusd_render_record_key_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                         key_copy, sizeof(key_copy),
                                         &key_bytes) == LIGHTUSD_OK);
  assert(key_bytes == 2 && memcmp(key_copy, "/M", 2) == 0);
  assert(lightusd_render_record_key_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                         key_copy, 1, &key_bytes) ==
         LIGHTUSD_ERR_INVALID_ARG);
  size_t name_bytes = 0;
  assert(lightusd_render_record_name_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                          NULL, 0, &name_bytes) == LIGHTUSD_OK);
  assert(name_bytes == 1);
  char name_copy[1] = {0};
  assert(lightusd_render_record_name_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                          name_copy, sizeof(name_copy),
                                          &name_bytes) == LIGHTUSD_OK);
  assert(name_copy[0] == 'M');
  assert(lightusd_render_record_name_copy(first, LIGHTUSD_RENDER_MESH, 0,
                                          name_copy, 0, &name_bytes) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_render_record_name_copy(first, LIGHTUSD_RENDER_ROOT_NODE, 0,
                                          name_copy, sizeof(name_copy),
                                          &name_bytes) == LIGHTUSD_OK);
  assert(name_copy[0] == 'M');
  assert(lightusd_render_count(first, LIGHTUSD_RENDER_POINTS) == 0);
  size_t joint_child_count = 99;
  assert(lightusd_render_skeleton_joint_children_copy(
             first, -1, 0, NULL, 0, &joint_child_count) ==
         LIGHTUSD_ERR_NOT_FOUND);
  assert(joint_child_count == 0);
  lightusd_buffer_view compact = {0};
  assert(lightusd_render_instancer_buffer(first, -1,
                                          LIGHTUSD_INST_BUF_COMPACT,
                                          &compact) == LIGHTUSD_ERR_NOT_FOUND);
  assert(lightusd_render_instancer_buffer(first, -1,
                                          LIGHTUSD_INST_BUF_PROTO_TRANSFORMS,
                                          &compact) == LIGHTUSD_ERR_NOT_FOUND);
  lightusd_render_point_instance_draw_info draw_info = {0};
  assert(lightusd_render_point_instance_draw_get_info(
             first, -1, &draw_info) == LIGHTUSD_ERR_NOT_FOUND);
  assert(lightusd_render_record_get(first, LIGHTUSD_RENDER_MESH, 1, &record) ==
         LIGHTUSD_ERR_NOT_FOUND);
  lightusd_render_animation_info animation_info = {0};
  assert(lightusd_render_animation_get_info(first, -1, &animation_info) ==
         LIGHTUSD_ERR_NOT_FOUND);
  lightusd_render_animation_channel_info channel_info = {0};
  assert(lightusd_render_animation_channel_get_info(first, -1, 0,
                                                    &channel_info) ==
         LIGHTUSD_ERR_NOT_FOUND);
  lightusd_buffer_view animation_buffer = {0};
  assert(lightusd_render_animation_channel_buffer(
             first, -1, 0, LIGHTUSD_ANIMATION_BUF_TIMES,
             &animation_buffer) == LIGHTUSD_ERR_NOT_FOUND);
  assert(lightusd_render_animation_channel_string_copy(
             first, -1, 0, 0, 0, NULL, 0, &key_bytes) ==
         LIGHTUSD_ERR_NOT_FOUND);
  assert(lightusd_render_animation_clip_asset_copy(
             first, -1, 0, NULL, 0, &key_bytes) == LIGHTUSD_ERR_NOT_FOUND);
  assert(events.begin == 1 && events.upsert >= 1 && events.end == 1 &&
         events.abort == 0 && events.last_id != 0 && events.mesh_id != 0);
  assert(events.first_sequence_count >= 3 &&
         events.first_sequence[0] == LIGHTUSD_RENDER_EVENT_BEGIN &&
         events.first_sequence[events.first_sequence_count - 1] ==
             LIGHTUSD_RENDER_EVENT_END);
  assert(lightusd_render_session_resource_id(session, 2, "/M") ==
         events.mesh_id);
  assert(lightusd_render_session_resource_id(session, 2, "/missing") == 0);
  lightusd_string* glb = NULL;
  lightusd_strlist* glb_losses = NULL;
  assert(lightusd_render_export_glb(first, "", 0, UINT64_C(1) << 20,
                                     &glb, &glb_losses) == LIGHTUSD_OK);
  const lightusd_sv glb_bytes = lightusd_string_view(glb);
  assert(glb_bytes.len >= 12 && memcmp(glb_bytes.data, "glTF", 4) == 0);
  lightusd_string_destroy(glb);
  lightusd_strlist_destroy(glb_losses);
  glb = NULL;
  glb_losses = NULL;
  assert(lightusd_render_export_glb(first, "", 0, 0,
                                     &glb, &glb_losses) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(glb == NULL && glb_losses == NULL);
  lightusd_buffer_view first_points = {0};
  assert(lightusd_render_mesh_buffer(first, 0, LIGHTUSD_MESH_BUF_POINTS,
                                     &first_points) == LIGHTUSD_OK);
  assert(first_points.count == 3 && first_points.nbytes == sizeof(points));
  assert(memcmp(first_points.data, points, sizeof(points)) == 0);
  size_t copied_bytes = 0;
  assert(lightusd_buffer_copy(&first_points, NULL, 0, &copied_bytes) ==
         LIGHTUSD_OK);
  assert(copied_bytes == sizeof(points));
  uint8_t copied[sizeof(points)] = {0};
  assert(lightusd_buffer_copy(&first_points, copied, sizeof(copied),
                             &copied_bytes) == LIGHTUSD_OK);
  assert(copied_bytes == sizeof(points) && memcmp(copied, points, sizeof(points)) == 0);
  assert(lightusd_buffer_copy(&first_points, copied, sizeof(copied) - 1,
                             &copied_bytes) == LIGHTUSD_ERR_INVALID_ARG);
  lightusd_buffer_view invalid_view = first_points;
  invalid_view.data = NULL;
  assert(lightusd_buffer_copy(&invalid_view, copied, sizeof(copied),
                             &copied_bytes) == LIGHTUSD_ERR_INVALID_ARG);

  const float moved_points[] = {0, 0, 0, 2, 0, 0, 0, 1, 0};
  assert(lightusd_attr_set(stage, "/M", "points", LIGHTUSD_TYPE_POINT3F, 1,
                           moved_points, 3, 0) == LIGHTUSD_OK);
  const lightusd_sv properties[] = {{"points", 6}};
  const lightusd_prim_change prim_change = {
      "/M", LIGHTUSD_CHANGE_TOPOLOGY, properties, 1};
  const lightusd_render_change_set changes = {
      sizeof(lightusd_render_change_set), 0, 0, {0, 0}, 1, &prim_change, 1};
  lightusd_render_scene* second = NULL;
  info.struct_size = sizeof(info);
  assert(lightusd_render_session_apply(session, stage, &changes, &second,
                                       &info) ==
         LIGHTUSD_OK);
  assert(second != NULL && info.revision == 2 && info.full_resync == 0);
  assert(info.converted_resource_count == 1 && info.upsert_count == 1 &&
         info.remove_count == 0);
  assert(events.begin == 2 && events.end == 2 && events.abort == 0 &&
         events.upsert >= 2);
  lightusd_buffer_view second_points = {0};
  assert(lightusd_render_mesh_buffer(second, 0, LIGHTUSD_MESH_BUF_POINTS,
                                     &second_points) == LIGHTUSD_OK);
  assert(memcmp(second_points.data, moved_points, sizeof(moved_points)) == 0);
  /* Published snapshots own their data across later updates and reset. */
  assert(memcmp(first_points.data, points, sizeof(points)) == 0);

  /* Prepared updates do not publish events or revisions until commit. */
  const float prepared_points[] = {0, 0, 0, 3, 0, 0, 0, 1, 0};
  assert(lightusd_attr_set(stage, "/M", "points", LIGHTUSD_TYPE_POINT3F, 1,
                           prepared_points, 3, 0) == LIGHTUSD_OK);
  const lightusd_sv prepared_properties[] = {{"points", 6}};
  const lightusd_prim_change prepared_prim_change = {
      "/M", LIGHTUSD_CHANGE_TOPOLOGY, prepared_properties, 1};
  const lightusd_render_change_set prepared_changes = {
      sizeof(lightusd_render_change_set), 0, 0, {0, 0}, 2,
      &prepared_prim_change, 1};
  lightusd_render_prepared_update* prepared = NULL;
  info.struct_size = sizeof(info);
  assert(lightusd_render_session_prepare(session, stage, &prepared_changes,
                                         &prepared, &info) == LIGHTUSD_OK);
  assert(prepared != NULL && info.revision == 3);
  assert(events.begin == 2 && events.end == 2 &&
         lightusd_render_session_revision(session) == 2);
  lightusd_render_scene* candidate_scene = NULL;
  assert(lightusd_render_prepared_scene_copy(prepared, &candidate_scene) ==
         LIGHTUSD_OK);
  assert(candidate_scene != NULL);
  events.candidate_scene = candidate_scene;
  events.expected_mesh_points = prepared_points;
  events.expected_mesh_bytes = sizeof(prepared_points);
  lightusd_render_scene* committed = NULL;
  lightusd_render_update_info invalid_info = {0};
  assert(lightusd_render_session_commit(session, prepared, &committed,
                                        &invalid_info) == LIGHTUSD_ERR_INVALID_ARG);
  assert(committed == NULL && events.begin == 2 &&
         lightusd_render_session_revision(session) == 2);
  info.struct_size = sizeof(info);
  assert(lightusd_render_session_commit(session, prepared, &committed, &info) ==
         LIGHTUSD_OK);
  assert(committed != NULL && info.revision == 3);
  assert(events.candidate_mesh_reads == 1);
  events.candidate_scene = NULL;
  assert(events.begin == 3 && events.end == 3 && events.abort == 0);
  lightusd_buffer_view committed_points = {0};
  assert(lightusd_render_mesh_buffer(committed, 0, LIGHTUSD_MESH_BUF_POINTS,
                                     &committed_points) == LIGHTUSD_OK);
  assert(memcmp(committed_points.data, prepared_points,
                sizeof(prepared_points)) == 0);
  lightusd_buffer_view candidate_points = {0};
  assert(lightusd_render_mesh_buffer(candidate_scene, 0,
                                     LIGHTUSD_MESH_BUF_POINTS,
                                     &candidate_points) == LIGHTUSD_OK);
  assert(memcmp(candidate_points.data, prepared_points,
                sizeof(prepared_points)) == 0);
  /* Prepared and committed handles retain the same immutable scene. */
  lightusd_render_record candidate_record = {0};
  lightusd_render_record committed_record = {0};
  assert(lightusd_render_record_get(candidate_scene, LIGHTUSD_RENDER_MESH, 0,
                                    &candidate_record) == LIGHTUSD_OK);
  assert(lightusd_render_record_get(committed, LIGHTUSD_RENDER_MESH, 0,
                                    &committed_record) == LIGHTUSD_OK);
  assert(candidate_record.key.data == committed_record.key.data);
  lightusd_render_scene_destroy(candidate_scene);

  /* A rejected commit retains its candidate for inspection or abort. */
  const lightusd_render_change_set rejected_prepare_changes = {
      sizeof(lightusd_render_change_set), 0, 0, {0, 0}, 3,
      &prepared_prim_change, 1};
  lightusd_render_prepared_update* rejected_prepared = NULL;
  info.struct_size = sizeof(info);
  assert(lightusd_render_session_prepare(session, stage,
                                         &rejected_prepare_changes,
                                         &rejected_prepared, &info) ==
         LIGHTUSD_OK);
  assert(rejected_prepared != NULL && info.revision == 4);
  lightusd_render_scene* rejected_clone = NULL;
  assert(lightusd_render_prepared_scene_copy(rejected_prepared,
                                             &rejected_clone) == LIGHTUSD_OK);
  events.reject_begin = 1;
  lightusd_render_scene* rejected_prepared_scene = NULL;
  assert(lightusd_render_session_commit(session, rejected_prepared,
                                        &rejected_prepared_scene, &info) !=
         LIGHTUSD_OK);
  assert(rejected_prepared_scene == NULL &&
         lightusd_render_session_revision(session) == 3);
  assert(lightusd_render_count(rejected_clone, LIGHTUSD_RENDER_MESH) == 1);
  assert(lightusd_render_prepared_scene_copy(rejected_prepared,
                                             &rejected_prepared_scene) ==
         LIGHTUSD_OK);
  lightusd_render_scene_destroy(rejected_prepared_scene);
  rejected_prepared_scene = NULL;
  lightusd_render_session_abort(session, rejected_prepared);
  assert(lightusd_render_count(rejected_clone, LIGHTUSD_RENDER_MESH) == 1);
  lightusd_render_scene_destroy(rejected_clone);
  events.reject_begin = 0;

  /* A published removal emits a remove event and retires the old lookup. */
  assert(lightusd_stage_remove_prim(stage, "/M") == LIGHTUSD_OK);
  const lightusd_prim_change remove_change = {
      "/M", LIGHTUSD_CHANGE_RESYNC, NULL, 0};
  const lightusd_render_change_set remove_changes = {
      sizeof(lightusd_render_change_set), 0, 0, {0, 0}, 3,
      &remove_change, 1};
  lightusd_render_scene* removed = NULL;
  info.struct_size = sizeof(info);
  assert(lightusd_render_session_apply(session, stage, &remove_changes,
                                       &removed, &info) == LIGHTUSD_OK);
  assert(removed != NULL && info.revision == 4 && info.remove_count >= 1);
  assert(lightusd_render_count(removed, LIGHTUSD_RENDER_MESH) == 0);
  assert(lightusd_render_session_resource_id(session, 2, "/M") == 0);
  assert(events.remove >= 1 && events.begin == 5 && events.end == 4);

  /* A sink rejection aborts the candidate without publishing a revision. */
  assert(lightusd_stage_define_prim(stage, "/M", "Mesh", 0, NULL) ==
         LIGHTUSD_OK);
  assert(lightusd_attr_set(stage, "/M", "points", LIGHTUSD_TYPE_POINT3F, 1,
                           points, 3, 0) == LIGHTUSD_OK);
  assert(lightusd_attr_set(stage, "/M", "faceVertexCounts", LIGHTUSD_TYPE_INT,
                           1, counts, 1, 0) == LIGHTUSD_OK);
  assert(lightusd_attr_set(stage, "/M", "faceVertexIndices", LIGHTUSD_TYPE_INT,
                           1, indices, 3, 0) == LIGHTUSD_OK);
  events.reject_begin = 1;
  lightusd_render_scene* rejected_update = NULL;
  assert(lightusd_render_session_update(session, stage, &rejected_update,
                                        &info) != LIGHTUSD_OK);
  assert(rejected_update == NULL && lightusd_render_session_revision(session) == 4);
  assert(events.abort >= 1);
  events.reject_begin = 0;

  lightusd_prim_change bad_change = prim_change;
  bad_change.flags = UINT32_C(1) << 31;
  lightusd_render_change_set bad_changes = changes;
  bad_changes.base_revision = 4;
  bad_changes.prims = &bad_change;
  rejected = NULL;
  assert(lightusd_render_session_apply(session, stage, &bad_changes, &rejected,
                                       NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(rejected == NULL && lightusd_render_session_revision(session) == 4);
  lightusd_render_prepared_update* rejected_candidate = NULL;
  assert(lightusd_render_session_prepare(session, stage, &bad_changes,
                                         &rejected_candidate, NULL) ==
         LIGHTUSD_ERR_INVALID_ARG);
  assert(rejected_candidate == NULL &&
         lightusd_render_session_revision(session) == 4);
  bad_change = prim_change;
  bad_change.prim_path = "/M.points";
  assert(lightusd_render_session_apply(session, stage, &bad_changes, &rejected,
                                       NULL) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_render_session_prepare(session, stage, &bad_changes,
                                         &rejected_candidate, NULL) ==
         LIGHTUSD_ERR_INVALID_ARG);

  lightusd_render_session_reset(session);
  assert(lightusd_render_session_revision(session) == 0);
  assert(lightusd_render_session_resource_id(session, 2, "/M") == 0);
  assert(lightusd_render_count(first, LIGHTUSD_RENDER_MESH) == 1);

  lightusd_render_scene_destroy(second);
  lightusd_render_scene_destroy(committed);
  lightusd_render_scene_destroy(removed);
  lightusd_render_scene_destroy(first);
  lightusd_render_session_destroy(session);
  lightusd_stage_destroy(stage);
  return 0;
}
