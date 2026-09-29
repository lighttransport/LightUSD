// SPDX-License-Identifier: Apache-2.0
// This consumer must compile using only installed C/facade headers.
#include "lightusd-cpp.hh"
#if defined(LIGHTUSD_TEST_RENDER_API)
#include "lightusd-render-cpp.hh"
#endif
#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(LIGHTUSD_TEST_RENDER_API)
static_assert(std::is_standard_layout<lightusd_render_config>::value,
              "render config must remain C layout");
static_assert(std::is_standard_layout<lightusd_document_options>::value,
              "document options must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_document_options>::value,
              "document options must remain POD");
static_assert(std::is_standard_layout<lightusd_document_progress>::value,
              "document progress must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_document_progress>::value,
              "document progress must remain POD");
static_assert(std::is_trivially_copyable<lightusd_render_config>::value,
              "render config must remain POD");
static_assert(std::is_standard_layout<lightusd_render_update_info>::value,
              "update info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_update_info>::value,
              "update info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_change_set>::value,
              "change set must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_change_set>::value,
              "change set must remain POD");
static_assert(std::is_standard_layout<lightusd_render_event>::value,
              "render event must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_event>::value,
              "render event must remain POD");
static_assert(std::is_standard_layout<lightusd_buffer_view>::value,
              "buffer view must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_buffer_view>::value,
              "buffer view must remain POD");
static_assert(std::is_standard_layout<lightusd_render_curves_info>::value,
              "curves info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_curves_info>::value,
              "curves info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_animation_info>::value,
              "animation info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_animation_info>::value,
              "animation info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_animation_channel_info>::value,
              "animation channel info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_animation_channel_info>::value,
              "animation channel info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_material_diagnostic>::value,
              "material diagnostic must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_material_diagnostic>::value,
              "material diagnostic must remain POD");
static_assert(std::is_standard_layout<lightusd_render_material_info>::value,
              "material info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_material_info>::value,
              "material info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_stats>::value,
              "render stats must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_stats>::value,
              "render stats must remain POD");
static_assert(std::is_standard_layout<lightusd_render_scene_info>::value,
              "scene info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_scene_info>::value,
              "scene info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_texture_info>::value,
              "texture info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_texture_info>::value,
              "texture info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_light_info>::value,
              "light info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_light_info>::value,
              "light info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_camera_info>::value,
              "camera info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_camera_info>::value,
              "camera info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_instancer_info>::value,
              "instancer info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_instancer_info>::value,
              "instancer info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_physics_info>::value,
              "physics info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_physics_info>::value,
              "physics info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_material_retained_param>::value,
              "retained material parameter must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_material_retained_param>::value,
              "retained material parameter must remain POD");
static_assert(std::is_standard_layout<lightusd_render_skeleton_info>::value,
              "skeleton info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_skeleton_info>::value,
              "skeleton info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_node_info>::value,
              "node info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_node_info>::value,
              "node info must remain POD");
static_assert(std::is_standard_layout<lightusd_render_mesh_info>::value,
              "mesh info must remain C layout");
static_assert(std::is_trivially_copyable<lightusd_render_mesh_info>::value,
              "mesh info must remain POD");
#endif

