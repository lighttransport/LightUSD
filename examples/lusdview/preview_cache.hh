// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lightusd-cpp.hh"

namespace lusdview {

enum class PreviewCacheMode { Off, Auto, Refresh };

struct PreviewCacheOptions {
  PreviewCacheMode mode{PreviewCacheMode::Off};
  std::string directory;
  size_t maxBytes{size_t(8) << 30};
  bool timing{false};
};

struct PreviewCacheLookup {
  bool hit{false};
  std::string reason;
  std::string key;
  lightusd::api::Stage stage;
};

std::string DefaultPreviewCacheDirectory();
std::string PreviewCacheFingerprint(const std::string& root,
                                    const std::string& compositionOptions);
PreviewCacheLookup LoadPreviewCache(const PreviewCacheOptions& options,
                                    const std::string& root,
                                    const std::string& fingerprint);
bool StorePreviewCache(const PreviewCacheOptions& options,
                       const std::string& root,
                       const std::string& fingerprint,
                       const lightusd::api::Stage& preview,
                       const std::vector<std::string>& dependencies,
                       std::string* reason);

}  // namespace lusdview
