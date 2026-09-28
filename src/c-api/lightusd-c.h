/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * LightUSD C API over the "next" core (lightusd::next).
 *
 * Design:
 * - C11, no exceptions cross the boundary (the core is built -fno-exceptions).
 * - Every fallible function returns lightusd_status; details via the thread-local
 *   lightusd_last_error(). Results go through out-params.
 * - Owning opaque handles (lightusd_stage, lightusd_value, lightusd_string, lightusd_strlist)
 *   release each owning reference with their lightusd_*_destroy function.
 *   Stage references can be shared with lightusd_stage_retain().
 * - lightusd_prim is a small BY-VALUE handle (no allocation, nothing to destroy).
 *   It stays valid until its stage is destroyed or structurally mutated
 *   (define/remove prim); lightusd_stage_generation() lets bindings detect that.
 * - Borrowed views (lightusd_sv, lightusd_value_view) point into stage-owned storage:
 *   valid until the stage is destroyed or the owning prim/property is mutated.
 *
 * Threading:
 * - Distinct handles are fully independent.
 * - Concurrent READS of one stage are safe (internal lazy-decode is serialized
 *   by a private mutex).
 * - A WRITE (any set / define / remove / add function taking a mutable
 *   lightusd_stage pointer) must not run concurrently with reads of that stage.
 * - lightusd_last_error() is thread-local.
 */

#ifndef LIGHTUSD_C_H_
#define LIGHTUSD_C_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LIGHTUSD_API
#if defined(_WIN32) && defined(LIGHTUSD_C_BUILD_SHARED)
#define LIGHTUSD_API __declspec(dllexport)
#elif defined(_WIN32) && defined(LIGHTUSD_C_USE_SHARED)
#define LIGHTUSD_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define LIGHTUSD_API __attribute__((visibility("default")))
#else
#define LIGHTUSD_API
#endif
#endif

#define LIGHTUSD_API_VERSION_MAJOR 4
#define LIGHTUSD_API_VERSION_MINOR 0
#define LIGHTUSD_API_VERSION_PATCH 0

/* ============================================================
 * Status / error handling
 * ============================================================ */

typedef enum lightusd_status {
  LIGHTUSD_OK = 0,
  LIGHTUSD_ERR_INVALID_ARG = -1,
  LIGHTUSD_ERR_IO = -2,
  LIGHTUSD_ERR_PARSE = -3,
  LIGHTUSD_ERR_NOT_FOUND = -4,
  LIGHTUSD_ERR_TYPE_MISMATCH = -5,
  LIGHTUSD_ERR_OUT_OF_MEMORY = -6,
  LIGHTUSD_ERR_UNSUPPORTED = -7,
  LIGHTUSD_ERR_COMPOSITION = -8,
  LIGHTUSD_ERR_RESOURCE_LIMIT = -9,
  LIGHTUSD_ERR_OVERFLOW = -10,
  LIGHTUSD_ERR_BUSY = -11,
  LIGHTUSD_ERR_STALE_REVISION = -12,
  LIGHTUSD_ERR_CANCELLED = -13,
  LIGHTUSD_ERR_INTERNAL = -99
} lightusd_status;

/* (major << 16) | (minor << 8) | patch */
LIGHTUSD_API uint32_t lightusd_api_version(void);
LIGHTUSD_API const char* lightusd_version_string(void);

/* Thread-local message for the most recent failing call on this thread.
 * Never NULL (empty string when no error). Valid until the next failing
 * call on the same thread. */
LIGHTUSD_API const char* lightusd_last_error(void);

/* ============================================================
 * Strings
 * ============================================================ */

/* Borrowed string view. `data` is NUL-terminated for stage-owned strings but
 * always carry `len` (export_usdc reuses lightusd_string as a byte buffer). */
typedef struct lightusd_sv {
  const char* data;
  size_t len;
} lightusd_sv;

/* Copy a borrowed view into caller-owned storage. `required` receives the
 * exact byte count (excluding any NUL terminator). Pass out=NULL/cap=0 to
 * query the required size. Source and destination may overlap. A short
 * destination returns LIGHTUSD_ERR_INVALID_ARG after writing `required`.
 */
LIGHTUSD_API lightusd_status lightusd_sv_copy(lightusd_sv view, char* out,
                                               size_t cap, size_t* required);

/* Owned string / byte buffer. */
typedef struct lightusd_string lightusd_string;
LIGHTUSD_API lightusd_sv lightusd_string_view(const lightusd_string* s);
LIGHTUSD_API void lightusd_string_destroy(lightusd_string* s);

/* Owned list of strings. */
typedef struct lightusd_strlist lightusd_strlist;
LIGHTUSD_API size_t lightusd_strlist_size(const lightusd_strlist* l);
LIGHTUSD_API lightusd_sv lightusd_strlist_get(const lightusd_strlist* l, size_t index);
LIGHTUSD_API void lightusd_strlist_destroy(lightusd_strlist* l);

/* ============================================================
 * Types (mirrors lightusd::next::TypeId numerically)
 * ============================================================ */

typedef uint16_t lightusd_type;

