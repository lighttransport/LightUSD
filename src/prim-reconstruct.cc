// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Reconstruct concrete Prim from PropertyMap or PrimSpec.
//
// TODO:
//   - [ ] Refactor code
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdGeom.hh"
#include "usdSkel.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdMtlx.hh"

#include "common-macros.inc"
#include "value-types.hh"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

// __VA_ARGS__ does not allow empty, thus # of args must be 2+
#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

//
// NOTE:
//
// There are mainly 5 variant of Primtive property(relationship/attribute)
//
// - TypedAttribute<T> : Uniform only. `uniform T` or `uniform T var.connect`
// - TypedAttribute<Animatable<T>> : Varying. `T var`, `T var = val`, `T var.connect` or `T value.timeSamples`
// - optional<T> : For output attribute(Just author it. e.g. `float outputs:rgb`)
// - Relationship : Typeless relation(e.g. `rel material:binding`)
// - TypedConnection : Typed relation(e.g. `token outputs:result = </material/diffuse.rgb>`)

namespace lightusd {
namespace prim {

//constexpr auto kTag = "[PrimReconstruct]";

[[maybe_unused]] constexpr auto kProxyPrim = "proxyPrim";
[[maybe_unused]] constexpr auto kVisibility = "visibility";
[[maybe_unused]] constexpr auto kExtent = "extent";
[[maybe_unused]] constexpr auto kPurpose = "purpose";
[[maybe_unused]] constexpr auto kMaterialBinding = "material:binding";
[[maybe_unused]] constexpr auto kMaterialBindingCollection = "material:binding:collection";
[[maybe_unused]] constexpr auto kMaterialBindingPreview = "material:binding:preview";
[[maybe_unused]] constexpr auto kSkelSkeleton = "skel:skeleton";
[[maybe_unused]] constexpr auto kSkelAnimationSource = "skel:animationSource";
[[maybe_unused]] constexpr auto kSkelBlendShapes = "skel:blendShapes";
[[maybe_unused]] constexpr auto kSkelBlendShapeTargets = "skel:blendShapeTargets";
// kInputsVarname moved to prim-reconstruct-shader.cc

// MaterialX Validation Helpers moved to prim-reconstruct-shader.cc


///
/// LightUSD reconstruct some frequently used shaders(e.g. UsdPreviewSurface)
/// here, not in Tydra
///
template <typename T>
bool ReconstructShader(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options);


#include "prim-reconstruct-common.inc"


static bool ReconstructXformOpFromToken(

  const std::string &token, int i,
  std::map<std::string, Property> &properties,
  std::set<std::string> &table, /* inout */
  std::vector<XformOp> *xformOps, std::string *err) {
  if (!xformOps) {
    PUSH_ERROR_AND_RETURN("Internal error: xformOps ptr is null");
  }

  constexpr auto kTranslate = "xformOp:translate";
  constexpr auto kTransform = "xformOp:transform";
  constexpr auto kScale = "xformOp:scale";
  constexpr auto kRotateX = "xformOp:rotateX";
  constexpr auto kRotateY = "xformOp:rotateY";
  constexpr auto kRotateZ = "xformOp:rotateZ";
  constexpr auto kRotateXYZ = "xformOp:rotateXYZ";
  constexpr auto kRotateXZY = "xformOp:rotateXZY";
  constexpr auto kRotateYXZ = "xformOp:rotateYXZ";
  constexpr auto kRotateYZX = "xformOp:rotateYZX";
  constexpr auto kRotateZXY = "xformOp:rotateZXY";
  constexpr auto kRotateZYX = "xformOp:rotateZYX";
  constexpr auto kOrient = "xformOp:orient";

  // false : no prefix found.
  // true : return suffix(first namespace ':' is ommited.).
  // - "" for prefix only "xformOp:translate"
  // - "blender:pivot" for "xformOp:translate:blender:pivot"
  auto SplitXformOpToken =
      [](const std::string &s,
         const std::string &prefix) -> nonstd::optional<std::string> {
    if (startsWith(s, prefix)) {
      if (s.compare(prefix) == 0) {
        // prefix only.
        return std::string();  // empty suffix
      } else {
        std::string suffix = removePrefix(s, prefix);
        DCOUT("suffix = " << suffix);
        if (suffix.length() == 1) {  // maybe namespace only.
          return nonstd::nullopt;
        }

        // remove namespace ':'
        if (suffix[0] == ':') {
          // ok
          suffix.erase(0, 1);
        } else {
          return nonstd::nullopt;
        }

        return std::move(suffix);
      }
    }

    return nonstd::nullopt;
  };

  std::string tok = token;
        XformOp op;

        DCOUT("xformOp token = " << tok);

        if (startsWith(tok, "!resetXformStack!")) {
          if (tok.compare("!resetXformStack!") != 0) {
            PUSH_ERROR_AND_RETURN(
                "`!resetXformStack!` must be defined solely(not to be a prefix "
                "to \"xformOp:*\")");
          }

          if (i != 0) {
            PUSH_ERROR_AND_RETURN(
                "`!resetXformStack!` must appear at the first element of "
                "xformOpOrder list.");
          }

          op.op_type = XformOp::OpType::ResetXformStack;
          xformOps->emplace_back(std::move(op));

          // skip looking up property
          return true;
        }

        if (startsWith(tok, "!invert!")) {
          DCOUT("invert!");
          op.inverted = true;
          tok = removePrefix(tok, "!invert!");
          DCOUT("tok = " << tok);
        }

        auto it = properties.find(tok);
        if (it == properties.end()) {
          PUSH_ERROR_AND_RETURN("Property `" + tok + "` not found.");
        }
        if (it->second.is_attribute_connection()) {
          PUSH_ERROR_AND_RETURN(
              "Connection(.connect) for xformOp attribute is not yet supported: "
              "`" +
              tok + "`");
        }
        const Attribute &attr = it->second.get_attribute();

        // Check `xformOp` namespace
        if (auto xfm = SplitXformOpToken(tok, kTransform)) {
          op.op_type = XformOp::OpType::Transform;
          op.suffix = xfm.value();  // may contain nested namespaces

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::matrix4d>::type_id()) {
                value::matrix4d dummy{value::matrix4d::identity()};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:transform` must be type `matrix4d`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::matrix4d>()) {
              op.set_value(pvd.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:transform` must be type `matrix4d`, but got type `" +
                  attr.type_name() + "`.");
            }
          }

        } else if (auto tx = SplitXformOpToken(tok, kTranslate)) {
          op.op_type = XformOp::OpType::Translate;
          op.suffix = tx.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:translate` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:translate` must be type `double3` or `float3`, but "
                  "got type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto scale = SplitXformOpToken(tok, kScale)) {
          op.op_type = XformOp::OpType::Scale;
          op.suffix = scale.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:scale` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:scale` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotX = SplitXformOpToken(tok, kRotateX)) {
          op.op_type = XformOp::OpType::RotateX;
          op.suffix = rotX.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<double>::type_id()) {
                double dummy(0.0);
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<float>::type_id()) {
                float dummy(0.0f);
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateX` must be type `double` or `float`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<double>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<float>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateX` must be type `double` or `float`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotY = SplitXformOpToken(tok, kRotateY)) {
          op.op_type = XformOp::OpType::RotateY;
          op.suffix = rotY.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<double>::type_id()) {
                double dummy(0.0);
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<float>::type_id()) {
                float dummy(0.0f);
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateY` must be type `double` or `float`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<double>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<float>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateY` must be type `double` or `float`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotZ = SplitXformOpToken(tok, kRotateZ)) {
          op.op_type = XformOp::OpType::RotateZ;
          op.suffix = rotZ.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<double>::type_id()) {
                double dummy(0.0);
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<float>::type_id()) {
                float dummy(0.0f);
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateZ` must be type `double` or `float`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<double>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<float>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateZ` must be type `double` or `float`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateXYZ = SplitXformOpToken(tok, kRotateXYZ)) {
          op.op_type = XformOp::OpType::RotateXYZ;
          op.suffix = rotateXYZ.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateXYZ` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateXYZ` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateXZY = SplitXformOpToken(tok, kRotateXZY)) {
          op.op_type = XformOp::OpType::RotateXZY;
          op.suffix = rotateXZY.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateXZY` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateXZY` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateYXZ = SplitXformOpToken(tok, kRotateYXZ)) {
          op.op_type = XformOp::OpType::RotateYXZ;
          op.suffix = rotateYXZ.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateYXZ` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateYXZ` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateYZX = SplitXformOpToken(tok, kRotateYZX)) {
          op.op_type = XformOp::OpType::RotateYZX;
          op.suffix = rotateYZX.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateYZX` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateYZX` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateZXY = SplitXformOpToken(tok, kRotateZXY)) {
          op.op_type = XformOp::OpType::RotateZXY;
          op.suffix = rotateZXY.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateZXY` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateZXY` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto rotateZYX = SplitXformOpToken(tok, kRotateZYX)) {
          op.op_type = XformOp::OpType::RotateZYX;
          op.suffix = rotateZYX.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::double3>::type_id()) {
                value::double3 dummy{0.0, 0.0, 0.0};
                op.set_value(dummy);
              } else if (attr.type_id() == value::TypeTraits<value::float3>::type_id()) {
                value::float3 dummy{0.0f, 0.0f, 0.0f};
                op.set_value(dummy);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:rotateZYX` must be type `double3` or `float3`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::double3>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::float3>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:rotateZYX` must be type `double3` or `float3`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else if (auto orient = SplitXformOpToken(tok, kOrient)) {
          op.op_type = XformOp::OpType::Orient;
          op.suffix = orient.value();

          // Check if timeSamples were authored (even if empty)
          if (attr.get_var().has_timesamples() || attr.get_var().ts_raw().type_id() != 0) {
            op.set_timesamples(attr.get_var().ts_raw());
          }

          if (attr.get_var().has_default()) {
            if (attr.has_blocked()) {
              // Set dummy value for `op.get_value_type_id/op.get_value_type_name'
              if (attr.type_id() == value::TypeTraits<value::quatf>::type_id()) {
                value::quatf q;
                q.real = 1.0f;
                q.imag = {0.0f, 0.0f, 0.0f};
                op.set_value(q);
              } else if (attr.type_id() == value::TypeTraits<value::quatd>::type_id()) {
                value::quatd q;
                q.real = 1.0;
                q.imag = {0.0, 0.0, 0.0};
                op.set_value(q);
              } else {
                PUSH_ERROR_AND_RETURN(
                    "`xformOp:orient` must be type `quatf` or `quatd`, but got "
                    "type `" +
                    attr.type_name() + "`.");
              }
              op.set_blocked(true);
            } else if (auto pvd = attr.get_value<value::quatf>()) {
              op.set_value(pvd.value());
            } else if (auto pvf = attr.get_value<value::quatd>()) {
              op.set_value(pvf.value());
            } else {
              PUSH_ERROR_AND_RETURN(
                  "`xformOp:orient` must be type `quatf` or `quatd`, but got "
                  "type `" +
                  attr.type_name() + "`.");
            }
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "token for xformOpOrder must have namespace `xformOp:***`, or .");
        }

        xformOps->emplace_back(std::move(op));
        table.insert(tok);

    return true;
  }


bool ReconstructXformOpsFromProperties(
  const Specifier &spec,
  std::set<std::string> &table, /* inout */
  std::map<std::string, Property> &properties,
  std::vector<XformOp> *xformOps,
  std::string *err)
{

  if (spec == Specifier::Class) {
    // Do not materialize xformOps here.
    return true;
  }


  // Lookup xform values from `xformOpOrder`
  // TODO: TimeSamples, Connection
  if (properties.count("xformOpOrder")) {
    // array of string
    auto prop = properties.at("xformOpOrder");

    // 'uniform' check
    if (prop.get_attribute().variability() != Variability::Uniform) {
      PUSH_ERROR_AND_RETURN("`xformOpOrder` must have `uniform` variability.");
    }

    //const Attribute &attr = prop.get_attribute();
    //const auto &v = attr.get_var();
    //LIGHTUSD_LOG_I("attr.value.type " << v.type_name());

    if (prop.is_relationship()) {
      PUSH_ERROR_AND_RETURN("Relationship for `xformOpOrder` is not supported.");
    } else if (auto tpv =
                   prop.get_attribute().get_value<TypedArray<value::token>>()) {


      for (size_t i = 0; i < tpv.value().size(); i++) {
        const auto &item = tpv.value()[i];

        if (!ReconstructXformOpFromToken(item.str(), int(i), properties, table, xformOps, err)) {
          return false;
        }
      }
    } else if (auto pv =
                   prop.get_attribute().get_value<std::vector<value::token>>()) {

      // 'uniform' check
      if (prop.get_attribute().variability() != Variability::Uniform) {
        PUSH_ERROR_AND_RETURN("`xformOpOrder` must have `uniform` variability.");
      }

      for (size_t i = 0; i < pv.value().size(); i++) {
        const auto &item = pv.value()[i];

        if (!ReconstructXformOpFromToken(item.str(), int(i), properties, table, xformOps, err)) {
          return false;
        }
      }

    } else {
      PUSH_ERROR_AND_RETURN(
          "`xformOpOrder` must be type `token[]` but got type `"
          << prop.get_attribute().type_name() << "`.");
    }
  }

  table.insert("xformOpOrder");
  return true;
}

#include "prim-reconstruct-geom-detail.inc"


template <>
bool ReconstructPrim<Xform>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Xform *xform,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)options;
  (void)references;

  std::set<std::string> table;
  if (!ReconstructGPrimProperties(spec, table, properties, xform, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    ADD_PROPERTY(table, prop, Xform, xform->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Model>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Model *model,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Model ");
  (void)spec;
  (void)references;
  (void)model;
  (void)err;
  (void)options;

  std::set<std::string> table;
  if (!ReconstructMaterialBindingProperties(table, properties, model, err)) {
    return false;
  }
  if (!ReconstructCollectionProperties(
          table, properties, model, warn, err,
          options.strict_allowedToken_check)) {
    return false;
  }
  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    ADD_PROPERTY(table, prop, Model, model->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Scope>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Scope *scope,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  // `Scope` is just a namespace in scene graph(no node xform)

  (void)spec;
  (void)references;
  (void)scope;
  (void)err;
  (void)options;

  DCOUT("Scope");
  std::set<std::string> table;
  if (!ReconstructMaterialBindingProperties(table, properties, scope, err)) {
    return false;
  }
  if (!ReconstructCollectionProperties(
          table, properties, scope, warn, err,
          options.strict_allowedToken_check)) {
    return false;
  }
  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, Scope,
                   scope->visibility, options.strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, Scope,
                   scope->purpose, options.strict_allowedToken_check)
    ADD_PROPERTY(table, prop, Scope, scope->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

// Placeholder schema prim types (UsdVol / UsdRender): retain all authored
// properties generically in `prim->props` (no typed schema fields yet).
#define RECONSTRUCT_PLACEHOLDER_PRIM(__ty)                              \
  template <>                                                           \
  bool ReconstructPrim<__ty>(                                           \
      const Specifier &spec, PropertyMap &properties,                   \
      const ReferenceList &references, __ty *prim, std::string *warn,   \
      std::string *err, const PrimReconstructOptions &options) {        \
    (void)spec;                                                         \
    (void)references;                                                   \
    (void)err;                                                          \
    (void)options;                                                      \
    std::set<std::string> table;                                        \
    for (auto &prop : properties) {                                     \
      ADD_PROPERTY(table, prop, __ty, prim->props)                      \
      PARSE_PROPERTY_END_MAKE_WARN(table, prop)                         \
    }                                                                   \
    return true;                                                        \
  }

RECONSTRUCT_PLACEHOLDER_PRIM(RenderSettings)
RECONSTRUCT_PLACEHOLDER_PRIM(RenderProduct)
RECONSTRUCT_PLACEHOLDER_PRIM(RenderVar)
RECONSTRUCT_PLACEHOLDER_PRIM(GenerativeProcedural)
#undef RECONSTRUCT_PLACEHOLDER_PRIM

template <>
bool ReconstructPrim(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GPrim *gprim,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)gprim;
  (void)err;

  (void)references;
  (void)properties;

  std::set<std::string> table;
  if (!ReconstructGPrimProperties(spec, table, properties, gprim, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

  return true;
}

#define RECONSTRUCT_PRIM_PRIMSPEC_IMPL(__prim_ty) \
template <> \
bool ReconstructPrim<__prim_ty>( \
    PrimSpec &primspec, \
    __prim_ty *prim, \
    std::string *warn, \
    std::string *err, \
    const PrimReconstructOptions &options) { \
 \
  ReferenceList references; /* dummy */ \
 \
  return ReconstructPrim<__prim_ty>(primspec.specifier(), primspec.props(), references, prim, warn, err, options); \
}

#define RECONSTRUCT_PRIM_PROPERTYMAP_DECL(__prim_ty) \
template <> \
bool ReconstructPrim<__prim_ty>( \
    const Specifier &spec, \
    PropertyMap &properties, \
    const ReferenceList &references, \
    __prim_ty *prim, \
    std::string *warn, \
    std::string *err, \
    const PrimReconstructOptions &options);

RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomMesh)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(Volume)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(FieldAsset)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(OpenVDBAsset)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(Field3DAsset)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomPoints)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomCylinder)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomCube)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomCone)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomSphere)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomCapsule)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomBasisCurves)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomCamera)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomSubset)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeomPointInstancer)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(SphereLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(DomeLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(CylinderLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(DiskLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(DistantLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(RectLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(GeometryLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(PortalLight)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(DomeLight_1)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(LightFilter)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(PluginLightFilter)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(SkelRoot)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(Skeleton)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(SkelAnimation)
RECONSTRUCT_PRIM_PROPERTYMAP_DECL(BlendShape)
#undef RECONSTRUCT_PRIM_PROPERTYMAP_DECL

RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Xform)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Model)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Scope)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(RenderSettings)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(RenderProduct)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(RenderVar)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GenerativeProcedural)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomMesh)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Volume)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(FieldAsset)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(OpenVDBAsset)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Field3DAsset)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomPoints)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomCylinder)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomCube)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomCone)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomSphere)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomCapsule)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomBasisCurves)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomCamera)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomSubset)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeomPointInstancer)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(SphereLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(DomeLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(CylinderLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(DiskLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(DistantLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(RectLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(GeometryLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PortalLight)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(DomeLight_1)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(LightFilter)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PluginLightFilter)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(SkelRoot)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Skeleton)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(SkelAnimation)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(BlendShape)
// Shader, Material, NodeGraph PrimSpec wrappers are in prim-reconstruct-shader.cc
// Physics + MuJoCo PrimSpec wrappers are in prim-reconstruct-physics.cc


} // namespace prim

} // namespace lightusd
