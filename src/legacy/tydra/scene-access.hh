// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.

///
/// @file scene-access.hh
/// @brief High-level USD scene traversal and query API
///
/// Provides convenient functions for querying and accessing USD scene data.
/// This API is designed for ease of use and language binding compatibility
/// by avoiding C++ specific features like nonstd::optional, nonstd::expected,
/// and std::function.
///
/// Key features:
/// - Template-based prim listing and filtering
/// - Path-based scene graph traversal
/// - Shader and material queries
/// - Type-safe prim access
/// - Animation and geometry utilities
///
/// Main functions:
/// - ListPrims(): Find all prims of a specific type
/// - ListShaders(): Find all shaders of a specific type
/// - VisitPrims(): Traverse scene hierarchy with callbacks
/// - GetPrimAtPath(): Direct path-based prim access
///
/// Usage example:
/// ```cpp
/// lightusd::tydra::PathPrimMap<GeomMesh> meshes;
/// if (lightusd::tydra::ListPrims(stage, meshes)) {
///   for (const auto &pair : meshes) {
///     const std::string &path = pair.first;
///     const GeomMesh *mesh = pair.second;
///     // Process mesh...
///   }
/// }
/// ```
///
/// Note: This API avoids nonstd::optional, nonstd::expected, std::function
/// and other advanced STL features for easier language bindings.
///
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "prim-type-macros.inc"
#include "core/prim.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "core/path-expression.hh"  // ParsedPathExpression
#include "stage.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "value-type-macros.inc"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

struct FNV1StringHash {
  size_t operator()(const std::string &s) const noexcept {
    static constexpr uint64_t kFNV_Prime = 0x00000100000001B3ull;
    static constexpr uint64_t kFNV_Offset_Basis = 0xcbf29ce484222325ull;

    uint64_t hash = kFNV_Offset_Basis;
    for (char ch : s) {
      hash = (kFNV_Prime * hash) ^ static_cast<unsigned char>(ch);
    }
    return static_cast<size_t>(hash);
  }
};

///
/// Map from absolute prim path to prim pointer of type T.
/// Key = fully absolute Prim path string (e.g. "/xform/geom0")
/// Value = const pointer to prim of type T
///
template <typename T>
using PathPrimMap = std::unordered_map<std::string, const T *, FNV1StringHash>;

///
/// Map from shader path to shader data.
/// Key = absolute path to shader prim
/// Value = pair of (Shader prim containing "info:id", concrete shader type instance)
/// The first element is the generic Shader prim, the second is the typed shader
/// (e.g., UsdPreviewSurface, UsdUVTexture)
///
template <typename T>
using PathShaderMap =
    std::map<std::string, std::pair<const Shader *, const T *>>;

// TODO: extern template to suppress possible `-Wundefined-func-template`?

///
/// List Prim of type T from the Stage.
/// Returns false when unsupported/unimplemented Prim type T is given.
///
template <typename T>
bool ListPrims(const lightusd::Stage &stage, PathPrimMap<T> &m /* output */);

#define EXTERN_LISTPRIMS(__ty)                                 \
  extern template bool ListPrims(const lightusd::Stage &stage, \
                                 PathPrimMap<__ty> &m);

APPLY_FUNC_TO_PRIM_TYPES(EXTERN_LISTPRIMS)

#undef EXTERN_LISTPRIMS

///
/// List Shader of shader type T from the Stage.
/// Returns false when unsupported/unimplemented Shader type T is given.
/// TODO: User defined shader type("info:id")
///
template <typename T>
bool ListShaders(const lightusd::Stage &stage,
                 PathShaderMap<T> &m /* output */);

extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPreviewSurface> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdUVTexture> &m);

extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_string> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_int> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_float> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_float2> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_float3> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_float4> &m);
extern template bool ListShaders(const lightusd::Stage &stage,
                                 PathShaderMap<UsdPrimvarReader_matrix> &m);

