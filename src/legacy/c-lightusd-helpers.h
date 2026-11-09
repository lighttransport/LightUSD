/* SPDX-License-Identifier: Apache 2.0
 *
 * Small C-callable helpers that fill gaps in c-lightusd.h for the Python
 * extension. Everything here is pure C11; C++ interop lives in the .cc file.
 *
 * Ownership model (changed from the first draft):
 *
 *   - Functions named `*_get_*` may return a borrowed pointer OR allocate an
 *     owned handle, following the convention stated per-function.
 *   - `*_copy_*` / `*_new_*` always allocate; caller owns the result and must
 *     pair it with the matching `*_free` / `c_lightusd_*_free`.
 *   - Borrowed pointers are stable only for the lifetime of the source object
 *     given — the "owner" argument establishes that lifetime contract.
 *
 * In particular, `c_lightusd_prim_get_attribute` returns an OWNED
 * CLightUSDAttribute*. This removes a correctness bug in the earlier version
 * where two concurrent handles aliased a single thread-local cache.
 */
#ifndef C_LIGHTUSD_HELPERS_H
#define C_LIGHTUSD_HELPERS_H

#include "c-lightusd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Stage helpers ---- */

/* Number of root prims in the stage. Returns 0 if stage is null. */
C_LIGHTUSD_EXPORT uint64_t
c_lightusd_stage_num_root_prims(const CLightUSDStage *stage);

/* Borrowed pointer to i-th root prim. *out is valid until the Stage is
 * modified or freed. Returns 1 on success, 0 otherwise.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_get_root_prim(const CLightUSDStage *stage, uint64_t index,
                              const CLightUSDPrim **out);

/* Borrowed pointer to the prim at `abs_path` ("/World/Mesh", etc).
 * Returns 1 on success; 0 if not found or on error (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_get_prim_at_path(const CLightUSDStage *stage,
                                 const char *abs_path,
                                 const CLightUSDPrim **out,
                                 c_lightusd_string_t *err);

/* Save stage to file. `format` is one of C_LIGHTUSD_FORMAT_USDA / USDC / USDZ
 * (AUTO = infer from extension). Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_save_to_file(const CLightUSDStage *stage, const char *filename,
                             CLightUSDFormat format,
                             c_lightusd_string_t *warn,
                             c_lightusd_string_t *err);

/* Save stage as USDZ with additional packed assets.
 *
 * Each (asset_names[i], asset_data[i], asset_sizes[i]) triple becomes
 * an entry in the USDZ archive next to the root layer. The first
 * archive entry is always the root layer (root.usdc) regardless of
 * the additional assets; the additional assets follow in the order
 * supplied. The archive is uncompressed (Store-only) and 64-byte
 * aligned per the AOUSD USDZ spec.
 *
 * Pass num_assets=0 (and NULL pointers) to behave like
 * c_lightusd_stage_save_to_file with format=USDZ.
 *
 * Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_save_as_usdz_with_assets(
    const CLightUSDStage *stage, const char *filename,
    const char *const *asset_names,
    const uint8_t *const *asset_data,
    const uint64_t *asset_sizes,
    uint64_t num_assets,
    c_lightusd_string_t *warn,
    c_lightusd_string_t *err);

/* Parse USDA/USDC/USDZ bytes from memory into the supplied Stage. Unlike the
 * existing c_lightusd_load_*_from_memory entry points (which lack a Stage *
 * output and are effectively useless), these thread the stage through
 * properly.
 *
 * `format` may be C_LIGHTUSD_FORMAT_AUTO to sniff from the first bytes.
 * Returns 1 on success, 0 on failure (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_load_from_memory(CLightUSDStage *stage,
                                 const uint8_t *data, size_t nbytes,
                                 CLightUSDFormat format,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err);

/* ---- Prim helpers ---- */

/* Return an OWNED attribute copy for `name` on `prim`, or NULL if not found
 * (or if the property is a relationship rather than an attribute). Free with
 * c_lightusd_attribute_free.
 *
 * The returned attribute contains its own copy of the underlying value; the
 * value buffer is stable as long as the attribute is alive, which is what
 * `c_lightusd_value_array_data` relies on for zero-copy access.
 */
C_LIGHTUSD_EXPORT CLightUSDAttribute *
c_lightusd_prim_get_attribute(const CLightUSDPrim *prim, const char *name);

