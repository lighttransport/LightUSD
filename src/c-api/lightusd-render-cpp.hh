// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lightusd-cpp.hh"
#include "lightusd-session-cpp.hh"
#include "lightusd-render-c.h"
#include <memory>

namespace lightusd {
namespace api {

using ResourceBudget = lightusd_resource_budget;
using TextureFit = lightusd_texture_fit;
inline lightusd_status ComputeResourceBudget(uint64_t host, uint64_t vram,
                                             uint32_t quality, ResourceBudget* out) noexcept {
  return lightusd_resource_budget_compute(host, vram, quality, out);
}
inline ResourceBudget ComputeResourceBudget(uint64_t host, uint64_t vram) noexcept {
  ResourceBudget result{};
  lightusd_resource_budget_compute(host, vram, 1, &result);
  return result;
}
inline bool ParseTextureFit(const char* text, TextureFit* out) {
  return lightusd_texture_fit_parse(text, out) == LIGHTUSD_OK;
}
inline lightusd_status TextureFitThresholdBytes(const TextureFit& fit, uint64_t vram,
                                                uint64_t* out) noexcept {
  return lightusd_texture_fit_threshold(&fit, vram, out);
}
inline uint64_t BudgetPercent(uint64_t value, uint64_t percent) noexcept {
  return lightusd_budget_percent(value, percent);
}
inline const char* TextureFitName(const TextureFit& fit) noexcept {
  return lightusd_texture_fit_name(fit.policy);
}
inline uint32_t TextureFitPercent(const TextureFit& fit) noexcept {
  return lightusd_texture_fit_percent(fit.policy);
}

using RenderScene = Owner<lightusd_render_scene, lightusd_render_scene_destroy>;
using RenderSessionHandle =
    Owner<lightusd_render_session, lightusd_render_session_destroy>;

inline void InitRenderConfig(lightusd_render_config* config) noexcept {
  lightusd_render_config_init(config);
}

inline void InitRenderUpdateInfo(lightusd_render_update_info* info) noexcept {
  lightusd_render_update_info_init(info);
}

inline void InitRenderChangeSet(lightusd_render_change_set* changes) noexcept {
  lightusd_render_change_set_init(changes);
}

inline void InitRenderEventSink(lightusd_render_event_sink* sink) noexcept {
  lightusd_render_event_sink_init(sink);
}

inline size_t RenderCount(const RenderScene& scene, uint8_t kind) noexcept {
  return lightusd_render_count(scene.get(), kind);
}

inline int32_t RenderLookup(const RenderScene& scene, uint8_t kind,
                            const char* prim_path) noexcept {
  return lightusd_render_lookup(scene.get(), kind, prim_path);
}

inline int32_t RenderRootNode(const RenderScene& scene, size_t index) noexcept {
  return lightusd_render_root_node(scene.get(), index);
}

inline lightusd_status RenderSceneInfo(
    const RenderScene& scene, lightusd_render_scene_info* out) noexcept {
  return lightusd_render_scene_get_info(scene.get(), out);
}

inline size_t RenderSceneMemoryBytes(const RenderScene& scene) noexcept {
  return lightusd_render_scene_memory_bytes(scene.get());
}

inline lightusd_status RenderSceneStats(const RenderScene& scene,
                                        lightusd_render_stats* out) noexcept {
  return lightusd_render_scene_get_stats(scene.get(), out);
}

inline size_t RenderPhysicsCount(const RenderScene& scene, uint8_t kind) noexcept {
  return lightusd_render_physics_count(scene.get(), kind);
}

inline lightusd_status RenderPhysicsInfo(
    const RenderScene& scene, uint8_t kind, size_t index,
    lightusd_render_physics_info* out) noexcept {
  return lightusd_render_physics_get_info(scene.get(), kind, index, out);
}

inline lightusd_status RenderPhysicsExtensionProperty(
    const RenderScene& scene, uint8_t kind, size_t index,
    size_t property_index, lightusd_sv* name, lightusd_sv* value) noexcept {
  return lightusd_render_physics_extension_property(
      scene.get(), kind, index, property_index, name, value);
}

inline lightusd_status RenderPhysicsStringCopy(
    const RenderScene& scene, uint8_t kind, size_t index, uint8_t which,
    size_t string_index, char* out, size_t cap, size_t* required) noexcept {
  return lightusd_render_physics_string_copy(
      scene.get(), kind, index, which, string_index, out, cap, required);
}

inline lightusd_status RenderSceneWarnings(const RenderScene& scene,
                                           StringList* out) noexcept {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_scene_warnings(scene.get(), out->put());
}

inline lightusd_status ExportGLB(const RenderScene& scene,
                                const char* source_path, bool strict,
                                uint64_t max_output_bytes, String* glb,
                                StringList* losses) {
  if (!glb || !losses) return LIGHTUSD_ERR_INVALID_ARG;
  glb->reset();
  losses->reset();
  return lightusd_render_export_glb(scene.get(), source_path, strict ? 1 : 0,
                                    max_output_bytes, glb->put(), losses->put());
}

inline lightusd_status RenderRecord(const RenderScene& scene, uint8_t kind,
                                    size_t index,
                                    lightusd_render_record* out) noexcept {
  return lightusd_render_record_get(scene.get(), kind, index, out);
}

inline lightusd_status RenderRecordKeyCopy(const RenderScene& scene,
                                           uint8_t kind, size_t index,
                                           char* out, size_t cap,
                                           size_t* required) noexcept {
  return lightusd_render_record_key_copy(scene.get(), kind, index, out, cap,
                                         required);
}

inline lightusd_status RenderRecordNameCopy(const RenderScene& scene,
                                            uint8_t kind, size_t index,
                                            char* out, size_t cap,
                                            size_t* required) noexcept {
  return lightusd_render_record_name_copy(scene.get(), kind, index, out, cap,
                                          required);
}

inline lightusd_status RenderInstancerPrototypePathCopy(
    const RenderScene& scene, int32_t instancer_id, size_t prototype_index,
    char* out, size_t cap, size_t* required) noexcept {
  return lightusd_render_instancer_prototype_path_copy(
      scene.get(), instancer_id, prototype_index, out, cap, required);
}

inline lightusd_status RenderBufferCopy(const lightusd_buffer_view& view,
                                        void* out, size_t cap,
                                        size_t* required) noexcept {
  return lightusd_buffer_copy(&view, out, cap, required);
}

inline lightusd_status RenderNodeInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_node_info* out) noexcept {
  return lightusd_render_node_get_info(scene.get(), id, out);
}

inline size_t RenderNodeChildren(const RenderScene& scene, int32_t id,
                                 int32_t* out, size_t cap) noexcept {
  return lightusd_render_node_children(scene.get(), id, out, cap);
}

inline lightusd_status RenderMeshInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_mesh_info* out) noexcept {
  return lightusd_render_mesh_get_info(scene.get(), id, out);
}

