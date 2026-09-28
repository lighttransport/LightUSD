/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_WEB_NEXT_API_H_
#define LIGHTUSD_WEB_NEXT_API_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal browser bridge. All object IDs are uint32, including on memory64.
 * Handles validate their index and generation before each typed operation. */
uint32_t lightusd_next_create(uint32_t kind);
void lightusd_next_destroy(uint32_t handle);
uint8_t* lightusd_next_alloc(uint32_t size);
void lightusd_next_free(uint8_t* ptr);
/* Bounded authored metahuman-profile JSON for LayerDocument handles (kind 6).
 * Query size first, then copy exactly that many UTF-8 bytes. */
int32_t lightusd_next_layer_mh_profile_json_size(uint32_t handle);
int32_t lightusd_next_layer_mh_profile_json_copy(uint32_t handle,
                                                  uint8_t* out,
                                                  uint32_t cap);
int32_t lightusd_next_layer_shading_graph_json_size(uint32_t handle);
int32_t lightusd_next_layer_shading_graph_json_copy(uint32_t handle,
                                                     uint8_t* out,
                                                     uint32_t cap);
int32_t lightusd_next_layer_export_usdz(uint32_t handle,
                                        uint32_t asset_store_handle,
                                        uint8_t root_format);
int32_t lightusd_next_layer_export_usdz_copy(uint32_t handle, uint8_t* out,
                                             uint32_t cap);
/* Memory-bounded next-only image encoding. PNG, BMP, TIFF/DNG, and optional EXR are supported; returns
 * encoded byte count, 2 for invalid dimensions, -2 for unsupported format,
 * -3 when encoded output exceeds the 512 MiB cap, and -1 for bad arguments.
 * Query with out=NULL/cap=0, then call again with an output buffer. */
int32_t lightusd_next_encode_image(const uint8_t* pixels, uint32_t pixel_size,
                                  int32_t width, int32_t height,
                                  int32_t channels, const uint8_t* format,
                                  uint32_t format_size, uint8_t* out,
                                  uint32_t cap);
/* EXR query retains one encoded result until copied or released. */
uintptr_t lightusd_next_encoded_image_data(void);
void lightusd_next_encoded_image_release(void);
/* SubdivStreamer inputs are typed, counted arrays. A nonzero callback_id
 * identifies a synchronous JS batch callback. Returns 0 on success, 1 on a
 * refinement error (query subdiv_error), and -1 for invalid arguments. */
typedef struct lightusd_next_subdiv_options {
  uint32_t struct_size;
  int32_t uv_interpolation;
  int32_t scheme;
  int32_t boundary;
  int32_t level;
  int32_t batch_faces;
  int32_t block_faces;
  int32_t halo_rings;
  uint32_t want_normals;
} lightusd_next_subdiv_options;
int32_t lightusd_next_subdiv_refine(
    uint32_t handle, const float* points, uint32_t point_values,
    const uint32_t* face_counts, uint32_t face_count,
    const uint32_t* face_indices, uint32_t index_count,
    const float* uv_values, uint32_t uv_value_count,
    const uint32_t* uv_indices, uint32_t uv_index_count,
    const lightusd_next_subdiv_options* options, uint32_t callback_id);
int32_t lightusd_next_subdiv_error(uint32_t handle, uint8_t* out,
                                  uint32_t cap);
/* Next-only root rewrite. The result bytes remain owned by the converter
 * until its next rewrite or destruction; buffer queries copy them. Format is
 * 0=USDC, 1=USDA. A zero max_memory keeps finite library defaults. */
typedef struct lightusd_next_rewrite_options {
  uint32_t struct_size;
  uint32_t format;
  double max_memory;
  uint32_t usda_lazy;
  uint32_t reserved;
} lightusd_next_rewrite_options;
typedef struct lightusd_next_rewrite_info {
  uint32_t struct_size;
  uint32_t format;
  uint32_t data_size;
  uint32_t reserved;
  double token_count;
  double path_count;
  double spec_count;
} lightusd_next_rewrite_info;
int32_t lightusd_next_converter_rewrite(
    uint32_t handle, const uint8_t* data, uint32_t size,
    const lightusd_next_rewrite_options* options,
    lightusd_next_rewrite_info* out);
int32_t lightusd_next_converter_rewrite_buffer(uint32_t handle,
                                               uint8_t* out, uint32_t cap);
/* kind 0=last error, 1=last warning. */
int32_t lightusd_next_converter_string(uint32_t handle, uint8_t kind,
                                       uint8_t* out, uint32_t cap);
/* Converter byte inputs: 0=load stage (name is diagnostic filename),
 * 1=set packaged asset, 2=create URDF physics scene (name ignored).
 * Returns 1 on success, 0 on converter error, -1 on invalid input. */
int32_t lightusd_next_converter_set_bytes(uint32_t handle, uint8_t kind,
                                         const uint8_t* name,
                                         uint32_t name_size,
                                         const uint8_t* data, uint32_t size);
int32_t lightusd_next_converter_asset_preflight(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t byte_size);
/* Returns 1 when shape and aggregate mesh budget permit staging, 0 for
 * rejected mesh data, and -1 for an invalid handle or count. */
int32_t lightusd_next_converter_mesh_preflight(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t position_count,
    uint32_t normal_count, uint32_t uv_count, uint32_t index_count);
/* kind 0=visual, 1=collision. Both share the same named mesh store. */
int32_t lightusd_next_converter_set_mesh(
    uint32_t handle, uint8_t kind, const uint8_t* name, uint32_t name_size,
    const float* positions, uint32_t position_count,
    const float* normals, uint32_t normal_count,
    const float* uvs, uint32_t uv_count,
    const uint32_t* indices, uint32_t index_count);
/* 0=clear mesh buffers, 1=set USDC/USDZ export limits (MiB),
 * 2=set aggregate packaged-asset byte limit, 3=query asset bytes (first=0)
 * or max bytes (first=1), 4=set aggregate retained-mesh byte limit,
 * 5=query mesh bytes (first=0) or max bytes (first=1),
 * 6=set aggregate root/asset/mesh retained-payload limit,
 * 7=query aggregate retained-payload bytes (first=0) or limit (first=1).
 * The first limit
 * enforces the output file size. Positive working-memory limits are not yet
 * supported and return -2; non-positive values leave that limit unbounded. */
int32_t lightusd_next_converter_control(uint32_t handle, uint8_t kind,
                                        int32_t first, int32_t second);
int32_t lightusd_next_converter_root_preflight(uint32_t handle,
                                               uint32_t size);
/* Output kinds: 0=physics JSON, 1=USDA, 2=USDC, 3=USDZ.
 * Returns 1 on success, 0 on converter error, -1 on invalid input. */
int32_t lightusd_next_converter_export(uint32_t handle, uint8_t kind);
int32_t lightusd_next_converter_export_with_options(uint32_t handle,
                                                    uint8_t kind,
                                                    uint8_t root_format);
/* USDZ only. Parses an owned JSON object mapping authored asset paths to
 * packaged paths, applies it to a cloned stage, and writes the selected root
 * format. The input is borrowed for this call; output remains converter-owned. */
int32_t lightusd_next_converter_export_with_remap(uint32_t handle,
                                                  const uint8_t* remap_json,
                                                  uint32_t remap_json_size,
                                                  uint8_t root_format);
/* Borrowed pointer into converter-owned export bytes. Valid until the next
 * converter export or destruction; copy before retaining across either. */
uintptr_t lightusd_next_converter_export_data(uint32_t handle);
int32_t lightusd_next_converter_export_buffer(uint32_t handle,
                                              uint8_t* out, uint32_t cap);
/* Validate one byte span and return owned NUL-terminated JSON.
 * Release the result with lightusd_next_free. Null means allocation failure. */
