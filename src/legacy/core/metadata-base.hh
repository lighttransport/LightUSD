// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// metadata-base.hh - MetadataBase class for dictionary-based metadata storage
//
#pragma once

#include <string>

#include "nonstd/optional.hpp"
#include "meta-variable.hh"
#include "value-types.hh"

namespace lightusd {

///
/// MetadataBase: Dictionary-based metadata storage with typed accessors
///
/// Instead of storing many optional<T> fields (which wastes memory when
/// most are empty), this class uses a single Dictionary and provides
/// typed accessor methods for commonly used metadata fields.
///
/// Usage:
///   meta.set_displayName("My Object");
///   if (meta.has_displayName()) {
///     std::string name = meta.get_displayName();
///   }
///   meta.remove_displayName();
///
class MetadataBase {
 public:
  MetadataBase() = default;
  MetadataBase(const MetadataBase&) = default;
  MetadataBase(MetadataBase&&) noexcept = default;
  MetadataBase& operator=(const MetadataBase&) = default;
  MetadataBase& operator=(MetadataBase&&) noexcept = default;

  //
  // Generic Dictionary access
  //

  /// Get the underlying dictionary (const)
  const Dictionary& data() const LIGHTUSD_LIFETIMEBOUND { return _data; }

  /// Get the underlying dictionary (mutable)
  Dictionary& data() LIGHTUSD_LIFETIMEBOUND { return _data; }

  /// Check if any metadata is authored
  bool authored() const { return !_data.empty(); }

  /// Clear all metadata
  void clear() { _data.clear(); }

  //
  // Generic typed accessors
  //

  /// Check if a key exists
  bool has(const std::string& key) const {
    return _data.count(key) > 0;
  }

  /// Get a value by key (returns nullopt if not found or type mismatch)
  template<typename T>
  nonstd::optional<T> get(const std::string& key) const {
    auto it = _data.find(key);
    if (it == _data.end()) {
      return nonstd::nullopt;
    }
    return it->second.get_value<T>();
  }

  /// Set a value by key
  template<typename T>
  void set(const std::string& key, const T& value) {
    _data[key] = MetaVariable(value);
  }

  /// Remove a key
  bool remove(const std::string& key) {
    return _data.erase(key) > 0;
  }

  //
  // Common metadata fields with typed accessors
  //

  // ----- displayName (string) -----
  static constexpr const char* kDisplayName = "displayName";

  bool has_displayName() const { return has(kDisplayName); }

  std::string get_displayName() const {
    auto v = get<std::string>(kDisplayName);
    return v.has_value() ? v.value() : std::string();
  }

  void set_displayName(const std::string& value) {
    set(kDisplayName, value);
  }

  void remove_displayName() { remove(kDisplayName); }

  // ----- comment (StringData) -----
  static constexpr const char* kComment = "comment";

  bool has_comment() const { return has(kComment); }

  value::StringData get_comment() const {
    auto v = get<value::StringData>(kComment);
    return v.has_value() ? v.value() : value::StringData();
  }

  void set_comment(const value::StringData& value) {
    set(kComment, value);
  }

  void set_comment(const std::string& value) {
    set(kComment, value::StringData(value));
  }

  void remove_comment() { remove(kComment); }

  // ----- doc/documentation (StringData) -----
  static constexpr const char* kDoc = "documentation";

  bool has_doc() const { return has(kDoc); }

  value::StringData get_doc() const {
    auto v = get<value::StringData>(kDoc);
    return v.has_value() ? v.value() : value::StringData();
  }

  void set_doc(const value::StringData& value) {
    set(kDoc, value);
  }

  void set_doc(const std::string& value) {
    set(kDoc, value::StringData(value));
  }

  void remove_doc() { remove(kDoc); }

  // ----- hidden (bool) -----
  static constexpr const char* kHidden = "hidden";

  bool has_hidden() const { return has(kHidden); }

