// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// relationship.hh - Relationship class and TypedConnection template
//
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "nonstd/optional.hpp"
#include "path.hh"
#include "attr-metas.hh"
#include "prim-enums.hh"
#include "value-types.hh"

namespace lightusd {

//
// Relationship(typeless property)
//
class Relationship {
 public:
  Relationship() = default;
  Relationship(const Relationship &) = default;
  Relationship(Relationship &&) noexcept = default;
  Relationship &operator=(const Relationship &) = default;
  Relationship &operator=(Relationship &&) noexcept = default;

  // NOTE: no explicit `uniform` variability for Relationship
  // Relatinship have `uniform` variability implicitly.
  // (in Crate, variability is encoded as `uniform`)

  // (varying?) rel myrel    : DefineOnly(or empty)
  // (varying?) rel myrel = </a> : Path
  // (varying?) rel myrel = [</a>, </b>, ...] : PathVector
  // (varying?) rel myrel = None : ValueBlock
  //
  enum class Type { DefineOnly, Path, PathVector, ValueBlock };

  // TODO: move to private
  Type type{Type::DefineOnly};
  Path targetPath;
  std::vector<Path> targetPathVector;
  ListEditQual listOpQual{ListEditQual::ResetToExplicit};

  void set_listedit_qual(ListEditQual q) { listOpQual = q; }
  ListEditQual get_listedit_qual() const { return listOpQual; }

  void set_novalue() { type = Type::DefineOnly; }

  void set(const Path &p) {
    targetPath = p;
    type = Type::Path;
  }

  void set(const std::vector<Path> &pv) {
    targetPathVector = pv;
    type = Type::PathVector;
  }

  void set(std::vector<Path> &&pv) {
    targetPathVector = std::move(pv);
    type = Type::PathVector;
  }

  void set(const value::ValueBlock &v) {
    (void)v;
    type = Type::ValueBlock;
  }

  void set_blocked() { type = Type::ValueBlock; }

  bool has_value() const { return type != Type::DefineOnly; }

  bool is_path() const { return type == Type::Path; }

  bool is_pathvector() const { return type == Type::PathVector; }

  bool is_blocked() const { return type == Type::ValueBlock; }

  void set_varying_authored() { _varying_authored = true; }

  bool is_varying_authored() const { return _varying_authored; }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }

  size_t estimate_memory_usage() const;

  /// Estimate actual (size-based) memory usage.
  size_t estimate_actual_usage() const;

 private:
  AttrMeta _metas;

  // `varying` keyword is explicitly specified?
  bool _varying_authored{false};
};

//
// To represent Property which is explicitly Relationship(for builtin property)
//
// - When authored()
//   - !has_value() => "rel material:binding"
//   - has_value() => targetPath or array of targetPath. "rel material:binding =
//   </rel>" or "rel material:binding = [</rel1>, </rel2>]"
//   - is_blocked() => "rel material:binding = None"
//
class RelationshipProperty {
 public:
  RelationshipProperty() = default;
  RelationshipProperty(const RelationshipProperty &) = default;
  RelationshipProperty(RelationshipProperty &&) noexcept = default;
  RelationshipProperty &operator=(const RelationshipProperty &) = default;
  RelationshipProperty &operator=(RelationshipProperty &&) noexcept = default;

  RelationshipProperty(const Relationship &rel)
      : _authored(true), _relationship(rel) {}

  RelationshipProperty(const Path &p) { set(p); }

  RelationshipProperty(const std::vector<Path> &pv) { set(pv); }

  RelationshipProperty(const value::ValueBlock &v) { set(v); }

  void set_listedit_qual(ListEditQual q) { _relationship.set_listedit_qual(q); }
  ListEditQual get_listedit_qual() const {
    return _relationship.get_listedit_qual();
  }

  void set_authored() { _authored = true; }

  bool authored() const { return _authored; }

  // Clear the relationship and reset authored state
  void reset() {
    _authored = false;
    _relationship = Relationship();
  }

  // Declare-only: e.g. `rel myrel`
  void set_empty() {
    _relationship.set_novalue();
    _authored = true;
  }

  void set(const Path &p) {
    _relationship.set(p);
    _authored = true;
  }

  void set(const std::vector<Path> &pv) {
    _relationship.set(pv);
    _authored = true;
  }

  void set(std::vector<Path> &&pv) {
    _relationship.set(std::move(pv));
    _authored = true;
  }

  void set(const value::ValueBlock &v) {
    (void)v;
    _relationship.set_blocked();
    _authored = true;
  }

  void set_blocked() {
    _relationship.set_blocked();
    _authored = true;
  }

  const std::vector<Path> get_targetPaths() const {
    std::vector<Path> paths;
    if (_relationship.is_path()) {
      paths.push_back(_relationship.targetPath);
    } else if (_relationship.is_pathvector()) {
      paths = _relationship.targetPathVector;
    }
    return paths;
  }

  // TODO: Deprecate this direct access API to Relationship value?
  const Relationship &relationship() const LIGHTUSD_LIFETIMEBOUND {
    return _relationship;
  }

  Relationship &relationship() LIGHTUSD_LIFETIMEBOUND { return _relationship; }

  bool has_value() const { return _relationship.has_value(); }

  bool is_blocked() const { return _relationship.is_blocked(); }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND {
    return _relationship.metas();
  }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _relationship.metas(); }

 private:
  bool _authored{false};
  Relationship _relationship;
};

//
// TypedConnection is a typed version of Relationship
// example:
//
// token varname.connect = </Material/uv.name>
// float specular.connect = </Material/uv.specular>
// float specular:collection.connect = [</Material/uv.specular>,
// </Material/uv.specular_lod0>]
//
//
template <typename T>
class TypedConnection {
 public:
  using type = typename value::TypeTraits<T>::value_type;

  static std::string type_name() { return value::TypeTraits<T>::type_name(); }

  void set_listedit_qual(ListEditQual q) { _listOpQual = q; }
  ListEditQual get_listedit_qual() const { return _listOpQual; }

  // Define-only: token output:surface
  void set_empty() { _authored = true; }

  void set(const Path &p) {
    _targetPaths.clear();
    _targetPaths.push_back(p);
    _authored = true;
  }

  void set(const std::vector<Path> &pv) {
    _targetPaths = pv;
    _authored = true;
  }

  void set(const value::ValueBlock &v) {
    (void)v;
    _blocked = true;
    _authored = true;
  }

  void set_blocked() {
    _blocked = true;
    _authored = true;
  }

  const std::vector<Path> &get_connections() const LIGHTUSD_LIFETIMEBOUND {
    return _targetPaths;
  }

  bool authored() const { return _authored; }

  bool has_value() const { return _targetPaths.size(); }

  bool is_blocked() const { return _blocked; }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }

 private:
  std::vector<Path> _targetPaths;
  bool _authored{false};
  bool _blocked{false};
  AttrMeta _metas;
  ListEditQual _listOpQual{ListEditQual::ResetToExplicit};
};

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Relationship, "Relationship", TYPE_ID_RELATIONSHIP, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
