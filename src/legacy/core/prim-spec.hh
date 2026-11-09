// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// core/prim-spec.hh - PrimSpec class and prim:: namespace typedefs
//
// PrimSpec is a Prim object state just after reading from USDA/USDC,
// before compositions and Prim reconstruction by applying schema.
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "value-types.hh"
#include "core/prim-enums.hh"
#include "core/prim-metas.hh"
#include "core/property.hh"
#include "core/composition-types.hh"

namespace lightusd {

// Forward declarations needed before variant-types.hh
class Prim;
class PrimSpec;
struct VariantSet;

} // namespace lightusd

// variant-types.hh uses Prim/PrimSpec forward declarations
#include "core/variant-types.hh"

namespace lightusd {

/// Similar to PrimSpec
/// PrimSpec is a Prim object state just after reading it from USDA and USDC.
/// The state before compositions and Prim reconstruction by applying
/// schema(ReconstructPrim in prim-reconstruct.hh) happens.
///
/// Its composed primarily of name, specifier, PrimMeta and
/// Properties(Relationships and Attributes)
class PrimSpec {
 public:
  PrimSpec() {
    //LIGHTUSD_LOG_I("PrimSpec default constructor called");
  }

  PrimSpec(const Specifier &spec, const std::string &name)
      : _specifier(spec), _name(name) {
    //LIGHTUSD_LOG_I("PrimSpec constructor called with spec and name: " << name);
  }
  PrimSpec(const Specifier &spec, const std::string &typeName,
           const std::string &name)
      : _specifier(spec), _typeName(typeName), _name(name) {
    //LIGHTUSD_LOG_I("PrimSpec constructor called with spec, typeName, and name: " << name);
  }

  PrimSpec(const PrimSpec &rhs) {
    //LIGHTUSD_LOG_I("PrimSpec copy constructor called");
    if (this != &rhs) {
      CopyFrom(rhs);
    }
  }

  PrimSpec &operator=(const PrimSpec &rhs) {
    //LIGHTUSD_LOG_I("PrimSpec copy assignment operator called");
    if (this != &rhs) {
      CopyFrom(rhs);
    }

    return *this;
  }

  PrimSpec(PrimSpec &&rhs) noexcept {
    //LIGHTUSD_LOG_I("PrimSpec move constructor called");
    if (this != &rhs) {
      MoveFrom(rhs);
    }
  }

  PrimSpec &operator=(PrimSpec &&rhs) noexcept {
    //LIGHTUSD_LOG_I("PrimSpec move assignment operator called");
    if (this != &rhs) {
      MoveFrom(rhs);
    }

    return *this;
  }

  const std::string &name() const LIGHTUSD_LIFETIMEBOUND { return _name; }
  std::string &name() LIGHTUSD_LIFETIMEBOUND { return _name; }

  const std::string &typeName() const LIGHTUSD_LIFETIMEBOUND {
    return _typeName;
  }
  // Can change type name
  std::string &typeName() LIGHTUSD_LIFETIMEBOUND { return _typeName; }

  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND {
    return _specifier;
  }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return _specifier; }

  const std::vector<PrimSpec> &children() const LIGHTUSD_LIFETIMEBOUND {
    return _children;
  }
  std::vector<PrimSpec> &children() LIGHTUSD_LIFETIMEBOUND {
    return _children;
  }

  ///
  /// Select variant.
  ///
  bool select_variant(const std::string &target_name,
                      const std::string &variant_name) {
    if (metas().variants.has_value()) {
      const auto m = metas().variants.value().find(target_name);
      if (m != metas().variants.value().end()) {
        _current_vsmap[target_name] = variant_name;
        return true;
      } else {
        return false;
      }
    }
    return false;
  }

  bool current_variant_selection(const std::string &target_name,
                      std::string *selected_variant_name) {

    if (!selected_variant_name) {
      return false;
    }

    if (!metas().variants.has_value()) {
      return false;
    }

    const auto &vsmap = metas().variants.value();

    const auto m = vsmap.find(target_name);
    if (m != vsmap.end()) {
      const auto sm = _current_vsmap.find(target_name);
      if (sm != _current_vsmap.end()) {
        (*selected_variant_name) = sm->second;
      } else {
        (*selected_variant_name) = m->second;
      }
      return true;
    } else {
      return false;
    }
  }

  ///
  /// List variants in this PrimSpec
  /// key = variant name
  /// value = variats
  ///
  const VariantSelectionMap &get_variant_selection_map() const {
    if (metas().variants.has_value()) {
      return metas().variants.value();
    }
    static const VariantSelectionMap s_empty;
    return s_empty;
  }