inline lightusd_status RenderMeshExtraInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_mesh_extra_info* out) noexcept {
  return lightusd_render_mesh_get_extra_info(scene.get(), id, out);
}

inline lightusd_status RenderMeshSubset(
    const RenderScene& scene, int32_t mesh_id, size_t index,
    uint32_t* face_start, uint32_t* face_count,
    int32_t* material_id) noexcept {
  return lightusd_render_mesh_subset(scene.get(), mesh_id, index, face_start,
                                     face_count, material_id);
}

inline lightusd_status RenderMeshPrimvarInfo(
    const RenderScene& scene, int32_t mesh_id, size_t index,
    lightusd_render_primvar_info* out) noexcept {
  return lightusd_render_mesh_primvar_info(scene.get(), mesh_id, index, out);
}

inline lightusd_status RenderMeshBuffer(
    RenderScene& scene, int32_t mesh_id, uint8_t kind,
    lightusd_buffer_view* out) noexcept {
  return lightusd_render_mesh_buffer(scene.get(), mesh_id, kind, out);
}

inline lightusd_status RenderMeshPrimvarBuffer(
    RenderScene& scene, int32_t mesh_id, size_t primvar_index, uint8_t which,
    lightusd_buffer_view* out) noexcept {
  return lightusd_render_mesh_primvar_buffer(scene.get(), mesh_id,
                                             primvar_index, which, out);
}

