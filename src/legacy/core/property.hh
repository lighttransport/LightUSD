// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// property.hh - Property class (Attribute or Relationship)
//
#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "nonstd/optional.hpp"
#include "attribute.hh"
#include "relationship.hh"
#include "path.hh"
#include "prim-enums.hh"

namespace lightusd {

// Generic container for Attribute or Relation/Connection. And has this property
// is custom or not (Need to lookup schema if the property is custom or not for
// Crate data)
// Uses std::variant for efficient storage - only one of Attribute/Relationship
// is stored at a time, saving ~80-150 bytes per Property compared to dual storage.
// TODO: Deprecate `custom` attribute:
// https://github.com/PixarAnimationStudios/USD/issues/2069
class Property {
 public:
  // Simplified type enum:
  // - Empty: No data (monostate in variant)
  // - Attribute: Holds Attribute (may or may not have value/connection)
  // - Relationship: Holds Relationship (may or may not have targets)
  //
  // Legacy enum values mapped as:
  // - EmptyAttrib -> is_attribute() && !get_attribute().has_value() && !get_attribute().has_connections()
  // - Attrib -> is_attribute() && (get_attribute().has_value() || get_attribute().has_timesamples())
  // - Relation -> is_relationship() && get_relationship().has_value()
  // - NoTargetsRelation -> is_relationship() && !get_relationship().has_value()
  // - Connection -> is_attribute() && get_attribute().has_connections()
  enum class Type {
    EmptyAttrib,        // Attrib with no data. (DEPRECATED: use is_attribute() && !attr.has_value())
    Attrib,             // Attrib which contains actual data
    Relation,           // `rel` with targetPath(s).
    NoTargetsRelation,  // `rel` with no targets. (DEPRECATED: use is_relationship() && !rel.has_value())
    Connection,         // Connection attribute(`.connect` suffix). (DEPRECATED: use is_attribute_connection())
  };

  Property() = default;

  template <typename T>
  Property(bool custom = false) : _has_custom(custom) {
    Attribute a;
    a.set_type_name(value::TypeTraits<T>::type_name());
    _data = std::move(a);
  }

  static Property MakeEmptyAttrib(const std::string &type_name,
                                  bool custom = false) {
    Property p;
    p.set_custom(custom);
    Attribute a;
    a.set_type_name(type_name);
    p._data = std::move(a);
    return p;
  }

  Property(const Attribute &a, bool custom = false)
      : _data(a), _has_custom(custom) {
  }

  Property(Attribute &&a, bool custom = false)
      : _data(std::move(a)), _has_custom(custom) {
  }

  // Relationship(typeless)
  Property(const Relationship &r, bool custom = false)
      : _data(r), _has_custom(custom) {
    set_listedit_qual(r.get_listedit_qual());
  }

  // Relationship(typeless)
  Property(Relationship &&r, bool custom = false)
      : _has_custom(custom) {
    set_listedit_qual(r.get_listedit_qual());
    _data = std::move(r);
  }

  // Attribute Connection: has type
  Property(const Path &path, const std::string &prop_value_type_name,
           bool custom = false)
      : _has_custom(custom) {
    Attribute a;
    a.set_connection(path);
    a.set_type_name(prop_value_type_name);
    _data = std::move(a);
  }

  // Attribute Connection: has multiple targetPaths
  Property(const std::vector<Path> &paths,
           const std::string &prop_value_type_name, bool custom = false)
      : _has_custom(custom) {
    Attribute a;
    a.set_connections(paths);
    a.set_type_name(prop_value_type_name);
    _data = std::move(a);
  }

  // Copy constructor
  Property(const Property& rhs) = default;

  // Move constructor
  Property(Property&& rhs) noexcept = default;

  // Copy assignment operator
  Property& operator=(const Property& rhs) = default;

  // Move assignment operator
  Property& operator=(Property&& rhs) noexcept = default;

  bool is_attribute() const {
    return std::holds_alternative<Attribute>(_data);
  }

  bool is_empty() const {
    if (std::holds_alternative<std::monostate>(_data)) {
      return true;
    }
    if (is_attribute()) {
      const auto& a = std::get<Attribute>(_data);
      return !a.has_value() && !a.has_timesamples() && !a.has_connections();
    }
    if (is_relationship()) {
      return !std::get<Relationship>(_data).has_value();
    }
    return true;
  }

  bool is_relationship() const {
    return std::holds_alternative<Relationship>(_data);
  }

  bool is_attribute_connection() const {
    if (is_attribute()) {
      return std::get<Attribute>(_data).is_connection();
    }
    return false;
  }

  std::string value_type_name() const {
    if (is_relationship()) {
      // relation is typeless.
      return std::string();
    } else if (is_attribute()) {
      return std::get<Attribute>(_data).type_name();
    }
    return std::string();
  }

  bool has_custom() const { return _has_custom; }
  void set_custom(const bool onoff) { _has_custom = onoff; }

