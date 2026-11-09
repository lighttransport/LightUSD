// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <memory>

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#endif

#include "nonstd/optional.hpp"

namespace lightusd {

// Forward declarations - we'll include the full types in the implementation
class PrimSpec;
class Path;
struct LayerMetas;
struct LayerImpl;

// Similar to SdfLayer
// It is basically hold the list of PrimSpec and Layer metadatum.
class Layer {
 public:
  Layer();
  ~Layer();

  // Copy constructor and assignment
  Layer(const Layer& other);
  Layer& operator=(const Layer& other);

  // Move constructor and assignment
  Layer(Layer&& other) noexcept;
  Layer& operator=(Layer&& other) noexcept;

  const std::string name() const;

  void set_name(const std::string name);

  void clear_primspecs();

  // Check if `primname` exists in root Prims?
  bool has_primspec(const std::string &primname) const;

  ///
  /// Add PrimSpec(copy PrimSpec instance).
  ///
  /// @return false when `name` already exists in `primspecs`, `name` is empty
  /// string or `name` contains invalid character to be used in Prim
  /// element_name.
  ///
  bool add_primspec(const std::string &name, const PrimSpec &ps);

  ///
  /// Add PrimSpec.
  ///
  /// @return false when `name` already exists in `primspecs`, `name` is empty
  /// string or `name` contains invalid character to be used in Prim
  /// element_name.
  ///
  bool emplace_primspec(const std::string &name, PrimSpec &&ps);

  ///
  /// Replace PrimSpec(copy PrimSpec instance)
  ///
  /// @return false when `name` does not exist in `primspecs`, `name` is empty
  /// string or `name` contains invalid character to be used in Prim
  /// element_name.
  ///
  bool replace_primspec(const std::string &name, const PrimSpec &ps);

  ///
  /// Replace PrimSpec
  ///
  /// @return false when `name` does not exist in `primspecs`, `name` is empty
  /// string or `name` contains invalid character to be used in Prim
  /// element_name.
  ///
  bool replace_primspec(const std::string &name, PrimSpec &&ps);

  const std::unordered_map<std::string, PrimSpec> &primspecs() const;

  std::unordered_map<std::string, PrimSpec> &primspecs();

  const LayerMetas &metas() const;
  LayerMetas &metas();

  bool has_unresolved_references() const;

  bool has_unresolved_payload() const;

  bool has_unresolved_variant() const;

  bool has_over_primspec() const;

  bool has_class_primspec() const;

  bool has_unresolved_inherits() const;

  bool has_unresolved_specializes() const;

  ///
  /// Check if PrimSpec tree contains any `references` and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any (unresolved) `references`. false if not.
  ///
  bool check_unresolved_references(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Check if PrimSpec tree contains any `payload` and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any (unresolved) `payload`. false if not.
  ///
  bool check_unresolved_payload(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Check if PrimSpec tree contains any `variant` and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any (unresolved) `variant`. false if not.
  ///
  bool check_unresolved_variant(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Check if PrimSpec tree contains any `specializes` and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any (unresolved) `specializes`. false if not.
  ///
  bool check_unresolved_specializes(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Check if PrimSpec tree contains any `inherits` and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any (unresolved) `inherits`. false if not.
  ///
  bool check_unresolved_inherits(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Check if PrimSpec tree contains any Prim with `over` specifier and cache the result.
  ///
  /// @param[in] max_depth Maximum PrimSpec traversal depth.
  /// @returns true if PrimSpec tree contains any Prim with `over` specifier. false if not.
  ///
  bool check_over_primspec(const uint32_t max_depth = 1024 * 1024) const;

  ///
  /// Find a PrimSpec at `path` and returns it if found.
  ///
  /// @param[in] path PrimSpec path to find.
  /// @param[out] ps Pointer to PrimSpec pointer
  /// @param[out] err Error message
  ///
  bool find_primspec_at(const Path &path, const PrimSpec **ps, std::string *err) const;


  ///
  /// Set state for AssetResolution in the subsequent composition operation.
  ///
  void set_asset_resolution_state(
    const std::string &cwp, const std::vector<std::string> &search_paths, void *userdata=nullptr);

  void get_asset_resolution_state(
    std::string &cwp, std::vector<std::string> &search_paths, void *&userdata);

  const std::string get_current_working_path() const;

  const std::vector<std::string> get_asset_search_paths() const;

  ///
  /// Estimate memory usage of this Layer in bytes.
  /// This includes memory used by PrimSpecs, metadata, and internal data structures.
  ///
  /// @return Estimated memory usage in bytes
  ///
  size_t estimate_memory_usage() const;

 private:
  std::unique_ptr<LayerImpl> _impl;

};

}  // namespace lightusd