///
/// Get parent Prim from Path.
/// Path must be fully expanded absolute path.
///
/// Example: Return "/xform" Prim for "/xform/mesh0" path
///
/// Returns nullptr when the given Path is a root Prim or invalid Path(`err`
/// will be filled when failed).
///
const Prim *GetParentPrim(const lightusd::Stage &stage,
                          const lightusd::Path &path, std::string *err);

///
/// Visit Stage and invoke callback functions for each Prim.
/// Can be used for alternative method of Stage::Traverse() in pxrUSD
///

///
/// Use old-style Callback function approach for easier language bindings
///
/// @param[in] abs_path Prim's absolute path(e.g. "/xform/mesh0")
/// @param[in] prim Prim
/// @param[in] tree_depth Tree depth of this Prim. 0 = root prim.
/// @param[inout] userdata User data.
/// @param[out] error message.
///
/// @return Usually true. return false + no error message to notify early
/// termination of visiting Prims.
///
typedef bool (*VisitPrimFunction)(const Path &abs_path, const Prim &prim,
                                  const int32_t tree_depth, void *userdata,
                                  std::string *err);

///
/// Visit Prims in Stage.
/// Use `primChildren` metadatum to determine traversal order of Prims if
/// exists(USDC usually contains `primChildren`) Traversal will be failed when
/// no Prim found specified in `primChildren`(if exists)
///
/// @param[out] err Error message.
///
bool VisitPrims(const lightusd::Stage &stage, VisitPrimFunction visitor_fun,
                void *userdata = nullptr, std::string *err = nullptr);

///
/// Get Property(Attribute or Relationship) of given Prim by name.
/// Similar to UsdPrim::GetProperty() in pxrUSD.
///
/// @param[in] prim Prim
/// @param[in] prop_name Property name
/// @param[out] prop Property
/// @param[out] err Error message(filled when returning false)
///
/// @return true if Property found in given Prim.
/// @return false if Property is not found in given Prim.
///
bool GetProperty(const lightusd::Prim &prim, const std::string &prop_name,
                 Property *prop, std::string *err);

///
/// Get List of Property(Attribute and Relationship) names of given Prim by
/// name. It includes authored builtin Property names.
///
/// @param[in] prim Prim
/// @param[out] prop_names Property names
/// @param[out] err Error message(filled when returning false)
///
/// @return true upon success.
/// @return false when something go wrong.
///
bool GetPropertyNames(const lightusd::Prim &prim,
                      std::vector<std::string> *prop_names, std::string *err);

///
/// Get List of Attribute names of given Prim.
/// It includes authored builtin Attribute names(e.g. "points" for `GeomMesh`).
///
/// @param[in] prim Prim
/// @param[out] attr_names Attribute names
/// @param[out] err Error message(filled when returning false)
///
/// @return true upon success.
/// @return false when something go wrong.
///
bool GetAttributeNames(const lightusd::Prim &prim,
                       std::vector<std::string> *attr_names, std::string *err);

///
/// Get List of Relationship names of given Prim.
/// It includes authored builtin Relationship names(e.g. "proxyPrim" for
/// `GeomMesh`).
///
/// @param[in] prim Prim
/// @param[out] rel_names Relationship names
/// @param[out] err Error message(filled when returning false)
///
/// @return true upon success.
/// @return false when something go wrong.
///
bool GetRelationshipNames(const lightusd::Prim &prim,
                          std::vector<std::string> *rel_names,
                          std::string *err);

///
/// Get Attribute of given Prim by name.
/// Similar to UsdPrim::GetAttribute() in pxrUSD.
///
/// @param[in] prim Prim
/// @param[in] attr_name Attribute name
/// @param[out] attr Attribute
/// @param[out] err Error message(filled when returning false)
///
/// @return true if Attribute found in given Prim.
/// @return false if Attribute is not found in given Prim, or `attr_name` is a
/// Relationship.
///
bool GetAttribute(const lightusd::Prim &prim, const std::string &attr_name,
                  Attribute *attr, std::string *err);