int main() {
  using namespace lightusd::api;
  assert(ApiVersion() == ((LIGHTUSD_API_VERSION_MAJOR << 16) |
                          (LIGHTUSD_API_VERSION_MINOR << 8) |
                          LIGHTUSD_API_VERSION_PATCH));
  assert(VersionString() && *VersionString());
  assert(TypeFromName("float") == LIGHTUSD_TYPE_FLOAT);
  assert(std::strcmp(TypeName(LIGHTUSD_TYPE_FLOAT), "float") == 0);
  assert(TypeSize(LIGHTUSD_TYPE_FLOAT) == sizeof(float));
  assert(TypeComponentCount(LIGHTUSD_TYPE_FLOAT) == 1);
  assert(FlattenFileToUSDC(nullptr, nullptr) == LIGHTUSD_ERR_INVALID_ARG);
  AssetResolver resolver;
  assert(CreateAssetResolver(&resolver) == LIGHTUSD_OK);
  static const uint8_t asset_bytes[] = {'a', 0, 'b'};
  size_t resolver_count = 0, resolver_bytes = 0, resolver_limit = 0;
  assert(GetAssetResolverMemoryStats(resolver, &resolver_count, &resolver_bytes,
                                     &resolver_limit) == LIGHTUSD_OK);
  assert(resolver_count == 0 && resolver_bytes == 0 && resolver_limit == 0);
  assert(SetAssetResolverMemoryLimit(resolver, sizeof(asset_bytes)) ==
         LIGHTUSD_OK);
  assert(RegisterMemoryAsset(resolver, "test:asset.bin", asset_bytes,
                             sizeof(asset_bytes)) == LIGHTUSD_OK);
  assert(GetAssetResolverMemoryStats(resolver, &resolver_count, &resolver_bytes,
                                     &resolver_limit) == LIGHTUSD_OK);
  assert(resolver_count == 1 && resolver_bytes == sizeof(asset_bytes) &&
         resolver_limit == sizeof(asset_bytes));
  char asset_identifier[32] = {};
  size_t asset_identifier_size = 0;
  assert(AssetResolverMemoryIdentifier(resolver, 0, asset_identifier,
                                       sizeof(asset_identifier),
                                       &asset_identifier_size) == LIGHTUSD_OK);
  assert(asset_identifier_size == std::strlen("test:asset.bin") &&
         std::memcmp(asset_identifier, "test:asset.bin",
                     asset_identifier_size) == 0);
  assert(SetAssetResolverMemoryLimit(resolver, sizeof(asset_bytes) - 1) ==
         LIGHTUSD_ERR_RESOURCE_LIMIT);
  String resolved_asset;
  assert(ReadResolvedAsset(resolver, "test:asset.bin", &resolved_asset) ==
         LIGHTUSD_OK);
  lightusd_sv resolved_view = lightusd_string_view(resolved_asset.get());
  assert(resolved_view.len == sizeof(asset_bytes));
  assert(std::memcmp(resolved_view.data, asset_bytes, sizeof(asset_bytes)) == 0);
  assert(UnregisterMemoryAsset(resolver, "test:asset.bin") == LIGHTUSD_OK);
  assert(UnregisterMemoryAsset(resolver, "test:asset.bin") ==
         LIGHTUSD_ERR_NOT_FOUND);
  assert(ReadResolvedAsset(resolver, "test:asset.bin", &resolved_asset) ==
         LIGHTUSD_ERR_IO);
  Prim retained;
  {
    Stage memory_stage;
    static const uint8_t memory_usda[] =
        "#usda 1.0\ndef Xform \"MemoryRoot\" (\n"
        "    customData = {\n string owner = \"portable\"\n token[] labels = [\"left\", \"right\"]\n }\n"
        "    assetInfo = { string source = \"memory\" }\n"
        ") {}\n";
    assert(memory_stage.load_from_memory(memory_usda,
                                         sizeof(memory_usda) - 1) ==
           LIGHTUSD_OK);
    assert(memory_stage.root_prim_count() == 1);
    assert(memory_stage.prim("/MemoryRoot"));
    lightusd::api::Prim memory_root = memory_stage.prim("/MemoryRoot");
    DictionaryView prim_custom_data;
    assert(memory_root.custom_data(&prim_custom_data) == LIGHTUSD_OK);
    assert(prim_custom_data.valid() && prim_custom_data.size() == 2);
    StringList labels;
    assert(prim_custom_data.token_array("labels", &labels) == LIGHTUSD_OK);
    assert(StringListSize(labels) == 2);
    assert(StringListGet(labels, 1).len == 5);
    assert(std::memcmp(StringListGet(labels, 1).data, "right", 5) == 0);
    lightusd_value_view owner_value{};
    lightusd_sv owner_string{};
    assert(prim_custom_data.find("owner", &owner_value, &owner_string) ==
           LIGHTUSD_OK);
    assert(owner_value.type == LIGHTUSD_TYPE_STRING);
    assert(owner_string.len == 8 &&
           std::memcmp(owner_string.data, "portable", 8) == 0);
    DictionaryView asset_info;
    assert(memory_root.asset_info(&asset_info) == LIGHTUSD_OK);
    assert(asset_info.valid() && asset_info.size() == 1);
    lightusd_sv asset_source{};
    assert(asset_info.find("source", nullptr, &asset_source) == LIGHTUSD_OK);
    assert(asset_source.len == 6 &&
           std::memcmp(asset_source.data, "memory", 6) == 0);
    lightusd_load_options load_options{};
    InitLoadOptions(&load_options);
    lightusd_save_options save_options{};
    InitSaveOptions(&save_options);
    assert(load_options.struct_size == sizeof(load_options));
    assert(save_options.struct_size == sizeof(save_options));

    Stage stage;
    assert(stage.create() == LIGHTUSD_OK);
    Prim defined_world;
    assert(stage.define_prim("/World", "Xform", 0, &defined_world) ==
           LIGHTUSD_OK);
    assert(defined_world && defined_world.path().len == 6);
    assert(defined_world.specifier() == 0 && defined_world.is_active());
    static const char kind[] = "component";
    assert(stage.set_prim_metadata("/World", "kind", LIGHTUSD_TYPE_TOKEN,
                                   kind, 1) == LIGHTUSD_OK);
    assert(stage.set_default_prim("World") == LIGHTUSD_OK);
    assert(stage.default_prim_path().len == 5);
    assert(stage.save_usdz_with_assets(nullptr, nullptr, nullptr, nullptr, 0) ==
           LIGHTUSD_ERR_INVALID_ARG);
    Value default_prim_value;
    assert(stage.metadata("defaultPrim", &default_prim_value) == LIGHTUSD_OK);
    StringList sublayers;
    assert(stage.sublayers(&sublayers) == LIGHTUSD_OK);
    DictionaryView custom_data;
    assert(stage.custom_layer_data(&custom_data) == LIGHTUSD_OK);
    assert(stage.explain_property(nullptr, nullptr, 0.0, nullptr) ==
           LIGHTUSD_ERR_INVALID_ARG);
    assert(stage.generation() != 0);
    assert(stage.prim_count() >= 1);
    lightusd_stage_stats stage_stats{};
    assert(stage.stats(&stage_stats) == LIGHTUSD_OK);
    assert(stage_stats.prim_count >= 1);
    assert(stage.start_timecode() <= stage.end_timecode());
    assert(stage.add_variant_set("/World", "lod") == LIGHTUSD_OK);
    assert(stage.add_variant("/World", "lod", "high") == LIGHTUSD_OK);
    assert(stage.set_variant_selection("/World", "lod", "high") ==
           LIGHTUSD_OK);
    retained = stage.prim("/World");
    assert(retained);
    assert(retained.kind().len == sizeof(kind) - 1);
    Value kind_value;
    assert(retained.metadata("kind", &kind_value) == LIGHTUSD_OK);
    assert(retained.variant_set_count() == 1);
    assert(retained.variant_count("lod") == 1);
    assert(retained.variant_selection("lod").len == 4);
    Stage moved(std::move(stage));
    assert(!stage && moved);
    Stage copy = moved;
    copy = copy;
    assert(copy.get() == moved.get());
    auto stale = copy.prim("/World");
    assert(copy.define_prim("/World/Child", "Sphere", 0) == LIGHTUSD_OK);
    static const char label[] = "sample";
    assert(copy.set_attribute("/World/Child", "label", LIGHTUSD_TYPE_STRING,
                              0, label, 1) == LIGHTUSD_OK);
    const double sample = 2.0;
    assert(copy.set_attribute_timesample("/World/Child", "size", 1.0,
                                         LIGHTUSD_TYPE_DOUBLE, 0, &sample, 1) ==
           LIGHTUSD_OK);
    static const char doc[] = "child prim";
    assert(copy.set_prim_metadata("/World/Child", "doc",
                                  LIGHTUSD_TYPE_STRING, doc, 1) == LIGHTUSD_OK);
    const char* api_schemas[] = {"GeomMesh"};
    assert(copy.set_prim_metadata_token_array("/World/Child", "apiSchemas",
                                              api_schemas, 1) == LIGHTUSD_OK);
    assert(copy.set_attribute_metadata("/World/Child", "label", "doc",
                                       LIGHTUSD_TYPE_STRING, doc, 1) ==
           LIGHTUSD_OK);
    assert(copy.add_relationship_target("/World/Child", "binding",
                                        "/World") == LIGHTUSD_OK);
    assert(!stale && !retained);
    retained = copy.prim("/World/Child");
    assert(retained.property_count() == 2);
    assert(retained.has_relationship("binding"));
    assert(retained.has_property("label"));
    lightusd_sv label_view{};
    assert(retained.attribute_string("label", &label_view) == LIGHTUSD_OK);
    assert(label_view.len == 6 && std::strncmp(label_view.data, "sample", 6) == 0);
    assert(retained.has_timesamples("size"));
    assert(retained.timesample_count("size") == 1);
    double sample_time = 0.0;
    assert(retained.timesample_times("size", &sample_time, 1) == 1);
    assert(sample_time == 1.0);
    lightusd_value_view sampled{};
    assert(retained.timesample_at("size", 0, &sample_time, &sampled) == LIGHTUSD_OK);
    Value interpolated;
    assert(retained.interpolate("size", 1.0, 0, &interpolated) ==
           LIGHTUSD_OK);
    assert(!retained.has_property("missing"));
    assert(!retained.has_timesamples("missing"));
    assert(retained.timesample_count("missing") == 0);
    assert(!retained.child("missing"));
    assert(retained.relationship_count() == 1);
    StringList relationship_names;
    assert(retained.relationship_names(&relationship_names) == LIGHTUSD_OK);
    assert(retained.relationship_target_count("binding") == 1);
    assert(retained.relationship_target("binding", 0).len == 6);
    assert(retained.variant_set_count() == 0);
    assert(retained.variant_selection("missing").len == 0);
    double transform[16] = {};
    assert(retained.local_transform(0.0, transform) == LIGHTUSD_OK);
    assert(retained.world_transform(0.0, transform) == LIGHTUSD_OK);
#if defined(LIGHTUSD_TEST_RENDER_API)
    RenderScene scene;
    assert(Convert(copy, &scene) == LIGHTUSD_OK);
    assert(scene);
    lightusd_render_config render_config{};
    InitRenderConfig(&render_config);
    assert(render_config.struct_size == sizeof(render_config));
    lightusd_render_update_info initialized_update{};
    InitRenderUpdateInfo(&initialized_update);
    assert(initialized_update.struct_size == sizeof(initialized_update));
    lightusd_render_change_set initialized_changes{};
    InitRenderChangeSet(&initialized_changes);
    assert(initialized_changes.struct_size == sizeof(initialized_changes));
    lightusd_render_event_sink initialized_sink{};
    InitRenderEventSink(&initialized_sink);
    assert(initialized_sink.struct_size == sizeof(initialized_sink));
    assert(RenderCount(scene, LIGHTUSD_RENDER_MESH) == 1);
    assert(RenderLookup(scene, LIGHTUSD_RENDER_MESH, "/World/Child") == 0);
    assert(RenderRootNode(scene, 0) == 0);
    assert(RenderSceneMemoryBytes(scene) != 0);
    static const uint8_t preview_graph_usda[] = R"USDA(#usda 1.0
def Xform "World" {
  def Scope "Looks" {
    def Material "Mat" {
      token outputs:surface.connect = </World/Looks/Mat/Preview.outputs:surface>
      def Shader "Preview" {
        uniform token info:id = "ND_UsdPreviewSurface_surfaceshader"
        color3f inputs:diffuseColor.connect = </World/Looks/Mat/Graph.outputs:color>
        token outputs:surface
      }
      def NodeGraph "Graph" {
        color3f outputs:color.connect = </World/Looks/Mat/Graph/Color.outputs:out>
        def Shader "Color" {
          uniform token info:id = "ND_constant_color3"
          color3f inputs:value = (0.2, 0.4, 0.6)
          color3f outputs:out
        }
      }
    }
  }
}
)USDA";
    Stage preview_graph_stage;
    assert(preview_graph_stage.load_from_memory(
               preview_graph_usda, sizeof(preview_graph_usda) - 1) ==
           LIGHTUSD_OK);
    RenderScene preview_graph_scene;
    assert(Convert(preview_graph_stage, &preview_graph_scene) == LIGHTUSD_OK);
    assert(RenderCount(preview_graph_scene, LIGHTUSD_RENDER_MATERIAL) == 1);
    size_t preview_graph_size = 0;
    assert(RenderMaterialNodegraphCopy(preview_graph_scene, 0, 2, nullptr, 0,
                                       &preview_graph_size) == LIGHTUSD_OK);
    assert(preview_graph_size != 0);
    size_t short_graph_size = 0;
    assert(RenderMaterialNodegraphCopy(preview_graph_scene, 0, 2, nullptr, 0,
                                       nullptr) == LIGHTUSD_ERR_INVALID_ARG);
    std::vector<char> short_graph_json(preview_graph_size, '\0');
    assert(RenderMaterialNodegraphCopy(
               preview_graph_scene, 0, 2, short_graph_json.data(),
               preview_graph_size - 1, &short_graph_size) ==
           LIGHTUSD_ERR_INVALID_ARG);
    assert(short_graph_size == preview_graph_size);
    std::vector<char> preview_graph_json(preview_graph_size + 1, '\0');
    assert(RenderMaterialNodegraphCopy(
               preview_graph_scene, 0, 2, preview_graph_json.data(),
               preview_graph_size, &preview_graph_size) == LIGHTUSD_OK);
    const std::string preview_graph_text(preview_graph_json.data());
    assert(preview_graph_text.find("\"nodegraph\"") != std::string::npos);
    assert(preview_graph_text.find("Color") != std::string::npos);
    lightusd_render_stats render_stats{};
    assert(RenderSceneStats(scene, &render_stats) == LIGHTUSD_OK);
    assert(render_stats.node_count >= 2 && render_stats.mesh_count == 1);
    assert(render_stats.memory_bytes == RenderSceneMemoryBytes(scene));
    StringList warnings;
    assert(RenderSceneWarnings(scene, &warnings) == LIGHTUSD_OK);
    lightusd_render_scene_info scene_info{};
    assert(RenderSceneInfo(scene, &scene_info) == LIGHTUSD_OK);
    assert(scene_info.working_to_display_linear[0] == 1.0f &&
           scene_info.working_to_display_linear[4] == 1.0f &&
           scene_info.working_to_display_linear[8] == 1.0f);
    lightusd_render_node_info node_info{};
    assert(RenderNodeInfo(scene, 0, &node_info) == LIGHTUSD_OK);
    assert(RenderNodeChildren(scene, -1, nullptr, 0) == 0);
    lightusd_render_mesh_info mesh_info{};
    assert(RenderMeshInfo(scene, 0, &mesh_info) == LIGHTUSD_OK);
    lightusd_render_primvar_info primvar_info{};
    assert(RenderMeshPrimvarInfo(scene, 0, 0, &primvar_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_buffer_view blendshape_buffer{};
    lightusd_sv blendshape_name{};
    float blendshape_weight = 0.0f;
    assert(RenderMeshBlendshape(scene, -1, 0, 0, &blendshape_name,
                                &blendshape_weight, &blendshape_buffer) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_material_info material_info{};
    assert(RenderMaterialInfo(scene, -1, &material_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_materialx_config_info mtlx_config_info{};
    assert(RenderMaterialMtlxConfig(scene, -1, &mtlx_config_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_texture_info texture_info{};
    assert(RenderTextureInfo(scene, -1, &texture_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_image_info image_info{};
    assert(RenderImageInfo(scene, -1, &image_info) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_light_info light_info{};
    assert(RenderLightInfo(scene, -1, &light_info) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_camera_info camera_info{};
    assert(RenderCameraInfo(scene, -1, &camera_info) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_skeleton_info skeleton_info{};
    assert(RenderSkeletonInfo(scene, -1, &skeleton_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_joint_info joint_info{};
    assert(RenderSkeletonJoint(scene, -1, 0, &joint_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    size_t missing_child_count = 99;
    assert(RenderSkeletonJointChildrenCopy(scene, -1, 0, nullptr, 0,
                                           &missing_child_count) ==
           LIGHTUSD_ERR_NOT_FOUND);
    assert(missing_child_count == 0);
    lightusd_render_instancer_info instancer_info{};
    assert(RenderInstancerInfo(scene, -1, &instancer_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_point_instance_draw_info draw_info{};
    assert(RenderPointInstanceDrawInfo(scene, -1, &draw_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_unsupported_info unsupported_info{};
    assert(RenderUnsupportedInfo(scene, -1, &unsupported_info) ==
           LIGHTUSD_ERR_NOT_FOUND);
    lightusd_buffer_view instancer_buffer{};
    assert(RenderInstancerBuffer(scene, -1, LIGHTUSD_INST_BUF_POSITIONS,
                                 &instancer_buffer) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_record scene_record{};
    assert(RenderRecord(scene, LIGHTUSD_RENDER_MESH, 0, &scene_record) ==
           LIGHTUSD_OK);
    assert(scene_record.key.len == 12);
    size_t key_size = 0;
    assert(RenderRecordKeyCopy(scene, LIGHTUSD_RENDER_MESH, 0, nullptr, 0,
                               &key_size) == LIGHTUSD_OK);
    assert(key_size == scene_record.key.len);
    char key_copy[12] = {};
    assert(RenderRecordKeyCopy(scene, LIGHTUSD_RENDER_MESH, 0, key_copy,
                               sizeof(key_copy), &key_size) == LIGHTUSD_OK);
    assert(key_size == sizeof(key_copy) &&
           std::memcmp(key_copy, "/World/Child", sizeof(key_copy)) == 0);
    assert(RenderInstancerPrototypePathCopy(scene, -1, 0, nullptr, 0,
                                            &key_size) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_curves_info curves_info = {};
    assert(RenderCurvesInfo(scene, -1, &curves_info) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_material_diagnostic material_diagnostic = {};
    assert(RenderMaterialDiagnostic(scene, -1, 0, &material_diagnostic) ==
           LIGHTUSD_ERR_NOT_FOUND);
    size_t nodegraph_bytes = 0;
    assert(RenderMaterialNodegraphCopy(scene, -1, 0, nullptr, 0,
                                       &nodegraph_bytes) ==
           LIGHTUSD_ERR_NOT_FOUND);
    assert(RenderMaterialTerminalPathCopy(scene, -1, 0, nullptr, 0,
                                          &nodegraph_bytes) ==
           LIGHTUSD_ERR_NOT_FOUND);
    int32_t parameter_texture = -1;
    float parameter_value[4] = {};
    assert(RenderMaterialParam(scene, -1, "base_color", &parameter_texture,
                               parameter_value) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_render_material_retained_param retained_parameter{};
    assert(RenderMaterialRetainedParamCount(scene, -1) == 0);
    assert(RenderMaterialRetainedParam(scene, -1, 0, &retained_parameter) ==
           LIGHTUSD_ERR_NOT_FOUND);
    size_t light_link_bytes = 0;
    assert(RenderLightLinkCount(scene, -1, 0) == 0);
    assert(RenderLightLinkCopy(scene, -1, 0, 0, nullptr, 0,
                               &light_link_bytes) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_buffer_view skeleton_buffer{};
    assert(RenderSkeletonBuffer(scene, -1,
                                LIGHTUSD_SKELETON_BUF_BIND_TRANSFORMS,
                                &skeleton_buffer) == LIGHTUSD_ERR_NOT_FOUND);
    lightusd_buffer_view points = {};
    assert(lightusd_render_mesh_buffer(scene.get(), 0, LIGHTUSD_MESH_BUF_POINTS,
                                       &points) == LIGHTUSD_OK);
    size_t point_bytes = 0;
    assert(RenderBufferCopy(points, nullptr, 0, &point_bytes) ==
           LIGHTUSD_OK);
    assert(point_bytes == points.nbytes);
    std::vector<unsigned char> point_copy(point_bytes);
    assert(RenderBufferCopy(points, point_copy.data(), point_copy.size(),
                            &point_bytes) == LIGHTUSD_OK);
    static const uint8_t physics_usda[] =
        "#usda 1.0\ndef Xform \"World\" {\n"
        "  def PhysicsScene \"Scene\" {}\n"
        "}\n";
    Stage physics_stage;
    assert(physics_stage.load_from_memory(physics_usda,
                                          sizeof(physics_usda) - 1) ==
           LIGHTUSD_OK);
    RenderScene physics_scene;
    assert(Convert(physics_stage, &physics_scene) == LIGHTUSD_OK);
    assert(RenderPhysicsCount(physics_scene,
                              LIGHTUSD_RENDER_PHYSICS_SCENE) == 1);
    lightusd_render_physics_info physics_info{};
    assert(RenderPhysicsInfo(physics_scene, LIGHTUSD_RENDER_PHYSICS_SCENE, 0,
                             &physics_info) == LIGHTUSD_OK);
    assert(physics_info.prim_path.len == 12);
    assert(physics_info.scalar0 > 9.8f && physics_info.scalar0 < 9.9f);
    static const uint8_t camera_usda[] =
        "#usda 1.0\ndef Xform \"World\" {\n"
        "  def Camera \"MainCamera\" {\n"
        "    float focalLength = 35\n"
        "    float focusDistance = 12\n"
        "    float fStop = 2.5\n"
        "    float horizontalApertureOffset = 1.5\n"
        "    float exposure = 0.75\n"
        "    token stereoRole = \"left\"\n"
        "    double shutter:open = -0.25\n"
        "    double shutter:close = 0.5\n"
        "  }\n}\n";
    Stage camera_stage;
    assert(camera_stage.load_from_memory(camera_usda,
                                         sizeof(camera_usda) - 1) ==
           LIGHTUSD_OK);
    RenderScene camera_scene;
    assert(Convert(camera_stage, &camera_scene) == LIGHTUSD_OK);
    assert(RenderCount(camera_scene, LIGHTUSD_RENDER_CAMERA) == 1);
    lightusd_render_camera_info authored_camera{};
    assert(RenderCameraInfo(camera_scene, 0, &authored_camera) == LIGHTUSD_OK);
    assert(authored_camera.focal_length == 35.0f);
    assert(authored_camera.focus_distance == 12.0f);
    assert(authored_camera.fstop == 2.5f);
    assert(authored_camera.horizontal_aperture_offset == 1.5f);
    assert(authored_camera.exposure == 0.75f);
    assert(authored_camera.stereo_role == 1);
    assert(authored_camera.shutter_open == -0.25);
    assert(authored_camera.shutter_close == 0.5);
    assert(authored_camera.vertical_aperture > 0.0f);
    const float expected_aspect = authored_camera.horizontal_aperture /
                                  authored_camera.vertical_aperture;
    assert(authored_camera.aspect > expected_aspect - 0.001f &&
           authored_camera.aspect < expected_aspect + 0.001f);
    static const uint8_t instancer_usda[] =
        "#usda 1.0\ndef PointInstancer \"Instances\" {\n"
        "  int[] protoIndices = [0, 0]\n"
        "  point3f[] positions = [(0, 0, 0), (2, 0, 0)]\n"
        "  rel prototypes = [</Instances/Proto>]\n"
        "  def Mesh \"Proto\" {\n"
        "    int[] faceVertexCounts = [3]\n"
        "    int[] faceVertexIndices = [0, 1, 2]\n"
        "    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
        "  }\n}\n";
    Stage instancer_stage;
    assert(instancer_stage.load_from_memory(instancer_usda,
                                            sizeof(instancer_usda) - 1) ==
           LIGHTUSD_OK);
    RenderScene instancer_scene;
    assert(Convert(instancer_stage, &instancer_scene) == LIGHTUSD_OK);
    assert(RenderCount(instancer_scene, LIGHTUSD_RENDER_INSTANCER) == 1);
    lightusd_render_instancer_info authored_instancer{};
    assert(RenderInstancerInfo(instancer_scene, 0, &authored_instancer) ==
           LIGHTUSD_OK);
    assert(authored_instancer.instance_count == 2);
    assert(authored_instancer.visible_instance_count == 2);
    assert(authored_instancer.prototype_count == 1);
    assert(authored_instancer.valid == 1);
    assert(authored_instancer.validation_error.len == 0);
    assert(authored_instancer.draw_count > 0);
    assert(RenderLookup(instancer_scene, LIGHTUSD_RENDER_INSTANCER,
                        "/Instances") == 0);
    assert(RenderLookup(instancer_scene, LIGHTUSD_RENDER_POINT_INSTANCE_DRAW,
                        "/Instances") == 0);
    lightusd_render_record draw_record{};
    assert(RenderRecord(instancer_scene, LIGHTUSD_RENDER_POINT_INSTANCE_DRAW,
                        0, &draw_record) == LIGHTUSD_OK);
    assert(draw_record.key.len == sizeof("/Instances") - 1);
    size_t instancer_name_bytes = 0;
    assert(RenderRecordNameCopy(instancer_scene, LIGHTUSD_RENDER_INSTANCER,
                                0, nullptr, 0, &instancer_name_bytes) ==
           LIGHTUSD_OK);
    assert(instancer_name_bytes == sizeof("Instances") - 1);
    char instancer_name[sizeof("Instances") - 1]{};
    assert(RenderRecordNameCopy(instancer_scene, LIGHTUSD_RENDER_INSTANCER,
                                0, instancer_name, sizeof(instancer_name),
                                &instancer_name_bytes) == LIGHTUSD_OK);
    assert(std::memcmp(instancer_name, "Instances", sizeof(instancer_name)) == 0);
    assert(RenderRecordNameCopy(instancer_scene,
                                LIGHTUSD_RENDER_POINT_INSTANCE_DRAW, 0,
                                nullptr, 0, &instancer_name_bytes) ==
           LIGHTUSD_ERR_NOT_FOUND);
    assert(authored_instancer.has_orientations == 0);
    assert(authored_instancer.has_scales == 0);
    assert(authored_instancer.has_velocities == 0);
    assert(authored_instancer.has_angular_velocities == 0);
    static const uint8_t skeleton_usda[] =
        "#usda 1.0\ndef SkelRoot \"Rig\" {\n"
        "  def Skeleton \"Skeleton\" {\n"
        "    uniform token[] joints = [\"Root\", \"Root/Spine\", \"Root/Spine/Head\"]\n"
        "    uniform matrix4d[] bindTransforms = [\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1)),\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,2,0,1))]\n"
        "    uniform matrix4d[] restTransforms = [\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1)),\n"
        "      ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,2,0,1))]\n"
        "  }\n}\n";
    Stage skeleton_stage;
    assert(skeleton_stage.load_from_memory(skeleton_usda,
                                           sizeof(skeleton_usda) - 1) ==
           LIGHTUSD_OK);
    RenderScene skeleton_scene;
    assert(Convert(skeleton_stage, &skeleton_scene) == LIGHTUSD_OK);
    assert(RenderCount(skeleton_scene, LIGHTUSD_RENDER_SKELETON) == 1);
    lightusd_render_skeleton_info authored_skeleton{};
    assert(RenderSkeletonInfo(skeleton_scene, 0, &authored_skeleton) ==
           LIGHTUSD_OK);
    assert(authored_skeleton.joint_count == 3);
    size_t child_count = 0;
    assert(RenderSkeletonJointChildrenCopy(skeleton_scene, 0, 0, nullptr, 0,
                                           &child_count) == LIGHTUSD_OK);
    assert(child_count == 1);
    int32_t child_id = -1;
    assert(RenderSkeletonJointChildrenCopy(skeleton_scene, 0, 0, &child_id, 0,
                                           &child_count) ==
           LIGHTUSD_ERR_INVALID_ARG);
    assert(RenderSkeletonJointChildrenCopy(skeleton_scene, 0, 0, &child_id, 1,
                                           &child_count) == LIGHTUSD_OK);
    assert(child_id == 1);
    assert(RenderSkeletonJointChildrenCopy(skeleton_scene, 0, 2, nullptr, 0,
                                           &child_count) == LIGHTUSD_OK);
    assert(child_count == 0);
    lightusd_render_stats cached_stats{};
    assert(RenderSceneStats(scene, &cached_stats) == LIGHTUSD_OK);
    assert(cached_stats.memory_bytes == RenderSceneMemoryBytes(scene));
    RenderScene destination(std::move(scene));
    assert(!scene && destination);
    RenderSession session;
    DocumentSession document;
    assert(document.create() == LIGHTUSD_OK);
    RenderSession unopened_document_renderer;
    assert(unopened_document_renderer.create(document) ==
           LIGHTUSD_ERR_NOT_FOUND);
    assert(session.create(copy) == LIGHTUSD_OK);
    lightusd_render_update_info update{};
    update.struct_size = sizeof(update);
    RenderScene first;
    assert(session.update(copy, &first, &update) == LIGHTUSD_OK);
    assert(first && update.revision == 1 && update.full_resync == 1);
    assert(session.revision() == 1);
    assert(session.resource_id(2, "/World/Child") != 0);
    RenderScene second;
    update.struct_size = sizeof(update);
    assert(session.update(copy, &second, &update) == LIGHTUSD_OK);
    assert(second && update.revision == 2);
    assert(RenderCount(first, LIGHTUSD_RENDER_MESH) == 1);
    lightusd_render_change_set changes{};
    changes.struct_size = sizeof(changes);
    changes.full_resync = 1;
    changes.base_revision = 2;
    PreparedRenderUpdate prepared;
    update.struct_size = sizeof(update);
    assert(session.prepare(copy, changes, &prepared, &update) == LIGHTUSD_OK);
    assert(prepared && update.revision == 3);
    RenderScene prepared_scene;
    assert(prepared.scene_copy(&prepared_scene) == LIGHTUSD_OK);
    assert(RenderCount(prepared_scene, LIGHTUSD_RENDER_MESH) == 1);
    RenderScene committed;
    update.struct_size = sizeof(update);
    assert(session.commit(&prepared, &committed, &update) == LIGHTUSD_OK);
    assert(committed && update.revision == 3 && session.revision() == 3);
    assert(RenderCount(prepared_scene, LIGHTUSD_RENDER_MESH) == 1);
    session.reset();
    assert(session.revision() == 0);
    PreparedRenderUpdate orphan;
    {
      RenderSession temporary;
      assert(temporary.create(copy) == LIGHTUSD_OK);
      lightusd_render_change_set orphan_changes{};
      orphan_changes.struct_size = sizeof(orphan_changes);
      orphan_changes.full_resync = 1;
      lightusd_render_update_info orphan_info{};
      orphan_info.struct_size = sizeof(orphan_info);
      assert(temporary.prepare(copy, orphan_changes, &orphan, &orphan_info) ==
             LIGHTUSD_OK);
      assert(orphan);
    }
    RenderScene orphan_scene;
    assert(orphan.scene_copy(&orphan_scene) == LIGHTUSD_OK);
    orphan.reset();
    assert(!orphan);
    assert(session.create(copy) == LIGHTUSD_OK);
    PreparedRenderUpdate recreated_orphan;
    lightusd_render_change_set recreate_changes{};
    recreate_changes.struct_size = sizeof(recreate_changes);
    recreate_changes.full_resync = 1;
    lightusd_render_update_info recreate_info{};
    recreate_info.struct_size = sizeof(recreate_info);
    assert(session.prepare(copy, recreate_changes, &recreated_orphan,
                           &recreate_info) == LIGHTUSD_OK);
    assert(recreated_orphan);
    assert(session.create(copy) == LIGHTUSD_OK);
    recreated_orphan.reset();
#endif
    assert(copy.root_prim_count() == 1);
    Prim root = copy.root_prim(0);
    assert(root && root.path().len == 6 &&
           std::memcmp(root.path().data, "/World", 6) == 0);
    String text;
    assert(copy.export_usda(&text) == LIGHTUSD_OK);
    assert(lightusd_string_view(text.get()).len != 0);
  }
  // All Stage wrappers above are gone. The prim retains its owning stage.
  assert(retained);
  assert(lightusd_prim_type_name(retained.get()).len == 6);
  Prim moved(std::move(retained));
  assert(!retained && moved);
  retained = std::move(moved);
  assert(!moved && retained);
  retained = Prim();
  assert(!retained);
  Stage remove_stage;
  assert(remove_stage.create() == LIGHTUSD_OK);
  assert(remove_stage.define_prim("/Delete", nullptr, 0) == LIGHTUSD_OK);
  assert(remove_stage.remove_prim("/Delete") == LIGHTUSD_OK);
  assert(!remove_stage.prim("/Delete"));
}
