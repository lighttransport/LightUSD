// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file usdLux.hh
/// @brief USD Lighting schema definitions
///
/// Implements light source primitives following USD's UsdLux schema.
/// Supports various light types commonly used in rendering applications.
///
/// Supported light types:
/// - SphereLight: Point light with sphere shape
/// - CylinderLight: Cylindrical area light
/// - DomeLight: Environment/HDR lighting
/// - DiskLight: Disk-shaped area light
/// - RectLight: Rectangular area light
/// - DistantLight: Directional light (sun/moon)
/// - GeometryLight: Light emitted from geometry
/// - PortalLight: Portal for environment lighting
/// - PluginLight: Custom light implementations
///
#pragma once

// Core includes (replaces monolithic prim-types.hh)
#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "nonstd/expected.hpp"
#include "core/prim-enums.hh"        // Specifier, Visibility, Purpose
#include "core/extent.hh"            // Extent
#include "core/composition-types.hh" // Reference, Payload, ListEditQual
#include "core/prim-metas.hh"       // PrimMeta
#include "core/prim.hh"             // Prim (for IsLightPrim etc.)
#include "core/animatable.hh"       // Animatable
#include "core/typed-attribute.hh"  // TypedAttribute, TypedAttributeWithFallback
#include "core/relationship.hh"     // Relationship, RelationshipProperty
#include "core/property.hh"         // Property
#include "core/xform-op.hh"         // XformOp (needed by Xformable in xform.hh)
#include "core/collection-api.hh"   // Collection
#include "core/variant-types.hh"    // VariantSet
#include "xform.hh"                 // Xformable

namespace lightusd {

constexpr auto kSphereLight = "SphereLight";
constexpr auto kCylinderLight = "CylinderLight";
constexpr auto kDomeLight = "DomeLight";
constexpr auto kDiskLight = "DiskLight";
constexpr auto kRectLight = "RectLight";
constexpr auto kDistantLight = "DistantLight";
constexpr auto kGeometryLight = "GeometryLight";
constexpr auto kPortalLight = "PortalLight";
constexpr auto kPluginLight = "PluginLight";
constexpr auto kLightFilter = "LightFilter";
constexpr auto kPluginLightFilter = "PluginLightFilter";
constexpr auto kDomeLight_1 = "DomeLight_1";

// Relationship property names
constexpr auto kGeometry = "geometry";

//
// API Schemas - Declared before light classes since they're used as optional members
//

// ShapingAPI: Light emission shaping (cone, focus, IES)
struct ShapingAPI {
  TypedAttributeWithFallback<Animatable<float>> shapingFocus{0.0f}; // inputs:shaping:focus
  TypedAttributeWithFallback<Animatable<value::color3f>> shapingFocusTint{value::color3f({0.0f, 0.0f, 0.0f})}; // inputs:shaping:focusTint
  TypedAttributeWithFallback<Animatable<float>> shapingConeAngle{90.0f}; // inputs:shaping:cone:angle (degrees)
  TypedAttributeWithFallback<Animatable<float>> shapingConeSoftness{0.0f}; // inputs:shaping:cone:softness
  TypedAttribute<Animatable<value::AssetPath>> shapingIesFile; // inputs:shaping:ies:file
  TypedAttributeWithFallback<Animatable<float>> shapingIesAngleScale{0.0f}; // inputs:shaping:ies:angleScale
  TypedAttributeWithFallback<Animatable<bool>> shapingIesNormalize{false}; // inputs:shaping:ies:normalize
};

// ShadowAPI: Shadow controls
struct ShadowAPI {
  TypedAttributeWithFallback<Animatable<bool>> shadowEnable{true}; // inputs:shadow:enable
  TypedAttributeWithFallback<Animatable<value::color3f>> shadowColor{value::color3f({0.0f, 0.0f, 0.0f})}; // inputs:shadow:color
  TypedAttributeWithFallback<Animatable<float>> shadowDistance{-1.0f}; // inputs:shadow:distance (-1 = infinite)
  TypedAttributeWithFallback<Animatable<float>> shadowFalloff{-1.0f}; // inputs:shadow:falloff (-1 = no falloff)
  TypedAttributeWithFallback<Animatable<float>> shadowFalloffGamma{1.0f}; // inputs:shadow:falloffGamma
};

// MeshLightAPI: Applied to mesh geometry to make it emit light
struct MeshLightAPI {
  // Inherits LightAPI properties
  // materialSyncMode defaults to "materialGlowTintsLight"
  TypedAttributeWithFallback<Animatable<value::token>> materialSyncMode{value::token("materialGlowTintsLight")}; // light:materialSyncMode
};

// VolumeLightAPI: Applied to volume geometry for volumetric lighting
struct VolumeLightAPI {
  // Inherits LightAPI properties
  // materialSyncMode defaults to "materialGlowTintsLight"
  TypedAttributeWithFallback<Animatable<value::token>> materialSyncMode{value::token("materialGlowTintsLight")}; // light:materialSyncMode
};

//
// Light Base Classes
//

/// LightAPI: Common attributes shared by all USD light types.
/// This base class implements UsdLuxLightAPI, UsdLuxShadowAPI, and UsdLuxShapingAPI.
/// Both BoundableLight and NonboundableLight inherit from this class.
class LightAPI {
 public:
  // Core Light API (UsdLuxLightAPI)
  TypedAttributeWithFallback<Animatable<value::color3f>> color{value::color3f({1.0f, 1.0f, 1.0f})}; // inputs:color Light energy in linear color space.
  TypedAttributeWithFallback<Animatable<float>> colorTemperature{6500.0f};  // inputs:colorTemperature
  TypedAttributeWithFallback<Animatable<float>> diffuse{1.0f}; // inputs:diffuse diffuse multiplier
  TypedAttributeWithFallback<Animatable<bool>> enableColorTemperature{false}; // inputs:enableColorTemperature
  TypedAttributeWithFallback<Animatable<float>> exposure{0.0f}; // inputs:exposure EV
  TypedAttributeWithFallback<Animatable<float>> intensity{1.0f}; // inputs:intensity
  TypedAttributeWithFallback<Animatable<bool>> normalize{false}; // inputs:normalize normalize power by the surface area of the light.
  TypedAttributeWithFallback<Animatable<float>> specular{1.0f}; // inputs:specular specular multiplier