inline lightusd_status RenderMeshBlendshape(
    RenderScene& scene, int32_t mesh_id, size_t blendshape_index, uint8_t which,
    lightusd_sv* name, float* weight, lightusd_buffer_view* out) noexcept {
  return lightusd_render_mesh_blendshape(scene.get(), mesh_id,
                                         blendshape_index, which, name, weight,
                                         out);
}

inline lightusd_status RenderMaterialInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_material_info* out) noexcept {
  return lightusd_render_material_get_info(scene.get(), id, out);
}

inline lightusd_status RenderMaterialMtlxConfig(
    const RenderScene& scene, int32_t id,
    lightusd_render_materialx_config_info* out) noexcept {
  return lightusd_render_material_mtlx_config(scene.get(), id, out);
}

inline lightusd_status RenderTextureInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_texture_info* out) noexcept {
  return lightusd_render_texture_get_info(scene.get(), id, out);
}

inline lightusd_status RenderImageInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_image_info* out) noexcept {
  return lightusd_render_image_get_info(scene.get(), id, out);
}

inline lightusd_status RenderImageBuffer(
    RenderScene& scene, int32_t id, lightusd_buffer_view* out) noexcept {
  return lightusd_render_image_buffer(scene.get(), id, out);
}

inline lightusd_status RenderLightInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_light_info* out) noexcept {
  return lightusd_render_light_get_info(scene.get(), id, out);
}

inline size_t RenderLightLinkCount(const RenderScene& scene, int32_t id,
                                   uint8_t which) noexcept {
  return lightusd_render_light_link_count(scene.get(), id, which);
}

inline lightusd_status RenderLightLinkCopy(
    const RenderScene& scene, int32_t id, uint8_t which, size_t index,
    char* out, size_t cap, size_t* required) noexcept {
  return lightusd_render_light_link_copy(scene.get(), id, which, index, out,
                                          cap, required);
}

inline lightusd_status RenderCameraInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_camera_info* out) noexcept {
  return lightusd_render_camera_get_info(scene.get(), id, out);
}

inline lightusd_status RenderSkeletonInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_skeleton_info* out) noexcept {
  return lightusd_render_skeleton_get_info(scene.get(), id, out);
}

inline lightusd_status RenderSkeletonJoint(
    const RenderScene& scene, int32_t skeleton_id, size_t joint_index,
    lightusd_render_joint_info* out) noexcept {
  return lightusd_render_skeleton_joint(scene.get(), skeleton_id, joint_index,
                                        out);
}

inline lightusd_status RenderSkeletonJointChildrenCopy(
    const RenderScene& scene, int32_t skeleton_id, size_t joint_index,
    int32_t* out, size_t cap, size_t* required) noexcept {
  return lightusd_render_skeleton_joint_children_copy(
      scene.get(), skeleton_id, joint_index, out, cap, required);
}

inline lightusd_status RenderSkeletonBuffer(
    RenderScene& scene, int32_t skeleton_id, uint8_t kind,
    lightusd_buffer_view* out) noexcept {
  return lightusd_render_skeleton_buffer(scene.get(), skeleton_id, kind, out);
}

inline lightusd_status RenderInstancerInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_instancer_info* out) noexcept {
  return lightusd_render_instancer_get_info(scene.get(), id, out);
}

inline lightusd_status RenderPointInstanceDrawInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_point_instance_draw_info* out) noexcept {
  return lightusd_render_point_instance_draw_get_info(scene.get(), id, out);
}

inline lightusd_status RenderUnsupportedInfo(
    const RenderScene& scene, int32_t id,
    lightusd_render_unsupported_info* out) noexcept {
  return lightusd_render_unsupported_get_info(scene.get(), id, out);
}

inline lightusd_status RenderInstancerBuffer(
    RenderScene& scene, int32_t id, uint8_t kind,
    lightusd_buffer_view* out) noexcept {
  return lightusd_render_instancer_buffer(scene.get(), id, kind, out);
}

inline lightusd_status RenderPointsBuffer(RenderScene& scene, int32_t points_id,
                                          uint8_t kind, lightusd_buffer_view* out) noexcept {
  return lightusd_render_points_buffer(scene.get(), points_id, kind, out);
}

