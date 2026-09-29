// SPDX-License-Identifier: Apache-2.0
#include "binding-combined-api.h"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "next/pipeline/flatten.hh"

static_assert(sizeof(lightusd_combined_material_info) == 240, "material layout changed");
static_assert(sizeof(lightusd_combined_mesh_value_info) == 80, "mesh value layout changed");
static_assert(sizeof(lightusd_combined_mesh_pointer_info) == 64, "mesh pointer layout changed");
static_assert(sizeof(lightusd_combined_mesh_attribute) == 40, "mesh attribute layout changed");
static_assert(sizeof(lightusd_combined_mesh_submesh) == 16, "mesh submesh layout changed");

static_assert(sizeof(lightusd_combined_bone_texture_info) == 72,
              "combined bone texture POD layout changed");

static_assert(sizeof(lightusd_combined_animation_info) == 64,
              "combined animation POD layout changed");
static_assert(sizeof(lightusd_combined_sampler_info) == 40,
              "combined sampler POD layout changed");
static_assert(sizeof(lightusd_combined_channel_info) == 32,
              "combined channel POD layout changed");

static_assert(sizeof(lightusd_combined_skeleton_info) == 16,
              "combined skeleton POD layout changed");
static_assert(sizeof(lightusd_combined_joint_info) == 272,
              "combined joint POD layout changed");

static_assert(sizeof(lightusd_combined_light_info) == 424,
              "combined light POD layout changed");

static_assert(sizeof(lightusd_combined_image_info) == 128,
              "combined image POD layout changed");

static_assert(sizeof(lightusd_combined_node_info) == 288,
              "combined node POD layout changed");

static_assert(sizeof(lightusd_combined_instance_info) == 280,
              "combined instance POD layout changed");

static_assert(sizeof(lightusd_combined_flatten_info) == 88,
              "combined flatten POD layout changed");
static_assert(sizeof(lightusd_combined_flatten_step_info) == 96,
              "combined flatten step POD layout changed");
static_assert(sizeof(lightusd_combined_memory_stats) == 104,
              "combined memory stats POD layout changed");

static_assert(sizeof(lightusd_combined_stream_info) == 40,
              "combined stream POD layout changed");

static_assert(sizeof(lightusd_combined_asset_info) == 24,
              "combined asset POD layout changed");

static_assert(sizeof(lightusd_combined_loading_progress) == 72,
              "combined loading progress POD layout changed");
static_assert(sizeof(lightusd_combined_async_load_info) == 16,
              "combined async load POD layout changed");

// JS callback bridges; the registry lives in combined-api.js. A callback that
// throws reports failure here and the adapter rethrows after the C call.
EM_JS(int, CombinedFlattenEmitChunk,
      (uint32_t sink_id, const uint8_t* data, uint32_t size), {
  return Module['__lightusdCombinedFlattenEmit'](sink_id, Number(data), size);
});
EM_JS(int, CombinedFlattenLayerExists,
      (uint32_t exists_id, const uint8_t* key, uint32_t size), {
  return Module['__lightusdCombinedFlattenExists'](exists_id, Number(key), size);
});
EM_JS(int32_t, CombinedFlattenFetchSize,
      (uint32_t fetch_id, const uint8_t* key, uint32_t size), {
  return Module['__lightusdCombinedFlattenFetch'](fetch_id, Number(key), size);
});
EM_JS(int, CombinedFlattenFetchCopy,
      (uint32_t fetch_id, uint8_t* out, uint32_t size), {
  return Module['__lightusdCombinedFlattenFetchCopy'](fetch_id, Number(out), size);
});

