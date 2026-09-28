// SPDX-License-Identifier: Apache-2.0
// C++17 ownership facade. Algorithms and storage live behind the C API.
#pragma once

#include "lightusd-c.h"
#include <utility>

namespace lightusd {
namespace api {

// Adopt one owning C reference. No C++ allocation or exception machinery.
template <class T, void (*Destroy)(T*)>
class Owner {
 public:
  explicit Owner(T* handle = nullptr) noexcept : handle_(handle) {}
  ~Owner() { Destroy(handle_); }
  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;
  Owner(Owner&& other) noexcept : handle_(other.release()) {}
  Owner& operator=(Owner&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  T* get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  T* release() noexcept { return std::exchange(handle_, nullptr); }
  void reset(T* handle = nullptr) noexcept {
    if (handle_ != handle) Destroy(std::exchange(handle_, handle));
  }
  // Release the old value before passing an out-parameter to a C function.
  T** put() noexcept { reset(); return &handle_; }

 private:
  T* handle_;
};

using Value = Owner<lightusd_value, lightusd_value_destroy>;
using BackPlate = Owner<lightusd_backplate, lightusd_backplate_destroy>;
using SkelSample = Owner<lightusd_skel_sample, lightusd_skel_sample_destroy>;
using String = Owner<lightusd_string, lightusd_string_destroy>;
using StringList = Owner<lightusd_strlist, lightusd_strlist_destroy>;
using AssetResolver = Owner<lightusd_asset_resolver,
                            lightusd_asset_resolver_destroy>;

inline lightusd_sv StringView(const String& string) noexcept {
  return lightusd_string_view(string.get());
}

inline const char* LastError() noexcept { return lightusd_last_error(); }
inline uint32_t ApiVersion() noexcept { return lightusd_api_version(); }
inline const char* VersionString() noexcept { return lightusd_version_string(); }

inline const char* TypeName(lightusd_type type) noexcept {
  return lightusd_type_name(type);
}
inline lightusd_type TypeFromName(const char* name) noexcept {
  return lightusd_type_from_name(name);
}
inline size_t TypeSize(lightusd_type type) noexcept {
  return lightusd_type_size(type);
}
inline size_t TypeComponentCount(lightusd_type type) noexcept {
  return lightusd_type_component_count(type);
}

inline lightusd_status FlattenFileToUSDC(
    const char* input, const char* output,
    const lightusd_load_options* options = nullptr) {
  return lightusd_flatten_file_to_usdc(input, output, options);
}

inline lightusd_status CopyStringView(lightusd_sv view, char* out, size_t cap,
                                     size_t* required) noexcept {
  return lightusd_sv_copy(view, out, cap, required);
}

inline size_t StringListSize(const StringList& strings) noexcept {
  return lightusd_strlist_size(strings.get());
}

inline lightusd_sv StringListGet(const StringList& strings,
                                 size_t index) noexcept {
  return lightusd_strlist_get(strings.get(), index);
}

inline lightusd_status ValueView(const Value& value,
                                 lightusd_value_view* out) noexcept {
  return lightusd_value_get_view(value.get(), out);
}

inline lightusd_status ValueString(const Value& value,
                                   lightusd_sv* out) noexcept {
  return lightusd_value_get_string(value.get(), out);
}

inline lightusd_status ValueTokenArray(const Value& value,
                                       StringList* out) noexcept {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_value_get_token_array(value.get(), out->put());
}

// Non-owning read-only cursor into customData, assetInfo, or layer data.
// Keep its owner alive and reacquire it after structural mutation.
class DictionaryView {
 public:
  DictionaryView() = default;
  explicit DictionaryView(lightusd_dict_ref ref) noexcept : ref_(ref) {}
  bool valid() const noexcept { return lightusd_dict_is_valid(ref_) != 0; }
  size_t size() const noexcept { return lightusd_dict_size(ref_); }
  lightusd_status token_array(const char* key, StringList* out) const {
    return out ? lightusd_dict_get_token_array(ref_, key, out->put()) : LIGHTUSD_ERR_INVALID_ARG;
  }
  lightusd_status find(const char* key, lightusd_value_view* value,
                       lightusd_sv* string_value = nullptr) const noexcept {
    return lightusd_dict_find(ref_, key, value, string_value, nullptr);
  }
  lightusd_status entry(size_t index, lightusd_sv* key,
                        lightusd_value_view* value,
                        lightusd_sv* string_value = nullptr) const noexcept {
    return lightusd_dict_entry(ref_, index, key, value, string_value, nullptr);
  }