enum {
  LIGHTUSD_TYPE_INVALID = 0,
  LIGHTUSD_TYPE_BOOL = 1,
  LIGHTUSD_TYPE_INT = 2,
  LIGHTUSD_TYPE_UINT = 3,
  LIGHTUSD_TYPE_INT64 = 4,
  LIGHTUSD_TYPE_UINT64 = 5,
  LIGHTUSD_TYPE_HALF = 6,
  LIGHTUSD_TYPE_FLOAT = 7,
  LIGHTUSD_TYPE_DOUBLE = 8,
  LIGHTUSD_TYPE_STRING = 9,
  LIGHTUSD_TYPE_TOKEN = 10,
  LIGHTUSD_TYPE_ASSET_PATH = 11,
  LIGHTUSD_TYPE_INT2 = 12,
  LIGHTUSD_TYPE_INT3 = 13,
  LIGHTUSD_TYPE_INT4 = 14,
  LIGHTUSD_TYPE_UINT2 = 15,
  LIGHTUSD_TYPE_UINT3 = 16,
  LIGHTUSD_TYPE_UINT4 = 17,
  LIGHTUSD_TYPE_HALF2 = 18,
  LIGHTUSD_TYPE_HALF3 = 19,
  LIGHTUSD_TYPE_HALF4 = 20,
  LIGHTUSD_TYPE_FLOAT2 = 21,
  LIGHTUSD_TYPE_FLOAT3 = 22,
  LIGHTUSD_TYPE_FLOAT4 = 23,
  LIGHTUSD_TYPE_DOUBLE2 = 24,
  LIGHTUSD_TYPE_DOUBLE3 = 25,
  LIGHTUSD_TYPE_DOUBLE4 = 26,
  LIGHTUSD_TYPE_QUATH = 27,
  LIGHTUSD_TYPE_QUATF = 28,
  LIGHTUSD_TYPE_QUATD = 29,
  LIGHTUSD_TYPE_POINT3H = 30,
  LIGHTUSD_TYPE_POINT3F = 31,
  LIGHTUSD_TYPE_POINT3D = 32,
  LIGHTUSD_TYPE_VECTOR3H = 33,
  LIGHTUSD_TYPE_VECTOR3F = 34,
  LIGHTUSD_TYPE_VECTOR3D = 35,
  LIGHTUSD_TYPE_NORMAL3H = 36,
  LIGHTUSD_TYPE_NORMAL3F = 37,
  LIGHTUSD_TYPE_NORMAL3D = 38,
  LIGHTUSD_TYPE_COLOR3H = 39,
  LIGHTUSD_TYPE_COLOR3F = 40,
  LIGHTUSD_TYPE_COLOR3D = 41,
  LIGHTUSD_TYPE_COLOR4H = 42,
  LIGHTUSD_TYPE_COLOR4F = 43,
  LIGHTUSD_TYPE_COLOR4D = 44,
  LIGHTUSD_TYPE_MATRIX2F = 45,
  LIGHTUSD_TYPE_MATRIX2D = 46,
  LIGHTUSD_TYPE_MATRIX3F = 47,
  LIGHTUSD_TYPE_MATRIX3D = 48,
  LIGHTUSD_TYPE_MATRIX4F = 49,
  LIGHTUSD_TYPE_MATRIX4D = 50,
  LIGHTUSD_TYPE_TEXCOORD2H = 51,
  LIGHTUSD_TYPE_TEXCOORD2F = 52,
  LIGHTUSD_TYPE_TEXCOORD2D = 53,
  LIGHTUSD_TYPE_TEXCOORD3H = 54,
  LIGHTUSD_TYPE_TEXCOORD3F = 55,
  LIGHTUSD_TYPE_TEXCOORD3D = 56,
  LIGHTUSD_TYPE_TIMECODE = 57,
  LIGHTUSD_TYPE_EXTENT = 58,
  LIGHTUSD_TYPE_DICTIONARY = 59
};

/* Storage component type of a lightusd_value_view / buffer. Half-element data is
 * materialized as float32 by the core, so LIGHTUSD_COMP_FLOAT16 never appears in
 * views today (kept for ABI completeness). */
typedef enum lightusd_component_type {
  LIGHTUSD_COMP_NONE = 0, /* no POD buffer (string-family / dictionary / block) */
  LIGHTUSD_COMP_UINT8 = 1,
  LIGHTUSD_COMP_INT32 = 2,
  LIGHTUSD_COMP_UINT32 = 3,
  LIGHTUSD_COMP_INT64 = 4,
  LIGHTUSD_COMP_UINT64 = 5,
  LIGHTUSD_COMP_FLOAT16 = 6,
  LIGHTUSD_COMP_FLOAT32 = 7,
  LIGHTUSD_COMP_FLOAT64 = 8,
  LIGHTUSD_COMP_UINT16 = 9,
  LIGHTUSD_COMP_INT16 = 10
} lightusd_component_type;

LIGHTUSD_API const char* lightusd_type_name(lightusd_type t);
LIGHTUSD_API lightusd_type lightusd_type_from_name(const char* name);
LIGHTUSD_API size_t lightusd_type_size(lightusd_type t);          /* bytes per element */
LIGHTUSD_API size_t lightusd_type_component_count(lightusd_type t);

/* ============================================================
 * Value views
 * ============================================================ */

/* Borrowed, zero-copy view of a value.
 * - POD scalar / vector / matrix / array data: `data` points into stage-owned
 *   storage, tightly packed; `count` is 1 for scalars, the array length for
 *   arrays; `components` scalars per element of `storage` component type.
 * - String / token / asset-path scalars, token arrays, and dictionaries have
 *   data == NULL, storage == LIGHTUSD_COMP_NONE: fetch through
 *   lightusd_attr_get_string / lightusd_attr_get_token_array / dict cursor instead.
 */
typedef struct lightusd_value_view {
  lightusd_type type;
  uint8_t is_array;
  uint8_t is_block; /* authored `= None` */
  uint8_t storage;  /* lightusd_component_type of `data` */
  uint8_t components;
  size_t count;
  const void* data;
  size_t nbytes;
} lightusd_value_view;

/* Owned value (results of interpolation / eval / metadata queries). */
typedef struct lightusd_value lightusd_value;
LIGHTUSD_API void lightusd_value_destroy(lightusd_value* v);
/* View into the owned value; lifetime = the lightusd_value's lifetime. */
LIGHTUSD_API lightusd_status lightusd_value_get_view(const lightusd_value* v,
                                         lightusd_value_view* out);
LIGHTUSD_API lightusd_status lightusd_value_get_string(const lightusd_value* v, lightusd_sv* out);
LIGHTUSD_API lightusd_status lightusd_value_get_token_array(const lightusd_value* v,
                                                lightusd_strlist** out);