///
/// Check if Prim has Attribute.
///
/// @param[in] prim Prim
/// @param[in] attr_name Attribute name to query.
///
/// @return true if `attr_name` Attribute exists in the Prim.
///
bool HasAttribute(const lightusd::Prim &prim, const std::string &attr_name);

///
/// Get Relationship of given Prim by name.
/// Similar to UsdPrim::GetRelationship() in pxrUSD.
///
/// @param[in] prim Prim
/// @param[in] rel_name Relationship name
/// @param[out] rel Relationship
/// @param[out] err Error message(filled when returning false)
///
/// @return true if Relationship found in given Prim.
/// @return false if Relationship is not found in given Prim, or `rel_name` is a
/// Attribute.
///
bool GetRelationship(const lightusd::Prim &prim, const std::string &rel_name,
                     Relationship *rel, std::string *err);

///
/// Check if Prim has Relationship.
///
/// @param[in] prim Prim
/// @param[in] rel_name Relationship name to query.
///
/// @return true if `rel_name` Relationship exists in the Prim.
///
bool HasRelationship(const lightusd::Prim &prim, const std::string &rel_name);

///
/// For efficient Xform retrieval from Stage.
///
/// XformNode's pointer value and hierarchy become invalid when Prim is
/// removed/added from/to Stage. If you change the content of Stage, please
/// rebuild XformNode using BuildXformNodeFromStage() again
///
/// TODO: Use prim_id and deprecate the pointer to Prim.
///
struct XformNode {
  std::string element_name;  // e.g. "geom0"
  Path absolute_path;        // e.g. "/xform/geom0"

  const Prim *prim{nullptr};  // The pointer to Prim.
  int64_t prim_id{-1};        // Prim id(1 or greater for valid Prim ID)

  XformNode *parent{nullptr};  // pointer to parent
  std::vector<XformNode> children;

  const value::matrix4d &get_local_matrix() const { return _local_matrix; }

  // world matrix = parent_world_matrix x local_matrix
  // Equivalent to GetLocalToWorldMatrix in pxrUSD
  // if !resetXformStack! exists in Prim's xformOpOrder, this returns Prim's
  // local matrix (clears parent's world matrix)
  const value::matrix4d &get_world_matrix() const { return _world_matrix; }

  const value::matrix4d &get_parent_world_matrix() const {
    return _parent_world_matrix;
  }

  // TODO: accessible only from Friend class?
  void set_local_matrix(const value::matrix4d &m) { _local_matrix = m; }

  void set_world_matrix(const value::matrix4d &m) { _world_matrix = m; }

  void set_parent_world_matrix(const value::matrix4d &m) {
    _parent_world_matrix = m;
  }

  // true: Prim with Xform(e.g. GeomMesh)
  // false: Prim with no Xform(e.g. Stage root("/"), Scope, Material, ...)
  bool has_xform() const { return _has_xform; }
  bool &has_xform() { return _has_xform; }

  bool has_resetXformStack() const { return _has_resetXformStack; }
  bool &has_resetXformStack() { return _has_resetXformStack; }

 private:
  bool _has_xform{false};
  bool _has_resetXformStack{false};  // !resetXformStack! in xformOps
  value::matrix4d _local_matrix{value::matrix4d::identity()};
  value::matrix4d _world_matrix{value::matrix4d::identity()};
  value::matrix4d _parent_world_matrix{value::matrix4d::identity()};
};

///
/// Build Xform hierachy from Stage.
///
/// Xform value is evaluated at specified time and timeSample interpolation
/// type.
///
///
bool BuildXformNodeFromStage(
    const lightusd::Stage &stage, XformNode *root, /* out */
    const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);

std::string DumpXformNode(const XformNode &root);