inline lightusd_status RenderCurvesBuffer(RenderScene& scene, int32_t curves_id,
                                          uint8_t kind, lightusd_buffer_view* out) noexcept {
  return lightusd_render_curves_buffer(scene.get(), curves_id, kind, out);
}

inline lightusd_status RenderCurvesInfo(const RenderScene& scene, int32_t curves_id,
                                        lightusd_render_curves_info* out) noexcept {
  return lightusd_render_curves_get_info(scene.get(), curves_id, out);
}

inline lightusd_status RenderAnimationInfo(
    const RenderScene& scene, int32_t animation_id,
    lightusd_render_animation_info* out) noexcept {
  return lightusd_render_animation_get_info(scene.get(), animation_id, out);
}

inline lightusd_status RenderAnimationChannelInfo(
    const RenderScene& scene, int32_t animation_id, size_t channel_index,
    lightusd_render_animation_channel_info* out) noexcept {
  return lightusd_render_animation_channel_get_info(
      scene.get(), animation_id, channel_index, out);
}

inline lightusd_status RenderAnimationChannelBuffer(
    RenderScene& scene, int32_t animation_id, size_t channel_index,
    uint8_t kind, lightusd_buffer_view* out) noexcept {
  return lightusd_render_animation_channel_buffer(
      scene.get(), animation_id, channel_index, kind, out);
}

inline lightusd_status RenderAnimationChannelStringCopy(
    const RenderScene& scene, int32_t animation_id, size_t channel_index,
    uint8_t which, size_t string_index, char* out, size_t cap,
    size_t* required) noexcept {
  return lightusd_render_animation_channel_string_copy(
      scene.get(), animation_id, channel_index, which, string_index, out, cap,
      required);
}

inline lightusd_status RenderAnimationClipAssetCopy(
    const RenderScene& scene, int32_t animation_id, size_t asset_index,
    char* out, size_t cap, size_t* required) noexcept {
  return lightusd_render_animation_clip_asset_copy(
      scene.get(), animation_id, asset_index, out, cap, required);
}

inline size_t RenderMaterialDiagnosticCount(const RenderScene& scene,
                                            int32_t material_id) noexcept {
  return lightusd_render_material_diagnostic_count(scene.get(), material_id);
}

inline lightusd_status RenderMaterialDiagnostic(
    const RenderScene& scene, int32_t material_id, size_t index,
    lightusd_render_material_diagnostic* out) noexcept {
  return lightusd_render_material_diagnostic_get(scene.get(), material_id,
                                                 index, out);
}

inline lightusd_status RenderMaterialTerminalPathCopy(
    const RenderScene& scene, int32_t material_id, uint8_t which, char* out,
    size_t cap, size_t* required) noexcept {
  return lightusd_render_material_terminal_path_copy(
      scene.get(), material_id, which, out, cap, required);
}

inline lightusd_status RenderMaterialNodegraphCopy(
    const RenderScene& scene, int32_t material_id, uint8_t which, char* out,
    size_t cap, size_t* required) noexcept {
  return lightusd_render_material_nodegraph_copy(scene.get(), material_id,
                                                  which, out, cap, required);
}

inline lightusd_status RenderMaterialParam(
    const RenderScene& scene, int32_t material_id, const char* param,
    int32_t* texture_id, float value[4]) noexcept {
  return lightusd_render_material_param(scene.get(), material_id, param,
                                        texture_id, value);
}

inline size_t RenderMaterialRetainedParamCount(const RenderScene& scene,
                                               int32_t material_id) noexcept {
  return lightusd_render_material_retained_param_count(scene.get(), material_id);
}

inline lightusd_status RenderMaterialRetainedParam(
    const RenderScene& scene, int32_t material_id, size_t index,
    lightusd_render_material_retained_param* out) noexcept {
  return lightusd_render_material_retained_param_get(scene.get(), material_id,
                                                      index, out);
}

inline lightusd_status Convert(const Stage& stage, RenderScene* scene,
                              const lightusd_render_config* config = nullptr) {
  if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_convert(stage.get(), config, scene->put());
}

inline lightusd_status ConvertMesh(const Stage& stage, lightusd_prim prim,
                                   const lightusd_render_config* config,
                                   int32_t subdivision_level,
                                   RenderScene* scene) {
  if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_convert_mesh(stage.get(), prim, config,
                                      subdivision_level, scene->put());
}