/* Format an owned value using the USDA value syntax. The returned string is
 * owned by the caller, including for scalar and array values. */
LIGHTUSD_API lightusd_status lightusd_value_to_usda(const lightusd_value* v,
                                         lightusd_string** out);

/* Borrowed dictionary cursor (customData / assetInfo / customLayerData). */
typedef struct lightusd_dict_ref {
  const void* _dict; /* const next::Dict* */
} lightusd_dict_ref;

LIGHTUSD_API int lightusd_dict_is_valid(lightusd_dict_ref d);
LIGHTUSD_API size_t lightusd_dict_size(lightusd_dict_ref d);
/* Fetch entry i. Any out-param may be NULL. If the entry is itself a
 * dictionary, *subdict becomes valid and *val reports LIGHTUSD_TYPE_DICTIONARY.
 * String-family entry values are returned via *sval. */
LIGHTUSD_API lightusd_status lightusd_dict_entry(lightusd_dict_ref d, size_t index,
                                     lightusd_sv* key, lightusd_value_view* val,
                                     lightusd_sv* sval, lightusd_dict_ref* subdict);
LIGHTUSD_API lightusd_status lightusd_dict_find(lightusd_dict_ref d, const char* key,
                                    lightusd_value_view* val, lightusd_sv* sval,
                                    lightusd_dict_ref* subdict);
/* Copy a string-family array entry into an owned list. Failure clears *out. */
LIGHTUSD_API lightusd_status lightusd_dict_get_token_array(
    lightusd_dict_ref d, const char* key, lightusd_strlist** out);

/* ============================================================
 * Stage: load / create / save
 * ============================================================ */

typedef struct lightusd_stage lightusd_stage;

typedef enum lightusd_format {
  LIGHTUSD_FORMAT_AUTO = 0,
  LIGHTUSD_FORMAT_USDA = 1,
  LIGHTUSD_FORMAT_USDC = 2,
  LIGHTUSD_FORMAT_USDZ = 3
} lightusd_format;

typedef enum lightusd_input_policy {
  LIGHTUSD_INPUT_UNTRUSTED = 0,
  LIGHTUSD_INPUT_TRUSTED = 1
} lightusd_input_policy;

#define LIGHTUSD_LIMIT_UNLIMITED UINT64_MAX

/* Optional synchronous parser progress callback. Return zero to cancel. */
typedef int (*lightusd_load_progress_fn)(void* userdata, const char* phase,
                                         size_t current, size_t total);

typedef struct lightusd_load_options {
  uint32_t struct_size; /* = sizeof(lightusd_load_options); enables ABI growth */
  uint32_t format;      /* lightusd_format; AUTO sniffs extension + content */
  uint32_t input_policy; /* lightusd_input_policy */
  int32_t max_threads;   /* 0 = bounded auto; 1 = serial; >1 = fixed */
  uint64_t max_input_bytes;
  uint64_t max_asset_bytes;
  uint64_t max_resident_bytes;
  uint64_t max_array_elements;
  uint64_t max_archive_entries;
  uint32_t max_parse_depth;
  uint32_t max_composition_depth;
  uint32_t max_namespace_depth;
  uint8_t composed;     /* 1: resolve composition arcs (LoadUSDComposed) */
  uint8_t load_payloads;
  /* USDA array parsing policy */
  uint8_t enable_usda_lazy_arrays; /* 1: enable lazy USDA array materialization */
  /* 1: keep native instance prototypes; 0: flatten to self-contained holders
   * for export/save round-trips (the default). */
  uint8_t preserve_native_instances;
  uint8_t _pad[4];
  /* Variant selection overrides (set name -> variant name), applied on every
   * prim defining that set; stronger than authored selections. Parallel
   * arrays of length variant_override_count. */
  const char* const* variant_sets;
  const char* const* variant_names;
  size_t variant_override_count;
  lightusd_load_progress_fn progress_callback;
  void* progress_userdata;
} lightusd_load_options;

LIGHTUSD_API void lightusd_load_options_init(lightusd_load_options* opts);

LIGHTUSD_API lightusd_status lightusd_stage_load(const char* filename,
                                     const lightusd_load_options* opts, /* nullable */
                                     lightusd_stage** out);
/* Single-layer load from memory (no composition: no anchor for externals). */
LIGHTUSD_API lightusd_status lightusd_stage_load_from_memory(const uint8_t* data,
                                                 size_t size,
                                                 const lightusd_load_options* opts,
                                                 lightusd_stage** out);
/* Empty stage ready for authoring. */
LIGHTUSD_API lightusd_status lightusd_stage_create(lightusd_stage** out);
/* Retain/release one owning reference. Prim handles and views remain borrowed;
 * retain the stage while storing them, and release each retained reference. */
LIGHTUSD_API void lightusd_stage_retain(lightusd_stage* stage);
LIGHTUSD_API void lightusd_stage_destroy(lightusd_stage* stage);
/* True for immutable document snapshot views. NULL reports false. */
LIGHTUSD_API int lightusd_stage_is_read_only(const lightusd_stage* stage);

/* Drain warnings accumulated by load (empty string when none). */
LIGHTUSD_API lightusd_status lightusd_stage_take_warnings(lightusd_stage* stage,
                                              lightusd_string** out);

/* Bumped by every structural mutation (define/remove prim); lets bindings
 * detect stale lightusd_prim handles cheaply. */
LIGHTUSD_API uint64_t lightusd_stage_generation(const lightusd_stage* stage);

typedef struct lightusd_save_options {
  uint32_t struct_size;
  uint32_t format; /* lightusd_format; AUTO derives from the file extension */
} lightusd_save_options;

LIGHTUSD_API void lightusd_save_options_init(lightusd_save_options* opts);