  // Light Filter Relationships
  RelationshipProperty lightFilters; // rel light:filters - Array of filter prims to apply

  // Shadow API (UsdLuxShadowAPI)
  TypedAttributeWithFallback<Animatable<bool>> shadowEnable{true}; // bool inputs:shadow:enable = 1
  TypedAttributeWithFallback<Animatable<value::color3f>> shadowColor{value::color3f({0.0f, 0.0f, 0.0f})}; // color3f inputs:shadow:color = (0, 0, 0)
  TypedAttributeWithFallback<Animatable<float>> shadowDistance{-1.0f}; // float inputs:shadow:distance = -1
  TypedAttributeWithFallback<Animatable<float>> shadowFalloff{-1.0f}; // float inputs:shadow:falloff = -1
  TypedAttributeWithFallback<Animatable<float>> shadowFalloffGamma{1.0f}; // float inputs:shadow:falloffGamma = 1

  // Shaping API (UsdLuxShapingAPI)
  TypedAttributeWithFallback<Animatable<float>> shapingFocus{0.0f}; // float inputs:shaping:focus = 0
  TypedAttributeWithFallback<Animatable<value::color3f>> shapingFocusTint{value::color3f({0.0f, 0.0f, 0.0f})}; // color3f inputs:shaping:focusTint = (0, 0, 0)
  TypedAttributeWithFallback<Animatable<float>> shapingConeAngle{90.0f}; // float inputs:shaping:cone:angle = 90
  TypedAttributeWithFallback<Animatable<float>> shapingConeSoftness{0.0f}; // float inputs:shaping:cone:softness = 0
  TypedAttribute<Animatable<value::AssetPath>> shapingIesFile; // asset inputs:shaping:ies:file
  TypedAttributeWithFallback<Animatable<float>> shapingIesAngleScale{0.0f}; // float inputs:shaping:ies:angleScale = 0
  TypedAttributeWithFallback<Animatable<bool>> shapingIesNormalize{false}; // bool inputs:shaping:ies:normalize = false

