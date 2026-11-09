// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
#pragma once

#include "lightusd.hh"
#include "stream-reader.hh"

#include "ascii-parser.hh"
//#include "asset-resolution.hh"

namespace lightusd {

namespace usda {

struct USDAReaderConfig {
  bool allow_unknown_prims{true};
  bool allow_unknown_shader{true};
  bool allow_unknown_apiSchema{true};
  bool strict_allowedToken_check{false};
  bool strict_shader_type_check{false}; // Shader input/output type conformance(false: warn, true: error)
  size_t max_memory_limit_in_mb{512}; // Fail-closed default; 0 disables the cap.

  // MaterialX validation options
  bool validate_mtlx_connection_types{false};
  bool validate_mtlx_info_id{false};
  bool validate_mtlx_connection_targets{false};
  bool validate_mtlx_duplicate_names{false};
  bool validate_mtlx_index_bounds{false};
  bool strict_mtlx_check{false};  // Enable all above
  bool error_detail{false};
};

///
/// Test if input file is USDA format.
///
bool IsUSDA(const std::string &filename, size_t max_filesize = 0);

class USDAReader {
 public:
  USDAReader(StreamReader *sr);
  ~USDAReader();

  // Prevent copy/move — raw owning pointer (_impl).
  USDAReader(const USDAReader &) = delete;
  USDAReader &operator=(const USDAReader &) = delete;
  USDAReader(USDAReader &&) = delete;
  USDAReader &operator=(USDAReader &&) = delete;


  ///
  /// Base filesystem directory to search asset files.
  ///
  void set_base_dir(const std::string &base_dir);
  void SetBaseDir(const std::string &base_dir) { // Deprecated
    set_base_dir(base_dir);
  }

  ///
  /// Set source filename for displaying surrounding context in error messages
  ///
  void set_filename(const std::string &filename);
  void SetFilename(const std::string &filename) { // Deprecated
    set_filename(filename);
  }

  ///
  /// Set AssetResolution resolver.
  ///
  //void set_asset_resolution_resolver(const AssetResolutionResolver &arr);

  ///
  /// Set reader option
  ///
  void set_reader_config(const USDAReaderConfig &config);

  ///
  /// Set progress callback for monitoring parsing progress.
  ///
  /// @param[in] callback Function to call during parsing to report progress
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetProgressCallback(std::function<bool(float progress, void *userptr)> callback, void *userptr = nullptr);

  ///
  /// Get reader option
  ///
  const USDAReaderConfig get_reader_config() const; // NOTE: Not returning reference to avoid static memory allocation.

  ///
  /// Check if header data is USDA
  ///
  bool check_header();
  bool CheckHeader() { // Deprecated
    return check_header();
  }

  ///
  /// Reader entry point
  ///
  /// `as_primspec` : Create PrimSpec instead of concrete(typed) Prim. Set true if you want to do composition
  ///
  bool read(uint32_t load_state = static_cast<uint32_t>(LoadState::Toplevel), bool as_primspec = false);
  bool Read(LoadState state = LoadState::Toplevel, bool as_primspec = false) { // Deprecated
    uint32_t ustate = static_cast<uint32_t>(state);
    return read(ustate, as_primspec);
  }

  ///
  /// Get error message(when reading USDA failed)
  ///
  std::string get_error();
  std::string GetError() { // Deprecated
    return get_error();
  }

  ///
  /// Get warning message.
  ///
  std::string get_warning();
  std::string GetWarning() { // Deprecated
    return get_warning();
  }

  ///
  /// Get read USD scene data as Layer
  /// Must be called after `read`
  ///
  /// FIXME: Currently concrete(typed) Prims are not included in destination Layer.
  /// If you use this function, you'll need to invoke `read` with `as_primspec=true`.
  ///
  ///
  bool get_as_layer(Layer *layer);
  bool GetAsLayer(Layer *layer) { // Deprecated
    return get_as_layer(layer);
  }

  ///
  /// Reconstruct Stage from loaded USD scene data.
  /// Must be called after `Read`
  ///
  bool reconstruct_stage();
  bool ReconstructStage() { // Deprecated
    return reconstruct_stage();
  }

  ///
  /// Get as stage(scene graph). Must call `ReconstructStage` beforehand.
  ///
  const Stage& get_stage() const;
  const Stage& GetStage() const { // Deprecated
    return get_stage();
  }

 private:
#if defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)
  Stage *_empty_stage{nullptr};
#else
  class Impl;
  Impl *_impl{nullptr};
#endif

};

} // namespace usda

} // namespace lightusd
