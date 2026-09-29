/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_WEB_BINDING_COMBINED_API_H_
#define LIGHTUSD_WEB_BINDING_COMBINED_API_H_
#include <stdint.h>
#ifdef __cplusplus
#include <map>
#include <string>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lightusd_combined_flatten_info {
  uint32_t struct_size;
  uint32_t data_size;
  uint32_t reserved0;
  uint32_t reserved1;
  double input_bytes;
  double output_bytes;
  double prim_count;
  double arrays_passed_through;
  double arrays_reencoded;
  double asset_paths_remapped;
  double read_ms;
  double compose_ms;
  double write_ms;
} lightusd_combined_flatten_info;

/* One-shot USDC flatten. Returns 1 on success, 0 on a pipeline error, and -1
 * for invalid arguments. Result bytes and error remain available until the
 * next call. Copy functions return the required byte count or -1. */
int32_t lightusd_combined_next_flatten_usdc(const uint8_t* data,
                                            uint32_t size,
                                            uint8_t lazy_arrays,
                                            lightusd_combined_flatten_info* out);
int32_t lightusd_combined_set_flatten_error(const uint8_t* data,
                                            uint32_t size);
int32_t lightusd_combined_next_flatten_buffer(
    void* loader, const uint8_t* uuid, uint32_t uuid_size, uint8_t lazy_arrays,
    lightusd_combined_flatten_info* out);
int32_t lightusd_combined_next_flatten_async_end(void* loader,
                                                const uint8_t* session,
                                                uint32_t session_size);
int32_t lightusd_combined_next_flatten_async_provide_layer(
    void* loader, const uint8_t* session, uint32_t session_size,
    const uint8_t* key, uint32_t key_size, const uint8_t* data,
    uint32_t data_size);
int32_t lightusd_combined_next_flatten_async_begin(
    void* loader, const uint8_t* uuid, uint32_t uuid_size,
    const uint8_t* root_name, uint32_t root_name_size, uint8_t lazy_arrays,
    uint8_t* session_out, uint32_t session_cap, uint32_t* session_size_out);
int32_t lightusd_combined_next_flatten_async_begin_remap(
    void* loader, const uint8_t* uuid, uint32_t uuid_size,
    const uint8_t* root_name, uint32_t root_name_size, uint8_t lazy_arrays,
    const uint8_t* remap_pairs, uint32_t remap_pairs_size,
    uint8_t* session_out, uint32_t session_cap, uint32_t* session_size_out);
int32_t lightusd_combined_next_flatten_async_begin_remap_variants(
    void* loader, const uint8_t* uuid, uint32_t uuid_size,
    const uint8_t* root_name, uint32_t root_name_size, uint8_t lazy_arrays,
    const uint8_t* remap_pairs, uint32_t remap_pairs_size,
    const uint8_t* variant_pairs, uint32_t variant_pairs_size,
    uint8_t* session_out, uint32_t session_cap, uint32_t* session_size_out);
int32_t lightusd_combined_next_flatten_buffer_maps(
    void* loader, const uint8_t* uuid, uint32_t uuid_size,
    uint8_t lazy_arrays, const uint8_t* remap_pairs,
    uint32_t remap_pairs_size, const uint8_t* variant_pairs,
    uint32_t variant_pairs_size, lightusd_combined_flatten_info* out);
/* Result of a callback-driven flatten. status: -1 = rejected before the
 * pipeline ran, 0 = pipeline error (both leave a message in the error copy),
 * 1 = sink abort on a session step (the session stays ready), 2 = the session
 * needs a layer (key string 0), 3 = done. data_size is nonzero only for
 * buffered output, read through lightusd_combined_next_flatten_copy. */
typedef struct lightusd_combined_flatten_step_info {
  uint32_t struct_size;
  int32_t status;
  uint32_t data_size;
  uint32_t asset_path_count;
  uint32_t composition_error_count;
  uint32_t reserved;
  double input_bytes;
  double output_bytes;
  double prim_count;
  double arrays_passed_through;
  double arrays_reencoded;
  double asset_paths_remapped;
  double read_ms;
  double compose_ms;
  double write_ms;
} lightusd_combined_flatten_step_info;

/* Callback IDs name JS functions registered by combined-api.js for the
 * duration of one call; 0 means none. sink_id=0 buffers the output on the
 * multi-buffer and session-step calls. Each call returns 0 when `out` was
 * filled and -1 for invalid arguments. */
int32_t lightusd_combined_next_flatten_to_sink(
    void* loader, const uint8_t* uuid, uint32_t uuid_size, uint8_t lazy_arrays,
    uint32_t sink_id, const uint8_t* remap_pairs, uint32_t remap_pairs_size,
    const uint8_t* variant_pairs, uint32_t variant_pairs_size,
    lightusd_combined_flatten_step_info* out);
int32_t lightusd_combined_next_flatten_multi(
    void* loader, const uint8_t* uuid, uint32_t uuid_size,
    const uint8_t* root_name, uint32_t root_name_size, uint8_t lazy_arrays,
    uint32_t sink_id, uint32_t exists_id, uint32_t fetch_id,
    const uint8_t* remap_pairs, uint32_t remap_pairs_size,
    const uint8_t* variant_pairs, uint32_t variant_pairs_size,
    lightusd_combined_flatten_step_info* out);
int32_t lightusd_combined_next_flatten_async_step(
    void* loader, const uint8_t* session, uint32_t session_size,
    uint32_t sink_id, lightusd_combined_flatten_step_info* out);
/* String results of the last callback-driven flatten. kind: 0 = need-layer
 * key, 1 = referenced asset path, 2 = composition error. Returns the
 * required byte count or -1. */
int32_t lightusd_combined_next_flatten_string(uint8_t kind, uint32_t index,
                                             uint8_t* out, uint32_t cap);