///
/// Get GeomSubset children of the given Prim path
///
/// The pointer address is valid until Stage's content is unchanged.
///
/// @param[in] familyName Get GeomSubset having this `familyName`. empty token =
/// return all GeomSubsets.
/// @param[in] prim_must_be_geommesh Prim path must point to GeomMesh Prim.
///
/// (TODO: Return id of GeomSubset Prim object, instead of the ponter address)
///
/// @return array of GeomSubset pointers. Empty array when failed or no
/// GeomSubset Prim(with `familyName`) attached to the Prim.
///
///
std::vector<const GeomSubset *> GetGeomSubsets(
    const lightusd::Stage &stage, const lightusd::Path &prim_path,
    const lightusd::value::token &familyName,
    bool prim_must_be_geommesh = true);

///
/// Get GeomSubset children of the given Prim
///
/// The pointer address is valid until Stage's content is unchanged.
///
/// @param[in] familyName Get GeomSubset having this `familyName`. empty token =
/// return all GeomSubsets.
/// @param[in] prim_must_be_geommesh Prim must be GeomMesh Prim type.
///
/// (TODO: Return id of GeomSubset Prim object, instead of the ponter address)
///
/// @return array of GeomSubset pointers. Empty array when failed or no
/// GeomSubset Prim(with `familyName`) attached to the Prim.
///
std::vector<const GeomSubset *> GetGeomSubsetChildren(
    const lightusd::Prim &prim, const lightusd::value::token &familyName,
    bool prim_must_be_geommesh = true);

//
// Get BlendShape prims in this GeomMesh Prim
// (`skel:blendShapes`, `skel:blendShapeTargets`)
//
std::vector<std::pair<std::string, const lightusd::BlendShape *>>
GetBlendShapes(const lightusd::Stage &stage, const lightusd::Prim &prim,
                std::string *err = nullptr);


///
/// For composition. Convert Concrete Prim(Xform, GeomMesh, ...) to PrimSpec,
/// generic Prim container.
/// TODO: Move to *core* module?
///
bool PrimToPrimSpec(const Prim &prim, PrimSpec &ps, std::string *err);

///
/// For MaterialX
/// TODO: Move to shader-network.hh?
///
bool ShaderToPrimSpec(const UsdUVTexture &node, PrimSpec &ps, std::string *warn,
                      std::string *err);
bool ShaderToPrimSpec(const UsdTransform2d &node, PrimSpec &ps,
                      std::string *warn, std::string *err);

template <typename T>
bool ShaderToPrimSpec(const UsdPrimvarReader<T> &node, PrimSpec &ps,
                      std::string *warn, std::string *err);

//
// Utilities and Query for CollectionAPI
//

///
/// Get `Collection` object(properties defined in Collection API) from a given
/// Prim.
///
/// @param[in] prim Prim
/// @param[out] Pointer to the pointer of found Collection.
/// @return true upon success.
///
bool GetCollection(const Prim &prim, const Collection **collection);

// Resolved membership rules for one collection instance, in either mode.
class CollectionMembershipQuery {
 public:
  enum class Mode { Empty, Relationship, Expression };

  Mode mode{Mode::Empty};

  // Anchor prim path (predicate context / relative-expression base).
  std::string owner_prim_path;

  // Relationship mode (`includes`/`excludes`/`includeRoot`).
  std::vector<Path> includes;
  std::vector<Path> excludes;
  bool include_root{false};
  CollectionInstance::ExpansionRule expansion_rule{
      CollectionInstance::ExpansionRule::ExpandPrims};

  // Expression mode (`membershipExpression`).
  ParsedPathExpression membership_expression;

  bool valid() const { return mode != Mode::Empty; }
};

