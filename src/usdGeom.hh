// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file usdGeom.hh
/// @brief USD Geometry schema definitions
///
/// Implements geometry primitives and related utilities following USD's
/// UsdGeom schema. Includes basic geometry types like Mesh, Cube, Sphere,
/// BasisCurves, Camera, and supporting classes like GeomPrimvar for
/// primitive variables (vertex data, texture coordinates, etc).
///
/// Key classes:
/// - GPrim: Base class for geometry primitives
/// - GeomPrimvar: Wrapper for primvars (per-vertex data)
/// - Mesh, Cube, Sphere, etc.: Specific geometry types
/// - Xform: Transformation primitive
///
///
#pragma once

// Core includes (replaces monolithic prim-types.hh)
#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "nonstd/expected.hpp"
#include "core/prim-enums.hh"        // Specifier, Orientation, Visibility, Purpose, Axis, Interpolation
#include "core/extent.hh"            // Extent
#include "core/composition-types.hh" // Reference, Payload, ListEditQual
#include "core/prim-metas.hh"       // PrimMeta
#include "core/animatable.hh"       // Animatable
#include "core/typed-attribute.hh"  // TypedAttribute, TypedAttributeWithFallback
#include "core/relationship.hh"     // Relationship, RelationshipProperty
#include "core/attribute.hh"        // Attribute
#include "core/property.hh"         // Property
#include "core/xform-op.hh"         // XformOp (needed by Xformable in xform.hh)
#include "core/collection-api.hh"   // Collection
#include "core/material-binding.hh" // MaterialBinding
#include "core/variant-types.hh"    // VariantSet
#include "xform.hh"                 // Xformable, Identity functions
#include "usdShade.hh"

namespace lightusd {

// From schema definition.
constexpr auto kGPrim = "GPrim";
constexpr auto kGeomCube = "Cube";
constexpr auto kGeomXform = "Xform";
constexpr auto kGeomMesh = "Mesh";
constexpr auto kGeomSubset = "GeomSubset";
constexpr auto kGeomBasisCurves = "BasisCurves";
constexpr auto kGeomNurbsCurves = "NurbsCurves";
constexpr auto kGeomCylinder = "Cylinder";
constexpr auto kGeomCapsule = "Capsule";
constexpr auto kGeomPoints = "Points";
constexpr auto kGeomCone = "Cone";
constexpr auto kGeomSphere = "Sphere";
constexpr auto kGeomCamera = "Camera";
constexpr auto kPointInstancer = "PointInstancer";
constexpr auto kGeomPlane = "Plane";
constexpr auto kGeomCylinder_1 = "Cylinder_1";
constexpr auto kGeomCapsule_1 = "Capsule_1";
constexpr auto kGeomTetMesh = "TetMesh";
constexpr auto kGeomNurbsPatch = "NurbsPatch";
constexpr auto kGeomHermiteCurves = "HermiteCurves";

// UsdVol
constexpr auto kVolume = "Volume";
constexpr auto kFieldAsset = "FieldAsset";
constexpr auto kOpenVDBAsset = "OpenVDBAsset";
constexpr auto kField3DAsset = "Field3DAsset";

constexpr auto kMaterialBinding = "material:binding";
constexpr auto kMaterialBindingCollection = "material:binding:collection";
constexpr auto kMaterialBindingPreview = "material:binding:preview";
constexpr auto kMaterialBindingFull = "material:binding:full";

struct GPrim;

bool IsSupportedGeomPrimvarType(uint32_t tyid);
bool IsSupportedGeomPrimvarType(const std::string &type_name);

//
// GeomPrimvar is a wrapper class for Attribute and indices(for Indexed Primvar)
// - Attribute with `primvars` prefix. e.g. "primvars:
// - Optional: indices.
//
// GeomPrimvar is only constructable from GPrim.
// This class COPIES variable from GPrim for `get` operation.
//
// Currently read-only operation is well provided. writing feature is not well tested(`set_value` may have issue)
// (If you struggled to ue GeomPrimvar, please operate on `GPrim::props` directly)
//
// Limitation:
// TimeSamples are not supported for indices.
// Also, TimeSamples are not supported both when constructing GeomPrimvar with Typed Attribute value and retrieving Attribute value.
//
//
class GeomPrimvar {

 friend GPrim;

 public:
  GeomPrimvar() : _has_value(false) {
    //LIGHTUSD_LOG_I("GeomPrimvar default constructor called");
  }

  GeomPrimvar(const Attribute &attr) : _attr(attr) {
    //LIGHTUSD_LOG_I("GeomPrimvar constructor called with Attribute");
    _has_value = true;
  }

  GeomPrimvar(const Attribute &attr, const std::vector<int32_t> &indices) : _attr(attr)
  {
    //LIGHTUSD_LOG_I("GeomPrimvar constructor called with Attribute and indices vector");
    _indices = indices;
    _has_value = true;
  }

  GeomPrimvar(const Attribute &attr, const value::TimeSamples &indices) : _attr(attr)
  {
    _ts_indices = indices;
    _has_value = true;
  }

  GeomPrimvar(const Attribute &attr, value::TimeSamples &&indices) : _attr(attr)
  {
    _ts_indices = std::move(indices);
    _has_value = true;
  }

  GeomPrimvar(const GeomPrimvar &) = default;
  GeomPrimvar &operator=(const GeomPrimvar &) = default;
  GeomPrimvar(GeomPrimvar &&) noexcept = default;
  GeomPrimvar &operator=(GeomPrimvar &&) noexcept = default;

  ///
  /// For Indexed Primvar(array value + indices)
  ///
  /// equivalent to ComputeFlattened in pxrUSD.
  ///
  /// ```
  /// for i in len(indices):
  ///   dest[i] = values[indices[i]]
  /// ```
  ///
  /// Use Default time and Linear interpolation when `indices` and/or primvar is timesamples.
  ///
  /// If Primvar does not have indices, return attribute value as is(same with `get_value`).
  /// For now, we only support Attribute with 1D array type.
  ///
  /// Return false when operation failed or if the attribute type is not supported for Indexed Primvar.
  ///
  ///
  template <typename T>
  bool flatten_with_indices(std::vector<T> *dst, std::string *err = nullptr) const;