/* Drops the retained output, error, and strings of the last flatten. */
void lightusd_combined_next_flatten_release(void);
int32_t lightusd_combined_next_flatten_copy(uint8_t* out, uint32_t cap);
int32_t lightusd_combined_next_flatten_error(uint8_t* out, uint32_t cap);
/* Composition queries and passes on the loader's legacy layer. */
enum {
  LIGHTUSD_COMBINED_HAS_SUBLAYERS = 0,
  LIGHTUSD_COMBINED_HAS_REFERENCES = 1,
  LIGHTUSD_COMBINED_HAS_PAYLOAD = 2,
  LIGHTUSD_COMBINED_HAS_INHERITS = 3,
  LIGHTUSD_COMBINED_HAS_VARIANTS = 4,
  LIGHTUSD_COMBINED_COMPOSE_SUBLAYERS = 5,
  LIGHTUSD_COMBINED_COMPOSE_REFERENCES = 6,
  LIGHTUSD_COMBINED_COMPOSE_PAYLOAD = 7,
  LIGHTUSD_COMBINED_COMPOSE_INHERITS = 8,
  LIGHTUSD_COMBINED_COMPOSE_VARIANTS = 9,
  LIGHTUSD_COMBINED_LOD_VARIANT_COUNT = 10
};
/* Returns the result as 0/1 or a count, or -1 for an invalid loader or op.
 * A failed compose pass leaves its message in the loader's error(). */
int32_t lightusd_combined_layer_op(void* loader, uint32_t op);
/* Selects `variant` in `variant_set` on `prim_path`. Returns 0/1, or -1 for
 * invalid arguments. */
int32_t lightusd_combined_apply_variant_selection(
    void* loader, const uint8_t* prim_path, uint32_t prim_path_size,
    const uint8_t* variant_set, uint32_t variant_set_size,
    const uint8_t* variant, uint32_t variant_size);
/* Selects `variant` on every variantSet of the pristine layer. */
int32_t lightusd_combined_apply_global_variant_selection(
    void* loader, const uint8_t* variant, uint32_t variant_size);

/* Layer string tables. kind 0/1/2 = sublayer/reference/payload asset paths.
 * kind 3 = variant info: for each prim with variant sets, strings are its
 * path then, per set, name, selection, and options; the shape holds the
 * prim's set count followed by each set's option count. Fills the retained
 * table, stores its shape length, and returns the string count or -1. */
enum {
  LIGHTUSD_COMBINED_SUBLAYER_ASSET_PATHS = 0,
  LIGHTUSD_COMBINED_REFERENCE_ASSET_PATHS = 1,
  LIGHTUSD_COMBINED_PAYLOAD_ASSET_PATHS = 2,
  LIGHTUSD_COMBINED_VARIANT_INFO = 3
};
int32_t lightusd_combined_layer_strings(void* loader, uint32_t kind,
                                       uint32_t* shape_size_out);
/* Copy one table string; returns the required byte count or -1. */
int32_t lightusd_combined_table_string(uint32_t index, uint8_t* out,
                                      uint32_t cap);
/* Copies the table shape; returns 0 or -1 when `count` does not match. */
int32_t lightusd_combined_table_shape(uint32_t* out, uint32_t count);
void lightusd_combined_table_release(void);
/* Streaming operations share counted byte keys and input chunks. Return a
 * boolean/count/pointer as a double, or -1 for invalid arguments. Size inputs
 * must be finite nonnegative integers exactly representable by JS and size_t. */
enum {
  LIGHTUSD_COMBINED_STREAM_START = 0,
  LIGHTUSD_COMBINED_STREAM_APPEND = 1,
  LIGHTUSD_COMBINED_STREAM_FINALIZE = 2,
  LIGHTUSD_COMBINED_STREAM_COMPLETE = 3,
  LIGHTUSD_COMBINED_ZERO_PTR = 4,
  LIGHTUSD_COMBINED_ZERO_PTR_OFFSET = 5,
  LIGHTUSD_COMBINED_ZERO_MARK = 6,
  LIGHTUSD_COMBINED_ZERO_FINALIZE = 7,
  LIGHTUSD_COMBINED_ZERO_CANCEL = 8,
  LIGHTUSD_COMBINED_MMAP_SET = 9,
  LIGHTUSD_COMBINED_MMAP_GET = 10,
  LIGHTUSD_COMBINED_ZERO_KEYS = 11,
  LIGHTUSD_COMBINED_STREAM_SIZE_MAX = 12
};
double lightusd_combined_stream_op(void* loader, uint32_t op,
    const uint8_t* key, uint32_t key_size, const uint8_t* data,
    uint32_t data_size, double value);
/* kind 0=stream progress, 1=zero-copy progress, 2=allocate zero-copy buffer.
 * Returns 1 when found/allocated, 0 when absent/allocation rejected, -1 on
 * invalid arguments. Successful calls publish UUID and asset name as table
 * strings 0 and 1; rejected allocations publish the error as string 0.
 * ZERO_KEYS publishes sorted UUIDs in the same table and returns its count.
 * Copy the table before another table-producing call; release it afterward.
 * flags: bit 0=complete, bit 1=finalized. progress is percentage for stream
 * progress, and a fraction rounded through float for zero-copy progress.
 * buffer_ptr is borrowed until finalization/cancellation/loader destruction;
 * reacquire the JS heap view after any WASM allocation. */
typedef struct lightusd_combined_stream_info {
  uint32_t struct_size;
  uint32_t flags;
  double total;
  double current;
  double progress;
  double buffer_ptr;
} lightusd_combined_stream_info;
int32_t lightusd_combined_stream_info_get(void* loader, uint32_t kind,
    const uint8_t* key, uint32_t key_size, double size, double max_bytes,
    lightusd_combined_stream_info* out);

/* Exact size_t variants used by JS. Input low/high words retain the entire
 * memory64 unsigned range, including sizes above JS's exact Number range.
 * stream_size_op accepts STREAM_START/ZERO_PTR_OFFSET/ZERO_MARK. */
double lightusd_combined_stream_size_op(void* loader, uint32_t op,
    const uint8_t* key, uint32_t key_size, uint32_t low, uint32_t high);
int32_t lightusd_combined_stream_allocate(void* loader,
    const uint8_t* key, uint32_t key_size,
    uint32_t size_low, uint32_t size_high, uint32_t max_low, uint32_t max_high,
    lightusd_combined_stream_info* out);

/* Asset cache/resolver calls. Scalar operations return boolean/count/size or
 * zero for void operations; -1 indicates invalid arguments. Keys and data
 * are counted bytes, not NUL-terminated strings. */
