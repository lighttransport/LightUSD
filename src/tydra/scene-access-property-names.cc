// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "core/prim.hh"
#include "pprint-enum.hh"
#include "scene-access.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "usdShade.hh"
#include "usdSkel.hh"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace tydra {

constexpr auto kInfoId = "info:id";

bool GetPropertyNamesForPrim(const lightusd::Prim &prim,
                             std::vector<std::string> *out_prop_names,
                             bool attr_prop, bool rel_prop, std::string *err);

namespace {

void AppendXformablePropertyNames(const Xformable &xformable,
                                  std::vector<std::string> *prop_names) {
  if (!prop_names) {
    return;
  }

  for (const auto &xop : xformable.xformOps) {
    if (xop.op_type == XformOp::OpType::ResetXformStack) {
      continue;
    }

    std::string varname = to_string(xop.op_type);
    if (!xop.suffix.empty()) {
      varname += ":" + xop.suffix;
    }

    prop_names->push_back(varname);
  }

  if (!xformable.xformOps.empty()) {
    prop_names->push_back("xformOpOrder");
  }
}

template <typename T>
bool GetPrimPropertyNamesImpl(const T &prim,
                              std::vector<std::string> *prop_names,
                              bool attr_prop = true, bool rel_prop = true);

template <typename T>
void AppendPropertyNameIfAuthored(const T &prop, const std::string &name,
                                  std::vector<std::string> *prop_names) {
  if (!prop_names) {
    return;
  }

  if (prop.authored()) {
    prop_names->push_back(name);
  }
}

template <typename T>
void AppendPropertyNamesFromCustomProps(const std::map<std::string, T> &props,
                                        std::vector<std::string> *prop_names,
                                        bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return;
  }

  for (const auto &prop : props) {
    if (prop.second.is_relationship()) {
      if (rel_prop) {
        prop_names->push_back(prop.first);
      }
    } else if (attr_prop) {
      prop_names->push_back(prop.first);
    }
  }
}

void AppendRelationshipPropertyNameIfAuthored(
    const nonstd::optional<Relationship> &rel, const std::string &name,
    std::vector<std::string> *prop_names) {
  if (!prop_names) {
    return;
  }

  if (rel) {
    prop_names->push_back(name);
  }
}

void AppendRelationshipPropertyNameIfAuthored(
    const RelationshipProperty &rel, const std::string &name,
    std::vector<std::string> *prop_names) {
  if (!prop_names) {
    return;
  }

  if (rel.authored()) {
    prop_names->push_back(name);
  }
}

template <typename T>
bool GetPrimvarReaderPropertyNamesImpl(const UsdPrimvarReader<T> &preader,
                                       std::vector<std::string> *prop_names,
                                       bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(preader.fallback, "inputs:fallback",
                                 prop_names);
    AppendPropertyNameIfAuthored(preader.varname, "inputs:varname", prop_names);
    AppendPropertyNameIfAuthored(preader.result, "outputs:result", prop_names);
  }

  AppendPropertyNamesFromCustomProps(preader.props, prop_names, attr_prop,
                                     rel_prop);

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Model &model,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  // TODO: Use propertyNames()
  for (const auto &prop : model.props) {
    if (prop.second.is_relationship()) {
      if (rel_prop) {
        prop_names->push_back(prop.first);
      }
    } else {  // assume attribute
      if (attr_prop) {
        prop_names->push_back(prop.first);
      }
    }
  }

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Scope &scope,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  // TODO: Use propertyNames()
  for (const auto &prop : scope.props) {
    if (prop.second.is_relationship()) {
      if (rel_prop) {
        prop_names->push_back(prop.first);
      }
    } else {  // assume attribute
      if (attr_prop) {
        prop_names->push_back(prop.first);
      }
    }
  }

  return true;
}