  // LTE SpectralAPI: Spectral emission support
  // See doc/lte_spectral_api.md for specification
  TypedAttribute<std::vector<value::float2>> spectralEmission; // float2[] wavelength:emission
  // Metadata stored in attribute's customData:
  // - string interpolation: "linear" (default), "held", "cubic"
  // - string illuminantPreset: "d65", "d50", "a", "e", "f1", "f2", "f7", "f11"
  // - string unitForWavelength: "nanometers" (default), "micrometers"
};

/// BoundableLight: Base class for lights with a bounding extent (SphereLight, RectLight, etc.)
class BoundableLight : public Xformable, public Collection, public LightAPI {

 public:
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};  // Index to xform node

  TypedAttribute<Animatable<Extent>> extent; // float3[] - bounding extent
  TypedAttributeWithFallback<Animatable<Visibility>> visibility{Visibility::Inherited};
  TypedAttributeWithFallback<Purpose> purpose{Purpose::Default};

  // LightAPI attributes inherited via LightAPI base class

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;
  PrimMeta meta; // TODO: move to private

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:

  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

/// NonboundableLight: Base class for lights without a bounding extent (DistantLight, DomeLight, etc.)
/// Differences with BoundableLight: no `extent` attribute.
class NonboundableLight : public Xformable, public Collection, public LightAPI {

 public:
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};  // Index to xform node

  TypedAttributeWithFallback<Animatable<Visibility>> visibility{Visibility::Inherited};
  TypedAttributeWithFallback<Purpose> purpose{Purpose::Default};

  // LightAPI attributes inherited via LightAPI base class

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;
  PrimMeta meta; // TODO: move to private

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:

  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct SphereLight : public BoundableLight {

  TypedAttributeWithFallback<Animatable<float>> radius{0.5f}; // inputs:radius
  // ShapingAPI attributes inherited from BoundableLight

};

struct CylinderLight : public BoundableLight {

  TypedAttributeWithFallback<Animatable<float>> length{1.0f}; // inputs:length size in Y axis
  TypedAttributeWithFallback<Animatable<float>> radius{0.5f}; // inputs:radius  size in X axis

};


struct RectLight : public BoundableLight {

  TypedAttribute<Animatable<value::AssetPath>> file; // asset inputs:texture:file
  TypedAttributeWithFallback<Animatable<float>> height{1.0f}; // inputs:height size in Y axis
  TypedAttributeWithFallback<Animatable<float>> width{1.0f}; // inputs:width  size in X axis
  // ShapingAPI attributes inherited from BoundableLight

};

struct DiskLight : public BoundableLight {

  TypedAttributeWithFallback<Animatable<float>> radius{0.5f}; // inputs:radius

};

// NOTE: Make Distance xformable?
struct DistantLight : public NonboundableLight {

  TypedAttributeWithFallback<Animatable<float>> angle{0.53f}; // inputs:angle in degrees

};

struct DomeLight : public NonboundableLight {

  enum class TextureFormat {
    Automatic, // "automatic"
    Latlong, // "latlong"
    MirroredBall, // "mirroredBall"
    Angular // "angular"
  };

  // DomeLight specific
  TypedAttributeWithFallback<Animatable<float>> guideRadius{1.0e5f};
  TypedAttribute<Animatable<value::AssetPath>> file; // asset inputs:texture:file
  TypedAttributeWithFallback<Animatable<TextureFormat>> textureFormat{TextureFormat::Automatic}; // token inputs:texture:format

  // Relationships
  RelationshipProperty portals; // rel portals - portal lights for dome light
  RelationshipProperty proxyPrim; // rel proxyPrim - proxy geometry for light shape

};

// DomeLight_1: adds poleAxis for flexible orientation
struct DomeLight_1 : public NonboundableLight {

  enum class TextureFormat {
    Automatic,
    Latlong,
    MirroredBall,
    Angular
  };

  TypedAttributeWithFallback<Animatable<float>> guideRadius{1.0e5f};
  TypedAttribute<Animatable<value::AssetPath>> file;
  TypedAttributeWithFallback<Animatable<TextureFormat>> textureFormat{TextureFormat::Automatic};
  // "scene", "Y", "Z"
  TypedAttributeWithFallback<value::token> poleAxis{value::token("scene")};

  RelationshipProperty portals;
  RelationshipProperty proxyPrim;
};

// TODO: Deprecate
struct GeometryLight : public NonboundableLight {

