// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// USDCReader::Impl prim-field parsing — split out of usdc-reader-prim.cc.
// ParsePrimSpec / ParseVariantSetFields / ParseCommonPrimFields decode a prim's
// crate FieldValuePairVector into PrimMeta/PropertyMap. Separated from the prim
// dispatch/reconstruct (ToAPISchemas/ReconstrcutStageMeta/ReconstructPrimFromTypeName,
// which stay in usdc-reader-prim.cc with detail.inc) for faster single-TU rebuilds.
// All are USDCReader::Impl methods declared in usdc-reader-impl.hh.
#include "usdc-reader-impl.hh"

namespace lightusd {
namespace usdc {

bool USDCReader::Impl::ParsePrimSpec(const crate::FieldValuePairVector &fvs,
                                     nonstd::optional<std::string> &typeName,
                                     nonstd::optional<Specifier> &specifier,
                                     std::vector<value::token> &primChildren,
                                     std::vector<value::token> &properties,
                                     PrimMeta &primMeta) {
  // Fields for Prim and Prim metas.
  for (const auto &fv : fvs) {
    if (fv.first == "typeName") {
      if (auto pv = fv.second.as<value::token>()) {
        typeName = pv->str();
        DCOUT("typeName = " << typeName.value());
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`typeName` must be type `token`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "specifier") {
      if (auto pv = fv.second.as<Specifier>()) {
        specifier = (*pv);
        DCOUT("specifier = " << to_string(specifier.value()));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`specifier` must be type `Specifier`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "properties") {
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        properties = (*pv);
        DCOUT("properties = " << properties);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`properties` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "primChildren") {
      // Crate only
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        primChildren = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`primChildren` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "primOrder") {
      // `reorder nameChildren = [...]`. pxr keeps the body statement verbatim as
      // a token vector under primOrder (NOT primChildren, which is the full
      // child-name list).
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        primMeta.nameChildrenReorder = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`primOrder` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "propertyOrder") {
      // `reorder properties = [...]`
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        primMeta.propertiesReorder = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`propertyOrder` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "active") {
      if (auto pv = fv.second.as<bool>()) {
        primMeta.set_active(*pv);
        DCOUT("active = " << to_string(primMeta.get_active()));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`active` must be type `bool`, but got type `"
                                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "hidden") {
      if (auto pv = fv.second.as<bool>()) {
        primMeta.set_hidden(*pv);
        DCOUT("hidden = " << to_string(primMeta.get_hidden()));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`hidden` must be type `bool`, but got type `"
                                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "instanceable") {
      if (auto pv = fv.second.as<bool>()) {
        primMeta.set_instanceable(*pv);
        DCOUT("instanceable = " << to_string(primMeta.get_instanceable()));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`instanceable` must be type `bool`, but got type `"
                                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "assetInfo") {
      if (auto pv = fv.second.as<CustomDataType>()) {
        primMeta.set_assetInfo(*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`assetInfo` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "clips") {
      if (auto pv = fv.second.as<CustomDataType>()) {
        primMeta.set_clips(*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`clips` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "kind") {
      if (auto pv = fv.second.as<value::token>()) {

          const value::token tok = (*pv);
          if (tok.str() == "subcomponent") {
            primMeta.set_kind(Kind::Subcomponent);
          } else if (tok.str() == "component") {
            primMeta.set_kind(Kind::Component);
          } else if (tok.str() == "model") {
            primMeta.set_kind(Kind::Model);
          } else if (tok.str() == "group") {
            primMeta.set_kind(Kind::Group);
          } else if (tok.str() == "assembly") {
            primMeta.set_kind(Kind::Assembly);
          } else if (tok.str() == "sceneLibrary") {
            primMeta.set_kind(Kind::SceneLibrary);
          } else {
            primMeta.set_kind(tok.str());
          }
      } else {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "`kind` must be type `token`, but got type `"
                                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "apiSchemas") {
      if (auto pv = fv.second.as<ListOp<value::token>>()) {
        auto listop = (*pv);

        std::string warn;
        auto ret = ToAPISchemas(listop, _config.allow_unknown_apiSchemas, warn);
        if (!ret) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag, "Failed to validate `apiSchemas`: " + ret.error());
        } else {
          if (warn.size()) {
            PUSH_WARN(warn);
          }
          primMeta.set_apiSchemas(*ret);
        }
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`apiSchemas` must be type `ListOp[Token]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "documentation") {
      if (auto pv = fv.second.as<std::string>()) {
        value::StringData s;
        s.value = unwrap(*pv);
        s.is_triple_quoted = hasNewline(s.value);
        primMeta.set_doc(s);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`documentation` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "comment") {
      if (auto pv = fv.second.as<std::string>()) {
        value::StringData s;
        s.value = unwrap(*pv);
        s.is_triple_quoted = hasNewline(s.value);
        primMeta.set_comment(s);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`comment` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "sdrMetadata") {
      if (auto pv = fv.second.as<CustomDataType>()) {
        primMeta.set_sdrMetadata(*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`sdrMetadata` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "customData") {
      if (auto pv = fv.second.as<CustomDataType>()) {
        primMeta.set_customData(*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`customData` must be type `dictionary`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "variantSelection") {
      if (auto pv = fv.second.as<VariantSelectionMap>()) {
        primMeta.variants = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`variantSelection` must be type `variants`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "variantChildren") {
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        primMeta.variantChildren = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`variantChildren` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }

    } else if (fv.first == "variantSetChildren") {
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        primMeta.variantSetChildren = (*pv);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`variantSetChildren` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }

    } else if (fv.first == "variantSetNames") {
      if (auto pv = fv.second.as<ListOp<std::string>>()) {
        const ListOp<std::string> &p = *pv;
        DCOUT("variantSetNames = " << to_string(p));

        auto ps = DecodeListOp<std::string>(p);
        primMeta.variantSets = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag,
            "`variantSetNames` must be type `ListOp[String]`, but got type `"
                << fv.second.type_name() << "`");
      }
    } else if (fv.first == "sceneName") {  // USDZ extension
      if (auto pv = fv.second.as<std::string>()) {
        primMeta.set_sceneName(unwrap(*pv));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`sceneName` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "displayName") {  // USD supported since 23.xx?
      if (auto pv = fv.second.as<std::string>()) {
        primMeta.set_displayName(unwrap(*pv));
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`displayName` must be type `string`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "inherits") {  // `inherits` composition
      if (auto pvb = fv.second.as<value::ValueBlock>()) {
        (void)pvb;
        primMeta.inherits = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
        primMeta.inherits->push_back(
            std::make_pair(ListEditQual::ResetToExplicit, std::vector<Path>()));
      } else if (auto pv = fv.second.as<ListOp<Path>>()) {
        const ListOp<Path> &p = *pv;
        DCOUT("inherits = " << to_string(p));
        auto ps = DecodeListOp<Path>(p);
        primMeta.inherits = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`inherits` must be type `path` o `path[]`, but got type `"
                      << fv.second.type_name() << "`");
      }

    } else if (fv.first == "references") {  // `references` composition
      if (auto pvb = fv.second.as<value::ValueBlock>()) {
        (void)pvb;
        primMeta.references = std::vector<std::pair<ListEditQual, std::vector<Reference>>>();
        primMeta.references->push_back(
            std::make_pair(ListEditQual::ResetToExplicit, std::vector<Reference>()));
      } else if (auto pv = fv.second.as<ListOp<Reference>>()) {
        const ListOp<Reference> &p = *pv;
        DCOUT("references = " << to_string(p));
        auto ps = DecodeListOp<Reference>(p);
        primMeta.references = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag,
            "`references` must be type `ListOp[Reference]`, but got type `"
                << fv.second.type_name() << "`");
      }
    } else if (fv.first == "payload") {  // `payload` composition
      if (auto pvb = fv.second.as<value::ValueBlock>()) {
        (void)pvb;
        primMeta.payload = std::vector<std::pair<ListEditQual, std::vector<Payload>>>();
        primMeta.payload->push_back(
            std::make_pair(ListEditQual::ResetToExplicit, std::vector<Payload>()));
      } else if (auto pv = fv.second.as<Payload>()) {
        // pxr's crate encoding of `payload = None` is a single Payload with
        // empty asset path and no prim path (see Payload::is_none()), NOT a
        // ValueBlock and not a payload entry. Keep the empty explicit list-op
        // the ASCII parser produces for the same text -- storing the empty
        // Payload as an item made the writer emit `payload = <>`, which pxr
        // reads back as a payload with empty paths.
        std::vector<Payload> pls;
        if (!pv->is_none()) {
          pls.push_back(*pv);
        }
        primMeta.payload = std::vector<std::pair<ListEditQual, std::vector<Payload>>>();
        primMeta.payload->push_back(std::make_pair(ListEditQual::ResetToExplicit, pls));
      } else if (auto pvs = fv.second.as<ListOp<Payload>>()) {
        const ListOp<Payload> &p = *pvs;
        DCOUT("payload = " << to_string(p));
        auto ps = DecodeListOp<Payload>(p);
        primMeta.payload = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag,
            "`payload` must be type `ListOp[Payload]`, but got type `"
                << fv.second.type_name() << "`");
      }
    } else if (fv.first == "specializes") {  // `specializes` composition
      if (auto pv = fv.second.as<ListOp<Path>>()) {
        const ListOp<Path> &p = *pv;
        DCOUT("specializes = " << to_string(p));
        auto ps = DecodeListOp<Path>(p);
        primMeta.specializes = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`specializes` must be type `ListOp[Path]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else if (fv.first == "inheritPaths") {  // `inherits` composition (alternate field name)
      if (auto pv = fv.second.as<ListOp<Path>>()) {
        const ListOp<Path> &p = *pv;
        DCOUT("inheritPaths = " << to_string(p));
        auto ps = DecodeListOp<Path>(p);
        primMeta.inherits = ps;
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`inheritPaths` must be type `ListOp[Path]`, but got type `"
                      << fv.second.type_name() << "`");
      }

    } else if (endsWith(fv.first, ".targetPaths")) {
      if (auto pv = fv.second.as<std::vector<Path>>()) {
        DCOUT("Relationship " << fv.first << " = " << to_string(*pv));
        primMeta.unregisteredMetas[fv.first] = to_string(*pv);
      } else {
        PUSH_WARN("Relationship targetPaths field `" << fv.first << "` is not Path vector type (got " << fv.second.type_name() << "). Ignoring.");
      }
    } else {
      MetaVariable unknown_meta;
      unknown_meta.set_value(fv.first, fv.second.get_raw());
      primMeta.data()[fv.first] = std::move(unknown_meta);

      if (auto pv = fv.second.as<std::string>()) {
        primMeta.unregisteredMetas[fv.first] = (*pv);
      } else if (auto ptv = fv.second.as<value::token>()) {
        primMeta.unregisteredMetas[fv.first] = quote((*ptv).str());
      } else {
        DCOUT("Preserved unknown prim metadata: " << fv.first);
        PUSH_WARN("Preserved unknown prim metadata: " << fv.first);
      }
    }
  }

  return true;
}

bool USDCReader::Impl::ParseVariantSetFields(
    const crate::FieldValuePairVector &fvs,
    std::vector<value::token> &variantChildren) {
  for (const auto &fv : fvs) {
    if (fv.first == "variantChildren") {
      if (auto pv = fv.second.as<std::vector<value::token>>()) {
        variantChildren = (*pv);
        DCOUT("variantChildren: " << variantChildren);
      } else {
        PUSH_ERROR_AND_RETURN_TAG(
            kTag, "`variantChildren` must be type `token[]`, but got type `"
                      << fv.second.type_name() << "`");
      }
    } else {
      DCOUT("Unknown/invalid field in VariantSet: " << fv.first);
      PUSH_WARN("Ignoreing unknown/invalid field in VariantSet: " << fv.first);
    }
  }

  return true;
}

bool USDCReader::Impl::ParseCommonPrimFields(
    const crate::FieldValuePairVector &fvs,
    int current,
    Specifier default_spec,
    PrimFieldsResult *result) {

  if (!result) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "(Internal error) null PrimFieldsResult.");
  }