///
/// Get terminal Attribute. Similar to GetValueProducingAttribute in pxrUSD.
///
/// On the contrary to EvaluateAttribute, Do not evaluate Attribute value at
/// specified time.
///
/// - if Attribute is connection, follow its targetPath recursively until
/// encountering non-connection Attribute.
/// - if Attribute is blocked, return Attribute ValueBlock.
/// - if Attribute is timesamples, return TimeSamples Attribute.
/// - if Attribute is scalar, return scalar Attribute.
///
/// @return true upon success.
bool GetTerminalAttribute(const Stage &stage, const Attribute &attr,
                          const std::string &attr_name, Attribute *attr_out,
                          std::string *err);

template <typename T>
bool GetTerminalAttribute(const Stage &stage, const TypedAttribute<T> &attr,
                          const std::string &attr_name, Attribute *attr_out,
                          std::string *err) {
  if (!attr_out) {
    return false;
  }

  Attribute value;
  if (attr.has_connections()) {
    // A connection overrides the value (USD). Carry the authored value too, so
    // the Stage GetTerminalAttribute can fall back to it if the connection
    // cannot be resolved. (is_connection() is false when a value is present.)
    Attribute input;
    input.set_connections(attr.connections());
    if (attr.has_value()) {
      input.set_value(attr.get_value());
    }
    return GetTerminalAttribute(stage, input, attr_name, attr_out, err);
  } else if (attr.is_blocked()) {
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
    value.set_type_name(value::TypeTraits<T>::type_name());
    value.set_blocked(true);
    (*attr_out) = std::move(value);
    return true;
  } else if (attr.is_value_empty()) {
    value.set_type_name(value::TypeTraits<T>::type_name());
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
  } else {
    value.set_value(attr.get_value());
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
  }

  (*attr_out) = std::move(value);
  return true;
}

bool GetTerminalAttribute(const Layer &layer, const Attribute &attr,
                          const std::string &attr_name, Attribute *attr_out,
                          std::string *err);

template <typename T>
bool GetTerminalAttribute(const Layer &layer, const TypedAttribute<T> &attr,
                          const std::string &attr_name, Attribute *attr_out,
                          std::string *err) {
  if (!attr_out) {
    return false;
  }

  Attribute value;
  if (attr.is_connection()) {
    Attribute input;
    input.set_connections(attr.connections());
    return GetTerminalAttribute(layer, input, attr_name, attr_out, err);
  } else if (attr.is_blocked()) {
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
    value.set_type_name(value::TypeTraits<T>::type_name());
    value.set_blocked(true);
    (*attr_out) = std::move(value);
    return true;
  } else if (attr.is_value_empty()) {
    value.set_type_name(value::TypeTraits<T>::type_name());
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
  } else {
    value.set_value(attr.get_value());
    value.metas() = attr.metas();
    value.variability() = Variability::Uniform;
  }

  (*attr_out) = std::move(value);
  return true;
}

///
/// Get Geom Primvar.
///
/// This API supports Connection Attribute(which requires finding Prim of
/// targetPath in Stage).
///
/// example of Primvar with Connection Attribute:
///
///   texCoord2f[] primvars:uvs = </root/geom0.uvs>
///   int[] primvars:uvs:indices.connection = </root/geom0.indices>
///
/// @param[in] stage Stage
/// @param[in] prim The pointer to GPrim.
/// @param[in] name Primvar name(`primvars:` prefix omitted)
/// @param[out] primvar GeomPrimvar output.
/// @param[out] err Error message.
/// @param[out] warn Non-fatal warning message (optional). e.g. a
///                  `primvars:<name>:indices` declared with no authored value
///                  (treated as un-indexed).
///
/// @return true upon success.
///
bool GetGeomPrimvar(const Stage &stage, const GPrim *prim,
                    const std::string &name, GeomPrimvar *primvar,
                    std::string *err = nullptr, std::string *warn = nullptr);

///
/// Get Primvars in GPrim.
///
/// This API supports Connection Attribute(which requires finding Prim of
/// targetPath in Stage).
///
std::vector<GeomPrimvar> GetGeomPrimvars(const Stage &stage, const GPrim &prim);