inline lightusd_status ConvertMeshProxy(
    const Stage& stage, lightusd_prim prim, uint8_t proxy_kind,
    const float bounds_min[3], const float bounds_max[3],
    RenderScene* scene) {
  if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_convert_mesh_proxy(stage.get(), prim, proxy_kind,
                                            bounds_min, bounds_max,
                                            scene->put());
}

inline lightusd_status ConvertInstancer(
    const Stage& stage, lightusd_prim prim,
    const lightusd_render_config* config, RenderScene* scene) {
  if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_convert_instancer(stage.get(), prim, config,
                                           scene->put());
}

inline lightusd_status ConvertMaterial(
    const Stage& stage, lightusd_prim prim,
    const lightusd_render_config* config, RenderScene* scene) {
  if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_render_convert_material(stage.get(), prim, config,
                                          scene->put());
}

class RenderSession;

class PreparedRenderUpdate {
 public:
  PreparedRenderUpdate() noexcept = default;
  ~PreparedRenderUpdate() { reset(); }
  PreparedRenderUpdate(const PreparedRenderUpdate&) = delete;
  PreparedRenderUpdate& operator=(const PreparedRenderUpdate&) = delete;
  PreparedRenderUpdate(PreparedRenderUpdate&& other) noexcept
      : session_(std::exchange(other.session_, nullptr)),
        handle_(std::exchange(other.handle_, nullptr)),
        lifetime_(std::move(other.lifetime_)) {}
  PreparedRenderUpdate& operator=(PreparedRenderUpdate&& other) noexcept {
    if (this != &other) {
      reset();
      session_ = std::exchange(other.session_, nullptr);
      handle_ = std::exchange(other.handle_, nullptr);
      lifetime_ = std::move(other.lifetime_);
    }
    return *this;
  }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  lightusd_status scene_copy(RenderScene* scene) const {
    if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_render_prepared_scene_copy(handle_, scene->put());
  }
  void reset() noexcept {
    if (handle_) {
      lightusd_render_session_abort(lifetime_.expired() ? nullptr : session_,
                                    handle_);
    }
    session_ = nullptr;
    handle_ = nullptr;
    lifetime_.reset();
  }

 private:
  friend class RenderSession;
  lightusd_render_session* session_ = nullptr;
  lightusd_render_prepared_update* handle_ = nullptr;
  std::weak_ptr<uint8_t> lifetime_;
};

