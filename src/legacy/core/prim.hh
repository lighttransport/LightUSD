// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// core/prim.hh - Prim class for the Stage scene graph
//
// Prim is a freezed-state scene graph element. For scene graph manipulation,
// use PrimSpec (in core/prim-spec.hh) instead.
//
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "value-types.hh"
#include "core/path.hh"
#include "core/prim-enums.hh"
#include "core/prim-metas.hh"

namespace lightusd {

// Forward declarations needed before variant-types.hh
class Prim;
class PrimSpec;
struct VariantSet;

} // namespace lightusd

// variant-types.hh uses Prim/PrimSpec forward declarations
#include "core/variant-types.hh"

namespace lightusd {

///
/// Get elementName from Prim(e.g., Xform::name, GeomMesh::name)
/// `v` must be the value of Prim class.
///
nonstd::optional<std::string> GetPrimElementName(const value::Value &v);

///
/// Set name for Prim `v`(e.g. Xform::name = elementName)
/// `v` must be the value of Prim class.
///
bool SetPrimElementName(value::Value &v, const std::string &elementName);

//
// For `Stage` scene graph.
// Its a freezed state of an element of a scene graph(so no Prim
// additin/deletion from a scene graph is considered). May be Similar to `Prim`
// in pxrUSD. If you want to manipulate scene graph, use PrimSpec instead(but
// PrimSpec is W.I.P.) This class uses tree-representation of `Prim`. Easy to
// use, but may not be performant than flattened array index representation of
// Prim tree(Index-based scene graph such like glTF).
//
class Prim {
 public:
  // elementName is read from `rhs`(if it is a class of Prim)
  Prim(const value::Value &rhs);
  Prim(value::Value &&rhs);

  Prim(const std::string &elementName, const value::Value &rhs);
  Prim(const std::string &elementName, value::Value &&rhs);

  template <typename T>
  Prim(const T &prim) {
    set_primdata(prim);
  }

  template <typename T>
  Prim(const std::string &elementName, const T &prim) {
    set_primdata(elementName, prim);
  }

  // Default constructor (creates empty/invalid Prim)
  Prim() = default;

  // Special member functions (copy and move)
  Prim(const Prim&) = default;
  Prim& operator=(const Prim&) = default;
  Prim(Prim&&) noexcept = default;
  Prim& operator=(Prim&&) noexcept = default;

  // Replace exting prim
  template <typename T>
  void set_primdata(const T &prim) {
    // Check if T is Prim class type.
    static_assert((value::TypeId::TYPE_ID_MODEL_BEGIN <=
                   value::TypeTraits<T>::type_id()) &&
                      (value::TypeId::TYPE_ID_MODEL_END >
                       value::TypeTraits<T>::type_id()),
                  "T is not a Prim class type");
    _data = prim;
    // Use prim.name for elementName
    _elementPath = Path(prim.name, "");
  }

  // Replace exting prim
  template <typename T>
  void set_primdata(const std::string &elementName, const T &prim) {
    // Check if T is Prim class type.
    static_assert((value::TypeId::TYPE_ID_MODEL_BEGIN <=
                   value::TypeTraits<T>::type_id()) &&
                      (value::TypeId::TYPE_ID_MODEL_END >
                       value::TypeTraits<T>::type_id()),
                  "T is not a Prim class type");
    _data = prim;
    SetPrimElementName(_data, elementName);
    _elementPath = Path(elementName, "");
  }

  ///
  /// Add Prim as a child.
  /// When `rename_element_name` is true, rename input Prims elementName to make
  /// it unique among children(since USD(Crate) spec doesn't allow same Prim
  /// elementName in the same Prim hierarchy.
  ///
  /// Renaming rule is Maya-like:
  /// - No elementName given: `default`
  /// - Add or increment number suffix to the elementName:
  ///    - `plane` => `plane1`
  ///    - `plane1` => `plane2`
  ///
  /// Note: This function is thread-safe.
  ///
  /// @return true Upon success. false when failed(e.g. Prim with same
  /// Prim::element_name() already exists when `rename_element_name` is false)
  /// and fill `err` with error message
  ///
  bool add_child(Prim &&prim, const bool rename_element_name = true,
                 std::string *err = nullptr);