LIGHTUSD_API lightusd_status lightusd_stage_save(const lightusd_stage* stage,
                                     const char* filename,
                                     const lightusd_save_options* opts /* nullable */);
LIGHTUSD_API lightusd_status lightusd_stage_save_usdz_with_assets(
    const lightusd_stage* stage, const char* filename,
    const char* const* asset_names, const uint8_t* const* asset_data,
    const size_t* asset_sizes, size_t asset_count);
LIGHTUSD_API lightusd_status lightusd_stage_export_usda(const lightusd_stage* stage,
                                            lightusd_string** out);
/* Crate bytes; use lightusd_string_view() for (data, len). */
LIGHTUSD_API lightusd_status lightusd_stage_export_usdc(const lightusd_stage* stage,
                                            lightusd_string** out);
/* AOUSD Core validation of the stage's root layer. Validation findings are
 * reported as counts; a successfully checked but invalid layer still returns
 * LIGHTUSD_OK. */
LIGHTUSD_API lightusd_status lightusd_stage_validate_core(
    const lightusd_stage* stage, size_t* errors, size_t* warnings);
/* Flatten composition into a new single-layer stage. */
LIGHTUSD_API lightusd_status lightusd_stage_flatten(const lightusd_stage* stage,
                                        lightusd_stage** out);
/* Explain composed property resolution as an owned JSON document. `time` may
 * be NaN to request the default time; otherwise it is a numeric time code. */
LIGHTUSD_API lightusd_status lightusd_stage_explain_property(
    const lightusd_stage* stage, const char* prim_path, const char* property,
    double time, lightusd_string** out);

/* Resolver-backed dependency inventory. Memory assets and aliases support
 * virtual asset namespaces; without registrations the resolver checks files.
 * An alias asserts that its target resolves, so register readable target bytes
 * (or use a file target) before collecting dependencies.
 * Mutations must not run concurrently with dependency collection. The report
 * is owned JSON; `complete` is zero when dependencies are missing or rejected
 * by the bounded scan and is independent of the call status. */
typedef struct lightusd_asset_resolver lightusd_asset_resolver;
LIGHTUSD_API lightusd_status lightusd_asset_resolver_create(
    lightusd_asset_resolver** out);
LIGHTUSD_API void lightusd_asset_resolver_destroy(lightusd_asset_resolver* resolver);
LIGHTUSD_API lightusd_status lightusd_asset_resolver_register_memory(
    lightusd_asset_resolver* resolver, const char* identifier,
    const uint8_t* data, size_t size);
/* Remove a previously registered memory asset. Returns NOT_FOUND when the
 * identifier is not present; aliases to it remain registered and will fail to
 * read until the target is registered again. */
LIGHTUSD_API lightusd_status lightusd_asset_resolver_unregister_memory(
    lightusd_asset_resolver* resolver, const char* identifier);
/* Memory assets are unlimited by default. A nonzero limit bounds the sum of
 * registered payload bytes. Registration beyond the limit fails atomically;
 * changing to a limit below current usage is rejected without mutation. */
LIGHTUSD_API lightusd_status lightusd_asset_resolver_set_memory_limit(
    lightusd_asset_resolver* resolver, size_t limit_bytes);
LIGHTUSD_API lightusd_status lightusd_asset_resolver_get_memory_stats(
    const lightusd_asset_resolver* resolver, size_t* asset_count,
    size_t* bytes_used, size_t* limit_bytes);
/* Copy the identifier at a sorted index. The required UTF-8 byte count
 * excludes a trailing NUL. A null output performs a size query; a short
 * output returns RESOURCE_LIMIT and leaves the output untouched. */
LIGHTUSD_API lightusd_status lightusd_asset_resolver_memory_identifier(
    const lightusd_asset_resolver* resolver, size_t index, char* out,
    size_t capacity, size_t* required_size);
LIGHTUSD_API lightusd_status lightusd_asset_resolver_set_alias(
    lightusd_asset_resolver* resolver, const char* authored_path,
    const char* resolved_identifier);
LIGHTUSD_API lightusd_status lightusd_asset_resolver_read(
    const lightusd_asset_resolver* resolver, const char* resolved_identifier,
    lightusd_string** out);
LIGHTUSD_API lightusd_status lightusd_dependency_report_json(
    lightusd_asset_resolver* resolver, const char* root,
    lightusd_string** out, uint8_t* complete);
/* Low-memory file->file flatten pipeline (lazy arrays passed through). */
LIGHTUSD_API lightusd_status lightusd_flatten_file_to_usdc(const char* in_filename,
                                               const char* out_filename,
                                               const lightusd_load_options* opts);

/* ============================================================
 * Stage: metadata & stats
 * ============================================================ */

/* Key-based stage metadata. Supported keys:
 *   "defaultPrim" (token), "upAxis" (token), "metersPerUnit" (double),
 *   "timeCodesPerSecond" (double), "startTimeCode" (double),
 *   "endTimeCode" (double), "framesPerSecond" (double),
 *   "kilogramsPerUnit" (double), "doc" (string), "comment" (string),
 *   "colorConfiguration" (asset), "colorManagementSystem" (token)
 * Get returns LIGHTUSD_ERR_NOT_FOUND for unknown keys. */
/* True when a supported stage metadata key has an authored opinion in this
 * view, including an explicit default/empty value. Unknown keys/null return 0. */
LIGHTUSD_API int lightusd_stage_metadata_is_authored(const lightusd_stage* stage, const char* key);
LIGHTUSD_API lightusd_status lightusd_stage_get_metadata(const lightusd_stage* stage,
                                             const char* key,
                                             lightusd_value** out);
LIGHTUSD_API lightusd_status lightusd_stage_set_metadata(lightusd_stage* stage, const char* key,
                                             lightusd_type type, const void* data,
                                             size_t count);