///
/// Find a primvar on the given prim or its nearest ancestor that defines it.
/// Walks up the prim hierarchy from `prim_path` to the root.
///
/// @param[in] stage The USD Stage.
/// @param[in] prim_path Absolute path to the starting prim.
/// @param[in] primvar_name The primvar name (without "primvars:" prefix).
/// @param[out] out The found GeomPrimvar.
/// @param[out] err Optional error string.
/// @return true if found.
///
bool FindPrimvarWithInheritance(const Stage &stage, const Path &prim_path,
    const std::string &primvar_name, GeomPrimvar *out,
    std::string *err = nullptr);

///
/// Build Collection Membership
///
/// It traverse collection paths starting from `seedCollectionInstance` in the
/// Stage. Note: No circular referencing path allowed.
///
/// @returns CollectionMembershipQuery object. When encountered an error,
/// CollectionMembershipQuery contains empty info(i.e, all query will fail)
///
CollectionMembershipQuery BuildCollectionMembershipQuery(
    const Stage &stage, const CollectionInstance &seedCollectionInstance,
    const std::string &owner_prim_path = "");

bool IsPathIncluded(const CollectionMembershipQuery &query, const Stage &stage,
                    const Path &abs_path,
                    const CollectionInstance::ExpansionRule expansionRule =
                        CollectionInstance::ExpansionRule::ExpandPrims);

///
/// Evaluate an SdfPathExpression predicate (the raw text inside `{...}`, e.g.
/// "defined", "model", "kind:component", "isa:Mesh", "hasAPI:SkelBindingAPI")
/// against the prim at `prim_path` in `stage`. Used by expression-mode
/// collection membership and exposed for reuse.
///
bool EvalPathExpressionPredicate(const Stage &stage,
                                 const std::string &predicate,
                                 const std::string &prim_path);

// TODO: Layer version
// bool IsPathIncluded(const Layer &layer, const Path &abs_path, const
// CollectionInstance::ExpansionRule expansionRule =
// CollectionInstance::ExpansionRule::ExpandPrims);

//
// usdSkel
//

struct SkelNode {
  //std::string jointElementName;  // elementName(leaf node name) of jointPath.
  std::string joint_path;         // joints in UsdSkel. Relative or Absolute Prim
                                 // path(e.g. "root/head", "/root/head")
  std::string joint_name;         // jointNames in UsdSkel
  int joint_id{-1};               // jointIndex(array index in UsdSkel joints)

  value::matrix4d bind_transform{value::matrix4d::identity()};
  value::matrix4d rest_transform{value::matrix4d::identity()};
  //int parentNodeIndex{-1};

  std::vector<SkelNode> children;
};

class SkelHierarchy {
 public:
  SkelHierarchy() = default;

  std::string prim_name;                  // Skeleton Prim name
  std::string abs_path;                   // Absolute path to Skeleton Prim
  std::string display_name;               // `displayName` Prim meta

  SkelNode root_node;

  int anim_id{-1};                        // Default animation(SkelAnimation) attached to Skeleton
  std::vector<int> anim_ids;              // All animations(SkelAnimation) attached to Skeleton

  // Flat topology arrays (built alongside tree for efficient lookups).
  // parent_joint_indices[i] = parent joint index for joint i, -1 for root.
  std::vector<int> parent_joint_indices;

  // World-space bind transforms per joint (same order as Skeleton.joints).
  std::vector<value::matrix4d> bind_transforms;

  // Local rest transforms per joint (same order as Skeleton.joints).
  std::vector<value::matrix4d> rest_transforms;

  size_t num_joints() const { return parent_joint_indices.size(); }

 private:

};

using SkelNameToIndexMap = std::unordered_map<std::string, int, FNV1StringHash>;

SkelNameToIndexMap BuildSkelNameToIndexMap(const SkelHierarchy &skel);

///
/// Extract skeleleton info from Skeleton and build skeleton(bone) hierarchy.
///
bool BuildSkelHierarchy(const Skeleton &skel,
                        SkelNode &dst, std::string *err = nullptr);