  ///
  /// Specify time and interpolation type.
  ///
  template <typename T>
  bool flatten_with_indices(double t, std::vector<T> *dst, value::TimeSampleInterpolationType tinerp = value::TimeSampleInterpolationType::Linear, std::string *err = nullptr) const;


  // Generic Value version (returns flattened value::Value, not raw Attribute).
  bool flatten_with_indices(value::Value *dst, std::string *err = nullptr) const;
  bool flatten_with_indices(double t, value::Value *dst, value::TimeSampleInterpolationType tinterp = value::TimeSampleInterpolationType::Linear, std::string *err = nullptr) const;

  bool has_elementSize() const;
  uint32_t get_elementSize() const;

  bool has_interpolation() const;
  Interpolation get_interpolation() const;

  void set_elementSize(uint32_t n) {
    _elementSize = n;
  }

  void set_interpolation(const Interpolation interp) {
    _interpolation = interp;
  }

  bool has_unauthoredValuesIndex() const {
    return _unauthoredValuesIndex.has_value();
  }

  int get_unauthoredValuesIndex() const {
    return _unauthoredValuesIndex.value_or(-1);
  }

  void set_unauthoredValuesIndex(int n) {
    _unauthoredValuesIndex = n;
  }

  ///
  /// Get indices at specified timecode.
  /// Returns empty when appropriate indices does not exist for timecode 't'.
  ///
  std::vector<int32_t> get_indices(const double t = value::TimeCode::Default()) const;

  const std::vector<int32_t> &get_default_indices() const
      LIGHTUSD_LIFETIMEBOUND {
    return _indices;
  }

  const value::TimeSamples &get_timesampled_indices() const
      LIGHTUSD_LIFETIMEBOUND {
    return _ts_indices;
  }

  bool has_default_indices() const { return !_indices.empty(); }
  bool has_timesampled_indices() const { return _ts_indices.size() > 0; }

  bool has_indices() const {
    return has_default_indices() || has_timesampled_indices();
  }

  uint32_t type_id() const { return _attr.type_id(); }
  std::string type_name() const { return _attr.type_name(); }

  // Name of Primvar. "primvars:" prefix(namespace) is omitted.
  const std::string name() const { return _name; }

  ///
  /// Attribute has value?(Not empty)
  ///
  bool has_value() const {
    return _has_value;
  }

  ///
  /// Get type name of primvar.
  ///
  std::string get_type_name() const {
    if (!_has_value) {
      return "null";
    }

    return _attr.type_name();
  }

  ///
  /// Get type id of primvar.
  ///
  uint32_t get_type_id() const {
    if (!_has_value) {
      return value::TYPE_ID_NULL;
    }
    return _attr.type_id();
  }

  ///
  /// Get Attribute value.
  ///
  template <typename T>
  bool get_value(T *dst, std::string *err = nullptr) const;

  // Non-template value extraction (the templated get_value<T> overloads forward
  // here and cast with value::Value::as<T>()). The timecode overload is declared
  // further below.
  bool get_value(value::Value *dst, std::string *err = nullptr) const;


  ///
  /// Get Attribute value at specified time.
  ///
  template <typename T>
  bool get_value(double timecode, T *dst, const value::TimeSampleInterpolationType interp = value::TimeSampleInterpolationType::Linear, std::string *err = nullptr) const;

  bool get_value(double timecode, value::Value *dst, const value::TimeSampleInterpolationType interp = value::TimeSampleInterpolationType::Linear, std::string *err = nullptr) const;

  ///
  /// Set Attribute value.
  ///
  template <typename T>
  void set_value(const T &val) {
    _attr.set_value(val);
    _has_value = true;
  }

  void set_value(const Attribute &attr) {
    _attr = attr;
    _has_value = true;
  }

  void set_value(Attribute &&attr) {
    _attr = std::move(attr);
    _has_value = true;
  }

  void set_name(const std::string &name) { _name = name; }

  // Set indices for specified timecode 't'
  // indices will be replaced when there is an indices at timecode 't'.
  void set_indices(const std::vector<int32_t> &indices, const double t = value::TimeCode::Default());


  void set_default_indices(const std::vector<int32_t> &indices) {
    _indices = indices;
  }

  void set_default_indices(std::vector<int32_t> &&indices) {
    _indices = std::move(indices);
  }

  void set_timesampled_indices(const value::TimeSamples &indices) {
    _ts_indices = indices;
  }

  const Attribute &get_attribute() const LIGHTUSD_LIFETIMEBOUND {
    return _attr;
  }

 private:

  // Resolve indices at time `t`. Returns a const ref — either to `_indices`
  // directly (zero-copy) or to `buf` when fetched from timesamples.
  const std::vector<int32_t> &resolve_indices_at(
      double t, value::TimeSampleInterpolationType tinterp,
      std::vector<int32_t> &buf) const;

  std::string _name;
  bool _has_value{false};
  Attribute _attr;
  std::vector<int32_t> _indices;  // 'default' indices
  value::TimeSamples _ts_indices;

  // Store Attribute meta separately.
  nonstd::optional<int32_t> _unauthoredValuesIndex; // for sparse primvars in some DCC. default = -1.
  nonstd::optional<uint32_t> _elementSize;
  nonstd::optional<Interpolation> _interpolation;

};

// Geometric Prim. Encapsulates Imagable + Boundable in pxrUSD schema.
// <pxrUSD>/pxr/usd/usdGeom/schema.udsa
//
// Note: In OpenUSD, UsdGeomGprim inherits UsdGeomBoundable -> UsdGeomImageable
// -> UsdTyped. It does NOT inherit from UsdShadePrim; shading is accessed via
// MaterialBinding API schema (which LightUSD already mixes in).

struct GPrim : Xformable, MaterialBinding, Collection {
  std::string name;
  Specifier spec{Specifier::Def};

  int64_t parent_id{-1};  // Index to parent node

  std::string prim_type;  // Primitive type(if specified by `def`)

  void set_name(const std::string &name_) {
    name = name_;
  }

  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND {
    return name;
  }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  // Gprim

  TypedAttribute<Animatable<Extent>>
      extent;  // bounding extent. When authorized, the extent is the bounding
               // box of whole its children.