  ///
  /// Replace existing child Prim whose elementName is `child_prim_name`.
  /// When there is no child Prim with elementName `child_prim_name` exists,
  /// `prim` is added and rename is elementName to `child_prim_name`.
  ///
  /// @return true Upon success. false when failed(e.g. `child_prim_name` is
  /// empty string or invalid Prim name) and fill `err` with error message.
  ///
  bool replace_child(const std::string &child_prim_name, Prim &&prim,
                     std::string *err = nullptr);

  // TODO: Deprecate this API to disallow direct modification of children.
  std::vector<Prim> &children() LIGHTUSD_LIFETIMEBOUND { return _children; }

  const std::vector<Prim> &children() const LIGHTUSD_LIFETIMEBOUND {
    return _children;
  }

  const value::Value &data() const LIGHTUSD_LIFETIMEBOUND { return _data; }
  value::Value &get_data() LIGHTUSD_LIFETIMEBOUND { return _data; }

  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return _specifier; }

  Specifier specifier() const { return _specifier; }

  // local_path is reserved for Prim composition.
  // for a while please use absolute_path(full Prim absolute path) or
  // element_name(leaf Prim name).
  Path &local_path() LIGHTUSD_LIFETIMEBOUND { return _path; }
  const Path &local_path() const LIGHTUSD_LIFETIMEBOUND { return _path; }

  ///
  /// Absolute Prim Path(e.g. "/xform/mesh0") is available after
  /// Stage::compute_absolute_path() or assign it manually by an app.
  ///
  Path &absolute_path() LIGHTUSD_LIFETIMEBOUND { return _abs_path; }
  const Path &absolute_path() const LIGHTUSD_LIFETIMEBOUND {
    return _abs_path;
  }

  Path &element_path() LIGHTUSD_LIFETIMEBOUND { return _elementPath; }
  const Path &element_path() const LIGHTUSD_LIFETIMEBOUND {
    return _elementPath;
  }

  // elementName = element_path's prim part
  tstring_view element_name() const { return _elementPath.prim_part(); }

  const std::string type_name() const { return _data.type_name(); }

  uint32_t type_id() const { return _data.type_id(); }

  std::string &prim_type_name() LIGHTUSD_LIFETIMEBOUND {
    return _prim_type_name;
  }
  const std::string &prim_type_name() const LIGHTUSD_LIFETIMEBOUND {
    return _prim_type_name;
  }

  template <typename T>
  bool is() const {
    return (_data.type_id() == value::TypeTraits<T>::type_id());
  }

  // Return a pointer of a concrete Prim class(Xform, Material, ...)
  // Return nullptr when failed to cast or T is not a Prim type.
  template <typename T>
  const T *as() const {
    // Check if T is Prim type. e.g. Xform, Material, ...
    if ((value::TypeId::TYPE_ID_MODEL_BEGIN <=
         value::TypeTraits<T>::type_id()) &&
        (value::TypeId::TYPE_ID_MODEL_END > value::TypeTraits<T>::type_id())) {
      return _data.as<T>();
    }

    return nullptr;
  }

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND;

  int64_t prim_id() const { return _prim_id; }

  int64_t &prim_id() LIGHTUSD_LIFETIMEBOUND { return _prim_id; }