//
// For USDZ AR extensions
//

///
/// List up `sceneName` of given Prim's children
/// https://developer.apple.com/documentation/realitykit/usdz-schemas-for-ar
///
/// Prim's Kind must be `sceneLibrary`
/// @param[out] List of pair of (Is Specifier `over`, sceneName). For `def`
/// Specifier(primary scene), it is set to false.
///
///
bool ListSceneNames(const lightusd::Prim &root,
                    std::vector<std::pair<bool, std::string>> *sceneNames);

//
// Skeletal mesh extent computation
// (Follows OpenUSD's pivot-based approach from pxr/usd/usdSkel)
//

///
/// Compute bounding box from joint transform translation components (pivots).
/// Optionally applies a root transform and padding.
///
/// @param[in] jointXforms Joint transforms (e.g. posed world-space transforms)
/// @param[out] extent Computed bounding box
/// @param[in] padding Amount to expand the extent in all directions
/// @param[in] rootXform Optional root transform applied to each pivot
/// @return true on success
///
bool ComputeJointsExtent(
    const std::vector<value::matrix4d> &jointXforms,
    Extent *extent,
    float padding = 0.0f,
    const value::matrix4d *rootXform = nullptr);

///
/// Compute padding that accounts for mesh geometry extending beyond joint
/// pivot positions. Used with ComputeJointsExtent for fast skeletal extent.
///
/// @param[in] restJointXforms Rest-pose joint transforms
/// @param[in] meshRestExtent Rest-pose mesh bounding box
/// @param[in] geomBindTransform Geometry bind transform
/// @return padding value (>= 0)
///
float ComputeSkinnedExtentPadding(
    const std::vector<value::matrix4d> &restJointXforms,
    const Extent &meshRestExtent,
    const value::matrix4d &geomBindTransform);

///
/// Linear Blend Skinning of points.
///
/// @param[in] restPoints Rest-pose point positions
/// @param[in] geomBindTransform Transforms rest points into skeleton space
/// @param[in] jointXforms Per-joint transforms applied in skeleton bind space.
/// The function returns through inverse(geomBindTransform). Callers targeting
/// a mesh's current local space must therefore pre-compose the Skeleton-world /
/// mesh-world conversion into these matrices explicitly.
/// @param[in] jointIndices Flat array of joint indices per point
/// @param[in] jointWeights Flat array of joint weights per point
/// @param[in] numInfluencesPerPoint Number of influences per point
/// @param[out] skinnedPoints Output skinned positions
/// @param[out] err Optional error message
/// @return true on success
///
bool SkinPointsLBS(
    const std::vector<value::point3f> &restPoints,
    const value::matrix4d &geomBindTransform,
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<int> &jointIndices,
    const std::vector<float> &jointWeights,
    int numInfluencesPerPoint,
    std::vector<value::point3f> *skinnedPoints,
    std::string *err = nullptr);

///
/// Compute skinned mesh extent using the fast pivot-based approach.
/// Combines ComputeSkinnedExtentPadding + ComputeJointsExtent.
///
/// @param[in] jointXforms Current posed joint transforms
/// @param[in] restJointXforms Rest-pose joint transforms
/// @param[in] meshRestExtent Rest-pose mesh bounding box
/// @param[in] geomBindTransform Geometry bind transform
/// @param[out] extent Computed bounding box
/// @param[in] rootXform Optional root transform
/// @return true on success
///
bool ComputeSkinnedMeshExtent(
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<value::matrix4d> &restJointXforms,
    const Extent &meshRestExtent,
    const value::matrix4d &geomBindTransform,
    Extent *extent,
    const value::matrix4d *rootXform = nullptr);

//
// Additional skeleton utilities (ported from OpenUSD's UsdSkelUtils)
//