uint8_t* lightusd_next_validate_json(const uint8_t* data, uint32_t size,
                                    const uint8_t* filename,
                                    uint32_t filename_size,
                                    const uint8_t* options_json,
                                    uint32_t options_size);
/* Pre-composition layer diff. format: 0=text, 1=json, 2=both, 3=neither.
 * The owned NUL-terminated result is a JSON object; free it with
 * lightusd_next_free. Negative ulps/eps select the default tolerance. */
typedef struct lightusd_next_diff_options {
  uint32_t struct_size;
  int32_t ulps;
  double abs_eps;
  uint8_t compare_metadata;
  uint8_t fuzzy_asset_paths;
  uint8_t format;
  uint8_t reserved[5];
} lightusd_next_diff_options;
uint8_t* lightusd_next_diff_json(
    const lightusd_next_diff_options* options,
    const uint8_t* left, uint32_t left_size,
    const uint8_t* left_name, uint32_t left_name_size,
    const uint8_t* right, uint32_t right_size,
    const uint8_t* right_name, uint32_t right_name_size);
/* POD-only RenderStream queries. kind: 0=mesh, 1=node, 2=light,
 * 3=points, 4=curves, 5=camera, 6=point-instancer, 7=draw,
 * 8=skeleton, 9=unsupported, 10=animation. Returns -1 for an invalid
 * handle/kind; counts are zero before a stream has loaded a scene. */
int32_t lightusd_next_render_count(uint32_t handle, uint8_t kind);
int32_t lightusd_next_render_loaded(uint32_t handle);
/* RenderStream settings: flag kinds 0=material dedup, 1=mesh merge,
 * 2=merge bake transform, 3=flatten tree, 4=mesh only,
 * 5=compute tangents, 6=build vertex indices, 7=enable composition,
 * 8=enable value-clip evaluation. Text kinds 0=render
 * settings path, 1=tangent method. Control kinds 0=clear assets,
 * 1=clear variant overrides, 2=end. Return 0 on success, -1 on an
 * invalid handle, kind, or pointer/length pair. */
int32_t lightusd_next_render_set_flag(uint32_t handle, uint8_t kind,
                                     uint8_t enabled);
int32_t lightusd_next_render_flag(uint32_t handle, uint8_t kind);
int32_t lightusd_next_render_set_text(uint32_t handle, uint8_t kind,
                                     const uint8_t* text, uint32_t size);
int32_t lightusd_next_render_set_variant_override(
    uint32_t handle, const uint8_t* key, uint32_t key_size,
    const uint8_t* selection, uint32_t selection_size);
int32_t lightusd_next_render_control(uint32_t handle, uint8_t kind);
/* Defaults to the next untrusted-load policy (512 MiB). Accepted values are
 * 1 byte through 1 GiB; enforced before staging and reader parsing. */
int32_t lightusd_next_render_set_max_input_bytes(uint32_t handle,
                                                 uint32_t limit);
int32_t lightusd_next_render_max_input_bytes(uint32_t handle);
/* Set/query the load and conversion resident-memory budget in MiB.
 * Values must fit in size_t when converted to bytes and be at least 1. */
int32_t lightusd_next_render_set_memory_limit_mb(uint32_t handle,
                                                int32_t limit_mb);
int32_t lightusd_next_render_memory_limit_mb(uint32_t handle);
/* Remaining stream resident-memory allowance in bytes. */
double lightusd_next_render_remaining_memory_bytes(uint32_t handle);
int32_t lightusd_next_render_stream_asset_start(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t expected_size);
/* Stream asset status: start returns 0 on success; append/finalize/cancel
 * return 1 when applied, 0 when incomplete/missing, and -1 on invalid args. */
int32_t lightusd_next_render_stream_asset_append(uint32_t handle,
    const uint8_t* name, uint32_t name_size, const uint8_t* bytes,
    uint32_t byte_size);
/* Progress/size return transferred/expected bytes, or -1 when absent. */
int32_t lightusd_next_render_stream_asset_progress(uint32_t handle,
    const uint8_t* name, uint32_t name_size);
int32_t lightusd_next_render_stream_asset_size(uint32_t handle,
    const uint8_t* name, uint32_t name_size);
int32_t lightusd_next_render_stream_asset_uuid(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint8_t* out, uint32_t cap);
/* Borrowed writable WASM view at the current write cursor, or 0 if absent,
 * complete, or out of bounds. Invalidated by finalize,
 * cancel, streamed-root consumption, replacement, clear, or stream destruction. */
uintptr_t lightusd_next_render_stream_asset_view(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t byte_size);
uintptr_t lightusd_next_render_stream_asset_view_at(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t offset,
    uint32_t byte_size);
int32_t lightusd_next_render_stream_asset_mark_written(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t byte_size);
/* Range mark returns 1 when recorded, 0 for absent/out-of-bounds ranges,
 * -1 for invalid args, or -2 when the 128-range bookkeeping limit is reached. */
int32_t lightusd_next_render_stream_asset_mark_range_written(uint32_t handle,
    const uint8_t* name, uint32_t name_size, uint32_t offset,
    uint32_t byte_size);
int32_t lightusd_next_render_stream_asset_finalize(uint32_t handle,
    const uint8_t* name, uint32_t name_size);
int32_t lightusd_next_render_stream_asset_finalize_to_store(
    uint32_t render_handle, uint32_t asset_store_handle,
    const uint8_t* name, uint32_t name_size);
int32_t lightusd_next_render_stream_asset_cancel(uint32_t handle,
    const uint8_t* name, uint32_t name_size);
int32_t lightusd_next_render_begin_streamed_asset(uint32_t handle,
    const uint8_t* name, uint32_t name_size);
/* Shared next-only memory asset store. Empty identifier requests a generated
 * usd-anon identifier. Register returns its UTF-8 byte length, or negative. */
int32_t lightusd_next_asset_store_register(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size,
    const uint8_t* bytes, uint32_t byte_size, uint8_t* out_identifier,
    uint32_t out_capacity);
/* Copies a validated heap span into resolver-owned storage. Returns one when
 * the named asset existed before this write, zero for a new registration. */
int32_t lightusd_next_asset_store_set_raw(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size, uintptr_t bytes,
    uint32_t byte_size);
int32_t lightusd_next_asset_store_unregister(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size);
int32_t lightusd_next_asset_store_read(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size,
    uint8_t* out, uint32_t out_capacity);
/* Returns one for a borrowed cache view, zero if absent, and a negative
 * status on failure. The pointer is invalidated by replacement, deletion,
 * eviction, clear, or destruction of the store. */
int32_t lightusd_next_asset_store_view(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size,
    uintptr_t* out_data, uint32_t* out_size);
int32_t lightusd_next_asset_store_set_alias(uint32_t handle,
    const uint8_t* authored, uint32_t authored_size,
    const uint8_t* resolved, uint32_t resolved_size);
