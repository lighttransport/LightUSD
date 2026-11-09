// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Syoyo Fujita.
//
// schema-registry.hh - Schema property registry for variability resolution
//
// AOUSD Core Spec 12.2.3: Variability is determined from the prim definition
// (schema). When not explicitly authored, the schema-defined variability
// applies as the weakest-opinion fallback.
//
#pragma once

#include <string>

#include "prim-enums.hh"  // Variability
#include "../tiny-hashmap.hh"

namespace lightusd {

struct SchemaPropertyInfo {
  Variability variability{Variability::Varying};
  std::string type_name;  // e.g. "bool", "token", etc.
  std::string default_value;  // String representation of fallback default (empty = no fallback)
};

///
/// Singleton registry mapping (prim_type, property_name) -> SchemaPropertyInfo.
///
/// Used during composition to apply schema-defined variability when not
/// explicitly authored. Per AOUSD Core Spec 12.2.3, variability comes from
/// the prim definition and is resolved as the weakest opinion.
///
class SchemaRegistry {
 public:
  static SchemaRegistry &instance() {
    static SchemaRegistry reg;
    return reg;
  }

  /// Look up schema-defined property info.
  /// @return pointer to info if found, nullptr otherwise.
  const SchemaPropertyInfo *find(const std::string &prim_type,
                                  const std::string &prop_name) const {
    // Try exact (prim_type, prop_name) first
    std::string key = prim_type + ":" + prop_name;
    if (const SchemaPropertyInfo *info = _registry.find_value_ptr(key)) {
      return info;
    }

    // Fall back to wildcard ("*", prop_name) for properties common to all types
    key = "*:" + prop_name;
    return _registry.find_value_ptr(key);
  }

  /// Register a schema property.
  void register_property(const std::string &prim_type,
                          const std::string &prop_name,
                          const SchemaPropertyInfo &info) {
    std::string key = prim_type + ":" + prop_name;
    _registry[key] = info;
  }

 private:
  SchemaRegistry() { register_builtins(); }

  SchemaRegistry(const SchemaRegistry &) = delete;
  SchemaRegistry &operator=(const SchemaRegistry &) = delete;

  void register_builtins() {
    // UsdGeom common uniform properties (wildcard = all gprim types)
    // default_value is the schema-defined fallback per UsdGeom spec
    register_property("*", "doubleSided",
                      {Variability::Uniform, "bool", "false"});
    register_property("*", "orientation",
                      {Variability::Uniform, "token", "rightHanded"});
    register_property("*", "purpose",
                      {Variability::Uniform, "token", "default"});

    // UsdGeomMesh
    register_property("Mesh", "subdivisionScheme",
                      {Variability::Uniform, "token", "catmullClark"});
    register_property("Mesh", "interpolateBoundary",
                      {Variability::Uniform, "token", "edgeAndCorner"});
    register_property("Mesh", "faceVaryingLinearInterpolation",
                      {Variability::Uniform, "token", "cornersPlus1"});
    register_property("Mesh", "triangleSubdivisionRule",
                      {Variability::Uniform, "token", "catmullClark"});

    // UsdGeomBasisCurves
    register_property("BasisCurves", "type",
                      {Variability::Uniform, "token", "cubic"});
    register_property("BasisCurves", "basis",
                      {Variability::Uniform, "token", "bezier"});
    register_property("BasisCurves", "wrap",
                      {Variability::Uniform, "token", "nonperiodic"});

    // UsdGeomSubset
    register_property("GeomSubset", "elementType",
                      {Variability::Uniform, "token", "face"});
    register_property("GeomSubset", "familyName",
                      {Variability::Uniform, "token", ""});

    // UsdGeomCamera
    register_property("Camera", "projection",
                      {Variability::Varying, "token", "perspective"});
    register_property("Camera", "stereoRole",
                      {Variability::Uniform, "token", "mono"});

    // UsdGeom primitives (axis alignment)
    register_property("Capsule", "axis",
                      {Variability::Uniform, "token", "Y"});
    register_property("Cylinder", "axis",
                      {Variability::Uniform, "token", "Y"});
    register_property("Cone", "axis",
                      {Variability::Uniform, "token", "Y"});

    // UsdShade
    register_property("Material", "surface",
                      {Variability::Uniform, "token", ""});
    register_property("Material", "displacement",
                      {Variability::Uniform, "token", ""});
    register_property("Material", "volume",
                      {Variability::Uniform, "token", ""});

    // UsdSkel
    register_property("Skeleton", "joints",
                      {Variability::Uniform, "token[]", ""});
    register_property("Skeleton", "jointNames",
                      {Variability::Uniform, "token[]", ""});
    register_property("SkelRoot", "purpose",
                      {Variability::Uniform, "token", "default"});
  }

  lightusd::HashMap<std::string, SchemaPropertyInfo> _registry;
};

}  // namespace lightusd
