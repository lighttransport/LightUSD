// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// String interning pool for deduplicating repeated strings in USD scenes.
// Provides O(1) lookup via pointer comparison after first interning.
//

#pragma once

#include <deque>
#include <initializer_list>
#include <string>
#include "tiny-hashmap.hh"

namespace lightusd {

class StringPool {
public:
  // Intern a string: returns a stable const char* pointer.
  // If the string is already in the pool, returns the existing pointer.
  // Otherwise, allocates a copy and returns its pointer.
  const char *intern(const std::string &s) {
    auto it = _map.find(s);
    if (it != _map.end()) {
      return it->second;
    }
    _storage.push_back(s);
    const char *ptr = _storage.back().c_str();
    _map[s] = ptr;
    return ptr;
  }

  const char *intern(const char *s) {
    if (!s) return nullptr;
    return intern(std::string(s));
  }

  // Fast lookup without interning (returns nullptr if not found)
  const char *lookup(const std::string &s) const {
    auto it = _map.find(s);
    return (it != _map.end()) ? it->second : nullptr;
  }

  const char *lookup(const char *s) const {
    if (!s) return nullptr;
    return lookup(std::string(s));
  }

  // Pre-intern a batch of common strings
  void pre_intern(std::initializer_list<const char *> strings) {
    for (const char *s : strings) {
      intern(s);
    }
  }

  // Stats
  size_t size() const { return _map.size(); }

  static StringPool &instance() {
    static StringPool pool;
    return pool;
  }

private:
  StringPool() = default;
  StringPool(const StringPool &) = delete;
  StringPool &operator=(const StringPool &) = delete;

  // Map: string -> pointer into _storage
  HashMap<std::string, const char *> _map;
  // Backing storage: all interned strings live here. std::deque keeps
  // references/pointers to existing elements stable when appending.
  std::deque<std::string> _storage;
};

}  // namespace lightusd

// Pre-intern common USD strings. Call once at library init.
// Safe to call multiple times (idempotent).
namespace lightusd {
inline void PreInternCommonStrings() {
  StringPool::instance().pre_intern({
    // Metadata keys
    "active", "hidden", "kind", "documentation", "comment",
    "displayName", "sceneName", "instanceable", "customData",
    "sdrMetadata", "assetInfo", "clips", "interpolation",
    "elementSize", "weight", "colorSpace", "connectability",
    "renderType", "outputName", "bindMaterialAs",
    // Prim type names
    "Xform", "Mesh", "Material", "Shader", "Scope",
    "Cube", "Sphere", "Cylinder", "Capsule", "Cone",
    "BasisCurves", "Points", "Camera", "PointInstancer", "GeomSubset",
    // Common property names
    "points", "normals", "faceVertexCounts", "faceVertexIndices",
    "visibility", "purpose", "extent",
    // Material/shader
    "material:binding", "info:id", "UsdPreviewSurface",
    "UsdUVTexture", "UsdPrimvarReader_float2",
    "inputs:diffuseColor", "inputs:emissiveColor",
    "inputs:metallic", "inputs:roughness", "inputs:opacity",
    "inputs:ior", "inputs:normal", "inputs:st", "inputs:varname",
    // Tokens
    "vertex", "faceVarying", "uniform", "constant",
    "model", "group", "assembly", "component",
    "weakerThanDescendants",
    // Composition
    "references", "payload", "inherits", "specializes",
    "variantSets", "variants",
    // USDC field names
    "primChildren", "properties",
  });
}
}  // namespace lightusd
