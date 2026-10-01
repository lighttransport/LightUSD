// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Light Transport Entertainment, Inc.
#include <cassert>
#include <algorithm>
#include <iostream>
#include <limits>

#include "asset-resolution.hh"
#include "common-macros.inc"
#include "io-util.hh"
#include "value-pprint.hh"
#include "str-util.hh"
#include "logger.hh"
#include "tiny-format.hh"

namespace lightusd {

std::string AssetResolutionResolver::search_paths_str() const {
  std::string str;

  str += "[ ";
  for (size_t i = 0; i < _search_paths.size(); i++) {
    if (i > 0) {
      str += ", ";
    }
    // TODO: Escape character?
    str += _search_paths[i];
  }
  str += " ]";
  return str;
}

bool AssetResolutionResolver::find(const std::string &assetPath) const {
  DCOUT("search_paths = " << _search_paths);
  DCOUT("assetPath = " << assetPath);

  std::string ext = io::GetFileExtension(assetPath);

  if (_asset_resolution_handlers.count(ext)) {
    if (_asset_resolution_handlers.at(ext).resolve_fun && _asset_resolution_handlers.at(ext).size_fun) {
      std::string resolvedPath;
      std::string err;

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at(ext).userdata;

      int ret = _asset_resolution_handlers.at(ext).resolve_fun(assetPath.c_str(), _search_paths, &resolvedPath, &err, userdata);
      if (ret != 0) {
        return false;
      }

      uint64_t sz{0};
      ret = _asset_resolution_handlers.at(ext).size_fun(resolvedPath.c_str(), &sz, &err, userdata);
      if (ret != 0) {
        return false;
      }

      return sz > 0;

    } else {
      DCOUT("Either Resolve function or Size function is nullptr. Fallback to wildcard handler or built-in file handler.");
    }
  }

  // wildcard
  if (_asset_resolution_handlers.count("*")) {
    if (_asset_resolution_handlers.at("*").resolve_fun && _asset_resolution_handlers.at("*").size_fun) {
      std::string resolvedPath;
      std::string err;

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at("*").userdata;

      int ret = _asset_resolution_handlers.at("*").resolve_fun(assetPath.c_str(), _search_paths, &resolvedPath, &err, userdata);
      if (ret != 0) {
        return false;
      }

      uint64_t sz{0};
      ret = _asset_resolution_handlers.at("*").size_fun(resolvedPath.c_str(), &sz, &err, userdata);
      if (ret != 0) {
        return false;
      }

      return sz > 0;

    }

    return false;
  }

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
  LIGHTUSD_LOG_E("Failed to find asssetPath: " << assetPath);

  return false;
#else
  // default fallback: File-based
  return resolve_literal(assetPath).size();
#endif

}

std::string AssetResolutionResolver::resolve(
    const std::string &assetPath) const {
  return resolve(assetPath, /* used_suffix_fallback */ nullptr);
}

std::string AssetResolutionResolver::resolve(
    const std::string &assetPath, bool *used_suffix_fallback) const {

  if (used_suffix_fallback) {
    (*used_suffix_fallback) = false;
  }

  std::string resolvedPath = resolve_literal(assetPath);

  // A non-empty `resolvedPath` does not guarantee existence when a custom
  // handler is registered(a handler may echo the input back on a miss), so
  // verify with find() in that case before triggering the fallback.
  bool literal_ok{false};
  if (resolvedPath.empty()) {
    literal_ok = false;
  } else if (_asset_resolution_handlers.empty()) {
    literal_ok = true;  // file-based resolution verified existence already.
  } else {
    literal_ok = find(assetPath);
  }

  if (!literal_ok && _enable_suffix_fallback) {
    // Rebase a path authored against another machine's layout(e.g.
    // `../../../../../USD_Exports/Scene/Assets/mesh.usd` or
    // `F:/USD_Exports/Scene/Assets/mesh.usd` from an UnrealEngine export) by
    // retrying progressively shorter path suffixes against the search paths.
    const std::vector<std::string> candidates =
        io::AssetPathSuffixCandidates(assetPath);
    for (const auto &candidate : candidates) {
      if (_asset_resolution_handlers.empty()) {
        std::string rpath = resolve_literal(candidate);
        if (rpath.size()) {
          if (used_suffix_fallback) {
            (*used_suffix_fallback) = true;
          }
          resolvedPath = rpath;
          break;
        }
      } else if (find(candidate)) {
        if (used_suffix_fallback) {
          (*used_suffix_fallback) = true;
        }
        resolvedPath = resolve_literal(candidate);
        break;
      }
    }
  }

  // On total miss, `resolvedPath` is the literal result unchanged(preserves
  // legacy echo-back semantics of custom handlers).
  return resolvedPath;
}

std::string AssetResolutionResolver::resolve_literal(
    const std::string &assetPath) const {

  std::string ext = io::GetFileExtension(assetPath);

  std::string resolvedPath;
  bool resolved{false};

  if (_asset_resolution_handlers.count(ext)) {
    if (_asset_resolution_handlers.at(ext).resolve_fun) {
      std::string err;

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at(ext).userdata;

      int ret = _asset_resolution_handlers.at(ext).resolve_fun(assetPath.c_str(), _search_paths, &resolvedPath, &err, userdata);
      if (ret != 0) {
        resolvedPath = std::string();

      }
      resolved = true;

    } else {
      DCOUT("Resolve function is nullptr. Fallback to wildcard handler or built-in file handler.");
    }
  }

  if (!resolved && _asset_resolution_handlers.count("*")) {
    if (_asset_resolution_handlers.at("*").resolve_fun) {
      //std::string resolvedPath;
      std::string err;

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at("*").userdata;

      int ret = _asset_resolution_handlers.at("*").resolve_fun(assetPath.c_str(), _search_paths, &resolvedPath, &err, userdata);
      if (ret != 0) {
        resolvedPath = std::string();
      }

      resolved = true;
    }
  }

  if (!resolved) {
    //DCOUT("cwd = " << _current_working_path);
    //DCOUT("search_paths = " << _search_paths);
    //DCOUT("assetPath = " << assetPath);

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    LIGHTUSD_LOG_E("Failed to resolve asssetPath: " << assetPath);
#else

    auto cache_it = _cached_resolved_paths.find(assetPath);
    if (cache_it != _cached_resolved_paths.end()) {
      // Return through the NRVO variable (mixed named returns defeat copy
      // elision and trip clang's -Werror,-Wnrvo).
      resolvedPath = cache_it->second;
      return std::string(resolvedPath);
    }

    std::string rpath;
    if ((_current_working_path == ".") || (_current_working_path == "./")) {
      rpath = io::FindFile(assetPath, {});
    } else {
      rpath = io::FindFile(assetPath, {_current_working_path});
    }

    if (rpath.size()) {
      resolvedPath = rpath;
    } else {
      resolvedPath = io::FindFile(assetPath, _search_paths);
    }

    _cached_resolved_paths[assetPath] = resolvedPath;
#endif
  }

  return std::string(resolvedPath);

}

bool AssetResolutionResolver::open_asset(const std::string &resolvedPath, const std::string &assetPath,
                  Asset *asset_out, std::string *warn, std::string *err) const {
  return open_asset(resolvedPath, assetPath, asset_out, warn, err,
                    (std::numeric_limits<size_t>::max)());
}
bool AssetResolutionResolver::open_asset(const std::string& resolvedPath, const std::string& assetPath,
                  Asset* asset_out, std::string* warn, std::string* err, size_t call_limit) const {

  if (!asset_out) {
    if (err) {
      (*err) = "`asset` arg is nullptr.";
    }
    return false;
  }

  std::atomic<uint32_t> *open_fd_counter = &_open_file_descriptors;
  struct FileDescriptorBudgetGuard {
    std::atomic<uint32_t> *counter{nullptr};
    bool acquired{false};

    FileDescriptorBudgetGuard(std::atomic<uint32_t> *c, uint32_t limit,
                              const std::string &path, std::string *e)
        : counter(c) {
      uint32_t cur = counter->load();
      while (true) {
        if (cur >= limit) {
          if (e) {
            (*e) += fmt::format(
                "Asset resolver file descriptor limit reached while opening "
                "`{}` ({} / {}).\n",
                path, cur, limit);
          }
          return;
        }
        if (counter->compare_exchange_weak(cur, cur + 1)) {
          acquired = true;
          return;
        }
      }
    }

    ~FileDescriptorBudgetGuard() {
      if (acquired) {
        counter->fetch_sub(1);
      }
    }
  };

  FileDescriptorBudgetGuard fd_guard(open_fd_counter,
                                     get_max_file_descriptors(), resolvedPath,
                                     err);
  if (!fd_guard.acquired) {
    return false;
  }

  DCOUT("Opening asset: " << resolvedPath);

  (void)assetPath;
  (void)warn;

  std::string ext = io::GetFileExtension(resolvedPath);
  constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;
  constexpr uint64_t kMaxAssetBytesInMiB =
      (std::numeric_limits<uint64_t>::max)() / kBytesPerMiB;
  const uint64_t max_asset_bytes =
      (static_cast<uint64_t>(_max_asset_bytes_in_mb) > kMaxAssetBytesInMiB)
          ? (std::numeric_limits<uint64_t>::max)()
          : static_cast<uint64_t>(_max_asset_bytes_in_mb) * kBytesPerMiB;
  const uint64_t max_addressable_asset_bytes =
      (std::min)(max_asset_bytes,
                 static_cast<uint64_t>(call_limit));

  if (_asset_resolution_handlers.count(ext)) {
    if (_asset_resolution_handlers.at(ext).size_fun && _asset_resolution_handlers.at(ext).read_fun) {

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at(ext).userdata;

      // Get asset size.
      uint64_t sz{0};
      int ret = _asset_resolution_handlers.at(ext).size_fun(resolvedPath.c_str(), &sz, err, userdata);
      if (ret != 0) {
        if (err) {
          (*err) += "Get size of asset through handler failed.\n";
        }
        return false;
      }

      if (sz > max_addressable_asset_bytes) {
        if (err) {
          (*err) += fmt::format("Asset `{}` exceeds max bytes ({} > {}).\n",
                                resolvedPath, sz,
                                max_addressable_asset_bytes);
        }
        return false;
      }

      // `sz` is now bounded by both the user limit and addressable memory.

      DCOUT("asset_size: " << sz);

      lightusd::Asset asset;
      asset.resize(size_t(sz));

      uint64_t read_size{0};

      ret = _asset_resolution_handlers.at(ext).read_fun(resolvedPath.c_str(), /* req_size */asset.size(), asset.data(), &read_size, err, userdata);

      if (ret != 0) {
        if (err) {
          (*err) += "Read asset through handler failed.\n";
        }
        return false;
      }

      if (read_size > asset.size()) {
        if (err) {
          (*err) += "Read asset reported larger size than requested.\n";
        }
        return false;
      }

      if (read_size < sz) {
        asset.resize(size_t(read_size));
        // May optimize memory usage
        asset.shrink_to_fit();
      }

      (*asset_out) = std::move(asset);

      return true;
    } else {
      DCOUT("Resolve function is nullptr. Fallback to built-in file handler.");
    }
  }

  if (_asset_resolution_handlers.count("*")) {
    if (_asset_resolution_handlers.at("*").size_fun && _asset_resolution_handlers.at("*").read_fun) {

      // Use custom handler's userdata
      void *userdata = _asset_resolution_handlers.at("*").userdata;

      // Get asset size.
      uint64_t sz{0};
      int ret = _asset_resolution_handlers.at("*").size_fun(resolvedPath.c_str(), &sz, err, userdata);
      if (ret != 0) {
        if (err) {
          (*err) += "Get size of asset through handler failed.\n";
        }
        return false;
      }

      if (sz > max_addressable_asset_bytes) {
        if (err) {
          (*err) += fmt::format("Asset `{}` exceeds max bytes ({} > {}).\n",
                                resolvedPath, sz,
                                max_addressable_asset_bytes);
        }
        return false;
      }

      // `sz` is now bounded by both the user limit and addressable memory.

      DCOUT("asset_size: " << sz);

      lightusd::Asset asset;
      asset.resize(size_t(sz));

      uint64_t read_size{0};

      ret = _asset_resolution_handlers.at("*").read_fun(resolvedPath.c_str(), /* req_size */asset.size(), asset.data(), &read_size, err, userdata);

      if (ret != 0) {
        if (err) {
          (*err) += "Read asset through handler failed.\n";
        }
        return false;
      }

      if (read_size > asset.size()) {
        if (err) {
          (*err) += "Read asset reported larger size than requested.\n";
        }
        return false;
      }

      if (read_size < sz) {
        asset.resize(size_t(read_size));
        // May optimize memory usage
        asset.shrink_to_fit();
      }

      (*asset_out) = std::move(asset);

      return true;
    }

    return false;
  }

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
  if (err) {
    (*err) += "(wasm) Open asset failed.\n";
  }

  return false;
#else

  // Default: read from a file.
  std::vector<uint8_t> data;
  const size_t max_bytes = static_cast<size_t>(max_addressable_asset_bytes);
  if (!io::ReadWholeFile(&data, err, resolvedPath, max_bytes,
                           /* userdata */ nullptr)) {

    if (err) {
      (*err) += "Open asset from a file failed.\n";
    }

    return false;
  }

  asset_out->set_data(std::move(data));

  return true;
#endif
}

}  // namespace lightusd
