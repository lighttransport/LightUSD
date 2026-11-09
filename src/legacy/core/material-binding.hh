// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// material-binding.hh - MaterialBinding class
//
#pragma once

#include <map>
#include <string>

#include "nonstd/optional.hpp"
#include "ordered-dict.hh"
#include "relationship.hh"
#include "value-types.hh"

namespace lightusd {

// for bindMaterialAs
constexpr auto kWeakerThanDescendants = "weakerThanDescendants";
constexpr auto kStrongerThanDescendants = "strongerThanDescendants";

enum class MaterialBindingStrength
{
  WeakerThanDescendants, // default
  StrongerThanDescendants
};

// TODO: Move to pprinter.hh?
std::string to_string(const MaterialBindingStrength strength);

class MaterialBinding {
 public:
  MaterialBinding() = default;
  MaterialBinding(const MaterialBinding &) = default;
  MaterialBinding(MaterialBinding &&) noexcept = default;
  MaterialBinding &operator=(const MaterialBinding &) = default;
  MaterialBinding &operator=(MaterialBinding &&) noexcept = default;

  static value::token kAllPurpose() {
    return value::token("");
  }

  //
  // NOTE on material binding.
  // https://openusd.org/release/wp_usdshade.html
  //
  //  - "all purpose", direct binding, material:binding. single relationship target only
  //  - a purpose-restricted, direct, fallback binding, e.g. material:binding:preview
  //  - an all-purpose, collection-based binding, e.g. material:binding:collection:metalBits
  //  - a purpose-restricted, collection-based binding, e.g. material:binding:collection:full:metalBits
  //
  // In LightUSD, treat empty purpose token as "all purpose"
  //

  // Some frequently used materialBindings
  RelationshipProperty materialBinding; // material:binding
  RelationshipProperty materialBindingPreview; // material:binding:preview
  RelationshipProperty materialBindingFull; // material:binding:full

  value::token get_materialBindingStrength(const value::token &purpose);
  value::token get_materialBindingStrengthCollection(const value::token &collection_name, const value::token &purpose);

  bool has_materialBinding() const {
    return materialBinding.authored();
  }

  bool has_materialBindingPreview() const {
    return materialBindingPreview.authored();
  }

  bool has_materialBindingFull() const {
    return materialBindingFull.authored();
  }

  bool has_materialBinding(const value::token &mat_purpose) const {
    if (mat_purpose.str() == kAllPurpose().str()) {
      return has_materialBinding();
    } else if (mat_purpose.str() == "full") {
      return has_materialBindingFull();
    } else if (mat_purpose.str() == "preview") {
      return has_materialBindingPreview();
    } else {
      return _materialBindingMap.count(mat_purpose.str()) > 0;
    }
  }

  void clear_materialBinding() {
    materialBinding.reset();
  }

  void clear_materialBindingPreview() {
    materialBindingPreview.reset();
  }

  void clear_materialBindingFull() {
    materialBindingFull.reset();
  }

  void set_materialBinding(const Relationship &rel) {
    materialBinding = rel;
  }

  void set_materialBinding(const Relationship &rel, const MaterialBindingStrength strength) {
    value::token strength_tok(to_string(strength));
    materialBinding = rel;
    materialBinding.relationship().metas().set_bindMaterialAs(strength_tok);
  }

  void set_materialBindingPreview(const Relationship &rel) {
    materialBindingPreview = rel;
  }

  void set_materialBindingPreview(const Relationship &rel, const MaterialBindingStrength strength) {
    value::token strength_tok(to_string(strength));
    materialBindingPreview = rel;
    materialBindingPreview.relationship().metas().set_bindMaterialAs(strength_tok);
  }

  void set_materialBindingFull(const Relationship &rel) {
    materialBindingFull = rel;
  }

  void set_materialBindingFull(const Relationship &rel, const MaterialBindingStrength strength) {
    value::token strength_tok(to_string(strength));
    materialBindingFull = rel;
    materialBindingFull.relationship().metas().set_bindMaterialAs(strength_tok);
  }

  void set_materialBinding(const Relationship &rel, const value::token &mat_purpose) {

    if (mat_purpose.str().empty()) {
      return set_materialBinding(rel);
    } else if (mat_purpose.str() == "full") {
      return set_materialBindingFull(rel);
    } else if (mat_purpose.str() == "preview") {
      return set_materialBindingPreview(rel);
    } else {
      _materialBindingMap[mat_purpose.str()] = rel;
    }
  }