enum {
  LIGHTUSD_COMBINED_ASSET_SET = 0,
  LIGHTUSD_COMBINED_ASSET_HAS = 1,
  LIGHTUSD_COMBINED_ASSET_DELETE = 2,
  LIGHTUSD_COMBINED_ASSET_DELETE_UUID = 3,
  LIGHTUSD_COMBINED_ASSET_DELETE_NAME = 4,
  LIGHTUSD_COMBINED_ASSET_COUNT = 5,
  LIGHTUSD_COMBINED_ASSET_CACHE_SIZE = 6,
  LIGHTUSD_COMBINED_ASSET_CACHE_LIMIT_SET = 7,
  LIGHTUSD_COMBINED_ASSET_CACHE_LIMIT_GET = 8,
  LIGHTUSD_COMBINED_ASSET_EXISTS = 9,
  LIGHTUSD_COMBINED_ASSET_CLEAR = 10,
  LIGHTUSD_COMBINED_ASSET_PARENT_PATHS_SET = 11,
  LIGHTUSD_COMBINED_ASSET_PARENT_PATHS_GET = 12,
  LIGHTUSD_COMBINED_ASSET_BASE_PATH_SET = 13,
  LIGHTUSD_COMBINED_ASSET_SEARCH_PATHS_CLEAR = 14,
  LIGHTUSD_COMBINED_ASSET_SEARCH_PATH_ADD = 15,
  LIGHTUSD_COMBINED_ASSET_VERIFY_HASH = 16
};
double lightusd_combined_asset_op(void* loader, uint32_t op,
    const uint8_t* key, uint32_t key_size, const uint8_t* data,
    uint32_t data_size, double value);
/* Exact size_t operations: ASSET_COUNT/CACHE_SIZE/CACHE_LIMIT_GET require
 * out[2] (low/high uint32 words); CACHE_LIMIT_SET takes the input words and
 * permits a null out. Returns 0 or -1. JS preserves BigInt results on memory64. */
int32_t lightusd_combined_asset_size_op(void* loader, uint32_t op,
    uint32_t low, uint32_t high, uint32_t* out);
/* Return 1 if an existing entry was overwritten, 0 if newly inserted or
 * rejected, -1 for invalid arguments. Input is copied into cache ownership;
 * null, empty, >1 GiB or out-of-heap spans are rejected without dereferencing. */
int32_t lightusd_combined_asset_set_raw(void* loader,
    const uint8_t* key, uint32_t key_size, const uint8_t* data,
    uint32_t size_low, uint32_t size_high);
/* Asset bytes are borrowed until cache mutation or loader destruction.
 * Reacquire the JS heap view after native allocation. Copying getters must
 * copy the bytes into JS ownership before returning them. Success publishes
 * name/hash/UUID as table strings 0/1/2; by_uuid=1 misses publish error text
 * as string 0. Return 1=found, 0=missing, -1=invalid arguments. */
typedef struct lightusd_combined_asset_info {
  uint32_t struct_size;
  uint32_t reserved;
  double size;
  double data_ptr;
} lightusd_combined_asset_info;
int32_t lightusd_combined_asset_info_get(void* loader, uint32_t by_uuid,
    const uint8_t* key, uint32_t key_size, lightusd_combined_asset_info* out);
/* Publishes a retained string table and returns its string count or -1.
 * Single-value queries publish one string (empty on missing key).
 * UUIDS publishes name/UUID pairs in cache name order. */
enum {
  LIGHTUSD_COMBINED_ASSET_HASH = 0,
  LIGHTUSD_COMBINED_ASSET_UUID = 1,
  LIGHTUSD_COMBINED_ASSET_STREAM_UUID = 2,
  LIGHTUSD_COMBINED_ASSET_FIND_UUID = 3,
  LIGHTUSD_COMBINED_ASSET_BASE_PATH = 4,
  LIGHTUSD_COMBINED_ASSET_SEARCH_PATHS = 5,
  LIGHTUSD_COMBINED_ASSET_UUIDS = 6
};
int32_t lightusd_combined_asset_strings(void* loader, uint32_t kind,
    const uint8_t* key, uint32_t key_size);

/* Loading/diagnostic operations. Inputs are three counted byte strings;
 * unused arguments must be empty. Returns 0/1 for boolean operations, 0 for
 * void/string operations, or -1 for invalid arguments. String results publish
 * table string 0. VALIDATE_BINARY takes bytes, filename, options JSON;
 * LOAD_TEST takes filename, bytes; other loads take bytes then filename. */
enum {
  LIGHTUSD_COMBINED_LOAD_BINARY = 0,
  LIGHTUSD_COMBINED_LOAD_LAYER = 1,
  LIGHTUSD_COMBINED_LOAD_PROGRESS = 2,
  LIGHTUSD_COMBINED_LOAD_LAYER_PROGRESS = 3,
  LIGHTUSD_COMBINED_LOAD_CACHED = 4,
  LIGHTUSD_COMBINED_LOAD_LAYER_CACHED = 5,
  LIGHTUSD_COMBINED_LOAD_JSON = 6,
  LIGHTUSD_COMBINED_LOAD_TEST = 7,
  LIGHTUSD_COMBINED_LOAD_CANCEL = 8,
  LIGHTUSD_COMBINED_LOAD_WAS_CANCELLED = 9,
  LIGHTUSD_COMBINED_LOAD_IN_PROGRESS = 10,
  LIGHTUSD_COMBINED_LOAD_RESET_PROGRESS = 11,
  LIGHTUSD_COMBINED_LOAD_RELEASE_SOURCE = 12,
  LIGHTUSD_COMBINED_LOAD_RESET = 13,
  LIGHTUSD_COMBINED_LOAD_OK = 14,
  LIGHTUSD_COMBINED_LOAD_ERROR = 15,
  LIGHTUSD_COMBINED_LOAD_WARNING = 16,
  LIGHTUSD_COMBINED_LOAD_VALIDATE_BINARY = 17,
  LIGHTUSD_COMBINED_LOAD_VALIDATE_LAYER = 18
};
int32_t lightusd_combined_loading_op(void* loader, uint32_t op,
    const uint8_t* a, uint32_t a_size, const uint8_t* b, uint32_t b_size,
    const uint8_t* c, uint32_t c_size);