namespace {

std::vector<uint8_t> g_flatten_output;
std::string g_flatten_error;
std::string g_flatten_key;
std::vector<std::string> g_flatten_asset_paths;
std::vector<std::string> g_flatten_composition_errors;
std::vector<std::string> g_table_strings;
std::vector<uint32_t> g_table_shape;

void ClearFlattenStrings() {
  g_flatten_key.clear();
  std::vector<std::string>().swap(g_flatten_asset_paths);
  std::vector<std::string>().swap(g_flatten_composition_errors);
}

int32_t CopyBytes(const uint8_t* source, uint32_t size, uint8_t* out,
                  uint32_t cap) {
  if (out && cap < size) return -1;
  if (!out && cap) return -1;
  if (out && size) std::memcpy(out, source, size);
  return static_cast<int32_t>(size);
}

int32_t RunFlatten(std::string&& input, uint8_t lazy_arrays,
                   const std::map<std::string, std::string>& remap,
                   const std::map<std::string, std::string>& variants,
                   lightusd_combined_flatten_info* out) {
  if (!out || out->struct_size < sizeof(*out)) return -1;
  *out = {};
  out->struct_size = sizeof(*out);
  g_flatten_output.clear();
  g_flatten_error.clear();
  ClearFlattenStrings();
  lightusd::next::pipeline::FlattenOptions options;
  options.read.lazy_arrays = lazy_arrays != 0;
  options.asset_path_remap = remap;
  options.composition.variant_overrides = variants;
  lightusd::next::pipeline::FlattenStats stats;
  if (!lightusd::next::pipeline::FlattenUSDCToUSDCOwned(
          std::move(input), g_flatten_output, options, &stats,
          &g_flatten_error)) {
    g_flatten_output.clear();
    return 0;
  }
  if (g_flatten_output.size() > INT32_MAX) {
    g_flatten_output.clear();
    g_flatten_error = "Output exceeds 2 GiB limit";
    return 0;
  }
  out->data_size = static_cast<uint32_t>(g_flatten_output.size());
  out->input_bytes = static_cast<double>(stats.input_bytes);
  out->output_bytes = static_cast<double>(stats.output_bytes);
  out->prim_count = static_cast<double>(stats.prim_count);
  out->arrays_passed_through = static_cast<double>(stats.arrays_passed_through);
  out->arrays_reencoded = static_cast<double>(stats.arrays_reencoded);
  out->asset_paths_remapped = static_cast<double>(stats.asset_paths_remapped);
  out->read_ms = stats.read_ms;
  out->compose_ms = stats.compose_ms;
  out->write_ms = stats.write_ms;
  return 1;
}

}  // namespace

int32_t lightusd::web::combined::NextFlattenOwned(
    std::string&& input, uint8_t lazy_arrays,
    lightusd_combined_flatten_info* out) {
  static const std::map<std::string, std::string> empty;
  return RunFlatten(std::move(input), lazy_arrays, empty, empty, out);
}

int32_t lightusd::web::combined::NextFlattenOwnedWithMaps(
    std::string&& input, uint8_t lazy_arrays,
    const std::map<std::string, std::string>& remap,
    const std::map<std::string, std::string>& variants,
    lightusd_combined_flatten_info* out) {
  return RunFlatten(std::move(input), lazy_arrays, remap, variants, out);
}

void lightusd::web::combined::StoreFlattenResult(
    int32_t status, std::string&& message, std::vector<uint8_t>&& data,
    lightusd::next::pipeline::FlattenStats* stats,
    lightusd_combined_flatten_step_info* out) {
  const uint32_t struct_size = out->struct_size;
  *out = {};
  out->struct_size = struct_size;
  ClearFlattenStrings();
  std::vector<uint8_t>().swap(g_flatten_output);
  g_flatten_error.clear();
  if (status == 3 && data.size() > INT32_MAX) {
    status = 0;
    message = "Output exceeds 2 GiB limit";
  }
  out->status = status;
  if (status == 2) {
    g_flatten_key = std::move(message);
    return;
  }
  if (status != 3) {
    if (status <= 0) g_flatten_error = std::move(message);
    return;
  }
  g_flatten_output = std::move(data);
  g_flatten_asset_paths = std::move(stats->referenced_assets);
  g_flatten_composition_errors = std::move(stats->composition_errors);
  if (g_flatten_asset_paths.size() > UINT32_MAX ||
      g_flatten_composition_errors.size() > UINT32_MAX) {
    ClearFlattenStrings();
    std::vector<uint8_t>().swap(g_flatten_output);
    out->status = 0;
    g_flatten_error = "Too many flatten result strings";
    return;
  }
  out->data_size = static_cast<uint32_t>(g_flatten_output.size());
  out->asset_path_count = static_cast<uint32_t>(g_flatten_asset_paths.size());
  out->composition_error_count =
      static_cast<uint32_t>(g_flatten_composition_errors.size());
  out->input_bytes = static_cast<double>(stats->input_bytes);
  out->output_bytes = static_cast<double>(stats->output_bytes);
  out->prim_count = static_cast<double>(stats->prim_count);
  out->arrays_passed_through =
      static_cast<double>(stats->arrays_passed_through);
  out->arrays_reencoded = static_cast<double>(stats->arrays_reencoded);
  out->asset_paths_remapped = static_cast<double>(stats->asset_paths_remapped);
  out->read_ms = stats->read_ms;
  out->compose_ms = stats->compose_ms;
  out->write_ms = stats->write_ms;
}

void lightusd::web::combined::StoreStringTable(
    std::vector<std::string>&& strings, std::vector<uint32_t>&& shape) {
  g_table_strings = std::move(strings);
  g_table_shape = std::move(shape);
}

bool lightusd::web::combined::EmitFlattenChunk(uint32_t sink_id,
                                                const uint8_t* data,
                                                size_t size) {
  return size <= UINT32_MAX &&
         CombinedFlattenEmitChunk(sink_id, data,
                                  static_cast<uint32_t>(size)) != 0;
}