/* Return an OWNED relationship copy for `name` on `prim`. Free with
 * c_lightusd_relationsip_free (sic: existing C API typo preserved for ABI).
 */
C_LIGHTUSD_EXPORT CLightUSDRelationship *
c_lightusd_prim_get_relationship(const CLightUSDPrim *prim, const char *name);

/* Number of children on `prim`. (Already exists as
 * c_lightusd_prim_num_children — kept here as a documented entry point.) */

/* ---- Attribute helpers ---- */

/* Allocate and initialise an empty Attribute. */
C_LIGHTUSD_EXPORT CLightUSDAttribute *
c_lightusd_attribute_new(void);

/* Free an owned Attribute allocated by c_lightusd_prim_get_attribute /
 * c_lightusd_attribute_new. Returns 0 if `attr` was null.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_free(CLightUSDAttribute *attr);

/* Borrowed pointer to the default value of `attr`. Returns 1 if a default
 * value is authored, 0 otherwise. *out is valid as long as `attr` is alive.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_get_value(const CLightUSDAttribute *attr,
                              const CLightUSDValue **out);

/* Copy the attribute's name / USD type-name into caller-allocated string. */
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_get_name(const CLightUSDAttribute *attr,
                             c_lightusd_string_t *out);
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_get_type_name(const CLightUSDAttribute *attr,
                                  c_lightusd_string_t *out);

/* ---- Value helpers ---- */

/* Return a zero-copy view of the stored array element storage.
 *
 *   *out_ptr         pointer to contiguous component storage
 *   *out_n_outer     number of outer-dimension elements (e.g. N points)
 *   *out_n_inner     number of inner-dimension components per outer item
 *                    (1 for plain scalars, 3 for point3f, 4 for quatf, 16
 *                    for matrix4d, etc.)
 *   *out_component_size  size of ONE component in bytes
 *                        (4 for float, 8 for double, 2 for half)
 *   *out_format      single-char Python struct format for the component
 *                    ('f', 'd', 'e', 'i', 'I', 'q', 'Q', 'B')
 *
 * Total buffer length in bytes = n_outer * n_inner * component_size.
 * Python can expose this as shape (n_outer,) or (n_outer, n_inner) based on
 * whether n_inner > 1.
 *
 * Returns 1 on success. 0 if the Value is not an array, is empty, or has an
 * unsupported element type.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_value_array_data(const CLightUSDValue *value,
                           const void **out_ptr,
                           uint64_t *out_n_outer,
                           uint32_t *out_n_inner,
                           uint32_t *out_component_size,
                           const char **out_format);

/* Scalar-coercion accessors. */
C_LIGHTUSD_EXPORT int
c_lightusd_value_as_double(const CLightUSDValue *value, double *out);
C_LIGHTUSD_EXPORT int
c_lightusd_value_as_int64(const CLightUSDValue *value, int64_t *out);
C_LIGHTUSD_EXPORT int
c_lightusd_value_as_bool(const CLightUSDValue *value, int *out);

/* For token/string scalars, copy into `out`. Returns 1 on success. */
C_LIGHTUSD_EXPORT int
c_lightusd_value_get_string(const CLightUSDValue *value, c_lightusd_string_t *out);

/* Is this value a 1D array? (e.g. float3[] not float3.) */
C_LIGHTUSD_EXPORT int
c_lightusd_value_is_array(const CLightUSDValue *value);

/* ---- Path helpers ---- */

/* Copy the string form of a Path into `out`. Returns 1 on success. */
C_LIGHTUSD_EXPORT int
c_lightusd_path_to_string(const CLightUSDPath *path, c_lightusd_string_t *out);

/* ---- Authoring helpers (used by Python bindings) ---- */

