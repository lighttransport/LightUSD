// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// prim-enums.hh - Enumeration types for USD primitives
//
#pragma once

#include <string>
#include "nonstd/optional.hpp"
#include "value-types.hh"

namespace lightusd {

// SpecType enum must be same order with pxrUSD's SdfSpecType(since enum value
// is stored in Crate directly)
enum class SpecType {
  Unknown = 0,  // must be 0
  Attribute,
  Connection,
  Expression,
  Mapper,
  MapperArg,
  Prim,
  PseudoRoot,
  Relationship,
  RelationshipTarget,
  Variant,
  VariantSet,
  Invalid,  // or NumSpecTypes
};

enum class Orientation {
  RightHanded,  // 0
  LeftHanded,
  Invalid
};

enum class Visibility {
  Inherited,  // "inherited" (default)
  Invisible,  // "invisible"
  Invalid
};

enum class Purpose {
  Default,  // 0
  Render,   // "render"
  Proxy,    // "proxy"
  Guide,    // "guide"
};

//
// USDZ extension: sceneLibrary
// https://developer.apple.com/documentation/arkit/usdz_schemas_for_ar/scenelibrary
//

enum class Kind {
  Model,
  Group,
  Assembly,
  Component,
  Subcomponent,
  SceneLibrary,  // USDZ extension
  UserDef, // Unknown or user defined Kind
  Invalid
};

// Attribute interpolation
enum class Interpolation {
  Constant,     // "constant"
  Uniform,      // "uniform"
  Varying,      // "varying"
  Vertex,       // "vertex"
  FaceVarying,  // "faceVarying"
  Invalid
};

// NOTE: Attribute cannot have ListEdit qualifier
enum class ListEditQual {
  ResetToExplicit,  // "unqualified"(no qualifier)
  Append,           // "append"
  Add,              // "add"
  Delete,           // "delete"
  Prepend,          // "prepend"
  Order,            // "order"
  Invalid
};

enum class Axis { X, Y, Z, Invalid };

// metrics(UsdGeomLinearUnits in pxrUSD)
// To avoid linkage error, defined as static constexpr function.
struct Units {
  static constexpr double Nanometers = 1e-9;
  static constexpr double Micrometers = 1e-6;
  static constexpr double Millimeters = 0.001;
  static constexpr double Centimeters = 0.01;
  static constexpr double Meters = 1.0;
  static constexpr double Kilometers = 1000;
  static constexpr double LightYears = 9.4607304725808e15;
  static constexpr double Inches = 0.0254;
  static constexpr double Feet = 0.3048;
  static constexpr double Yards = 0.9144;
  static constexpr double Miles = 1609.344;
};

// For PrimSpec
enum class Specifier {
  Def,  // 0
  Over,
  Class,
  Invalid
};

enum class Permission {
  Public,  // 0
  Private,
  Invalid
};

enum class Variability {
  Varying,  // 0
  Uniform,
  Config,
  Invalid
};

// String conversion functions
nonstd::optional<Interpolation> InterpolationFromString(const std::string &v);
nonstd::optional<Orientation> OrientationFromString(const std::string &v);
nonstd::optional<Kind> KindFromString(const std::string &v);

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Specifier, "specifier", TYPE_ID_SPECIFIER, 1);
DEFINE_TYPE_TRAIT(Permission, "permission", TYPE_ID_PERMISSION, 1);
DEFINE_TYPE_TRAIT(Variability, "variability", TYPE_ID_VARIABILITY, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