  TypedAttributeWithFallback<bool> doubleSided{
      false};  // "uniform bool doubleSided"

  TypedAttributeWithFallback<Orientation> orientation{
      Orientation::RightHanded};  // "uniform token orientation"
  TypedAttributeWithFallback<Animatable<Visibility>> visibility{
      Visibility::Inherited};  // "token visibility"
  TypedAttributeWithFallback<Purpose> purpose{
      Purpose::Default};  // "uniform token purpose"

  // Handy API to get `primvars:displayColor` and `primvars:displayOpacity`
  bool get_displayColor(value::color3f *col, const double t = value::TimeCode::Default(), const value::TimeSampleInterpolationType tinterp = value::TimeSampleInterpolationType::Linear) const;

  bool get_displayOpacity(float *opacity, const double t = value::TimeCode::Default(), const value::TimeSampleInterpolationType tinterp = value::TimeSampleInterpolationType::Linear) const;

  const std::vector<value::color3f> get_displayColors(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  Interpolation get_displayColorsInterpolation() const;

  RelationshipProperty proxyPrim;

  std::map<std::string, Property> props;

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;

  // For GeomPrimvar.

  ///
  /// Get Attribute(+ indices Attribute for Indexed Primvar) with "primvars:" suffix(namespace) in `props`
  ///
  /// NOTE: This API does not support Connection Atttribute(e.g. `int[] primvars:uvs:indices = </root/geom0.indices>`)
  /// If you want to get Primvar with possible Connection Attribute, use Tydra API: `GetGeomPrimvar`
  ///
  /// @param[in] name Primvar name(`primvars:` prefix omitted. e.g. "normals", "st0", ...)
  /// @param[out] primvar GeomPrimvar output.
  /// @param[out] err Optional Error message(filled when returning false)
  ///
  bool get_primvar(const std::string &name, GeomPrimvar *primvar, std::string *err = nullptr) const;

  ///
  /// Check if primvar exists with given name
  ///
  /// @param[in] name Primvar name(`primvars:` prefix omitted. e.g. "normals", "st0", ...)
  ///
  bool has_primvar(const std::string &name) const;

  ///
  /// Return List of Primvar in this GPrim contains.
  ///
  /// NOTE: This API does not support Connection Atttribute(e.g. `int[] primvars:uvs:indices = </root/geom0.indices>`)
  /// If you want to get Primvar with possible Connection Attribute, use Tydra API: `GetGeomPrimvars`
  ///
  std::vector<GeomPrimvar> get_primvars() const;

  ///
  /// Set Attribute(+ indices Attribute for Indexed Primvar) with "primvars:" suffix(namespace) to `props`
  ///
  /// @param[in] primvar GeomPrimvar
  /// @param[out] err Optional Error message(filled when returning false)
  ///
  /// Returns true when success to add primvar. Return false on error(e.g. `primvar` does not contain valid name).
  ///
  bool set_primvar(const GeomPrimvar &primvar, std::string *err = nullptr);


  ///
  /// Aux infos
  ///
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND {
    return _primChildrenNames;
  }

  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND {
    return _propertyNames;
  }

  const std::vector<value::token> &primChildrenNames() const
      LIGHTUSD_LIFETIMEBOUND {
    return _primChildrenNames;
  }

  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND {
    return _propertyNames;
  }

  const std::map<std::string, VariantSet> &variantSetList() const
      LIGHTUSD_LIFETIMEBOUND {
    return _variantSetMap;
  }

  std::map<std::string, VariantSet> &variantSetList() LIGHTUSD_LIFETIMEBOUND {
    return _variantSetMap;
  }

  // Prim metadataum.
  PrimMeta meta; // TODO: Move to private

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND {
    return meta;
  }

  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND {
    return meta;
  }

 private:

  //bool _valid{true};  // default behavior is valid(allow empty GPrim)

  std::vector<value::token> _primChildrenNames;
  std::vector<value::token> _propertyNames;

  // For Variants
  std::map<std::string, VariantSet> _variantSetMap;

};

struct Xform : GPrim {
  // Xform() {}
};

// GeomSubset
struct GeomSubset : public MaterialBinding, Collection {
  enum class ElementType { Face, Point, Edge, Tetrahedron };

  enum class FamilyType {
    Partition,       // 'partition'
    NonOverlapping,  // 'nonOverlapping'
    Unrestricted,    // 'unrestricted' (fallback)
  };

  std::string name;
  Specifier spec{Specifier::Def};

  int64_t parent_id{-1};  // Index to parent node

  TypedAttributeWithFallback<ElementType> elementType{ElementType::Face};
  TypedAttribute<value::token> familyName;  // "uniform token familyName"

  // FamilyType attribute is described in parent GeomMesh's `subsetFamily:<FAMILYNAME>:familyType` attribute.
  //TypedAttributeWithFallback<FamilyType> familyType{FamilyType::Unrestricted};

  nonstd::expected<bool, std::string> SetElementType(const std::string &str) {
    if (str == "face") {
      elementType = ElementType::Face;
      return true;
    } else if (str == "point") {
      elementType = ElementType::Point;
      return true;
    } else if (str == "edge") {
      elementType = ElementType::Edge;
      return true;
    } else if (str == "tetrahedron") {
      elementType = ElementType::Tetrahedron;
      return true;
    }

    return nonstd::make_unexpected(
        "`face`, `point`, `edge` or `tetrahedron` is supported for `elementType`, but `" + str +
        "` specified");
  }

  TypedAttribute<Animatable<std::vector<int32_t>>> indices; // int[] indices

  std::map<std::string, Property> props;  // custom Properties
  PrimMeta meta;

  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND {
    return _primChildrenNames;
  }

  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND {
    return _propertyNames;
  }

  const std::vector<value::token> &primChildrenNames() const
      LIGHTUSD_LIFETIMEBOUND {
    return _primChildrenNames;
  }

  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND {
    return _propertyNames;
  }

  static bool ValidateSubsets(
    const std::vector<const GeomSubset *> &subsets,
    const size_t elementCount,
    const FamilyType &familyType, std::string *err);