 private:
  lightusd_dict_ref ref_{};
};

inline lightusd_status CreateAssetResolver(AssetResolver* out) {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_asset_resolver_create(out->put());
}

inline lightusd_status RegisterMemoryAsset(const AssetResolver& resolver,
                                           const char* identifier,
                                           const uint8_t* data, size_t size) {
  return lightusd_asset_resolver_register_memory(resolver.get(), identifier,
                                                 data, size);
}

inline lightusd_status UnregisterMemoryAsset(const AssetResolver& resolver,
                                             const char* identifier) {
  return lightusd_asset_resolver_unregister_memory(resolver.get(), identifier);
}

inline lightusd_status SetAssetResolverMemoryLimit(const AssetResolver& resolver,
                                                   size_t limit_bytes) {
  return lightusd_asset_resolver_set_memory_limit(resolver.get(), limit_bytes);
}

inline lightusd_status GetAssetResolverMemoryStats(const AssetResolver& resolver,
                                                   size_t* asset_count,
                                                   size_t* bytes_used,
                                                   size_t* limit_bytes) {
  return lightusd_asset_resolver_get_memory_stats(
      resolver.get(), asset_count, bytes_used, limit_bytes);
}

inline lightusd_status AssetResolverMemoryIdentifier(
    const AssetResolver& resolver, size_t index, char* out, size_t capacity,
    size_t* required_size) {
  return lightusd_asset_resolver_memory_identifier(
      resolver.get(), index, out, capacity, required_size);
}

inline lightusd_status SetAssetAlias(const AssetResolver& resolver,
                                     const char* authored_path,
                                     const char* resolved_identifier) {
  return lightusd_asset_resolver_set_alias(resolver.get(), authored_path,
                                           resolved_identifier);
}

inline lightusd_status ReadResolvedAsset(const AssetResolver& resolver,
                                         const char* identifier, String* out) {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_asset_resolver_read(resolver.get(), identifier, out->put());
}

inline lightusd_status DependencyReportJSON(AssetResolver& resolver,
                                            const char* root, String* out,
                                            bool* complete) {
  if (!out || !complete) return LIGHTUSD_ERR_INVALID_ARG;
  uint8_t is_complete = 0;
  const lightusd_status status = lightusd_dependency_report_json(
      resolver.get(), root, out->put(), &is_complete);
  if (status == LIGHTUSD_OK) *complete = is_complete != 0;
  return status;
}

inline lightusd_status ValueToUSDA(const Value& value, String* out) {
  if (!out) return LIGHTUSD_ERR_INVALID_ARG;
  return lightusd_value_to_usda(value.get(), out->put());
}

inline void InitLoadOptions(lightusd_load_options* options) noexcept {
  lightusd_load_options_init(options);
}

inline void InitSaveOptions(lightusd_save_options* options) noexcept {
  lightusd_save_options_init(options);
}

class Prim;

// Copies retain the same stage; they do not clone its scene data.
class Stage {
 public:
  explicit Stage(lightusd_stage* handle = nullptr) noexcept : handle_(handle) {}
  ~Stage() { lightusd_stage_destroy(handle_); }
  Stage(const Stage& other) noexcept : handle_(other.handle_) {
    lightusd_stage_retain(handle_);
  }
  Stage(Stage&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Stage& operator=(Stage other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
  }
  lightusd_stage* get() const noexcept { return handle_; }
  bool is_read_only() const noexcept { return lightusd_stage_is_read_only(handle_) != 0; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }
  lightusd_status create() {
    lightusd_stage* handle = nullptr;
    const auto status = lightusd_stage_create(&handle);
    if (status == LIGHTUSD_OK) *this = Stage(handle);
    return status;
  }
  lightusd_status load(const char* filename,
                       const lightusd_load_options* options = nullptr) {
    lightusd_stage* handle = nullptr;
    const auto status = lightusd_stage_load(filename, options, &handle);
    if (status == LIGHTUSD_OK) *this = Stage(handle);
    return status;
  }
  lightusd_status load_from_memory(const uint8_t* data, size_t size,
                                   const lightusd_load_options* options = nullptr) {
    lightusd_stage* handle = nullptr;
    const auto status = lightusd_stage_load_from_memory(data, size, options, &handle);
    if (status == LIGHTUSD_OK) *this = Stage(handle);
    return status;
  }
  lightusd_status define_prim(const char* path, const char* type_name,
                              uint8_t specifier, Prim* out = nullptr);
  lightusd_status remove_prim(const char* path) {
    return lightusd_stage_remove_prim(handle_, path);
  }
  lightusd_status set_attribute(const char* prim_path, const char* name,
                                lightusd_type type, uint8_t is_array,
                                const void* data, size_t count,
                                uint16_t flags = 0) {
    return lightusd_attr_set(handle_, prim_path, name, type, is_array, data,
                             count, flags);
  }
  lightusd_status set_attribute_token_array(
      const char* prim_path, const char* name, lightusd_type type,
      const char* const* items, size_t count, uint16_t flags = 0) {
    return lightusd_attr_set_token_array(handle_, prim_path, name, type, items,
                                         count, flags);
  }
  lightusd_status set_attribute_timesample(
      const char* prim_path, const char* name, double time,
      lightusd_type type, uint8_t is_array, const void* data, size_t count) {
    return lightusd_attr_set_timesample(handle_, prim_path, name, time, type,
                                        is_array, data, count);
  }
  lightusd_status block_attribute(const char* prim_path, const char* name) {
    return lightusd_attr_block(handle_, prim_path, name);
  }
  lightusd_status remove_attribute(const char* prim_path, const char* name) {
    return lightusd_attr_remove(handle_, prim_path, name);
  }
  lightusd_status set_attribute_metadata(
      const char* prim_path, const char* name, const char* key,
      lightusd_type type, const void* data, size_t count) {
    return lightusd_attr_set_metadata(handle_, prim_path, name, key, type,
                                      data, count);
  }
  lightusd_status add_connection(const char* prim_path, const char* name,
                                 const char* target) {
    return lightusd_attr_add_connection(handle_, prim_path, name, target);
  }
  lightusd_status add_relationship_target(const char* prim_path,
                                          const char* relationship,
                                          const char* target) {
    return lightusd_rel_add_target(handle_, prim_path, relationship, target);
  }
  lightusd_status set_relationship_targets(const char* prim_path,
                                           const char* relationship,
                                           const char* const* targets,
                                           size_t count) {
    return lightusd_rel_set_targets(handle_, prim_path, relationship, targets,
                                    count);
  }
  lightusd_status remove_relationship(const char* prim_path,
                                      const char* relationship) {
    return lightusd_rel_remove(handle_, prim_path, relationship);
  }
  lightusd_status add_arc(const char* prim_path, uint8_t arc_type,
                          const char* asset_path,
                          const char* target_prim_path) {
    return lightusd_prim_add_arc(handle_, prim_path, arc_type, asset_path,
                                 target_prim_path);
  }
  lightusd_status set_prim_metadata(const char* prim_path, const char* key,
                                    lightusd_type type, const void* data,
                                    size_t count) {
    return lightusd_prim_set_metadata(handle_, prim_path, key, type, data,
                                      count);
  }
  lightusd_status set_prim_metadata_token_array(
      const char* prim_path, const char* key, const char* const* items,
      size_t count) {
    return lightusd_prim_set_metadata_token_array(handle_, prim_path, key,
                                                  items, count);
  }
  lightusd_status take_warnings(String* out) {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_take_warnings(handle_, out->put());
  }
  uint64_t generation() const noexcept { return lightusd_stage_generation(handle_); }
  lightusd_status save(const char* filename,
                       const lightusd_save_options* options = nullptr) const {
    return lightusd_stage_save(handle_, filename, options);
  }
  lightusd_status save_usdz_with_assets(
      const char* filename, const char* const* asset_names,
      const uint8_t* const* asset_data, const size_t* asset_sizes,
      size_t asset_count) const {
    return lightusd_stage_save_usdz_with_assets(
        handle_, filename, asset_names, asset_data, asset_sizes, asset_count);
  }
  lightusd_status export_usda(String* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_export_usda(handle_, out->put());
  }
  lightusd_status export_usdc(String* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_export_usdc(handle_, out->put());
  }
  lightusd_status validate_core(size_t* errors, size_t* warnings) const {
    return lightusd_stage_validate_core(handle_, errors, warnings);
  }
  lightusd_status flatten(Stage* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_stage* flattened = nullptr;
    const lightusd_status status = lightusd_stage_flatten(handle_, &flattened);
    if (status == LIGHTUSD_OK) *out = Stage(flattened);
    return status;
  }
  lightusd_status metadata(const char* key, Value* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_get_metadata(handle_, key, out->put());
  }
  lightusd_status set_metadata(const char* key, lightusd_type type,
                               const void* data, size_t count) {
    return lightusd_stage_set_metadata(handle_, key, type, data, count);
  }
  lightusd_status explain_property(const char* prim_path, const char* property,
                                   double time, String* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_explain_property(handle_, prim_path, property, time,
                                           out->put());
  }
  lightusd_status set_default_prim(const char* prim_name) {
    return lightusd_stage_set_default_prim(handle_, prim_name);
  }
  lightusd_status add_variant_set(const char* prim_path, const char* set_name) {
    return lightusd_prim_add_variant_set(handle_, prim_path, set_name);
  }
  lightusd_status add_variant(const char* prim_path, const char* set_name,
                              const char* variant_name) {
    return lightusd_prim_add_variant(handle_, prim_path, set_name,
                                     variant_name);
  }
  lightusd_status set_variant_selection(const char* prim_path,
                                        const char* set_name,
                                        const char* variant_name) {
    return lightusd_prim_set_variant_selection(handle_, prim_path, set_name,
                                               variant_name);
  }
  lightusd_status sublayers(StringList* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_stage_sublayers(handle_, out->put());
  }
  lightusd_status add_sublayer_path(const char* asset_path) {
    return lightusd_stage_add_sublayer_path(handle_, asset_path);
  }
  lightusd_status custom_layer_data(DictionaryView* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_dict_ref ref{};
    const lightusd_status status = lightusd_stage_custom_layer_data(handle_, &ref);
    if (status == LIGHTUSD_OK) *out = DictionaryView(ref);
    return status;
  }
  size_t root_prim_count() const noexcept {
    return lightusd_stage_root_prim_count(handle_);
  }
  size_t prim_count() const noexcept { return lightusd_stage_prim_count(handle_); }
  lightusd_sv default_prim_path() const noexcept {
    return lightusd_stage_default_prim_path(handle_);
  }
  lightusd_status stats(lightusd_stage_stats* out) const noexcept {
    return lightusd_stage_get_stats(handle_, out);
  }
  double start_timecode() const noexcept {
    return lightusd_stage_start_timecode(handle_);
  }
  double end_timecode() const noexcept {
    return lightusd_stage_end_timecode(handle_);
  }
  Prim root_prim(size_t index) const;
  Prim default_prim() const;
  Prim prim(const char* path) const;

 private:
  lightusd_stage* handle_;
};

// Keeps the owner alive, but does not prevent structural mutation. Reacquire
// from Stage after mutation; operator bool checks the generation in C.
class Prim {
 public:
  Prim() = default;
  Prim(const Prim&) = default;
  Prim& operator=(const Prim&) = default;
  Prim(Prim&& other) noexcept
      : owner_(std::move(other.owner_)),
        prim_(std::exchange(other.prim_, lightusd_prim{})) {}
  Prim& operator=(Prim&& other) noexcept {
    if (this != &other) {
      owner_ = std::move(other.owner_);
      prim_ = std::exchange(other.prim_, lightusd_prim{});
    }
    return *this;
  }
  lightusd_prim get() const noexcept { return prim_; }
  explicit operator bool() const { return lightusd_prim_is_valid(prim_) != 0; }
  lightusd_sv name() const noexcept { return lightusd_prim_name(prim_); }
  lightusd_sv type_name() const noexcept { return lightusd_prim_type_name(prim_); }
  lightusd_sv path() const noexcept { return lightusd_prim_path(prim_); }
  uint8_t specifier() const noexcept { return lightusd_prim_specifier(prim_); }
  bool is_active() const noexcept { return lightusd_prim_is_active(prim_) != 0; }
  lightusd_sv kind() const noexcept { return lightusd_prim_kind(prim_); }
  Prim parent() const { return Prim(owner_, lightusd_prim_parent(prim_)); }
  size_t child_count() const noexcept { return lightusd_prim_child_count(prim_); }
  Prim child(size_t index) const { return Prim(owner_, lightusd_prim_child(prim_, index)); }
  Prim child(const char* name) const {
    return Prim(owner_, lightusd_prim_child_by_name(prim_, name));
  }
  size_t property_count() const noexcept {
    return lightusd_prim_property_count(prim_);
  }
  lightusd_sv property_name(size_t index) const noexcept {
    return lightusd_prim_property_name(prim_, index);
  }
  uint16_t property_flags(size_t index) const noexcept {
    return lightusd_prim_property_flags_at(prim_, index);
  }
  uint16_t property_flags(const char* name) const noexcept {
    return lightusd_prim_property_flags(prim_, name);
  }
  bool has_property(const char* name) const noexcept {
    return lightusd_prim_has_property(prim_, name) != 0;
  }
  bool has_relationship(const char* name) const noexcept {
    return lightusd_prim_has_relationship(prim_, name) != 0;
  }
  lightusd_sv property_type_name(const char* name) const noexcept {
    return lightusd_prim_property_type_name(prim_, name);
  }
  lightusd_status metadata(const char* name, const char* key,
                           Value* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_attr_metadata(prim_, name, key, out->put());
  }
  lightusd_status metadata(const char* key, Value* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_prim_get_metadata(prim_, key, out->put());
  }
  lightusd_status asset_info(DictionaryView* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_dict_ref ref{};
    const lightusd_status status = lightusd_prim_asset_info(prim_, &ref);
    if (status == LIGHTUSD_OK) *out = DictionaryView(ref);
    return status;
  }
  lightusd_status custom_data(const char* name, DictionaryView* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_dict_ref ref{};
    const lightusd_status status = lightusd_attr_custom_data(prim_, name, &ref);
    if (status == LIGHTUSD_OK) *out = DictionaryView(ref);
    return status;
  }
  lightusd_status custom_data(DictionaryView* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    lightusd_dict_ref ref{};
    const lightusd_status status = lightusd_prim_custom_data(prim_, &ref);
    if (status == LIGHTUSD_OK) *out = DictionaryView(ref);
    return status;
  }
  size_t connection_count(const char* name) const noexcept {
    return lightusd_attr_connection_count(prim_, name);
  }
  lightusd_sv connection(const char* name, size_t index) const noexcept {
    return lightusd_attr_connection(prim_, name, index);
  }
  size_t relationship_count() const noexcept {
    return lightusd_prim_relationship_count(prim_);
  }
  lightusd_status relationship_names(StringList* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_prim_relationship_names(prim_, out->put());
  }
  size_t relationship_target_count(const char* name) const noexcept {
    return lightusd_rel_target_count(prim_, name);
  }
  lightusd_sv relationship_target(const char* name, size_t index) const noexcept {
    return lightusd_rel_target(prim_, name, index);
  }
  size_t variant_set_count() const noexcept {
    return lightusd_prim_variant_set_count(prim_);
  }
  lightusd_sv variant_set_name(size_t index) const noexcept {
    return lightusd_prim_variant_set_name(prim_, index);
  }
  size_t variant_count(const char* set_name) const noexcept {
    return lightusd_variant_count(prim_, set_name);
  }
  lightusd_sv variant_name(const char* set_name, size_t index) const noexcept {
    return lightusd_variant_name(prim_, set_name, index);
  }
  lightusd_sv variant_selection(const char* set_name) const noexcept {
    return lightusd_variant_selection(prim_, set_name);
  }
  lightusd_status local_transform(double time, double out16[16]) const noexcept {
    return lightusd_prim_local_transform(prim_, time, out16);
  }
  lightusd_status world_transform(double time, double out16[16]) const noexcept {
    return lightusd_prim_world_transform(owner_.get(), prim_, time, out16);
  }
  lightusd_status attribute(const char* name, lightusd_value_view* out) const {
    return lightusd_attr_get(prim_, name, out);
  }
  lightusd_status attribute_string(const char* name, lightusd_sv* out) const {
    return lightusd_attr_get_string(prim_, name, out);
  }
  lightusd_status attribute_token_array(const char* name, StringList* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_attr_get_token_array(prim_, name, out->put());
  }
  bool has_timesamples(const char* name) const noexcept {
    return lightusd_attr_has_timesamples(prim_, name) != 0;
  }
  size_t timesample_count(const char* name) const noexcept {
    return lightusd_attr_timesample_count(prim_, name);
  }
  size_t timesample_times(const char* name, double* out, size_t cap) const noexcept {
    return lightusd_attr_timesample_times(prim_, name, out, cap);
  }
  lightusd_status timesample_at(const char* name, size_t index, double* time,
                                lightusd_value_view* out) const {
    return lightusd_attr_timesample_at(prim_, name, index, time, out);
  }
  lightusd_status interpolate(const char* name, double time, uint8_t mode,
                              Value* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_attr_interpolate(prim_, name, time, mode, out->put());
  }
  lightusd_status evaluate(const char* name, double time, uint8_t interp_mode,
                           bool follow_connections, Value* out) const {
    if (!out) return LIGHTUSD_ERR_INVALID_ARG;
    return lightusd_attr_eval_ex(owner_.get(), prim_, name, time, interp_mode,
                                 follow_connections ? 1 : 0, out->put());
  }
  lightusd_status skel_animation_joint_count_at_time(double time,
                                                     size_t* out) const {
    return lightusd_skel_animation_joint_count_at_time(owner_.get(), prim_,
                                                       time, out);
  }

 private:
  friend class Stage;
  Prim(const Stage& owner, lightusd_prim prim) : owner_(owner), prim_(prim) {}
  Stage owner_;
  lightusd_prim prim_{};
};

inline Prim Stage::prim(const char* path) const {
  return Prim(*this, lightusd_stage_prim_at_path(handle_, path));
}

inline lightusd_status Stage::define_prim(const char* path,
                                          const char* type_name,
                                          uint8_t specifier, Prim* out) {
  lightusd_prim prim{};
  const lightusd_status status = lightusd_stage_define_prim(
      handle_, path, type_name, specifier, out ? &prim : nullptr);
  if (status == LIGHTUSD_OK && out) *out = Prim(*this, prim);
  return status;
}

inline Prim Stage::root_prim(size_t index) const {
  return Prim(*this, lightusd_stage_root_prim(handle_, index));
}

inline Prim Stage::default_prim() const {
  return Prim(*this, lightusd_stage_default_prim(handle_));
}

}  // namespace api
}  // namespace lightusd
