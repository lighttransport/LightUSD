// SPDX-License-Identifier: Apache 2.0
// Copyright 2020 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDC reader: Prim spec parsing, StageMeta, type dispatch
// (extracted from usdc-reader.cc)
//

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "usdc-reader-impl.hh"
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"
#include "mjcPhysics.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDC_READER)

#include "usdc-reader-prim-detail.inc"

namespace lightusd {
namespace usdc {

nonstd::expected<APISchemas, std::string> USDCReader::Impl::ToAPISchemas(
    const ListOp<value::token> &arg, bool ignore_unknown, std::string &warn) {
  APISchemas schemas;
  (void)warn;  // unknown-API-schema preservation is a debug note, not a warning

  // Resolve a list of schema tokens into the resolved view
  // (schemas.names/unknownSchemas for additive ops, deleted* for delete) and
  // record the op verbatim in `authoredOps` for lossless round-trip.
  auto ProcessItems = [&](const std::vector<value::token> &items,
                          ListEditQual qual)
      -> nonstd::expected<bool, std::string> {
    const bool isDelete = (qual == ListEditQual::Delete);
    std::vector<std::pair<std::string, std::string>> authoredItems;
    for (const auto &item : items) {
      // Preserve the full token (incl. any `:instance` suffix) for round-trip.
      authoredItems.push_back({item.str(), std::string()});
      // Resolved view: split multi-apply instances `SchemaName:instanceName`.
      if (auto pv = enum_handler::APISchemaNameWithInstanceOpt(item.str())) {
        const std::pair<APISchemas::APIName, std::string> entry = pv.value();
        if (isDelete) {
          schemas.deletedNames.push_back(entry);
          schemas.names.erase(
              std::remove(schemas.names.begin(), schemas.names.end(), entry),
              schemas.names.end());
        } else {
          schemas.names.push_back(entry);
        }
      } else if (ignore_unknown) {
        // Unknown base schema: keep the full token verbatim.
        const std::pair<std::string, std::string> entry{item.str(), std::string()};
        if (isDelete) {
          schemas.deletedUnknownSchemas.push_back(entry);
          schemas.unknownSchemas.erase(
              std::remove(schemas.unknownSchemas.begin(), schemas.unknownSchemas.end(), entry),
              schemas.unknownSchemas.end());
        } else {
          schemas.unknownSchemas.push_back(entry);
          // Preserved (round-tripped) intentionally; not a warning (OpenUSD is silent).
          DCOUT("Preserving unknown API schema: " << item.str());
        }
      } else {
        return nonstd::make_unexpected("Invalid or Unsupported API schema: " +
                                       item.str());
      }
    }
    schemas.authoredOps.push_back({qual, std::move(authoredItems)});
    // First non-delete qualifier wins for the resolved single-qualifier view.
    if (!isDelete && schemas.listOpQual == ListEditQual::ResetToExplicit) {
      schemas.listOpQual = qual;
    }
    return true;
  };

  if (arg.IsExplicit()) {  // fast path
    if (arg.GetExplicitItems().empty()) {
      // `apiSchemas = None` (explicit empty list).
      schemas.explicitlyEmpty = true;
    }
    auto r = ProcessItems(arg.GetExplicitItems(), ListEditQual::ResetToExplicit);
    if (!r) return nonstd::make_unexpected(r.error());
    if (schemas.explicitlyEmpty) {
      schemas.authoredOps.clear();  // None has no items to author
    }

  } else {
    // USD allows multiple list-edit qualifiers on the same field (e.g.
    // `delete` + `prepend`). Process each present group, preserving it in
    // `authoredOps`. Canonical apply order: delete, add, append, prepend,
    // order.
    struct { const std::vector<value::token>& items; ListEditQual qual; } groups[] = {
      // Some writers populate explicit items without setting the IsExplicit()
      // flag (e.g. apiSchemas round-tripped through USDC). Handle that here so
      // an explicit-but-unflagged list is not mistaken for an empty ListOp.
      {arg.GetExplicitItems(), ListEditQual::ResetToExplicit},
      {arg.GetDeletedItems(), ListEditQual::Delete},
      {arg.GetAddedItems(), ListEditQual::Add},
      {arg.GetAppendedItems(), ListEditQual::Append},
      {arg.GetPrependedItems(), ListEditQual::Prepend},
      {arg.GetOrderedItems(), ListEditQual::Order},
    };
    bool any = false;
    for (const auto &g : groups) {
      if (g.items.empty()) continue;
      any = true;
      auto r = ProcessItems(g.items, g.qual);
      if (!r) return nonstd::make_unexpected(r.error());
    }
    if (!any) {
      return nonstd::make_unexpected("Internal error: ListOp conversion.");
    }
  }

  return std::move(schemas);
}



bool USDCReader::Impl::ReconstrcutStageMeta(
    const crate::FieldValuePairVector &fvs, StageMetas *metas) {

  std::vector<std::string> subLayers;
  std::vector<LayerOffset> subLayerOffsets;

  for (const auto &fv : fvs) {
    if (fv.first == "upAxis") {
      auto vt = fv.second.get_value<value::token>();
      if (!vt) {
        PUSH_ERROR_AND_RETURN("`upAxis` must be `token` type.");
      }

      std::string v = vt.value().str();
      if (v == "Y") {
        metas->upAxis = Axis::Y;
      } else if (v == "Z") {
        metas->upAxis = Axis::Z;
      } else if (v == "X") {
        metas->upAxis = Axis::X;
      } else {
        PUSH_ERROR_AND_RETURN("`upAxis` must be 'X', 'Y' or 'Z' but got '" + v +
                              "'(note: Case sensitive)");
      }
      DCOUT("upAxis = " << to_string(metas->upAxis.get_value()));

    } else if (fv.first == "metersPerUnit") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->metersPerUnit = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->metersPerUnit = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`metersPerUnit` value must be double or float type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("metersPerUnit = " << metas->metersPerUnit.get_value());
    } else if (fv.first == "kilogramsPerUnit") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->kilogramsPerUnit = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->kilogramsPerUnit = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`kilogramsPerUnit` value must be double or float type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("kilogramsPerUnit = " << metas->kilogramsPerUnit.get_value());
    } else if (fv.first == "timeCodesPerSecond") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->timeCodesPerSecond = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->timeCodesPerSecond = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`timeCodesPerSecond` value must be double or float "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("timeCodesPerSecond = " << metas->timeCodesPerSecond.get_value());
    } else if (fv.first == "startTimeCode") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->startTimeCode = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->startTimeCode = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`startTimeCode` value must be double or float "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("startimeCode = " << metas->startTimeCode.get_value());
    } else if (fv.first == "subLayers") {
      if (auto vs = fv.second.get_value<std::vector<std::string>>()) {
        subLayers = vs.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`subLayers` value must be string[] "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
    } else if (fv.first == "subLayerOffsets") {
      if (auto vs = fv.second.get_value<std::vector<LayerOffset>>()) {
        subLayerOffsets = vs.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`subLayerOffsets` value must be LayerOffset[] "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
    } else if (fv.first == "relocates") {
      // AOUSD Core Spec 10.3.2.6: layer-level relocates (Crate type 58).
      if (auto vr =
              fv.second.get_value<std::vector<std::pair<Path, Path>>>()) {
        metas->layerRelocates = vr.value();
        DCOUT("relocates = " << metas->layerRelocates.size() << " pair(s)");
      } else {
        PUSH_ERROR_AND_RETURN(
            "`relocates` value must be SdfRelocates "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
    } else if (fv.first == "endTimeCode") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->endTimeCode = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->endTimeCode = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`endTimeCode` value must be double or float "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("endTimeCode = " << metas->endTimeCode.get_value());
    } else if (fv.first == "framesPerSecond") {
      if (auto vf = fv.second.get_value<float>()) {
        metas->framesPerSecond = double(vf.value());
      } else if (auto vd = fv.second.get_value<double>()) {
        metas->framesPerSecond = vd.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`framesPerSecond` value must be double or float "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("framesPerSecond = " << metas->framesPerSecond.get_value());
    } else if (fv.first == "autoPlay") {
      if (auto vf = fv.second.get_value<bool>()) {
        metas->autoPlay = vf.value();
      } else if (auto vs = fv.second.get_value<std::string>()) {
        bool autoPlay{true};
        if (vs.value() == "true") {
          autoPlay = true;
        } else if (vs.value() == "false") {
          autoPlay = false;
        } else {
          PUSH_ERROR_AND_RETURN(
              "Unsupported value for `autoPlay`: " << vs.value());
        }
        metas->autoPlay = autoPlay;
      } else {
        PUSH_ERROR_AND_RETURN(
            "`autoPlay` value must be bool "
            "type or string type, but got '" +
            fv.second.type_name() + "'");
      }
      DCOUT("autoPlay = " << metas->autoPlay.get_value());
    } else if (fv.first == "playbackMode") {
      if (auto vf = fv.second.get_value<value::token>()) {
        if (vf.value().str() == "none") {
          metas->playbackMode = StageMetas::PlaybackMode::PlaybackModeNone;
        } else if (vf.value().str() == "loop") {
          metas->playbackMode = StageMetas::PlaybackMode::PlaybackModeLoop;
        } else {
          PUSH_ERROR_AND_RETURN("Unsupported token value for `playbackMode`.");
        }
      } else if (auto vs = fv.second.get_value<std::string>()) {
        std::string val = vs.value();
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
          val = val.substr(1, val.size() - 2);
        }
        if (val == "none") {
          metas->playbackMode = StageMetas::PlaybackMode::PlaybackModeNone;
        } else if (val == "loop") {
          metas->playbackMode = StageMetas::PlaybackMode::PlaybackModeLoop;
        } else {
          PUSH_ERROR_AND_RETURN(
              "Unsupported value for `playbackMode`: " << val);
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            "`playbackMode` value must be token "
            "type, but got '" +
            fv.second.type_name() + "'");
      }
    } else if ((fv.first == "defaultPrim")) {
      auto v = fv.second.get_value<value::token>();
      if (!v) {
        PUSH_ERROR_AND_RETURN("`defaultPrim` must be `token` type.");
      }

      metas->defaultPrim = v.value();
      DCOUT("defaultPrim = " << metas->defaultPrim.str());
    } else if (fv.first == "customLayerData") {
      if (auto v = fv.second.get_value<CustomDataType>()) {
        metas->customLayerData = v.value();
        metas->customLayerDataAuthored = true;
      } else {
        PUSH_ERROR_AND_RETURN(
            "customLayerData must be `dictionary` type, but got type `" +
            fv.second.type_name());
      }
    } else if (fv.first == "primChildren") {  // only appears in USDC.
      auto v = fv.second.get_value<std::vector<value::token>>();
      if (!v) {
        PUSH_ERROR_AND_RETURN("Type must be `token[]` for `primChildren`, but got " +
                   fv.second.type_name());
      }

      metas->primChildren = v.value();
    } else if (fv.first == "documentation") {  // 'doc'
      auto v = fv.second.get_value<std::string>();
      if (!v) {
        PUSH_ERROR_AND_RETURN("Type must be `string` for `documentation`, but got " +
                   fv.second.type_name());
      }
      value::StringData sdata;
      sdata.value = v.value();
      sdata.is_triple_quoted = hasNewline(sdata.value);
      metas->doc = sdata;
      DCOUT("doc = " << metas->doc.value);
    } else if (fv.first == "comment") {  // 'comment'
      auto v = fv.second.get_value<std::string>();
      if (!v) {
        PUSH_ERROR_AND_RETURN("Type must be `string` for `comment`, but got " +
                   fv.second.type_name());
      }
      value::StringData sdata;
      sdata.value = v.value();
      sdata.is_triple_quoted = hasNewline(sdata.value);
      metas->comment = sdata;
      DCOUT("comment = " << metas->comment.value);
    } else if (fv.first == "colorConfiguration") {
      if (auto v = fv.second.get_value<value::AssetPath>()) {
        metas->colorConfiguration = v.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`colorConfiguration` must be `asset` type, but got " +
            fv.second.type_name());
      }
    } else if (fv.first == "colorManagementSystem") {
      if (auto v = fv.second.get_value<value::token>()) {
        metas->colorManagementSystem = v.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`colorManagementSystem` must be `token` type, but got " +
            fv.second.type_name());
      }
    } else if (fv.first == "renderSettingsPrimPath") {
      if (auto v = fv.second.get_value<std::string>()) {
        metas->renderSettingsPrimPath = v.value();
      } else {
        PUSH_ERROR_AND_RETURN(
            "`renderSettingsPrimPath` must be `string` type, but got " +
            fv.second.type_name());
      }
    } else if (fv.second.IsUnregisteredValue()) {
      // Unregistered layer metadata (pxr SdfUnregisteredValue): the crate
      // stores the raw USDA text of the value (quotes included for string
      // values). Keep it verbatim so the printer can write it back unchanged.
      if (const std::string *uv = fv.second.GetUnregisteredValueString()) {
        metas->unregisteredMetas[fv.first] = *uv;
      }
      DCOUT("Preserved unregistered layer metadata: " << fv.first);
    } else {
      PUSH_WARN("[StageMeta] TODO: " + fv.first);
    }
  }

  if (subLayers.size()) {
    std::vector<SubLayer> dst;
    for (size_t i = 0; i < subLayers.size(); i++) {
      SubLayer s;
      s.assetPath = subLayers[i];
      dst.push_back(s);
    }

    if (subLayers.size() == subLayerOffsets.size()) {
      for (size_t i = 0; i < subLayerOffsets.size(); i++) {
        dst[i].layerOffset = subLayerOffsets[i];
      }
    }

    metas->subLayers = dst;

  } else if (subLayerOffsets.size()) {
    PUSH_WARN("Corrupted subLayer info? `subLayers` Fileld not found.");
  }

  return true;
}

