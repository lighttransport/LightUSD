// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-api.h"
#include "binding-next-assets.hh"
#include "binding-next-layer.hh"
#include "binding-next-render.hh"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

namespace {
static_assert(sizeof(lightusd_next_flatten_step_info) == 80,
              "flatten step POD layout changed");
static_assert(sizeof(lightusd_next_diff_options) == 24,
              "diff options POD layout changed");
static_assert(sizeof(lightusd_next_rewrite_options) == 24 &&
                  sizeof(lightusd_next_rewrite_info) == 40,
              "converter rewrite POD layout changed");
static_assert(offsetof(lightusd_next_mesh_view, local_matrix) == 24,
              "mesh view matrix layout changed");
static_assert(offsetof(lightusd_next_mesh_view, ptr) == 408,
              "mesh view pointer layout changed");
static_assert(offsetof(lightusd_next_mesh_view, length) == 464,
              "mesh view length layout changed");
static_assert(sizeof(lightusd_next_mesh_view) <= 496,
              "mesh view exceeds the JS-owned POD block");
static_assert(offsetof(lightusd_next_output_material_info, value) == 16 &&
                  sizeof(lightusd_next_output_material_info) == 176,
              "output material info must match the JS view");
static_assert(offsetof(lightusd_next_output_texture_meta,
                       source_to_display_linear) == 16 &&
                  sizeof(lightusd_next_output_texture_meta) == 52,
              "output texture metadata must match the JS view");
static_assert(sizeof(lightusd_next_blend_shape_info) == 16,
              "blend-shape info must match the JS view");
struct Slot {
  void* object;
  uint32_t generation;
  uint32_t kind;
};
Slot* slots = nullptr;
uint32_t slot_count = 0;
constexpr uint32_t index_bits = 20;
constexpr uint32_t index_mask = (1u << index_bits) - 1u;
constexpr uint32_t max_generation = (1u << (32u - index_bits)) - 1u;

Slot* Lookup(uint32_t handle) {
  const uint32_t index = handle & index_mask;
  if (!index || index > slot_count) return nullptr;
  Slot* slot = &slots[index - 1];
  return slot->object && slot->generation == (handle >> index_bits) ? slot : nullptr;
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_next_create(uint32_t kind) {
  using namespace lightusd::web_next;
  if (kind < 1 || kind > 6) return 0;
  uint32_t index = 0;
  while (index < slot_count && (slots[index].object || !slots[index].generation)) ++index;
  if (index == slot_count) {
    if (slot_count == index_mask) return 0;
    void* allocation = std::realloc(slots, (size_t(slot_count) + 1) * sizeof(Slot));
    if (!allocation) return 0;
    slots = static_cast<Slot*>(allocation);
    slots[slot_count++] = Slot{nullptr, 1, 0};
  }
  void* object = NextCreateObject(kind);
  if (!object) return 0;
  slots[index].object = object;
  slots[index].kind = kind;
  return (slots[index].generation << index_bits) | (index + 1);
}

EMSCRIPTEN_KEEPALIVE uint8_t* lightusd_next_alloc(uint32_t size) {
  return size ? static_cast<uint8_t*>(std::malloc(size)) : nullptr;
}

EMSCRIPTEN_KEEPALIVE void lightusd_next_free(uint8_t* ptr) { std::free(ptr); }

EMSCRIPTEN_KEEPALIVE void lightusd_next_destroy(uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot) return;
  void* object = slot->object;
  const uint32_t kind = slot->kind;
  slot->object = nullptr;
  // Never wrap: a retired slot cannot make an old handle valid again.
  slot->generation = slot->generation == max_generation ? 0 : slot->generation + 1;
  lightusd::web_next::NextDestroyObject(kind, object);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_subdiv_refine(
    uint32_t handle, const float* points, uint32_t point_values,
    const uint32_t* face_counts, uint32_t face_count,
    const uint32_t* face_indices, uint32_t index_count,
    const float* uv_values, uint32_t uv_value_count,
    const uint32_t* uv_indices, uint32_t uv_index_count,
    const lightusd_next_subdiv_options* options, uint32_t callback_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 2 || !options ||
      options->struct_size < sizeof(lightusd_next_subdiv_options) ||
      (!points && point_values) || (!face_counts && face_count) ||
      (!face_indices && index_count) || (!uv_values && uv_value_count) ||
      (!uv_indices && uv_index_count) || !callback_id) return -1;
  return lightusd::web_next::NextSubdivRefine(
      slot->object, points, point_values, face_counts, face_count,
      face_indices, index_count, uv_values, uv_value_count,
      uv_indices, uv_index_count, options, callback_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_subdiv_error(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 2) return -1;
  return lightusd::web_next::NextSubdivError(slot->object, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_udim(
    uint32_t handle, uint32_t apply, const uint8_t* data, uint32_t size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || apply > 1 || size > 4*1024*1024 ||
      (size && (!data || reinterpret_cast<uintptr_t>(data) > emscripten_get_heap_size() || size > emscripten_get_heap_size() - reinterpret_cast<uintptr_t>(data)))) return -1;
  return lightusd::web_next::NextConverterUDIM(slot->object, apply != 0, data, size);
}
EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_udim_copy(uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || (cap && (!out || reinterpret_cast<uintptr_t>(out) > emscripten_get_heap_size() || cap > emscripten_get_heap_size()-reinterpret_cast<uintptr_t>(out)))) return -1;
  return lightusd::web_next::NextConverterUDIMCopy(slot->object, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_rewrite(
    uint32_t handle, const uint8_t* data, uint32_t size,
    const lightusd_next_rewrite_options* options,
    lightusd_next_rewrite_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || !options || !out ||
      options->struct_size < sizeof(lightusd_next_rewrite_options) ||
      out->struct_size < sizeof(lightusd_next_rewrite_info) ||
      size > (uint32_t{1} << 30) || (size && !data) || options->format > 1 ||
      !std::isfinite(options->max_memory) || options->max_memory < 0 ||
      options->max_memory >= std::ldexp(1.0, sizeof(size_t) * 8)) return -1;
  return lightusd::web_next::NextConverterRewrite(
      slot->object, data, size, options, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_rewrite_buffer(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1) return -1;
  return lightusd::web_next::NextConverterRewriteBuffer(slot->object, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_string(
    uint32_t handle, uint8_t kind, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || kind > 1) return -1;
  return lightusd::web_next::NextConverterString(slot->object, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_set_bytes(
    uint32_t handle, uint8_t kind, const uint8_t* name, uint32_t name_size,
    const uint8_t* data, uint32_t size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || kind > 2 || name_size > (1u << 20) ||
      size > (1u << 30) || (name_size && !name) || (size && !data)) return -1;
  return lightusd::web_next::NextConverterSetBytes(
      slot->object, kind, name, name_size, data, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_asset_preflight(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t byte_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || name_size > (1u << 20) ||
      (name_size && !name) || byte_size > (1u << 30)) return -1;
  return lightusd::web_next::NextConverterPreflightAsset(
      slot->object, name, name_size, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_mesh_preflight(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t position_count, uint32_t normal_count, uint32_t uv_count,
    uint32_t index_count) {
  const Slot* slot = Lookup(handle);
  constexpr uint32_t max_values = 1u << 28;
  if (!slot || slot->kind != 1 || name_size > (1u << 20) ||
      (name_size && !name) || position_count > max_values ||
      normal_count > max_values || uv_count > max_values ||
      index_count > max_values) return -1;
  return lightusd::web_next::NextConverterPreflightMesh(
      slot->object, name, name_size, position_count, normal_count, uv_count,
      index_count);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_set_mesh(
    uint32_t handle, uint8_t kind, const uint8_t* name, uint32_t name_size,
    const float* positions, uint32_t position_count,
    const float* normals, uint32_t normal_count,
    const float* uvs, uint32_t uv_count,
    const uint32_t* indices, uint32_t index_count) {
  const Slot* slot = Lookup(handle);
  constexpr uint32_t max_values = 1u << 28;
  if (!slot || slot->kind != 1 || kind > 1 || name_size > (1u << 20) ||
      (name_size && !name) || position_count > max_values ||
      normal_count > max_values || uv_count > max_values ||
      index_count > max_values || (position_count && !positions) ||
      (normal_count && !normals) || (uv_count && !uvs) ||
      (index_count && !indices)) return -1;
  return lightusd::web_next::NextConverterSetMesh(
      slot->object, name, name_size, positions, position_count,
      normals, normal_count, uvs, uv_count, indices, index_count);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_control(
    uint32_t handle, uint8_t kind, int32_t first, int32_t second) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || kind > 7) return -1;
  return lightusd::web_next::NextConverterControl(
      slot->object, kind, first, second);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_root_preflight(
    uint32_t handle, uint32_t size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1) return -1;
  return lightusd::web_next::NextConverterRootPreflight(slot->object, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_export(
    uint32_t handle, uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || kind > 3) return -1;
  return lightusd::web_next::NextConverterExport(slot->object, kind);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_export_with_options(
    uint32_t handle, uint8_t kind, uint8_t root_format) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1) return -1;
  return lightusd::web_next::NextConverterExportWithOptions(
      slot->object, kind, root_format);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_export_with_remap(
    uint32_t handle, const uint8_t* remap_json, uint32_t remap_json_size,
    uint8_t root_format) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1 || (!remap_json && remap_json_size)) return -1;
  return lightusd::web_next::NextConverterExportWithRemap(
      slot->object, remap_json, remap_json_size, root_format);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_converter_export_buffer(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1) return -1;
  return lightusd::web_next::NextConverterExportBuffer(slot->object, out, cap);
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_converter_export_data(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 1) return 0;
  return reinterpret_cast<uintptr_t>(
      lightusd::web_next::NextConverterExportData(slot->object));
}

EMSCRIPTEN_KEEPALIVE uint8_t* lightusd_next_validate_json(
    const uint8_t* data, uint32_t size, const uint8_t* filename,
    uint32_t filename_size, const uint8_t* options_json,
    uint32_t options_size) {
  return lightusd::web_next::NextValidateJSON(
      data, size, filename, filename_size, options_json, options_size);
}

EMSCRIPTEN_KEEPALIVE uint8_t* lightusd_next_check_json(
    const uint8_t* data, uint32_t size, const uint8_t* filename,
    uint32_t filename_size, const uint8_t* options_json,
    uint32_t options_size, uint32_t asset_store) {
  const Slot* store = asset_store ? Lookup(asset_store) : nullptr;
  if (asset_store && (!store || store->kind != 5)) return nullptr;
  return lightusd::web_next::NextCheckJSON(data, size, filename, filename_size,
      options_json, options_size, store ? store->object : nullptr);
}

EMSCRIPTEN_KEEPALIVE uint8_t* lightusd_next_diff_json(
    const lightusd_next_diff_options* options,
    const uint8_t* left, uint32_t left_size,
    const uint8_t* left_name, uint32_t left_name_size,
    const uint8_t* right, uint32_t right_size,
    const uint8_t* right_name, uint32_t right_name_size) {
  return lightusd::web_next::NextDiffJSON(
      options, left, left_size, left_name, left_name_size,
      right, right_size, right_name, right_name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_count(uint32_t handle,
                                                        uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const auto* stream = static_cast<const lightusd::web_next::RenderStream*>(
      slot->object);
  switch (kind) {
    case 0: return stream->meshCount();
    case 1: return stream->nodeCount();
    case 2: return stream->lightCount();
    case 3: return stream->pointsCount();
    case 4: return stream->curvesCount();
    case 5: return stream->cameraCount();
    case 6: return stream->pointInstancerCount();
    case 7: return stream->pointInstanceDrawCount();
    case 8: return stream->skeletonCount();
    case 9: return stream->unsupportedRenderableCount();
    case 10: return stream->animationCount();
    case 11: return stream->imageCount();
    case 12: return stream->materialCount();
    case 13: return stream->textureCount();
    case 14: return stream->rootNodeCount();
    case 15: return stream->rootNodeId(0);
    default: return -1;
  }
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_loaded(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->loaded() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_begin(
    uint32_t handle, const uint8_t* root, uint32_t root_size,
    const uint8_t* name, uint32_t name_size, uint8_t lazy_arrays) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenBegin(
      slot->object, root, root_size, name, name_size, lazy_arrays != 0);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_provide_layer(
    uint32_t handle, const uint8_t* key, uint32_t key_size,
    const uint8_t* data, uint32_t data_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenProvideLayer(
      slot->object, key, key_size, data, data_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_add_sublayer(
    uint32_t handle, const uint8_t* path, uint32_t path_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3 || !path || path_size == 0 ||
      path_size > (1u << 20)) return -1;
  return lightusd::web_next::NextFlattenAddSublayer(
      slot->object, path, path_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_add_prim_arc(
    uint32_t handle, uint8_t kind, uint8_t list_op, const uint8_t* prim_path,
    uint32_t prim_size, const uint8_t* asset_path, uint32_t asset_size,
    const uint8_t* target_path, uint32_t target_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3 || kind > 3 || list_op > 5 ||
      !prim_path || !prim_size ||
      !target_path || !target_size || (asset_size && !asset_path) ||
      prim_size > (1u << 20) || asset_size > (1u << 20) ||
      target_size > (1u << 20)) return -1;
  return lightusd::web_next::NextFlattenAddPrimArc(
      slot->object, kind, list_op, prim_path, prim_size, asset_path, asset_size,
      target_path, target_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_set_asset_path_remap(
    uint32_t handle, const uint8_t* json, uint32_t json_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3 || (json_size && !json) ||
      json_size > (1u << 30)) return -1;
  return lightusd::web_next::NextFlattenSetAssetPathRemap(
      slot->object, json, json_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_remap_layer_asset_paths(
    uint32_t handle, const uint8_t* json, uint32_t json_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3 || (json_size && !json) ||
      json_size > (1u << 30)) return -1;
  return lightusd::web_next::NextFlattenRemapLayerAssetPaths(
      slot->object, json, json_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_set_max_input_bytes(
    uint32_t handle, uint32_t limit_bytes) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenSetMaxInputBytes(slot->object,
                                                         limit_bytes);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_max_input_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenMaxInputBytes(slot->object);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_set_max_output_bytes(
    uint32_t handle, uint32_t limit_bytes) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenSetMaxOutputBytes(slot->object,
                                                          limit_bytes);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_max_output_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenMaxOutputBytes(slot->object);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_input_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenInputBytes(slot->object);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_preflight_layer(
    uint32_t handle, const uint8_t* key, uint32_t key_size,
    uint32_t data_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenPreflightLayer(
      slot->object, key, key_size, data_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_set_variant(
    uint32_t handle, const uint8_t* key, uint32_t key_size,
    const uint8_t* selection, uint32_t selection_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenSetVariant(
      slot->object, key, key_size, selection, selection_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_end(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  lightusd::web_next::NextFlattenEnd(slot->object);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_error(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenError(slot->object, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_step(
    uint32_t handle, uint32_t callback_id,
    lightusd_next_flatten_step_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3 || !out ||
      out->struct_size < sizeof(lightusd_next_flatten_step_info)) return -1;
  return lightusd::web_next::NextFlattenStep(slot->object, callback_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_step_buffer(
    uint32_t handle, uint8_t kind, uint32_t index, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenStepBuffer(
      slot->object, kind, index, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_layer_dependency_count(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  return lightusd::web_next::NextFlattenLayerDependencyCount(slot->object);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_flatten_release_step(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 3) return -1;
  lightusd::web_next::NextFlattenReleaseStep(slot->object);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_flag(
    uint32_t handle, uint8_t kind, uint8_t enabled) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  auto* stream = static_cast<lightusd::web_next::RenderStream*>(slot->object);
  const bool value = enabled != 0;
  switch (kind) {
    case 0: stream->setMaterialDedup(value); break;
    case 1: stream->setMeshMerge(value); break;
    case 2: stream->setMeshMergeBakeTransform(value); break;
    case 3: stream->setFlattenRenderTree(value); break;
    case 4: stream->setMeshOnly(value); break;
    case 5: stream->setComputeTangents(value); break;
    case 6: stream->setBuildVertexIndices(value); break;
    case 7: stream->setEnableComposition(value); break;
    case 8: stream->setEnableValueClips(value); break;
    case 9: stream->setLoadTextureInNative(value); break;
    case 10: stream->setCombineUDIMTiles(value); break;
    default: return -1;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_flag(uint32_t handle,
                                                       uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const auto* stream = static_cast<const lightusd::web_next::RenderStream*>(slot->object);
  return stream->renderFlag(kind);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_text(
    uint32_t handle, uint8_t kind, const uint8_t* value, uint32_t size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || (size && !value)) return -1;
  std::string text;
  if (size) text.assign(reinterpret_cast<const char*>(value), size);
  auto* stream = static_cast<lightusd::web_next::RenderStream*>(slot->object);
  switch (kind) {
    case 0: stream->setRenderSettingsPath(text); break;
    case 1: stream->setTangentMethod(text); break;
    default: return -1;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_variant_override(
    uint32_t handle, const uint8_t* key, uint32_t key_size,
    const uint8_t* selection, uint32_t selection_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || (key_size && !key) ||
      (selection_size && !selection)) return -1;
  std::string key_text, selection_text;
  if (key_size) key_text.assign(reinterpret_cast<const char*>(key), key_size);
  if (selection_size) {
    selection_text.assign(reinterpret_cast<const char*>(selection),
                          selection_size);
  }
  static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->setVariantOverride(key_text, selection_text);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_control(
    uint32_t handle, uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  auto* stream = static_cast<lightusd::web_next::RenderStream*>(slot->object);
  switch (kind) {
    case 0: stream->clearAssets(); break;
    case 1: stream->clearVariantOverrides(); break;
    case 2: stream->end(); break;
    case 3: stream->reset(); break;
    case 4: stream->releaseSourceLayer(); break;
    default: return -1;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_max_input_bytes(
    uint32_t handle, uint32_t limit) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->setMaxInputBytes(limit);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_max_input_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->maxInputBytes();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_memory_limit_mb(
    uint32_t handle, int32_t limit_mb) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->setMaxMemoryLimitMB(limit_mb);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_memory_limit_mb(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->maxMemoryLimitMB();
}

EMSCRIPTEN_KEEPALIVE double lightusd_next_render_remaining_memory_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1.0;
  return static_cast<double>(
      static_cast<const lightusd::web_next::RenderStream*>(slot->object)
          ->remainingMemoryLimitBytes());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_start(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t expected_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->startStreamingAsset(name, name_size, expected_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_append(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    const uint8_t* bytes, uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->appendStreamingAsset(name, name_size, bytes, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_progress(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->streamingAssetBytesWritten(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_size(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->streamingAssetExpectedBytes(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_uuid(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->streamingAssetUuidCopy(name, name_size, out, cap);
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_render_stream_asset_view(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return 0;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->streamingAssetViewPtr(name, name_size, byte_size);
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_render_stream_asset_view_at(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t offset, uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return 0;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->streamingAssetViewPtrAt(name, name_size, offset, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_mark_written(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->markStreamingAssetBytesWritten(name, name_size, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_mark_range_written(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint32_t offset, uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->markStreamingAssetRangeWritten(name, name_size, offset, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_finalize(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->finalizeStreamingAsset(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t
lightusd_next_render_stream_asset_finalize_to_store(
    uint32_t render_handle, uint32_t asset_store_handle,
    const uint8_t* name, uint32_t name_size) {
  Slot* render = Lookup(render_handle);
  Slot* store = Lookup(asset_store_handle);
  if (!render || render->kind != 4 || !store || store->kind != 5) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(render->object)
      ->finalizeStreamingAssetToStore(
          name, name_size,
          *static_cast<lightusd::web_next::NextAssetStore*>(store->object));
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stream_asset_cancel(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->cancelStreamingAsset(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_begin_streamed_asset(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->beginStreamingAssetAsRoot(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_value_clip_setting(
    uint32_t handle, uint8_t field, double value) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->setValueClipSetting(field, value);
}

EMSCRIPTEN_KEEPALIVE double lightusd_next_render_value_clip_setting(
    uint32_t handle, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return std::numeric_limits<double>::quiet_NaN();
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->valueClipSetting(field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_mesh_setting(
    uint32_t handle, uint8_t field, int32_t value) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  auto* stream = static_cast<lightusd::web_next::RenderStream*>(slot->object);
  switch (field) {
    case 0: return stream->setSphereSubdivisions(value);
    case 1:
      if (value != 0 && value != 1) return -1;
      return stream->setEnableBoneReduction(value != 0);
    case 2:
      if (value < 1 || value > 128) return -1;
      return stream->setTargetBoneCount(static_cast<uint32_t>(value));
    case 3:
      if (value != 0 && value != 1) return -1;
      return stream->setRoundBoneCount(value != 0);
    default: return -1;
  }
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_setting(
    uint32_t handle, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const auto* stream = static_cast<const lightusd::web_next::RenderStream*>(slot->object);
  switch (field) {
    case 0: return stream->sphereSubdivisions();
    case 1: return stream->enableBoneReduction() ? 1 : 0;
    case 2: return static_cast<int32_t>(stream->targetBoneCount());
    case 3: return stream->roundBoneCount() ? 1 : 0;
    default: return -1;
  }
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_begin(
    uint32_t handle, const uint8_t* bytes, uint32_t size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->beginBytes(bytes, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_begin_layer_document(
    uint32_t render_handle, uint32_t layer_handle) {
  const Slot* render = Lookup(render_handle);
  const Slot* layer = Lookup(layer_handle);
  if (!render || render->kind != 4 || !layer || layer->kind != 6) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(render->object)
      ->beginFromLayerDocument(
          *static_cast<lightusd::web_next::NextLayerDocument*>(layer->object));
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_preflight_input(
    uint32_t handle, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->preflightBeginInput(size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_begin_cached_asset(
    uint32_t render_handle, uint32_t asset_store_handle,
    const uint8_t* identifier, uint32_t identifier_size) {
  Slot* render = Lookup(render_handle);
  Slot* store = Lookup(asset_store_handle);
  if (!render || render->kind != 4 || !store || store->kind != 5) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(render->object)
      ->beginCachedAsset(
          *static_cast<lightusd::web_next::NextAssetStore*>(store->object),
          identifier, identifier_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_provide_asset(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    const uint8_t* bytes, uint32_t byte_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->provideAssetBytes(name, name_size, bytes, byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_remove_asset(
    uint32_t handle, const uint8_t* name, uint32_t name_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !name || name_size == 0) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->removeAssetBytes(name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_provided_asset_count(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->providedAssetCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_provided_asset_name(
    uint32_t handle, int32_t asset_id, uint8_t* out, uint32_t capacity) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->providedAssetNameCopy(asset_id, out, capacity);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_provided_asset_bytes(
    uint32_t handle, const uint8_t* name, uint32_t name_size,
    uint8_t* out, uint32_t capacity) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->providedAssetBytesCopy(name, name_size, out, capacity);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_set_provided_asset_byte_limit(
    uint32_t handle, uint32_t limit) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->setProvidedAssetByteLimit(limit);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_provided_asset_byte_limit(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->providedAssetByteLimit();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_variant_set_count(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->variantSetCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_variant_name_count(
    uint32_t handle, int32_t set_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->variantNameCount(set_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_variant_string(
    uint32_t handle, int32_t set_id, int32_t variant_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->variantStringCopy(set_id, variant_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_layer_asset_count(
    uint32_t handle, uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->layerAssetPathCount(kind);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_layer_asset_string(
    uint32_t handle, uint8_t kind, int32_t path_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->layerAssetPathCopy(kind, path_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_layer_arc_present(
    uint32_t handle, uint8_t kind) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->layerArcPresent(kind);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_field(
    uint32_t handle, int32_t node_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodeField(node_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_native_instance_node_ids(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nativeInstanceNodeIdsCopy(out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_prototype_path(
    uint32_t handle, int32_t node_id, uint8_t* out, uint32_t capacity) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodePrototypePathCopy(node_id, out, capacity);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_child(
    uint32_t handle, int32_t node_id, int32_t child_index) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodeChildId(node_id, child_index);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_child_count(
    uint32_t handle, int32_t node_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodeChildCount(node_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_root_node(
    uint32_t handle, int32_t root_index) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->rootNodeId(root_index);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_path(
    uint32_t handle, int32_t node_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodePathCopy(node_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_node_transform(
    uint32_t handle, int32_t node_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->nodeTransformCopy(node_id, kind, out, cap);
}


EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_resource_path(
    uint32_t handle, uint8_t kind, int32_t resource_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->resourcePathCopy(kind, resource_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_resource_name(
    uint32_t handle, uint8_t kind, int32_t resource_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->resourceNameCopy(kind, resource_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_record_path(
    uint32_t handle, uint8_t kind, int32_t record_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->recordPathCopy(kind, record_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_channel_count(
    uint32_t handle, int32_t animation_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationChannelCount(animation_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_channel_field(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationChannelField(animation_id, channel_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_channel_buffer(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationChannelBufferCopy(animation_id, channel_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_array_view_get(
    uint32_t handle, int32_t animation_id, int32_t channel_id,
    lightusd_next_animation_array_view* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationArrayView(animation_id, channel_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_channel_string(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationChannelStringCopy(animation_id, channel_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_channel_order_string(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    int32_t order_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationChannelOrderStringCopy(animation_id, channel_id, kind, order_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_points_buffer(
    uint32_t handle, int32_t points_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->pointsBufferCopy(points_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_points_info_get(
    uint32_t handle, int32_t points_id, lightusd_next_points_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->pointsInfo(points_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_curves_buffer(
    uint32_t handle, int32_t curves_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->curvesBufferCopy(curves_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_curves_info_get(
    uint32_t handle, int32_t curves_id, lightusd_next_curves_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->curvesInfo(curves_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_curves_field(
    uint32_t handle, int32_t curves_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->curvesField(curves_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_instancer_buffer(
    uint32_t handle, int32_t instancer_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->instancerBufferCopy(instancer_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_instancer_string(
    uint32_t handle, int32_t instancer_id, int32_t prototype_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->instancerStringCopy(instancer_id, prototype_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_instancer_info_get(
    uint32_t handle, int32_t instancer_id, lightusd_next_instancer_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->instancerInfo(instancer_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_point_instance_draw_field(
    uint32_t handle, int32_t draw_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->pointInstanceDrawField(draw_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_point_instance_draw_transform(
    uint32_t handle, int32_t draw_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->pointInstanceDrawTransformCopy(draw_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_field(
    uint32_t handle, int32_t light_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightField(light_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_transform(
    uint32_t handle, int32_t light_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightTransformCopy(light_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_color(
    uint32_t handle, int32_t light_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightColorCopy(light_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_info_get(
    uint32_t handle, int32_t light_id, lightusd_next_light_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightInfo(light_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_string(
    uint32_t handle, int32_t light_id, uint8_t kind, int32_t item_id,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightStringCopy(light_id, kind, item_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_mesh_ids(
    uint32_t handle, int32_t light_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightMeshIdsCopy(light_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_camera_field(
    uint32_t handle, int32_t camera_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->cameraField(camera_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_camera_info_get(
    uint32_t handle, int32_t camera_id, lightusd_next_camera_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->cameraInfo(camera_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_camera_transform(
    uint32_t handle, int32_t camera_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->cameraTransformCopy(camera_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_camera_optics(
    uint32_t handle, int32_t camera_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->cameraOpticsCopy(camera_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_skeleton_field(
    uint32_t handle, int32_t skeleton_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->skeletonField(skeleton_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_skeleton_joint_buffer(
    uint32_t handle, int32_t skeleton_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->skeletonJointBufferCopy(skeleton_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_skeleton_joint_string(
    uint32_t handle, int32_t skeleton_id, int32_t joint_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->skeletonJointStringCopy(skeleton_id, joint_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_skeleton_joint_children(
    uint32_t handle, int32_t skeleton_id, int32_t joint_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->skeletonJointChildrenCopy(skeleton_id, joint_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_field(
    uint32_t handle, int32_t mesh_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->meshField(mesh_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_primvar_count(
    uint32_t handle, int32_t mesh_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshPrimvarCount(mesh_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_primvar_field(
    uint32_t handle, int32_t mesh_id, int32_t primvar_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshPrimvarField(mesh_id, primvar_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_primvar_name(
    uint32_t handle, int32_t mesh_id, int32_t primvar_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshPrimvarNameCopy(mesh_id, primvar_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_primvar_buffer(
    uint32_t handle, int32_t mesh_id, int32_t primvar_id, uint8_t kind,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshPrimvarBufferCopy(mesh_id, primvar_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_buffer(
    uint32_t handle, int32_t mesh_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->meshBufferCopy(mesh_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_request_mesh_tangents(
    uint32_t handle, int32_t mesh_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->requestMeshTangents(mesh_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_view_get(
    uint32_t handle, int32_t mesh_id, lightusd_next_mesh_view* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->meshView(mesh_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_view_string(
    uint32_t handle, int32_t mesh_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshViewStringCopy(mesh_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_subset_output(
    uint32_t handle, int32_t mesh_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->meshSubsetOutputCopy(mesh_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_blend_shape_count(
    uint32_t handle, int32_t mesh_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshBlendShapeCount(mesh_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_blend_shape_info_get(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    lightusd_next_blend_shape_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshBlendShapeInfo(mesh_id, shape_id, inbetween_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_blend_shape_name(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->meshBlendShapeNameCopy(mesh_id, shape_id, inbetween_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_mesh_blend_shape_offsets(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    uint8_t kind, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->meshBlendShapeOffsetsCopy(mesh_id, shape_id, inbetween_id,
                                  kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_output_material_info_get(
    uint32_t handle, int32_t material_id,
    lightusd_next_output_material_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->outputMaterialInfo(material_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_output_material_string(
    uint32_t handle, int32_t material_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->outputMaterialStringCopy(material_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_format_status(
    uint32_t handle, int32_t material_id, uint8_t format) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialFormatStatus(material_id, format);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_format_string(
    uint32_t handle, int32_t material_id, uint8_t format, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialFormatStringCopy(material_id, format, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_export_stage_usdc(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(slot->object)
      ->exportStageUSDC();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stage_usdc_size(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->stageUSDCSize();
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_render_stage_usdc_data(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return 0;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->stageUSDCData();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_format_status(
    uint32_t handle, int32_t light_id, uint8_t format) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightFormatStatus(light_id, format);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_light_format_string(
    uint32_t handle, int32_t light_id, uint8_t format, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->lightFormatStringCopy(light_id, format, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_output_texture_meta_get(
    uint32_t handle, int32_t material_id, uint8_t texture_slot,
    lightusd_next_output_texture_meta* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->outputTextureMeta(material_id, texture_slot, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_output_texture_string(
    uint32_t handle, int32_t material_id, uint8_t texture_slot,
    uint8_t kind, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->outputTextureStringCopy(material_id, texture_slot, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_field(
    uint32_t handle, int32_t material_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialField(material_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_diagnostic_count(
    uint32_t handle, int32_t material_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialDiagnosticCount(material_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_diagnostic_kind(
    uint32_t handle, int32_t material_id, int32_t diagnostic_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialDiagnosticKind(material_id, diagnostic_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_diagnostic_string(
    uint32_t handle, int32_t material_id, int32_t diagnostic_id,
    uint8_t kind, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialDiagnosticStringCopy(material_id, diagnostic_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_param_buffer(
    uint32_t handle, int32_t material_id, uint8_t param, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialParamBufferCopy(material_id, param, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_material_param_texture(
    uint32_t handle, int32_t material_id, uint8_t param) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->materialParamTextureId(material_id, param);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_field(
    uint32_t handle, int32_t texture_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureField(texture_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_string(
    uint32_t handle, int32_t texture_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureStringCopy(texture_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureImageBufferCopy(texture_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_image_field(
    uint32_t handle, int32_t image_id, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->imageField(image_id, field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_image_asset_identifier(
    uint32_t handle, int32_t image_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->imageAssetIdentifierCopy(image_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_image_buffer(
    uint32_t handle, int32_t image_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->imageBufferCopy(image_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_render_image_data(
    uint32_t handle, int32_t image_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return 0;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->imageBufferData(image_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_sampling_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureSamplingBufferCopy(texture_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_transform_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureTransformBufferCopy(texture_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_udim_remap_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureUDIMRemapBufferCopy(texture_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_udim_tile_count(
    uint32_t handle, int32_t udim_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->udimTileCount(udim_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_udim_count(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->udimTextureCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_udim_tiles(
    uint32_t handle, int32_t udim_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->udimTileBufferCopy(udim_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_udim_string(
    uint32_t handle, int32_t udim_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->udimStringCopy(udim_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_texture_color_transform_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->textureColorTransformBufferCopy(texture_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_scene_field(
    uint32_t handle, uint8_t field) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->sceneField(field);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_scene_string(
    uint32_t handle, uint8_t kind, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->sceneStringCopy(kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_scene_metadata_get(
    uint32_t handle, lightusd_next_scene_metadata* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->sceneMetadata(out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_unsupported_string(
    uint32_t handle, int32_t unsupported_id, uint8_t kind, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->unsupportedStringCopy(unsupported_id, kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_info_get(
    uint32_t handle, lightusd_next_render_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out ||
      out->struct_size < sizeof(lightusd_next_render_info)) {
    return -1;
  }
  const auto* stream = static_cast<const lightusd::web_next::RenderStream*>(
      slot->object);
  const uint32_t size = out->struct_size;
  std::memset(out, 0, sizeof(*out));
  out->struct_size = size;
  out->mesh_count = stream->meshCount();
  out->node_count = stream->nodeCount();
  out->light_count = stream->lightCount();
  out->points_count = stream->pointsCount();
  out->curves_count = stream->curvesCount();
  out->camera_count = stream->cameraCount();
  out->point_instancer_count = stream->pointInstancerCount();
  out->point_instance_draw_count = stream->pointInstanceDrawCount();
  out->skeleton_count = stream->skeletonCount();
  out->unsupported_renderable_count = stream->unsupportedRenderableCount();
  out->animation_count = stream->animationCount();
  const std::string& error = stream->error();
  out->error_size = error.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())
                        ? (std::numeric_limits<int32_t>::max)()
                        : static_cast<int32_t>(error.size());
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stats_get(
    uint32_t handle, lightusd_next_render_stats* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->renderStats(out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_stats_detail_get(
    uint32_t handle, lightusd_next_render_stats_detail* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4 || !out) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->renderStatsDetail(out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_info_get(
    uint32_t handle, int32_t animation_id, lightusd_next_animation_info* out) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationInfo(animation_id, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_animation_clip_asset(
    uint32_t handle, int32_t animation_id, int32_t asset_id, uint8_t* out,
    uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  return static_cast<const lightusd::web_next::RenderStream*>(slot->object)
      ->animationClipAssetCopy(animation_id, asset_id, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_error(uint32_t handle,
                                                       char* out,
                                                       uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const std::string& error =
      static_cast<const lightusd::web_next::RenderStream*>(slot->object)->error();
  if (out && cap != 0) {
    const size_t copy = std::min<size_t>(error.size(), cap - 1);
    std::memcpy(out, error.data(), copy);
    out[copy] = '\0';
  }
  if (error.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    return (std::numeric_limits<int32_t>::max)();
  }
  return static_cast<int32_t>(error.size());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_warning(uint32_t handle,
                                                         char* out,
                                                         uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const std::string& warning =
      static_cast<const lightusd::web_next::RenderStream*>(slot->object)->warning();
  if (out && cap != 0) {
    const size_t copy = std::min<size_t>(warning.size(), cap - 1);
    std::memcpy(out, warning.data(), copy);
    out[copy] = '\0';
  }
  if (warning.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return (std::numeric_limits<int32_t>::max)();
  return static_cast<int32_t>(warning.size());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_composition_record(
    uint32_t handle, uint8_t field, uint32_t index, char* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 4) return -1;
  const auto& report = static_cast<const lightusd::web_next::RenderStream*>(
      slot->object)->compositionReport();
  if (field == 0) return static_cast<int32_t>(report.layer_dependencies.size());
  if (field == 2) return static_cast<int32_t>(report.issues.size());
  if (field == 3) {
    if (index >= report.issues.size()) return -1;
    return static_cast<int32_t>(report.issues[index].code);
  }
  const std::string* value = nullptr;
  if (field == 1) {
    if (index >= report.layer_dependencies.size()) return -1;
    value = &report.layer_dependencies[index];
  } else if (field == 4 || field == 5) {
    if (index >= report.issues.size()) return -1;
    value = field == 4 ? &report.issues[index].site
                       : &report.issues[index].message;
  } else {
    return -1;
  }
  if (out && cap != 0) {
    const size_t copy = std::min<size_t>(value->size(), cap - 1);
    std::memcpy(out, value->data(), copy);
    out[copy] = '\0';
  }
  if (value->size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return (std::numeric_limits<int32_t>::max)();
  return static_cast<int32_t>(value->size());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_register(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size,
    const uint8_t* bytes, uint32_t byte_size, uint8_t* out_identifier,
    uint32_t out_capacity) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  std::string registered;
  const int status = static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->registerMemoryAsset(identifier, identifier_size, bytes, byte_size,
                            &registered);
  if (status != 0) return status;
  if (registered.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return -1;
  if (out_identifier && out_capacity >= registered.size() && !registered.empty())
    std::memcpy(out_identifier, registered.data(), registered.size());
  return static_cast<int32_t>(registered.size());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_raw(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size,
    uintptr_t bytes, uint32_t byte_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5 || (!identifier && identifier_size) ||
      bytes == 0 || byte_size == 0 || byte_size > (uint32_t{1} << 30)) return -1;
  const size_t heap_size = emscripten_get_heap_size();
  if (bytes > heap_size || byte_size > heap_size - bytes) return -1;
  const uintptr_t identifier_address = reinterpret_cast<uintptr_t>(identifier);
  if (identifier_size && (identifier_address > heap_size ||
      identifier_size > heap_size - identifier_address)) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setAssetFromRawPointer(identifier, identifier_size,
                               reinterpret_cast<const uint8_t*>(bytes),
                               byte_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_import_asset_store(
    uint32_t render_handle, uint32_t asset_store_handle) {
  Slot* render = Lookup(render_handle);
  Slot* store = Lookup(asset_store_handle);
  if (!render || render->kind != 4 || !store || store->kind != 5) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(render->object)
      ->importAssetStore(
          *static_cast<lightusd::web_next::NextAssetStore*>(store->object));
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_render_clear_imported_asset_store(
    uint32_t handle) {
  Slot* render = Lookup(handle);
  if (!render || render->kind != 4) return -1;
  return static_cast<lightusd::web_next::RenderStream*>(render->object)
      ->clearImportedAssetStore();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_unregister(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->unregisterMemoryAsset(identifier, identifier_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_read(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size,
    uint8_t* out, uint32_t out_capacity) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  std::vector<uint8_t> bytes;
  const int status = static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->readMemoryAsset(identifier, identifier_size, &bytes);
  if (status <= 0) return status == 0 ? -2 : -1;
  if (bytes.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return -1;
  if (out && out_capacity >= bytes.size() && !bytes.empty())
    std::memcpy(out, bytes.data(), bytes.size());
  return static_cast<int32_t>(bytes.size());
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_view(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size,
    uintptr_t* out_data, uint32_t* out_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5 || !out_data || !out_size) return -1;
  const uint8_t* data = nullptr;
  uint32_t byte_size = 0;
  const int status = static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->borrowedAssetView(identifier, identifier_size, &data, &byte_size);
  if (status != 1) return status;
  *out_data = reinterpret_cast<uintptr_t>(data);
  *out_size = byte_size;
  return 1;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_alias(
    uint32_t handle, const uint8_t* authored, uint32_t authored_size,
    const uint8_t* resolved, uint32_t resolved_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setAlias(authored, authored_size, resolved, resolved_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_identifier_count(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->identifierCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_identifier_copy(
    uint32_t handle, int32_t index, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->identifierCopy(index, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_base_path(
    uint32_t handle, const uint8_t* path, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setBaseWorkingPath(path, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_base_path(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->baseWorkingPathCopy(out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_add_search_path(
    uint32_t handle, const uint8_t* path, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->addSearchPath(path, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_clear_search_paths(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->clearSearchPaths();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_search_path_count(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->searchPathCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_search_path_copy(
    uint32_t handle, int32_t index, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->searchPathCopy(index, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_allow_parent_paths(
    uint32_t handle, int32_t allow) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5 || (allow != 0 && allow != 1)) return -1;
  static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setAllowParentRelativeAssetPaths(allow != 0);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_get_allow_parent_paths(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->allowParentRelativeAssetPaths() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_streaming_uuid(
    uint32_t handle, const uint8_t* identifier, uint32_t size,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->streamingAssetUuid(identifier, size, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_uuid(
    uint32_t handle, const uint8_t* identifier, uint32_t size,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->assetUuid(identifier, size, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_find_uuid(
    uint32_t handle, const uint8_t* uuid, uint32_t size,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->findAssetByUuid(uuid, size, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_hash(
    uint32_t handle, const uint8_t* identifier, uint32_t size,
    uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->assetHash(identifier, size, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_verify_hash(
    uint32_t handle, const uint8_t* identifier, uint32_t identifier_size,
    const uint8_t* hash, uint32_t hash_size) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->verifyAssetHash(identifier, identifier_size, hash, hash_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_delete_uuid(
    uint32_t handle, const uint8_t* uuid, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->deleteAssetByUuid(uuid, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_memory_limit(
    uint32_t handle, uint32_t limit_bytes) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setMemoryLimit(limit_bytes);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_memory_limit(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->memoryLimit();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_memory_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->memoryBytes();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_metadata_limit(
    uint32_t handle, uint64_t limit_bytes) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setMetadataLimit(limit_bytes);
}

EMSCRIPTEN_KEEPALIVE uint64_t lightusd_next_asset_store_metadata_limit(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return 0;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->metadataLimit();
}

EMSCRIPTEN_KEEPALIVE uint64_t lightusd_next_asset_store_metadata_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return 0;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->metadataBytes();
}

EMSCRIPTEN_KEEPALIVE uint64_t lightusd_next_asset_store_cache_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return 0;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->cacheSizeBytes();
}

EMSCRIPTEN_KEEPALIVE uint64_t lightusd_next_asset_store_cache_max_bytes(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return 0;
  return static_cast<const lightusd::web_next::NextAssetStore*>(slot->object)
      ->cacheMaxSizeBytes();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_set_cache_max_bytes(
    uint32_t handle, uint64_t limit_bytes) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  static_cast<lightusd::web_next::NextAssetStore*>(slot->object)
      ->setCacheMaxSizeBytes(limit_bytes);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_asset_store_clear(uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 5) return -1;
  return static_cast<lightusd::web_next::NextAssetStore*>(slot->object)->clear();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_load(
    uint32_t handle, const uint8_t* bytes, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->load(bytes, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_load_json(
    uint32_t handle, const uint8_t* bytes, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->loadJSON(bytes, size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_load_with_progress(
    uint32_t handle, const uint8_t* bytes, uint32_t size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)->load(
      bytes, size, [](const char* phase, size_t current, size_t total) {
        return lightusd::web_next::reportNextLayerLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      });
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_mh_profile_json_size(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->mhProfileJSONSize();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_mh_profile_json_copy(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->mhProfileJSONCopy(out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_shading_graph_json_size(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->shadingGraphJSONSize();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_shading_graph_json_copy(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->shadingGraphJSONCopy(out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_export_usdz(
    uint32_t handle, uint32_t asset_store_handle, uint8_t root_format) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6 || root_format > 1) return -1;
  lightusd::web_next::NextAssetStore* assets = nullptr;
  if (asset_store_handle) {
    Slot* asset_slot = Lookup(asset_store_handle);
    if (!asset_slot || asset_slot->kind != 5) return -1;
    assets = static_cast<lightusd::web_next::NextAssetStore*>(asset_slot->object);
  }
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportUsdz(assets, root_format);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_export_usdz_copy(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  const auto* document =
      static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object);
  const int32_t size = document->exportUsdzSize();
  if (size < 0 || (!out && cap)) return -1;
  if (!out || cap < static_cast<uint32_t>(size)) return size;
  const uintptr_t data = document->exportUsdzData();
  if (size && !data) return -1;
  std::memcpy(out, reinterpret_cast<const void*>(data), static_cast<size_t>(size));
  return size;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_define_prim(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* type, uint32_t type_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->definePrim(path, path_size, type, type_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_remove_prim(
    uint32_t handle, const uint8_t* path, uint32_t path_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->removePrim(path, path_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_remove_attribute(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->removeAttribute(path, path_size, name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_string(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, const uint8_t* value,
    uint32_t value_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setStringAttribute(path, path_size, name, name_size, value, value_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_number(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, double value) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setNumberAttribute(path, path_size, name, name_size, value);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_attribute_metadata(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, const uint8_t* key,
    uint32_t key_size, uint8_t kind, const uint8_t* payload,
    uint32_t payload_size, double number, int32_t integer) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setAttributeMetadata(path, path_size, name, name_size, key, key_size,
                             kind, payload, payload_size, number, integer);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_get_attribute_metadata(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, const uint8_t* key,
    uint32_t key_size, uint8_t* kind, uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->getAttributeMetadata(path, path_size, name, name_size, key, key_size,
                            kind, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_stage_metadata(
    uint32_t handle, const uint8_t* key, uint32_t key_size, uint8_t kind,
    const uint8_t* text, uint32_t text_size, double number) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setStageMetadata(key, key_size, kind, text, text_size, number);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_prim_metadata(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* key, uint32_t key_size, uint8_t kind,
    const uint8_t* payload, uint32_t payload_size, uint32_t count) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setPrimMetadata(path, path_size, key, key_size, kind, payload,
                        payload_size, count);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_get_prim_metadata(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* key, uint32_t key_size, uint8_t* kind, uint32_t* count,
    uint8_t* out, uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->getPrimMetadata(path, path_size, key, key_size, kind, count, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_prim_metadata_is_authored(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* key, uint32_t key_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->primMetadataIsAuthored(path, path_size, key, key_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_get_stage_metadata_number(
    uint32_t handle, const uint8_t* key, uint32_t key_size, double* out) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->getStageMetadataNumber(key, key_size, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_get_stage_metadata_string(
    uint32_t handle, const uint8_t* key, uint32_t key_size, uint8_t* out,
    uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->getStageMetadataString(key, key_size, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_stage_metadata_is_authored(
    uint32_t handle, const uint8_t* key, uint32_t key_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->stageMetadataIsAuthored(key, key_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_relationship_targets(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, const uint8_t* packed_targets,
    uint32_t packed_size, uint32_t count) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setRelationshipTargets(path, path_size, name, name_size,
                               packed_targets, packed_size, count);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_relationship_target_count(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->relationshipTargetCount(path, path_size, name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_relationship_target_copy(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, uint32_t index, uint8_t* out,
    uint32_t cap) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->relationshipTargetCopy(path, path_size, name, name_size, index, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_remove_relationship(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->removeRelationship(path, path_size, name, name_size);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_set_typed(
    uint32_t handle, const uint8_t* path, uint32_t path_size,
    const uint8_t* name, uint32_t name_size, const uint8_t* type,
    uint32_t type_size, uint8_t is_array, const uint8_t* data,
    uint32_t data_size, uint32_t count) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->setAttribute(path, path_size, name, name_size, type, type_size,
                     is_array, data, data_size, count);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_prim_count(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->primCount();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_loaded(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->loaded();
}

EMSCRIPTEN_KEEPALIVE void lightusd_next_layer_end(uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return;
  static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)->end();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_error_size(uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->errorSize();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_error_copy(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->errorCopy(out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_export_usda_size(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportUsdaSize();
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_layer_export_usda_data(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return 0;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportUsdaData();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_export_json_size(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportJSONSize();
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_layer_export_json_data(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return 0;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportJSONData();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_next_layer_export_usdc_size(
    uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return -1;
  return static_cast<lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportUsdcSize();
}

EMSCRIPTEN_KEEPALIVE uintptr_t lightusd_next_layer_export_usdc_data(
    uint32_t handle) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 6) return 0;
  return static_cast<const lightusd::web_next::NextLayerDocument*>(slot->object)
      ->exportUsdcData();
}
}