  void set_materialBinding(const Relationship &rel, const value::token &mat_purpose, const MaterialBindingStrength strength) {
    value::token strength_tok(to_string(strength));

    if (mat_purpose.str().empty()) {
      return set_materialBinding(rel, strength);
    } else if (mat_purpose.str() == "full") {
      return set_materialBindingFull(rel, strength);
    } else if (mat_purpose.str() == "preview") {
      return set_materialBindingPreview(rel, strength);
    } else {
      _materialBindingMap[mat_purpose.str()] = rel;
      _materialBindingMap[mat_purpose.str()].metas().set_bindMaterialAs(strength_tok);
    }
  }

  bool has_materialBindingCollection(const std::string &tok) {

    if (!_materialBindingCollectionMap.count(tok)) {
      return false;
    }

    return _materialBindingCollectionMap.count(tok) > 0;
  }

  void set_materialBindingCollection(const value::token &tok, const value::token &mat_purpose, const Relationship &rel) {

    // NOTE:
    // https://openusd.org/release/wp_usdshade.html#basic-proposal-for-collection-based-assignment
    // says: material:binding:collection defines a namespace of binding relationships to be applied in namespace order, with the earliest ordered binding relationship the strongest
    //
    // so the app is better first check if `tok` element alreasy exists(using has_materialBindingCollection)

    auto &m = _materialBindingCollectionMap[tok.str()];

    m.insert(mat_purpose.str(), rel);
  }

  void clear_materialBindingCollection(const value::token &tok, const value::token &mat_purpose) {
    if (_materialBindingCollectionMap.count(tok.str())) {
      _materialBindingCollectionMap[tok.str()].erase(mat_purpose.str());
    }
  }

  void set_materialBindingCollection(const value::token &tok, const value::token &mat_purpose, const Relationship &rel, MaterialBindingStrength strength) {
    value::token strength_tok(to_string(strength));

    Relationship r = rel;
    r.metas().set_bindMaterialAs(strength_tok);

    _materialBindingCollectionMap[tok.str()].insert(mat_purpose.str(), r);
  }

  const std::map<std::string, Relationship> &materialBindingMap() const
      LIGHTUSD_LIFETIMEBOUND {
    return _materialBindingMap;
  }

  const std::map<std::string, ordered_dict<Relationship>>
      &materialBindingCollectionMap() const LIGHTUSD_LIFETIMEBOUND {
    return _materialBindingCollectionMap;
  }

  bool get_materialBinding(const value::token &mat_purpose, Relationship *relOut) const {
    if (!relOut) {
      return false;
    }

    if (mat_purpose.str().empty()) {
      if (materialBinding.authored()) {
        (*relOut) = materialBinding.relationship();
        return true;
      } else {
        return false; // not authored
      }
    } else if (mat_purpose.str() == "full") {
      if (materialBindingFull.authored()) {
        (*relOut) = materialBindingFull.relationship();
        return true;
      } else {
        return false; // not authored
      }
    } else if (mat_purpose.str() == "preview") {
      if (materialBindingPreview.authored()) {
        (*relOut) = materialBindingPreview.relationship();
        return true;
      } else {
        return false; // not authored
      }
    } else {
      if (_materialBindingMap.count(mat_purpose.str())) {
        (*relOut) = _materialBindingMap.at(mat_purpose.str());
        return true;
      } else {
        return false; // not authored
      }
    }
  }

 private:

  // For material:binding(excludes frequently used `material:binding`, `material:binding:full` and `material:binding:preview`)
  // key = PURPOSE, value = rel
  std::map<std::string, Relationship> _materialBindingMap;

  // For material:binding:collection
  // Use ordered dict since the requests:
  //
  // https://openusd.org/release/wp_usdshade.html#basic-proposal-for-collection-based-assignment
  //
  // `...with the earliest ordered binding relationship the strongest`
  //
  // key = PURPOSE, value = map<NAME, Rel>
  // TODO: Use multi-index map
  std::map<std::string, ordered_dict<Relationship>> _materialBindingCollectionMap;
};

}  // namespace lightusd
