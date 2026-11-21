// SPDX-License-Identifier: Apache 2.0
// Centralized security limits/policies used across loaders and converters.
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace security_policy {

// Safe defaults for untrusted input. Callers handling trusted, genuinely large
// scenes can raise these through their loader options explicitly.
constexpr size_t kDefaultInputLimitBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kDefaultAssetLimitBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kDefaultArchiveEntryCount = 65536;

// MCP limits
constexpr size_t kMCPMaxRequestBodyBytes = 16 * 1024 * 1024;
constexpr size_t kMCPMaxBase64InputBytes = 64 * 1024 * 1024;
constexpr size_t kMCPMaxBase64DecodedBytes = 48 * 1024 * 1024;

// JSON->USD conversion limits
constexpr size_t kJSONMaxBase64InputChars = 96 * 1024 * 1024;
constexpr size_t kJSONMaxDecodedBytes = 64 * 1024 * 1024;

// Raw asset reads through resolver paths (render/material/texture conversions).
// This is the *default*; the effective cap is runtime-settable (see below) so
// trusted local workflows can load legitimately-large single crates without a
// recompile, while the default security posture stays at 512MB.
constexpr size_t kResolverMaxAssetReadBytes = 512 * 1024 * 1024;

// Runtime-settable effective cap. Backed by a function-local static in an inline
// function, so there is a single instance across translation units. Defaults to
// kResolverMaxAssetReadBytes. Set via SetMaxAssetReadBytes (e.g. from a CLI arg).
inline size_t &MaxAssetReadBytesRef() {
  static size_t v = kResolverMaxAssetReadBytes;
  return v;
}
inline size_t GetMaxAssetReadBytes() { return MaxAssetReadBytesRef(); }
inline void SetMaxAssetReadBytes(size_t n) { MaxAssetReadBytesRef() = n; }

inline bool EstimateBase64DecodedSize(const std::string &data,
                                      size_t *decoded_size) {
  if (!decoded_size) {
    return false;
  }
  if ((data.size() % 4) != 0) {
    return false;
  }

  size_t padding = 0;
  if (!data.empty() && data[data.size() - 1] == '=') {
    padding++;
    if ((data.size() > 1) && (data[data.size() - 2] == '=')) {
      padding++;
    }
  }

  size_t q = data.size() / 4;
  if (q > ((std::numeric_limits<size_t>::max)() / 3)) {
    return false;
  }
  size_t raw = q * 3;
  if (raw < padding) {
    return false;
  }

  *decoded_size = raw - padding;
  return true;
}

// Validates an asset path as "relative and contained":
//   - must not be empty
//   - must not start with '/' or a Windows drive (e.g. "C:")
//   - must not contain any '..' segment (unless `allow_parent_refs` is set)
// On success, writes the slash-normalized path (backslashes -> '/', empty
// and '.' segments removed) into *out and returns true.
//
// When `allow_parent_refs` is true, '..' segments are permitted: a "<seg>/.."
// pair is collapsed lexically, and any leading '..' that cannot be collapsed is
// preserved in the output. POSIX-absolute paths are still rejected, but a
// Windows drive prefix (e.g. "F:/USD_Exports/...", as authored by UnrealEngine
// USD exports) is demoted to a relative path by stripping the drive — the
// resolver's suffix fallback then rebases it onto the local search paths.
// Resolving the surviving '..' (against a base/search directory) is then the
// asset resolver's responsibility — appropriate when a custom, sandboxed
// resolver (e.g. an in-memory or fetch-backed handler) controls what is
// reachable, where USD's legitimate parent-directory references (e.g.
// `../common/foo.usd`) must work.
inline bool ValidateAndNormalizeAssetPath(const std::string &path,
                                          std::string *out,
                                          bool allow_parent_refs) {
  if (!out) {
    return false;
  }
  if (path.empty()) {
    return false;
  }

  std::string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');

  // Reject paths containing null bytes — they can confuse downstream
  // C string consumers (e.g. fopen, stat) and are never legitimate
  // in asset paths.
  if (p.find('\0') != std::string::npos) {
    return false;
  }

  if (p.size() >= 2 &&
      ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':') {
    if (!allow_parent_refs) {
      return false;
    }
    // Demote drive-absolute to relative for resolver rebasing.
    p = p.substr(2);
    while (!p.empty() && p[0] == '/') {
      p = p.substr(1);
    }
    if (p.empty()) {
      return false;
    }
  }

  if (!p.empty() && p[0] == '/') {
    return false;
  }

  std::stringstream ss(p);
  std::string part;
  std::vector<std::string> parts;
  while (std::getline(ss, part, '/')) {
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      if (!allow_parent_refs) {
        return false;
      }
      // Collapse "<seg>/.." lexically; preserve a leading ".." that has no
      // preceding segment to pop (the resolver resolves it against its base).
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else {
        parts.push_back("..");
      }
      continue;
    }
    parts.push_back(std::move(part));
  }

  if (parts.empty()) {
    return false;
  }

  std::string result;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) {
      result.push_back('/');
    }
    result += parts[i];
  }

  *out = std::move(result);
  return true;
}

// Default (strict) overload: '..' segments are rejected.
inline bool ValidateAndNormalizeAssetPath(const std::string &path,
                                          std::string *out) {
  return ValidateAndNormalizeAssetPath(path, out, /* allow_parent_refs */ false);
}

// bool-only convenience wrapper.
inline bool IsSafeRelativeAssetPath(const std::string &path) {
  std::string discard;
  return ValidateAndNormalizeAssetPath(path, &discard);
}

} // namespace security_policy
} // namespace lightusd