bool GetGPrimPropertyNamesImpl(const GPrim *gprim,
                               std::vector<std::string> *prop_names,
                               bool attr_prop, bool rel_prop) {
  if (!gprim) {
    return false;
  }

  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    if (gprim->doubleSided.authored()) {
      prop_names->push_back("doubleSided");
    }

    if (gprim->orientation.authored()) {
      prop_names->push_back("orientation");
    }

    if (gprim->purpose.authored()) {
      prop_names->push_back("purpose");
    }

    if (gprim->extent.authored()) {
      prop_names->push_back("extent");
    }

    if (gprim->visibility.authored()) {
      prop_names->push_back("visibility");
    }

    // xformOps.
    for (const auto &xop : gprim->xformOps) {
      if (xop.op_type == XformOp::OpType::ResetXformStack) {
        // skip
        continue;
      }
      std::string varname = to_string(xop.op_type);
      if (!xop.suffix.empty()) {
        varname += ":" + xop.suffix;
      }
      prop_names->push_back(varname);
    }
  }

  if (rel_prop) {
    if (gprim->materialBinding.authored()) {
      prop_names->push_back(kMaterialBinding);
    }

    if (gprim->materialBindingPreview.authored()) {
      prop_names->push_back(kMaterialBindingPreview);
    }

    if (gprim->materialBindingFull.authored()) {
      prop_names->push_back(kMaterialBindingFull);
    }

    for (const auto &item : gprim->materialBindingMap()) {
      prop_names->push_back("material:binding:" + item.first);
    }

    for (const auto &collection : gprim->materialBindingCollectionMap()) {
      std::string purpose_name;
      if (!collection.first.empty()) {
        purpose_name = ":" + collection.first;
      }

      for (size_t i = 0; i < collection.second.size(); i++) {
        const std::string &coll_name = collection.second.keys()[i];
        std::string rel_name;
        if (collection.first.empty()) {
          rel_name = kMaterialBindingCollection + purpose_name;
        } else {
          rel_name = kMaterialBindingCollection + std::string(":") + coll_name +
                     purpose_name;
        }

        prop_names->push_back(rel_name);
      }
    }

    if (gprim->proxyPrim.authored()) {
      prop_names->push_back("proxyPrim");
    }
  }

  // other props
  for (const auto &prop : gprim->props) {
    if (prop.second.is_relationship()) {
      if (rel_prop) {
        prop_names->push_back(prop.first);
      }
    } else {  // assume attribute
      if (attr_prop) {
        prop_names->push_back(prop.first);
      }
    }
  }

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Xform &xform,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (!GetGPrimPropertyNamesImpl(&xform, prop_names, attr_prop, rel_prop)) {
    return false;
  }

  if (attr_prop && !xform.xformOps.empty()) {
    prop_names->push_back("xformOpOrder");
  }

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const GeomMesh &mesh,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (!GetGPrimPropertyNamesImpl(&mesh, prop_names, attr_prop, rel_prop)) {
    return false;
  }

  if (attr_prop) {
    if (mesh.points.authored()) {
      prop_names->push_back("points");
    }

    if (mesh.faceVertexCounts.authored()) {
      prop_names->push_back("faceVertexCounts");
    }

    if (mesh.faceVertexIndices.authored()) {
      prop_names->push_back("faceVertexIndices");
    }

    if (mesh.normals.authored()) {
      prop_names->push_back("normals");
    }

    if (mesh.velocities.authored()) {
      prop_names->push_back("velocities");
    }

    if (mesh.cornerIndices.authored()) {
      prop_names->push_back("cornerIndices");
    }

    if (mesh.cornerSharpnesses.authored()) {
      prop_names->push_back("cornerSharpnesses");
    }

    if (mesh.creaseIndices.authored()) {
      prop_names->push_back("creaseIndices");
    }

    if (mesh.creaseSharpnesses.authored()) {
      prop_names->push_back("creaseSharpnesses");
    }

    if (mesh.holeIndices.authored()) {
      prop_names->push_back("holeIndices");
    }

    if (mesh.interpolateBoundary.authored()) {
      prop_names->push_back("interpolateBoundary");
    }

    if (mesh.subdivisionScheme.authored()) {
      prop_names->push_back("subdivisionScheme");
    }

    if (mesh.faceVaryingLinearInterpolation.authored()) {
      prop_names->push_back("faceVaryingLinearInterpolation");
    }
  }

  if (rel_prop && mesh.skeleton) {
    prop_names->push_back("skeleton");
  }

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const GeomCamera &camera,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (!GetGPrimPropertyNamesImpl(&camera, prop_names, attr_prop, rel_prop)) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(camera.clippingPlanes, "clippingPlanes",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.clippingRange, "clippingRange",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.exposure, "exposure", prop_names);
    AppendPropertyNameIfAuthored(camera.focalLength, "focalLength",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.focusDistance, "focusDistance",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.horizontalAperture,
                                 "horizontalAperture", prop_names);
    AppendPropertyNameIfAuthored(camera.horizontalApertureOffset,
                                 "horizontalApertureOffset", prop_names);
    AppendPropertyNameIfAuthored(camera.verticalAperture, "verticalAperture",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.verticalApertureOffset,
                                 "verticalApertureOffset", prop_names);
    AppendPropertyNameIfAuthored(camera.fStop, "fStop", prop_names);
    AppendPropertyNameIfAuthored(camera.projection, "projection", prop_names);
    AppendPropertyNameIfAuthored(camera.stereoRole, "stereoRole", prop_names);
    AppendPropertyNameIfAuthored(camera.shutterClose, "shutterClose",
                                 prop_names);
    AppendPropertyNameIfAuthored(camera.shutterOpen, "shutterOpen",
                                 prop_names);
  }

  return true;
}