int32_t lightusd_next_asset_store_identifier_count(uint32_t handle);
int32_t lightusd_next_asset_store_identifier_copy(uint32_t handle,
    int32_t index, uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_set_base_path(uint32_t handle,
    const uint8_t* path, uint32_t size);
int32_t lightusd_next_asset_store_base_path(uint32_t handle,
    uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_add_search_path(uint32_t handle,
    const uint8_t* path, uint32_t size);
int32_t lightusd_next_asset_store_clear_search_paths(uint32_t handle);
int32_t lightusd_next_asset_store_search_path_count(uint32_t handle);
int32_t lightusd_next_asset_store_search_path_copy(uint32_t handle,
    int32_t index, uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_set_allow_parent_paths(uint32_t handle,
    int32_t allow);
int32_t lightusd_next_asset_store_get_allow_parent_paths(uint32_t handle);
int32_t lightusd_next_asset_store_streaming_uuid(uint32_t handle,
    const uint8_t* identifier, uint32_t size, uint8_t* out, uint32_t cap);
/* Cache identity follows replacement semantics: replacing a name assigns a
 * fresh UUID. String calls return byte length, zero when absent, negative on
 * invalid handles/arguments. */
int32_t lightusd_next_asset_store_uuid(uint32_t handle,
    const uint8_t* identifier, uint32_t size, uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_find_uuid(uint32_t handle,
    const uint8_t* uuid, uint32_t size, uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_hash(uint32_t handle,
    const uint8_t* identifier, uint32_t size, uint8_t* out, uint32_t cap);
int32_t lightusd_next_asset_store_verify_hash(uint32_t handle,
    const uint8_t* identifier, uint32_t identifier_size,
    const uint8_t* hash, uint32_t hash_size);
int32_t lightusd_next_asset_store_delete_uuid(uint32_t handle,
    const uint8_t* uuid, uint32_t size);
int32_t lightusd_next_asset_store_set_memory_limit(uint32_t handle,
    uint32_t limit_bytes);
int32_t lightusd_next_asset_store_memory_limit(uint32_t handle);
int32_t lightusd_next_asset_store_memory_bytes(uint32_t handle);
int32_t lightusd_next_asset_store_set_metadata_limit(uint32_t handle,
    uint64_t limit_bytes);
uint64_t lightusd_next_asset_store_metadata_limit(uint32_t handle);
uint64_t lightusd_next_asset_store_metadata_bytes(uint32_t handle);
uint64_t lightusd_next_asset_store_cache_bytes(uint32_t handle);
uint64_t lightusd_next_asset_store_cache_max_bytes(uint32_t handle);
int32_t lightusd_next_asset_store_set_cache_max_bytes(uint32_t handle,
    uint64_t limit_bytes);
int32_t lightusd_next_asset_store_clear(uint32_t handle);
/* Value-clip setting fields: 0=sample rate, 1=use explicit range (0/1),
 * 2=range start, 3=range end. Get returns NaN for invalid handles/fields. */
int32_t lightusd_next_render_set_value_clip_setting(uint32_t handle,
                                                    uint8_t field,
                                                    double value);
double lightusd_next_render_value_clip_setting(uint32_t handle,
                                               uint8_t field);
/* Mesh setting fields: 0=sphere subdivisions (0 through 6),
 * 1=enable bone reduction (boolean), 2=target bone influences (1 through 128),
 * 3=round bone influence width to a standard GPU value (boolean). */
int32_t lightusd_next_render_set_mesh_setting(uint32_t handle, uint8_t field,
                                              int32_t value);
int32_t lightusd_next_render_mesh_setting(uint32_t handle, uint8_t field);
/* Load a USDA/USDC/USDZ byte span (up to 1 GiB). Returns 1 on success,
 * 0 for a load error (query render_error), -1 for an invalid handle. */
int32_t lightusd_next_render_begin(uint32_t handle, const uint8_t* bytes,
                                  uint32_t size);
/* Export a loaded LayerDocument to retained USDA and feed that buffer directly
 * to RenderStream, avoiding an intermediate JS/heap byte copy. */
int32_t lightusd_next_render_begin_layer_document(uint32_t render_handle,
                                                  uint32_t layer_handle);
/* Checks input-byte and current resident-memory headroom before JS staging.
 * Returns 1 when accepted, 0 and sets the stream diagnostic when rejected. */
int32_t lightusd_next_render_preflight_input(uint32_t handle, uint32_t size);
/* Reads a cached root asset after checking its stored size against the stream
 * input limit, imports the store for composition dependencies, then loads it. */
int32_t lightusd_next_render_begin_cached_asset(uint32_t render_handle,
    uint32_t asset_store_handle, const uint8_t* identifier,
    uint32_t identifier_size);
/* Copies a named dependency layer for value-clip lookup. The byte payload is
 * limited to 1 GiB. Returns 0 on success, -1 on invalid input/handle. */
int32_t lightusd_next_render_provide_asset(uint32_t handle,
                                          const uint8_t* name,
                                          uint32_t name_size,
                                          const uint8_t* bytes,
                                          uint32_t byte_size);
/* Imports a snapshot of a standalone NextAssetStore for composition and
 * resolver-backed dependencies. Returns 0 on success, -1 on invalid input. */
int32_t lightusd_next_render_import_asset_store(uint32_t render_handle,
                                               uint32_t asset_store_handle);
/* Drops imported resolver entries and releases their snapshot references;
 * stream-local provided and streaming assets remain intact. */
int32_t lightusd_next_render_clear_imported_asset_store(uint32_t handle);
/* Remove one previously provided dependency layer by normalized name.
 * Returns 1 when removed, 0 when absent, and -1 for invalid handle/input. */
int32_t lightusd_next_render_remove_asset(uint32_t handle,
                                         const uint8_t* name,
                                         uint32_t name_size);
/* Provided value-clip dependency names, sorted by normalized path. String
 * copy returns required UTF-8 bytes without a trailing NUL, or -1. */
int32_t lightusd_next_render_provided_asset_count(uint32_t handle);
int32_t lightusd_next_render_provided_asset_name(uint32_t handle,
                                                int32_t asset_id,
                                                uint8_t* out,
                                                uint32_t capacity);
/* Copy one provided dependency payload by normalized name. A null/short
 * output queries the required byte count; missing assets return -1. */
int32_t lightusd_next_render_provided_asset_bytes(uint32_t handle,
                                                 const uint8_t* name,
                                                 uint32_t name_size,
                                                 uint8_t* out,
                                                 uint32_t capacity);
/* Set the aggregate resident limit for provided dependency assets. Zero is
 * unlimited. A lower limit than current use is rejected without mutation. */
int32_t lightusd_next_render_set_provided_asset_byte_limit(uint32_t handle,
                                                          uint32_t limit);
int32_t lightusd_next_render_provided_asset_byte_limit(uint32_t handle);
/* Authored variant sets from the last loaded root layer. All counts return
 * -1 for invalid handles/indices. String kind: 0=prim path, 1=set name,
 * 2=selected variant, 3=variant name at variant_id. A null/short buffer
 * queries the required UTF-8 byte count without a trailing NUL. */
int32_t lightusd_next_render_variant_set_count(uint32_t handle);
int32_t lightusd_next_render_variant_name_count(uint32_t handle,
                                                int32_t set_id);
int32_t lightusd_next_render_variant_string(uint32_t handle, int32_t set_id,
                                            int32_t variant_id, uint8_t kind,
                                            uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_layer_asset_count(uint32_t handle, uint8_t kind);
int32_t lightusd_next_render_layer_asset_string(uint32_t handle, uint8_t kind,
                                                int32_t path_id, uint8_t* out,
                                                uint32_t cap);
/* Authored root-layer arc presence: 0=sublayers, 1=references, 2=payloads,
 * 3=inherits. Returns 0/1 for absent/present, -1 for invalid kind/handle. */
int32_t lightusd_next_render_layer_arc_present(uint32_t handle, uint8_t kind);
/* field: 0=node type, 1=parent id, 2=payload data id, 3=visible. */
int32_t lightusd_next_render_node_field(uint32_t handle, int32_t node_id,
                                        uint8_t field);
/* Copies sorted-in-node-order native instance node IDs; returns required bytes. */
int32_t lightusd_next_render_native_instance_node_ids(uint32_t handle,
                                                       uint8_t* out,
                                                       uint32_t cap);
int32_t lightusd_next_render_node_prototype_path(uint32_t handle,
                                                 int32_t node_id,
                                                 uint8_t* out,
                                                 uint32_t capacity);
int32_t lightusd_next_render_node_child(uint32_t handle, int32_t node_id,
                                        int32_t child_index);
int32_t lightusd_next_render_node_child_count(uint32_t handle,
                                              int32_t node_id);
int32_t lightusd_next_render_root_node(uint32_t handle, int32_t root_index);
/* Returns UTF-8 path bytes required; a short/null buffer queries the size. */
int32_t lightusd_next_render_node_path(uint32_t handle, int32_t node_id,
                                       uint8_t* out, uint32_t cap);
/* kind: 0=local matrix, 1=world matrix. Returns 64 bytes. */
int32_t lightusd_next_render_node_transform(uint32_t handle, int32_t node_id,
                                            uint8_t kind, uint8_t* out,
                                            uint32_t cap);
/* Copies a path-bearing resource key; returns UTF-8 bytes required. Kinds
 * 0..8 are the primary resources; 9=unsupported, 10=instancer, 12=points,
 * 13=curves, and 14=point-instance draw. */
int32_t lightusd_next_render_resource_path(uint32_t handle, uint8_t kind,
                                           int32_t resource_id, uint8_t* out,
                                           uint32_t cap);
/* Copies a resource's display name as UTF-8 bytes; a short/null buffer
 * queries the required size. Kinds match lightusd_next_render_resource_path. */
int32_t lightusd_next_render_resource_name(uint32_t handle, uint8_t kind,
                                           int32_t resource_id, uint8_t* out,
                                           uint32_t cap);
/* Copies the stable UTF-8 key for a typed scene record. Kind values match
 * lightusd_render_kind; a short/null buffer queries the required byte count. */
int32_t lightusd_next_render_record_path(uint32_t handle, uint8_t kind,
                                         int32_t record_id, uint8_t* out,
                                         uint32_t cap);
/* kind: 0=positions, 1=widths, 2=colors. Returns required bytes. */
int32_t lightusd_next_render_points_buffer(uint32_t handle, int32_t points_id,
                                           uint8_t kind, uint8_t* out,
                                           uint32_t cap);
typedef struct lightusd_next_points_info {
  uint32_t struct_size;
  int32_t point_count;
  int32_t material_id;
  int32_t has_bounds;
  float bbox_min[3];
  float bbox_max[3];
} lightusd_next_points_info;
int32_t lightusd_next_render_points_info_get(uint32_t handle,
                                             int32_t points_id,
                                             lightusd_next_points_info* out);
/* kind: 0=control points, 1=tessellated points, 2=widths, 3=colors,
 * 4=authored vertex counts, 5=tessellated vertex counts,
 * 6=tessellated widths, 7=tessellated colors, 8=opacities,
 * 9=tessellated opacities. */
int32_t lightusd_next_render_curves_buffer(uint32_t handle, int32_t curves_id,
                                           uint8_t kind, uint8_t* out,
                                           uint32_t cap);
typedef struct lightusd_next_curves_info {
  uint32_t struct_size;
  int32_t curve_count;
  int32_t control_point_count;
  int32_t tessellated_point_count;
  int32_t material_id;
  int32_t has_bounds;
  float bbox_min[3];
  float bbox_max[3];
} lightusd_next_curves_info;
int32_t lightusd_next_render_curves_info_get(uint32_t handle,
                                             int32_t curves_id,
                                             lightusd_next_curves_info* out);
/* Curve fields: 0=type, 1=basis, 2=wrap, 3=is NURBS, 4=is Hermite,
 * 5=width interpolation, 6=color interpolation, 7=opacity interpolation. */
int32_t lightusd_next_render_curves_field(uint32_t handle, int32_t curves_id,
                                          uint8_t field);
/* kind: 0=compact records, 1=positions, 2=orientations, 3=scales,
 * 4=prototype indices, 5=visibility, 6=prototype node ids,
 * 7=prototype mesh offsets, 8=prototype mesh ids, 9=prototype transforms. */
int32_t lightusd_next_render_instancer_buffer(uint32_t handle,
                                              int32_t instancer_id,
                                              uint8_t kind, uint8_t* out,
                                              uint32_t cap);
typedef struct lightusd_next_instancer_info {
  uint32_t struct_size;
  int32_t draw_start;
  int32_t draw_count;
  int32_t prototype_count;
  int32_t instance_count;
  int32_t visible_instance_count;
  int32_t has_transforms;
  int32_t has_orientations;
  int32_t has_scales;
  int32_t has_velocities;
  int32_t has_angular_velocities;
  int32_t valid;
} lightusd_next_instancer_info;
int32_t lightusd_next_render_instancer_info_get(uint32_t handle,
                                                int32_t instancer_id,
                                                lightusd_next_instancer_info* out);
/* kind: 0=prototype path, 1=validation error (prototype_id ignored).
 * Returns required UTF-8 bytes. */
int32_t lightusd_next_render_instancer_string(uint32_t handle,
                                              int32_t instancer_id,
                                              int32_t prototype_id,
                                              uint8_t kind, uint8_t* out,
                                              uint32_t cap);
/* field: 0=instancer id, 1=instance index, 2=prototype index, 3=mesh id,
 * 4=material id, 5=expanded mesh id. */
int32_t lightusd_next_render_point_instance_draw_field(uint32_t handle,
                                                       int32_t draw_id,
                                                       uint8_t field);
/* Returns 64 bytes containing one point-instance draw transform. */
int32_t lightusd_next_render_point_instance_draw_transform(
    uint32_t handle, int32_t draw_id, uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_light_field(uint32_t handle, int32_t light_id,
                                         uint8_t field);
int32_t lightusd_next_render_light_transform(uint32_t handle, int32_t light_id,
                                             uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_light_color(uint32_t handle, int32_t light_id,
                                         uint8_t* out, uint32_t cap);
/* flags: normalize, color-temperature enabled, IES normalize, light links all,
 * shadow links all, shadow enabled, dome texture file available (bits 0..6).
 * counts: light/shadow/filter target strings, light/shadow mesh IDs.
 * values: intensity, exposure, color temperature, diffuse, specular, shaping
 * focus, tint RGB, cone softness, IES angle scale, shadow distance/falloff/
 * gamma, color RGB, shadow color RGB, type parameter 0/1. */
typedef struct lightusd_next_light_info {
  uint32_t struct_size;
  int32_t type;
  uint32_t flags;
  int32_t dome_texture_id;
  int32_t dome_texture_format;
  int32_t counts[5];
  float values[22];
  float transform[16];
} lightusd_next_light_info;
int32_t lightusd_next_render_light_info_get(uint32_t handle, int32_t light_id,
                                            lightusd_next_light_info* out);
/* kind: 0=IES file, 1/2/3=light/shadow/filter target by item index,
 * 4=resolved dome texture file. Returns required UTF-8 bytes. */
int32_t lightusd_next_render_light_string(uint32_t handle, int32_t light_id,
                                          uint8_t kind, int32_t item_id,
                                          uint8_t* out, uint32_t cap);
/* kind: 0=resolved light mesh IDs, 1=resolved shadow mesh IDs (i32). */
int32_t lightusd_next_render_light_mesh_ids(uint32_t handle, int32_t light_id,
                                            uint8_t kind, uint8_t* out,
                                            uint32_t cap);
int32_t lightusd_next_render_camera_field(uint32_t handle, int32_t camera_id,
                                          uint8_t field);
typedef struct lightusd_next_camera_info {
  uint32_t struct_size;
  int32_t type;
  float focal_length;
  float horizontal_aperture;
  float vertical_aperture;
  float ortho_width;
  float near_clip;
  float far_clip;
  float focus_distance;
  float fstop;
  float fov_x;
  float fov_y;
  float aspect;
  float horizontal_aperture_offset;
  float vertical_aperture_offset;
  float exposure;
  int32_t stereo_role;
  int32_t reserved;
  double shutter_open;
  double shutter_close;
} lightusd_next_camera_info;
int32_t lightusd_next_render_camera_info_get(uint32_t handle,
                                             int32_t camera_id,
                                             lightusd_next_camera_info* out);
int32_t lightusd_next_render_camera_transform(uint32_t handle, int32_t camera_id,
                                              uint8_t* out, uint32_t cap);
/* Returns 8 f32 values: focal length, horizontal/vertical aperture,
 * orthographic width, near/far clip, and horizontal/vertical FOV. */
int32_t lightusd_next_render_camera_optics(uint32_t handle, int32_t camera_id,
                                           uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_skeleton_field(uint32_t handle, int32_t skeleton_id,
                                            uint8_t field);
int32_t lightusd_next_render_skeleton_joint_buffer(uint32_t handle,
                                                   int32_t skeleton_id,
                                                   uint8_t kind, uint8_t* out,
                                                   uint32_t cap);
/* Copy one joint's ordered child ids as i32 values. */
int32_t lightusd_next_render_skeleton_joint_children(uint32_t handle,
                                                     int32_t skeleton_id,
                                                     int32_t joint_id,
                                                     uint8_t* out,
                                                     uint32_t cap);
/* kind: 0=joint name, 1=joint path, 2=animation source path (joint_id ignored).
 * Returns UTF-8 bytes required. */
int32_t lightusd_next_render_skeleton_joint_string(uint32_t handle,
                                                   int32_t skeleton_id,
                                                   int32_t joint_id, uint8_t kind,
                                                   uint8_t* out, uint32_t cap);
/* field: 0=vertex count, 1=face/triangle count, 2=material id,
 * 3=has normals, 4=has primary UVs, 5=has tangents, 6=has secondary UVs,
 * 7=has colors, 8=has skin, 9=has bounds, 10=skeleton id. */
int32_t lightusd_next_render_mesh_field(uint32_t handle, int32_t mesh_id,
                                        uint8_t field);
int32_t lightusd_next_render_mesh_primvar_count(uint32_t handle, int32_t mesh_id);
int32_t lightusd_next_render_mesh_primvar_field(uint32_t handle, int32_t mesh_id,
                                                int32_t primvar_id,
                                                uint8_t field);
int32_t lightusd_next_render_mesh_primvar_name(uint32_t handle, int32_t mesh_id,
                                               int32_t primvar_id, uint8_t* out,
                                               uint32_t cap);
/* kind: 0=primvar data, 1=uint32 indices. Returns required bytes. */
int32_t lightusd_next_render_mesh_primvar_buffer(uint32_t handle, int32_t mesh_id,
                                                 int32_t primvar_id, uint8_t kind,
                                                 uint8_t* out, uint32_t cap);
/* kind: 0=points f32x3, 1=indices u32, 2=normals f32x3, 3=primary UV f32x2,
 * 4=tangents f32x4, 5=colors f32, 6=opacities f32, 7=skin joint indices u16,
 * 8=skin joint weights f32, 9=secondary UV f32x2.
 * Returns required bytes; a short/null buffer queries the size. */
int32_t lightusd_next_render_mesh_buffer(uint32_t handle, int32_t mesh_id,
                                         uint8_t kind, uint8_t* out,
                                         uint32_t cap);
/* Legacy computeMeshTangents: makes the tangent stream (kind 4) available for
 * a normal-mapped mesh while tangents are deferred. Returns 1 for a valid mesh
 * id, 0 for an invalid one, and -1 for an invalid handle. */
int32_t lightusd_next_render_request_mesh_tangents(uint32_t handle,
                                                   int32_t mesh_id);
/* Borrowed output geometry. Arrays 0..6 are points f32, indices u32,
 * normals f32, UV0 f32, tangents f32, joint indices u16, joint weights f32.
 * Pointers remain valid until the next mesh materialization or stream end;
 * callers must copy them before either operation. Flag 0=double-sided,
 * 1=has geometric bind transform. */
typedef struct lightusd_next_mesh_view {
  uint32_t struct_size;
  uint32_t flags;
  int32_t material_id;
  int32_t skeleton_id;
  int32_t element_size;
  uint32_t reserved;
  double local_matrix[16];
  double world_matrix[16];
  double geom_bind_matrix[16];
  uint64_t ptr[7];
  uint32_t length[7];
} lightusd_next_mesh_view;
int32_t lightusd_next_render_mesh_view_get(uint32_t handle, int32_t mesh_id,
                                          lightusd_next_mesh_view* out);
/* kind: 0=output name, 1=output path, 2=skeleton path,
 * 3=tangent method, 4=last mesh-view build error.
 * Returns UTF-8 bytes required. */
int32_t lightusd_next_render_mesh_view_string(uint32_t handle, int32_t mesh_id,
                                             uint8_t kind, uint8_t* out,
                                             uint32_t cap);
/* Browser mesh subset payload: int32 material_count, int32 group_count,
 * material_count output material IDs, then group_count triplets
 * [triangle start, triangle count, material index]. Zero bytes means no
 * subset payload. Returns bytes required; a short buffer queries size. */
int32_t lightusd_next_render_mesh_subset_output(uint32_t handle, int32_t mesh_id,
                                               uint8_t* out, uint32_t cap);
/* Blend shapes on browser output meshes. Inbetween ID -1 addresses the base
 * shape. Offset kind 0=point, 1=normal (base only). Offset copies return
 * tightly packed float3 values in output-mesh vertex order. Callers own the
 * destination bytes; the mesh view is built on demand for remapping. */
int32_t lightusd_next_render_mesh_blend_shape_count(uint32_t handle,
                                                    int32_t mesh_id);
typedef struct lightusd_next_blend_shape_info {
  uint32_t struct_size;
  float weight;
  uint32_t inbetween_count;
  uint32_t flags;  /* bit 0: base normal offsets present */
} lightusd_next_blend_shape_info;
int32_t lightusd_next_render_mesh_blend_shape_info_get(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    lightusd_next_blend_shape_info* out);
int32_t lightusd_next_render_mesh_blend_shape_name(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_mesh_blend_shape_offsets(
    uint32_t handle, int32_t mesh_id, int32_t shape_id, int32_t inbetween_id,
    uint8_t kind, uint8_t* out, uint32_t cap);
/* Browser output-material ID, distinct from RenderScene material IDs.
 * value slots: base color[0..2], metallic[3], roughness[4], opacity[5],
 * occlusion[6], emissive[7..9], opacity threshold[10], working matrix[11..19],
 * hair tints[20..28], roughness pairs[29..34], absorption[35..37],
 * IOR[38], cuticle angle[39]. Flags: 0=render material, 1=MaterialX authored,
 * 2=hair. */
typedef struct lightusd_next_output_material_info {
  uint32_t struct_size;
  int32_t id;
  uint32_t flags;
  uint32_t reserved;
  float value[40];
} lightusd_next_output_material_info;
int32_t lightusd_next_render_output_material_info_get(
    uint32_t handle, int32_t material_id,
    lightusd_next_output_material_info* out);
/* kind: 0=key, 1=prim path, 2=shader type, 3=working colorspace,
 * 4=material JSON, 5=preferred surface nodegraph JSON, 6=MaterialX version,
 * 7=namespace, 8=colorspace, 9=source URI, 10=volume nodegraph JSON,
 * 11=PreviewSurface utility nodegraph JSON. */
int32_t lightusd_next_render_output_material_string(
    uint32_t handle, int32_t material_id, uint8_t kind, uint8_t* out,
    uint32_t cap);
/* format: 0=JSON, 1=XML; other values return the supported-format error. */
int32_t lightusd_next_render_material_format_status(
    uint32_t handle, int32_t material_id, uint8_t format);
int32_t lightusd_next_render_material_format_string(
    uint32_t handle, int32_t material_id, uint8_t format, uint8_t* out,
    uint32_t cap);
/* Light serialization: format 0=JSON, 1=XML (currently unsupported by the
 * shared legacy serializer); status 1=success, 0=owned error string, -1=bad handle. */
int32_t lightusd_next_render_light_format_status(
    uint32_t handle, int32_t light_id, uint8_t format);
int32_t lightusd_next_render_light_format_string(
    uint32_t handle, int32_t light_id, uint8_t format, uint8_t* out,
    uint32_t cap);
/* Texture slot: 0=base color, 1=normal, 2=roughness, 3=metallic,
 * 4=occlusion, 5=emissive, 6=opacity. Flags: 0=metadata path present,
 * 1=UDIM, 2=color transform valid, 3=transform bypass, 4=source is data. */
typedef struct lightusd_next_output_texture_meta {
  uint32_t struct_size;
  uint32_t flags;
  float source_gamma;
  float source_linear_bias;
  float source_to_display_linear[9];
} lightusd_next_output_texture_meta;
int32_t lightusd_next_render_output_texture_meta_get(
    uint32_t handle, int32_t material_id, uint8_t slot,
    lightusd_next_output_texture_meta* out);
/* kind: 0=texture path, 1=metadata path, 2=source colorspace,
 * 3=wrap S, 4=wrap T. */
int32_t lightusd_next_render_output_texture_string(
    uint32_t handle, int32_t material_id, uint8_t slot, uint8_t kind,
    uint8_t* out, uint32_t cap);
/* field: 0=shader type, 1=alpha mode, 2=double-sided,
 * 3=opacity*1e6, 4=roughness*1e6, 5=clearcoat*1e6,
 * 6=clearcoat roughness*1e6, 7=default fallback. */
int32_t lightusd_next_render_material_field(uint32_t handle,
                                            int32_t material_id,
                                            uint8_t field);
/* Material diagnostics are typed records. String kind: 0=material path,
 * 1=node path, 2=shader id, 3=message. String calls return required bytes. */
int32_t lightusd_next_render_material_diagnostic_count(uint32_t handle,
                                                       int32_t material_id);
int32_t lightusd_next_render_material_diagnostic_kind(uint32_t handle,
                                                      int32_t material_id,
                                                      int32_t diagnostic_id);
int32_t lightusd_next_render_material_diagnostic_string(
    uint32_t handle, int32_t material_id, int32_t diagnostic_id,
    uint8_t kind, uint8_t* out, uint32_t cap);
/* Common shader parameter slots: 0=base/diffuse color, 1=emissive,
 * 2=metallic, 3=roughness, 4=opacity. Returns 16 bytes of f32x4. */
int32_t lightusd_next_render_material_param_buffer(uint32_t handle,
                                                   int32_t material_id,
                                                   uint8_t param, uint8_t* out,
                                                   uint32_t cap);
int32_t lightusd_next_render_material_param_texture(uint32_t handle,
                                                    int32_t material_id,
                                                    uint8_t param);
/* field: 0=image id, 1=width, 2=height, 3=channels, 4=mip levels,
 * 5=loaded, 6=wrap S, 7=wrap T, 8=output channel, 9=UV rotation*1e6,
 * 10=has authored UsdTransform2d, 11=is UDIM, 12=UDIM record id. */
int32_t lightusd_next_render_texture_field(uint32_t handle, int32_t texture_id,
                                           uint8_t field);
/* kind: 0=UV primvar, 1=source color space, 2=target color space. */
int32_t lightusd_next_render_texture_string(uint32_t handle,
                                            int32_t texture_id,
                                            uint8_t kind, uint8_t* out,
                                            uint32_t cap);
/* Returns required decoded image bytes; a short/null buffer queries the size. */
int32_t lightusd_next_render_texture_buffer(uint32_t handle, int32_t texture_id,
                                           uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_image_field(uint32_t handle, int32_t image_id,
                                         uint8_t field);
int32_t lightusd_next_render_image_asset_identifier(uint32_t handle,
    int32_t image_id, uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_image_buffer(uint32_t handle, int32_t image_id,
                                          uint8_t* out, uint32_t cap);
uintptr_t lightusd_next_render_image_data(uint32_t handle, int32_t image_id);
/* Returns 13 f32 values: offset[2], scale[2], rotation, bias[4],
 * value-scale[4]. A short/null buffer queries the required byte count. */
int32_t lightusd_next_render_texture_sampling_buffer(uint32_t handle,
                                                     int32_t texture_id,
                                                     uint8_t* out,
                                                     uint32_t cap);
/* Returns 5 f32 values: authored txRotation (degrees), txScale[2],
 * txTranslation[2]. A short/null buffer queries the required byte count. */
int32_t lightusd_next_render_texture_transform_buffer(uint32_t handle,
                                                      int32_t texture_id,
                                                      uint8_t* out,
                                                      uint32_t cap);
int32_t lightusd_next_render_texture_udim_remap_buffer(uint32_t handle,
                                                       int32_t texture_id,
                                                       uint8_t* out,
                                                       uint32_t cap);
/* UDIM tile records are packed {udim,u,v,image_id} int32 tuples. */
int32_t lightusd_next_render_udim_count(uint32_t handle);
int32_t lightusd_next_render_udim_tile_count(uint32_t handle, int32_t udim_id);
int32_t lightusd_next_render_udim_tiles(uint32_t handle, int32_t udim_id,
                                        uint8_t* out, uint32_t cap);
/* kind: 0=primName, 1=absPath, 2=displayName, 3=assetIdentifier. */
int32_t lightusd_next_render_udim_string(uint32_t handle, int32_t udim_id,
                                         uint8_t kind, uint8_t* out,
                                         uint32_t cap);
/* 14 f32 values: flags(valid,bypass,data), source gamma/bias, 3x3 transform. */
int32_t lightusd_next_render_texture_color_transform_buffer(
    uint32_t handle, int32_t texture_id, uint8_t* out, uint32_t cap);
/* field: 0=meters/unit*1e6, 1=up axis, 2=start*1e3, 3=end*1e3,
 * 4=frames/second*1e3. */
int32_t lightusd_next_render_scene_field(uint32_t handle, uint8_t field);
/* kind: 0=name, 1=default prim, 2=render-settings path,
 * 3=working color space, 4=authored up axis, 5=stage comment. Returns required UTF-8 bytes
 * excluding NUL. */
/* Scene strings: 0=name, 1=default prim, 2=render settings path,
 * 3=working color space, 4=up axis, 5=comment, 6=copyright. */
int32_t lightusd_next_render_scene_string(uint32_t handle, uint8_t kind,
                                          uint8_t* out, uint32_t cap);
typedef struct lightusd_next_scene_metadata {
  uint32_t struct_size;
  uint32_t reserved;
  double meters_per_unit;
  double kilograms_per_unit;
  double frames_per_second;
  double time_codes_per_second;
  double start_time_code;
  double end_time_code;
  float working_to_display_linear[9];
} lightusd_next_scene_metadata;
/* Returns 1 before loading, 0 on success, -1 for invalid handle/size. */
int32_t lightusd_next_render_scene_metadata_get(
    uint32_t handle, lightusd_next_scene_metadata* out);
/* kind: 0=prim path, 1=type name, 2=reason. */
int32_t lightusd_next_render_unsupported_string(uint32_t handle,
                                                int32_t unsupported_id,
                                                uint8_t kind, uint8_t* out,
                                                uint32_t cap);

typedef struct lightusd_next_render_info {
  uint32_t struct_size;
  int32_t mesh_count;
  int32_t node_count;
  int32_t light_count;
  int32_t points_count;
  int32_t curves_count;
  int32_t camera_count;
  int32_t point_instancer_count;
  int32_t point_instance_draw_count;
  int32_t skeleton_count;
  int32_t unsupported_renderable_count;
  int32_t animation_count;
  int32_t error_size;
} lightusd_next_render_info;

typedef struct lightusd_next_render_stats {
  uint32_t struct_size;
  int32_t source_meshes;
  int32_t source_materials;
  int32_t source_textures;
  int32_t optimized_meshes;
  int32_t optimized_materials;
  int32_t optimized_textures;
  int32_t merged_meshes;
  int32_t merge_groups;
  int32_t skipped_merge_meshes;
  double stage_load_ms;
  double input_copy_ms;
  double input_bytes;
  double stage_memory_bytes;
  double render_scene_memory_bytes;
  double geometry_borrowed_bytes;
  double geometry_materialized_bytes;
} lightusd_next_render_stats;

int32_t lightusd_next_render_stats_get(uint32_t handle,
                                       lightusd_next_render_stats* out);

/* Companion stats record for getStats(). Flags: bit 0=material dedup,
 * 1=mesh merge, 2=bake transforms, 3=flatten render tree,
 * 4=render scene available.
 * timings_ms: composition, mesh discovery, optimize, material, material
 * identity, material conversion, geometry build, merge append.
 * memory_bytes: provided assets, mesh points, normals, UVs, topology,
 * triangulation. cache_counts: identity hits/misses, graph hits/misses.
 * scene_counts: nodes, meshes, points, curves, instancers, instance draws,
 * materials, textures, images, lights, cameras, animations, skeletons,
 * unsupported renderables, warnings. */
typedef struct lightusd_next_render_stats_detail {
  uint32_t struct_size;
  uint32_t flags;
  double timings_ms[8];
  double memory_bytes[6];
  int32_t cache_counts[4];
  int32_t scene_counts[15];
} lightusd_next_render_stats_detail;

int32_t lightusd_next_render_stats_detail_get(
    uint32_t handle, lightusd_next_render_stats_detail* out);

typedef struct lightusd_next_animation_info {
  uint32_t struct_size;
  int32_t id;
  double start_time;
  double end_time;
  int32_t track_count;
  int32_t target_node_count;
  int32_t clip_asset_count;
  int32_t flags; /* bit 0=skeletal, 1=node, 2=value-clip baked */
} lightusd_next_animation_info;

int32_t lightusd_next_render_animation_info_get(
    uint32_t handle, int32_t animation_id, lightusd_next_animation_info* out);
/* Copy one retained value-clip asset path as UTF-8 bytes. */
int32_t lightusd_next_render_animation_clip_asset(uint32_t handle,
                                                  int32_t animation_id,
                                                  int32_t asset_id,
                                                  uint8_t* out, uint32_t cap);
int32_t lightusd_next_render_animation_channel_count(uint32_t handle,
                                                     int32_t animation_id);
/* field: 0=target node, 1=target skeleton, 2=keyframe count,
 * 3=element count, 4=value stride, 5=interpolation, 6=skeletal,
 * 7=target path, 8=joint-order count, 9=blend-shape-order count,
 * 10=joint-remap count. */
int32_t lightusd_next_render_animation_channel_field(uint32_t handle,
                                                     int32_t animation_id,
                                                     int32_t channel_id,
                                                     uint8_t field);
/* kind: 0=f64 keyframe times, 1=f32 path-width values,
 * 2=f32 full array values, 3=i32 joint remap. */
int32_t lightusd_next_render_animation_channel_buffer(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    uint8_t* out, uint32_t cap);
/* Borrowed float array; the pointer becomes invalid after stream end or
 * reloading the scene. Use only while the render session is live. */
typedef struct lightusd_next_animation_array_view {
  uint32_t struct_size;
  uint32_t reserved;
  uint64_t ptr;
  uint32_t length;
  uint32_t comps;
} lightusd_next_animation_array_view;
int32_t lightusd_next_render_animation_array_view_get(
    uint32_t handle, int32_t animation_id, int32_t channel_id,
    lightusd_next_animation_array_view* out);
/* kind: 0=target prim path, 1=property name, 2=target skeleton path. */
int32_t lightusd_next_render_animation_channel_string(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    uint8_t* out, uint32_t cap);
/* kind: 0=joint order, 1=blend-shape order. */
int32_t lightusd_next_render_animation_channel_order_string(
    uint32_t handle, int32_t animation_id, int32_t channel_id, uint8_t kind,
    int32_t order_id, uint8_t* out, uint32_t cap);

/* Fills one caller-owned POD block with all RenderStream counts and the
 * current error length. Returns zero on success, -1 for an invalid handle or
 * too-small struct. The error text itself is retrieved with
 * lightusd_next_render_error. */
int32_t lightusd_next_render_info_get(uint32_t handle,
                                      lightusd_next_render_info* out);
/* Copies the current RenderStream error into a caller-owned UTF-8 buffer.
 * Returns the required byte count excluding the NUL, or -1 for an invalid
 * handle. A zero-capacity query is valid. */
int32_t lightusd_next_render_error(uint32_t handle, char* out, uint32_t cap);
/* Copies current load/composition warning text; same bounded query contract. */
int32_t lightusd_next_render_warning(uint32_t handle, char* out, uint32_t cap);
/* field: 0=dependency count, 1=dependency string, 2=issue count,
 * 3=issue code, 4=issue site, 5=issue message. String queries return byte
 * length and NUL-terminate when a non-empty output buffer is supplied. */
int32_t lightusd_next_render_composition_record(uint32_t handle,
    uint8_t field, uint32_t index, char* out, uint32_t cap);
/* NextFlattenSession typed input path. begin/provide return 1 for success,
 * 0 for a session error, -1 for an invalid handle. Other mutators return
 * 0 on success and -1 for invalid input/handle. */
int32_t lightusd_next_flatten_begin(uint32_t handle, const uint8_t* root,
                                   uint32_t root_size, const uint8_t* name,
                                   uint32_t name_size, uint8_t lazy_arrays);
int32_t lightusd_next_flatten_provide_layer(uint32_t handle,
                                           const uint8_t* key,
                                           uint32_t key_size,
                                           const uint8_t* data,
                                           uint32_t data_size);
/* Append one relative sublayer path to the authored root before flattening.
 * Returns 1 on success, 0 on parse/budget error, -1 on invalid handle. */
int32_t lightusd_next_flatten_add_sublayer(uint32_t handle,
                                           const uint8_t* path,
                                           uint32_t path_size);
/* Arc kind: 0=reference, 1=payload, 2=inherit, 3=specialize. Reference and
 * payload may include an asset path; target_path is an absolute prim path.
 * Returns 1 on success, 0 on author/parse/budget error, -1 on invalid input
 * or handle. */
int32_t lightusd_next_flatten_add_prim_arc(
    uint32_t handle, uint8_t kind, uint8_t list_op, const uint8_t* prim_path,
    uint32_t prim_size, const uint8_t* asset_path, uint32_t asset_size,
    const uint8_t* target_path, uint32_t target_size);
/* Atomically replace the root's asset-valued property remap from a JSON
 * object. Returns 1 on success, 0 on parse/budget failure, -1 on bad handle. */
int32_t lightusd_next_flatten_set_asset_path_remap(
    uint32_t handle, const uint8_t* json, uint32_t json_size);
/* Mutate retained root asset-valued properties with one remap object. Returns
 * the number of changed values, or a negative status without changing root. */
int32_t lightusd_next_flatten_remap_layer_asset_paths(
    uint32_t handle, const uint8_t* json, uint32_t json_size);
/* Aggregate root + provided-layer byte cap. Defaults to 512 MiB; zero and
 * values below retained input are rejected. */
int32_t lightusd_next_flatten_set_max_input_bytes(uint32_t handle,
                                                  uint32_t limit_bytes);
int32_t lightusd_next_flatten_max_input_bytes(uint32_t handle);
/* Hard writer cap applied before Crate output grows; defaults to 512 MiB. */
int32_t lightusd_next_flatten_set_max_output_bytes(uint32_t handle,
                                                   uint32_t limit_bytes);
int32_t lightusd_next_flatten_max_output_bytes(uint32_t handle);
int32_t lightusd_next_flatten_input_bytes(uint32_t handle);
int32_t lightusd_next_flatten_preflight_layer(uint32_t handle,
    const uint8_t* key, uint32_t key_size, uint32_t data_size);
int32_t lightusd_next_flatten_set_variant(uint32_t handle,
                                         const uint8_t* key,
                                         uint32_t key_size,
                                         const uint8_t* selection,
                                         uint32_t selection_size);
int32_t lightusd_next_flatten_end(uint32_t handle);
int32_t lightusd_next_flatten_error(uint32_t handle, uint8_t* out,
                                   uint32_t cap);
/* One flatten step. callback_id=0 buffers the output; a nonzero ID names a
 * JS callback registered by the post-JS adapter. status: -1=not started,
 * 0=error, 1=ready (callback abort), 2=need-layer, 3=done. */
typedef struct lightusd_next_flatten_step_info {
  uint32_t struct_size;
  int32_t status;
  uint32_t asset_path_count;
  uint32_t composition_error_count;
  double input_bytes;
  double output_bytes;
  double prim_count;
  double arrays_passed_through;
  double arrays_reencoded;
  double read_ms;
  double compose_ms;
  double write_ms;
} lightusd_next_flatten_step_info;
int32_t lightusd_next_flatten_step(uint32_t handle, uint32_t callback_id,
                                  lightusd_next_flatten_step_info* out);
/* step_buffer kind: 0=buffered output, 1=needed layer key,
 * 2=referenced asset path, 3=composition diagnostic,
 * 4=loaded layer dependency identifier. */
int32_t lightusd_next_flatten_step_buffer(uint32_t handle, uint8_t kind,
                                         uint32_t index, uint8_t* out,
                                         uint32_t cap);
/* Counts identifiers loaded during the most recent flatten step; values are
 * copied with step_buffer kind 4 and invalidated by the next step/release. */
int32_t lightusd_next_flatten_layer_dependency_count(uint32_t handle);
int32_t lightusd_next_flatten_release_step(uint32_t handle);
#ifdef __cplusplus
}
namespace lightusd {
namespace web_next {
void* NextCreateObject(uint32_t kind);
void NextDestroyObject(uint32_t kind, void* object);
int NextFlattenBegin(void* object, const uint8_t* root, uint32_t root_size,
                     const uint8_t* name, uint32_t name_size, bool lazy_arrays);
int NextFlattenProvideLayer(void* object, const uint8_t* key,
                            uint32_t key_size, const uint8_t* data,
                            uint32_t data_size);
int NextFlattenAddSublayer(void* object, const uint8_t* path,
                           uint32_t path_size);
int NextFlattenAddPrimArc(void* object, uint8_t kind, uint8_t list_op,
                          const uint8_t* prim_path, uint32_t prim_size,
                          const uint8_t* asset_path, uint32_t asset_size,
                          const uint8_t* target_path, uint32_t target_size);
int NextFlattenSetAssetPathRemap(void* object, const uint8_t* json,
                                uint32_t json_size);
int NextFlattenRemapLayerAssetPaths(void* object, const uint8_t* json,
                                   uint32_t json_size);
int NextFlattenSetMaxInputBytes(void* object, uint32_t limit_bytes);
int NextFlattenMaxInputBytes(void* object);
int NextFlattenSetMaxOutputBytes(void* object, uint32_t limit_bytes);
int NextFlattenMaxOutputBytes(void* object);
int NextFlattenInputBytes(void* object);
int NextFlattenPreflightLayer(void* object, const uint8_t* key,
                              uint32_t key_size, uint32_t data_size);
int NextFlattenSetVariant(void* object, const uint8_t* key,
                          uint32_t key_size, const uint8_t* selection,
                          uint32_t selection_size);
void NextFlattenEnd(void* object);
int NextFlattenError(void* object, uint8_t* out, uint32_t cap);
int NextFlattenStep(void* object, uint32_t callback_id,
                    lightusd_next_flatten_step_info* out);
int NextFlattenStepBuffer(void* object, uint8_t kind, uint32_t index,
                          uint8_t* out, uint32_t cap);
int NextFlattenLayerDependencyCount(void* object);
void NextFlattenReleaseStep(void* object);
int NextSubdivRefine(void* object, const float* points, uint32_t point_values,
                    const uint32_t* face_counts, uint32_t face_count,
                    const uint32_t* face_indices, uint32_t index_count,
                    const float* uv_values, uint32_t uv_value_count,
                    const uint32_t* uv_indices, uint32_t uv_index_count,
                    const lightusd_next_subdiv_options* options,
                    uint32_t callback_id);
int NextSubdivError(void* object, uint8_t* out, uint32_t cap);
int NextConverterRewrite(void* object, const uint8_t* data, uint32_t size,
                         const lightusd_next_rewrite_options* options,
                         lightusd_next_rewrite_info* out);
int NextConverterRewriteBuffer(void* object, uint8_t* out, uint32_t cap);
int NextConverterString(void* object, uint8_t kind, uint8_t* out, uint32_t cap);
int NextConverterSetBytes(void* object, uint8_t kind, const uint8_t* name,
                          uint32_t name_size, const uint8_t* data,
                          uint32_t size);
int NextConverterPreflightAsset(void* object, const uint8_t* name,
                                uint32_t name_size, uint32_t size);
int NextConverterRootPreflight(void* object, uint32_t size);
int NextConverterPreflightMesh(void* object, const uint8_t* name,
                               uint32_t name_size, uint32_t position_count,
                               uint32_t normal_count, uint32_t uv_count,
                               uint32_t index_count);
int NextConverterSetMesh(void* object, const uint8_t* name,
                         uint32_t name_size, const float* positions,
                         uint32_t position_count, const float* normals,
                         uint32_t normal_count, const float* uvs,
                         uint32_t uv_count, const uint32_t* indices,
                         uint32_t index_count);
int NextConverterControl(void* object, uint8_t kind, int32_t first,
                         int32_t second);
int NextConverterExport(void* object, uint8_t kind);
int NextConverterExportWithOptions(void* object, uint8_t kind,
                                   uint8_t root_format);
int NextConverterExportWithRemap(void* object, const uint8_t* remap_json,
                                 uint32_t remap_json_size,
                                 uint8_t root_format);
const uint8_t* NextConverterExportData(void* object);
int NextConverterExportBuffer(void* object, uint8_t* out, uint32_t cap);
uint8_t* NextValidateJSON(const uint8_t* data, uint32_t size,
                          const uint8_t* filename, uint32_t filename_size,
                          const uint8_t* options, uint32_t options_size);
uint8_t* NextDiffJSON(const lightusd_next_diff_options* options,
                      const uint8_t* left, uint32_t left_size,
                      const uint8_t* left_name, uint32_t left_name_size,
                      const uint8_t* right, uint32_t right_size,
                      const uint8_t* right_name, uint32_t right_name_size);
}
}
#endif
#endif
