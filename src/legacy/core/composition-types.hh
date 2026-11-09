// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// composition-types.hh - USD composition types (AssetInfo, Reference, Payload, etc.)
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "path.hh"
#include "meta-variable.hh"
#include "prim-enums.hh"
#include "value-types.hh"

namespace lightusd {

struct AssetInfo {
  // builtin fields
  value::AssetPath identifier;
  std::string name;
  std::vector<value::AssetPath> payloadAssetDependencies;
  std::string version;

  // Other fields
  Dictionary _fields;
};

struct APISchemas {
  // LightUSD does not allow user-supplied API schema for now
  enum class APIName {
    // usdShade
    MaterialBindingAPI,  // "MaterialBindingAPI"
    ConnectableAPI, // "ConnectableAPI"
    CoordSysAPI, // "CoordSysAPI"
    NodeDefAPI, // "NodeDefAPI"
    MaterialXConfigAPI, // "MaterialXConfigAPI" (usdMtlx: config:mtlx:*)
    MaterialUEConfigAPI, // "MaterialUEConfigAPI" (LightUSD UE material bridge)

    CollectionAPI,      // "CollectionAPI"
    ColorSpaceAPI,      // "ColorSpaceAPI"
    ColorSpaceDefinitionAPI,  // "ColorSpaceDefinitionAPI" (multi-apply)
    // usdGeom
    GeomModelAPI, // "GeomModelAPI"
    MotionAPI, // "MotionAPI"
    PrimvarsAPI, // "PrimvarsAPI"
    VisibilityAPI, // "VisibilityAPI"
    XformCommonAPI, // "XformCommonAPI"

    // usdLux
    LightAPI, // "LightAPI"
    LightListAPI, // "LightListAPI"
    ListAPI, // "ListAPI"
    MeshLightAPI, // "MeshLightAPI"
    ShapingAPI, // "ShapingAPI"
    ShadowAPI,  // "ShadowAPI"
    VolumeLightAPI,  // "VolumeLightAPI"

    // usdSkel
    SkelBindingAPI,      // "SkelBindingAPI"

    // USDZ AR extensions
    Preliminary_AnchoringAPI,
    Preliminary_PhysicsColliderAPI,
    Preliminary_PhysicsMaterialAPI,
    Preliminary_PhysicsRigidBodyAPI,

    // UsdPhysics API schemas
    PhysicsRigidBodyAPI,
    PhysicsCollisionAPI,
    PhysicsMaterialAPI,
    PhysicsMeshCollisionAPI,

    // MuJoCo (mjcPhysics) API schemas
    MjcSceneAPI,
    MjcJointAPI,
    MjcCollisionAPI,
    MjcMeshCollisionAPI,
    MjcMaterialAPI,
    MjcSiteAPI,
    MjcImageableAPI,
    MjcEqualityAPI,
    MjcEqualityConnectAPI,
    MjcEqualityWeldAPI,
    MjcEqualityJointAPI,

    // Newton physics API schemas
    NewtonSceneAPI,
    NewtonXpbdSceneAPI,
    NewtonKaminoSceneAPI,
    NewtonArticulationRootAPI,
    NewtonCollisionAPI,
    NewtonMeshCollisionAPI,
    NewtonMaterialAPI,
    NewtonMimicAPI,
    NewtonActuatorDelayAPI,
    NewtonActuatorControlBaseAPI,
    NewtonPDControlAPI,
    NewtonPIDControlAPI,
    NewtonNeuralControlAPI,
    NewtonActuatorClampingBaseAPI,
    NewtonMaxEffortClampingAPI,
    NewtonDCMotorClampingAPI,
    NewtonPositionBasedClampingAPI,

    // Additional UsdPhysics API schemas
    PhysicsMassAPI,
    PhysicsFilteredPairsAPI,
    PhysicsArticulationRootAPI,

    // PhysX (Omniverse) API schemas
    PhysxJointAPI,

    // UsdMedia API schemas
    AssetPreviewsAPI,