void AppendLightAPIPropertyNames(const LightAPI &light,
                                 std::vector<std::string> *prop_names,
                                 bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.color, "inputs:color", prop_names);
    AppendPropertyNameIfAuthored(light.colorTemperature,
                                 "inputs:colorTemperature", prop_names);
    AppendPropertyNameIfAuthored(light.diffuse, "inputs:diffuse",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.enableColorTemperature,
                                 "inputs:enableColorTemperature", prop_names);
    AppendPropertyNameIfAuthored(light.exposure, "inputs:exposure",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.intensity, "inputs:intensity",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.normalize, "inputs:normalize",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.specular, "inputs:specular",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.shadowEnable, "inputs:shadow:enable",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.shadowColor, "inputs:shadow:color",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.shadowDistance,
                                 "inputs:shadow:distance", prop_names);
    AppendPropertyNameIfAuthored(light.shadowFalloff,
                                 "inputs:shadow:falloff", prop_names);
    AppendPropertyNameIfAuthored(light.shadowFalloffGamma,
                                 "inputs:shadow:falloffGamma", prop_names);
    AppendPropertyNameIfAuthored(light.shapingFocus,
                                 "inputs:shaping:focus", prop_names);
    AppendPropertyNameIfAuthored(light.shapingFocusTint,
                                 "inputs:shaping:focusTint", prop_names);
    AppendPropertyNameIfAuthored(light.shapingConeAngle,
                                 "inputs:shaping:cone:angle", prop_names);
    AppendPropertyNameIfAuthored(light.shapingConeSoftness,
                                 "inputs:shaping:cone:softness", prop_names);
    AppendPropertyNameIfAuthored(light.shapingIesFile,
                                 "inputs:shaping:ies:file", prop_names);
    AppendPropertyNameIfAuthored(light.shapingIesAngleScale,
                                 "inputs:shaping:ies:angleScale", prop_names);
    AppendPropertyNameIfAuthored(light.shapingIesNormalize,
                                 "inputs:shaping:ies:normalize", prop_names);
    AppendPropertyNameIfAuthored(light.spectralEmission,
                                 "wavelength:emission", prop_names);
  }

  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(light.lightFilters,
                                             "light:filters", prop_names);
  }
}