/* flags bit 0=cancel requested. Publishes stage, operation, error, current
 * mesh name and Tydra stage as strings 0..4. Return 0 or -1. */
typedef struct lightusd_combined_loading_progress {
  uint32_t struct_size;
  uint32_t flags;
  double progress;
  double percentage;
  double bytes_processed;
  double total_bytes;
  double meshes_processed;
  double meshes_total;
  double materials_processed;
  double materials_total;
} lightusd_combined_loading_progress;
int32_t lightusd_combined_loading_progress_get(void* loader,
    lightusd_combined_loading_progress* out);
/* Value memory probe. Returns record count or -1. The retained table contains
 * names; shape contains each size_t byte count as low/high uint32 words.
 * Rejects negative counts and variable-array sizes over the load memory limit. */
int32_t lightusd_combined_loading_memory_probe(void* loader, int32_t array_length);

/* Cooperative async load: begin owns input bytes and returns a positive task
 * ID (0 means rejected). Keep the loader alive through end; each step advances
 * one phase, returning 1=pending, 2=success, 0=pipeline error, -1=invalid call.
 * Error results publish table string 0. End releases the task, whether pending
 * or finished. IDs are never reused in one loader. */
typedef struct lightusd_combined_async_load_info {
  uint32_t struct_size;
  uint32_t mesh_count;
  uint32_t material_count;
  uint32_t texture_count;
} lightusd_combined_async_load_info;
uint32_t lightusd_combined_loading_async_begin(void* loader,
    const uint8_t* data, uint32_t size, const uint8_t* filename, uint32_t filename_size);
int32_t lightusd_combined_loading_async_step(void* loader, uint32_t task,
    lightusd_combined_async_load_info* out);
int32_t lightusd_combined_loading_async_end(void* loader, uint32_t task);

/* Layer/scalar export operations. 0=layer text, 1=layer JSON, 2=layer JSON
 * with embed_buffers + array_mode, 3=USDA, 4=flatten, 5=render conversion.
 * Text operations publish table string 0 and return 0; boolean operations
 * return 0/1. Invalid inputs return -1. */
int32_t lightusd_combined_export_op(void* loader, uint32_t op,
    uint32_t embed_buffers, const uint8_t* array_mode, uint32_t array_mode_size);
typedef struct lightusd_combined_export_info {
  uint32_t struct_size;
  uint32_t reserved;
  double size;
  double data_ptr;
} lightusd_combined_export_info;
/* On success returns an owning byte-result handle and fills the record. The
 * byte view survives all loader operations, including destruction. Release
 * exactly once with export_release after copying/consuming bytes. Null means
 * invalid inputs or export failure (read loader.error for pipeline failures).
 * as_layer=0 exports the reconstructed stage; 1 writes the current layer. */
void* lightusd_combined_export_usdc(void* loader, uint32_t as_layer,
    lightusd_combined_export_info* out);
void lightusd_combined_export_release(void* result);

/* Layer readiness check (sets the legacy "No layer loaded" error on failure).
 * Returns 1/0, or -1 for a null loader. Used before inspecting JS output objects. */
int32_t lightusd_combined_export_layer_ready(void* loader);
/* Serialize for a caller-supplied JS output buffer. kind: 0=valid byteLength,
 * 1=null/undefined, 2=invalid byteLength. Capacity is truncated to size_t after
 * range validation. Returns an owning result or null (loader.error describes
 * pipeline/buffer failures). Finish after copying, then release the result. */
void* lightusd_combined_export_usdc_buffer(void* loader, uint32_t as_layer,
    uint32_t buffer_kind, double capacity, lightusd_combined_export_info* out);
/* Publishes the completed copy's warning as table string 0 and updates loader
 * warning state. Do not call when the JS copy threw. Returns 0 or -1. */
int32_t lightusd_combined_export_buffer_finish(void* loader, void* result);

/* Optimization fields in order: material atlas size/tile size/padding/min group,
 * or geometry max input faces/max input points/max aggregate faces/min group.
 * present is a four-bit mask; absent fields use native defaults. */
typedef struct lightusd_combined_export_optimization {
  uint32_t struct_size;
  uint32_t present;
  int32_t values[4];
} lightusd_combined_export_optimization;
int32_t lightusd_combined_export_optimize(void* loader, uint32_t kind,
    const uint8_t* mode, uint32_t mode_size,
    const lightusd_combined_export_optimization* options);
/* Package preparation owns its stage snapshot. Keep the loader alive until end.
 * For layer exports, readiness is checked without snapshotting the authored
 * layer. Begin returns null on pipeline failure; optimize returns 1/0, -1 invalid.
 * Maps use the counted-u32 pair encoding documented for the flatten boundary.
 * Write returns 1/0, -1 invalid and exposes a borrowed loader-owned USDZ view.
 * The next package write or loader destruction invalidates that view. */
void* lightusd_combined_package_begin(void* loader, uint32_t as_layer);
int32_t lightusd_combined_package_write(void* loader, void* package,
    const uint8_t* remap, uint32_t remap_size,
    const uint8_t* root_format, uint32_t root_format_size, uint32_t arkit,
    lightusd_combined_export_info* out);
void lightusd_combined_package_end(void* package);
/* Returns remapped path count; -1 means no layer, -2 invalid arguments/encoding. */
int32_t lightusd_combined_export_remap(void* loader, const uint8_t* remap, uint32_t size);

/* Render scalar queries. Keys: 0 meshes, 1 instances, 2 materials, 3 textures,
 * 4 images, 5 lights, 6 cameras, 7 UDIM textures, 8 root nodes, 9 animations,
 * 10 skeletons, 11 default root id, 12 URI, 13 up axis. Strings publish table
 * string 0. Returns 0, or -1 for invalid inputs; numeric values include -1 IDs. */
int32_t lightusd_combined_render_scalar(void* loader, uint32_t key, double* out);
/* Inspection JSON queries: 0 MetaHuman profile, 1 authored shading graph.
 * Publishes JSON in table string 0, copied with table_string and released with
 * table_release. Empty/missing results retain the loader's existing error
 * behavior. Returns 0, or -1 for null loader/unknown query. */