  RelationshipProperty geometry; // `rel geometry`

};

struct PortalLight : public NonboundableLight {

  // Portal geometry relationship - defines the area where this portal applies
  RelationshipProperty geometry; // `rel geometry`

};

struct PluginLight : public Xformable, public Collection {
  // Plugin-based lights defined via shader registry
  TypedAttribute<Animatable<value::token>> shaderId; // light:shaderId
};

//
// Light Filters
//

// Base class for light filters
struct LightFilter : public Xformable {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  TypedAttributeWithFallback<Animatable<Visibility>> visibility{Visibility::Inherited};
  TypedAttributeWithFallback<Purpose> purpose{Purpose::Default};

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;
  PrimMeta meta;

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct PluginLightFilter : public LightFilter {
  TypedAttribute<Animatable<value::token>> shaderId; // light:shaderId
};

inline bool IsLightPrim(const Prim &prim) {
  return (prim.type_id() > value::TYPE_ID_LUX_BEGIN) && (prim.type_id() < value::TYPE_ID_LUX_END);
}

//
// Utility functions
//

// Convert DomeLight::TextureFormat to string
std::string to_string(const DomeLight::TextureFormat &format);
std::string to_string(const DomeLight_1::TextureFormat &format);

// Parse string to DomeLight::TextureFormat
bool DomeLight_TextureFormat_from_string(const std::string &str, DomeLight::TextureFormat *format);

// Check if a prim is a light filter
bool IsLightFilterPrim(const Prim &prim);

// Check if a light is boundable
bool IsBoundableLight(const Prim &prim);

// Check if a light is non-boundable
bool IsNonboundableLight(const Prim &prim);

//
// Light API helper functions
//

// Compute effective light color including color temperature
value::color3f ComputeEffectiveLightColor(
    const value::color3f &baseColor,
    bool enableColorTemperature,
    float colorTemperature);

// Compute light intensity from exposure (EV)
float ComputeLightIntensityFromExposure(float baseIntensity, float exposure);

// Compute final light intensity combining base intensity and exposure
float ComputeFinalLightIntensity(float baseIntensity, float exposure);

//
// Shaping API helper functions
//

// Check if a light has shaping applied (cone angle < 90 degrees or IES profile)
bool HasLightShaping(const ShapingAPI &shaping);

//
// Shadow API helper functions
//

// Check if shadows are enabled
bool AreShadowsEnabled(const ShadowAPI &shadow);

// Get effective shadow color
value::color3f GetEffectiveShadowColor(const ShadowAPI &shadow);

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

DEFINE_TYPE_TRAIT(DomeLight, kDomeLight, TYPE_ID_LUX_DOME, 1);
DEFINE_TYPE_TRAIT(CylinderLight, kCylinderLight, TYPE_ID_LUX_CYLINDER, 1);
DEFINE_TYPE_TRAIT(SphereLight, kSphereLight, TYPE_ID_LUX_SPHERE, 1);
DEFINE_TYPE_TRAIT(DiskLight, kDiskLight, TYPE_ID_LUX_DISK, 1);
DEFINE_TYPE_TRAIT(DistantLight, kDistantLight, TYPE_ID_LUX_DISTANT, 1);
DEFINE_TYPE_TRAIT(RectLight, kRectLight, TYPE_ID_LUX_RECT, 1);
DEFINE_TYPE_TRAIT(GeometryLight, kGeometryLight, TYPE_ID_LUX_GEOMETRY, 1);
DEFINE_TYPE_TRAIT(PortalLight, kPortalLight, TYPE_ID_LUX_PORTAL, 1);
DEFINE_TYPE_TRAIT(PluginLight, kPluginLight, TYPE_ID_LUX_PLUGIN, 1);
DEFINE_TYPE_TRAIT(DomeLight_1, kDomeLight_1, TYPE_ID_LUX_DOME_1, 1);
DEFINE_TYPE_TRAIT(LightFilter, kLightFilter, TYPE_ID_LUX_LIGHT_FILTER, 1);
DEFINE_TYPE_TRAIT(PluginLightFilter, kPluginLightFilter, TYPE_ID_LUX_PLUGIN_LIGHT_FILTER, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

} // namespace value

}  // namespace lightusd