 private:
  std::vector<value::token> _primChildrenNames;
  std::vector<value::token> _propertyNames;
};

// Polygon mesh geometry
// X11's X.h uses `None` macro, so add extra prefix to `None` enum
struct GeomMesh : GPrim {
  enum class InterpolateBoundary {
    InterpolateBoundaryNone,  // "none"
    EdgeAndCorner,            // "edgeAndCorner"
    EdgeOnly                  // "edgeOnly"
  };

  enum class FaceVaryingLinearInterpolation {
    CornersPlus1,                        // "cornersPlus1"
    CornersPlus2,                        // "cornersPlus2"
    CornersOnly,                         // "cornersOnly"
    Boundaries,                          // "boundaries"
    FaceVaryingLinearInterpolationNone,  // "none"
    All,                                 // "all"
  };

  enum class SubdivisionScheme {
    CatmullClark,           // "catmullClark"
    Loop,                   // "loop"
    Bilinear,               // "bilinear"
    SubdivisionSchemeNone,  // "none"
  };

  // catmullClark-only `triangleSubdivisionRule`.
  enum class TriangleSubdivisionRule {
    CatmullClark,  // "catmullClark"
    Smooth,        // "smooth"
  };

  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;  // point3f[]
  TypedAttribute<Animatable<std::vector<value::normal3f>>>
      normals;  // normal3f[] (NOTE: "primvars:normals" are stored in
                // `GPrim::props`)

  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[]

  TypedAttribute<Animatable<std::vector<int32_t>>>
      faceVertexCounts;  // int[] faceVertexCounts
  TypedAttribute<Animatable<std::vector<int32_t>>>
      faceVertexIndices;  // int[] faceVertexIndices

  // Make SkelBindingAPI first citizen.
  nonstd::optional<Relationship> skeleton;  // rel skel:skeleton

  //
  // Utility functions
  //


  ///
  /// @brief Returns `points`.
  ///
  /// NOTE: No support for connected attribute. Using tydra::EvaluateTypedAttribute preferred.
  ///
  /// @param[in] time Time for TimeSampled `points` data.
  /// @param[in] interp Interpolation type for TimeSampled `points` data
  /// @return points vector(copied). Returns empty when `points` attribute is
  /// not defined.
  ///
  const std::vector<value::point3f> get_points(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  ///
  /// @brief Returns normals vector. Precedence order: `primvars:normals` then
  /// `normals`.
  ///
  /// NOTE: No support for connected attribute. Using tydra::GetGeomPrimvar preferred.
  ///
  /// @return normals vector(copied). Returns empty normals vector when neither
  /// `primvars:normals` nor `normals` attribute defined, attribute is a
  /// Relationship, Connection Attribute, or normals attribute have invalid type(other than `normal3f`).
  ///
  const std::vector<value::normal3f> get_normals(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  ///
  /// @brief Get interpolation of `primvars:normals`, then `normals`.
  /// @return Interpolation of normals. `vertex` by defaut.
  ///
  Interpolation get_normalsInterpolation() const;

  ///
  /// @brief Returns `faceVertexCounts`.
  ///
  /// NOTE: No support for connected attribute. Using tydra::EvaluateTypedAttribute preferred.
  ///
  /// @return face vertex counts vector(copied)
  ///
  const std::vector<int32_t> get_faceVertexCounts(double time = value::TimeCode::Default()) const;

  ///
  /// @brief Returns `faceVertexIndices`.
  ///
  /// @return face vertex indices vector(copied)
  ///
  const std::vector<int32_t> get_faceVertexIndices(double time = value::TimeCode::Default()) const;

  //
  // SubD attribs.
  //
  TypedAttribute<Animatable<std::vector<int32_t>>>
      cornerIndices;  // int[] cornerIndices
  TypedAttribute<Animatable<std::vector<float>>>
      cornerSharpnesses;  // float[] cornerSharpnesses
  TypedAttribute<Animatable<std::vector<int32_t>>>
      creaseIndices;  // int[] creaseIndices
  TypedAttribute<Animatable<std::vector<int32_t>>>
      creaseLengths;  // int[] creaseLengths
  TypedAttribute<Animatable<std::vector<float>>>
      creaseSharpnesses;  // float[] creaseSharpnesses
  TypedAttribute<Animatable<std::vector<int32_t>>>
      holeIndices;  // int[] holeIndices
  TypedAttributeWithFallback<Animatable<InterpolateBoundary>>
      interpolateBoundary{
          InterpolateBoundary::EdgeAndCorner};  // token interpolateBoundary
  TypedAttributeWithFallback<SubdivisionScheme> subdivisionScheme{
      SubdivisionScheme::CatmullClark};  // uniform token subdivisionScheme
  TypedAttributeWithFallback<Animatable<FaceVaryingLinearInterpolation>>
      faceVaryingLinearInterpolation{
          FaceVaryingLinearInterpolation::
              CornersPlus1};  // token faceVaryingLinearInterpolation
  TypedAttributeWithFallback<TriangleSubdivisionRule> triangleSubdivisionRule{
      TriangleSubdivisionRule::
          CatmullClark};  // uniform token triangleSubdivisionRule

  TypedAttribute<std::vector<value::token>> blendShapes; // uniform token[] skel:blendShapes
  nonstd::optional<Relationship> blendShapeTargets; // rel skel:blendShapeTargets (Path[])

  // Note: In OpenUSD, skel:jointIndices and skel:jointWeights are primvars
  // accessed via UsdGeomPrimvarsAPI (a NonAppliedAPI schema), not first-class
  // struct members. LightUSD follows the same approach: these are stored in
  // the `props` map and accessed via get_primvar()/GeomPrimvar.


  ///
  /// For GeomSubset
  ///
  /// This creates `uniform token subsetFamily:<familyName>:familyType = familyType` attribute when serialized.
  ///
  void set_subsetFamilyType(const value::token &familyName, GeomSubset::FamilyType familyType) {
    subsetFamilyTypeMap[familyName] = familyType;
  }

  ///
  /// This look ups `uniform token subsetFamily:<familyName>:familyType = familyType` attribute.
  ///
  /// @return true upon success, false when corresponding attribute not found or invalid.
  bool get_subsetFamilyType(const value::token &familyName, GeomSubset::FamilyType *familyType) {
    if (!familyType) {
      return false;
    }

    if (subsetFamilyTypeMap.count(familyName)) {
      (*familyType) = subsetFamilyTypeMap[familyName];
      return true;
    }
    return false;

  }

  ///
  /// Return the list of subet familyNames in this GeomMesh.
  ///
  /// This lists `uniform token subsetFamily:<familyName>:familyType` attributes.
  ///
  /// @return The list familyNames. Empty when no familyName attribute found.
  std::vector<value::token> get_subsetFamilyNames() {
    std::vector<value::token> toks;
    for (const auto &item : subsetFamilyTypeMap) {
      toks.push_back(item.first);
    }
    return toks;
  }


  // familyName -> familyType map
  std::map<value::token, GeomSubset::FamilyType> subsetFamilyTypeMap;

  // Get Explicit Joint orders: `uniform token[] skel:joints`
  std::vector<value::token> get_joints() const;

  ///
  /// Validate mesh topology.
  /// Checks faceVertexCounts/faceVertexIndices consistency, index bounds,
  /// and subdivision surface attributes (corners, creases, holes).
  ///
  bool ValidateTopology(std::string *err = nullptr,
      double time = value::TimeCode::Default()) const;
};

struct GeomCamera : public GPrim {
  enum class Projection {
    Perspective,   // "perspective"
    Orthographic,  // "orthographic"
  };

  enum class StereoRole {
    Mono,   // "mono"
    Left,   // "left"
    Right,  // "right"
  };

  //
  // Properties
  //
  // NOTE: fallback value is in [mm](tenth of scene unit)
  //

  TypedAttribute<Animatable<std::vector<value::float4>>> clippingPlanes; // float4[]
  TypedAttributeWithFallback<Animatable<value::float2>> clippingRange{
      value::float2({0.1f, 1000000.0f})};
  TypedAttributeWithFallback<Animatable<float>> exposure{0.0f};  // in EV
  TypedAttributeWithFallback<Animatable<float>> focalLength{50.0f};
  TypedAttributeWithFallback<Animatable<float>> focusDistance{5.0f};
  TypedAttributeWithFallback<Animatable<float>> horizontalAperture{20.965f};
  TypedAttributeWithFallback<Animatable<float>> horizontalApertureOffset{0.0f};
  TypedAttributeWithFallback<Animatable<float>> verticalAperture{15.2908f};
  TypedAttributeWithFallback<Animatable<float>> verticalApertureOffset{0.0f};
  TypedAttributeWithFallback<Animatable<float>> fStop{
      0.0f};  // 0.0 = no focusing
  TypedAttributeWithFallback<Animatable<Projection>> projection{
      Projection::Perspective};  // "token projection" Animatable

  TypedAttributeWithFallback<StereoRole> stereoRole{
      StereoRole::Mono};  // "uniform token stereoRole"

  TypedAttributeWithFallback<Animatable<double>> shutterClose{
      0.0};  // double shutter:close
  TypedAttributeWithFallback<Animatable<double>> shutterOpen{
      0.0};  // double shutter:open
};

// struct GeomBoundable : GPrim {};

struct GeomCone : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{1.0};

  TypedAttributeWithFallback<Axis> axis{Axis::Z};
};

struct GeomCapsule : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{0.5};
  TypedAttributeWithFallback<Axis> axis{Axis::Z};  // uniform token axis
};

struct GeomCylinder : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{1.0};
  TypedAttributeWithFallback<Axis> axis{Axis::Z};  // uniform token axis
};

struct GeomCube : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> size{2.0};
};

