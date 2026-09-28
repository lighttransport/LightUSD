// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "asset-uuid.hh"
#include "binding-next-common.hh"
#include "security-policy.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/pcp/cache.hh"
#include "next/schema/geom-mesh.hh"
#include "next/stage/stage.hh"
#include "tydra/next/render-converter.hh"
namespace lightusd {
namespace tydra { namespace next { template <typename T> struct ValueArrayRead; } }
namespace web_next {
class NextAssetStore;
class NextLayerDocument;
class RenderStream {
 public:
  RenderStream();
  ~RenderStream();

  void setMaterialDedup(bool enabled) { material_dedup_ = enabled; }
  void setMeshMerge(bool enabled) { mesh_merge_ = enabled; }
  void setMeshMergeBakeTransform(bool enabled) {
    mesh_merge_bake_transform_ = enabled;
  }
  void setFlattenRenderTree(bool enabled) { flatten_render_tree_ = enabled; }
  void setMeshOnly(bool enabled) { mesh_only_ = enabled; }
  void setComputeTangents(bool enabled) { compute_tangents_ = enabled; }
  void setRenderSettingsPath(const std::string& path) {
    render_settings_path_ = path;
  }
  void setBuildVertexIndices(bool enabled) {
    build_vertex_indices_ = enabled;
    build_vertex_indices_set_ = true;
  }
  void setEnableComposition(bool enabled) { enable_composition_ = enabled; }
  void setEnableValueClips(bool enabled) { enable_value_clips_ = enabled; }
  void setLoadTextureInNative(bool enabled) { load_texture_in_native_ = enabled; }
  void setCombineUDIMTiles(bool enabled) { combine_udim_tiles_ = enabled; }
  int renderFlag(uint8_t kind) const {
    switch (kind) {
      case 0: return material_dedup_ ? 1 : 0;
      case 1: return mesh_merge_ ? 1 : 0;
      case 2: return mesh_merge_bake_transform_ ? 1 : 0;
      case 3: return flatten_render_tree_ ? 1 : 0;
      case 4: return mesh_only_ ? 1 : 0;
      case 5: return compute_tangents_ ? 1 : 0;
      case 6: return build_vertex_indices_ ? 1 : 0;
      case 7: return enable_composition_ ? 1 : 0;
      case 8: return enable_value_clips_ ? 1 : 0;
      case 9: return load_texture_in_native_ ? 1 : 0;
      case 10: return combine_udim_tiles_ ? 1 : 0;
      default: return -1;
    }
  }
  bool enableValueClips() const { return enable_value_clips_; }
  int sphereSubdivisions() const { return sphere_subdivisions_; }
  int setSphereSubdivisions(int value);
  bool enableBoneReduction() const { return enable_bone_reduction_; }
  int setEnableBoneReduction(bool enabled) {
    enable_bone_reduction_ = enabled;
    return 0;
  }
  uint32_t targetBoneCount() const { return target_bone_count_; }
  int setTargetBoneCount(uint32_t value);
  bool roundBoneCount() const { return round_bone_count_; }
  int setRoundBoneCount(bool enabled) {
    round_bone_count_ = enabled;
    return 0;
  }
  int setValueClipSetting(uint8_t field, double value);
  double valueClipSetting(uint8_t field) const;

  int provideAssetBytes(const uint8_t* name, uint32_t name_size,
                        const uint8_t* bytes, uint32_t byte_size);
  int importAssetStore(const NextAssetStore& store);
  int beginCachedAsset(NextAssetStore& store, const uint8_t* identifier,
                      uint32_t identifier_size);
  int clearImportedAssetStore();
  void reset();
  int startStreamingAsset(const uint8_t* name, uint32_t name_size,
                          uint32_t expected_size);
  int appendStreamingAsset(const uint8_t* name, uint32_t name_size,
                           const uint8_t* bytes, uint32_t byte_size);
  int streamingAssetBytesWritten(const uint8_t* name, uint32_t name_size) const;
  int streamingAssetExpectedBytes(const uint8_t* name, uint32_t name_size) const;
  int streamingAssetUuidCopy(const uint8_t* name, uint32_t name_size,
                             uint8_t* out, uint32_t cap) const;
  uintptr_t streamingAssetViewPtr(const uint8_t* name, uint32_t name_size,
                                  uint32_t byte_size) const;
  uintptr_t streamingAssetViewPtrAt(const uint8_t* name, uint32_t name_size,
                                    uint32_t offset, uint32_t byte_size) const;
  int markStreamingAssetBytesWritten(const uint8_t* name, uint32_t name_size,
                                     uint32_t byte_size);
  int markStreamingAssetRangeWritten(const uint8_t* name, uint32_t name_size,
                                     uint32_t offset, uint32_t byte_size);
  int finalizeStreamingAsset(const uint8_t* name, uint32_t name_size);
  int finalizeStreamingAssetToStore(const uint8_t* name, uint32_t name_size,
                                   NextAssetStore& store);
  int cancelStreamingAsset(const uint8_t* name, uint32_t name_size);
  int beginStreamingAssetAsRoot(const uint8_t* name, uint32_t name_size);
  int removeAssetBytes(const uint8_t* name, uint32_t name_size);
  int providedAssetCount() const;
  int providedAssetNameCopy(int asset_id, uint8_t* out, uint32_t cap) const;
  int providedAssetBytesCopy(const uint8_t* name, uint32_t name_size,
                             uint8_t* out, uint32_t cap) const;
  int setProvidedAssetByteLimit(uint32_t limit);
  uint32_t providedAssetByteLimit() const { return provided_asset_byte_limit_; }
  int setMaxInputBytes(uint32_t limit);
  uint32_t maxInputBytes() const { return max_input_bytes_; }
  int setMaxMemoryLimitMB(int32_t limit_mb);
  int32_t maxMemoryLimitMB() const { return max_memory_limit_mb_; }
  size_t maxMemoryLimitBytes() const {
    return static_cast<size_t>(max_memory_limit_mb_) * (size_t{1} << 20);
  }
  size_t remainingMemoryLimitBytes() const;
  size_t providedAssetMemoryBytes() const;
  void clearAssets() {
    clip_assets_.clear();
    clip_asset_budget_reservations_.clear();
    streaming_assets_.clear();
    asset_resolver_.ClearMemoryAssets();
    imported_asset_bytes_ = 0;
    payload_budget_.reset();
  }

