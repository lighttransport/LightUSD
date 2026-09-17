// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Enum / Path string converters (extracted from pprinter.cc).
//
#include "pprint-enum.hh"

#include "str-util.hh"
#include "value-pprint.hh"
#include "core/collection-api.hh"
//
#include "common-macros.inc"

namespace lightusd {

// Forward declaration — defined in pprint-meta.cc (or pprinter.cc).
std::string print_customData(const CustomDataType &customData,
                             const std::string &name, const uint32_t indent);

//
// APISchemas::APIName
//
std::string to_string(const APISchemas::APIName &name) {
  std::string s;

  switch (name) {
    case APISchemas::APIName::VisibilityAPI: {
      s = "VisibilityAPI";
      break;
    }
    case APISchemas::APIName::XformCommonAPI: {
      s = "XformCommonAPI";
      break;
    }
    case APISchemas::APIName::SkelBindingAPI: {
      s = "SkelBindingAPI";
      break;
    }
    case APISchemas::APIName::MotionAPI: {
      s = "MotionAPI";
      break;
    }
    case APISchemas::APIName::PrimvarsAPI: {
      s = "PrimvarsAPI";
      break;
    }
    case APISchemas::APIName::CollectionAPI: {
      s = "CollectionAPI";
      break;
    }
    case APISchemas::APIName::ColorSpaceAPI: {
      s = "ColorSpaceAPI";
      break;
    }
    case APISchemas::APIName::ColorSpaceDefinitionAPI: {
      s = "ColorSpaceDefinitionAPI";
      break;
    }
    case APISchemas::APIName::ConnectableAPI: {
      s = "ConnectableAPI";
      break;
    }
    case APISchemas::APIName::CoordSysAPI: {
      s = "CoordSysAPI";
      break;
    }
    case APISchemas::APIName::NodeDefAPI: {
      s = "NodeDefAPI";
      break;
    }
    case APISchemas::APIName::MaterialXConfigAPI: {
      s = "MaterialXConfigAPI";
      break;
    }
    case APISchemas::APIName::MaterialUEConfigAPI: {
      s = "MaterialUEConfigAPI";
      break;
    }
    case APISchemas::APIName::MaterialBindingAPI: {
      s = "MaterialBindingAPI";
      break;
    }
    case APISchemas::APIName::ShapingAPI: {
      s = "ShapingAPI";
      break;
    }
    case APISchemas::APIName::ShadowAPI: {
      s = "ShadowAPI";
      break;
    }
    case APISchemas::APIName::GeomModelAPI: {
      s = "GeomModelAPI";
      break;
    }
    case APISchemas::APIName::ListAPI: {
      s = "ListAPI";
      break;
    }
    case APISchemas::APIName::LightAPI: {
      s = "LightAPI";
      break;
    }
    case APISchemas::APIName::LightListAPI: {
      s = "LightListAPI";
      break;
    }
    case APISchemas::APIName::VolumeLightAPI: {
      s = "VolumeLightAPI";
      break;
    }
    case APISchemas::APIName::MeshLightAPI: {
      s = "MeshLightAPI";
      break;
    }
    case APISchemas::APIName::Preliminary_AnchoringAPI: {
      s = "Preliminary_AnchoringAPI";
      break;
    }
    case APISchemas::APIName::Preliminary_PhysicsColliderAPI: {
      s = "Preliminary_PhysicsColliderAPI";
      break;
    }
    case APISchemas::APIName::Preliminary_PhysicsRigidBodyAPI: {
      s = "Preliminary_PhysicsRigidBodyAPI";
      break;
    }
    case APISchemas::APIName::Preliminary_PhysicsMaterialAPI: {
      s = "Preliminary_PhysicsMaterialAPI";
      break;
    }
    // UsdPhysics
    case APISchemas::APIName::PhysicsRigidBodyAPI: {
      s = "PhysicsRigidBodyAPI";
      break;
    }
    case APISchemas::APIName::PhysicsCollisionAPI: {
      s = "PhysicsCollisionAPI";
      break;
    }
    case APISchemas::APIName::PhysicsMaterialAPI: {
      s = "PhysicsMaterialAPI";
      break;
    }
    case APISchemas::APIName::PhysicsMeshCollisionAPI: {
      s = "PhysicsMeshCollisionAPI";
      break;
    }
    case APISchemas::APIName::PhysicsMassAPI: {
      s = "PhysicsMassAPI";
      break;
    }
    case APISchemas::APIName::PhysicsFilteredPairsAPI: {
      s = "PhysicsFilteredPairsAPI";
      break;
    }
    case APISchemas::APIName::PhysicsArticulationRootAPI: {
      s = "PhysicsArticulationRootAPI";
      break;
    }
    // PhysX (Omniverse)
    case APISchemas::APIName::PhysxJointAPI: {
      s = "PhysxJointAPI";
      break;
    }
    case APISchemas::APIName::AssetPreviewsAPI: {
      s = "AssetPreviewsAPI";
      break;
    }
    // MuJoCo (mjcPhysics)
    case APISchemas::APIName::MjcSceneAPI: {
      s = "MjcSceneAPI";
      break;
    }
    case APISchemas::APIName::MjcJointAPI: {
      s = "MjcJointAPI";
      break;
    }
    case APISchemas::APIName::MjcCollisionAPI: {
      s = "MjcCollisionAPI";
      break;
    }
    case APISchemas::APIName::MjcMeshCollisionAPI: {
      s = "MjcMeshCollisionAPI";
      break;
    }
    case APISchemas::APIName::MjcMaterialAPI: {
      s = "MjcMaterialAPI";
      break;
    }
    case APISchemas::APIName::MjcSiteAPI: {
      s = "MjcSiteAPI";
      break;
    }
    case APISchemas::APIName::MjcImageableAPI: {
      s = "MjcImageableAPI";
      break;
    }
    case APISchemas::APIName::MjcEqualityAPI: {
      s = "MjcEqualityAPI";
      break;
    }
    case APISchemas::APIName::MjcEqualityConnectAPI: {
      s = "MjcEqualityConnectAPI";
      break;
    }
    case APISchemas::APIName::MjcEqualityWeldAPI: {
      s = "MjcEqualityWeldAPI";
      break;
    }
    case APISchemas::APIName::MjcEqualityJointAPI: {
      s = "MjcEqualityJointAPI";
      break;
    }
    // Newton physics
    case APISchemas::APIName::NewtonSceneAPI: {
      s = "NewtonSceneAPI";
      break;
    }
    case APISchemas::APIName::NewtonXpbdSceneAPI: {
      s = "NewtonXpbdSceneAPI";
      break;
    }
    case APISchemas::APIName::NewtonKaminoSceneAPI: {
      s = "NewtonKaminoSceneAPI";
      break;
    }
    case APISchemas::APIName::NewtonArticulationRootAPI: {
      s = "NewtonArticulationRootAPI";
      break;
    }
    case APISchemas::APIName::NewtonCollisionAPI: {
      s = "NewtonCollisionAPI";
      break;
    }
    case APISchemas::APIName::NewtonMeshCollisionAPI: {
      s = "NewtonMeshCollisionAPI";
      break;
    }
    case APISchemas::APIName::NewtonMaterialAPI: {
      s = "NewtonMaterialAPI";
      break;
    }
    case APISchemas::APIName::NewtonMimicAPI: {
      s = "NewtonMimicAPI";
      break;
    }
    case APISchemas::APIName::NewtonActuatorDelayAPI: {
      s = "NewtonActuatorDelayAPI";
      break;
    }
    case APISchemas::APIName::NewtonActuatorControlBaseAPI: {
      s = "NewtonActuatorControlBaseAPI";
      break;
    }
    case APISchemas::APIName::NewtonPDControlAPI: {
      s = "NewtonPDControlAPI";
      break;
    }
    case APISchemas::APIName::NewtonPIDControlAPI: {
      s = "NewtonPIDControlAPI";
      break;
    }
    case APISchemas::APIName::NewtonNeuralControlAPI: {
      s = "NewtonNeuralControlAPI";
      break;
    }
    case APISchemas::APIName::NewtonActuatorClampingBaseAPI: {
      s = "NewtonActuatorClampingBaseAPI";
      break;
    }
    case APISchemas::APIName::NewtonMaxEffortClampingAPI: {
      s = "NewtonMaxEffortClampingAPI";
      break;
    }
    case APISchemas::APIName::NewtonDCMotorClampingAPI: {
      s = "NewtonDCMotorClampingAPI";
      break;
    }
    case APISchemas::APIName::NewtonPositionBasedClampingAPI: {
      s = "NewtonPositionBasedClampingAPI";
      break;
    }
    case APISchemas::APIName::PhysicsDriveAPI: {
      s = "PhysicsDriveAPI";
      break;
    }
    case APISchemas::APIName::PhysicsLimitAPI: {
      s = "PhysicsLimitAPI";
      break;
    }
  }

  return s;
}

//
// CustomDataType
//
std::string to_string(const CustomDataType &custom) {
  return print_customData(custom, "", 0);
}

//
// Reference
//
std::string to_string(const Reference &v) {
  std::stringstream ss;

  // For internal references (no asset path, just prim path), don't output "@@"
  if (!v.asset_path.GetAssetPath().empty()) {
    ss << v.asset_path;
  }
  if (v.prim_path.is_valid()) {
    ss << v.prim_path;
  }

  ss << v.layerOffset;

  if (!v.customData.empty()) {
    // TODO: Indent
    ss << print_customData(v.customData, "customData", /* indent */ 0);
  }

  return ss.str();
}

//
// Payload
//
std::string to_string(const Payload &v) {
  std::stringstream ss;

  if (v.is_none()) {
    // pxrUSD serialize and prints 'None' for payload by filling all members in
    // Payload empty.
    ss << "None";

  } else {
    // For internal payloads (no asset path, just prim path), don't output "@@"
    if (!v.asset_path.GetAssetPath().empty()) {
      ss << v.asset_path;
    }
    if (v.prim_path.is_valid()) {
      ss << v.prim_path;
    }

    ss << v.layerOffset;
  }

  return ss.str();
}

//
// CollectionInstance::ExpansionRule
//
std::string to_string(lightusd::CollectionInstance::ExpansionRule rule) {
  std::string s;

  switch (rule) {
    case lightusd::CollectionInstance::ExpansionRule::ExplicitOnly: {
      s = kExplicitOnly;
      break;
    }
    case lightusd::CollectionInstance::ExpansionRule::ExpandPrims: {
      s = kExpandPrims;
      break;
    }
    case lightusd::CollectionInstance::ExpansionRule::ExpandPrimsAndProperties: {
      s = kExpandPrimsAndProperties;
      break;
    }
  }

  return s;
}

//
// Kind
//
std::string to_string(lightusd::Kind v) {
  if (v == lightusd::Kind::Model) {
    return "model";
  } else if (v == lightusd::Kind::Group) {
    return "group";
  } else if (v == lightusd::Kind::Assembly) {
    return "assembly";
  } else if (v == lightusd::Kind::Component) {
    return "component";
  } else if (v == lightusd::Kind::Subcomponent) {
    return "subcomponent";
  } else if (v == lightusd::Kind::SceneLibrary) {
    return "sceneLibrary";
  } else if (v == lightusd::Kind::UserDef) {
    // Should use PrimMeta::get_kind() to get actual Kind string value.
    return "[[InternalError. UserDefKind]]";
  } else {
    return "[[InvalidKind]]";
  }
}

//
// Axis
//
std::string to_string(lightusd::Axis v) {
  if (v == lightusd::Axis::X) {
    return "X";
  } else if (v == lightusd::Axis::Y) {
    return "Y";
  } else if (v == lightusd::Axis::Z) {
    return "Z";
  } else {
    return "[[InvalidAxis]]";
  }
}

//
// Visibility
//
std::string to_string(lightusd::Visibility v) {
  if (v == lightusd::Visibility::Inherited) {
    return "inherited";
  } else {
    return "invisible";
  }
}

//
// Orientation
//
std::string to_string(lightusd::Orientation o) {
  if (o == lightusd::Orientation::RightHanded) {
    return "rightHanded";
  } else {
    return "leftHanded";
  }
}

//
// ListEditQual
//
std::string to_string(lightusd::ListEditQual v) {
  if (v == lightusd::ListEditQual::ResetToExplicit) {
    return "";  // unqualified
  } else if (v == lightusd::ListEditQual::Append) {
    return "append";
  } else if (v == lightusd::ListEditQual::Add) {
    return "add";
  } else if (v == lightusd::ListEditQual::Delete) {
    return "delete";
  } else if (v == lightusd::ListEditQual::Prepend) {
    return "prepend";
  } else if (v == lightusd::ListEditQual::Order) {
    return "order";
  }

  return "[[Invalid ListEditQual value]]";
}

//
// Interpolation
//
std::string to_string(lightusd::Interpolation interp) {
  switch (interp) {
    case Interpolation::Invalid:
      return "[[Invalid interpolation value]]";
    case Interpolation::Constant:
      return "constant";
    case Interpolation::Uniform:
      return "uniform";
    case Interpolation::Varying:
      return "varying";
    case Interpolation::Vertex:
      return "vertex";
    case Interpolation::FaceVarying:
      return "faceVarying";
  }

  // Never reach here though
  return "[[Invalid interpolation value]]";
}

//
// SpecType
//
std::string to_string(lightusd::SpecType ty) {
  if (SpecType::Attribute == ty) {
    return "SpecTypeAttribute";
  } else if (SpecType::Connection == ty) {
    return "SpecTypeConnection";
  } else if (SpecType::Expression == ty) {
    return "SpecTypeExpression";
  } else if (SpecType::Mapper == ty) {
    return "SpecTypeMapper";
  } else if (SpecType::MapperArg == ty) {
    return "SpecTypeMapperArg";
  } else if (SpecType::Prim == ty) {
    return "SpecTypePrim";
  } else if (SpecType::PseudoRoot == ty) {
    return "SpecTypePseudoRoot";
  } else if (SpecType::Relationship == ty) {
    return "SpecTypeRelationship";
  } else if (SpecType::RelationshipTarget == ty) {
    return "SpecTypeRelationshipTarget";
  } else if (SpecType::Variant == ty) {
    return "SpecTypeVariant";
  } else if (SpecType::VariantSet == ty) {
    return "SpecTypeVariantSet";
  }
  return "SpecTypeInvalid";
}

//
// Specifier
//
std::string to_string(lightusd::Specifier s) {
  if (s == lightusd::Specifier::Def) {
    return "def";
  } else if (s == lightusd::Specifier::Over) {
    return "over";
  } else if (s == lightusd::Specifier::Class) {
    return "class";
  } else {
    return "[[SpecifierInvalid]]";
  }
}

//
// Permission
//
std::string to_string(lightusd::Permission s) {
  if (s == lightusd::Permission::Public) {
    return "public";
  } else if (s == lightusd::Permission::Private) {
    return "private";
  } else {
    return "[[PermissionInvalid]]";
  }
}

//
// Purpose
//
std::string to_string(lightusd::Purpose purpose) {
  switch (purpose) {
    case Purpose::Default: {
      return "default";
    }
    case Purpose::Render: {
      return "render";
    }
    case Purpose::Guide: {
      return "guide";
    }
    case Purpose::Proxy: {
      return "proxy";
    }
  }

  // Never reach here though
  return "[[Invalid Purpose value]]";
}

//
// Variability
//
std::string to_string(lightusd::Variability v) {
  if (v == lightusd::Variability::Varying) {
    return "varying";
  } else if (v == lightusd::Variability::Uniform) {
    return "uniform";
  } else if (v == lightusd::Variability::Config) {
    return "config";
  } else {
    return "\"[[VariabilityInvalid]]\"";
  }
}

//
// Extent
//
std::string to_string(lightusd::Extent e) {
  std::stringstream ss;

  ss << "[" << e.lower << ", " << e.upper << "]";

  return ss.str();
}

//
// Path
//
std::string to_string(const Path &path, bool show_full_path) {
  if (show_full_path) {
    return path.full_path_name();
  } else {
    return path.full_path_name();
  }
}

std::string to_string(const std::vector<Path> &v, bool show_full_path) {
  // TODO(syoyo): indent
  std::stringstream ss;
  ss << "[";

  for (size_t i = 0; i < v.size(); i++) {
    ss << to_string(v[i], show_full_path);
    if (i != (v.size() - 1)) {
      ss << ", ";
    }
  }
  ss << "]";
  return ss.str();
}

//
// XformOp::OpType
//
std::string to_string(const XformOp::OpType &op) {
  std::string ss;

  switch (op) {
    case XformOp::OpType::ResetXformStack: {
      ss = "!resetXformStack!";
      break;
    }
    case XformOp::OpType::Transform: {
      ss = "xformOp:transform";
      break;
    }
    case XformOp::OpType::Translate: {
      ss = "xformOp:translate";
      break;
    }
    case XformOp::OpType::Scale: {
      ss = "xformOp:scale";
      break;
    }
    case XformOp::OpType::RotateX: {
      ss = "xformOp:rotateX";
      break;
    }
    case XformOp::OpType::RotateY: {
      ss = "xformOp:rotateY";
      break;
    }
    case XformOp::OpType::RotateZ: {
      ss = "xformOp:rotateZ";
      break;
    }
    case XformOp::OpType::RotateXYZ: {
      ss = "xformOp:rotateXYZ";
      break;
    }
    case XformOp::OpType::RotateXZY: {
      ss = "xformOp:rotateXZY";
      break;
    }
    case XformOp::OpType::RotateYXZ: {
      ss = "xformOp:rotateYXZ";
      break;
    }
    case XformOp::OpType::RotateYZX: {
      ss = "xformOp:rotateYZX";
      break;
    }
    case XformOp::OpType::RotateZXY: {
      ss = "xformOp:rotateZXY";
      break;
    }
    case XformOp::OpType::RotateZYX: {
      ss = "xformOp:rotateZYX";
      break;
    }
    case XformOp::OpType::Orient: {
      ss = "xformOp:orient";
      break;
    }
  }

  return ss;
}

//
// dump_path (debug helper)
//
std::string dump_path(const Path &path) {
  std::stringstream ss;
  ss << "Path: Prim part = " << path.prim_part();
  ss << ", Prop part = " << path.prop_part();
  ss << ", Variant part = " << path.variant_part();
  ss << ", elementName = " << path.element_name();
  ss << ", isValid = " << path.is_valid();
  ss << ", isAbsolute = " << path.is_absolute_path();
  ss << ", isRelative = " << path.is_relative_path();

  return ss.str();
}

}  // namespace lightusd