struct GeomSphere : public GPrim {
  //
  // Predefined attribs.
  //
  // USD spec says default radius is 1.0 (UsdGeomSphere).
  TypedAttributeWithFallback<Animatable<double>> radius{1.0};
};

struct GeomPlane : public GPrim {
  TypedAttributeWithFallback<Animatable<double>> width{2.0};
  TypedAttributeWithFallback<Animatable<double>> length{2.0};
  TypedAttributeWithFallback<Axis> axis{Axis::Z};  // uniform token axis
};

struct GeomCylinder_1 : public GPrim {
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radiusTop{1.0};
  TypedAttributeWithFallback<Animatable<double>> radiusBottom{1.0};
  TypedAttributeWithFallback<Axis> axis{Axis::Z};  // uniform token axis
};

struct GeomCapsule_1 : public GPrim {
  TypedAttributeWithFallback<Animatable<double>> height{1.0};
  TypedAttributeWithFallback<Animatable<double>> radiusTop{0.5};
  TypedAttributeWithFallback<Animatable<double>> radiusBottom{0.5};
  TypedAttributeWithFallback<Axis> axis{Axis::Z};  // uniform token axis
};

struct GeomTetMesh : public GPrim {
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> velocities;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> accelerations;
  TypedAttribute<Animatable<std::vector<value::normal3f>>> normals;
  TypedAttribute<Animatable<std::vector<value::int4>>> tetVertexIndices;
  TypedAttribute<Animatable<std::vector<value::int3>>> surfaceFaceVertexIndices;
};

struct GeomNurbsPatch : public GPrim {
  // PointBased attributes
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> velocities;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> accelerations;
  TypedAttribute<Animatable<std::vector<value::normal3f>>> normals;

  // NurbsPatch-specific
  TypedAttribute<Animatable<int>> uVertexCount;
  TypedAttribute<Animatable<int>> vVertexCount;
  TypedAttribute<Animatable<int>> uOrder;
  TypedAttribute<Animatable<int>> vOrder;
  TypedAttribute<Animatable<std::vector<double>>> uKnots;
  TypedAttribute<Animatable<std::vector<double>>> vKnots;
  TypedAttributeWithFallback<value::token> uForm{value::token("open")};   // "open", "closed", "periodic"
  TypedAttributeWithFallback<value::token> vForm{value::token("open")};   // "open", "closed", "periodic"
  TypedAttribute<Animatable<value::double2>> uRange;
  TypedAttribute<Animatable<value::double2>> vRange;
  TypedAttribute<Animatable<std::vector<double>>> pointWeights;