  // Strongest variant selection. `key` is a variant-set name (applies to
  // every prim carrying that set) or the prim-scoped form
  // "<primPath>{<set>}" which wins over the bare-set key. Takes effect on
  // the next begin()/beginOwned().
  void setVariantOverride(const std::string &key,
                          const std::string &selection) {
    variant_overrides_[key] = selection;
  }
  void clearVariantOverrides() { variant_overrides_.clear(); }

  // Authored variant sets of the most recently loaded root layer (recorded
  // before composition consumes them). String kind: 0=prim path, 1=set name,
  // 2=selection, 3=variant name at variant_id.
  int variantSetCount() const;
  int variantNameCount(int set_id) const;
  int variantStringCopy(int set_id, int variant_id, uint8_t kind,
                        uint8_t* out, uint32_t cap) const;
  int layerAssetPathCount(uint8_t kind) const;
  int layerAssetPathCopy(uint8_t kind, int path_id, uint8_t* out,
                         uint32_t cap) const;
  int layerArcPresent(uint8_t kind) const;
  void setTangentMethod(const std::string &method);

  // Adopt the root bytes by move. USDC lazy arrays and USDA lazy slices retain
  // this buffer directly instead of copying it again inside the loader.
  bool beginOwned(std::string &&crate);

  // Begin from a JS Uint8Array (one copy into the WASM heap, then adopted).
  int preflightBeginInput(uint32_t size);
  int beginBytes(const uint8_t* bytes, uint32_t size);
  int beginFromLayerDocument(NextLayerDocument& document);
  int exportStageUSDC();
  int stageUSDCSize() const;
  uintptr_t stageUSDCData() const;

  int meshCount() const {
    if (!loaded_ && outputs_.empty()) return 0;
    const size_t authored = mesh_merge_ ? outputs_.size() : meshes_.size();
    return static_cast<int>(authored + analytic_outputs_.size());
  }

  int nodeCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.nodes.size()) : 0;
  }
  int nativeInstanceNodeIdsCopy(uint8_t* out, uint32_t cap) const;