bool lightusd::web::combined::FlattenLayerExists(uint32_t exists_id,
                                                  const std::string& key) {
  return key.size() <= UINT32_MAX &&
         CombinedFlattenLayerExists(
             exists_id, reinterpret_cast<const uint8_t*>(key.data()),
             static_cast<uint32_t>(key.size())) != 0;
}

bool lightusd::web::combined::FetchFlattenLayer(uint32_t fetch_id,
                                                 const std::string& key,
                                                 std::string* out) {
  if (!out || key.size() > UINT32_MAX) return false;
  const int32_t size = CombinedFlattenFetchSize(
      fetch_id, reinterpret_cast<const uint8_t*>(key.data()),
      static_cast<uint32_t>(key.size()));
  if (size <= 0 || size > (int32_t(1) << 30)) return false;
  out->resize(static_cast<size_t>(size));
  if (!CombinedFlattenFetchCopy(fetch_id,
                                reinterpret_cast<uint8_t*>(&(*out)[0]),
                                static_cast<uint32_t>(size))) {
    out->clear();
    return false;
  }
  return true;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_next_flatten_usdc(
    const uint8_t* data, uint32_t size, uint8_t lazy_arrays,
    lightusd_combined_flatten_info* out) {
  if (!out || out->struct_size < sizeof(*out) || (!data && size) ||
      size > (uint32_t(1) << 30)) {
    return -1;
  }
  std::string input;
  if (size) input.assign(reinterpret_cast<const char*>(data), size);
  static const std::map<std::string, std::string> empty;
  return RunFlatten(std::move(input), lazy_arrays, empty, empty, out);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_set_flatten_error(
    const uint8_t* data, uint32_t size) {
  if (!data && size) return -1;
  g_flatten_output.clear();
  g_flatten_error.assign(reinterpret_cast<const char*>(data), size);
  return 0;
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_next_flatten_copy(
    uint8_t* out, uint32_t cap) {
  if (g_flatten_output.size() > UINT32_MAX) return -1;
  return CopyBytes(g_flatten_output.data(),
                   static_cast<uint32_t>(g_flatten_output.size()), out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_next_flatten_error(
    uint8_t* out, uint32_t cap) {
  if (g_flatten_error.size() > UINT32_MAX) return -1;
  return CopyBytes(reinterpret_cast<const uint8_t*>(g_flatten_error.data()),
                   static_cast<uint32_t>(g_flatten_error.size()), out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_next_flatten_string(
    uint8_t kind, uint32_t index, uint8_t* out, uint32_t cap) {
  const std::string* value = nullptr;
  if (kind == 0 && index == 0) {
    value = &g_flatten_key;
  } else if (kind == 1 && index < g_flatten_asset_paths.size()) {
    value = &g_flatten_asset_paths[index];
  } else if (kind == 2 && index < g_flatten_composition_errors.size()) {
    value = &g_flatten_composition_errors[index];
  }
  if (!value || value->size() > INT32_MAX) return -1;
  return CopyBytes(reinterpret_cast<const uint8_t*>(value->data()),
                   static_cast<uint32_t>(value->size()), out, cap);
}

EMSCRIPTEN_KEEPALIVE void lightusd_combined_next_flatten_release(void) {
  std::vector<uint8_t>().swap(g_flatten_output);
  g_flatten_error.clear();
  ClearFlattenStrings();
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_table_string(
    uint32_t index, uint8_t* out, uint32_t cap) {
  if (index >= g_table_strings.size()) return -1;
  const std::string& value = g_table_strings[index];
  if (value.size() > INT32_MAX) return -1;
  return CopyBytes(reinterpret_cast<const uint8_t*>(value.data()),
                   static_cast<uint32_t>(value.size()), out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t lightusd_combined_table_shape(uint32_t* out,
                                                          uint32_t count) {
  if (count != g_table_shape.size() || (!out && count)) return -1;
  if (count) std::memcpy(out, g_table_shape.data(), count * sizeof(uint32_t));
  return 0;
}

EMSCRIPTEN_KEEPALIVE void lightusd_combined_table_release(void) {
  std::vector<std::string>().swap(g_table_strings);
  std::vector<uint32_t>().swap(g_table_shape);
}

EMSCRIPTEN_KEEPALIVE uint8_t* lightusd_combined_alloc(uint32_t size) {
  return static_cast<uint8_t*>(std::malloc(size ? size : 1));
}

EMSCRIPTEN_KEEPALIVE void lightusd_combined_free(uint8_t* ptr) {
  std::free(ptr);
}

}  // extern "C"

static_assert(sizeof(lightusd_combined_export_info) == 24, "export info ABI");

static_assert(sizeof(lightusd_combined_export_optimization) == 24, "export optimization ABI");

static_assert(sizeof(lightusd_combined_camera_info) == 72, "camera_info ABI");

static_assert(sizeof(lightusd_combined_scene_metadata) == 128, "scene_metadata ABI");

static_assert(sizeof(lightusd_combined_texture_info) == 152, "texture_info ABI");