template <typename T>
bool GetBoundableLightPropertyNamesImpl(const T &light,
                                        std::vector<std::string> *prop_names,
                                        bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.extent, "extent", prop_names);
    AppendPropertyNameIfAuthored(light.purpose, "purpose", prop_names);
    AppendPropertyNameIfAuthored(light.visibility, "visibility", prop_names);
    AppendXformablePropertyNames(light, prop_names);
  }

  AppendLightAPIPropertyNames(light, prop_names, attr_prop, rel_prop);
  AppendPropertyNamesFromCustomProps(light.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <typename T>
bool GetNonboundableLightPropertyNamesImpl(
    const T &light, std::vector<std::string> *prop_names, bool attr_prop,
    bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.purpose, "purpose", prop_names);
    AppendPropertyNameIfAuthored(light.visibility, "visibility", prop_names);
    AppendXformablePropertyNames(light, prop_names);
  }

  AppendLightAPIPropertyNames(light, prop_names, attr_prop, rel_prop);
  AppendPropertyNamesFromCustomProps(light.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const SphereLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetBoundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                          rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.radius, "inputs:radius", prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const CylinderLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetBoundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                          rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.length, "inputs:length", prop_names);
    AppendPropertyNameIfAuthored(light.radius, "inputs:radius", prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const RectLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetBoundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                          rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.file, "inputs:texture:file",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.height, "inputs:height", prop_names);
    AppendPropertyNameIfAuthored(light.width, "inputs:width", prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const DiskLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetBoundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                          rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.radius, "inputs:radius", prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const DistantLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetNonboundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                             rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.angle, "inputs:angle", prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const DomeLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetNonboundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                             rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.guideRadius, "guideRadius",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.file, "inputs:texture:file",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.textureFormat,
                                 "inputs:texture:format", prop_names);
  }
  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(light.portals, "portals",
                                             prop_names);
    AppendRelationshipPropertyNameIfAuthored(light.proxyPrim, "proxyPrim",
                                             prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const DomeLight_1 &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetNonboundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                             rel_prop)) {
    return false;
  }
  if (attr_prop) {
    AppendPropertyNameIfAuthored(light.guideRadius, "guideRadius",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.file, "inputs:texture:file",
                                 prop_names);
    AppendPropertyNameIfAuthored(light.textureFormat,
                                 "inputs:texture:format", prop_names);
    AppendPropertyNameIfAuthored(light.poleAxis, "poleAxis", prop_names);
  }
  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(light.portals, "portals",
                                             prop_names);
    AppendRelationshipPropertyNameIfAuthored(light.proxyPrim, "proxyPrim",
                                             prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const GeometryLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetNonboundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                             rel_prop)) {
    return false;
  }
  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(light.geometry, "geometry",
                                             prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const PortalLight &light,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!GetNonboundableLightPropertyNamesImpl(light, prop_names, attr_prop,
                                             rel_prop)) {
    return false;
  }
  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(light.geometry, "geometry",
                                             prop_names);
  }
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const GeomSubset &subset,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  (void)rel_prop;

  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    if (subset.elementType.authored()) {
      prop_names->push_back("elementType");
    }

    if (subset.familyName.authored()) {
      prop_names->push_back("familyName");
    }

    if (subset.indices.authored()) {
      prop_names->push_back("indices");
    }

    DCOUT("TODO: more attrs...");
  }

  if (rel_prop && subset.materialBinding.authored()) {
    prop_names->push_back("material:binding");
  }

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const SkelRoot &skelroot,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(skelroot.extent, "extent", prop_names);
    AppendPropertyNameIfAuthored(skelroot.purpose, "purpose", prop_names);
    AppendPropertyNameIfAuthored(skelroot.visibility, "visibility",
                                 prop_names);
    AppendXformablePropertyNames(skelroot, prop_names);
  }

  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(skelroot.proxyPrim, "proxyPrim",
                                             prop_names);
    AppendRelationshipPropertyNameIfAuthored(skelroot.animationSource,
                                             "animationSource", prop_names);
    AppendRelationshipPropertyNameIfAuthored(skelroot.skeleton, "skeleton",
                                             prop_names);
  }

  AppendPropertyNamesFromCustomProps(skelroot.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const BlendShape &blendshape,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(blendshape.offsets, "offsets", prop_names);
    AppendPropertyNameIfAuthored(blendshape.normalOffsets, "normalOffsets",
                                 prop_names);
    AppendPropertyNameIfAuthored(blendshape.pointIndices, "pointIndices",
                                 prop_names);
  }

  AppendPropertyNamesFromCustomProps(blendshape.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Skeleton &skel,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(skel.bindTransforms, "bindTransforms",
                                 prop_names);
    AppendPropertyNameIfAuthored(skel.jointNames, "jointNames", prop_names);
    AppendPropertyNameIfAuthored(skel.joints, "joints", prop_names);
    AppendPropertyNameIfAuthored(skel.restTransforms, "restTransforms",
                                 prop_names);
    AppendPropertyNameIfAuthored(skel.extent, "extent", prop_names);
    AppendPropertyNameIfAuthored(skel.purpose, "purpose", prop_names);
    AppendPropertyNameIfAuthored(skel.visibility, "visibility", prop_names);
    AppendXformablePropertyNames(skel, prop_names);
  }

  if (rel_prop) {
    AppendRelationshipPropertyNameIfAuthored(skel.proxyPrim, "proxyPrim",
                                             prop_names);
    AppendRelationshipPropertyNameIfAuthored(skel.animationSource,
                                             "animationSource", prop_names);
  }

  AppendPropertyNamesFromCustomProps(skel.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const SkelAnimation &anim,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(anim.blendShapes, "blendShapes", prop_names);
    AppendPropertyNameIfAuthored(anim.blendShapeWeights,
                                 "blendShapeWeights", prop_names);
    AppendPropertyNameIfAuthored(anim.joints, "joints", prop_names);
    AppendPropertyNameIfAuthored(anim.rotations, "rotations", prop_names);
    AppendPropertyNameIfAuthored(anim.scales, "scales", prop_names);
    AppendPropertyNameIfAuthored(anim.translations, "translations", prop_names);
  }

  AppendPropertyNamesFromCustomProps(anim.props, prop_names, attr_prop,
                                     rel_prop);
  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const UsdUVTexture &tex,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(tex.file, "inputs:file", prop_names);
    AppendPropertyNameIfAuthored(tex.st, "inputs:st", prop_names);
    AppendPropertyNameIfAuthored(tex.uv_set, "inputs:uv_set", prop_names);
    AppendPropertyNameIfAuthored(tex.uv_set_name, "inputs:uv_set_name",
                                 prop_names);
    AppendPropertyNameIfAuthored(tex.wrapS, "inputs:wrapS", prop_names);
    AppendPropertyNameIfAuthored(tex.wrapT, "inputs:wrapT", prop_names);
    AppendPropertyNameIfAuthored(tex.fallback, "inputs:fallback", prop_names);
    AppendPropertyNameIfAuthored(tex.sourceColorSpace,
                                 "inputs:sourceColorSpace", prop_names);
    AppendPropertyNameIfAuthored(tex.scale, "inputs:scale", prop_names);
    AppendPropertyNameIfAuthored(tex.bias, "inputs:bias", prop_names);
    AppendPropertyNameIfAuthored(tex.outputsR, "outputs:r", prop_names);
    AppendPropertyNameIfAuthored(tex.outputsG, "outputs:g", prop_names);
    AppendPropertyNameIfAuthored(tex.outputsB, "outputs:b", prop_names);
    AppendPropertyNameIfAuthored(tex.outputsA, "outputs:a", prop_names);
    AppendPropertyNameIfAuthored(tex.outputsRGB, "outputs:rgb", prop_names);
  }

  AppendPropertyNamesFromCustomProps(tex.props, prop_names, attr_prop,
                                     rel_prop);

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_float &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_float2 &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_float3 &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_float4 &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_int &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_string &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_vector &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_normal &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_point &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPrimvarReader_matrix &preader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  return GetPrimvarReaderPropertyNamesImpl(preader, prop_names, attr_prop,
                                           rel_prop);
}

