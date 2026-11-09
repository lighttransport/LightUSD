// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// variant-types.hh - Variant, VariantSet, VariantSetSpec types
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "prim-metas.hh"
#include "property.hh"

namespace lightusd {

// Forward declarations
class Prim;
class PrimSpec;
struct VariantSet;

// TODO: deprecate this and use PrimSpec for variantSet statement.
// Variant item in VariantSet.
// Variant can contain Prim metas, Prim tree and properties.
struct Variant {
  Variant() = default;
  Variant(const Variant &) = default;
  Variant(Variant &&) noexcept = default;
  Variant &operator=(const Variant &) = default;
  Variant &operator=(Variant &&) noexcept = default;

  // const std::string &name() const { return _name; }
  // std::string &name() { return _name; }

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }

  std::map<std::string, Property> &properties() LIGHTUSD_LIFETIMEBOUND {
    return _props;
  }
  const std::map<std::string, Property> &properties() const
      LIGHTUSD_LIFETIMEBOUND {
    return _props;
  }

  const std::vector<Prim> &primChildren() const LIGHTUSD_LIFETIMEBOUND {
    return _primChildren;
  }
  std::vector<Prim> &primChildren() LIGHTUSD_LIFETIMEBOUND {
    return _primChildren;
  }

  // For nested variantSet
  const std::map<std::string, VariantSet> &variantSets() const
      LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }
  std::map<std::string, VariantSet> &variantSets() LIGHTUSD_LIFETIMEBOUND {
    return _variantSets;
  }

 private:
  std::map<std::string, VariantSet> _variantSets;

  std::map<std::string, Property> _props;

  PrimMeta _metas;

  // We represent Prim children as `Prim` for a while.
  // TODO: Use PrimNode or PrimSpec?
  std::vector<Prim> _primChildren;
};


struct VariantSet {
  VariantSet() = default;
  VariantSet(const VariantSet &) = default;
  VariantSet(VariantSet &&) noexcept = default;
  VariantSet &operator=(const VariantSet &) = default;
  VariantSet &operator=(VariantSet &&) noexcept = default;

  // variantSet name = {
  //   "variant1" ...
  //   "variant2" ...
  //   ...
  // }

  std::string name;
  std::map<std::string, Variant> variantSet;
};

// For variantSet statement in PrimSpec(composition).
struct VariantSetSpec
{
  VariantSetSpec() = default;
  VariantSetSpec(const VariantSetSpec &) = default;
  VariantSetSpec(VariantSetSpec &&) noexcept = default;
  VariantSetSpec &operator=(const VariantSetSpec &) = default;
  VariantSetSpec &operator=(VariantSetSpec &&) noexcept = default;

  std::string name;
  std::map<std::string, PrimSpec> variantSet;
};

}  // namespace lightusd