  ///
  /// Variants
  ///
  /// VariantSet = Prim metas + Properties and/or child Prims
  ///            = repsetent as PrimNode for a while.
  ///
  ///
  /// key = variant name
  std::map<std::string, VariantSetSpec> &variantSets()
      LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }
  const std::map<std::string, VariantSetSpec> &variantSets() const
      LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND {
    static const PrimMeta empty;
    return _metas ? *_metas : empty;
  }

  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND {
    if (!_metas) _metas = std::make_unique<PrimMeta>();
    return *_metas;
  }

  using PropertyMap = std::map<std::string, Property>;

  const PropertyMap &props() const LIGHTUSD_LIFETIMEBOUND { return _props; }
  PropertyMap &props() LIGHTUSD_LIFETIMEBOUND { return _props; }

  const std::vector<Reference> &get_references();
  const ListEditQual &get_references_listedit_qualifier();

  const std::vector<Payload> &get_payloads();
  const ListEditQual &get_payloads_listedit_qualifier();

  const std::vector<value::token> &primChildren() const LIGHTUSD_LIFETIMEBOUND {
    return _primChildren;
  }

  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND {
    return _properties;
  }

  const std::string &get_current_working_path() const LIGHTUSD_LIFETIMEBOUND {
    return _current_working_path;
  }

  const std::vector<std::string> &get_asset_search_paths() const
      LIGHTUSD_LIFETIMEBOUND {
    return _asset_search_paths;
  }

  void set_current_working_path(const std::string &s) {
    _current_working_path = s;
  }

  void set_asset_search_paths(const std::vector<std::string> &search_paths) {
    _asset_search_paths = search_paths;
  }

  void set_asset_resolution_state(
    const std::string &cwp, const std::vector<std::string> &search_paths) {
    _current_working_path = cwp;
    _asset_search_paths = search_paths;
  }

 private:
  void CopyFrom(const PrimSpec &rhs) {
    _specifier = rhs._specifier;
    _typeName = rhs._typeName;
    _name = rhs._name;

    _children = rhs._children;

    _props = rhs._props;

    //_vsmap = rhs._vsmap;
    _current_vsmap = rhs._current_vsmap;

    _variantSets = rhs._variantSets;

    _primChildren = rhs._primChildren;
    _properties = rhs._properties;
    _variantChildren = rhs._variantChildren;

    _metas = rhs._metas ? std::make_unique<PrimMeta>(*rhs._metas) : nullptr;

    _current_working_path = rhs._current_working_path;
    _asset_search_paths = rhs._asset_search_paths;
  }

  void MoveFrom(PrimSpec &rhs) {
    _specifier = std::move(rhs._specifier);
    _typeName = std::move(rhs._typeName);
    _name = std::move(rhs._name);

    _children = std::move(rhs._children);

    _props = std::move(rhs._props);

    //_vsmap = std::move(rhs._vsmap);
    _current_vsmap = std::move(rhs._current_vsmap);

    _variantSets = std::move(rhs._variantSets);

    _primChildren = std::move(rhs._primChildren);
    _properties = std::move(rhs._properties);
    _variantChildren = std::move(rhs._variantChildren);

    _metas = std::move(rhs._metas);

    _current_working_path = rhs._current_working_path;
    _asset_search_paths = std::move(rhs._asset_search_paths);
  }

  Specifier _specifier{Specifier::Def};
  std::string _typeName;  // prim's typeName(e.g. "Xform", "Material") This is
                          // identitical to `typeName` in Crate format)
  std::string _name;      // elementName. Should not be empty.

  std::vector<PrimSpec> _children;  // child nodes

  PropertyMap _props;

  ///
  /// Variants
  ///
  /// variant element = Property or Prim
  ///
  using PrimSpecMap = std::map<std::string, PrimSpec>;

  //VariantSelectionMap _vsmap;  // Original variant selections
  VariantSelectionMap _current_vsmap;  // Currently selected variants

  std::map<std::string, VariantSetSpec> _variantSets;

  std::vector<value::token> _primChildren;  // List of child PrimSpec nodes
  std::vector<value::token> _properties;    // List of property names
  std::vector<value::token> _variantChildren;

  std::unique_ptr<PrimMeta> _metas;

  ///
  /// For solving asset path in nested composition.
  /// Keep asset resolution state.
  /// TODO: Use struct. Store userdata pointer.
  ///
  std::string _current_working_path;
  std::vector<std::string> _asset_search_paths;

};

namespace prim {

using PropertyMap = std::map<std::string, Property>;
// Single listop+items pair (used internally and in printing)
using ReferenceListOp = std::pair<ListEditQual, std::vector<Reference>>;
using PayloadListOp = std::pair<ListEditQual, std::vector<Payload>>;
// Full list supporting multiple listops
using ReferenceList = std::vector<ReferenceListOp>;
using PayloadList = std::vector<PayloadListOp>;

}  // namespace prim

} // namespace lightusd