template <>
bool GetPrimPropertyNamesImpl(const UsdTransform2d &tx,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(tx.in, "inputs:in", prop_names);
    AppendPropertyNameIfAuthored(tx.rotation, "inputs:rotation", prop_names);
    AppendPropertyNameIfAuthored(tx.scale, "inputs:scale", prop_names);
    AppendPropertyNameIfAuthored(tx.translation, "inputs:translation",
                                 prop_names);
    AppendPropertyNameIfAuthored(tx.result, "outputs:result", prop_names);
  }

  AppendPropertyNamesFromCustomProps(tx.props, prop_names, attr_prop,
                                     rel_prop);

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const UsdPreviewSurface &surface,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(surface.diffuseColor, "inputs:diffuseColor",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.emissiveColor,
                                 "inputs:emissiveColor", prop_names);
    AppendPropertyNameIfAuthored(surface.specularColor,
                                 "inputs:specularColor", prop_names);
    AppendPropertyNameIfAuthored(surface.useSpecularWorkflow,
                                 "inputs:useSpecularWorkflow", prop_names);
    AppendPropertyNameIfAuthored(surface.metallic, "inputs:metallic",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.clearcoat, "inputs:clearcoat",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.clearcoatRoughness,
                                 "inputs:clearcoatRoughness", prop_names);
    AppendPropertyNameIfAuthored(surface.roughness, "inputs:roughness",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.opacity, "inputs:opacity",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.opacityThreshold,
                                 "inputs:opacityThreshold", prop_names);
    AppendPropertyNameIfAuthored(surface.ior, "inputs:ior", prop_names);
    AppendPropertyNameIfAuthored(surface.normal, "inputs:normal", prop_names);
    AppendPropertyNameIfAuthored(surface.displacement,
                                 "inputs:displacement", prop_names);
    AppendPropertyNameIfAuthored(surface.occlusion, "inputs:occlusion",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.outputsSurface, "outputs:surface",
                                 prop_names);
    AppendPropertyNameIfAuthored(surface.outputsDisplacement,
                                 "outputs:displacement", prop_names);
  }

  AppendPropertyNamesFromCustomProps(surface.props, prop_names, attr_prop,
                                     rel_prop);

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Material &material,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop) {
    AppendPropertyNameIfAuthored(material.surface, "outputs:surface",
                                 prop_names);
    AppendPropertyNameIfAuthored(material.mtlxSurface,
                                 "outputs:mtlx:surface", prop_names);
    AppendPropertyNameIfAuthored(material.displacement,
                                 "outputs:displacement", prop_names);
    AppendPropertyNameIfAuthored(material.volume, "outputs:volume",
                                 prop_names);
  }

  AppendPropertyNamesFromCustomProps(material.props, prop_names, attr_prop,
                                     rel_prop);

  return true;
}