  // 1. Parse fields from the spec
  if (!ParsePrimSpec(fvs, result->typeName, result->specifier,
                     result->primChildren, result->properties,
                     result->primMeta)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to parse Prim fields.");
  }

  // 2. Resolve element path
  if (const auto &pv = GetElemPath(crate::Index(uint32_t(current)))) {
    DCOUT(fmt::format("Element path: {}", pv.value().full_path_name()));
    result->elemPath = pv.value();
  } else {
    PUSH_ERROR_AND_RETURN_TAG(kTag,
                              "(Internal errror) Element path not found.");
  }

  // 3. Validate / default specifier
  if (result->specifier) {
    if (result->specifier.value() == Specifier::Def) {
      // ok
    } else if (result->specifier.value() == Specifier::Class) {
      // ok
    } else if (result->specifier.value() == Specifier::Over) {
      // ok
    } else {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid Specifier.");
    }
  } else {
    result->specifier = default_spec;
  }

  // 4. Determine LightUSD prim type name
  if (!result->typeName) {
    result->pTyName = "Model";
  } else {
    result->pTyName = result->typeName.value();
  }

  // 5. Derive prim_name and primTypeName from element path
  result->prim_name = result->elemPath.prim_part();
  result->primTypeName = result->typeName.has_value()
                             ? result->typeName.value()
                             : "";

  // __AnyType__ normalisation
  if (result->typeName.has_value() &&
      result->typeName.value() == "__AnyType__") {
    result->primTypeName = "";
  }

  return true;
}

}  // namespace usdc
}  // namespace lightusd