std::unique_ptr<Prim> USDCReader::Impl::ReconstructPrimFromTypeName(
    const std::string &typeName, // LightUSD's Prim type name
    const std::string &primTypeName, // USD's Prim typeName
    const std::string &prim_name,
    const crate::CrateReader::Node &node, const Specifier spec,
    const std::vector<value::token> &primChildren,
    const std::vector<value::token> &properties,
    const PathIndexToSpecIndexMap &psmap, const PrimMeta &meta, bool *is_unsupported_prim) {

  if (is_unsupported_prim) {
    (*is_unsupported_prim) = false; // init with false
  }


// Per-type body lives in ReconstructTypedPrim<T> (usdc-reader-prim-detail.inc), instantiated
// in the usdc-reader-prim-reconstruct-*.cc siblings, so the per-type value::Value<T>
// construction is not all codegen'd in this dispatch TU.
#define RECONSTRUCT_PRIM(__primty, __node_ty, __prim_name, __spec) \
  if (__node_ty == value::TypeTraits<__primty>::type_name()) {     \
    return ReconstructTypedPrim<__primty>(node, __spec, psmap, __prim_name, \
                                          primTypeName, meta, properties, primChildren); \
  } else

  if (typeName == "Model" || typeName == "__AnyType__") {
    Model typed_prim;
    if (!ReconstructPrim(spec, node, psmap, &typed_prim)) {
      PUSH_ERROR("Failed to reconstruct Model");
      return nullptr;
    }
    typed_prim.meta = meta;
    typed_prim.name = prim_name;
    if (typeName == "__AnyType__") {
      typed_prim.prim_type_name = "";
    } else {
      typed_prim.prim_type_name = primTypeName;
    }
    typed_prim.spec = spec;
    typed_prim.propertyNames() = properties;
    typed_prim.primChildrenNames() = primChildren;
    value::Value primdata(std::move(typed_prim));
    auto result = std::unique_ptr<Prim>(new Prim(prim_name, std::move(primdata)));
    result->prim_type_name() = primTypeName;
    result->specifier() = spec;
    result->metas().primChildren = primChildren;
    return result;
  } else

  RECONSTRUCT_PRIM(Xform, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Model, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Scope, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Volume, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(VolumeFieldBase, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(FieldBase, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(VolumeFieldAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(OpenVDBAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Field3DAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(RenderSettings, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(RenderProduct, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(RenderVar, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GenerativeProcedural, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomMesh, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Volume, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(VolumeFieldBase, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(FieldBase, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(VolumeFieldAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(FieldAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(OpenVDBAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Field3DAsset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomPoints, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCylinder, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCube, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCone, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomSphere, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCapsule, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomBasisCurves, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomNurbsCurves, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomPlane, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCylinder_1, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCapsule_1, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomTetMesh, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomNurbsPatch, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomHermiteCurves, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomPointInstancer, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomCamera, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeomSubset, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(SphereLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(DomeLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(CylinderLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(DiskLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(DistantLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(RectLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(GeometryLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PortalLight, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(DomeLight_1, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(LightFilter, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PluginLightFilter, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(SkelRoot, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Skeleton, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(SkelAnimation, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(BlendShape, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Shader, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(NodeGraph, typeName, prim_name, spec)
  // UsdPhysics + mjcPhysics
  RECONSTRUCT_PRIM(PhysicsJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsScene, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsRevoluteJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsPrismaticJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsSphericalJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsFixedJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsDistanceJoint, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(PhysicsCollisionGroup, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(MjcActuator, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(NewtonActuator, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(MjcTendon, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(MjcKeyframe, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(MjcSensor, typeName, prim_name, spec)
  // AR/Interactive (Apple Preliminary_*)
  RECONSTRUCT_PRIM(Preliminary_PhysicsGravitationalForce, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_InfiniteColliderPlane, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_ReferenceImage, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_Behavior, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_Trigger, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_Action, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Preliminary_Text, typeName, prim_name, spec)
  // usdMedia
  RECONSTRUCT_PRIM(SpatialAudio, typeName, prim_name, spec)
  RECONSTRUCT_PRIM(Material, typeName, prim_name, spec) {
    PUSH_WARN("TODO or unsupported prim type: " << typeName);
    if (is_unsupported_prim) {
      (*is_unsupported_prim) = true;
    }
    return nullptr;
  }

#undef RECONSTRUCT_PRIM
}



}  // namespace usdc
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDC_READER