  bool get_hidden() const {
    auto v = get<bool>(kHidden);
    return v.has_value() ? v.value() : false;
  }

  void set_hidden(bool value) {
    set(kHidden, value);
  }

  void remove_hidden() { remove(kHidden); }

  // ----- active (bool) - Prim only -----
  static constexpr const char* kActive = "active";

  bool has_active() const { return has(kActive); }

  bool get_active() const {
    auto v = get<bool>(kActive);
    return v.has_value() ? v.value() : true;  // default is true
  }

  void set_active(bool value) {
    set(kActive, value);
  }

  void remove_active() { remove(kActive); }

  // ----- customData (Dictionary) -----
  static constexpr const char* kCustomData = "customData";

  bool has_customData() const { return has(kCustomData); }

  Dictionary get_customData() const {
    auto v = get<Dictionary>(kCustomData);
    return v.has_value() ? v.value() : Dictionary();
  }

  void set_customData(const Dictionary& value) {
    set(kCustomData, value);
  }

  void remove_customData() { remove(kCustomData); }

  // ----- sdrMetadata (Dictionary) - usdShade -----
  static constexpr const char* kSdrMetadata = "sdrMetadata";

  bool has_sdrMetadata() const { return has(kSdrMetadata); }

  Dictionary get_sdrMetadata() const {
    auto v = get<Dictionary>(kSdrMetadata);
    return v.has_value() ? v.value() : Dictionary();
  }

  void set_sdrMetadata(const Dictionary& value) {
    set(kSdrMetadata, value);
  }

  void remove_sdrMetadata() { remove(kSdrMetadata); }

  // ----- interpolation (token) - Attribute only -----
  static constexpr const char* kInterpolation = "interpolation";

  bool has_interpolation() const { return has(kInterpolation); }

  value::token get_interpolation() const {
    auto v = get<value::token>(kInterpolation);
    return v.has_value() ? v.value() : value::token();
  }

  void set_interpolation(const value::token& value) {
    set(kInterpolation, value);
  }

  void set_interpolation(const std::string& value) {
    set(kInterpolation, value::token(value));
  }

  void remove_interpolation() { remove(kInterpolation); }

  // ----- elementSize (uint32_t) - usdSkel -----
  static constexpr const char* kElementSize = "elementSize";

  bool has_elementSize() const { return has(kElementSize); }

  uint32_t get_elementSize() const {
    auto v = get<uint32_t>(kElementSize);
    return v.has_value() ? v.value() : 0;
  }

  void set_elementSize(uint32_t value) {
    set(kElementSize, value);
  }

  void remove_elementSize() { remove(kElementSize); }

  // ----- weight (double) - usdSkel BlendShape -----
  static constexpr const char* kWeight = "weight";

  bool has_weight() const { return has(kWeight); }

  double get_weight() const {
    auto v = get<double>(kWeight);
    return v.has_value() ? v.value() : 0.0;
  }

  void set_weight(double value) {
    set(kWeight, value);
  }

  void remove_weight() { remove(kWeight); }

  // ----- colorSpace (token) - Texture attributes -----
  static constexpr const char* kColorSpace = "colorSpace";

  bool has_colorSpace() const { return has(kColorSpace); }

  value::token get_colorSpace() const {
    auto v = get<value::token>(kColorSpace);
    return v.has_value() ? v.value() : value::token();
  }

  void set_colorSpace(const value::token& value) {
    set(kColorSpace, value);
  }

  void set_colorSpace(const std::string& value) {
    set(kColorSpace, value::token(value));
  }

  void remove_colorSpace() { remove(kColorSpace); }

  // ----- connectability (token) - usdShade -----
  static constexpr const char* kConnectability = "connectability";

  bool has_connectability() const { return has(kConnectability); }

  value::token get_connectability() const {
    auto v = get<value::token>(kConnectability);
    return v.has_value() ? v.value() : value::token();
  }