LIGHTUSD_API lightusd_sv lightusd_stage_default_prim_path(const lightusd_stage* stage);
LIGHTUSD_API lightusd_status lightusd_stage_set_default_prim(lightusd_stage* stage,
                                                 const char* prim_name);
LIGHTUSD_API lightusd_status lightusd_stage_sublayers(const lightusd_stage* stage,
                                          lightusd_strlist** out);
LIGHTUSD_API lightusd_status lightusd_stage_add_sublayer_path(lightusd_stage* stage,
                                                  const char* asset_path);
LIGHTUSD_API lightusd_status lightusd_stage_custom_layer_data(const lightusd_stage* stage,
                                                  lightusd_dict_ref* out);

typedef struct lightusd_stage_stats {
  uint64_t prim_count;
  uint64_t layer_count;
  uint64_t total_properties;
  uint64_t memory_bytes;
} lightusd_stage_stats;

LIGHTUSD_API lightusd_status lightusd_stage_get_stats(const lightusd_stage* stage,
                                          lightusd_stage_stats* out);
LIGHTUSD_API double lightusd_stage_start_timecode(const lightusd_stage* stage);
LIGHTUSD_API double lightusd_stage_end_timecode(const lightusd_stage* stage);

/* ============================================================
 * Prim access & traversal
 * ============================================================ */

/* Borrowed, generation-checked prim handle. No allocation, no destroy.
 * The owner must remain alive. Structural mutation invalidates the handle;
 * access then returns invalid/empty or LIGHTUSD_ERR_INVALID_ARG.
 * Never touch the members directly. */
typedef struct lightusd_prim {
  const lightusd_stage* _owner;
  const void* _layer;
  uint64_t _generation;
  uint32_t _index;
  uint32_t _pad;
} lightusd_prim;

LIGHTUSD_API int lightusd_prim_is_valid(lightusd_prim p);

LIGHTUSD_API lightusd_prim lightusd_stage_pseudo_root(const lightusd_stage* stage);
LIGHTUSD_API lightusd_prim lightusd_stage_prim_at_path(const lightusd_stage* stage,
                                           const char* path);
LIGHTUSD_API lightusd_prim lightusd_stage_default_prim(const lightusd_stage* stage);
LIGHTUSD_API size_t lightusd_stage_root_prim_count(const lightusd_stage* stage);
LIGHTUSD_API lightusd_prim lightusd_stage_root_prim(const lightusd_stage* stage, size_t index);
LIGHTUSD_API size_t lightusd_stage_prim_count(const lightusd_stage* stage);

LIGHTUSD_API lightusd_sv lightusd_prim_name(lightusd_prim p);
LIGHTUSD_API lightusd_sv lightusd_prim_type_name(lightusd_prim p);
LIGHTUSD_API lightusd_sv lightusd_prim_path(lightusd_prim p);
/* Opaque identity for root-layer geometry resource accounting. Equal nonzero
 * values identify handles backed by the same root-layer prim spec within the
 * same stage generation. Zero means the prim is not backed by that layer (or
 * the handle is invalid). Do not persist across stages or structural edits. */
LIGHTUSD_API uint64_t lightusd_prim_root_layer_resource_id(lightusd_prim p);
/* 0 = def, 1 = over, 2 = class */
LIGHTUSD_API uint8_t lightusd_prim_specifier(lightusd_prim p);
LIGHTUSD_API int lightusd_prim_is_active(lightusd_prim p);
/* Whether this prim has authored payload arcs in the current stage view.
 * Resolved composition may consume these arcs. Invalid handles return zero. */
LIGHTUSD_API int lightusd_prim_has_payload(lightusd_prim p);

LIGHTUSD_API lightusd_prim lightusd_prim_parent(lightusd_prim p);
LIGHTUSD_API size_t lightusd_prim_child_count(lightusd_prim p);
LIGHTUSD_API lightusd_prim lightusd_prim_child(lightusd_prim p, size_t index);
LIGHTUSD_API lightusd_prim lightusd_prim_child_by_name(lightusd_prim p, const char* name);

/* ============================================================
 * Properties / attributes (read)
 * ============================================================ */

/* Property flags (mirror next::PropSlot flag bits). */
enum {
  LIGHTUSD_PROP_CUSTOM = 0x0001,
  LIGHTUSD_PROP_UNIFORM = 0x0002,
  LIGHTUSD_PROP_TIMESAMPLED = 0x0004,
  LIGHTUSD_PROP_CONNECTION = 0x0008,
  LIGHTUSD_PROP_RELATIONSHIP = 0x0010,
  LIGHTUSD_PROP_ARRAY = 0x0020
};

LIGHTUSD_API size_t lightusd_prim_property_count(lightusd_prim p);
/* Resolved names include inherited and schema-defined properties, in USD
 * property order. Owned list; failure clears out. Indexed property_count/name
 * remain authored-slot queries. */
LIGHTUSD_API lightusd_status lightusd_prim_property_names(lightusd_prim p, lightusd_strlist** out);
/* Inspect an authored/inherited/schema-fallback default without time sampling
 * or following connections. Blocks/absent defaults return NOT_FOUND. Arrays
 * report type/count only (data NULL), avoiding lazy array materialization.
 * Scalar POD data and optional string-family text are borrowed from the stage
 * or schema registry. Failure clears both outputs. */
LIGHTUSD_API lightusd_status lightusd_attr_inspect_default(
    lightusd_prim p, const char* name, lightusd_value_view* out, lightusd_sv* text);
/* Reports whether obtaining a packed array buffer requires lazy materialization
 * or decompression. Arrays already stored in directly borrowable form return 0. */
LIGHTUSD_API lightusd_status lightusd_attr_would_materialize_array(
    lightusd_prim p, const char* name, int* out);
/* Owned resolved default, with the same schema/instance fallback as inspect.
 * Materializes lazy arrays into the result without changing the stage. No time
 * sampling or connection following. Blocks/absent defaults return NOT_FOUND;
 * failed lazy decoding returns INTERNAL. Failure clears out. */