///
/// Concatenate joint transforms: compute world-space transforms from
/// local joint transforms and topology.
///
/// Equivalent to pxrUSD's UsdSkelConcatJointTransforms.
///
/// @param[in] topology Parent joint indices (-1 for root)
/// @param[in] localXforms Per-joint local transforms
/// @param[out] worldXforms Output world-space transforms
/// @param[in] rootXform Optional root transform prepended to roots
/// @return true on success
///
bool ConcatJointTransforms(
    const std::vector<int> &topology,
    const std::vector<value::matrix4d> &localXforms,
    std::vector<value::matrix4d> *worldXforms,
    const value::matrix4d *rootXform = nullptr);

///
/// Compute joint-local transforms from world-space transforms and topology.
///
/// Equivalent to pxrUSD's UsdSkelComputeJointLocalTransforms.
///
/// @param[in] topology Parent joint indices (-1 for root)
/// @param[in] worldXforms Per-joint world-space transforms
/// @param[out] localXforms Output local transforms
/// @param[in] rootXform Optional inverse root transform for roots
/// @return true on success
///
bool ComputeJointLocalTransforms(
    const std::vector<int> &topology,
    const std::vector<value::matrix4d> &worldXforms,
    std::vector<value::matrix4d> *localXforms,
    const value::matrix4d *inverseRootXform = nullptr);

///
/// Build a transform matrix from translation, rotation (quaternion), and scale.
/// This is the transform composition used by SkelAnimation data
/// (translations, rotations, scales).
///
/// Equivalent to pxrUSD's UsdSkelMakeTransform.
///
/// @param[in] translation Translation component
/// @param[in] rotation Rotation quaternion (w, x, y, z order in value::quatf)
/// @param[in] scale Scale component (half3 from SkelAnimation)
/// @return Composed 4x4 transform matrix
///
value::matrix4d SkelMakeTransform(
    const value::float3 &translation,
    const value::quatf &rotation,
    const value::half3 &scale);

///
/// Skin normals using Linear Blend Skinning (LBS).
/// Normals are transformed using the inverse-transpose of the skinning matrix
/// (direction-only, no translation).
///
/// @param[in] restNormals Rest-pose normals
/// @param[in] geomBindTransform Transforms rest normals into skeleton space
/// @param[in] jointXforms Per-joint transforms (skeleton-space)
/// @param[in] jointIndices Flat array of joint indices per point
/// @param[in] jointWeights Flat array of joint weights per point
/// @param[in] numInfluencesPerPoint Number of influences per point
/// @param[out] skinnedNormals Output skinned normals (renormalized)
/// @param[out] err Optional error message
/// @return true on success
///
bool SkinNormalsLBS(
    const std::vector<value::normal3f> &restNormals,
    const value::matrix4d &geomBindTransform,
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<int> &jointIndices,
    const std::vector<float> &jointWeights,
    int numInfluencesPerPoint,
    std::vector<value::normal3f> *skinnedNormals,
    std::string *err = nullptr);

///
/// Expand constant (per-prim) skinning influences to per-vertex.
///
/// When jointIndices/jointWeights interpolation is "constant", we have a single
/// set of numInfluencesPerComponent indices+weights that applies to every vertex.
/// This expands them to a per-vertex flat array.
///
/// @param[in] indices Constant joint indices (size = numInfluencesPerComponent)
/// @param[in] weights Constant joint weights (size = numInfluencesPerComponent)
/// @param[in] numVertices Number of vertices
/// @param[out] expandedIndices Output per-vertex indices (size = numVertices * numInfluencesPerComponent)
/// @param[out] expandedWeights Output per-vertex weights (size = numVertices * numInfluencesPerComponent)
/// @return true on success
///
bool ExpandConstantInfluencesToVarying(
    const std::vector<int> &indices,
    const std::vector<float> &weights,
    size_t numVertices,
    std::vector<int> *expandedIndices,
    std::vector<float> *expandedWeights);

}  // namespace tydra
}  // namespace lightusd