  // Trim curves
  TypedAttribute<Animatable<std::vector<int>>> trimCurve_counts;         // trimCurve:counts
  TypedAttribute<Animatable<std::vector<int>>> trimCurve_orders;         // trimCurve:orders
  TypedAttribute<Animatable<std::vector<int>>> trimCurve_vertexCounts;   // trimCurve:vertexCounts
  TypedAttribute<Animatable<std::vector<double>>> trimCurve_knots;       // trimCurve:knots
  TypedAttribute<Animatable<std::vector<value::double2>>> trimCurve_ranges;   // trimCurve:ranges
  TypedAttribute<Animatable<std::vector<value::double3>>> trimCurve_points;   // trimCurve:points
};

struct GeomHermiteCurves : public GPrim {
  // PointBased / Curves attributes
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> velocities;
  TypedAttribute<Animatable<std::vector<value::vector3f>>> accelerations;
  TypedAttribute<Animatable<std::vector<value::normal3f>>> normals;
  TypedAttribute<Animatable<std::vector<int>>> curveVertexCounts;
  TypedAttribute<Animatable<std::vector<float>>> widths;

  // HermiteCurves-specific
  TypedAttribute<Animatable<std::vector<value::vector3f>>> tangents;
};

//
// Basis Curves(for hair/fur)
//
struct GeomBasisCurves : public GPrim {
  enum class Type {
    Cubic,   // "cubic"(default)
    Linear,  // "linear"
  };

  enum class Basis {
    Bezier,      // "bezier"(default)
    Bspline,     // "bspline"
    CatmullRom,  // "catmullRom"
  };

  enum class Wrap {
    Nonperiodic,  // "nonperiodic"(default)
    Periodic,     // "periodic"
    Pinned,       // "pinned"
  };

  TypedAttributeWithFallback<Type> type{Type::Cubic};
  TypedAttributeWithFallback<Basis> basis{Basis::Bezier};
  TypedAttributeWithFallback<Wrap> wrap{Wrap::Nonperiodic};

  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;    // point3f
  TypedAttribute<Animatable<std::vector<value::normal3f>>> normals;  // normal3f
  TypedAttribute<Animatable<std::vector<int>>> curveVertexCounts;
  TypedAttribute<Animatable<std::vector<float>>> widths;
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f

