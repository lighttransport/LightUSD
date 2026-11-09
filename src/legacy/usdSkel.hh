// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file usdSkel.hh
/// @brief USD Skeleton and Animation schema definitions
///
/// Implements skeletal animation primitives following USD's UsdSkel schema.
/// Includes support for skeletons, bone hierarchies, skinning, and blend shapes.
///
/// Key classes:
/// - SkelRoot: Root of skeletal hierarchy
/// - Skeleton: Bone structure and joint transforms
/// - SkelAnimation: Animation data for skeletons
/// - BlendShape: Facial/blend shape animation support
///
/// Features:
/// - Joint hierarchies and transformations
/// - Skinning weight computation
/// - Animation curves and keyframes
/// - Blend shape targets and weights
///
#pragma once

// Core includes (replaces monolithic prim-types.hh)
#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "nonstd/expected.hpp"
#include "core/prim-enums.hh"        // Specifier, Visibility, Purpose
#include "core/path.hh"              // Path
#include "core/extent.hh"            // Extent
#include "core/composition-types.hh" // Reference, Payload, ListEditQual
#include "core/prim-metas.hh"       // PrimMeta
#include "core/animatable.hh"       // Animatable
#include "core/typed-attribute.hh"  // TypedAttribute, TypedAttributeWithFallback
#include "core/relationship.hh"     // Relationship
#include "core/attribute.hh"        // Attribute
#include "core/property.hh"         // Property
#include "core/xform-op.hh"         // XformOp (needed by Xformable in xform.hh)
#include "core/variant-types.hh"    // VariantSet
#include "xform.hh"                 // Xformable

namespace lightusd {

constexpr auto kSkelRoot = "SkelRoot";
constexpr auto kSkeleton = "Skeleton";
constexpr auto kSkelAnimation = "SkelAnimation";
constexpr auto kBlendShape = "BlendShape";

// BlendShapes
struct BlendShape {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) {
    name = name_;
  }

  const std::string &get_name() const {
    return name;
  }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<std::vector<value::vector3f>>
      offsets;  // uniform vector3f[]. required property
  TypedAttribute<std::vector<value::vector3f>>
      normalOffsets;  // uniform vector3f[]. required property

  TypedAttribute<std::vector<int>>
      pointIndices;  // uniform int[]. optional. vertex indices to the original
                     // mesh for each values in `offsets` and `normalOffsets`.

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  ///
  /// Add attribute as in-beteen BlendShape attribute.
  ///
  /// - add `inbetweens` namespace prefix
  /// - add `weight` attribute as Attribute meta.
  ///
  bool add_inbetweenBlendShape(double weight, Attribute &&attr);

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

  PrimMeta meta;

  PrimMeta &metas() {
    return meta;
  }

  const PrimMeta &metas() const {
    return meta;
  }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

// Skeleton
struct Skeleton : Xformable {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) {
    name = name_;
  }

  const std::string &get_name() const {
    return name;
  }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }


  TypedAttribute<std::vector<value::matrix4d>>
      bindTransforms;  // uniform matrix4d[]. bind-pose transform of each joint
                       // in world coordinate.

  TypedAttribute<std::vector<value::token>> jointNames;  // uniform token[]
  TypedAttribute<std::vector<value::token>> joints;      // uniform token[]

  TypedAttribute<std::vector<value::matrix4d>>
      restTransforms;  // uniform matrix4d[] rest-pose transforms of each
                       // joint in local coordinate.

  nonstd::optional<Relationship> proxyPrim;  // rel proxyPrim

  // SkelBindingAPI
  nonstd::optional<Relationship>
      animationSource;  // rel skel:animationSource = </path/...>

  TypedAttributeWithFallback<Animatable<Visibility>> visibility{
      Visibility::Inherited};  // "token visibility"
  TypedAttribute<Animatable<Extent>>
      extent;  // bounding extent. When authorized, the extent is the bounding
               // box of whole its children.
  TypedAttributeWithFallback<Purpose> purpose{
      Purpose::Default};  // "uniform token purpose"

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;
  //std::vector<value::token> xformOpOrder;

  PrimMeta meta;

  PrimMeta &metas() {
    return meta;
  }

  const PrimMeta &metas() const {
    return meta;
  }


  bool get_animationSource(Path *path, ListEditQual *qual = nullptr) {
    if (!path) {
      return false;
    }

    if (!animationSource.has_value()) {
      return false;
    }

    const Relationship &rel = animationSource.value();
    if (qual) {
      (*qual) = rel.get_listedit_qual();
    }

    if (rel.is_path()) {
      (*path) = rel.targetPath;
    } else if (rel.is_pathvector()) {
      if (rel.targetPathVector.size()) {
        (*path) = rel.targetPathVector[0];
      } else {
        return false;
      }
    } else {
      return false;
    }

    return true;
  }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

  private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