class RenderSession {
 public:
  lightusd_status create(const Stage& stage,
                         const lightusd_render_config* config = nullptr) {
    invalidate_prepared();
    return lightusd_render_session_create(stage.get(), config, handle_.put());
  }
  lightusd_status create(const DocumentSession& document,
                         const lightusd_render_config* config = nullptr) {
    invalidate_prepared();
    return lightusd_render_session_create_document(document.get(), config,
                                                   handle_.put());
  }
  explicit operator bool() const noexcept { return bool(handle_); }
  uint64_t revision() const noexcept {
    return lightusd_render_session_revision(handle_.get());
  }
  uint64_t resource_id(uint8_t kind, const char* key) const noexcept {
    return lightusd_render_session_resource_id(handle_.get(), kind, key);
  }
  lightusd_status set_event_sink(const lightusd_render_event_sink* sink) {
    return lightusd_render_session_set_event_sink(handle_.get(), sink);
  }
  void reset() noexcept {
    invalidate_prepared();
    lightusd_render_session_reset(handle_.get());
  }
  lightusd_status update(const Stage& stage, RenderScene* scene,
                         lightusd_render_update_info* info = nullptr) {
    if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_render_session_update(handle_.get(), stage.get(),
                                          scene->put(), info);
  }
  lightusd_status apply(const Stage& stage,
                        const lightusd_render_change_set& changes,
                        RenderScene* scene,
                        lightusd_render_update_info* info = nullptr) {
    if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_render_session_apply(handle_.get(), stage.get(), &changes,
                                         scene->put(), info);
  }
  lightusd_status apply(const DocumentSnapshot& snapshot,
                        RenderScene* scene,
                        lightusd_render_update_info* info = nullptr) {
    if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_render_session_apply_document(handle_.get(), snapshot.get(),
                                                  scene->put(), info);
  }
  lightusd_status prepare(const Stage& stage,
                          const lightusd_render_change_set& changes,
                          lightusd_render_prepared_update** prepared,
                          lightusd_render_update_info* info = nullptr) {
    return lightusd_render_session_prepare(handle_.get(), stage.get(),
                                           &changes, prepared, info);
  }
  lightusd_status prepare(const DocumentSnapshot& snapshot,
                          lightusd_render_prepared_update** prepared,
                          lightusd_render_update_info* info = nullptr) {
    return lightusd_render_session_prepare_document(handle_.get(),
                                                    snapshot.get(), prepared,
                                                    info);
  }
  lightusd_status prepare(const DocumentSnapshot& snapshot,
                          PreparedRenderUpdate* prepared,
                          lightusd_render_update_info* info = nullptr) {
    if (!prepared) return LIGHTUSD_ERR_INVALID_ARG;
    prepared->reset();
    lightusd_render_prepared_update* handle = nullptr;
    const lightusd_status status = lightusd_render_session_prepare_document(
        handle_.get(), snapshot.get(), &handle, info);
    if (status == LIGHTUSD_OK) {
      prepared->session_ = handle_.get();
      prepared->handle_ = handle;
      prepared->lifetime_ = lifetime_;
    }
    return status;
  }
  lightusd_status prepare(const DocumentSnapshot& snapshot,
                          const lightusd_render_change_set& changes,
                          PreparedRenderUpdate* prepared,
                          lightusd_render_update_info* info = nullptr) {
    if (!prepared) return LIGHTUSD_ERR_INVALID_ARG;
    prepared->reset();
    lightusd_render_prepared_update* handle = nullptr;
    const auto status = lightusd_render_session_prepare_document_changes(
        handle_.get(), snapshot.get(), &changes, &handle, info);
    if (status == LIGHTUSD_OK) {
      prepared->session_ = handle_.get();
      prepared->handle_ = handle;
      prepared->lifetime_ = lifetime_;
    }
    return status;
  }
  lightusd_status prepare(const Stage& stage,
                          const lightusd_render_change_set& changes,
                          PreparedRenderUpdate* prepared,
                          lightusd_render_update_info* info = nullptr) {
    if (!prepared) return LIGHTUSD_ERR_INVALID_ARG;
    prepared->reset();
    lightusd_render_prepared_update* handle = nullptr;
    const lightusd_status status = lightusd_render_session_prepare(
        handle_.get(), stage.get(), &changes, &handle, info);
    if (status == LIGHTUSD_OK) {
      prepared->session_ = handle_.get();
      prepared->handle_ = handle;
      prepared->lifetime_ = lifetime_;
    }
    return status;
  }
  lightusd_status commit(lightusd_render_prepared_update* prepared,
                         RenderScene* scene,
                         lightusd_render_update_info* info = nullptr) {
    if (!scene) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_render_session_commit(handle_.get(), prepared,
                                          scene->put(), info);
  }
  lightusd_status commit(PreparedRenderUpdate* prepared, RenderScene* scene,
                         lightusd_render_update_info* info = nullptr) {
    if (!prepared || !scene || prepared->session_ != handle_.get() ||
        !prepared->handle_) {
      return LIGHTUSD_ERR_INVALID_ARG;
    }
    const lightusd_status status = lightusd_render_session_commit(
        handle_.get(), prepared->handle_, scene->put(), info);
    if (status == LIGHTUSD_OK) {
      prepared->session_ = nullptr;
      prepared->handle_ = nullptr;
      prepared->lifetime_.reset();
    }
    return status;
  }
  void abort(lightusd_render_prepared_update* prepared) noexcept {
    lightusd_render_session_abort(handle_.get(), prepared);
  }
  void abort(PreparedRenderUpdate* prepared) noexcept {
    if (prepared && prepared->session_ == handle_.get()) prepared->reset();
  }

 private:
  void invalidate_prepared() noexcept {
    lifetime_.reset();
    lifetime_ = std::make_shared<uint8_t>(0);
  }
  RenderSessionHandle handle_;
  std::shared_ptr<uint8_t> lifetime_ = std::make_shared<uint8_t>(0);
};

}  // namespace api
}  // namespace lightusd