int32_t lightusd_combined_render_json(void* loader, uint32_t query);
/* Mesh operations: 0 publishes authored primvar JSON in table string 0 and
 * returns 0; 1 computes deferred tangents and returns 0/1. Negative return
 * means null loader or unknown operation. Invalid mesh IDs retain each
 * operation's existing JSON error/false result. Tangent computation mutates
 * the mesh and invalidates its cached borrowed arrays. */
int32_t lightusd_combined_mesh_operation(void* loader, uint32_t operation, int32_t mesh_id);
typedef struct lightusd_combined_bone_texture_info {
  uint32_t struct_size, width, height, texels_per_vertex;
  uint32_t max_influences, vertex_count, element_size, reserved;
  uint64_t result, texture_data, texture_count, vertex_offsets, offset_count;
} lightusd_combined_bone_texture_info;
/* Returns 1 with an owning result handle and float32 spans, 0 with error text
 * in table string 0, or -1 for invalid arguments/allocation failure. The result
 * survives loader mutation/destruction; release exactly once with end (NULL
 * is allowed). Span addresses are valid until end. JS copies both arrays. */
int32_t lightusd_combined_bone_texture_begin(void* loader, int32_t mesh_id,
    int32_t max_influences, lightusd_combined_bone_texture_info* out);
void lightusd_combined_bone_texture_end(void* result);
typedef struct lightusd_combined_mesh_pointer_info {
  uint32_t struct_size, flags; /* subsets=1, single index=2, triangles=4, display color=8, double sided=16 */
  int32_t material_id;
  uint32_t reserved;
  uint64_t vertex_count;
  uint32_t attribute_count, submesh_count;
  double display_color[3];
  uint64_t result;
} lightusd_combined_mesh_pointer_info;
typedef struct lightusd_combined_mesh_attribute {
  uint32_t struct_size, key, slot, dtype, components, reserved;
  uint64_t address, count;
} lightusd_combined_mesh_attribute;
typedef struct lightusd_combined_mesh_submesh {
  uint32_t struct_size;
  int32_t start, count, material_id;
} lightusd_combined_mesh_submesh;
/* Mesh descriptor result: begin returns 1 found, 0 missing, -1 invalid/OOM.
 * Strings are primName/displayName/absPath in the shared string table.
 * Attribute keys: points=0, indices=1, face counts=2, normals=3, UV slot=4,
 * vertex colors=5, colors=6, opacity=7, owned JS float4 tangents=8.
 * Dtypes: f32=0, u32=1, snorm8=2, snorm16=3, u8=4, i8=5.
 * Attribute/submesh return 1 found, 0 past end, -1 invalid record/index.
 * Result metadata is owned; spans borrow loader/decoded mesh caches. Keep the
 * loader alive and unchanged until all reads/copies finish, then release the
 * result exactly once. Returned pointer descriptors retain this borrowed
 * lifetime after end; tangent data must be copied before any scene mutation.
 */
int32_t lightusd_combined_mesh_pointer_begin(void* loader, int32_t mesh_id,
    lightusd_combined_mesh_pointer_info* out);
int32_t lightusd_combined_mesh_pointer_attribute(void* result, int32_t index,
    lightusd_combined_mesh_attribute* out);
int32_t lightusd_combined_mesh_pointer_submesh(void* result, int32_t index,
    lightusd_combined_mesh_submesh* out);
void lightusd_combined_mesh_pointer_end(void* result);

typedef struct lightusd_combined_mesh_value_info {
  uint32_t struct_size, flags; /* double sided=1, area light=2, normalize=4,
                               geom bind authored=8, display color=16, subsets=32 */
  int32_t material_id, element_size, skeleton_id;
  uint32_t attribute_count, submesh_count, reserved;
  double display_color[3], light_intensity, light_exposure;
  uint64_t result;
} lightusd_combined_mesh_value_info;
/* Same result ownership/read/end functions as mesh_pointer_begin. Extra keys:
 * 9 texcoords, 10 packed tangents, 11 light color, 12 joint indices,
 * 13 joint weights, 14 geom bind matrix. Extra dtypes: i32=6, f64=7.
 * Strings: primName/displayName/absPath/lightMaterialSyncMode.
 * Copy accessor copies every span; deprecated getMesh exposes heap views.
 */
int32_t lightusd_combined_mesh_value_begin(void* loader, int32_t mesh_id,
    lightusd_combined_mesh_value_info* out);
/* Marks the native once-per-loader deprecation warning; 1 first, 0 seen, -1 null. */
int32_t lightusd_combined_mesh_warn(void* loader);

/* Encode 8-bit pixels. Returns -1 invalid C input, 0 encoding/format failure
 * (loader error), 1 borrowed output, 2 invalid dimensions (diagnostics unchanged).
 * Output bytes borrow the loader's image-export buffer until the next successful
 * encode, reset or destruction. This record is not an owning export handle. */
int32_t lightusd_combined_encode_image(void* loader, const uint8_t* pixels, uint32_t pixel_size,
    int32_t width, int32_t height, int32_t channels, const uint8_t* format, uint32_t format_size,
    lightusd_combined_export_info* out);

/* Schema operations: 0 physics JSON (string table slot 0), 1 sample scene,
 * 2 clear uploaded meshes, 3 URDF JSON scene. Only operation 3 accepts data.
 * Returns -1 invalid C input, otherwise boolean success. Copy/release the
 * string table after operation 0. Operations mutate loader state/diagnostics. */
int32_t lightusd_combined_schema_operation(void* loader, uint32_t operation,
    const uint8_t* data, uint32_t size);
/* Copies caller-owned arrays into the shared visual/collision mesh registry.
 * Counts are scalar elements, at most 2^28 each. Null is allowed only for an
 * empty span. Returns -1 invalid C input, 0 invalid mesh (loader error), 1 stored.
 * Typed input pointers require their natural alignment. */
int32_t lightusd_combined_schema_mesh(void* loader, const uint8_t* name, uint32_t name_size,
    const float* positions, uint32_t position_count, const float* normals, uint32_t normal_count,
    const float* uvs, uint32_t uv_count, const int32_t* indices, uint32_t index_count);