LIGHTUSD_API lightusd_status lightusd_attr_copy_default(
    lightusd_prim p, const char* name, lightusd_value** out);
LIGHTUSD_API lightusd_sv lightusd_prim_property_name(lightusd_prim p, size_t index);
LIGHTUSD_API uint16_t lightusd_prim_property_flags_at(lightusd_prim p, size_t index);
LIGHTUSD_API int lightusd_prim_has_property(lightusd_prim p, const char* name);
LIGHTUSD_API uint16_t lightusd_prim_property_flags(lightusd_prim p, const char* name);
/* Declared USD type name (e.g. "color3f", "float[]"); empty if unrecorded. */
LIGHTUSD_API lightusd_sv lightusd_prim_property_type_name(lightusd_prim p, const char* name);

/* Zero-copy default-value view (materializes lazy crate arrays in place,
 * thread-safely). LIGHTUSD_ERR_NOT_FOUND if the property or its default value is
 * absent. */
LIGHTUSD_API lightusd_status lightusd_attr_get(lightusd_prim p, const char* name,
                                   lightusd_value_view* out);
/* String / token / asset-path scalar defaults. */
LIGHTUSD_API lightusd_status lightusd_attr_get_string(lightusd_prim p, const char* name,
                                          lightusd_sv* out);
/* Token/string array defaults (owned list, one call). */
LIGHTUSD_API lightusd_status lightusd_attr_get_token_array(lightusd_prim p, const char* name,
                                               lightusd_strlist** out);

/* Property metadata by key. Supported keys: "interpolation", "elementSize",
 * "colorSpace", "displayName", "displayGroup", "doc", "hidden", "renderType",
 * "connectability", "bindMaterialAs", "weight", "customData" (dictionary).
 * LIGHTUSD_ERR_NOT_FOUND when the key was not authored. */
LIGHTUSD_API lightusd_status lightusd_attr_metadata(lightusd_prim p, const char* name,
                                        const char* key, lightusd_value** out);
LIGHTUSD_API lightusd_status lightusd_attr_custom_data(lightusd_prim p, const char* name,
                                           lightusd_dict_ref* out);

LIGHTUSD_API size_t lightusd_attr_connection_count(lightusd_prim p, const char* name);
LIGHTUSD_API lightusd_sv lightusd_attr_connection(lightusd_prim p, const char* name,
                                      size_t index);

/* Composition-time evaluation (follows connections, samples time). */
LIGHTUSD_API lightusd_status lightusd_attr_eval(const lightusd_stage* stage, lightusd_prim p,
                                    const char* name, double time,
                                    lightusd_value** out);
/* Extended evaluation: NaN requests authored default-time evaluation;
 * interp_mode is 0=held or 1=linear; follow_connections is boolean. */
LIGHTUSD_API lightusd_status lightusd_attr_eval_ex(
    const lightusd_stage* stage, lightusd_prim p, const char* name, double time,
    uint8_t interp_mode, uint8_t follow_connections, lightusd_value** out);

/* Evaluated BackPlateAPI instance. The owning result is independent of stage
 * lifetime; string views in info remain valid until the result is destroyed.
 * Evaluation requires finite numeric time and uses the shared schema
 * implementation without loading images. Invalid arguments return INVALID_ARG;
 * an unapplied instance returns NOT_FOUND. Both query functions clear out on
 * failure. Destroy accepts NULL. */
typedef struct lightusd_backplate lightusd_backplate;
typedef struct lightusd_backplate_info {
  lightusd_sv image, alpha_image, depth_image, plate_visibility;
  float depth_min_offset, depth_normalizing_factor, depth_camera_space_offset;
  float scale_tweak[2], rotate_xyz_tweak[3], translate_tweak[3];
  float luma_gain[3], luma_lift[3], luma_gamma[3];
} lightusd_backplate_info;
LIGHTUSD_API lightusd_status lightusd_backplate_eval(
    const lightusd_stage* stage, lightusd_prim prim, const char* instance_name,
    double time, lightusd_backplate** out);
LIGHTUSD_API lightusd_status lightusd_backplate_get_info(
    const lightusd_backplate* plate, lightusd_backplate_info* out);
LIGHTUSD_API void lightusd_backplate_destroy(lightusd_backplate* plate);

/* Validate and sample a SkelAnimation prim at numeric time, returning its
 * authored joint count. This uses the same schema evaluation as native next. */
LIGHTUSD_API lightusd_status lightusd_skel_animation_joint_count_at_time(
    const lightusd_stage* stage, lightusd_prim p, double time, size_t* out);

/* Owned skeleton pose input. All array and name views borrow from the sample,
 * which remains valid after its source stage is released. An all-zero animation
 * handle requests the rest pose; an unresolved animation is also ignored. */
typedef struct lightusd_skel_sample lightusd_skel_sample;
typedef struct lightusd_skel_sample_info {
  size_t joint_count, animation_joint_count;
  const int32_t* parent_indices;
  const double* rest_transforms;
  size_t rest_transform_count;
  const double* bind_transforms;
  size_t bind_transform_count;
  const float* translations;
  size_t translation_count;
  const float* rotations;
  size_t rotation_count;
  const float* scales;
  size_t scale_count;
  uint8_t has_translations, has_rotations, has_scales;
} lightusd_skel_sample_info;
LIGHTUSD_API lightusd_status lightusd_skel_sample_create(
    const lightusd_stage* stage, lightusd_prim skeleton,
    lightusd_prim animation, double time, lightusd_skel_sample** out);
LIGHTUSD_API lightusd_status lightusd_skel_sample_get_info(
    const lightusd_skel_sample* sample, lightusd_skel_sample_info* out);
LIGHTUSD_API lightusd_status lightusd_skel_sample_joint_name(
    const lightusd_skel_sample* sample, size_t index, lightusd_sv* out);
