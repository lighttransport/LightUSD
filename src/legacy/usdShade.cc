// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// UsdGeom API implementations

#include <sstream>

#include "usdShade.hh"
#include "str-util.hh"

#include "common-macros.inc"

namespace lightusd {

std::string to_string(const MaterialBindingStrength strength) {
  switch (strength) {
    case MaterialBindingStrength::WeakerThanDescendants: {
      return kWeakerThanDescendants;
    }
    case MaterialBindingStrength::StrongerThanDescendants: {
      return kStrongerThanDescendants;
    }
  }

  return "[[Invalid MaterialBindingStrength]]";
}

bool UsdShadePrim::has_sdr_metadata(const std::string &key) {
  if (!metas().has_sdrMetadata()) {
    return false;
  }

  const Dictionary dict = metas().get_sdrMetadata();

  if (!HasCustomDataKey(dict, key)) {
    return false;
  }

  // check the type of value.
  MetaVariable value;
  if (!GetCustomDataByKey(dict, key, &value)) {
    return false;
  }

  if (value.type_id() != value::TypeTraits<std::string>::type_id()) {
    return false;
  }

  return true;
}

const std::string UsdShadePrim::get_sdr_metadata(const std::string &key) {
  std::string svalue;

  if (metas().has_sdrMetadata()) {

    const Dictionary dict = metas().get_sdrMetadata();

    if (HasCustomDataKey(dict, key)) {

      // check the type of value.
      MetaVariable var;
      if (GetCustomDataByKey(dict, key, &var)) {

        if (var.type_id() == value::TypeTraits<std::string>::type_id()) {
          if (!var.get_value(&svalue)) {
            svalue = std::string();
          }
        }

      }
    }
  }


  return svalue;
}

bool UsdShadePrim::set_sdr_metadata(const std::string &key, const std::string &sdr_value) {

  Dictionary dict = metas().get_sdrMetadata();
  bool ret = SetCustomDataByKey(key, sdr_value, dict);
  if (ret) {
    metas().set_sdrMetadata(dict);
  }
  return ret;
}

value::token MaterialBinding::get_materialBindingStrength(const value::token &purpose) {

  if (purpose.str() == kAllPurpose().str()) {
    if (materialBinding.authored() && materialBinding.relationship().metas().has_bindMaterialAs()) {
      return materialBinding.relationship().metas().get_bindMaterialAs();
    }
  } else if (purpose.str() == "full") {
    if (materialBindingFull.authored() && materialBindingFull.relationship().metas().has_bindMaterialAs()) {
      return materialBindingFull.relationship().metas().get_bindMaterialAs();
    }
  } else if (purpose.str() == "preview") {
    if (materialBindingPreview.authored() && materialBindingPreview.relationship().metas().has_bindMaterialAs()) {
      return materialBindingPreview.relationship().metas().get_bindMaterialAs();
    }
  } else {
    if (_materialBindingMap.count(purpose.str())) {
      const auto &m = _materialBindingMap.at(purpose.str());
      if (m.metas().has_bindMaterialAs()) {
        return m.metas().get_bindMaterialAs();
      }
    }
  }

  return value::token(kWeakerThanDescendants);
}

value::token MaterialBinding::get_materialBindingStrengthCollection(const value::token &coll_name, const value::token &purpose) {

  if (coll_name.str().empty()) {
    return get_materialBindingStrength(purpose);
  }

  if (_materialBindingCollectionMap.count(coll_name.str())) {
    const auto &coll_mb = _materialBindingCollectionMap.at(coll_name.str());

    if (coll_mb.count(purpose.str())) {
      const Relationship *prel{nullptr};
      if (coll_mb.at(purpose.str(), &prel)) {
        if (prel->metas().has_bindMaterialAs()) {
          return prel->metas().get_bindMaterialAs();
        }
      }
    }
  }

  return value::token(kWeakerThanDescendants);
}

namespace {

//constexpr auto kPrimvarPrefix = "primvar::";

} // namespace


} // namespace lightusd