/* Set the element name (leaf path component) of a Prim. Required after
 * c_lightusd_prim_new()/_new_builtin() and before adding the prim under a
 * Stage / parent Prim. Returns 1 on success, 0 on bad input.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_set_element_name(CLightUSDPrim *prim, const char *name);

/* Add `prim` as a root prim under `stage`. The prim is *copied* into the
 * stage; the caller still owns `prim` and must free it (or transfer it).
 * Returns 1 on success, 0 on failure (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_add_root_prim(CLightUSDStage *stage, CLightUSDPrim *prim,
                              c_lightusd_string_t *err);

/* Set name / type-name / value on an Attribute that was allocated via
 * c_lightusd_attribute_new(). The Value is copied; the caller still owns it.
 * Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_set_name(CLightUSDAttribute *attr, const char *name);
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_set_type_name(CLightUSDAttribute *attr, const char *type_name);
C_LIGHTUSD_EXPORT int
c_lightusd_attribute_set_value(CLightUSDAttribute *attr,
                              const CLightUSDValue *value);

/* Add an Attribute (as a Property) to the Prim's properties map. The
 * attribute name comes from c_lightusd_attribute_get_name (i.e. the attribute
 * must have been named beforehand). The attribute is copied; the caller
 * still owns `attr`. Returns 1 on success, 0 on failure (err is filled).
 *
 * Supported prim types: Xform, GeomMesh, GeomSphere, GeomCube, GeomCylinder,
 * GeomCone, GeomCapsule, GeomCamera, GeomPoints, Scope, Model. Other prim
 * types currently return 0 with an explanatory error message.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_attribute(CLightUSDPrim *prim,
                             const CLightUSDAttribute *attr,
                             c_lightusd_string_t *err);

/* Construct a Value holding a token[] (array of tokens). Returns NULL on
 * allocation failure. The caller owns the returned Value and must free it
 * via c_lightusd_value_free. The token strings are copied.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_token(uint64_t n, const char *const *toks);

/* Construct a Value holding a string[]. */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_string(uint64_t n, const char *const *strs);

/* Append an applied API schema to the prim's apiSchemas metadata.
 * `instance_name` is the optional instance suffix for multi-apply schemas
 * (e.g. for `CollectionAPI:material` pass instance_name="material"); pass
 * NULL or "" for single-apply schemas like MaterialXConfigAPI.
 *
 * Schema names not recognised by lightusd' built-in enum are stored under
 * `apiSchemas.unknownSchemas` so they survive write/read round-trips.
 * Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_apply_api_schema(CLightUSDPrim *prim, const char *schema_name,
                                const char *instance_name);

/* Get the list of applied API schemas as a comma-separated string. The
 * returned form is the schema-name list, with multi-apply instances
 * formatted as "Schema:instance". Returns 1 if any schema is authored.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_api_schemas(const CLightUSDPrim *prim,
                               c_lightusd_string_t *out);

/* Set a string-valued Prim metadatum (kind, doc, displayName, comment, ...).
 * `meta_name` is one of "kind", "displayName", "doc", "documentation",
 * "comment", "sceneName"; other names are stored as a generic string entry.
 * Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_meta_set_string(CLightUSDPrim *prim, const char *meta_name,
                               const char *value);

/* Get a string-valued Prim metadatum. Returns 1 if authored, 0 otherwise.
 * On success, `*out` is filled with the string value.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_meta_get_string(const CLightUSDPrim *prim, const char *meta_name,
                               c_lightusd_string_t *out);

/* Bool-valued prim metadata (active, hidden). */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_meta_set_bool(CLightUSDPrim *prim, const char *meta_name,
                             int value);
C_LIGHTUSD_EXPORT int
c_lightusd_prim_meta_get_bool(const CLightUSDPrim *prim, const char *meta_name,
                             int *out);

/* ---- Double-precision value constructors ----
 *
 * Fill the gap left by c-lightusd.h, which only exposes float* constructors.
 * USD spec types like `double3` (xformOp:translate), `matrix4d` (transform),
 * `metersPerUnit`, etc. require these.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_double(double v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_double2(c_lightusd_double2_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_double3(c_lightusd_double3_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_double4(c_lightusd_double4_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_double(uint64_t n, const double *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_double3(uint64_t n, const c_lightusd_double3_t *v);
/* matrix4d: 16 doubles in row-major order. */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_matrix4d(const double v[16]);