LIGHTUSD_API lightusd_status lightusd_skel_sample_animation_joint_name(
    const lightusd_skel_sample* sample, size_t index, lightusd_sv* out);
LIGHTUSD_API void lightusd_skel_sample_destroy(lightusd_skel_sample* sample);

LIGHTUSD_API lightusd_status lightusd_prim_local_transform(lightusd_prim p, double time,
                                               double out16[16]);
LIGHTUSD_API lightusd_status lightusd_prim_world_transform(const lightusd_stage* stage,
                                               lightusd_prim p, double time,
                                               double out16[16]);

/* ============================================================
 * Time samples
 * ============================================================ */

LIGHTUSD_API int lightusd_attr_has_timesamples(lightusd_prim p, const char* name);
LIGHTUSD_API size_t lightusd_attr_timesample_count(lightusd_prim p, const char* name);
/* Copy up to `cap` sample times into out. Returns total count via return
 * value regardless of cap (call with cap=0, out=NULL to size). */
LIGHTUSD_API size_t lightusd_attr_timesample_times(lightusd_prim p, const char* name,
                                           double* out, size_t cap);
/* Zero-copy view of sample `index` (sorted by time). */
LIGHTUSD_API lightusd_status lightusd_attr_timesample_at(lightusd_prim p, const char* name,
                                             size_t index, double* time,
                                             lightusd_value_view* out);
/* Interpolated value at `time` (owned). interp_mode: 0=held, 1=linear. */
LIGHTUSD_API lightusd_status lightusd_attr_interpolate(lightusd_prim p, const char* name,
                                           double time, uint8_t interp_mode,
                                           lightusd_value** out);

/* ============================================================
 * Relationships
 * ============================================================ */

/* Includes inherited instance-source relationships, with local opinions taking
 * precedence. Targets are unforwarded authored paths. Name lists are owned;
 * target strings borrow the stage. Failure clears names output. */
LIGHTUSD_API size_t lightusd_prim_relationship_count(lightusd_prim p);
LIGHTUSD_API lightusd_status lightusd_prim_relationship_names(lightusd_prim p,
                                                  lightusd_strlist** out);
LIGHTUSD_API int lightusd_prim_has_relationship(lightusd_prim p, const char* name);
LIGHTUSD_API size_t lightusd_rel_target_count(lightusd_prim p, const char* name);
LIGHTUSD_API lightusd_sv lightusd_rel_target(lightusd_prim p, const char* name, size_t index);
/* Resolve an inherited UsdShade binding. Empty purpose uses the
 * preview-compatible fallback order. The result is an owned path, empty when
 * unbound. */
LIGHTUSD_API lightusd_status lightusd_prim_bound_material_path(
    const lightusd_stage* stage, lightusd_prim prim, const char* purpose,
    lightusd_string** out);
enum {
  LIGHTUSD_MATERIAL_SHADER_SURFACE = 0,
  LIGHTUSD_MATERIAL_SHADER_DISPLACEMENT = 1,
  LIGHTUSD_MATERIAL_SHADER_VOLUME = 2
};
LIGHTUSD_API lightusd_status lightusd_material_shader_path(
    const lightusd_stage* stage, lightusd_prim material, uint8_t kind,
    lightusd_string** out);
LIGHTUSD_API lightusd_status lightusd_shader_port_value(
    const lightusd_stage* stage, lightusd_prim shader, const char* input,
    double time, lightusd_value** out);

/* ============================================================
 * Variants
 * ============================================================ */

LIGHTUSD_API size_t lightusd_prim_variant_set_count(lightusd_prim p);
LIGHTUSD_API lightusd_sv lightusd_prim_variant_set_name(lightusd_prim p, size_t set_index);
LIGHTUSD_API size_t lightusd_variant_count(lightusd_prim p, const char* set_name);
LIGHTUSD_API lightusd_sv lightusd_variant_name(lightusd_prim p, const char* set_name,
                                   size_t index);
LIGHTUSD_API lightusd_sv lightusd_variant_selection(lightusd_prim p, const char* set_name);
/* Authored selection keys, including sets without local definitions. Owned list;
 * failure clears out. Use variant_selection to read each selected value. */
LIGHTUSD_API lightusd_status lightusd_prim_variant_selection_names(lightusd_prim p, lightusd_strlist** out);

/* ============================================================
 * Prim metadata
 * ============================================================ */

/* Key-based prim metadata. Supported keys: "active" (bool), "hidden" (bool),
 * "instanceable" (bool), "kind" (token), "doc" (string), "comment" (string),
 * "displayName" (string), "apiSchemas" (token[]).
 * LIGHTUSD_ERR_NOT_FOUND when unauthored / unknown. */
LIGHTUSD_API lightusd_status lightusd_prim_get_metadata(lightusd_prim p, const char* key,
                                            lightusd_value** out);
/* Reports whether a supported prim metadata key has a local authored opinion. */
LIGHTUSD_API lightusd_status lightusd_prim_metadata_is_authored(
    lightusd_prim p, const char* key, int* out_authored);
LIGHTUSD_API lightusd_status lightusd_prim_custom_data(lightusd_prim p, lightusd_dict_ref* out);
LIGHTUSD_API lightusd_status lightusd_prim_asset_info(lightusd_prim p, lightusd_dict_ref* out);
LIGHTUSD_API lightusd_sv lightusd_prim_kind(lightusd_prim p);
/* Prototype-root path for a native scenegraph instance; empty for the
 * prototype holder and non-instance prims. Borrowed from the owning stage. */
LIGHTUSD_API lightusd_sv lightusd_prim_instance_prototype_path(lightusd_prim p);
/* True when this prim authors value-clip dictionary metadata. */
LIGHTUSD_API int lightusd_prim_has_value_clips(lightusd_prim p);

/* ============================================================
 * Authoring (path-addressed; all take the stage write lock and, when
 * structural, bump the stage generation)
 * ============================================================ */