template <>
bool GetPrimPropertyNamesImpl(const Shader &shader,
                              std::vector<std::string> *prop_names,
                              bool attr_prop, bool rel_prop) {
  if (!prop_names) {
    return false;
  }

  if (attr_prop && !shader.info_id.empty()) {
    prop_names->push_back(kInfoId);
  }

  if (const auto preader_f = shader.value.as<UsdPrimvarReader_float>()) {
    return GetPrimPropertyNamesImpl(*preader_f, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_f2 =
                 shader.value.as<UsdPrimvarReader_float2>()) {
    return GetPrimPropertyNamesImpl(*preader_f2, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_f3 =
                 shader.value.as<UsdPrimvarReader_float3>()) {
    return GetPrimPropertyNamesImpl(*preader_f3, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_f4 =
                 shader.value.as<UsdPrimvarReader_float4>()) {
    return GetPrimPropertyNamesImpl(*preader_f4, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_i = shader.value.as<UsdPrimvarReader_int>()) {
    return GetPrimPropertyNamesImpl(*preader_i, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_s =
                 shader.value.as<UsdPrimvarReader_string>()) {
    return GetPrimPropertyNamesImpl(*preader_s, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_v =
                 shader.value.as<UsdPrimvarReader_vector>()) {
    return GetPrimPropertyNamesImpl(*preader_v, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_n =
                 shader.value.as<UsdPrimvarReader_normal>()) {
    return GetPrimPropertyNamesImpl(*preader_n, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_p =
                 shader.value.as<UsdPrimvarReader_point>()) {
    return GetPrimPropertyNamesImpl(*preader_p, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto preader_m =
                 shader.value.as<UsdPrimvarReader_matrix>()) {
    return GetPrimPropertyNamesImpl(*preader_m, prop_names, attr_prop,
                                    rel_prop);
  } else if (const auto ptx2d = shader.value.as<UsdTransform2d>()) {
    return GetPrimPropertyNamesImpl(*ptx2d, prop_names, attr_prop, rel_prop);
  } else if (const auto ptex = shader.value.as<UsdUVTexture>()) {
    return GetPrimPropertyNamesImpl(*ptex, prop_names, attr_prop, rel_prop);
  } else if (const auto psurf = shader.value.as<UsdPreviewSurface>()) {
    return GetPrimPropertyNamesImpl(*psurf, prop_names, attr_prop, rel_prop);
  }

  AppendPropertyNamesFromCustomProps(shader.props, prop_names, attr_prop,
                                     rel_prop);

  // Generic Shader: parsed shaders with unknown info_id are stored as
  // ShaderNode in `shader.value`; their properties live there, not in
  // `shader.props`.
  if (const auto pnode = shader.value.as<ShaderNode>()) {
    AppendPropertyNamesFromCustomProps(pnode->props, prop_names, attr_prop,
                                       rel_prop);
  }
  return true;
}

}  // namespace

bool GetPropertyNamesForPrim(const lightusd::Prim &prim,
                             std::vector<std::string> *out_prop_names,
                             bool attr_prop, bool rel_prop, std::string *err) {
#define GET_PRIM_PROPERTY_NAMES(__ty)                                      \
  if (prim.is<__ty>()) {                                                   \
    if (!GetPrimPropertyNamesImpl(*prim.as<__ty>(), out_prop_names,        \
                                  attr_prop, rel_prop)) {                  \
      if (err) *err += fmt::format(                                        \
          "Failed to list up Property names of Prim type {}",             \
          value::TypeTraits<__ty>::type_name());                           \
      return false;                                                        \
    }                                                                      \
  } else
  GET_PRIM_PROPERTY_NAMES(Model) GET_PRIM_PROPERTY_NAMES(Xform)
  GET_PRIM_PROPERTY_NAMES(Scope) GET_PRIM_PROPERTY_NAMES(GeomMesh)
  GET_PRIM_PROPERTY_NAMES(GeomCamera) GET_PRIM_PROPERTY_NAMES(GeomSubset)
  GET_PRIM_PROPERTY_NAMES(SphereLight) GET_PRIM_PROPERTY_NAMES(CylinderLight)
  GET_PRIM_PROPERTY_NAMES(RectLight) GET_PRIM_PROPERTY_NAMES(DiskLight)
  GET_PRIM_PROPERTY_NAMES(DistantLight) GET_PRIM_PROPERTY_NAMES(DomeLight)
  GET_PRIM_PROPERTY_NAMES(DomeLight_1) GET_PRIM_PROPERTY_NAMES(GeometryLight)
  GET_PRIM_PROPERTY_NAMES(PortalLight) GET_PRIM_PROPERTY_NAMES(Shader)
  GET_PRIM_PROPERTY_NAMES(Material) GET_PRIM_PROPERTY_NAMES(SkelRoot)
  GET_PRIM_PROPERTY_NAMES(BlendShape) GET_PRIM_PROPERTY_NAMES(Skeleton)
  GET_PRIM_PROPERTY_NAMES(SkelAnimation) {
    if (err) *err += "TODO: Prim type " + prim.type_name();
    return false;
  }
#undef GET_PRIM_PROPERTY_NAMES
  return true;
}

}  // namespace tydra
}  // namespace lightusd