  const std::map<std::string, VariantSet> &variantSets() const
      LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }

  std::map<std::string, VariantSet> &variantSets() LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }

  ///
  /// Get indices for children().
  ///
  /// This is an utility API to traverse child Prims according to `primChildren`
  /// Prim metadata. If you want to traverse child Prims as done in pxrUSD(which
  /// used `primChildren` to determine the order of traversal), use this
  /// function.
  ///
  /// If no `primChildren` Prim metadata, it will simply returns [0,
  /// children().size()) sequence.
  ///
  /// index may have -1, which means invalid(child Prim not found described in
  /// by primChildren) Also, app should extra check of the value of index if
  /// `indices_is_valid` is set to false(index may be duplicated(Duplicated Prim
  /// name exits in `primChildren`)  and not in range `[0, children() -1`)
  ///
  /// NOTE: The result is computed on each call and returned by value (no
  /// internal cache), so this is safe to call on a shared Prim concurrently.
  ///
  /// @param[in] force_update Deprecated no-op (nothing is cached anymore).
  /// @param[out] indices_is_valid Optional. Set true when returned indices are
  /// valid.
  ///
  std::vector<int64_t> get_child_indices_from_primChildren(
      bool force_update = true, bool *indices_is_valid = nullptr) const;

  //
  // AOUSD Core Spec 11.5: Stage query predicates
  //

  /// Returns true if this prim is active (default: true).
  /// Inactive prims are pruned from traversal.
  bool IsActive() const { return metas().get_active(); }

  /// Returns true if this prim has a defining specifier (def or class).
  /// An "undefined" prim only has `over` opinions — no definition.
  bool IsDefined() const {
    return _specifier == Specifier::Def || _specifier == Specifier::Class;
  }

  /// Returns true if this prim has `class` specifier (abstract prim).
  /// Abstract prims serve as templates and are not rendered.
  bool IsAbstract() const { return _specifier == Specifier::Class; }

  /// Returns true if this prim is marked as instanceable.
  bool IsInstance() const { return metas().get_instanceable(); }

  /// Returns true if this prim has any composition arcs authored
  /// (references, payload, inherits, specializes, or variantSets).
  /// Per AOUSD Spec 11.3.3, a prim is only an instance when
  /// IsInstance() && HasCompositionArcs().
  bool HasCompositionArcs() const {
    const auto &m = metas();
    return m.references.has_value() || m.payload.has_value() ||
           m.inherits.has_value() || m.specializes.has_value() ||
           m.variantSets.has_value();
  }

  /// Returns true if this prim is a model (has kind metadata).
  /// Per Spec 11.4, model hierarchy is rooted at kind=component or kind=group.
  bool IsModel() const { return metas().has_kind(); }

  /// Returns true if this prim is a group model (kind=group or kind=assembly).
  bool IsGroup() const {
    Kind k = metas().get_kind_enum();
    return k == Kind::Group || k == Kind::Assembly;
  }

  // TODO: Add API to get parent Prim directly?
  // (Currently we need to traverse parent Prim using Stage)

 private:
  Path _abs_path;  // Absolute Prim path in a freezed(after composition state).
                   // Usually set by Stage::compute_absolute_path()
  Path _path;  // Prim's local path name. May contain Property, Relationship and
               // other infos, but do not include parent's path. To get fully
               // absolute path of a Prim(e.g. "/xform0/mymesh0", You need to
               // traverse Prim tree and concatename `elementPath` or use
               // ***(T.B.D>) method in `Stage` class
  Path _elementPath;  // leaf("terminal") Prim name.(e.g. "myxform" for `def
                      // Xform "myform"`). For root node, elementPath name is
                      // empty string("").

  std::string _prim_type_name;  // Prim's type name. e.g. "Xform", "Mesh",
                                // "UnknownPrim", ... Could be empty for `def
                                // "myprim" {}`

  Specifier _specifier{
      Specifier::Invalid};  // `def`, `over` or `class`. Usually `def`
  value::Value
      _data;  // Generic container for concrete Prim object. GPrim, Xform, ...

  std::vector<Prim> _children;  // child Prim nodes
  // std::set<std::string> _childrenNames; // child Prim name(elementName).
  std::multiset<std::string>
      _childrenNameSet;  // Stores input child Prim's elementName to assign
                         // unique elementName in `add_child`

  int64_t _prim_id{
      -1};  // Unique Prim id when positive(starts with 1). Id is assigned by
            // Stage::compute_absolute_prim_path_and_assign_prim_id. Usually [1,
            // NumPrimsInStage)

  std::map<std::string, VariantSet> _variantSets;
};

bool IsXformablePrim(const Prim &prim);

// forward decl(xform.hh)
struct Xformable;
bool CastToXformable(const Prim &prim, const Xformable **xformable);

///
/// Get Prim's local transform(xformOps) at specified time.
/// For non-Xformable Prim it returns identity matrix.
///
/// @param[in] prim Prim
/// @param[out] resetXformStack Whether Prim's xformOps contains
/// `!resetXformStack!` or not
/// @param[in] t time
/// @param[in] tinterp Interpolation type(Linear or Held)
///
value::matrix4d GetLocalTransform(const Prim &prim, bool *resetXformStak,
                                  double t = value::TimeCode::Default(),
                                  value::TimeSampleInterpolationType tinterp =
                                      value::TimeSampleInterpolationType::Linear);

} // namespace lightusd