  // Convenience getters
  const std::vector<value::point3f> get_points(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<value::normal3f> get_normals(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<int> get_curveVertexCounts(
      double time = value::TimeCode::Default()) const;

  const std::vector<float> get_widths(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;
};

struct GeomNurbsCurves : public GPrim {

  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;
  TypedAttribute<Animatable<std::vector<int>>>
      curveVertexCounts;
  TypedAttribute<Animatable<std::vector<value::normal3f>>>
      normals;
  TypedAttribute<Animatable<std::vector<value::point3f>>>
      points;
  TypedAttribute<Animatable<std::vector<float>>>
      widths;


  TypedAttribute<Animatable<std::vector<int>>> order;
  TypedAttribute<Animatable<std::vector<double>>> knots;
  TypedAttribute<Animatable<std::vector<value::double2>>> ranges;
  TypedAttribute<Animatable<std::vector<double>>> pointWeights;

  // Convenience getters
  const std::vector<value::point3f> get_points(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<value::normal3f> get_normals(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<int> get_curveVertexCounts(
      double time = value::TimeCode::Default()) const;

  const std::vector<float> get_widths(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<int> get_order(
      double time = value::TimeCode::Default()) const;

  const std::vector<double> get_knots(
      double time = value::TimeCode::Default()) const;
};

//
// Points primitive.
//
struct GeomPoints : public GPrim {
  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;  // point3f[]
  TypedAttribute<Animatable<std::vector<value::normal3f>>>
      normals;                                            // normal3f[]
  TypedAttribute<Animatable<std::vector<float>>> widths;  // float[]
  TypedAttribute<Animatable<std::vector<int64_t>>>
      ids;  // int64[] per-point ids.
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[]
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f[]

  // Convenience getters
  const std::vector<value::point3f> get_points(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<value::normal3f> get_normals(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<float> get_widths(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  const std::vector<int64_t> get_ids(
      double time = value::TimeCode::Default()) const;
};

// Point instancer.
// In OpenUSD, UsdGeomPointInstancer provides ComputeExtentAtTime() which
// evaluates instance transforms at a given time and computes a union of
// transformed prototype extents. LightUSD stores the raw attributes;
// extent computation requires resolving prototype prims and is handled
// at the Tydra/application level.
struct GeomPointInstancer : public GPrim {
  nonstd::optional<Relationship> prototypes;  // rel prototypes

  TypedAttribute<Animatable<std::vector<int32_t>>>
      protoIndices;                                      // int[] protoIndices
  TypedAttribute<Animatable<std::vector<int64_t>>> ids;  // int64[] ids
  TypedAttribute<Animatable<std::vector<value::point3f>>>
      positions;  // point3f[] positions
  TypedAttribute<Animatable<std::vector<value::quath>>>
      orientations;  // quath[] orientations
  TypedAttribute<Animatable<std::vector<value::float3>>>
      scales;  // float3[] scales
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[] velocities
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f[] accelerations
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      angularVelocities;  // vector3f[] angularVelocities
  TypedAttribute<Animatable<std::vector<int64_t>>>
      invisibleIds;  // int64[] invisibleIds
  TypedAttribute<std::vector<int64_t>>
      inactiveIds;  // int64[] inactiveIds

  // --- Convenience accessors (sample animated attrs at `time`) ---
  // protoIndices is the per-instance prototype selector and the instance-count
  // authority. Integer/topology arrays use Held interpolation.
  const std::vector<int32_t> get_protoIndices(
      double time = value::TimeCode::Default()) const;
  const std::vector<value::point3f> get_positions(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;
  const std::vector<value::float3> get_scales(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;
  const std::vector<value::quath> get_orientations(
      double time = value::TimeCode::Default()) const;
  const std::vector<int64_t> get_ids(
      double time = value::TimeCode::Default()) const;
  const std::vector<int64_t> get_invisibleIds(
      double time = value::TimeCode::Default()) const;
  const std::vector<int64_t> get_inactiveIds() const;
};


// --- PointInstancer instance transform / mask computation ---
//
// Replicates the core of OpenUSD
// UsdGeomPointInstancer::ComputeInstanceTransformsAtTime(). For each instance
// `i` in [0, protoIndices.size()):
//
//   localXform[i] = Mult(Mult(S, R), T)   (row-vector: p * S * R * T)
//     S = scale(scales[i] or {1,1,1})
//     R = to_matrix(orientations[i] or identity quaternion)
//     T = translate(positions[i] or {0,0,0})
//
// If `proto_xforms` is non-null and sized == instance count, each entry is
// pre-multiplied (applied first to a prototype-local point):
//   localXform[i] = Mult((*proto_xforms)[i], localXform[i]).
//
// Preliminary limitations (vs OpenUSD):
//   * velocities / accelerations / angularVelocities are ignored (no sub-frame
//     motion-blur extrapolation).
//   * orientations are sampled with Held interpolation (no quaternion slerp
//     across timeSamples).
//
// `protoIndices` is the instance-count authority. Missing/blocked optional SRT
// arrays fall back to identity defaults (matching OpenUSD). Returns false (with
// *err) when an authored SRT array has a non-zero length that differs from the
// instance count. An empty/unauthored `protoIndices` yields an empty result and
// returns true.
bool ComputeInstanceTransformsAtTime(
    const GeomPointInstancer &pi, double time,
    value::TimeSampleInterpolationType interp,
    std::vector<value::matrix4d> *out_xforms, std::string *err,
    const std::vector<value::matrix4d> *proto_xforms = nullptr);

// Compute a per-instance visibility mask from `invisibleIds` (animatable) and
// `inactiveIds` (uniform). `out_mask[i] == false` => instance i should be
// culled. Instance ids come from `ids` when authored, otherwise the implicit
// index `i`. `out_mask` is sized to protoIndices.size(). Returns true on
// success.
bool ComputeMaskAtTime(const GeomPointInstancer &pi, double time,
                       std::vector<bool> *out_mask, std::string *err);


//
// UsdVol schema (OpenVDB / Field3D volumes).
//
// `Volume` is a Gprim that aggregates one or more field primitives via
// `field:<name>` relationships. Each field prim (OpenVDBAsset / Field3DAsset)
// references a volume asset file and names the grid to read from it.
//

// Base class for field-asset prims (UsdVolFieldAsset). Modeled as a GPrim for
// reconstruct reuse (it is Xformable in USD; the extra Boundable/Collection
// members are harmless and unauthored by default).
struct FieldAsset : GPrim {
  TypedAttribute<Animatable<value::AssetPath>> filePath;   // asset filePath
  TypedAttribute<Animatable<value::token>> fieldName;      // token fieldName
  TypedAttribute<Animatable<value::token>> fieldDataType;  // token fieldDataType
  TypedAttribute<Animatable<int>> fieldIndex;              // int fieldIndex
  // token vectorDataRoleHint ("None", "Point", "Normal", "Vector", "Color")
  TypedAttribute<Animatable<value::token>> vectorDataRoleHint;
};

// OpenVDB field asset (UsdVolOpenVDBAsset).
struct OpenVDBAsset : FieldAsset {
  // token fieldClass ("levelSet", "fogVolume", "staggered", "unknown")
  TypedAttribute<Animatable<value::token>> fieldClass;
};

// Field3D field asset (UsdVolField3DAsset).
struct Field3DAsset : FieldAsset {
  TypedAttribute<Animatable<value::token>> fieldPurpose;  // token fieldPurpose
};

// Volume Gprim (UsdVolVolume). `field:<name>` relationships map a field name to
// the path of a field-asset prim. Because the names are dynamic, they are kept
// in `fieldRelationships` (parsed from any `field:*` relationship) in addition
// to GPrim::props.
struct Volume : GPrim {
  // field name (e.g. "density") -> relationship targeting the field prim.
  std::map<std::string, Relationship> fieldRelationships;
};

// --- ComputeExtent helpers ---

bool ComputeExtent(const GeomMesh &mesh, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomPoints &pts, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomSphere &sphere, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomCube &cube, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomCone &cone, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomCylinder &cylinder, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomCapsule &capsule, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomBasisCurves &curves, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

bool ComputeExtent(const GeomNurbsCurves &curves, Extent *extent,
    double time = value::TimeCode::Default(), std::string *err = nullptr);

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

// Geom
DEFINE_TYPE_TRAIT(GPrim, kGPrim, TYPE_ID_GPRIM, 1);

DEFINE_TYPE_TRAIT(Xform, kGeomXform, TYPE_ID_GEOM_XFORM, 1);
DEFINE_TYPE_TRAIT(GeomMesh, kGeomMesh, TYPE_ID_GEOM_MESH, 1);
DEFINE_TYPE_TRAIT(GeomBasisCurves, kGeomBasisCurves, TYPE_ID_GEOM_BASIS_CURVES,
                  1);
DEFINE_TYPE_TRAIT(GeomNurbsCurves, kGeomNurbsCurves, TYPE_ID_GEOM_NURBS_CURVES,
                  1);
DEFINE_TYPE_TRAIT(GeomSphere, kGeomSphere, TYPE_ID_GEOM_SPHERE, 1);
DEFINE_TYPE_TRAIT(GeomCube, kGeomCube, TYPE_ID_GEOM_CUBE, 1);
DEFINE_TYPE_TRAIT(GeomCone, kGeomCone, TYPE_ID_GEOM_CONE, 1);
DEFINE_TYPE_TRAIT(GeomCylinder, kGeomCylinder, TYPE_ID_GEOM_CYLINDER, 1);
DEFINE_TYPE_TRAIT(GeomCapsule, kGeomCapsule, TYPE_ID_GEOM_CAPSULE, 1);
DEFINE_TYPE_TRAIT(GeomPoints, kGeomPoints, TYPE_ID_GEOM_POINTS, 1);
DEFINE_TYPE_TRAIT(GeomSubset, kGeomSubset, TYPE_ID_GEOM_GEOMSUBSET, 1);
DEFINE_TYPE_TRAIT(GeomCamera, kGeomCamera, TYPE_ID_GEOM_CAMERA, 1);
DEFINE_TYPE_TRAIT(GeomPointInstancer, kPointInstancer, TYPE_ID_GEOM_POINT_INSTANCER, 1);
DEFINE_TYPE_TRAIT(GeomPlane, kGeomPlane, TYPE_ID_GEOM_PLANE, 1);
DEFINE_TYPE_TRAIT(GeomCylinder_1, kGeomCylinder_1, TYPE_ID_GEOM_CYLINDER_1, 1);
DEFINE_TYPE_TRAIT(GeomCapsule_1, kGeomCapsule_1, TYPE_ID_GEOM_CAPSULE_1, 1);
DEFINE_TYPE_TRAIT(GeomTetMesh, kGeomTetMesh, TYPE_ID_GEOM_TET_MESH, 1);
DEFINE_TYPE_TRAIT(GeomNurbsPatch, kGeomNurbsPatch, TYPE_ID_GEOM_NURBS_PATCH, 1);
DEFINE_TYPE_TRAIT(GeomHermiteCurves, kGeomHermiteCurves, TYPE_ID_GEOM_HERMITE_CURVES, 1);

// UsdVol
DEFINE_TYPE_TRAIT(Volume, kVolume, TYPE_ID_VOLUME, 1);
DEFINE_TYPE_TRAIT(FieldAsset, kFieldAsset, TYPE_ID_FIELD_ASSET, 1);
DEFINE_TYPE_TRAIT(OpenVDBAsset, kOpenVDBAsset, TYPE_ID_OPENVDB_ASSET, 1);
DEFINE_TYPE_TRAIT(Field3DAsset, kField3DAsset, TYPE_ID_FIELD3D_ASSET, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

// Relation is supported as geomprimvar.
// example:
//
// rel primvar:myrel = [</a>, </b>]
//

// NOTE: `bool` type seems not supported on pxrUSD
// NOTE: `string` type need special treatment when `idFrom` Relationship exists( https://github.com/syoyo/lightusd/issues/113 )
#define APPLY_GEOMPRIVAR_TYPE_SCALAR_A(__FUNC) \
  __FUNC(bool) __FUNC(std::string) __FUNC(value::half) __FUNC(value::half2) \
  __FUNC(value::half3) __FUNC(value::half4) __FUNC(int)
#define APPLY_GEOMPRIVAR_TYPE_SCALAR_B(__FUNC) \
  __FUNC(value::int2) __FUNC(value::int3) __FUNC(value::int4) __FUNC(uint32_t) \
  __FUNC(value::uint2) __FUNC(value::uint3) __FUNC(value::uint4)
#define APPLY_GEOMPRIVAR_TYPE_SCALAR(__FUNC) \
  APPLY_GEOMPRIVAR_TYPE_SCALAR_A(__FUNC) APPLY_GEOMPRIVAR_TYPE_SCALAR_B(__FUNC)

#define APPLY_GEOMPRIVAR_TYPE_VEC_A(__FUNC) \
  __FUNC(float) __FUNC(value::float2) __FUNC(value::float3) __FUNC(value::float4) \
  __FUNC(double) __FUNC(value::double2) __FUNC(value::double3)
#define APPLY_GEOMPRIVAR_TYPE_VEC_B(__FUNC) \
  __FUNC(value::double4) __FUNC(value::matrix2d) __FUNC(value::matrix3d) \
  __FUNC(value::matrix4d) __FUNC(value::quath) __FUNC(value::quatf) __FUNC(value::quatd)
#define APPLY_GEOMPRIVAR_TYPE_VEC(__FUNC) \
  APPLY_GEOMPRIVAR_TYPE_VEC_A(__FUNC) APPLY_GEOMPRIVAR_TYPE_VEC_B(__FUNC)

#define APPLY_GEOMPRIVAR_TYPE_ROLE_A(__FUNC) \
  __FUNC(value::normal3h) __FUNC(value::normal3f) __FUNC(value::normal3d) \
  __FUNC(value::vector3h) __FUNC(value::vector3f) __FUNC(value::vector3d) \
  __FUNC(value::point3h) __FUNC(value::point3f) __FUNC(value::point3d) __FUNC(value::color3f)
#define APPLY_GEOMPRIVAR_TYPE_ROLE_B(__FUNC) \
  __FUNC(value::color3d) __FUNC(value::color4f) __FUNC(value::color4d) \
  __FUNC(value::texcoord2h) __FUNC(value::texcoord2f) __FUNC(value::texcoord2d) \
  __FUNC(value::texcoord3h) __FUNC(value::texcoord3f) __FUNC(value::texcoord3d)
#define APPLY_GEOMPRIVAR_TYPE_ROLE(__FUNC) \
  APPLY_GEOMPRIVAR_TYPE_ROLE_A(__FUNC) APPLY_GEOMPRIVAR_TYPE_ROLE_B(__FUNC)

// Full type list = the three groups concatenated (used by IsSupportedGeomPrimvarType
// and EXTERN_TEMPLATE_GET_VALUE). The six *_A/*_B leaf macros let each
// usdGeom-primvar-inst-*.cc instantiate ~8 types.
#define APPLY_GEOMPRIVAR_TYPE(__FUNC) \
  APPLY_GEOMPRIVAR_TYPE_SCALAR(__FUNC) \
  APPLY_GEOMPRIVAR_TYPE_VEC(__FUNC)    \
  APPLY_GEOMPRIVAR_TYPE_ROLE(__FUNC)

#define EXTERN_TEMPLATE_GET_VALUE(__ty) \
  extern template bool GeomPrimvar::get_value(__ty *dest, std::string *err) const; \
  extern template bool GeomPrimvar::get_value(double, __ty *dest, value::TimeSampleInterpolationType, std::string *err) const; \
  extern template bool GeomPrimvar::get_value(std::vector<__ty> *dest, std::string *err) const; \
  extern template bool GeomPrimvar::get_value(double, std::vector<__ty> *dest, value::TimeSampleInterpolationType, std::string *err) const; \
  extern template bool GeomPrimvar::flatten_with_indices(std::vector<__ty> *dest, std::string *err) const; \
  extern template bool GeomPrimvar::flatten_with_indices(double, std::vector<__ty> *dest, value::TimeSampleInterpolationType, std::string *err) const;

APPLY_GEOMPRIVAR_TYPE(EXTERN_TEMPLATE_GET_VALUE)

#undef EXTERN_TEMPLATE_GET_VALUE

//#undef APPLY_GEOMPRIVAR_TYPE


}  // namespace lightusd
