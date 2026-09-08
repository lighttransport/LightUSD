// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "pprinter.hh"
#include "core/prim.hh"
#include "primvar.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdMtlx.hh"
#include "usdSkel.hh"
#include "scene-access.hh"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace tydra {

#define PushError(msg) \
  if (err) {           \
    *err += msg;       \
  }

constexpr auto kInfoId = "info:id";

template <typename T>
value::TimeSamples EnumTimeSamplesToTypelessTimeSamples(
    const value::TimeSamples &ts) {
  static_assert(std::is_enum<T>::value,
                "EnumTimeSamplesToTypelessTimeSamples is for enum types only.");
  value::TimeSamples dst;
  for (const auto &sample : ts.get_samples()) {
    if (sample.blocked) {
      dst.add_blocked_sample(sample.t, value::Value());
    } else if (const int64_t *iv = sample.value.as<int64_t>()) {
      dst.add_sample(sample.t, value::token(to_string(static_cast<T>(*iv))));
    }
  }
  return dst;
}

namespace {

bool ListSceneNamesRec(const lightusd::Prim &root, uint32_t depth,
                       std::vector<std::pair<bool, std::string>> *sceneNames) {
  if (!sceneNames || depth > kMaxDefaultTraversalLimit) return false;
  if (root.metas().has_sceneName()) {
    sceneNames->emplace_back(root.specifier() == Specifier::Over,
                             root.metas().get_sceneName());
  }
  return true;
}

template <typename T>
bool ToProperty(const TypedAttribute<T> &input, Property &output, std::string *err) {


  Attribute attr;
  attr.variability() = Variability::Uniform;
  attr.set_type_name(value::TypeTraits<T>::type_name());

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {
    attr.set_connections(input.get_connections());
  }

  if (input.has_value()) {
    // Includes !authored()
    if (auto pv = input.get_value()) {
      value::Value val(pv.value());
      primvar::PrimVar pvar;
      pvar.set_value(val);

      attr.set_var(std::move(pvar));
    } else {
      if (err) {
        (*err) += fmt::format("[InternalError] Invalid TypedAttribute<{}> value.", value::TypeTraits<T>::type_name());
      }

      return false;
    }
  }

  attr.metas() = input.metas();

  output = Property(std::move(attr), /* custom */false);


  return true;
}

// Scalar or TimeSample-valued attribute.
// TypedAttribute* => Attribute defined in USD schema, so not a custom attr.
//
template <typename T>
bool ToProperty(const TypedAttribute<Animatable<T>> &input, Property &output, std::string *err) {

  DCOUT("ToProperty ");
  (void)err;

  Attribute attr;

  attr.variability() = Variability::Varying;
  attr.set_type_name(value::TypeTraits<T>::type_name());

  DCOUT("has_connections" << input.has_connections());
  DCOUT("has_value " << input.has_value());
  DCOUT("is_blocked " << input.is_blocked());

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {


    attr.set_connections(input.get_connections());
  }

  //DCOUT("has_default " << input.has_default());
  //DCOUT("has_timesamples " << input.has_timesamples());

  {
    primvar::PrimVar pvar;

    // Includes !authored()
    nonstd::optional<Animatable<T>> aval = input.get_value();
    if (aval) {

      if (aval.value().is_blocked()) {
        attr.set_blocked(true);
      }

      if (aval.value().has_value()) {
        T a;
        if (aval.value().get_default(&a)) {
          value::Value val(a);
          pvar.set_value(val);
        }
      }

      if (aval.value().has_timesamples()) {
        // Value types store a type-erased value::TimeSamples directly; copy it
        // (no typed round-trip).
        if (const value::TimeSamples *tsp =
                aval.value().get_timesamples_ptr()) {
          pvar.set_timesamples(*tsp);
        }
      }

      if (aval.value().has_value() || aval.value().has_timesamples()) {
        attr.set_var(std::move(pvar));
      }

    } else {
      DCOUT("no animatable value.");
    }
  }

  attr.metas() = input.metas();

  output = Property(std::move(attr), /*custom*/ false);

  return true;
}

template <typename T>
bool ToProperty(const TypedAttributeWithFallback<T> &input, Property &output,
                std::string *err) {

  Attribute attr;
  attr.variability() = Variability::Uniform;
  attr.set_type_name(value::TypeTraits<T>::type_name());

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {
    attr.set_connections(input.get_connections());
  }

  if (!input.is_value_empty()) {
    primvar::PrimVar pvar;
    pvar.set_value(value::Value(input.get_value()));
    attr.set_var(std::move(pvar));
  }

  attr.metas() = input.metas();
  output = Property(std::move(attr), /* custom */ false);

  (void)err;
  return true;
}

// Scalar or TimeSample-valued attribute.
// TypedAttribute* => Attribute defined in USD schema, so not a custom attr.
//
// TODO: Support timeSampled attribute.
template <typename T>
bool ToProperty(const TypedAttributeWithFallback<Animatable<T>> &input,
                Property &output, std::string *err) {

  Attribute attr;
  attr.variability() = Variability::Varying;
  attr.set_type_name(value::TypeTraits<T>::type_name());

  DCOUT("has_connections " << input.has_connections());

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {
    attr.set_connections(input.get_connections());
  }

  {
    // Includes !authored()
    // FIXME: Currently scalar only.
    Animatable<T> v = input.get_value();

    primvar::PrimVar pvar;
    DCOUT("has_timesamples " << v.has_timesamples());
    DCOUT("has_value " << v.has_value());

    if (v.has_timesamples()) {
      if (const value::TimeSamples *tsp = v.get_timesamples_ptr()) {
        pvar.set_timesamples(*tsp);
      }
    }

    if (v.has_value()) {
      T a;
      if (v.get_scalar(&a)) {
        value::Value val(a);
        pvar.set_value(val);
      } else {
        DCOUT("??? Invalid Animatable value.");
        if (err) {
          (*err) += "[InternalError] Invalid Animatable value.";
        }
        return false;
      }
    }

    attr.set_var(std::move(pvar));
  }

  attr.metas() = input.metas();

  output = Property(std::move(attr), /* custom */ false);


  return true;
}

// To Property with token type
template <typename T>
bool ToTokenProperty(const TypedAttributeWithFallback<Animatable<T>> &input,
                     Property &output, std::string *err) {

  Attribute attr;
  attr.variability() = Variability::Varying;
  attr.set_type_name(value::kToken);

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {
    attr.set_connections(input.get_connections());
  }

  {
    // Includes !authored()
    const Animatable<T> &v = input.get_value();

    primvar::PrimVar pvar;

    if (v.has_timesamples()) {
      value::TimeSamples ts =
          EnumTimeSamplesToTypelessTimeSamples<T>(*v.get_timesamples_ptr());
      pvar.set_timesamples(std::move(ts));
    }

    if (v.has_default()) {
      T a;
      if (v.get_default(&a)) {
        // to token type
        value::token tok(to_string(a));
        value::Value val(tok);
        pvar.set_value(val);
      } else {
        if (err) {
          (*err) += "[InternalError] Invalid Animatable value.";
        }
        return false;
      }
    }

    if (v.has_timesamples() || v.has_default()) {
      attr.set_var(std::move(pvar));
    }

  }

  attr.metas() = input.metas();

  output = Property(attr, /* custom */ false);

  return true;
}

// To Property with token type
template <typename T>
bool ToTokenProperty(const TypedAttributeWithFallback<T> &input,
                     Property &output, std::string *err) {
  (void)err;

  Attribute attr;
  attr.variability() = Variability::Uniform;
  attr.set_type_name(value::kToken);

  if (input.is_blocked()) {
    attr.set_blocked(input.is_blocked());
  }

  if (input.has_connections()) {
    attr.set_connections(input.get_connections());
  }

  {
    if (!input.is_value_empty()) {
      primvar::PrimVar pvar;
      value::token tok(to_string(input.get_value()));
      value::Value val(tok);
      pvar.set_value(val);
      attr.set_var(std::move(pvar));
    }
  }

  attr.metas() = input.metas();
  output = Property(attr, /* custom */ false);

  return true;
}

template <typename T>
nonstd::optional<Property> TypedTerminalAttributeToProperty(
    const TypedTerminalAttribute<T> &input) {
  if (!input.authored()) {
    // nothing to do
    return nonstd::nullopt;
  }

  Property output;

  // type info only
  if (input.has_actual_type()) {
    // type info only
    output = Property::MakeEmptyAttrib(input.get_actual_type_name(),
                                       /* custom */ false);
  } else {
    output = Property::MakeEmptyAttrib(input.type_name(), /* custom */ false);
  }

  return output;
}

bool XformOpToProperty(const XformOp &x, Property &prop) {
  primvar::PrimVar pv;

  Attribute attr;

  switch (x.op_type) {
    case XformOp::OpType::ResetXformStack: {
      // ??? Not exists in Prim's property
      return false;
    }
    case XformOp::OpType::Transform:
    case XformOp::OpType::Scale:
    case XformOp::OpType::Translate:
    case XformOp::OpType::RotateX:
    case XformOp::OpType::RotateY:
    case XformOp::OpType::RotateZ:
    case XformOp::OpType::Orient:
    case XformOp::OpType::RotateXYZ:
    case XformOp::OpType::RotateXZY:
    case XformOp::OpType::RotateYXZ:
    case XformOp::OpType::RotateYZX:
    case XformOp::OpType::RotateZXY:
    case XformOp::OpType::RotateZYX: {
      pv = x.get_var();
    }
  }

  attr.set_var(std::move(pv));
  // TODO: attribute meta

  prop = Property(attr, /* custom */ false);

  return true;
}

bool ToRelationshipProperty(const nonstd::optional<Relationship> &rel,
                            Property *out_prop) {
  if (!out_prop) {
    return false;
  }

  if (!rel) {
    return false;
  }

  (*out_prop) = Property(rel.value(), /* custom */ false);
  return true;
}

bool ToRelationshipProperty(const RelationshipProperty &rel,
                            Property *out_prop) {
  if (!out_prop) {
    return false;
  }

  if (!rel.authored()) {
    return false;
  }

  (*out_prop) = Property(rel.relationship(), /* custom */ false);
  return true;
}

bool GetXformablePropertyImpl(const Xformable &xformable,
                              const std::map<std::string, Property> &props,
                              const std::string &prop_name,
                              Property *out_prop) {
  if (!out_prop) {
    return false;
  }

  if (prop_name == "xformOpOrder") {
    std::vector<value::token> toks = xformable.xformOpOrder();
    primvar::PrimVar pvar;
    pvar.set_value(toks);

    Attribute attr;
    attr.set_var(std::move(pvar));
    attr.variability() = Variability::Uniform;

    Property prop;
    prop.set_attribute(attr);
    (*out_prop) = prop;
    return true;
  }

  for (const auto &item : xformable.xformOps) {
    std::string op_name = to_string(item.op_type);
    if (!item.suffix.empty()) {
      op_name += ":" + item.suffix;
    }

    if (op_name == prop_name) {
      return XformOpToProperty(item, *out_prop);
    }
  }

  const auto it = props.find(prop_name);
  if (it == props.end()) {
    return false;
  }

  (*out_prop) = it->second;
  return true;
}

#define TO_PROPERTY(__prop_name, __v)                                         \
  if (prop_name == __prop_name) {                                             \
    if (!ToProperty(__v, *out_prop, &err)) {                                  \
      return nonstd::make_unexpected(                                         \
          fmt::format("Convert Property {} failed: {}\n", __prop_name, err)); \
    }                                                                         \
  } else

#define TO_TOKEN_PROPERTY(__prop_name, __v)                                   \
  if (prop_name == __prop_name) {                                             \
    if (!ToTokenProperty(__v, *out_prop, &err)) {                             \
      return nonstd::make_unexpected(                                         \
          fmt::format("Convert Property {} failed: {}\n", __prop_name, err)); \
    }                                                                         \
  } else

#define TO_COMPAT_PROPERTY(__canonical_name, __legacy_name, __v)              \
  if ((prop_name == __canonical_name) || (prop_name == __legacy_name)) {      \
    if (!ToProperty(__v, *out_prop, &err)) {                                  \
      return nonstd::make_unexpected(fmt::format(                             \
          "Convert Property {} failed: {}\n", __canonical_name, err));        \
    }                                                                         \
  } else

// Return true: Property found(`out_prop` filled)
// Return false: Property not found
// Return unexpected: Some eror happened.
template <typename T>
nonstd::expected<bool, std::string> GetPrimProperty(
    const T &prim, const std::string &prop_name, Property *out_prop);

template <typename T>
nonstd::expected<bool, std::string> GetPrimvarReaderPropertyImpl(
    const UsdPrimvarReader<T> &preader, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("inputs:fallback", preader.fallback)
  TO_PROPERTY("inputs:varname", preader.varname)

  if (prop_name == "outputs:result") {
    if (auto pv = TypedTerminalAttributeToProperty(preader.result)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else {
    const auto it = preader.props.find(prop_name);
    if (it == preader.props.end()) {
      return false;
    }

    (*out_prop) = it->second;
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Model &model, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  const auto it = model.props.find(prop_name);
  if (it == model.props.end()) {
    // Attribute not found.
    return false;
  }

  (*out_prop) = it->second;

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Scope &scope, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  const auto it = scope.props.find(prop_name);
  if (it == scope.props.end()) {
    // Attribute not found.
    return false;
  }

  (*out_prop) = it->second;

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Xform &xform, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  if (!GetXformablePropertyImpl(xform, xform.props, prop_name, out_prop)) {
    return false;
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const GeomMesh &mesh, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("points", mesh.points)
  TO_PROPERTY("faceVertexCounts", mesh.faceVertexCounts)
  TO_PROPERTY("faceVertexIndices", mesh.faceVertexIndices)
  TO_PROPERTY("normals", mesh.normals)
  TO_PROPERTY("velocities", mesh.velocities)
  TO_PROPERTY("cornerIndices", mesh.cornerIndices)
  TO_PROPERTY("cornerSharpnesses", mesh.cornerSharpnesses)
  TO_PROPERTY("creaseIndices", mesh.creaseIndices)
  TO_PROPERTY("creaseSharpnesses", mesh.creaseSharpnesses)
  TO_PROPERTY("holeIndices", mesh.holeIndices)
  TO_TOKEN_PROPERTY("interpolateBoundary", mesh.interpolateBoundary)
  TO_TOKEN_PROPERTY("subdivisionScheme", mesh.subdivisionScheme)
  TO_TOKEN_PROPERTY("faceVaryingLinearInterpolation",
                    mesh.faceVaryingLinearInterpolation)

  if (prop_name == "skeleton") {
    if (mesh.skeleton) {
      const Relationship &rel = mesh.skeleton.value();
      (*out_prop) = Property(rel, /* custom */ false);
    } else {
      // empty
      return false;
    }
  } else {
    const auto it = mesh.props.find(prop_name);
    if (it == mesh.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }

  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

nonstd::expected<bool, std::string> GetGPrimPropertyImpl(
    const GPrim &gprim, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;

  TO_PROPERTY("doubleSided", gprim.doubleSided)
  TO_TOKEN_PROPERTY("orientation", gprim.orientation)
  TO_TOKEN_PROPERTY("purpose", gprim.purpose)
  TO_PROPERTY("extent", gprim.extent)
  TO_TOKEN_PROPERTY("visibility", gprim.visibility)

  if (prop_name == kMaterialBinding) {
    if (!ToRelationshipProperty(gprim.materialBinding, out_prop)) {
      return false;
    }
  } else if (prop_name == kMaterialBindingPreview) {
    if (!ToRelationshipProperty(gprim.materialBindingPreview, out_prop)) {
      return false;
    }
  } else if (prop_name == kMaterialBindingFull) {
    if (!ToRelationshipProperty(gprim.materialBindingFull, out_prop)) {
      return false;
    }
  } else if (prop_name.rfind("material:binding:", 0) == 0) {
    const std::string purpose =
        prop_name.substr(sizeof("material:binding:") - 1);
    const auto it = gprim.materialBindingMap().find(purpose);
    if (it == gprim.materialBindingMap().end()) {
      return false;
    }
    (*out_prop) = Property(it->second, /* custom */ false);
  } else if (prop_name == "proxyPrim") {
    if (!ToRelationshipProperty(gprim.proxyPrim, out_prop)) {
      return false;
    }
  } else if (!GetXformablePropertyImpl(gprim, gprim.props, prop_name,
                                       out_prop)) {
    return false;
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const GeomCamera &camera, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;

  TO_PROPERTY("clippingPlanes", camera.clippingPlanes)
  TO_PROPERTY("clippingRange", camera.clippingRange)
  TO_PROPERTY("exposure", camera.exposure)
  TO_PROPERTY("focalLength", camera.focalLength)
  TO_PROPERTY("focusDistance", camera.focusDistance)
  TO_PROPERTY("horizontalAperture", camera.horizontalAperture)
  TO_PROPERTY("horizontalApertureOffset", camera.horizontalApertureOffset)
  TO_PROPERTY("verticalAperture", camera.verticalAperture)
  TO_PROPERTY("verticalApertureOffset", camera.verticalApertureOffset)
  TO_PROPERTY("fStop", camera.fStop)
  TO_TOKEN_PROPERTY("projection", camera.projection)
  TO_TOKEN_PROPERTY("stereoRole", camera.stereoRole)
  TO_PROPERTY("shutterClose", camera.shutterClose)
  TO_PROPERTY("shutterOpen", camera.shutterOpen)

  {
    return GetGPrimPropertyImpl(camera, prop_name, out_prop);
  }

  return true;
}

nonstd::expected<bool, std::string> GetLightAPIPropertyImpl(
    const LightAPI &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;

  TO_PROPERTY("inputs:color", light.color)
  TO_PROPERTY("inputs:colorTemperature", light.colorTemperature)
  TO_PROPERTY("inputs:diffuse", light.diffuse)
  TO_PROPERTY("inputs:enableColorTemperature", light.enableColorTemperature)
  TO_PROPERTY("inputs:exposure", light.exposure)
  TO_PROPERTY("inputs:intensity", light.intensity)
  TO_PROPERTY("inputs:normalize", light.normalize)
  TO_PROPERTY("inputs:specular", light.specular)
  TO_PROPERTY("inputs:shadow:enable", light.shadowEnable)
  TO_PROPERTY("inputs:shadow:color", light.shadowColor)
  TO_PROPERTY("inputs:shadow:distance", light.shadowDistance)
  TO_PROPERTY("inputs:shadow:falloff", light.shadowFalloff)
  TO_PROPERTY("inputs:shadow:falloffGamma", light.shadowFalloffGamma)
  TO_PROPERTY("inputs:shaping:focus", light.shapingFocus)
  TO_PROPERTY("inputs:shaping:focusTint", light.shapingFocusTint)
  TO_PROPERTY("inputs:shaping:cone:angle", light.shapingConeAngle)
  TO_PROPERTY("inputs:shaping:cone:softness", light.shapingConeSoftness)
  TO_PROPERTY("inputs:shaping:ies:file", light.shapingIesFile)
  TO_PROPERTY("inputs:shaping:ies:angleScale", light.shapingIesAngleScale)
  TO_PROPERTY("inputs:shaping:ies:normalize", light.shapingIesNormalize)
  TO_PROPERTY("wavelength:emission", light.spectralEmission)

  if (prop_name == "light:filters") {
    if (!ToRelationshipProperty(light.lightFilters, out_prop)) {
      return false;
    }
  } else {
    return false;
  }

  return true;
}

template <typename T>
nonstd::expected<bool, std::string> GetBoundableLightPropertyImpl(
    const T &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;

  TO_PROPERTY("extent", light.extent)
  TO_TOKEN_PROPERTY("purpose", light.purpose)
  TO_TOKEN_PROPERTY("visibility", light.visibility)

  {
    auto ret = GetLightAPIPropertyImpl(light, prop_name, out_prop);
    if (!ret) {
      return nonstd::make_unexpected(std::move(ret.error()));
    }
    if (ret.value()) {
      return true;
    }

    if (!GetXformablePropertyImpl(light, light.props, prop_name, out_prop)) {
      return false;
    }
  }

  return true;
}

template <typename T>
nonstd::expected<bool, std::string> GetNonboundableLightPropertyImpl(
    const T &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;

  TO_TOKEN_PROPERTY("purpose", light.purpose)
  TO_TOKEN_PROPERTY("visibility", light.visibility)

  {
    auto ret = GetLightAPIPropertyImpl(light, prop_name, out_prop);
    if (!ret) {
      return nonstd::make_unexpected(std::move(ret.error()));
    }
    if (ret.value()) {
      return true;
    }

    if (!GetXformablePropertyImpl(light, light.props, prop_name, out_prop)) {
      return false;
    }
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const SphereLight &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("inputs:radius", light.radius)
  {
    return GetBoundableLightPropertyImpl(light, prop_name, out_prop);
  }
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const CylinderLight &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("inputs:length", light.length)
  TO_PROPERTY("inputs:radius", light.radius)
  {
    return GetBoundableLightPropertyImpl(light, prop_name, out_prop);
  }
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const RectLight &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("inputs:texture:file", light.file)
  TO_PROPERTY("inputs:height", light.height)
  TO_PROPERTY("inputs:width", light.width)
  {
    return GetBoundableLightPropertyImpl(light, prop_name, out_prop);
  }
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const DiskLight &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("inputs:radius", light.radius)
  {
    return GetBoundableLightPropertyImpl(light, prop_name, out_prop);
  }
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const DistantLight &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("inputs:angle", light.angle)
  {
    return GetNonboundableLightPropertyImpl(light, prop_name, out_prop);
  }
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const DomeLight &light, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("guideRadius", light.guideRadius)
  TO_PROPERTY("inputs:texture:file", light.file)
  TO_TOKEN_PROPERTY("inputs:texture:format", light.textureFormat)

  if (prop_name == "portals") {
    if (!ToRelationshipProperty(light.portals, out_prop)) {
      return false;
    }
  } else if (prop_name == "proxyPrim") {
    if (!ToRelationshipProperty(light.proxyPrim, out_prop)) {
      return false;
    }
  } else {
    return GetNonboundableLightPropertyImpl(light, prop_name, out_prop);
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const DomeLight_1 &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  std::string err;
  TO_PROPERTY("guideRadius", light.guideRadius)
  TO_PROPERTY("inputs:texture:file", light.file)
  TO_TOKEN_PROPERTY("inputs:texture:format", light.textureFormat)
  TO_PROPERTY("poleAxis", light.poleAxis)

  if (prop_name == "portals") {
    if (!ToRelationshipProperty(light.portals, out_prop)) {
      return false;
    }
  } else if (prop_name == "proxyPrim") {
    if (!ToRelationshipProperty(light.proxyPrim, out_prop)) {
      return false;
    }
  } else {
    return GetNonboundableLightPropertyImpl(light, prop_name, out_prop);
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const GeometryLight &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  if (prop_name == "geometry") {
    if (!ToRelationshipProperty(light.geometry, out_prop)) {
      return false;
    }
    return true;
  }
  return GetNonboundableLightPropertyImpl(light, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const PortalLight &light, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  if (prop_name == "geometry") {
    if (!ToRelationshipProperty(light.geometry, out_prop)) {
      return false;
    }
    return true;
  }
  return GetNonboundableLightPropertyImpl(light, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const GeomSubset &subset, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  // Currently GeomSubset does not support TimeSamples and AttributeMeta

  std::string err;

  DCOUT("prop_name = " << prop_name);
  TO_PROPERTY("indices", subset.indices);
  TO_TOKEN_PROPERTY("elementType", subset.elementType);
  // TO_TOKEN_PROPERTY("familyType", subset.familyType);
  TO_PROPERTY("familyName", subset.familyName);

  if (prop_name == "material:binding") {
    if (subset.materialBinding.authored()) {
      const Relationship &rel = subset.materialBinding.relationship();
      (*out_prop) = Property(rel, /* custom */ false);
    } else {
      return false;
    }
  } else {
    const auto it = subset.props.find(prop_name);
    if (it == subset.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }

  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdUVTexture &tex, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("inputs:file", tex.file)
  TO_PROPERTY("inputs:st", tex.st)
  TO_PROPERTY("inputs:uv_set", tex.uv_set)
  TO_PROPERTY("inputs:uv_set_name", tex.uv_set_name)
  TO_TOKEN_PROPERTY("inputs:wrapS", tex.wrapS)
  TO_TOKEN_PROPERTY("inputs:wrapT", tex.wrapT)
  TO_PROPERTY("inputs:fallback", tex.fallback)
  TO_TOKEN_PROPERTY("inputs:sourceColorSpace", tex.sourceColorSpace)
  TO_PROPERTY("inputs:scale", tex.scale)
  TO_PROPERTY("inputs:bias", tex.bias)

  if (prop_name == "outputs:r") {
    if (auto pv = TypedTerminalAttributeToProperty(tex.outputsR)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else if (prop_name == "outputs:g") {
    if (auto pv = TypedTerminalAttributeToProperty(tex.outputsG)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else if (prop_name == "outputs:b") {
    if (auto pv = TypedTerminalAttributeToProperty(tex.outputsB)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else if (prop_name == "outputs:a") {
    if (auto pv = TypedTerminalAttributeToProperty(tex.outputsA)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else if (prop_name == "outputs:rgb") {
    if (auto pv = TypedTerminalAttributeToProperty(tex.outputsRGB)) {
      (*out_prop) = pv.value();
    } else {
      return false;
    }
  } else {
    const auto it = tex.props.find(prop_name);
    if (it == tex.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_float2 &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_float3 &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_float4 &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_float &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_int &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_string &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_vector &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_normal &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_point &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPrimvarReader_matrix &preader, const std::string &prop_name,
    Property *out_prop) {
  return GetPrimvarReaderPropertyImpl(preader, prop_name, out_prop);
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdTransform2d &tx, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_COMPAT_PROPERTY("inputs:in", "in", tx.in)
  TO_COMPAT_PROPERTY("inputs:rotation", "rotation", tx.rotation)
  TO_COMPAT_PROPERTY("inputs:scale", "scale", tx.scale)
  TO_COMPAT_PROPERTY("inputs:translation", "translation", tx.translation)

  if (prop_name == "outputs:result") {
    // Terminal attribute
    if (!tx.result.authored()) {
      // not authored
      return false;
    }

    // empty. type info only
    std::string typeName = tx.result.has_actual_type()
                               ? tx.result.get_actual_type_name()
                               : tx.result.type_name();
    (*out_prop) = Property::MakeEmptyAttrib(typeName, /* custom */ false);
  } else {
    const auto it = tx.props.find(prop_name);
    if (it == tx.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const UsdPreviewSurface &surface, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_COMPAT_PROPERTY("inputs:diffuseColor", "diffuseColor", surface.diffuseColor)
  TO_COMPAT_PROPERTY("inputs:emissiveColor", "emissiveColor", surface.emissiveColor)
  TO_COMPAT_PROPERTY("inputs:specularColor", "specularColor", surface.specularColor)
  TO_COMPAT_PROPERTY("inputs:useSpecularWorkflow", "useSpecularWorkflow",
                     surface.useSpecularWorkflow)
  TO_COMPAT_PROPERTY("inputs:metallic", "metallic", surface.metallic)
  TO_COMPAT_PROPERTY("inputs:clearcoat", "clearcoat", surface.clearcoat)
  TO_COMPAT_PROPERTY("inputs:clearcoatRoughness", "clearcoatRoughness",
                     surface.clearcoatRoughness)
  TO_COMPAT_PROPERTY("inputs:roughness", "roughness", surface.roughness)
  TO_COMPAT_PROPERTY("inputs:opacity", "opacity", surface.opacity)
  TO_COMPAT_PROPERTY("inputs:opacityThreshold", "opacityThreshold",
                     surface.opacityThreshold)
  TO_COMPAT_PROPERTY("inputs:ior", "ior", surface.ior)
  TO_COMPAT_PROPERTY("inputs:normal", "normal", surface.normal)
  TO_COMPAT_PROPERTY("inputs:displacement", "displacement",
                     surface.displacement)
  TO_COMPAT_PROPERTY("inputs:occlusion", "occlusion", surface.occlusion)

  if (prop_name == "outputs:surface") {
    if (surface.outputsSurface.authored()) {
      // empty. type info only
      (*out_prop) =
          Property::MakeEmptyAttrib(value::kToken, /* custom */ false);
    } else {
      // Not authored
      return false;
    }
  } else if (prop_name == "outputs:displacement") {
    if (surface.outputsDisplacement.authored()) {
      // empty. type info only
      (*out_prop) =
          Property::MakeEmptyAttrib(value::kToken, /* custom */ false);
    } else {
      // Not authored
      return false;
    }
  } else {
    const auto it = surface.props.find(prop_name);
    if (it == surface.props.end()) {
      // Attribute not found.
      // TODO: report warn?
      return false;
    }

    (*out_prop) = it->second;
  }

  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Material &material, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  if (prop_name == "outputs:surface") {
    if (material.surface.authored()) {
      Attribute attr;
      attr.set_type_name(value::TypeTraits<value::token>::type_name());
      attr.set_connections(material.surface.get_connections());
      attr.metas() = material.surface.metas();
      (*out_prop) = Property(attr, /* custom */ false);
      out_prop->set_listedit_qual(material.surface.get_listedit_qual());
    } else {
      // Not authored
      return false;
    }
  } else if (prop_name == "outputs:mtlx:surface") {
    if (material.mtlxSurface.authored()) {
      Attribute attr;
      attr.set_type_name(value::TypeTraits<value::token>::type_name());
      attr.set_connections(material.mtlxSurface.get_connections());
      attr.metas() = material.mtlxSurface.metas();
      (*out_prop) = Property(attr, /* custom */ false);
      out_prop->set_listedit_qual(material.mtlxSurface.get_listedit_qual());
    } else {
      return false;
    }
  } else if (prop_name == "outputs:displacement") {
    if (material.displacement.authored()) {
      Attribute attr;
      attr.set_type_name(value::TypeTraits<value::token>::type_name());
      attr.set_connections(material.displacement.get_connections());
      attr.metas() = material.displacement.metas();
      (*out_prop) = Property(attr, /* custom */ false);
      out_prop->set_listedit_qual(material.displacement.get_listedit_qual());
    } else {
      // Not authored
      return false;
    }
  } else if (prop_name == "outputs:volume") {
    if (material.volume.authored()) {
      Attribute attr;
      attr.set_type_name(value::TypeTraits<value::token>::type_name());
      attr.set_connections(material.volume.get_connections());
      attr.metas() = material.volume.metas();
      (*out_prop) = Property(attr, /* custom */ false);
      out_prop->set_listedit_qual(material.volume.get_listedit_qual());
    } else {
      // Not authored
      return false;
    }
  } else {
    const auto it = material.props.find(prop_name);
    if (it == material.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }

  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const SkelRoot &skelroot, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("extent", skelroot.extent)
  TO_TOKEN_PROPERTY("purpose", skelroot.purpose)
  TO_TOKEN_PROPERTY("visibility", skelroot.visibility)

  if (prop_name == "proxyPrim") {
    if (!ToRelationshipProperty(skelroot.proxyPrim, out_prop)) {
      return false;
    }
  } else if (prop_name == "animationSource") {
    if (!ToRelationshipProperty(skelroot.animationSource, out_prop)) {
      return false;
    }
  } else if (prop_name == "skeleton") {
    if (!ToRelationshipProperty(skelroot.skeleton, out_prop)) {
      return false;
    }
  } else if (!GetXformablePropertyImpl(skelroot, skelroot.props, prop_name,
                                       out_prop)) {
    return false;
  }
  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());

  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const BlendShape &blendshape, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("offsets", blendshape.offsets)
  TO_PROPERTY("normalOffsets", blendshape.normalOffsets)
  TO_PROPERTY("pointIndices", blendshape.pointIndices)

  {
    const auto it = blendshape.props.find(prop_name);
    if (it == blendshape.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }
  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Skeleton &skel, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("bindTransforms", skel.bindTransforms)
  TO_PROPERTY("jointNames", skel.jointNames)
  TO_PROPERTY("joints", skel.joints)
  TO_PROPERTY("restTransforms", skel.restTransforms)
  TO_PROPERTY("extent", skel.extent)
  TO_TOKEN_PROPERTY("purpose", skel.purpose)
  TO_TOKEN_PROPERTY("visibility", skel.visibility)

  if (prop_name == "proxyPrim") {
    if (!ToRelationshipProperty(skel.proxyPrim, out_prop)) {
      return false;
    }
  } else if (prop_name == "animationSource") {
    if (!ToRelationshipProperty(skel.animationSource, out_prop)) {
      return false;
    }
  } else if (!GetXformablePropertyImpl(skel, skel.props, prop_name, out_prop)) {
    return false;
  }
  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const SkelAnimation &anim, const std::string &prop_name,
    Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  DCOUT("prop_name = " << prop_name);
  std::string err;

  TO_PROPERTY("blendShapes", anim.blendShapes)
  TO_PROPERTY("blendShapeWeights", anim.blendShapeWeights)
  TO_PROPERTY("joints", anim.joints)
  TO_PROPERTY("rotations", anim.rotations)
  TO_PROPERTY("scales", anim.scales)
  TO_PROPERTY("translations", anim.translations)

  {
    const auto it = anim.props.find(prop_name);
    if (it == anim.props.end()) {
      // Attribute not found.
      return false;
    }

    (*out_prop) = it->second;
  }
  DCOUT("Prop found: " << prop_name
                       << ", ty = " << out_prop->value_type_name());
  return true;
}

template <>
nonstd::expected<bool, std::string> GetPrimProperty(
    const Shader &shader, const std::string &prop_name, Property *out_prop) {
  if (!out_prop) {
    return nonstd::make_unexpected(
        "[InternalError] nullptr in output Property is not allowed.");
  }

  if (prop_name == kInfoId) {
    if (shader.info_id.empty()) {
      return false;
    }

    (*out_prop) =
        Property(Attribute::Uniform(value::token(shader.info_id)),
                 /* custom */ false);
    return true;
  }

  // API-schema and other non-node-definition properties are retained in the
  // concrete ShaderNode's custom property map. Consult those maps before the
  // typed node dispatch so properties such as ColorSpaceAPI's
  // `colorSpace:name` remain visible through the generic Prim access API.
  const auto copy_custom_property = [&](const auto *node) {
    if (!node) return false;
    const auto it = node->props.find(prop_name);
    if (it == node->props.end()) return false;
    *out_prop = it->second;
    return true;
  };
  if (copy_custom_property(&shader) ||
      copy_custom_property(shader.value.as<ShaderNode>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_float>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_float2>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_float3>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_float4>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_int>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_string>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_vector>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_normal>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_point>()) ||
      copy_custom_property(shader.value.as<UsdPrimvarReader_matrix>()) ||
      copy_custom_property(shader.value.as<UsdTransform2d>()) ||
      copy_custom_property(shader.value.as<UsdUVTexture>()) ||
      copy_custom_property(shader.value.as<UsdPreviewSurface>()) ||
      copy_custom_property(shader.value.as<OpenPBRSurface>()) ||
      copy_custom_property(shader.value.as<MtlxOpenPBRSurface>()) ||
      copy_custom_property(
          shader.value.as<MtlxAutodeskStandardSurface>())) {
    return true;
  }

  if (const auto preader_f = shader.value.as<UsdPrimvarReader_float>()) {
    return GetPrimProperty(*preader_f, prop_name, out_prop);
  } else if (const auto preader_f2 =
                 shader.value.as<UsdPrimvarReader_float2>()) {
    return GetPrimProperty(*preader_f2, prop_name, out_prop);
  } else if (const auto preader_f3 =
                 shader.value.as<UsdPrimvarReader_float3>()) {
    return GetPrimProperty(*preader_f3, prop_name, out_prop);
  } else if (const auto preader_f4 =
                 shader.value.as<UsdPrimvarReader_float4>()) {
    return GetPrimProperty(*preader_f4, prop_name, out_prop);
  } else if (const auto preader_i = shader.value.as<UsdPrimvarReader_int>()) {
    return GetPrimProperty(*preader_i, prop_name, out_prop);
  } else if (const auto preader_s =
                 shader.value.as<UsdPrimvarReader_string>()) {
    return GetPrimProperty(*preader_s, prop_name, out_prop);
  } else if (const auto preader_v =
                 shader.value.as<UsdPrimvarReader_vector>()) {
    return GetPrimProperty(*preader_v, prop_name, out_prop);
  } else if (const auto preader_n =
                 shader.value.as<UsdPrimvarReader_normal>()) {
    return GetPrimProperty(*preader_n, prop_name, out_prop);
  } else if (const auto preader_p =
                 shader.value.as<UsdPrimvarReader_point>()) {
    return GetPrimProperty(*preader_p, prop_name, out_prop);
  } else if (const auto preader_m =
                 shader.value.as<UsdPrimvarReader_matrix>()) {
    return GetPrimProperty(*preader_m, prop_name, out_prop);
  } else if (const auto ptx2d = shader.value.as<UsdTransform2d>()) {
    return GetPrimProperty(*ptx2d, prop_name, out_prop);
  } else if (const auto ptex = shader.value.as<UsdUVTexture>()) {
    return GetPrimProperty(*ptex, prop_name, out_prop);
  } else if (const auto psurf = shader.value.as<UsdPreviewSurface>()) {
    return GetPrimProperty(*psurf, prop_name, out_prop);
  } else if (shader.value.as<ShaderNode>() ||
             shader.value.as<OpenPBRSurface>() ||
             shader.value.as<MtlxOpenPBRSurface>() ||
             shader.value.as<MtlxAutodeskStandardSurface>()) {
    return false;
  } else {
    return nonstd::make_unexpected("TODO: " + shader.value.type_name());
  }
}

#undef TO_PROPERTY
#undef TO_TOKEN_PROPERTY
#undef TO_COMPAT_PROPERTY

}  // namespace

bool GetPropertyNamesForPrim(const lightusd::Prim &prim,
                             std::vector<std::string> *out_prop_names,
                             bool attr_prop, bool rel_prop, std::string *err);

bool GetProperty(const lightusd::Prim &prim, const std::string &attr_name,
                 Property *out_prop, std::string *err) {
#define GET_PRIM_PROPERTY(__ty)                                         \
  if (prim.is<__ty>()) {                                                \
    auto ret = GetPrimProperty(*prim.as<__ty>(), attr_name, out_prop);  \
    if (ret) {                                                          \
      if (!ret.value()) {                                               \
        PUSH_ERROR_AND_RETURN(                                          \
            fmt::format("Attribute `{}` does not exist in Prim {}({})", \
                        attr_name, prim.element_path().prim_part(),     \
                        value::TypeTraits<__ty>::type_name()));         \
      }                                                                 \
    } else {                                                            \
      PUSH_ERROR_AND_RETURN(ret.error());                               \
    }                                                                   \
  } else

  GET_PRIM_PROPERTY(Model)
  GET_PRIM_PROPERTY(Xform)
  GET_PRIM_PROPERTY(Scope)
  GET_PRIM_PROPERTY(GeomMesh)
  GET_PRIM_PROPERTY(GeomCamera)
  GET_PRIM_PROPERTY(GeomSubset)
  GET_PRIM_PROPERTY(SphereLight)
  GET_PRIM_PROPERTY(CylinderLight)
  GET_PRIM_PROPERTY(RectLight)
  GET_PRIM_PROPERTY(DiskLight)
  GET_PRIM_PROPERTY(DistantLight)
  GET_PRIM_PROPERTY(DomeLight)
  GET_PRIM_PROPERTY(DomeLight_1)
  GET_PRIM_PROPERTY(GeometryLight)
  GET_PRIM_PROPERTY(PortalLight)
  GET_PRIM_PROPERTY(Shader)
  GET_PRIM_PROPERTY(Material)
  GET_PRIM_PROPERTY(SkelRoot)
  GET_PRIM_PROPERTY(BlendShape)
  GET_PRIM_PROPERTY(Skeleton)
  GET_PRIM_PROPERTY(SkelAnimation)
  GET_PRIM_PROPERTY(SkelAnimation) {
    PUSH_ERROR_AND_RETURN("TODO: Prim type " << prim.type_name());
  }

#undef GET_PRIM_PROPERTY

  return true;
}

bool GetPropertyNames(const lightusd::Prim &prim,
                      std::vector<std::string> *out_prop_names,
                      std::string *err) {
  return GetPropertyNamesForPrim(prim, out_prop_names, true, true, err);
}

bool GetAttributeNames(const lightusd::Prim &prim,
                       std::vector<std::string> *out_attr_names,
                       std::string *err) {
  return GetPropertyNamesForPrim(prim, out_attr_names, true, false, err);
}

bool GetRelationshipNames(const lightusd::Prim &prim,
                          std::vector<std::string> *out_rel_names,
                          std::string *err) {
  return GetPropertyNamesForPrim(prim, out_rel_names, false, true, err);
}

bool GetAttribute(const lightusd::Prim &prim, const std::string &attr_name,
                  Attribute *out_attr, std::string *err) {
  if (!out_attr) {
    PUSH_ERROR_AND_RETURN("`out_attr` argument is nullptr.");
  }

  // First lookup as Property, then check if its Attribute
  Property prop;
  if (!GetProperty(prim, attr_name, &prop, err)) {
    return false;
  }

  if (prop.is_attribute()) {
    (*out_attr) = std::move(prop.get_attribute());
    return true;
  }

  PUSH_ERROR_AND_RETURN(fmt::format("{} is not a Attribute.", attr_name));
}

bool GetRelationship(const lightusd::Prim &prim, const std::string &rel_name,
                     Relationship *out_rel, std::string *err) {
  if (!out_rel) {
    PUSH_ERROR_AND_RETURN("`out_rel` argument is nullptr.");
  }

  // First lookup as Property, then check if its Relationship
  Property prop;
  if (!GetProperty(prim, rel_name, &prop, err)) {
    return false;
  }

  if (prop.is_relationship()) {
    (*out_rel) = std::move(prop.get_relationship());
    return true;
  }

  PUSH_ERROR_AND_RETURN(fmt::format("{} is not a Relationship.", rel_name));

  return true;
}

bool ListSceneNames(const lightusd::Prim &root,
                    std::vector<std::pair<bool, std::string>> *sceneNames) {
  if (!sceneNames) {
    return false;
  }

  bool has_sceneLibrary = false;
  if (root.metas().has_kind()) {
    if (root.metas().get_kind_enum() == Kind::SceneLibrary) {
      // ok
      has_sceneLibrary = true;
    }
  }

  if (!has_sceneLibrary) {
    return false;
  }

  for (const Prim &child : root.children()) {
    if (!ListSceneNamesRec(child, /* depth */ 0, sceneNames)) {
      return false;
    }
  }

  return true;
}

bool ShaderToPrimSpec(const UsdTransform2d &node, PrimSpec &ps,
                      std::string *warn, std::string *err) {
  (void)warn;

#define TO_PROPERTY(__prop_name, __v)                                    \
  {                                                                      \
    Property prop;                                                       \
    if (!ToProperty(__v, prop, err)) {                                   \
      PUSH_ERROR_AND_RETURN(                                             \
          fmt::format("Convert {} to Property failed.\n", __prop_name)); \
    }                                                                    \
    ps.props()[__prop_name] = prop;                                      \
  }

  // inputs
  TO_PROPERTY("inputs:in", node.in)
  TO_PROPERTY("inputs:rotation", node.rotation)
  TO_PROPERTY("inputs:scale", node.scale)
  TO_PROPERTY("inputs:translation", node.translation)

  // outputs
  if (auto pv = TypedTerminalAttributeToProperty(node.result)) {
    ps.props()["outputs:result"] = pv.value();
  }

  for (auto prop : node.props) {
    ps.props()[prop.first] = prop.second;
  }

  ps.props()[kInfoId] =
      Property(Attribute::Uniform(value::token(kUsdTransform2d)));
  ps.metas() = node.metas();
  ps.name() = node.name;
  ps.specifier() = node.spec;

  return true;
}

#undef PushError

}  // namespace tydra
}  // namespace lightusd