/* matrix2d / matrix3d row-major scalars. */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_matrix2d_t(c_lightusd_matrix2d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_matrix3d_t(c_lightusd_matrix3d_t v);
/* matrix4d via the typedef'd struct (alternative form to the [16] one above). */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_matrix4d_t(c_lightusd_matrix4d_t v);

/* Array forms for matrices and double-vectors. */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_double2(uint64_t n, const c_lightusd_double2_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_double4(uint64_t n, const c_lightusd_double4_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_matrix2d(uint64_t n, const c_lightusd_matrix2d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_matrix3d(uint64_t n, const c_lightusd_matrix3d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_matrix4d(uint64_t n, const c_lightusd_matrix4d_t *v);

/* ---- bool ----
 * USD has a `bool` value type distinct from `int` (mass enabled flags etc.).
 * c-lightusd.h doesn't expose it; provide it here.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_bool(int v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_bool(uint64_t n, const int *v);

/* ---- Typed float3/double3 alias scalars ----
 * color3f / point3f / normal3f / vector3f (and their double counterparts)
 * share the float3/double3 memory layout but author as distinct types so
 * the USDC writer emits the spec-correct attribute role.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_color3f(c_lightusd_color3f_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_point3f(c_lightusd_point3f_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_normal3f(c_lightusd_normal3f_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_vector3f(c_lightusd_float3_t v);

C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_color3d(c_lightusd_color3d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_point3d(c_lightusd_point3d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_normal3d(c_lightusd_normal3d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_vector3d(c_lightusd_double3_t v);

/* Array forms for typed-vec aliases. */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_color3f(uint64_t n, const c_lightusd_color3f_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_point3f(uint64_t n, const c_lightusd_point3f_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_normal3f(uint64_t n, const c_lightusd_normal3f_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_vector3f(uint64_t n, const c_lightusd_float3_t *v);

C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_color3d(uint64_t n, const c_lightusd_color3d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_point3d(uint64_t n, const c_lightusd_point3d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_normal3d(uint64_t n, const c_lightusd_normal3d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_vector3d(uint64_t n, const c_lightusd_double3_t *v);

/* Convenience: convert IEEE-754 binary32 to binary16 (half) bit pattern. */
C_LIGHTUSD_EXPORT c_lightusd_half_t
c_lightusd_float_to_half(float f);

/* ---- Half precision ----
 * Half values use the IEEE-754 binary16 bit pattern packed into uint16_t.
 * The caller is responsible for the float->half conversion (matches the
 * existing c_lightusd_half_t convention).
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_half(c_lightusd_half_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_half2(c_lightusd_half2_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_half3(c_lightusd_half3_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_half4(c_lightusd_half4_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_half(uint64_t n, const c_lightusd_half_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_half2(uint64_t n, const c_lightusd_half2_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_half3(uint64_t n, const c_lightusd_half3_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_half4(uint64_t n, const c_lightusd_half4_t *v);

/* ---- Wide integer scalars ---- */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_uint(uint32_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_uint64(uint64_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_int64(int64_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_uint(uint64_t n, const uint32_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_uint64(uint64_t n, const uint64_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_int64(uint64_t n, const int64_t *v);

/* ---- Quaternions ----
 * Memory layout is {imag[3], real} (matches both lightusd value::quat*
 * and c_lightusd_quat*_t). USDA spelling is `(w, x, y, z)` so the caller
 * is responsible for filling .imag and .real correctly.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_quath(c_lightusd_quath_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_quatf(c_lightusd_quatf_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_quatd(c_lightusd_quatd_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_quath(uint64_t n, const c_lightusd_quath_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_quatf(uint64_t n, const c_lightusd_quatf_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_quatd(uint64_t n, const c_lightusd_quatd_t *v);

/* ---- TexCoord role types (float2/double2 aliases) ----
 * texCoord2f/2d are 2-element role types; texCoord3f/3d are 3-element.
 * Memory layout is identical to float2/double2 and float3/double3.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_texcoord2f(c_lightusd_texcoord2f_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_texcoord2d(c_lightusd_texcoord2d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_texcoord3f(c_lightusd_float3_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_texcoord3d(c_lightusd_double3_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_texcoord2f(uint64_t n, const c_lightusd_texcoord2f_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_texcoord2d(uint64_t n, const c_lightusd_texcoord2d_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_texcoord3f(uint64_t n, const c_lightusd_float3_t *v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_texcoord3d(uint64_t n, const c_lightusd_double3_t *v);

/* ---- frame4d (semantic alias for matrix4d) ----
 * frame4d has identical layout to matrix4d; authored as frame4d in USDA/USDC.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_frame4d(c_lightusd_matrix4d_t v);
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_frame4d(uint64_t n, const c_lightusd_matrix4d_t *v);

/* ---- Stage convenience ----
 * Set/get the stage's `defaultPrim` metadatum. The name is the prim's
 * element name (no leading slash). Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_set_default_prim(CLightUSDStage *stage, const char *name);
C_LIGHTUSD_EXPORT int
c_lightusd_stage_get_default_prim(const CLightUSDStage *stage,
                                 c_lightusd_string_t *out_name);

/* ---- Asset value constructor ----
 *
 * Construct a Value holding an `asset` (e.g. `asset inputs:file = @./tex.png@`).
 * The path string is copied. Returns NULL on allocation failure.
 */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_asset(const char *asset_path);

/* Array form: `asset[]` */
C_LIGHTUSD_EXPORT CLightUSDValue *
c_lightusd_value_new_array_asset(uint64_t n, const char *const *paths);

/* ---- Time-sampled attribute authoring ----
 *
 * Author a single (time, value) sample on the named attribute. If the
 * attribute does not yet exist, it is created with the deduced type.
 * `value` is borrowed; the helper copies. Repeated calls with different
 * `time` values build up the TimeSamples vector.
 *
 * The attribute's USD type-name is taken from the first sample's value
 * (or kept if already authored). `type_name` may be NULL to infer.
 *
 * Returns 1 on success, 0 on failure (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_set_attribute_timesample(CLightUSDPrim *prim, const char *name,
                                        double time,
                                        const CLightUSDValue *value,
                                        const char *type_name,
                                        c_lightusd_string_t *err);

/* Read the number of authored time samples on `name`. Returns 0 if the
 * attribute is not found or has no time samples. */
C_LIGHTUSD_EXPORT uint64_t
c_lightusd_prim_get_attribute_timesample_count(const CLightUSDPrim *prim,
                                              const char *name);

/* Read the i-th authored time sample. *out_time is set to the sample time;
 * *out_value receives a NEW owned Value (caller must c_lightusd_value_free).
 * Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_attribute_timesample(const CLightUSDPrim *prim,
                                        const char *name, uint64_t index,
                                        double *out_time,
                                        CLightUSDValue **out_value);

/* ---- Stage metadata helpers ----
 *
 * Set / get stage-level metadata fields (those that USD writers emit between
 * the `#usda 1.0` header and the first prim).
 *
 * `key` is one of:
 *   "defaultPrim"          (token; bare prim name, no leading slash)
 *   "upAxis"               ("Y", "Z", or "X")
 *   "metersPerUnit"        (double)
 *   "timeCodesPerSecond"   (double)
 *   "framesPerSecond"      (double)
 *   "startTimeCode"        (double)
 *   "endTimeCode"          (double)
 *   "doc" / "documentation" (string)
 *   "comment"              (string)
 *
 * For string-valued keys use the *_string variants; for numeric keys use
 * *_double. Returns 1 on success, 0 on bad key or unsupported type.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_meta_set_string(CLightUSDStage *stage, const char *key,
                                const char *value);
C_LIGHTUSD_EXPORT int
c_lightusd_stage_meta_get_string(const CLightUSDStage *stage, const char *key,
                                c_lightusd_string_t *out);
C_LIGHTUSD_EXPORT int
c_lightusd_stage_meta_set_double(CLightUSDStage *stage, const char *key,
                                double value);
C_LIGHTUSD_EXPORT int
c_lightusd_stage_meta_get_double(const CLightUSDStage *stage, const char *key,
                                double *out);

/* ---- Attribute connection / metadata helpers ----
 *
 * Author an attribute connection (the `.connect = </path>` form):
 *
 *   inputs:diffuseColor.connect = </Mat/Tex.outputs:rgb>
 *
 * `type_name` is the USD attribute type (e.g. "color3f", "float", "token") —
 * required for typed attributes; pass NULL or "" to leave it inferred.
 * `target_paths` is an array of N target path strings.
 * Returns 1 on success, 0 on failure (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_attribute_connection(CLightUSDPrim *prim, const char *name,
                                        const char *type_name,
                                        uint64_t n_targets,
                                        const char *const *target_paths,
                                        c_lightusd_string_t *err);

/* Get the connection target paths of an attribute on a prim. Returns 1 if
 * the named attribute exists and has connections; 0 otherwise. *out_csv is
 * filled with comma-separated target paths.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_attribute_connections(const CLightUSDPrim *prim,
                                         const char *name,
                                         c_lightusd_string_t *out_csv);

/* Set an attribute-level metadatum (custom, hidden, displayName, doc, ...).
 * For string-valued metas use *_string; for boolean *_bool. The attribute
 * must already exist on the prim. Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_attribute_meta_set_string(CLightUSDPrim *prim,
                                         const char *attr_name,
                                         const char *meta_key,
                                         const char *value);
C_LIGHTUSD_EXPORT int
c_lightusd_prim_attribute_meta_set_bool(CLightUSDPrim *prim,
                                       const char *attr_name,
                                       const char *meta_key, int value);

/* Read an attribute-level string meta. Returns 1 if authored. */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_attribute_meta_get_string(const CLightUSDPrim *prim,
                                         const char *attr_name,
                                         const char *meta_key,
                                         c_lightusd_string_t *out);

/* ---- Relationship helpers ----
 *
 * Author a Relationship property on a prim with one or more target paths.
 * `target_paths` is an array of N absolute path strings (e.g. "/Mat/PBR").
 * Returns 1 on success, 0 on failure (err is filled).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_relationship(CLightUSDPrim *prim, const char *name,
                                uint64_t n_targets,
                                const char *const *target_paths,
                                c_lightusd_string_t *err);

C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_relationship_targets(const CLightUSDPrim *prim,
                                        const char *name,
                                        c_lightusd_string_t *out_csv);

/* ---- Composition arc authoring ----
 *
 * USD has four prim-level arcs that compose layers and prims:
 * `references`, `payload`, `inherits`, `specializes`. Each arc list
 * can be qualified by a list-edit operation ("prepend", "append",
 * "add", "delete", "explicit", "order"). The C API exposes a single
 * "add" entry point per arc that appends to the prim's existing
 * list; pass NULL for `prim_path` (or empty string) when no specific
 * prim should be targeted within the referenced layer.
 *
 * Qualifier values match `lightusd::ListEditQual`:
 *   0 = ResetToExplicit (no qualifier — replaces any existing list)
 *   1 = Append
 *   2 = Add
 *   3 = Delete
 *   4 = Prepend
 *   5 = Order
 */
typedef enum {
  C_LIGHTUSD_LISTEDITQUAL_RESETTOEXPLICIT = 0,
  C_LIGHTUSD_LISTEDITQUAL_APPEND = 1,
  C_LIGHTUSD_LISTEDITQUAL_ADD = 2,
  C_LIGHTUSD_LISTEDITQUAL_DELETE = 3,
  C_LIGHTUSD_LISTEDITQUAL_PREPEND = 4,
  C_LIGHTUSD_LISTEDITQUAL_ORDER = 5
} CLightUSDListEditQual;

/* Append a Reference to the prim's `references` list under the given
 * qualifier. `asset_path` may be empty/NULL for an internal reference;
 * `prim_path` may be empty/NULL to target the layer's defaultPrim.
 * `offset` and `scale` populate the LayerOffset (USD defaults: 0, 1).
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_reference(CLightUSDPrim *prim,
                             CLightUSDListEditQual qualifier,
                             const char *asset_path,
                             const char *prim_path,
                             double offset,
                             double scale);

/* Append a Payload to the prim's `payload` list. Same parameter
 * semantics as `c_lightusd_prim_add_reference`.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_payload(CLightUSDPrim *prim,
                           CLightUSDListEditQual qualifier,
                           const char *asset_path,
                           const char *prim_path,
                           double offset,
                           double scale);

/* Append a single Path entry to the prim's `inherits` list. */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_inherit(CLightUSDPrim *prim,
                           CLightUSDListEditQual qualifier,
                           const char *prim_path);

/* Append a single Path entry to the prim's `specializes` list. */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_specialize(CLightUSDPrim *prim,
                              CLightUSDListEditQual qualifier,
                              const char *prim_path);

/* Clear the prim's arc list (sets the optional<> back to nullopt). */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_references(CLightUSDPrim *prim);
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_payload(CLightUSDPrim *prim);
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_inherits(CLightUSDPrim *prim);
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_specializes(CLightUSDPrim *prim);

/* ---- Variant authoring (metadata-level) ---- */

/* Append `name` to the prim's `variantSets = [...]` listop under
 * `qualifier`. Returns 1 on success.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_add_variant_set_name(CLightUSDPrim *prim,
                                    CLightUSDListEditQual qualifier,
                                    const char *name);

/* Drop all authored variantSets list entries (set optional<> to nullopt). */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_variant_set_names(CLightUSDPrim *prim);

/* Set/replace the variant selection for `variant_set_name`. Equivalent
 * to `variants = { string variant_set_name = "variant_name" }`.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_set_variant_selection(CLightUSDPrim *prim,
                                     const char *variant_set_name,
                                     const char *variant_name);

/* Remove the selection for `variant_set_name`. Pass NULL or empty
 * string to clear ALL variant selections on this prim.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_clear_variant_selection(CLightUSDPrim *prim,
                                       const char *variant_set_name);

/* ---- Variant content authoring ---- */

/* Create (or get) a variant identified by (variant_set_name,
 * variant_name) on `prim`. The Variant is empty until populated by the
 * helpers below. Returns 1 on success.
 *
 * NOTE: This populates the Prim::variantSets() storage; for full
 * USDA round-trip you should also call
 * c_lightusd_prim_add_variant_set_name (declares the variantSet) and
 * optionally c_lightusd_prim_set_variant_selection.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_define_variant(CLightUSDPrim *prim,
                              const char *variant_set_name,
                              const char *variant_name);

/* Append a Prim subtree (copied) as a child of the named variant.
 * `child` must be non-null and not the same Prim. The variant is
 * created if it does not yet exist.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_variant_add_child(CLightUSDPrim *prim,
                                 const char *variant_set_name,
                                 const char *variant_name,
                                 const CLightUSDPrim *child);

/* Author an attribute on the named variant's `properties` map.
 * The Attribute is copied. The variant is created if needed.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_variant_add_attribute(CLightUSDPrim *prim,
                                     const char *variant_set_name,
                                     const char *variant_name,
                                     const CLightUSDAttribute *attr,
                                     c_lightusd_string_t *err);

/* Iteration helpers for already-authored variant content.
 *
 * `out_names` is a caller-allocated array of `c_lightusd_string_t *`
 * of length `cap`; on success the function fills the first
 * min(N, cap) entries via c_lightusd_string_replace and writes the
 * total count N to `*out_count`. Pass NULL/0 to query the count.
 *
 * Source of truth is `Prim::variantSets()` (the in-memory map), not
 * the `variantSets = [...]` listop in metas.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_variant_set_names(const CLightUSDPrim *prim,
                                     c_lightusd_string_t **out_names,
                                     uint64_t cap,
                                     uint64_t *out_count);

/* Enumerate the keys of the prim's `variants = {...}` selection map.
 * Distinct from `c_lightusd_prim_get_variant_set_names` (which reads
 * the variantSetNames listOp) — selections live in the metas
 * `variants` map and can be authored without a matching variantSet
 * declaration. Two-call pattern: first call with NULL+0 to get the
 * count, then allocate cap strings and call again. */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_variant_selection_keys(const CLightUSDPrim *prim,
                                          c_lightusd_string_t **out_names,
                                          uint64_t cap,
                                          uint64_t *out_count);

C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_variant_names(const CLightUSDPrim *prim,
                                 const char *variant_set_name,
                                 c_lightusd_string_t **out_names,
                                 uint64_t cap,
                                 uint64_t *out_count);

/* Read the variant selection for `variant_set_name`. Returns 1 if the
 * selection is authored (and writes it to `out`); returns 0 otherwise.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_prim_get_variant_selection(const CLightUSDPrim *prim,
                                     const char *variant_set_name,
                                     c_lightusd_string_t *out);

/* ---- Tydra: scene-access helpers ---- */

typedef int (*CLightUSDVisitFunction)(const CLightUSDPrim *prim,
                                     const CLightUSDPath *abs_path,
                                     uint32_t depth,
                                     void *userdata);

C_LIGHTUSD_EXPORT int
c_lightusd_stage_visit_prims(const CLightUSDStage *stage,
                            CLightUSDVisitFunction cb,
                            void *userdata,
                            c_lightusd_string_t *err);

/* Convenience DFS filter. For each prim whose `prim_type` matches
 * `type_name`, invoke `cb`. Pass NULL `type_name` to match any prim.
 */
C_LIGHTUSD_EXPORT int
c_lightusd_stage_list_prims_by_type(const CLightUSDStage *stage,
                                   const char *type_name,
                                   CLightUSDVisitFunction cb,
                                   void *userdata,
                                   c_lightusd_string_t *err);

#ifdef __cplusplus
}
#endif

#endif /* C_LIGHTUSD_HELPERS_H */