typedef struct lightusd_combined_material_info {
  uint32_t struct_size, flags; /* MaterialX authored=1, PreviewSurface=2, specular workflow=4 */
  uint32_t texture_mask, reserved;
  int32_t texture_ids[13];
  uint32_t padding;
  double values[21];
} lightusd_combined_material_info;
/* Returns -1 invalid input, 0 error text, 1 serialized data/format, 2 legacy
 * material. Error/serialized strings start at table slot 0. Legacy slots:
 * error/version/namespace/colorspace/sourceUri. The legacy numeric record has
 * diffuse/emissive/specular/normal RGB at values[0..11], followed by metallic,
 * roughness, clearcoat, clearcoatRoughness, opacity, opacityThreshold, ior,
 * displacement, occlusion. Texture bits/IDs follow that public property order:
 * diffuse, emissive, specular, metallic, roughness, clearcoat,
 * clearcoatRoughness, opacity, opacityThreshold, ior, normal, displacement,
 * occlusion. Copy/release the string table before another producer call. */
int32_t lightusd_combined_material_get(void* loader, int32_t id,
    const uint8_t* format, uint32_t size, lightusd_combined_material_info* out);

typedef struct lightusd_combined_camera_info {
  uint32_t struct_size;
  uint32_t reserved;
  double focal_length, vertical_aperture, horizontal_aperture;
  double znear, zfar, yfov, xfov, aspect_ratio;
} lightusd_combined_camera_info;
/* Returns 1 with name/path/display/projection strings 0..3; 0 with error
 * string 0 for unloaded/invalid ID; -1 for invalid C arguments/record. */
int32_t lightusd_combined_camera_get(void* loader, int32_t id, lightusd_combined_camera_info* out);
typedef struct lightusd_combined_scene_metadata {
  uint32_t struct_size;
  uint32_t flags; /* bit 0 autoPlay, bit 1 start time present, bit 2 end present */
  double meters_per_unit, kilograms_per_unit, frames_per_second, time_codes_per_second;
  double start_time, end_time;
  double working_to_display[9];
} lightusd_combined_scene_metadata;
/* Returns 1 with copyright/comment/upAxis/renderSettingsPrimPath/workingColorSpace
 * as strings 0..4; 0 unloaded; -1 invalid C arguments/record. */
int32_t lightusd_combined_metadata_get(void* loader, lightusd_combined_scene_metadata* out);
typedef struct lightusd_combined_texture_info {
  uint32_t struct_size;
  uint32_t flags; /* bit 0 transform present, bit 1 UDIM */
  int32_t image_id, udim_texture_id;
  double rotation, scale_u, scale_v, translation_u, translation_v;
  double bias[4], scale[4];
  double udim_scale_u, udim_scale_v, udim_offset_u, udim_offset_v;
} lightusd_combined_texture_info;
/* Returns 1 with wrapS/wrapT strings 0..1; 0 unloaded/missing; -1 invalid C record. */
int32_t lightusd_combined_texture_get(void* loader, int32_t id, lightusd_combined_texture_info* out);

typedef struct lightusd_combined_instance_info {
  uint32_t struct_size;
  uint32_t visible;
  int32_t prototype_index, mesh_id, material_id;
  uint32_t reserved;
  double local_matrix[16], global_matrix[16];
} lightusd_combined_instance_info;
/* Returns 1 with primName/absPath/displayName in table strings 0..2, 0 for
 * missing instance, -1 for invalid arguments. Matrices are row-major. */
int32_t lightusd_combined_instance_get(void* loader, int32_t id,
    lightusd_combined_instance_info* out);
/* Publishes matching instance indices, in scene order, through table_shape.
 * Returns count, or -1 for null loader/unrepresentable count. Negative mesh
 * IDs are matched literally. Copy the table before the next table-producing
 * query and release it with table_release. */
int32_t lightusd_combined_instances_for_mesh(void* loader, int32_t mesh_id);

typedef struct lightusd_combined_node_info {
  uint32_t struct_size;
  uint32_t flags; /* bit 0 reset transform, bit 1 instance */
  int32_t content_id, prototype_index, instance_id;
  uint32_t reserved;
  uint64_t child_count;
  double local_matrix[16], global_matrix[16];
} lightusd_combined_node_info;
/* Borrowed hierarchy cursor. Keep loader alive and do not change/reset its
 * render scene until end. Begin returns 1 with cursor, 0 for a missing root,
 * -1 invalid arguments, -2 allocation failure. use_default must be 0 or 1;
 * when 1, root_id is ignored. Always end each successfully opened cursor. */
int32_t lightusd_combined_nodes_begin(void* loader, int32_t root_id,
    uint32_t use_default, void** cursor);
/* Preorder traversal, preserving child order. Returns 1 with a record and
 * primName/displayName/absPath/category/type in table strings 0..4; 0 at end;
 * -1 invalid arguments. Invalid records do not advance the cursor. Copy each
 * table before the next table-producing call and release it. */
int32_t lightusd_combined_nodes_next(void* cursor, lightusd_combined_node_info* out);
void lightusd_combined_nodes_end(void* cursor);

/* Sparse UDIM metadata. Returns 1 with four table strings (primName, absPath,
 * displayName, assetIdentifier) and tile_count rows of four uint32 words in
 * table_shape: udim, u, v, imageId (signed int32 bit patterns). Returns 0 for
 * unloaded/missing texture, -1 for invalid arguments or unrepresentable table.
 * Copy both tables before another table-producing query, then table_release. */
int32_t lightusd_combined_udim_get(void* loader, int32_t id, uint32_t* tile_count);
/* Publishes unresolved image asset identifiers in image order, preserving
 * duplicates. Returns string count, or -1 for null loader/oversized result. */
int32_t lightusd_combined_unresolved_textures(void* loader);