    // Multi-apply UsdPhysics API schemas
    PhysicsDriveAPI,
    PhysicsLimitAPI,
  };

  ListEditQual listOpQual{ListEditQual::ResetToExplicit};  // first non-delete qualifier seen on the prim

  // std::get<1>: instance name. For Multi-apply API Schema e.g.
  // `material:MainMaterial` for `CollectionAPI:material:MainMaterial`
  std::vector<std::pair<APIName, std::string>> names;

  // Unknown/unsupported API schemas - stored as raw strings to preserve them
  // Each entry is (schema_name, instance_name) where instance_name may be empty
  std::vector<std::pair<std::string, std::string>> unknownSchemas;

  // Schemas marked for deletion via `delete apiSchemas = [...]` list-op.
  // The single-layer reader already subtracts these from `names`/
  // `unknownSchemas`; they are tracked separately so future multi-layer
  // composition can still see what was deleted by a strong layer.
  std::vector<std::pair<APIName, std::string>> deletedNames;
  std::vector<std::pair<std::string, std::string>> deletedUnknownSchemas;

  // --- Authored list-op, preserved verbatim for lossless round-trip ---
  //
  // In USD `apiSchemas` is an unresolved `SdfTokenListOp`: a layer stores the
  // authored ops (prepend/append/add/delete/explicit) as-is and composition
  // resolves them later. The fields above (`names`, `unknownSchemas`,
  // `deletedNames`, `listOpQual`) are the *resolved* single-layer view consumed
  // by schema application; they cannot reconstruct the original ops (a `delete`
  // is already subtracted from `names`). These fields keep what was authored so
  // the USDA/USDC writers can reproduce it (matches pxrUSD usdcat round-trip).
  //
  // Each entry is one authored op: `(<qualifier>, [(schemaName, instanceName), ...])`,
  // in authoring order. Schema names are kept as raw strings so unknown schemas
  // round-trip identically to known ones.
  std::vector<std::pair<ListEditQual,
                        std::vector<std::pair<std::string, std::string>>>>
      authoredOps;

  // Authored as `apiSchemas = None` (explicit empty list / ValueBlock).
  bool explicitlyEmpty{false};
};

// SdfLayerOffset
struct LayerOffset {
  double _offset{0.0};
  double _scale{1.0};
};

// SdfReference
struct Reference {
  value::AssetPath asset_path;
  Path prim_path;
  LayerOffset layerOffset;
  Dictionary customData;
};

// SdfPayload
struct Payload {
  value::AssetPath asset_path;  // std::string in SdfPayload
  Path prim_path;
  LayerOffset layerOffset;  // from 0.8.0
  // No customData for Payload

  // NOTE: pxrUSD encodes `payload = None` as Payload with empty paths in USDC(Crate).
  // (Not ValueBlock)
  bool is_none() const {
    return asset_path.GetAssetPath().empty() && !prim_path.is_valid();
  }
};

// AOUSD Core Spec 10.3.2.5: Deferred variant evaluation.
// Variant selection should use opinions from ALL composition arcs, not just
// the arc at V position. This struct collects selection opinions across arcs.
struct DeferredVariantInfo {
  // key = variant set name
  // value = ordered list of selection opinions (strongest first)
  // Each opinion is a VariantSelectionMap from a given arc.
  std::map<std::string, std::vector<VariantSelectionMap>> selection_opinions;
};

// AOUSD Core Spec 10.3.2.3: Arc origin tracking for implied inherits/specializes.
// When a referenced layer contains inherits or specializes, those arcs should
// be "implied" in all upstream layer stacks.
struct ArcOrigin {
  std::string source_layer_id;  // Layer identifier where the arc was authored
  Path source_prim_path;        // Prim path in that layer
};

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Reference, "ref", TYPE_ID_REFERENCE, 1);
DEFINE_TYPE_TRAIT(Payload, "payload", TYPE_ID_PAYLOAD, 1);
DEFINE_TYPE_TRAIT(LayerOffset, "LayerOffset", TYPE_ID_LAYER_OFFSET, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