  void set_connectability(const value::token& value) {
    set(kConnectability, value);
  }

  void remove_connectability() { remove(kConnectability); }

  // ----- renderType (token) - usdShade -----
  static constexpr const char* kRenderType = "renderType";

  bool has_renderType() const { return has(kRenderType); }

  value::token get_renderType() const {
    auto v = get<value::token>(kRenderType);
    return v.has_value() ? v.value() : value::token();
  }

  void set_renderType(const value::token& value) {
    set(kRenderType, value);
  }

  void remove_renderType() { remove(kRenderType); }

  // ----- outputName (token) - usdShade -----
  static constexpr const char* kOutputName = "outputName";

  bool has_outputName() const { return has(kOutputName); }

  value::token get_outputName() const {
    auto v = get<value::token>(kOutputName);
    return v.has_value() ? v.value() : value::token();
  }

  void set_outputName(const value::token& value) {
    set(kOutputName, value);
  }

  void remove_outputName() { remove(kOutputName); }

  // ----- bindMaterialAs (token) - MaterialBinding -----
  static constexpr const char* kBindMaterialAs = "bindMaterialAs";

  bool has_bindMaterialAs() const { return has(kBindMaterialAs); }

  value::token get_bindMaterialAs() const {
    auto v = get<value::token>(kBindMaterialAs);
    return v.has_value() ? v.value() : value::token();
  }

  void set_bindMaterialAs(const value::token& value) {
    set(kBindMaterialAs, value);
  }

  void remove_bindMaterialAs() { remove(kBindMaterialAs); }

  // ----- instanceable (bool) - Prim only -----
  static constexpr const char* kInstanceable = "instanceable";

  bool has_instanceable() const { return has(kInstanceable); }

  bool get_instanceable() const {
    auto v = get<bool>(kInstanceable);
    return v.has_value() ? v.value() : false;
  }

  void set_instanceable(bool value) {
    set(kInstanceable, value);
  }

  void remove_instanceable() { remove(kInstanceable); }

  // ----- sceneName (string) - USDZ extension -----
  static constexpr const char* kSceneName = "sceneName";

  bool has_sceneName() const { return has(kSceneName); }

  std::string get_sceneName() const {
    auto v = get<std::string>(kSceneName);
    return v.has_value() ? v.value() : std::string();
  }

  void set_sceneName(const std::string& value) {
    set(kSceneName, value);
  }

  void remove_sceneName() { remove(kSceneName); }

  // ----- assetInfo (Dictionary) - Prim only -----
  static constexpr const char* kAssetInfo = "assetInfo";

  bool has_assetInfo() const { return has(kAssetInfo); }

  Dictionary get_assetInfo() const {
    auto v = get<Dictionary>(kAssetInfo);
    return v.has_value() ? v.value() : Dictionary();
  }

  void set_assetInfo(const Dictionary& value) {
    set(kAssetInfo, value);
  }

  void remove_assetInfo() { remove(kAssetInfo); }

  // ----- clips (Dictionary) - Prim only -----
  static constexpr const char* kClips = "clips";

  bool has_clips() const { return has(kClips); }

  Dictionary get_clips() const {
    auto v = get<Dictionary>(kClips);
    return v.has_value() ? v.value() : Dictionary();
  }

  void set_clips(const Dictionary& value) {
    set(kClips, value);
  }

  void remove_clips() { remove(kClips); }

  //
  // Merge/update operations
  //

  /// Merge metadata from another MetadataBase
  /// @param rhs Source metadata
  /// @param override_existing If true, overwrite existing keys; if false, only add new keys
  void merge_from(const MetadataBase& rhs, bool override_existing = true) {
    for (const auto& [key, mv] : rhs._data) {
      if (override_existing || _data.count(key) == 0) {
        _data[key] = mv;
      }
    }
  }

 protected:
  Dictionary _data;
};

}  // namespace lightusd