/* specifier: 0=def, 1=over, 2=class */
LIGHTUSD_API lightusd_status lightusd_stage_define_prim(lightusd_stage* stage, const char* path,
                                            const char* type_name, /* nullable */
                                            uint8_t specifier,
                                            lightusd_prim* out /* nullable */);
LIGHTUSD_API lightusd_status lightusd_stage_remove_prim(lightusd_stage* stage, const char* path);
/* Rename the prim at `path` (last component becomes `new_name`); descendants
 * move with it and sibling order is kept. Authored paths elsewhere
 * (relationship targets, connections, arcs) are not retargeted. Fails with
 * LIGHTUSD_ERR_INVALID_ARG for an invalid name or a sibling collision. */
LIGHTUSD_API lightusd_status lightusd_stage_rename_prim(lightusd_stage* stage, const char* path,
                                            const char* new_name);

/* Author a full typed value in ONE call.
 * - POD types: `data` points at `count` elements of `type` (count > 1 or
 *   is_array != 0 authors an array; count == 1 && !is_array a scalar).
 * - LIGHTUSD_TYPE_STRING/TOKEN/ASSET_PATH scalar: data = const char* (NUL-term).
 * `flags` = LIGHTUSD_PROP_* to OR onto the property (custom/uniform). */
LIGHTUSD_API lightusd_status lightusd_attr_set(lightusd_stage* stage, const char* prim_path,
                                   const char* name, lightusd_type type,
                                   uint8_t is_array, const void* data,
                                   size_t count, uint16_t flags);
LIGHTUSD_API lightusd_status lightusd_attr_set_token_array(lightusd_stage* stage,
                                               const char* prim_path,
                                               const char* name,
                                               lightusd_type type, /* TOKEN, STRING, or ASSET_PATH */
                                               const char* const* items,
                                               size_t count, uint16_t flags);
LIGHTUSD_API lightusd_status lightusd_attr_set_timesample(lightusd_stage* stage,
                                              const char* prim_path,
                                              const char* name, double time,
                                              lightusd_type type, uint8_t is_array,
                                              const void* data, size_t count);
/* Set property metadata by key (same keys as lightusd_attr_metadata; value
 * encoding as in lightusd_attr_set). */
LIGHTUSD_API lightusd_status lightusd_attr_set_metadata(lightusd_stage* stage,
                                            const char* prim_path,
                                            const char* name, const char* key,
                                            lightusd_type type, const void* data,
                                            size_t count);
LIGHTUSD_API lightusd_status lightusd_attr_add_connection(lightusd_stage* stage,
                                              const char* prim_path,
                                              const char* name,
                                              const char* target);
/* Author a value block (`= None`). */
LIGHTUSD_API lightusd_status lightusd_attr_block(lightusd_stage* stage, const char* prim_path,
                                     const char* name);
LIGHTUSD_API lightusd_status lightusd_attr_remove(lightusd_stage* stage, const char* prim_path,
                                      const char* name);

LIGHTUSD_API lightusd_status lightusd_rel_add_target(lightusd_stage* stage,
                                         const char* prim_path,
                                         const char* rel_name,
                                         const char* target);
LIGHTUSD_API lightusd_status lightusd_rel_set_targets(lightusd_stage* stage,
                                          const char* prim_path,
                                          const char* rel_name,
                                          const char* const* targets,
                                          size_t count);
LIGHTUSD_API lightusd_status lightusd_rel_remove(lightusd_stage* stage, const char* prim_path,
                                     const char* rel_name);

/* Composition arcs. arc_type: 0=reference, 1=payload, 2=inherit,
 * 3=specialize. `asset_path` may be NULL/empty for internal arcs;
 * `target_prim_path` may be NULL for default-prim targeting. */
enum {
  LIGHTUSD_ARC_REFERENCE = 0,
  LIGHTUSD_ARC_PAYLOAD = 1,
  LIGHTUSD_ARC_INHERIT = 2,
  LIGHTUSD_ARC_SPECIALIZE = 3
};

/* Authored arcs remaining in this stage view; composition may consume them.
 * Invalid prims or unknown arc_type return zero. */
LIGHTUSD_API size_t lightusd_prim_arc_count(lightusd_prim p, uint8_t arc_type);
/* Borrowed arc text as stored in this view (for example @asset@</Prim>).
 * Intended for inspection; not a resolved asset path. Invalid/out-of-range is empty. */
LIGHTUSD_API lightusd_sv lightusd_prim_arc_text(lightusd_prim p, uint8_t arc_type, size_t index);

LIGHTUSD_API lightusd_status lightusd_prim_add_arc(lightusd_stage* stage, const char* prim_path,
                                       uint8_t arc_type,
                                       const char* asset_path,      /* nullable */
                                       const char* target_prim_path /* nullable */);

/* Prim metadata setters (same keys as lightusd_prim_get_metadata). */
LIGHTUSD_API lightusd_status lightusd_prim_set_metadata(lightusd_stage* stage,
                                            const char* prim_path,
                                            const char* key, lightusd_type type,
                                            const void* data, size_t count);
LIGHTUSD_API lightusd_status lightusd_prim_set_metadata_token_array(
    lightusd_stage* stage, const char* prim_path, const char* key,
    const char* const* items, size_t count);

/* Variant authoring. */
LIGHTUSD_API lightusd_status lightusd_prim_add_variant_set(lightusd_stage* stage,
                                               const char* prim_path,
                                               const char* set_name);
LIGHTUSD_API lightusd_status lightusd_prim_add_variant(lightusd_stage* stage,
                                           const char* prim_path,
                                           const char* set_name,
                                           const char* variant_name);
LIGHTUSD_API lightusd_status lightusd_prim_set_variant_selection(lightusd_stage* stage,
                                                     const char* prim_path,
                                                     const char* set_name,
                                                     const char* variant_name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUSD_C_H_ */