  // Scalar node metadata: 0=type, 1=parent id, 2=data id, 3=visible,
  // 4=reset-xform-stack, 5=native instance.
  int nodeField(int node_id, uint8_t field) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const auto& node = render_scene_.nodes[static_cast<size_t>(node_id)];
    switch (field) {
      case 0: return static_cast<int>(node.type);
      case 1: return node.parent_id;
      case 2: return node.data_id;
      case 3: return node.visible ? 1 : 0;
      case 4: return node.has_reset_xform ? 1 : 0;
      case 5: return node.is_instance ? 1 : 0;
      default: return -1;
    }
  }

  int nodeChildId(int node_id, int child_index) const {
    if (!render_scene_valid_ || node_id < 0 || child_index < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const auto& children =
        render_scene_.nodes[static_cast<size_t>(node_id)].children;
    return static_cast<size_t>(child_index) < children.size()
               ? children[static_cast<size_t>(child_index)]
               : -1;
  }

  int nodeChildCount(int node_id) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const size_t count =
        render_scene_.nodes[static_cast<size_t>(node_id)].children.size();
    return count <= static_cast<size_t>((std::numeric_limits<int>::max)())
               ? static_cast<int>(count)
               : -1;
  }

  int rootNodeId(int root_index) const {
    if (!render_scene_valid_ || root_index < 0 ||
        static_cast<size_t>(root_index) >= render_scene_.root_nodes.size())
      return -1;
    return render_scene_.root_nodes[static_cast<size_t>(root_index)];
  }

  // Copy the stable prim-path key for a node into caller-owned WASM memory.
  // Returns the UTF-8 byte count; a short or null buffer only queries size.
  int nodePathCopy(int node_id, uint8_t* out, uint32_t cap) const;
  int nodePrototypePathCopy(int node_id, uint8_t* out, uint32_t cap) const;

  // Copy a node's local (kind 0) or world (kind 1) 4x4 float matrix.
  // Returns 64 bytes; a short or null buffer only queries size.
  int nodeTransformCopy(int node_id, uint8_t kind, uint8_t* out,
                        uint32_t cap) const;

  // Copy a path-bearing resource key. kind: 0=node, 1=mesh, 2=material,
  // 3=texture, 4=image, 5=light, 6=camera, 7=skeleton, 8=animation,
  // 9=unsupported, 10=instancer, 11=root node, 12=points, 13=curves,
  // 14=instance draw. Root-node IDs address the root list.
  int resourcePathCopy(uint8_t kind, int resource_id, uint8_t* out,
                       uint32_t cap) const;
  int resourceNameCopy(uint8_t kind, int resource_id, uint8_t* out,
                       uint32_t cap) const;
  // Copy the stable key for a scene record. Kinds match lightusd_render_kind;
  // root-node record indices refer to the root list, not the node array.
  int recordPathCopy(uint8_t kind, int record_id, uint8_t* out,
                     uint32_t cap) const;
  // Point-cloud payloads: kind 0=positions, 1=widths, 2=colors (float data).
  int pointsBufferCopy(int points_id, uint8_t kind, uint8_t* out,
                       uint32_t cap) const;
  int pointsInfo(int points_id, lightusd_next_points_info* out) const;
  // Curve payloads: kind 0=control points, 1=tessellated points,
  // 2=widths, 3=colors (float data), 4=authored vertex counts,
  // 5=tessellated vertex counts (uint32 data).
  int curvesBufferCopy(int curves_id, uint8_t kind, uint8_t* out,
                       uint32_t cap) const;
  int curvesInfo(int curves_id, lightusd_next_curves_info* out) const;
  // Curve fields: 0=type, 1=basis, 2=wrap, 3=is NURBS, 4=is Hermite.
  int curvesField(int curves_id, uint8_t field) const;
  // Point-instancer payloads: kind 0=compact records, 1=positions,
  // 2=orientations, 3=scales, 4=prototype indices, 5=visibility,
  // 6=prototype node ids, 7=prototype mesh offsets, 8=prototype mesh ids,
  // 9=prototype transforms.
  int instancerBufferCopy(int instancer_id, uint8_t kind, uint8_t* out,
                          uint32_t cap) const;
  int instancerInfo(int instancer_id, lightusd_next_instancer_info* out) const;
  int instancerStringCopy(int instancer_id, int prototype_id, uint8_t kind,
                          uint8_t* out, uint32_t cap) const;
  // Point-instance draw fields: 0=instancer id, 1=instance index,
  // 2=prototype index, 3=mesh id, 4=material id, 5=expanded mesh id.
  int pointInstanceDrawField(int draw_id, uint8_t field) const;
  // Copy one point-instance draw transform as 16 float values (64 bytes).
  int pointInstanceDrawTransformCopy(int draw_id, uint8_t* out,
                                     uint32_t cap) const;
  // Light fields: 0=type, 1=intensity*1e3, 2=exposure*1e3,
  // 3=normalize, 4=shadow enabled. Camera fields: 0=type,
  // 1=focal length*1e3, 2=ortho width*1e3, 3=near clip*1e3,
  // 4=far clip*1e3, 5=fov x*1e3, 6=fov y*1e3.
  int lightField(int light_id, uint8_t field) const;
  int lightTransformCopy(int light_id, uint8_t* out, uint32_t cap) const;
  int lightColorCopy(int light_id, uint8_t* out, uint32_t cap) const;
  int lightInfo(int light_id, lightusd_next_light_info* out) const;
  int lightStringCopy(int light_id, uint8_t kind, int item_id, uint8_t* out,
                      uint32_t cap) const;
  int lightMeshIdsCopy(int light_id, uint8_t kind, uint8_t* out,
                       uint32_t cap) const;
  int cameraField(int camera_id, uint8_t field) const;
  int cameraInfo(int camera_id, lightusd_next_camera_info* out) const;
  int cameraTransformCopy(int camera_id, uint8_t* out, uint32_t cap) const;
  int cameraOpticsCopy(int camera_id, uint8_t* out, uint32_t cap) const;
  // Skeleton fields: 0=joint count, 1=root joint, 2=animation id.
  int skeletonField(int skeleton_id, uint8_t field) const;
  // Skeleton joint payloads: kind 0=bind matrices, 1=rest matrices,
  // 2=parent indices.
  int skeletonJointBufferCopy(int skeleton_id, uint8_t kind, uint8_t* out,
                              uint32_t cap) const;
  int skeletonJointChildrenCopy(int skeleton_id, int joint_id, uint8_t* out,
                                uint32_t cap) const;
  int skeletonJointStringCopy(int skeleton_id, int joint_id, uint8_t kind,
                              uint8_t* out, uint32_t cap) const;
  int animationChannelCount(int animation_id) const;
  // Fields: 0=target node, 1=target skeleton, 2=keyframe count,
  // 3=element count, 4=value stride, 5=interpolation, 6=skeletal,
  // 7=target path, 8=joint-order count, 9=blend-shape-order count,
  // 10=joint-remap count.
  int animationChannelField(int animation_id, int channel_id,
                            uint8_t field) const;
  // Payload kinds: 0=keyframe times f64, 1=keyframe values f32
  // (path-dependent width), 2=full array values f32, 3=joint remap i32.
  int animationChannelBufferCopy(int animation_id, int channel_id,
                                 uint8_t kind, uint8_t* out,
                                 uint32_t cap) const;
  int animationArrayView(int animation_id, int channel_id,
                         lightusd_next_animation_array_view* out) const;
  int animationChannelStringCopy(int animation_id, int channel_id, uint8_t kind,
                                 uint8_t* out, uint32_t cap) const;
  int animationChannelOrderStringCopy(int animation_id, int channel_id,
                                      uint8_t kind, int order_id, uint8_t* out,
                                      uint32_t cap) const;

  // Scalar mesh metadata. field: 0=vertex count, 1=face/triangle count,
  // 2=material id, 3=has normals, 4=has UVs, 5=has tangents,
  // 6=has secondary UVs, 7=has colors, 8=has skin, 9=has bounds,
  // 10=skeleton id, 11=computed purpose (0=default, 1=render, 2=proxy,
  // 3=guide). Returns -1 for invalid ids.
  int meshField(int mesh_id, uint8_t field);
  int meshPrimvarCount(int mesh_id) const;
  int meshPrimvarField(int mesh_id, int primvar_id, uint8_t field) const;
  int meshPrimvarNameCopy(int mesh_id, int primvar_id, uint8_t* out,
                          uint32_t cap) const;
  int meshPrimvarBufferCopy(int mesh_id, int primvar_id, uint8_t kind,
                            uint8_t* out, uint32_t cap) const;

  // Copy one GPU payload into caller-owned WASM memory. kind: 0=points f32x3,
  // 1=indices u32, 2=normals f32x3, 3=primary UV f32x2, 4=tangents f32x4,
  // 5=colors f32, 6=opacities f32, 7=skin joint indices u16,
  // 8=skin joint weights f32, 9=secondary UV f32x2. Returns the required
  // byte count; a short or null buffer only queries the size. Returns -1 for
  // an invalid mesh/kind. This keeps payload transfer out of emval.
  /// Legacy computeMeshTangents: request tangents for one output mesh.
  /// Returns 1 for a valid mesh id and 0 otherwise.
  int requestMeshTangents(int mesh_id);
  int meshBufferCopy(int mesh_id, uint8_t kind, uint8_t* out,
                     uint32_t cap);
  int meshView(int mesh_id, lightusd_next_mesh_view* out);
  int meshViewStringCopy(int mesh_id, uint8_t kind, uint8_t* out,
                         uint32_t cap) const;
  int meshSubsetOutputCopy(int mesh_id, uint8_t* out, uint32_t cap);
  int meshBlendShapeCount(int mesh_id) const;
  int meshBlendShapeInfo(int mesh_id, int shape_id, int inbetween_id,
                         lightusd_next_blend_shape_info* out) const;
  int meshBlendShapeNameCopy(int mesh_id, int shape_id, int inbetween_id,
                             uint8_t* out, uint32_t cap) const;
  int meshBlendShapeOffsetsCopy(int mesh_id, int shape_id, int inbetween_id,
                                uint8_t kind, uint8_t* out, uint32_t cap);
  int outputMaterialInfo(int material_id,
                         lightusd_next_output_material_info* out) const;
  int outputMaterialStringCopy(int material_id, uint8_t kind, uint8_t* out,
                               uint32_t cap) const;
  int materialFormatStatus(int material_id, uint8_t format) const;
  int materialFormatStringCopy(int material_id, uint8_t format, uint8_t* out,
                               uint32_t cap) const;
  int lightFormatStatus(int light_id, uint8_t format) const;
  int lightFormatStringCopy(int light_id, uint8_t format, uint8_t* out,
                            uint32_t cap) const;
  int outputTextureMeta(int material_id, uint8_t slot,
                        lightusd_next_output_texture_meta* out) const;
  int outputTextureStringCopy(int material_id, uint8_t slot, uint8_t kind,
                              uint8_t* out, uint32_t cap) const;

  // Scalar material metadata. field: 0=shader type, 1=alpha mode,
  // 2=double-sided, 3=opacity*1000000, 4=roughness*1000000,
  // 5=clearcoat*1000000, 6=clearcoat roughness*1000000, 7=fallback.
  int materialField(int material_id, uint8_t field) const;
  int materialDiagnosticCount(int material_id) const;
  int materialDiagnosticKind(int material_id, int diagnostic_id) const;
  int materialDiagnosticStringCopy(int material_id, int diagnostic_id,
                                   uint8_t kind, uint8_t* out,
                                   uint32_t cap) const;
  // Common shader parameter slots: 0=base/diffuse color, 1=emissive,
  // 2=metallic, 3=roughness, 4=opacity. Returns four f32 values.
  int materialParamBufferCopy(int material_id, uint8_t param, uint8_t* out,
                              uint32_t cap) const;
  int materialParamTextureId(int material_id, uint8_t param) const;

  // Scalar texture metadata. field: 0=image id, 1=width, 2=height,
  // 3=channels, 4=mip levels, 5=loaded.
  int textureField(int texture_id, uint8_t field) const;
  int textureStringCopy(int texture_id, uint8_t kind, uint8_t* out,
                        uint32_t cap) const;

  // Copy decoded image bytes for a texture into caller-owned WASM memory.
  // Returns required bytes; a short or null buffer only queries the size.
  int textureImageBufferCopy(int texture_id, uint8_t* out, uint32_t cap) const;
  int textureSamplingBufferCopy(int texture_id, uint8_t* out,
                                uint32_t cap) const;
  int textureTransformBufferCopy(int texture_id, uint8_t* out,
                                 uint32_t cap) const;
  int textureUDIMRemapBufferCopy(int texture_id, uint8_t* out,
                                 uint32_t cap) const;
  int udimTileCount(int udim_id) const;
  int udimTextureCount() const;
  int udimTileBufferCopy(int udim_id, uint8_t* out, uint32_t cap) const;
  int udimStringCopy(int udim_id, uint8_t kind, uint8_t* out,
                     uint32_t cap) const;
  int textureColorTransformBufferCopy(int texture_id, uint8_t* out,
                                      uint32_t cap) const;

  // Scalar scene metadata. field: 0=meters/unit * 1e6, 1=up axis,
  // 2=start time * 1e3, 3=end time * 1e3, 4=fps * 1e3.
  int sceneField(uint8_t field) const;
  // Scene strings: 0=name, 1=default prim, 2=render-settings path,
  // 3=working color space. Returns required UTF-8 bytes excluding NUL.
  int sceneStringCopy(uint8_t kind, uint8_t* out, uint32_t cap) const;
  int sceneMetadata(lightusd_next_scene_metadata* out) const;
  // Unsupported renderable strings: 0=prim path, 1=type name, 2=reason.
  int unsupportedStringCopy(int unsupported_id, uint8_t kind, uint8_t* out,
                            uint32_t cap) const;
  int renderStats(lightusd_next_render_stats* out) const;
  int renderStatsDetail(lightusd_next_render_stats_detail* out) const;
  int animationInfo(int animation_id, lightusd_next_animation_info* out) const;
  int animationClipAssetCopy(int animation_id, int asset_id, uint8_t* out,
                             uint32_t cap) const;

  int lightCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.lights.size()) : 0;
  }

  int pointsCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.points.size()) : 0;
  }

  int curvesCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.curves.size()) : 0;
  }

  int cameraCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.cameras.size()) : 0;
  }
  int imageField(int image_id, uint8_t field) const;
  int imageAssetIdentifierCopy(int image_id, uint8_t* out,
                               uint32_t cap) const;
  int imageBufferCopy(int image_id, uint8_t* out, uint32_t cap) const;
  uintptr_t imageBufferData(int image_id) const;
  int imageCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.images.size()) : 0;
  }
  // Public material ids index the (optionally deduplicated) bound-material
  // table, registered in mesh order at load. Like legacy, the count excludes
  // the trailing fallback record used by unbound meshes.
  int materialCount() const {
    if (!render_scene_valid_) return 0;
    size_t count = materials_.size();
    if (count && materials_.back().prim_path == "__default") --count;
    return static_cast<int>(count);
  }
  int textureCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.textures.size()) : 0;
  }
  bool loaded() const { return loaded_; }
  int rootNodeCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.root_nodes.size()) : 0;
  }

  int pointInstancerCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.point_instancers.size())
                              : 0;
  }

  int pointInstanceDrawCount() const {
    return render_scene_valid_
               ? static_cast<int>(render_scene_.point_instance_draws.size())
               : 0;
  }


  int skeletonCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.skeletons.size()) : 0;
  }

  int animationCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.animations.size()) : 0;
  }

  int unsupportedRenderableCount() const {
    return render_scene_valid_
               ? static_cast<int>(render_scene_.unsupported_renderables.size())
               : 0;
  }

  const std::string& error() const { return error_; }
  const std::string& warning() const { return warning_; }
  const lightusd::next::pcp::CompositionReport& compositionReport() const {
    return composition_report_;
  }














  // Free the stage, mesh list and scratch (returns the heap to the allocator).
  void end();
  // Composition replaces the source root in Stage in place, so there is no
  // additional source-layer copy to release. Keep the legacy lifecycle hook.
  void releaseSourceLayer() {}

  void buildRenderScene_();
  int prepareMaterialFormat_(int material_id, uint8_t format) const;

 private:
  struct TextureMeta {
    std::string path;
    std::string source_color_space = "auto";
    std::string wrap_s = "useMetadata";
    std::string wrap_t = "useMetadata";
    bool is_udim = false;
    bool color_transform_valid = false;
    bool color_transform_bypass = true;
    bool source_color_is_data = false;
    float source_gamma = 1.0f;
    float source_linear_bias = 0.0f;
    std::array<float, 9> source_to_display_linear = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f};
  };

  struct MaterialRecord {
    int32_t id = -1;
    std::string key;
    std::string prim_path;
    float base_color[3] = {0.8f, 0.8f, 0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;
    float occlusion = 1.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float opacity_threshold = -1.0f;
    bool has_hair = false;
    float hair_tint_r[3] = {0.42f, 0.12f, 0.035f};
    float hair_tint_tt[3] = {0.32f, 0.075f, 0.018f};
    float hair_tint_trt[3] = {0.16f, 0.035f, 0.008f};
    float hair_roughness_r[2] = {0.22f, 0.35f};
    float hair_roughness_tt[2] = {0.32f, 0.45f};
    float hair_roughness_trt[2] = {0.42f, 0.55f};
    float hair_absorption[3] = {0.35f, 0.8f, 1.4f};
    float hair_ior = 1.55f;
    float hair_cuticle_angle = 3.0f;
    std::string base_color_texture;
    std::string normal_texture;
    std::string roughness_texture;
    std::string metallic_texture;
    std::string occlusion_texture;
    std::string emissive_texture;
    std::string opacity_texture;
    TextureMeta base_color_meta;
    TextureMeta normal_meta;
    TextureMeta roughness_meta;
    TextureMeta metallic_meta;
    TextureMeta occlusion_meta;
    TextureMeta emissive_meta;
    TextureMeta opacity_meta;
  };

  struct OutputMesh {
    bool merged = false;
    int source_index = -1;
    std::string name;
    std::string prim_path;
    std::vector<float> points;
    std::vector<float> normals;
    std::vector<float> uv;
    std::vector<uint32_t> indices;
    bool soup = false;
    int32_t material_id = -1;
    bool double_sided = false;
    // Computed UsdGeomImageable purpose: 0=default, 1=render, 2=proxy, 3=guide.
    uint8_t purpose = 0;
    std::array<double, 16> local_matrix;
    std::array<double, 16> world_matrix;
  };

  template <typename Chunked>
  static void copyChunked_(const Chunked& src, std::vector<float>* dst) {
    if (!dst) return;
    dst->resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) (*dst)[i] = src[i];
  }

  void buildAnalyticOutputs_();
  void remapNodeMeshIds_();
  static uint8_t purposeCode_(const lightusd::next::UsdPrim &prim);

  struct Stats {
    size_t source_mesh_count = 0;
    size_t source_material_count = 0;
    size_t source_texture_count = 0;
    size_t merged_mesh_count = 0;
    size_t merge_group_count = 0;
    size_t skipped_merge_count = 0;
    size_t input_bytes = 0;
    double input_copy_ms = 0.0;
    double stage_load_ms = 0.0;
    double composition_ms = 0.0;
    double mesh_discovery_ms = 0.0;
    double optimize_ms = 0.0;
    double material_ms = 0.0;
    double material_identity_ms = 0.0;
    double material_conversion_ms = 0.0;
    double geometry_build_ms = 0.0;
    double merge_append_ms = 0.0;
    size_t material_identity_hits = 0;
    size_t material_identity_misses = 0;
    size_t material_graph_cache_hits = 0;
    size_t material_graph_cache_misses = 0;
    size_t geometry_borrowed_bytes = 0;
    size_t geometry_materialized_bytes = 0;
  };

  const tr::RenderMesh* sourceRenderMesh_(int mesh_id) const;
  const tr::RenderMesh::BlendShape* outputBlendShape_(
      int mesh_id, int shape_id) const;

  struct MeshOnlyPrimvar {
    std::string name;
    const lightusd::next::Value* value = nullptr;
    const std::vector<int32_t>* indices = nullptr;
    tr::VertexFormat format = tr::VertexFormat::Float;
    tr::Interpolation interpolation = tr::Interpolation::Vertex;
    uint32_t components = 1;
    size_t scalar_count = 0;
    int32_t element_size = 1;
  };
  const std::vector<MeshOnlyPrimvar>* meshOnlyPrimvars_(int mesh_id) const;
  const std::vector<MeshOnlyPrimvar>* sourcePrimvars_(int source_index) const;

  const tr::RenderMaterial* outputRenderMaterial_(int material_id) const;
  const TextureMeta* outputTextureMeta_(int material_id, uint8_t slot) const;

  template <typename T>
  static void freeVec_(std::vector<T> &v) { std::vector<T>().swap(v); }

  tr::MeshConfig::TangentComputationMethod tangentMethod_() const;

  bool computeScratchTangents_();
  void flattenRenderTree_();
  // Legacy parity: tangents exist only for meshes whose bound material (or a
  // GeomSubset material) has a normal-map texture, either eagerly when
  // deferral is off or after an explicit computeMeshTangents request.
  bool wantsTangents_(int source_index) const;
  bool hasNormalMap_(const lightusd::next::UsdPrim &prim) const;
  void prepareScratchSkin_(const tr::RenderMesh* mesh);

  bool readFloatArray_(const lightusd::next::UsdPrim &prim, const char *name,
                       tr::ValueArrayRead<float> *out);
  bool readIntArray_(const lightusd::next::UsdPrim &prim, const char *name,
                     tr::ValueArrayRead<int32_t> *out);
  static bool matBool_(const lightusd::next::UsdPrim &prim, const char *name,
                       bool fallback);

  static bool pointsArePlanar_(const std::vector<float> &points);

  bool effectiveDoubleSided_(const lightusd::next::UsdPrim &prim,
                             int32_t material_id,
                             const std::vector<float> &points) const;

  // Build render geometry for one mesh into the scratch (s_points_/s_normals_/
  // s_uv_/s_indices_). Returns true if the result is a NON-INDEXED triangle soup
  // (drawn with drawArrays), false if INDEXED.
  //   - all primvars per-vertex  -> keep the indexed form directly (compact);
  //   - indexed UVs / per-vertex UV with face-varying normals -> de-index AND
  //     WELD inline (one vertex per distinct pos/uv/normal tuple), recovering
  //     vertex sharing while keeping correct attributes at seams;
  //   - PURE face-varying UVs (no st indices) -> emit the non-indexed soup, the
  //     minimal form when corners are mostly unique (welding would only add
  //     index + hash-map overhead).
  // The full soup is never materialized in the welded path; at most one mesh is
  // resident at a time either way.
  bool buildRenderMesh_(const lightusd::next::UsdPrim &prim, bool *soup_out,
                        std::string *err);

  static std::string fmtFloat_(float v) {
    std::ostringstream ss;
    ss << std::setprecision(9) << v;
    return ss.str();
  }

  static std::string normTexKey_(const std::string &path);

  static bool isUdimPath_(const std::string &path) {
    return path.find("<UDIM>") != std::string::npos ||
           path.find("%04d") != std::string::npos ||
           path.find("%(UDIM)d") != std::string::npos;
  }

  TextureMeta texMeta_(const std::string &connPath);

  static void addTextureKey_(const std::string &role,
                             const std::string &path,
                             std::set<std::string> *keys) {
    if (!keys || path.empty()) return;
    keys->insert(role + ":" + normTexKey_(path));
  }

  static void appendMaterialKey_(const MaterialRecord &m,
                                 std::ostringstream *ss);

  static bool populateHairMaterial_(const lightusd::next::UsdPrim &prim,
                                    MaterialRecord *rec);

  bool ensureRenderMaterial_(const lightusd::next::UsdPrim &mat);

  bool requiresRenderMaterial_(const lightusd::next::UsdPrim &mat) const;

  std::string materialSourceIdentity_(
      const lightusd::next::UsdPrim &mat) const;

  MaterialRecord materialRecordForPrim_(
      const lightusd::next::UsdPrim &mat);

  int32_t registerMaterial_(const lightusd::next::UsdPrim &mat);

  int32_t materialIdForBoundPrim_(const lightusd::next::UsdPrim &prim);

  static bool hasGeomSubset_(const lightusd::next::UsdPrim &prim);

  static bool sameMatrix_(const std::array<double, 16> &a,
                          const std::array<double, 16> &b);

  static std::string matrixKey_(const std::array<double, 16> &m);

  static void transformPoint_(const std::array<double, 16> &m,
                              float *x, float *y, float *z);

  static void transformNormal_(const std::array<double, 16> &m,
                               float *x, float *y, float *z);

  struct MergeAccumulator {
    OutputMesh mesh;
    size_t source_count = 0;
    int first_source_index = -1;
  };

  static size_t triangleIndexCount_(const std::vector<uint32_t> &indices,
                                    const std::vector<float> &points) {
    return indices.empty() ? points.size() / 3 : indices.size();
  }

  void flushAccumulator_(MergeAccumulator *acc);

  bool appendToAccumulator_(const lightusd::next::UsdPrim &prim,
                            int source_index,
                            int32_t material_id,
                            bool double_sided,
                            bool soup,
                            MergeAccumulator *acc);

  void buildOptimizedOutputs_();

  // Resolve a UsdUVTexture connection path ("/.../Tex.outputs:rgb") to its
  // inputs:file asset path, which the JS caller maps to an archive texture entry.
  std::string texFile_(const std::string &connPath);

  // Triangulate faceVertexIndices grouped by faceVertexCounts. Quads use the
  // shorter diagonal, matching the full render converter; larger polygons
  // retain the bounded fan fallback used by the mesh-only fast path.
  template <typename FloatArray, typename IndexArray, typename CountArray>
  static void triangulate_(const FloatArray &points,
                           const IndexArray &fvi,
                           const CountArray &fvc,
                           std::vector<uint32_t> &out) {
    out.clear();
    if (fvi.empty()) return;
    if (fvc.empty()) {  // assume an already-triangulated index list
      out.reserve(fvi.size());
      for (int32_t v : fvi) {
        if (v >= 0) out.push_back(static_cast<uint32_t>(v));
      }
      return;
    }
    size_t base = 0;
    auto faceSpanAvailable = [](size_t base, int32_t n, size_t total) {
      if (n < 3) return false;
      const size_t count = static_cast<size_t>(n);
      return base <= total && count <= total - base;
    };
    auto advanceFaceBase = [](size_t base, int32_t n) {
      if (n <= 0) return base;
      const size_t add = static_cast<size_t>(n);
      if (base > (std::numeric_limits<size_t>::max)() - add) {
        return (std::numeric_limits<size_t>::max)();
      }
      return base + add;
    };
    auto quadUsesDiagonal13 = [&](size_t base) {
      if (base > fvi.size() || 4 > fvi.size() - base) return false;
      const int32_t ids[4] = {fvi[base], fvi[base + 1],
                              fvi[base + 2], fvi[base + 3]};
      const size_t point_count = points.size() / 3;
      for (int32_t id : ids) {
        if (id < 0 || static_cast<size_t>(id) >= point_count) return false;
      }
      auto distSq = [&](int32_t a, int32_t b) {
        const size_t ia = static_cast<size_t>(a) * 3;
        const size_t ib = static_cast<size_t>(b) * 3;
        const float dx = points[ia] - points[ib];
        const float dy = points[ia + 1] - points[ib + 1];
        const float dz = points[ia + 2] - points[ib + 2];
        return dx * dx + dy * dy + dz * dz;
      };
      return distSq(ids[1], ids[3]) < distSq(ids[0], ids[2]);
    };
    for (int32_t n : fvc) {
      if (!faceSpanAvailable(base, n, fvi.size())) {
        base = advanceFaceBase(base, n);
        continue;
      }
      if (n == 4 && quadUsesDiagonal13(base)) {
        const size_t corners[6] = {base, base + 1, base + 3,
                                   base + 1, base + 2, base + 3};
        for (size_t corner : corners) {
          out.push_back(static_cast<uint32_t>(fvi[corner]));
        }
      } else for (int32_t k = 2; k < n; ++k) {
        const int32_t a = fvi[base];
        const int32_t b = fvi[base + static_cast<size_t>(k) - 1];
        const int32_t c = fvi[base + static_cast<size_t>(k)];
        if (a < 0 || b < 0 || c < 0) continue;
        out.push_back(static_cast<uint32_t>(a));
        out.push_back(static_cast<uint32_t>(b));
        out.push_back(static_cast<uint32_t>(c));
      }
      base = advanceFaceBase(base, n);
    }
  }

  static std::vector<uint32_t> faceTriangleStarts_(
      const std::vector<int32_t> &fvc);

  static std::vector<int32_t> matIntStatic_(
      const lightusd::next::UsdPrim &prim, const char *name);

  // Area-weighted vertex normals from the triangulated indices.
  static void computeNormals_(const std::vector<float> &pos,
                              const std::vector<uint32_t> &idx,
                              std::vector<float> &out);

  static std::array<double, 16> identityMatrix_();
  static std::array<double, 16> multiplyMatrix_(
      const std::array<double, 16> &a, const std::array<double, 16> &b);
  void buildMeshTransformCaches_();
  std::array<double, 16> localMatrix_(
      const lightusd::next::UsdPrim &prim) const;

  std::array<double, 16> worldMatrix_(
      const lightusd::next::UsdPrim &prim) const;

  // World transform for a prim, preferring the RenderScene node table: its
  // hierarchy traversal handles native instances correctly, while the plain
  // GetParent() chain in worldMatrix_ drops the instance root's own xform.
  std::array<double, 16> worldMatrixForPrim_(
      const lightusd::next::UsdPrim &prim) const;

  lightusd::next::Stage stage_;
  tr::RenderScene render_scene_;
  bool render_scene_valid_ = false;
  std::vector<std::string> render_scene_warnings_;
  std::vector<lightusd::next::UsdGeomMesh> meshes_;
  std::vector<OutputMesh> outputs_;
  std::vector<OutputMesh> analytic_outputs_;
  std::vector<MaterialRecord> materials_;
  mutable int32_t formatted_material_id_ = -1;
  mutable uint8_t formatted_material_format_ = 0xff;
  mutable int32_t formatted_material_status_ = 0;
  mutable std::string formatted_material_text_;
  mutable int32_t formatted_light_id_ = -1;
  mutable uint8_t formatted_light_format_ = 0xff;
  mutable int32_t formatted_light_status_ = 0;
  mutable bool formatted_light_cached_ = false;
  mutable std::string formatted_light_text_;
  std::unordered_map<std::string, int32_t> material_key_to_id_;
  std::unordered_map<std::string, int32_t> material_path_to_id_;
  std::unordered_map<std::string, int32_t> material_identity_to_id_;
  mutable std::unordered_map<std::string, std::array<double, 16>>
      local_matrix_cache_;
  mutable std::unordered_map<std::string, std::array<double, 16>>
      world_matrix_cache_;
  std::set<std::string> source_material_keys_;
  std::set<std::string> source_texture_keys_;
  std::set<std::string> texture_keys_;
  std::map<std::string, std::string> clip_assets_;
  std::map<std::string, std::shared_ptr<void>> clip_asset_budget_reservations_;
  lightusd::next::AssetResolver asset_resolver_;
  std::shared_ptr<lightusd::next::AssetPayloadBudget> payload_budget_;
  size_t imported_asset_bytes_{0};
  struct StreamingAsset {
    std::string bytes;
    std::string uuid;
    std::shared_ptr<void> budget_reservation;
    uint32_t expected_size{0};
    uint32_t cursor{0};
    uint32_t written_size{0};
    static constexpr size_t kMaxWrittenRanges = 128;
    std::array<std::pair<uint32_t, uint32_t>, kMaxWrittenRanges> written_ranges{};
    size_t written_range_count{0};
  };
  size_t streamingRangeBookkeepingBytes_() const;
  int markStreamingRange_(StreamingAsset& stream, uint32_t offset,
                          uint32_t byte_size);
  std::map<std::string, StreamingAsset> streaming_assets_;
  uint32_t provided_asset_byte_limit_{0};  // 0 = unlimited
  uint32_t max_input_bytes_{static_cast<uint32_t>(
      security_policy::kDefaultInputLimitBytes)};
  // Next-only untrusted-load policy: keep one architecture-independent 1 GiB
  // resident default, independent of legacy WASM's 2/8 GiB render defaults.
  int32_t max_memory_limit_mb_{1024};
  struct VariantSetInfo {
    std::string prim_path;
    std::string set_name;
    std::string selected;
    std::vector<std::string> variant_names;
  };

  void collectVariantSets_();

  std::vector<VariantSetInfo> variant_sets_;
  std::map<std::string, std::string> variant_overrides_;

  Stats stats_;
  double pending_input_copy_ms_ = 0.0;
  size_t pending_input_bytes_ = 0;
  bool loaded_ = false;
  bool authored_has_inherits_ = false;
  bool authored_has_specializes_ = false;
  // Root-layer sublayer/reference/payload asset paths and arc presence as
  // authored, captured before in-place composition consumes the arcs.
  std::vector<std::string> authored_arc_paths_[3];
  bool authored_arc_present_[3] = {false, false, false};
  bool material_dedup_ = false;
  bool mesh_merge_ = false;
  bool mesh_merge_bake_transform_ = true;  // legacy default: bake merged meshes
  bool flatten_render_tree_ = false;
  bool mesh_only_ = false;
  bool compute_tangents_ = false;
  std::vector<uint8_t> tangent_requests_;  // per source mesh index
  bool build_vertex_indices_ = true;
  bool build_vertex_indices_set_ = false;
  bool enable_composition_ = true;
  bool enable_value_clips_ = true;
  bool load_texture_in_native_ = false;
  bool combine_udim_tiles_ = false;
  bool enable_bone_reduction_ = false;
  uint32_t target_bone_count_ = 4;
  bool round_bone_count_ = false;
  int sphere_subdivisions_ = 4;
  float value_clip_sample_rate_ = 0.0f;
  bool value_clip_use_time_range_ = false;
  double value_clip_start_time_ = 0.0;
  double value_clip_end_time_ = 0.0;
  std::string tangent_method_ = "hybrid";
  std::string render_settings_path_;
  std::string error_;
  std::string warning_;
  lightusd::next::pcp::CompositionReport composition_report_;
  std::string mesh_view_error_;
  int mesh_view_source_id_ = -1;
  std::vector<float> s_points_, s_normals_, s_uv_, s_tangents_;
  std::vector<uint8_t> stage_usdc_export_;
  mutable std::unordered_map<int, std::vector<MeshOnlyPrimvar>>
      source_primvars_;
  std::vector<uint32_t> s_indices_;
  std::vector<uint32_t> s_point_source_indices_;
  std::vector<uint16_t> s_joint_indices_;
  std::vector<float> s_joint_weights_;
};
}  // namespace web_next
}  // namespace lightusd