  // set_property_type: For backwards compatibility
  // This converts legacy Type enum values to the new variant storage
  void set_property_type(Type ty) {
    switch (ty) {
      case Type::EmptyAttrib:
        // If already an attribute, keep it but it's "empty"
        if (!is_attribute()) {
          _data = Attribute();
        }
        break;
      case Type::Attrib:
        // Should already have attribute set, just validate
        if (!is_attribute()) {
          _data = Attribute();
        }
        break;
      case Type::Connection:
        // Should already have attribute with connections set
        if (!is_attribute()) {
          _data = Attribute();
        }
        break;
      case Type::Relation:
        if (!is_relationship()) {
          _data = Relationship();
        }
        break;
      case Type::NoTargetsRelation:
        if (!is_relationship()) {
          _data = Relationship();
        }
        break;
    }
  }

  // get_property_type: For backwards compatibility
  // Maps the variant state back to legacy Type enum
  Type get_property_type() const {
    if (std::holds_alternative<std::monostate>(_data)) {
      return Type::EmptyAttrib;
    }
    if (is_attribute()) {
      const auto& a = std::get<Attribute>(_data);
      if (a.has_connections()) {
        return Type::Connection;
      }
      if (a.has_value() || a.has_timesamples()) {
        return Type::Attrib;
      }
      return Type::EmptyAttrib;
    }
    if (is_relationship()) {
      const auto& r = std::get<Relationship>(_data);
      if (r.has_value()) {
        return Type::Relation;
      }
      return Type::NoTargetsRelation;
    }
    return Type::EmptyAttrib;
  }

  void set_listedit_qual(ListEditQual qual) { _listOpQual = qual; }

  // get_attribute: Returns const reference to stored Attribute
  // Returns empty Attribute if not an attribute
  const Attribute &get_attribute() const LIGHTUSD_LIFETIMEBOUND {
    static const Attribute s_empty;
    const Attribute *p = std::get_if<Attribute>(&_data);
    return p ? *p : s_empty;
  }

  // attribute: Returns mutable reference to stored Attribute
  // Creates empty Attribute if not currently storing one
  Attribute &attribute() LIGHTUSD_LIFETIMEBOUND {
    if (!is_attribute()) {
      _data = Attribute();
    }
    return std::get<Attribute>(_data);
  }

  // Safe accessor - returns nullptr if not an attribute
  const Attribute* get_attribute_or_null() const LIGHTUSD_LIFETIMEBOUND {
    return std::get_if<Attribute>(&_data);
  }

  Attribute* get_attribute_or_null() LIGHTUSD_LIFETIMEBOUND {
    return std::get_if<Attribute>(&_data);
  }

  void set_attribute(const Attribute &attrib) {
    _data = attrib;
  }

  // get_relationship: Returns const reference to stored Relationship
  // Returns empty Relationship if not a relationship
  const Relationship &get_relationship() const LIGHTUSD_LIFETIMEBOUND {
    static const Relationship s_empty;
    const Relationship *p = std::get_if<Relationship>(&_data);
    return p ? *p : s_empty;
  }

  // relationship: Returns mutable reference to stored Relationship
  // Creates empty Relationship if not currently storing one
  Relationship &relationship() LIGHTUSD_LIFETIMEBOUND {
    if (!is_relationship()) {
      _data = Relationship();
    }
    return std::get<Relationship>(_data);
  }

  // Safe accessor - returns nullptr if not a relationship
  const Relationship* get_relationship_or_null() const LIGHTUSD_LIFETIMEBOUND {
    return std::get_if<Relationship>(&_data);
  }

  Relationship* get_relationship_or_null() LIGHTUSD_LIFETIMEBOUND {
    return std::get_if<Relationship>(&_data);
  }

  ///
  /// Convenient methods when Property is a Relationship
  ///

  ///
  /// Return single relationTarget path when Property is a Relationship.
  /// Return the first path when Relationship is composed of PathVector(multiple
  /// paths)
  ///
  nonstd::optional<Path> get_relationTarget() const {
    if (!is_relationship()) {
      return nonstd::nullopt;
    }
    const auto& rel = std::get<Relationship>(_data);
    if (rel.is_path()) {
      return rel.targetPath;
    } else if (rel.is_pathvector()) {
      if (rel.targetPathVector.size() > 0) {
        return rel.targetPathVector[0];
      }
    }
    return nonstd::nullopt;
  }

  ///
  /// Return multiple relationTarget paths when Property is a Relationship.
  /// Returns empty when Property is not a Relationship or a Relationship does
  /// not contain any target paths.
  ///
  std::vector<Path> get_relationTargets() const {
    std::vector<Path> pv;
    if (!is_relationship()) {
      return pv;
    }
    const auto& rel = std::get<Relationship>(_data);
    if (rel.is_path()) {
      pv.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      pv = rel.targetPathVector;
    }
    return pv;
  }

  ListEditQual get_listedit_qual() const { return _listOpQual; }

  size_t estimate_memory_usage() const;

  /// Estimate actual (size-based) memory usage.
  size_t estimate_actual_usage() const;

 private:
  // Variant storage: only one of monostate/Attribute/Relationship is active
  // This saves ~80-150 bytes compared to storing both Attribute and Relationship
  std::variant<std::monostate, Attribute, Relationship> _data;

  // List Edit qualifier(Attribute can never be list editable)
  ListEditQual _listOpQual{ListEditQual::ResetToExplicit};

  bool _has_custom{false};  // Qualified with 'custom' keyword? This will be
                            // deprecated though
};

}  // namespace lightusd