typedef struct lightusd_combined_image_info {
  uint32_t struct_size;
  uint32_t flags; /* decoded=1, transform valid=2, applied=4, bypass=8,
                    source data=16, buffer present=32 */
  int32_t width, height, channels, buffer_id;
  uint64_t data, byte_length;
  double source_gamma, source_linear_bias, source_to_display[9];
} lightusd_combined_image_info;
/* Returns 1 with metadata and table strings URI/colorSpace/usdColorSpace/
 * sourceColorSpaceName, 0 missing/unloaded, -1 invalid arguments/record.
 * load_buffer=1 preserves getImageCopy's lazy USDZ materialization; 0 only
 * inspects metadata. data is borrowed until loader reset/reload/destruction.
 * Heap growth invalidates JS views but not the returned byte offset. */
int32_t lightusd_combined_image_get(void* loader, int32_t id,
    uint32_t load_buffer, lightusd_combined_image_info* out);
/* Marks the legacy getImage warning as emitted. Returns 1 on first use, 0 on
 * later uses (including after reset), -1 for null loader. Emits no JS calls. */
int32_t lightusd_combined_image_warn(void* loader);

typedef struct lightusd_combined_light_info {
  uint32_t struct_size;
  uint32_t flags; /* normalize=1, color temperature=2, IES normalize=4,
                    shadow enable=8, spectral emission=16 */
  int32_t envmap_texture_id, geometry_mesh_id;
  uint64_t spectral_samples, spectral_count; /* borrowed packed float32 pairs */
  double color[3];
  double intensity;
  double exposure;
  double diffuse;
  double specular;
  double colorTemperature;
  double transform[16];
  double position[3];
  double direction[3];
  double radius;
  double width;
  double height;
  double length;
  double angle;
  double shapingConeAngle;
  double shapingConeSoftness;
  double shapingFocus;
  double shapingFocusTint[3];
  double shapingIesAngleScale;
  double shadowColor[3];
  double shadowDistance;
  double shadowFalloff;
  double shadowFalloffGamma;
  double guideRadius;
} lightusd_combined_light_info;
/* Returns 1 with table strings name/path/display/type/texture/IES/dome format/
 * material sync, followed by spectral interpolation/unit/preset when present.
 * Returns 0 with error string 0 for unloaded/missing light, -1 invalid record.
 * Spectral bytes remain borrowed until scene mutation or loader destruction. */
int32_t lightusd_combined_light_get(void* loader, int32_t id, lightusd_combined_light_info* out);
/* Returns 0 when unloaded; otherwise light count; -1 for null loader. */
int32_t lightusd_combined_lights_count(void* loader);
/* Returns 1 with serialized data/format in table strings 0/1, 0 with error in
 * string 0, -1 invalid C arguments. Format is counted UTF-8 (json or xml). */
int32_t lightusd_combined_light_format(void* loader, int32_t id,
    const uint8_t* format, uint32_t size);

typedef struct lightusd_combined_skeleton_info {
  uint32_t struct_size;
  int32_t anim_id;
  uint64_t cursor;
} lightusd_combined_skeleton_info;
typedef struct lightusd_combined_joint_info {
  uint32_t struct_size;
  int32_t joint_id;
  uint64_t child_count;
  double bind_transform[16], rest_transform[16];
} lightusd_combined_joint_info;
/* Returns 1 with cursor and prim_name/abs_path/display_name in table strings
 * 0..2, 0 with error string 0 for unloaded/invalid ID, -1 invalid record,
 * -2 cursor allocation failure. Keep loader alive and its scene unchanged
 * until skeleton_end. Copy metadata before advancing the cursor. */
int32_t lightusd_combined_skeleton_begin(void* loader, int32_t id,
    lightusd_combined_skeleton_info* out);
/* Preorder joints, including the existing synthetic root when present.
 * Returns 1 with joint_path/joint_name in table strings 0/1, 0 finished,
 * -1 invalid record (does not advance). Matrices are row-major doubles. */
int32_t lightusd_combined_skeleton_next(void* cursor, lightusd_combined_joint_info* out);
void lightusd_combined_skeleton_end(void* cursor);

typedef struct lightusd_combined_animation_info {
  uint32_t struct_size, flags; /* value clip=1, baked=2 */
  uint32_t channel_count, sampler_count, target_node_count;
  int32_t animated_joints, animated_nodes;
  uint32_t asset_count;
  double duration, clip_start, clip_end, clip_sample_rate;
} lightusd_combined_animation_info;
typedef struct lightusd_combined_sampler_info {
  uint32_t struct_size, interpolation; /* LINEAR=0, STEP=1, CUBICSPLINE=2 */
  uint64_t times, time_count, values, value_count; /* borrowed float32 spans */
} lightusd_combined_sampler_info;
typedef struct lightusd_combined_channel_info {
  uint32_t struct_size, flags; /* valid=1, scene node present=2, custom=4, skeletal=8 */
  int32_t sampler, target_node, skeleton_id, joint_id;
  uint32_t path, reserved; /* translation=0, rotation=1, scale=2, weights=3,
                            custom property=4, unknown=5 */
} lightusd_combined_channel_info;
/* All record queries return 1 found, 0 unloaded/missing, -1 invalid arguments,
 * record or unrepresentable counts. Animation strings: name/primName/absPath/
 * displayName/sourceType followed by asset paths. Channel strings: property
 * name, node name, track base path (last two empty without a scene node).
 * Copy/release string tables before another table-producing call. Sampler
 * spans remain borrowed until scene mutation or loader destruction. */
int32_t lightusd_combined_animation_get(void* loader, int32_t id, lightusd_combined_animation_info* out);
int32_t lightusd_combined_animation_sampler(void* loader, int32_t id, int32_t sampler, lightusd_combined_sampler_info* out);
int32_t lightusd_combined_animation_channel(void* loader, int32_t id, int32_t channel, lightusd_combined_channel_info* out);
int32_t lightusd_combined_animations_count(void* loader);

/* MCP context and JSON operations. Inputs are counted UTF-8 strings. Context
 * operations return 0/1; JSON operations return 0 and publish table string 0.
 * Invalid loader/op/null-nonempty inputs return -1. Read/copy the result before
 * another table-producing call, then release the table. */