// NOTE: SkelRoot itself does not have dedicated attributes in the schema.
struct SkelRoot : Xformable {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) {
    name = name_;
  }

  const std::string &get_name() const {
    return name;
  }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }


  TypedAttribute<Animatable<Extent>>
    extent;  // bounding extent. When authorized, the extent is the bounding
  // box of whole its children.
  TypedAttributeWithFallback<Purpose> purpose{
    Purpose::Default};  // "uniform token purpose"
  TypedAttributeWithFallback<Animatable<Visibility>> visibility{
    Visibility::Inherited};  // "token visibility"

  nonstd::optional<Relationship> proxyPrim;  // rel proxyPrim
  //std::vector<XformOp> xformOps;

  // SkelBindingAPI
  nonstd::optional<Relationship>
      animationSource;  // rel skel:animationSource = </path/...>
  nonstd::optional<Relationship>
      skeleton;          // rel skel:skeleton = </path/...>


  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

  PrimMeta meta;

  PrimMeta &metas() {
    return meta;
  }

  const PrimMeta &metas() const {
    return meta;
  }


 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;

};

struct SkelAnimation {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) {
    name = name_;
  }

  const std::string &get_name() const {
    return name;
  }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<std::vector<value::token>> blendShapes;  // uniform token[]
  TypedAttribute<Animatable<std::vector<float>>> blendShapeWeights;  // float[]
  TypedAttribute<std::vector<value::token>> joints;  // uniform token[]
  TypedAttribute<Animatable<std::vector<value::quatf>>>
      rotations;  // quatf[] Joint-local unit quaternion rotations
  TypedAttribute<Animatable<std::vector<value::half3>>>
      scales;  // half3[] Joint-local scaling in 16bit half float. TODO: Use
               // float3 for LightUSD for convenience?
  TypedAttribute<Animatable<std::vector<value::float3>>>
      translations;  // float3[] Joint-local translation.

  bool get_blendShapes(std::vector<value::token> *toks);
  bool get_blendShapeWeights(std::vector<float> *vals,
                             const double t = value::TimeCode::Default(),
                             const value::TimeSampleInterpolationType tinterp =
                                 value::TimeSampleInterpolationType::Held);
  bool get_joints(std::vector<value::token> *toks);
  bool get_rotations(std::vector<value::quatf> *vals,
                     const double t = value::TimeCode::Default(),
                     const value::TimeSampleInterpolationType tinterp =
                         value::TimeSampleInterpolationType::Held);
  bool get_scales(std::vector<value::half3> *vals,
                  const double t = value::TimeCode::Default(),
                  const value::TimeSampleInterpolationType tinterp =
                      value::TimeSampleInterpolationType::Held);
  bool get_translations(std::vector<value::float3> *vals,
                        const double t = value::TimeCode::Default(),
                        const value::TimeSampleInterpolationType tinterp =
                            value::TimeSampleInterpolationType::Held);

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

  PrimMeta meta;

  PrimMeta &metas() {
    return meta;
  }

  const PrimMeta &metas() const {
    return meta;
  }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

//
// Some usdSkel utility functions
//

// Equivalent to pxrUSD's UsdSkelNormalizeWeights
// Normalizes weight values in-place so that each group of numInfluencesPerComponent
// weights sums to 1.0 (or 0.0 if all weights in a group are below eps).
bool SkelNormalizeWeights(std::vector<float> &weights, int numInfluencesPerComponent, const float eps = std::numeric_limits<float>::epsilon());

// Equivalent to pxrUSD's UsdSkelSortInfluences
// Sorts joint indices and weights per component group by weight (descending).
bool SkelSortInfluences(std::vector<int> &indices, std::vector<float> &weights, int numInfluencesPerComponent);

//
// Build Skeleleton Topology(hierarchy) from Skeleton's joints.
// (Usually from Skeleton's `joints`attribute).
//
// If you want to get handy, full Skeleton hierarchy information, Use Tydra's BuildSkelHierarchy() API.
//
// @param[in] `joints` Joint paths
// @param[out] `dst` Built SkelTopology.  dst[i] = parent joint index. -1 for root joint.
// @param[out] `err` Error message when `joints` info is invalid.
//
// @return true upon success. false when error.
//
bool BuildSkelTopology(
  const std::vector<value::token> &joints,
  std::vector<int> &dst,
  std::string *err);

///
/// Validate a skeleton topology (parent indices array).
///
/// Checks for: single root, no cycles, valid parent indices,
/// parent ordering (parent index < child index).
///
/// Equivalent to pxrUSD's UsdSkelTopology::Validate.
///
/// @param[in] topology Parent indices (-1 for root)
/// @param[out] err Error message when invalid
/// @return true if topology is valid
///
bool SkelValidateTopology(
  const std::vector<int> &topology,
  std::string *err);

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

// Register usdSkel Prim type.
DEFINE_TYPE_TRAIT(SkelRoot, kSkelRoot, TYPE_ID_SKEL_ROOT, 1);
DEFINE_TYPE_TRAIT(Skeleton, kSkeleton, TYPE_ID_SKELETON, 1);
DEFINE_TYPE_TRAIT(SkelAnimation, kSkelAnimation, TYPE_ID_SKELANIMATION, 1);
DEFINE_TYPE_TRAIT(BlendShape, kBlendShape, TYPE_ID_BLENDSHAPE, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