enum {
  LIGHTUSD_COMBINED_MCP_CREATE_CONTEXT = 0,
  LIGHTUSD_COMBINED_MCP_SELECT_CONTEXT = 1,
  LIGHTUSD_COMBINED_MCP_TOOLS_LIST = 2,
  LIGHTUSD_COMBINED_MCP_TOOLS_CALL = 3,
  LIGHTUSD_COMBINED_MCP_RESOURCES_LIST = 4,
  LIGHTUSD_COMBINED_MCP_RESOURCES_READ = 5
};
int32_t lightusd_combined_mcp_op(void* loader, uint32_t op,
    const uint8_t* a, uint32_t a_size, const uint8_t* b, uint32_t b_size);

/* Loader configuration keys. Values cross as doubles holding the setting's
 * exact int32, uint32, float, double, or 0/1 value. */
enum {
  LIGHTUSD_COMBINED_CONFIG_COMBINE_UDIM_TILES = 0,          /* bool */
  LIGHTUSD_COMBINED_CONFIG_DEFER_TANGENT_COMPUTATION = 1,   /* bool */
  LIGHTUSD_COMBINED_CONFIG_ENABLE_BONE_REDUCTION = 2,       /* bool */
  LIGHTUSD_COMBINED_CONFIG_ENABLE_VALUE_CLIPS = 3,          /* bool */
  LIGHTUSD_COMBINED_CONFIG_MAX_MEMORY_LIMIT_MB = 4,         /* int32 */
  LIGHTUSD_COMBINED_CONFIG_ROUND_BONE_COUNT = 5,            /* bool */
  LIGHTUSD_COMBINED_CONFIG_SPHERE_SUBDIVISIONS = 6,         /* int32, 0-6 */
  LIGHTUSD_COMBINED_CONFIG_TARGET_BONE_COUNT = 7,           /* uint32, 1-128 */
  LIGHTUSD_COMBINED_CONFIG_VALUE_CLIP_SAMPLE_RATE = 8,      /* float */
  LIGHTUSD_COMBINED_CONFIG_VALUE_CLIP_USE_TIME_RANGE = 9,   /* bool */
  LIGHTUSD_COMBINED_CONFIG_VALUE_CLIP_START_TIME = 10,      /* get only */
  LIGHTUSD_COMBINED_CONFIG_VALUE_CLIP_END_TIME = 11,        /* get only */
  LIGHTUSD_COMBINED_CONFIG_VALUE_CLIP_TIME_RANGE = 12,      /* set only: a, b */
  LIGHTUSD_COMBINED_CONFIG_ENABLE_COMPOSITION = 13,         /* set only */
  LIGHTUSD_COMBINED_CONFIG_LOAD_TEXTURE_IN_NATIVE = 14,     /* set only */
  LIGHTUSD_COMBINED_CONFIG_NATIVE_FLATTEN_RENDER_TREE = 15, /* bool */
  LIGHTUSD_COMBINED_CONFIG_NATIVE_MATERIAL_DEDUP = 16,      /* bool */
  LIGHTUSD_COMBINED_CONFIG_NATIVE_MESH_MERGE = 17,          /* bool */
  LIGHTUSD_COMBINED_CONFIG_NATIVE_MESH_MERGE_BAKE_TRANSFORM = 18, /* bool */
  LIGHTUSD_COMBINED_CONFIG_USDC_EXPORT_LIMIT_MB = 19        /* set only: int32 a, b */
};
/* Applies one setting; `b` is used only by the two-value keys. Out-of-range
 * values the native setter would ignore are ignored. Returns 0, or -1 for an
 * invalid loader, key, or a value outside the key's integer type. */
int32_t lightusd_combined_config_set(void* loader, uint32_t key, double a,
                                     double b);
/* Reads one setting into `out`. Returns 0 or -1. */
int32_t lightusd_combined_config_get(void* loader, uint32_t key, double* out);

typedef struct lightusd_combined_memory_stats {
  uint32_t struct_size;
  uint32_t reserved;
  double num_meshes;
  double num_materials;
  double num_textures;
  double num_images;
  double num_buffers;
  double num_nodes;
  double num_lights;
  double buffer_memory_bytes;
  double asset_cache_count;
  double asset_cache_size_bytes;
  double asset_cache_max_bytes;
  double reordered_mesh_cache_count;
} lightusd_combined_memory_stats;
int32_t lightusd_combined_memory_stats_get(void* loader,
                                           lightusd_combined_memory_stats* out);
/* Reports a manual debug event and returns the wasm heap byte length, or -1
 * for an invalid loader or label. */
double lightusd_combined_debug_log_memory(void* loader, const uint8_t* label,
                                          uint32_t label_size);
uint8_t* lightusd_combined_alloc(uint32_t size);
void lightusd_combined_free(uint8_t* ptr);

#ifdef __cplusplus
}

#include <string>
#include <vector>
namespace lightusd::next::pipeline {
struct FlattenStats;
}
namespace lightusd::web::combined {
// Publishes a callback-driven flatten result into the retained copy buffers
// and fills `out`. Everything is written here, after the pipeline returns, so
// a JS callback that re-enters another flatten cannot clobber a running one.
// `message` is the error for status -1/0 and the layer key for status 2;
// `stats` is required for status 3 and ignored otherwise.
void StoreFlattenResult(int32_t status, std::string&& message,
                        std::vector<uint8_t>&& data,
                        lightusd::next::pipeline::FlattenStats* stats,
                        lightusd_combined_flatten_step_info* out);
// Replaces the retained string table read by lightusd_combined_table_*.
void StoreStringTable(std::vector<std::string>&& strings,
                      std::vector<uint32_t>&& shape);
bool EmitFlattenChunk(uint32_t sink_id, const uint8_t* data, size_t size);
bool FlattenLayerExists(uint32_t exists_id, const std::string& key);
// Copies the bytes returned by the JS fetch callback; false when it returned
// no bytes, an empty buffer, or more than 1 GiB.
bool FetchFlattenLayer(uint32_t fetch_id, const std::string& key,
                       std::string* out);
int32_t NextFlattenOwned(std::string&& input, uint8_t lazy_arrays,
                         lightusd_combined_flatten_info* out);
int32_t NextFlattenOwnedWithMaps(
    std::string&& input, uint8_t lazy_arrays,
    const std::map<std::string, std::string>& remap,
    const std::map<std::string, std::string>& variants,
    lightusd_combined_flatten_info* out);
}
#endif
#endif
