// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
//
// Material and texture conversion routines split from render-data.cc
//
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

#include "common-utils.hh"
#include "common-types.hh"
#include "color-management.hh"
#include "enum-handlers.hh"
#include "../tiny-hashmap.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#include "linear-algebra.hh"
#include "math-util.inc"
#include "pprinter.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "core/safe-arithmetic.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "value-pprint.hh"
#include "bone-util.hh"
#include "tydra/shape-to-mesh.hh"
#include "materialx-to-json.hh"
#include "mmap-array-ref.hh"
#include "materialx-to-json.hh"
#include "core/security-policy.hh"

//
#include "common-macros.inc"
#include "math-util.inc"


//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

#include "tydra/render-data-material-internal.hh"

namespace lightusd {
namespace tydra {

namespace {

constexpr int kMtlxGraphEvaluationDepth = 64;


// Structure to hold MaterialX NodeGraph info extracted from geometry_normal/geometry_tangent connections
// Used to capture tangent rotation and normal map scale for anisotropic materials

// Extract MaterialX NodeGraph info by traversing connections
// Returns the extracted info or an error message
// Multi-component constant value used by the MaterialX constant evaluator.
// Represents float (1 component) or color3f (3 components).

// Resolve a shader node within a NodeGraph prim by its absolute path.
// Tries stage lookup first, then falls back to searching NodeGraph children.
static const Shader *FindShaderInNodeGraph(
    const Stage &stage, const Prim *ng_prim,
    const std::string &prim_path) {
  std::string err;
  const Prim *prim{nullptr};
  if (stage.find_prim_at_path(Path(prim_path, ""), prim, &err) && prim) {
    return prim->as<Shader>();
  }
  if (ng_prim) {
    size_t last_slash = prim_path.rfind('/');
    if (last_slash != std::string::npos) {
      std::string child_name = prim_path.substr(last_slash + 1);
      for (const auto &child : ng_prim->children()) {
        if (child.element_name() == child_name) {
          return child.as<Shader>();
        }
      }
    }
  }
  return nullptr;
}

static const Prim *FindNodeGraphPrimForPath(
    const Stage &stage, const Prim *scope_prim,
    const std::string &prim_path) {
  std::string err;
  const Prim *prim{nullptr};
  if (stage.find_prim_at_path(Path(prim_path, ""), prim, &err) && prim &&
      prim->as<NodeGraph>()) {
    return prim;
  }
  if (scope_prim) {
    size_t last_slash = prim_path.rfind('/');
    if (last_slash != std::string::npos) {
      std::string child_name = prim_path.substr(last_slash + 1);
      for (const auto &child : scope_prim->children()) {
        if ((child.element_name() == child_name ||
             child.data().type_name() == "NodeGraph") &&
            child.as<NodeGraph>()) {
          return &child;
        }
      }
    }
  }
  return nullptr;
}

// Forward declaration.
// `output_name` is the name of the node output being read (the prop_part of the
// connection that led here, e.g. "outr"/"outg"/"outb" on a separate node, with
// any "outputs:" prefix stripped). It is only meaningful for multi-output nodes
// (separate/swizzle); single-output nodes ignore it.
static nonstd::expected<MtlxConstVal, std::string> EvaluateMtlxConstant(
    const Stage &stage, const Prim *ng_prim, const Shader *shader,
    int max_depth, const std::string &evaluation_space,
    const std::string &output_name = "");

static const Prim *FindPrimForShader(const Prim *root, const Shader *shader) {
  if (!root || !shader) return nullptr;
  if (root->as<Shader>() == shader) return root;
  for (const Prim &child : root->children()) {
    if (const Prim *found = FindPrimForShader(&child, shader)) return found;
  }
  return nullptr;
}

static bool TransformMtlxColorInput(
    const Stage &stage, const Prim *shader_prim, const Attribute &attribute,
    const std::string &evaluation_space, MtlxConstVal *value) {
  if (!shader_prim || !value || value->n < 3 || evaluation_space.empty()) {
    return false;
  }
  std::string source;
  bool authored = false;
  if (!color_management::ComputeColorSpaceName(
          stage, shader_prim->absolute_path(), &attribute.metas(), &source,
          &authored)) {
    return false;
  }
  if (!authored) source = evaluation_space;
  color::ColorTransform transform;
  if (!color_management::BuildColorTransform(
          stage, shader_prim->absolute_path(), source, evaluation_space,
          &transform)) {
    return false;
  }
  float rgb[3] = {value->v[0], value->v[1], value->v[2]};
  color::TransformRGB(transform, rgb);
  value->v[0] = rgb[0];
  value->v[1] = rgb[1];
  value->v[2] = rgb[2];
  value->color_managed =
      transform.source.kind != color::ColorSpaceKind::Data;
  return true;
}

static nonstd::expected<std::pair<const Prim *, const Shader *>, std::string>
ResolveNodeGraphTarget(const Stage &stage, const Path &connection_path,
                       std::string *target_out_name = nullptr);

// Helper: get the property map for a shader (prefer ShaderNode props if present)
static const std::map<std::string, Property> &GetShaderProps(
    const Shader *shader) {
  const ShaderNode *sn = shader->value.as<ShaderNode>();
  if (sn && !sn->props.empty()) return sn->props;
  return shader->props;
}

static const Shader *FindMtlxShaderForPath(
    const Stage &stage, const Prim *material_prim,
    const std::string &prim_path) {
  std::string err;
  const Prim *prim{nullptr};
  if (stage.find_prim_at_path(Path(prim_path, ""), prim, &err) && prim) {
    return prim->as<Shader>();
  }
  if (!material_prim) {
    return nullptr;
  }

  const size_t last_slash = prim_path.rfind('/');
  const std::string target_name =
      (last_slash != std::string::npos) ? prim_path.substr(last_slash + 1)
                                        : prim_path;
  for (const auto &child : material_prim->children()) {
    if (!child.as<NodeGraph>()) {
      continue;
    }
    for (const auto &ng_child : child.children()) {
      if (ng_child.element_name() == target_name) {
        return ng_child.as<Shader>();
      }
    }
  }
  return nullptr;
}

static bool MtlxInputConnection(const std::map<std::string, Property> &props,
                                const std::string &name, Path *path_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (!attr.has_connections() || attr.connections().empty()) {
    return false;
  }
  if (path_out) {
    *path_out = attr.connections()[0];
  }
  return true;
}

static bool MtlxStringOrTokenInput(
    const std::map<std::string, Property> &props, const std::string &name,
    std::string *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (auto v = attr.get_value<std::string>()) {
    if (value_out) {
      *value_out = *v;
    }
    return true;
  }
  if (auto v = attr.get_value<value::token>()) {
    if (value_out) {
      *value_out = v->str();
    }
    return true;
  }
  return false;
}

static bool MtlxIntInput(const std::map<std::string, Property> &props,
                         const std::string &name, int *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (auto v = attr.get_value<int>()) {
    if (value_out) {
      *value_out = *v;
    }
    return true;
  }
  return false;
}

static bool MtlxFloat2Input(const std::map<std::string, Property> &props,
                            const std::string &name, value::float2 *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (auto v = attr.get_value<value::float2>()) {
    if (value_out) {
      *value_out = *v;
    }
    return true;
  }
  return false;
}

static void ExtractMtlxTexcoordInfoFromConnection(
    const Stage &stage, const Prim *material_prim, const Path &start_path,
    MtlxNodeGraphInfo *info) {
  if (!info) {
    return;
  }

  Path current_path = start_path;
  int depth = kMtlxGraphEvaluationDepth;
  while (depth-- > 0) {
    const Shader *shader =
        FindMtlxShaderForPath(stage, material_prim, current_path.prim_part());
    if (!shader) {
      return;
    }
    const std::map<std::string, Property> &props = GetShaderProps(shader);
    const std::string &node_type = shader->info_id;

    if (node_type.find("ND_geompropvalue_") == 0) {
      std::string geomprop;
      if (MtlxStringOrTokenInput(props, "inputs:geomprop", &geomprop) &&
          !geomprop.empty()) {
        info->geomprop_name = geomprop;
        info->has_geomprop = true;
      }
      return;
    }

    if (node_type == "ND_texcoord_vector2" ||
        node_type == "ND_texcoord_vector3") {
      MtlxIntInput(props, "inputs:index", &info->texcoord_index);
      return;
    }

    if (node_type.find("ND_place2d_") == 0) {
      value::float2 v;
      if (MtlxFloat2Input(props, "inputs:scale", &v) ||
          MtlxFloat2Input(props, "inputs:uvtiling", &v)) {
        info->uvtiling[0] = v[0];
        info->uvtiling[1] = v[1];
        info->has_uvtransform = true;
      }
      if (MtlxFloat2Input(props, "inputs:translation", &v) ||
          MtlxFloat2Input(props, "inputs:uvoffset", &v)) {
        info->uvoffset[0] = v[0];
        info->uvoffset[1] = v[1];
        info->has_uvtransform = true;
      }
      if (MtlxInputConnection(props, "inputs:texcoord", &current_path)) {
        continue;
      }
      return;
    }

    if (MtlxInputConnection(props, "inputs:texcoord", &current_path) ||
        MtlxInputConnection(props, "inputs:in", &current_path) ||
        MtlxInputConnection(props, "inputs:in1", &current_path)) {
      continue;
    }
    return;
  }
}

// Helper: resolve a generic input to MtlxConstVal (constant or connected)
static nonstd::expected<MtlxConstVal, std::string> ResolveInput(
    const Stage &stage, const Prim *ng_prim,
    const Prim *shader_prim,
    const std::map<std::string, Property> &props,
    const std::string &input_name, const std::string &node_type,
    int max_depth, const std::string &evaluation_space) {
  auto it = props.find(input_name);
  if (it == props.end()) {
    return nonstd::make_unexpected(
        fmt::format("{} not found in {}", input_name, node_type));
  }
  if (!it->second.is_attribute()) {
    return nonstd::make_unexpected(
        fmt::format("{} is not an attribute", input_name));
  }
  const Attribute &attr = it->second.get_attribute();

  // If connected, follow recursively, carrying the connected output's name so
  // multi-output nodes (separate/swizzle) can return the requested channel.
  if (attr.has_connections() && !attr.connections().empty()) {
    const Path &conn = attr.connections()[0];
    if (FindNodeGraphPrimForPath(stage, ng_prim, conn.prim_part())) {
      std::string out_name;
      auto target = ResolveNodeGraphTarget(stage, conn, &out_name);
      if (!target) return nonstd::make_unexpected(target.error());
      return EvaluateMtlxConstant(stage, target->first, target->second,
                                  max_depth - 1, evaluation_space, out_name);
    }
    const Shader *next = FindShaderInNodeGraph(stage, ng_prim, conn.prim_part());
    if (!next) {
      return nonstd::make_unexpected(
          fmt::format("Cannot find shader at {}", conn.prim_part()));
    }
    std::string out_name = conn.prop_part();
    // USD stores MaterialX outputs as "outputs:<name>"; strip the schema prefix.
    if (out_name.compare(0, 8, "outputs:") == 0) out_name = out_name.substr(8);
    return EvaluateMtlxConstant(stage, ng_prim, next, max_depth - 1,
                                evaluation_space, out_name);
  }

  // Read constant value
  if (auto vf = attr.get_value<float>()) {
    return MtlxConstVal::Float(*vf);
  }
  if (auto vc = attr.get_value<value::color3f>()) {
    MtlxConstVal result = MtlxConstVal::Color3((*vc)[0], (*vc)[1], (*vc)[2]);
    if (!TransformMtlxColorInput(stage, shader_prim, attr, evaluation_space,
                                 &result)) {
      return nonstd::make_unexpected("Failed to transform MaterialX color3 input");
    }
    return result;
  }
  if (auto vf3 = attr.get_value<value::float3>()) {
    return MtlxConstVal::Color3((*vf3)[0], (*vf3)[1], (*vf3)[2]);
  }
  if (auto vc4 = attr.get_value<value::color4f>()) {
    MtlxConstVal result = MtlxConstVal::Color4(
        (*vc4)[0], (*vc4)[1], (*vc4)[2], (*vc4)[3]);
    if (!TransformMtlxColorInput(stage, shader_prim, attr, evaluation_space,
                                 &result)) {
      return nonstd::make_unexpected("Failed to transform MaterialX color4 input");
    }
    return result;
  }
  if (auto vf4 = attr.get_value<value::float4>()) {
    return MtlxConstVal::Color4((*vf4)[0], (*vf4)[1], (*vf4)[2],
                                (*vf4)[3]);
  }
  if (auto vf2 = attr.get_value<value::float2>()) {
    MtlxConstVal r;
    r.n = 2;
    r.v[0] = (*vf2)[0];
    r.v[1] = (*vf2)[1];
    return r;
  }
  if (auto vi = attr.get_value<int>()) {
    return MtlxConstVal::Float(static_cast<float>(*vi));
  }
  return nonstd::make_unexpected(
      fmt::format("Cannot read value from {}", input_name));
}

// Helper: convenience wrapper for ResolveInput
static nonstd::expected<MtlxConstVal, std::string> RI(
    const Stage &stage, const Prim *ng_prim,
    const Prim *shader_prim,
    const std::map<std::string, Property> &props,
    const std::string &input_name, const std::string &node_type,
    int max_depth, const std::string &evaluation_space) {
  return ResolveInput(stage, ng_prim, shader_prim, props, input_name,
                      node_type, max_depth, evaluation_space);
}

// Per-component binary operation on MtlxConstVal
static MtlxConstVal BinOp(const MtlxConstVal &a, const MtlxConstVal &b,
                           float (*op)(float, float)) {
  int nc = (std::max)(a.n, b.n);
  MtlxConstVal r; r.n = nc;
  r.color_managed = a.color_managed || b.color_managed;
  for (int i = 0; i < nc; ++i) {
    float va = (i < a.n) ? a.v[static_cast<size_t>(i)] : a.v[0];
    float vb = (i < b.n) ? b.v[static_cast<size_t>(i)] : b.v[0];
    r.v[static_cast<size_t>(i)] = op(va, vb);
  }
  return r;
}

// Simple RGB-to-HSV and HSV-to-RGB conversions (used by ND_hsv_adjust_color3).
static void RGBtoHSV(float r, float g, float b, float &h, float &s, float &v) {
  float mx = (std::max)({r, g, b});
  float mn = (std::min)({r, g, b});
  float d = mx - mn;
  v = mx;
  s = (mx > 0.0f) ? d / mx : 0.0f;
  if (d < 1e-7f) { h = 0.0f; return; }
  if (r >= mx)      h = (g - b) / d;
  else if (g >= mx) h = 2.0f + (b - r) / d;
  else              h = 4.0f + (r - g) / d;
  h /= 6.0f;
  if (h < 0.0f) h += 1.0f;
}

static void HSVtoRGB(float h, float s, float v, float &r, float &g, float &b) {
  if (s <= 0.0f) { r = g = b = v; return; }
  float hh = h * 6.0f;
  if (hh >= 6.0f) hh = 0.0f;
  int i = static_cast<int>(hh);
  float ff = hh - static_cast<float>(i);
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * ff);
  float t = v * (1.0f - s * (1.0f - ff));
  switch (i) {
    case 0: r=v; g=t; b=p; break;
    case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break;
    case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break;
    default: r=v; g=p; b=q; break;
  }
}

// Map a MaterialX separate-node output name ("outr"/"outg"/"outb"/"outa" or
// "outx"/"outy"/"outz"/"outw") to a component index. Returns -1 for an unnamed
// or unrecognized output.
static int MtlxOutputChannelIndex(const std::string &out_name) {
  if (out_name == "outr" || out_name == "outx") return 0;
  if (out_name == "outg" || out_name == "outy") return 1;
  if (out_name == "outb" || out_name == "outz") return 2;
  if (out_name == "outa" || out_name == "outw") return 3;
  return -1;
}

// Map a single MaterialX swizzle channel character to a component index.
// r/x=0, g/y=1, b/z=2, a/w=3. Returns -1 for '0'/'1' constants or unknowns.
static int MtlxSwizzleCharIndex(char c) {
  switch (c) {
    case 'r': case 'x': return 0;
    case 'g': case 'y': return 1;
    case 'b': case 'z': return 2;
    case 'a': case 'w': return 3;
    default: return -1;
  }
}

// Evaluate a MaterialX node graph expression that produces a constant value.
// Supports float and color3 node types with constant or connected inputs.
// Returns the evaluated value or an error.
static nonstd::expected<MtlxConstVal, std::string> EvaluateMtlxConstant(
    const Stage &stage,
    const Prim *ng_prim,
    const Shader *shader,
    int max_depth,
    const std::string &evaluation_space,
    const std::string &output_name) {
  if (max_depth <= 0) {
    return nonstd::make_unexpected("Max evaluation depth exceeded");
  }
  if (!shader) {
    return nonstd::make_unexpected("Null shader");
  }

  const std::string &nt = shader->info_id;
  const auto &props = GetShaderProps(shader);
  const Prim *shader_prim = FindPrimForShader(ng_prim, shader);

  // Macro-style shorthand for resolving inputs
  #define RESOLVE(name) RI(stage, ng_prim, shader_prim, props, name, nt, \
                           max_depth, evaluation_space)

  // --- Constant nodes ---
  if (nt == "ND_constant_float" || nt == "ND_constant_color3" ||
      nt == "ND_constant_color4" || nt == "ND_constant_vector2" ||
      nt == "ND_constant_vector3" || nt == "ND_constant_vector4") {
    auto r = RESOLVE("inputs:value");
    if (!r) return nonstd::make_unexpected(r.error());
    if (nt == "ND_constant_float") return MtlxConstVal::Float(r->as_float());
    return *r;
  }

  // --- Binary float/color ops ---
  if (nt.find("ND_add_") == 0 || nt.find("ND_subtract_") == 0 ||
      nt.find("ND_multiply_") == 0 || nt.find("ND_divide_") == 0 ||
      nt.find("ND_min_") == 0 || nt.find("ND_max_") == 0 ||
      nt.find("ND_power_") == 0 || nt.find("ND_modulo_") == 0 ||
      nt.find("ND_atan2_") == 0) {
    auto a = RESOLVE("inputs:in1");
    if (!a) return nonstd::make_unexpected(a.error());
    auto b = RESOLVE("inputs:in2");
    if (!b) return nonstd::make_unexpected(b.error());

    if (nt.find("ND_add_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return x + y; });
    if (nt.find("ND_subtract_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return x - y; });
    if (nt.find("ND_multiply_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return x * y; });
    if (nt.find("ND_divide_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return (y != 0.0f) ? x / y : 0.0f; });
    if (nt.find("ND_power_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return std::pow(x, y); });
    if (nt.find("ND_min_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return (std::min)(x, y); });
    if (nt.find("ND_max_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return (std::max)(x, y); });
    if (nt.find("ND_modulo_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return (y != 0.0f) ? std::fmod(x, y) : 0.0f; });
    if (nt.find("ND_atan2_") == 0)
      return BinOp(*a, *b, [](float x, float y) { return std::atan2(x, y); });
  }

  // --- Clamp (float or color3) ---
  if (nt.find("ND_clamp_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    auto lo = RESOLVE("inputs:low");
    if (!lo) return nonstd::make_unexpected(lo.error());
    auto hi = RESOLVE("inputs:high");
    if (!hi) return nonstd::make_unexpected(hi.error());
    auto clamped = BinOp(*v, *lo, [](float x, float y) { return (std::max)(x, y); });
    return BinOp(clamped, *hi, [](float x, float y) { return (std::min)(x, y); });
  }

  // --- Unary math ops (single input → single output) ---
  // ND_absval, ND_sign, ND_floor, ND_ceil, ND_round, ND_fract,
  // ND_sqrt, ND_invsqrt, ND_exp, ND_log,
  // ND_sin, ND_cos, ND_tan, ND_asin, ND_acos
  if (nt.find("ND_absval_") == 0 || nt.find("ND_sign_") == 0 ||
      nt.find("ND_floor_") == 0 || nt.find("ND_ceil_") == 0 ||
      nt.find("ND_round_") == 0 || nt.find("ND_fract_") == 0 ||
      nt.find("ND_sqrt_") == 0 || nt.find("ND_invsqrt_") == 0 ||
      nt.find("ND_exp_") == 0 || nt.find("ND_log_") == 0 ||
      nt.find("ND_sin_") == 0 || nt.find("ND_cos_") == 0 ||
      nt.find("ND_tan_") == 0 || nt.find("ND_asin_") == 0 ||
      nt.find("ND_acos_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    MtlxConstVal r; r.n = v->n; r.color_managed = v->color_managed;
    for (int i = 0; i < v->n; ++i) {
      float x = v->v[static_cast<size_t>(i)];
      float y = 0.0f;
      if (nt.find("ND_absval_") == 0) y = std::abs(x);
      else if (nt.find("ND_sign_") == 0) y = (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
      else if (nt.find("ND_floor_") == 0) y = std::floor(x);
      else if (nt.find("ND_ceil_") == 0) y = std::ceil(x);
      else if (nt.find("ND_round_") == 0) y = std::round(x);
      else if (nt.find("ND_fract_") == 0) y = x - std::floor(x);
      else if (nt.find("ND_sqrt_") == 0) y = (x >= 0.0f) ? std::sqrt(x) : 0.0f;
      else if (nt.find("ND_invsqrt_") == 0) y = (x > 0.0f) ? 1.0f / std::sqrt(x) : 0.0f;
      else if (nt.find("ND_exp_") == 0) y = std::exp(x);
      else if (nt.find("ND_log_") == 0) y = (x > 0.0f) ? std::log(x) : 0.0f;
      else if (nt.find("ND_sin_") == 0) y = std::sin(x);
      else if (nt.find("ND_cos_") == 0) y = std::cos(x);
      else if (nt.find("ND_tan_") == 0) y = std::tan(x);
      else if (nt.find("ND_asin_") == 0) y = std::asin((std::max)(-1.0f, (std::min)(1.0f, x)));
      else if (nt.find("ND_acos_") == 0) y = std::acos((std::max)(-1.0f, (std::min)(1.0f, x)));
      r.v[static_cast<size_t>(i)] = y;
    }
    return r;
  }

  // --- Convert (type conversion passthrough) ---
  if (nt.find("ND_convert_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    // float→color3: broadcast, color3→float: take first component
    if (nt.find("_float_color3") != std::string::npos ||
        nt.find("_float_vector3") != std::string::npos) {
      MtlxConstVal r =
          MtlxConstVal::Color3(v->as_float(), v->as_float(), v->as_float());
      r.color_managed = v->color_managed;
      return r;
    }
    if (nt.find("_color3_float") != std::string::npos ||
        nt.find("_vector3_float") != std::string::npos) {
      MtlxConstVal r = MtlxConstVal::Float(v->v[0]);
      r.color_managed = v->color_managed;
      return r;
    }
    // color3<->vector3 are the same layout
    return *v;
  }

  // --- Luminance (color3 → float via Rec.709 coefficients) ---
  if (nt == "ND_luminance_color3") {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    // Rec.709 luminance coefficients
    float lum = 0.2126f * v->v[0] + 0.7152f * v->v[1] + 0.0722f * v->v[2];
    MtlxConstVal r = MtlxConstVal::Color3(lum, lum, lum);
    r.color_managed = v->color_managed;
    return r;
  }

  // --- Dot product (vector3 → float) ---
  if (nt.find("ND_dotproduct_") == 0) {
    auto a = RESOLVE("inputs:in1");
    if (!a) return nonstd::make_unexpected(a.error());
    auto b = RESOLVE("inputs:in2");
    if (!b) return nonstd::make_unexpected(b.error());
    float dot = 0.0f;
    int nc = (std::min)(a->n, b->n);
    for (int i = 0; i < nc; ++i) dot += a->v[static_cast<size_t>(i)] * b->v[static_cast<size_t>(i)];
    return MtlxConstVal::Float(dot);
  }

  // --- Cross product (vector3 × vector3 → vector3) ---
  if (nt.find("ND_crossproduct_") == 0) {
    auto a = RESOLVE("inputs:in1");
    if (!a) return nonstd::make_unexpected(a.error());
    auto b = RESOLVE("inputs:in2");
    if (!b) return nonstd::make_unexpected(b.error());
    MtlxConstVal r = MtlxConstVal::Color3(
        a->v[1] * b->v[2] - a->v[2] * b->v[1],
        a->v[2] * b->v[0] - a->v[0] * b->v[2],
        a->v[0] * b->v[1] - a->v[1] * b->v[0]);
    r.color_managed = a->color_managed || b->color_managed;
    return r;
  }

  // --- Magnitude (vector3 → float) ---
  if (nt.find("ND_magnitude_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    float mag = 0.0f;
    for (int i = 0; i < v->n; ++i) mag += v->v[static_cast<size_t>(i)] * v->v[static_cast<size_t>(i)];
    MtlxConstVal r = MtlxConstVal::Float(std::sqrt(mag));
    r.color_managed = v->color_managed;
    return r;
  }

  // --- Normalize (vector3 → vector3) ---
  if (nt.find("ND_normalize_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    float mag = 0.0f;
    for (int i = 0; i < v->n; ++i) mag += v->v[static_cast<size_t>(i)] * v->v[static_cast<size_t>(i)];
    mag = std::sqrt(mag);
    if (mag < 1e-7f) return *v;
    MtlxConstVal r; r.n = v->n; r.color_managed = v->color_managed;
    for (int i = 0; i < v->n; ++i) r.v[static_cast<size_t>(i)] = v->v[static_cast<size_t>(i)] / mag;
    return r;
  }

  // --- Conditionals: ifgreater / ifgreatereq / ifequal (any output type) ---
  // Compares scalar value1 vs value2 and selects in1/in2. Covers every typed
  // variant (ND_ifgreater_float, ND_ifgreater_color3, ...), superseding the
  // former float-only handler.
  if (nt.find("ND_ifgreater_") == 0 || nt.find("ND_ifgreatereq_") == 0 ||
      nt.find("ND_ifequal_") == 0) {
    auto val1 = RESOLVE("inputs:value1");
    if (!val1) return nonstd::make_unexpected(val1.error());
    auto val2 = RESOLVE("inputs:value2");
    if (!val2) return nonstd::make_unexpected(val2.error());
    auto in1 = RESOLVE("inputs:in1");
    if (!in1) return nonstd::make_unexpected(in1.error());
    auto in2 = RESOLVE("inputs:in2");
    if (!in2) return nonstd::make_unexpected(in2.error());
    const float a = val1->as_float(), b = val2->as_float();
    bool cond;
    if (nt.find("ND_ifgreatereq_") == 0) cond = (a >= b);
    else if (nt.find("ND_ifgreater_") == 0) cond = (a > b);
    else {
      const float scale = (std::max)((std::max)(std::fabs(a), std::fabs(b)), 1.0f);
      cond = std::fabs(a - b) <= std::numeric_limits<float>::epsilon() * scale;
    }
    return cond ? *in1 : *in2;
  }

  // --- Smoothstep (Hermite interpolation, per component) ---
  if (nt.find("ND_smoothstep_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    auto lo = RESOLVE("inputs:low");
    if (!lo) return nonstd::make_unexpected(lo.error());
    auto hi = RESOLVE("inputs:high");
    if (!hi) return nonstd::make_unexpected(hi.error());
    MtlxConstVal r; r.n = v->n;
    r.color_managed = v->color_managed || lo->color_managed ||
                      hi->color_managed;
    for (int i = 0; i < v->n; ++i) {
      size_t ui = static_cast<size_t>(i);
      float l = (i < lo->n) ? lo->v[ui] : lo->v[0];
      float h = (i < hi->n) ? hi->v[ui] : hi->v[0];
      float denom = h - l;
      float t = (std::abs(denom) > 1e-7f) ? (v->v[ui] - l) / denom : 0.0f;
      t = (std::max)(0.0f, (std::min)(1.0f, t));
      r.v[ui] = t * t * (3.0f - 2.0f * t);
    }
    return r;
  }

  // --- Saturate (adjust color saturation toward/away from luminance) ---
  // MaterialX definition: out = lerp(luminance(in), in, amount). NOT clamp01.
  if (nt.find("ND_saturate_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    auto amt = RESOLVE("inputs:amount");
    const float a = amt ? amt->as_float() : 1.0f;  // amount defaults to 1
    const float lum =
        0.2126f * v->v[0] + 0.7152f * v->v[1] + 0.0722f * v->v[2];
    MtlxConstVal r; r.n = v->n; r.color_managed = v->color_managed;
    for (int i = 0; i < v->n; ++i) {
      size_t ui = static_cast<size_t>(i);
      r.v[ui] = lum + (v->v[ui] - lum) * a;
    }
    return r;
  }

  // --- Mix (lerp between bg and fg) ---
  if (nt.find("ND_mix_") == 0) {
    auto bg = RESOLVE("inputs:bg");
    if (!bg) return nonstd::make_unexpected(bg.error());
    auto fg = RESOLVE("inputs:fg");
    if (!fg) return nonstd::make_unexpected(fg.error());
    auto mx = RESOLVE("inputs:mix");
    if (!mx) return nonstd::make_unexpected(mx.error());
    float t = mx->as_float();
    int nc = (std::max)(bg->n, fg->n);
    MtlxConstVal r; r.n = nc;
    r.color_managed = bg->color_managed || fg->color_managed;
    for (int i = 0; i < nc; ++i) {
      float a = (i < bg->n) ? bg->v[static_cast<size_t>(i)] : bg->v[0];
      float b = (i < fg->n) ? fg->v[static_cast<size_t>(i)] : fg->v[0];
      r.v[static_cast<size_t>(i)] = a * (1.0f - t) + b * t;
    }
    return r;
  }

  // --- Remap: linearly remap from [inlow,inhigh] to [outlow,outhigh] ---
  if (nt.find("ND_remap_") == 0) {
    auto v = RESOLVE("inputs:in");
    if (!v) return nonstd::make_unexpected(v.error());
    auto inlo = RESOLVE("inputs:inlow");
    if (!inlo) return nonstd::make_unexpected(inlo.error());
    auto inhi = RESOLVE("inputs:inhigh");
    if (!inhi) return nonstd::make_unexpected(inhi.error());
    auto outlo = RESOLVE("inputs:outlow");
    if (!outlo) return nonstd::make_unexpected(outlo.error());
    auto outhi = RESOLVE("inputs:outhigh");
    if (!outhi) return nonstd::make_unexpected(outhi.error());
    int nc = v->n;
    MtlxConstVal r; r.n = nc;
    r.color_managed = v->color_managed || inlo->color_managed ||
                      inhi->color_managed || outlo->color_managed ||
                      outhi->color_managed;
    for (int i = 0; i < nc; ++i) {
      size_t ui = static_cast<size_t>(i);
      float vi = v->v[ui];
      float il = (i < inlo->n) ? inlo->v[ui] : inlo->v[0];
      float ih = (i < inhi->n) ? inhi->v[ui] : inhi->v[0];
      float ol = (i < outlo->n) ? outlo->v[ui] : outlo->v[0];
      float oh = (i < outhi->n) ? outhi->v[ui] : outhi->v[0];
      float denom = ih - il;
      float t_val = (std::abs(denom) > 1e-7f) ? (vi - il) / denom : 0.0f;
      r.v[ui] = ol + t_val * (oh - ol);
    }
    return r;
  }

  // --- Combine2 (two floats → vector2) ---
  if (nt.find("ND_combine2_") == 0) {
    auto c1 = RESOLVE("inputs:in1");
    if (!c1) return nonstd::make_unexpected(c1.error());
    auto c2 = RESOLVE("inputs:in2");
    if (!c2) return nonstd::make_unexpected(c2.error());
    MtlxConstVal r; r.n = 2;
    r.color_managed = c1->color_managed || c2->color_managed;
    r.v[0] = c1->as_float();
    r.v[1] = c2->as_float();
    return r;
  }

  // --- Combine3 (three floats → color3) ---
  if (nt.find("ND_combine3_") == 0) {
    auto c1 = RESOLVE("inputs:in1");
    if (!c1) return nonstd::make_unexpected(c1.error());
    auto c2 = RESOLVE("inputs:in2");
    if (!c2) return nonstd::make_unexpected(c2.error());
    auto c3 = RESOLVE("inputs:in3");
    if (!c3) return nonstd::make_unexpected(c3.error());
    MtlxConstVal r =
        MtlxConstVal::Color3(c1->as_float(), c2->as_float(), c3->as_float());
    r.color_managed = c1->color_managed || c2->color_managed ||
                      c3->color_managed;
    return r;
  }

  // --- Combine4 (four floats -> color4/vector4) ---
  if (nt.find("ND_combine4_") == 0) {
    auto c1 = RESOLVE("inputs:in1");
    if (!c1) return nonstd::make_unexpected(c1.error());
    auto c2 = RESOLVE("inputs:in2");
    if (!c2) return nonstd::make_unexpected(c2.error());
    auto c3 = RESOLVE("inputs:in3");
    if (!c3) return nonstd::make_unexpected(c3.error());
    auto c4 = RESOLVE("inputs:in4");
    if (!c4) return nonstd::make_unexpected(c4.error());
    MtlxConstVal r = MtlxConstVal::Color4(
        c1->as_float(), c2->as_float(), c3->as_float(), c4->as_float());
    r.color_managed = c1->color_managed || c2->color_managed ||
                      c3->color_managed || c4->color_managed;
    return r;
  }

  // --- Extract (vectorN → float by index) ---
  if (nt.find("ND_extract_") == 0) {
    auto in_v = RESOLVE("inputs:in");
    if (!in_v) return nonstd::make_unexpected(in_v.error());
    auto idx = RESOLVE("inputs:index");
    if (!idx) return nonstd::make_unexpected(idx.error());
    int i = static_cast<int>(idx->as_float());
    if (i < 0 || i >= in_v->n) i = 0;
    MtlxConstVal r =
        MtlxConstVal::Float(in_v->v[static_cast<size_t>(i)]);
    r.color_managed = in_v->color_managed;
    return r;
  }

  // --- Separate2/3/4 (vectorN → per-channel float outputs) ---
  // The requested output channel comes from the connection that led here
  // (output_name = outr/outg/outb/outa or outx/outy/outz/outw). When the caller
  // did not name a channel, fall back to returning the whole value.
  if (nt.find("ND_separate2_") == 0 || nt.find("ND_separate3_") == 0 ||
      nt.find("ND_separate4_") == 0) {
    auto in_v = RESOLVE("inputs:in");
    if (!in_v) return nonstd::make_unexpected(in_v.error());
    int ch = MtlxOutputChannelIndex(output_name);
    if (ch < 0) return *in_v;  // unnamed output
    if (ch >= in_v->n) ch = in_v->n - 1;
    MtlxConstVal r =
        MtlxConstVal::Float(in_v->v[static_cast<size_t>(ch)]);
    r.color_managed = in_v->color_managed;
    return r;
  }

  // --- Swizzle (reorder/broadcast channels via a "channels" string) ---
  if (nt.find("ND_swizzle_") == 0) {
    auto in_v = RESOLVE("inputs:in");
    if (!in_v) return nonstd::make_unexpected(in_v.error());
    std::string channels;
    auto cit = props.find("inputs:channels");
    if (cit != props.end() && cit->second.is_attribute()) {
      const Attribute &ca = cit->second.get_attribute();
      if (auto s = ca.get_value<std::string>()) channels = *s;
      else if (auto t = ca.get_value<value::token>()) channels = t->str();
    }
    if (channels.empty()) return *in_v;
    MtlxConstVal r;
    r.n = (std::min)(static_cast<int>(channels.size()), 4);
    r.color_managed = in_v->color_managed;
    for (int i = 0; i < r.n; ++i) {
      int ch = MtlxSwizzleCharIndex(channels[static_cast<size_t>(i)]);
      float val = 0.0f;
      if (ch >= 0) {
        if (ch >= in_v->n) ch = in_v->n - 1;
        val = in_v->v[static_cast<size_t>(ch)];
      }
      r.v[static_cast<size_t>(i)] = val;
    }
    return r;
  }

  // --- HSV adjust (two variants) ---
  // ND_hsv_adjust_color3: separate hue/saturation/value/fac inputs
  // ND_hsvadjust_color3: Blender export variant with inputs:amount (float3: h,s,v)
  if (nt == "ND_hsv_adjust_color3") {
    auto in_v = RESOLVE("inputs:in");
    if (!in_v) return nonstd::make_unexpected(in_v.error());
    auto hue = RESOLVE("inputs:hue");
    if (!hue) return nonstd::make_unexpected(hue.error());
    auto sat = RESOLVE("inputs:saturation");
    if (!sat) return nonstd::make_unexpected(sat.error());
    auto val = RESOLVE("inputs:value");
    if (!val) return nonstd::make_unexpected(val.error());
    auto fac = RESOLVE("inputs:fac");
    if (!fac) return nonstd::make_unexpected(fac.error());

    float h, s, v_hsv;
    RGBtoHSV(in_v->v[0], in_v->v[1], in_v->v[2], h, s, v_hsv);
    h += hue->as_float();
    if (h > 1.0f) h -= 1.0f;
    if (h < 0.0f) h += 1.0f;
    s *= sat->as_float();
    v_hsv *= val->as_float();

    float or_r, or_g, or_b;
    HSVtoRGB(h, s, v_hsv, or_r, or_g, or_b);

    float f = fac->as_float();
    MtlxConstVal r = MtlxConstVal::Color3(
        in_v->v[0] * (1.0f - f) + or_r * f,
        in_v->v[1] * (1.0f - f) + or_g * f,
        in_v->v[2] * (1.0f - f) + or_b * f);
    r.color_managed = in_v->color_managed;
    return r;
  }

  if (nt == "ND_hsvadjust_color3") {
    auto in_v = RESOLVE("inputs:in");
    if (!in_v) return nonstd::make_unexpected(in_v.error());
    auto amount = RESOLVE("inputs:amount");
    if (!amount) return nonstd::make_unexpected(amount.error());

    float h, s, v_hsv;
    RGBtoHSV(in_v->v[0], in_v->v[1], in_v->v[2], h, s, v_hsv);
    // amount.v[0]=hue shift, amount.v[1]=saturation mult, amount.v[2]=value mult
    h += amount->v[0];
    if (h > 1.0f) h -= 1.0f;
    if (h < 0.0f) h += 1.0f;
    s *= amount->v[1];
    v_hsv *= amount->v[2];

    float or_r, or_g, or_b;
    HSVtoRGB(h, s, v_hsv, or_r, or_g, or_b);
    MtlxConstVal r = MtlxConstVal::Color3(or_r, or_g, or_b);
    r.color_managed = in_v->color_managed;
    return r;
  }

  #undef RESOLVE

  // Structured, actionable diagnostic: name the node type AND the offending node
  // (and its enclosing NodeGraph) so an unsupported node can be located quickly.
  return nonstd::make_unexpected(fmt::format(
      "Unsupported node type for constant evaluation: '{}' (node '{}'{})", nt,
      shader->name.empty() ? "<unnamed>" : shader->name,
      (ng_prim && !ng_prim->element_name().empty())
          ? fmt::format(" in NodeGraph '{}'", ng_prim->element_name())
          : std::string()));
}

static nonstd::expected<std::pair<const Prim *, const Shader *>, std::string>
ResolveNodeGraphTargetImpl(const Stage &stage, const Path &connection_path,
                           std::string *target_out_name, int max_depth) {
  if (max_depth <= 0) {
    return nonstd::make_unexpected("Max NodeGraph output depth exceeded");
  }

  const std::string prim_part = connection_path.prim_part();
  const std::string prop_part = connection_path.prop_part();

  const Prim *ng_prim = FindNodeGraphPrimForPath(stage, nullptr, prim_part);
  if (!ng_prim) {
    std::string err;
    size_t last_slash = prim_part.rfind('/');
    if (last_slash == std::string::npos) {
      return nonstd::make_unexpected("Invalid path");
    }
    std::string parent_path = prim_part.substr(0, last_slash);
    std::string ng_name = prim_part.substr(last_slash + 1);
    const Prim *parent_prim{nullptr};
    if (stage.find_prim_at_path(Path(parent_path, ""), parent_prim, &err) &&
        parent_prim) {
      for (const auto &child : parent_prim->children()) {
        if (child.element_name() == ng_name ||
            child.data().type_name() == "NodeGraph") {
          ng_prim = &child;
          break;
        }
      }
    }
    if (!ng_prim) {
      return nonstd::make_unexpected(
          fmt::format("NodeGraph not found at {}", prim_part));
    }
  }

  const NodeGraph *ng = ng_prim->as<NodeGraph>();
  if (!ng) {
    return nonstd::make_unexpected("Not a NodeGraph");
  }

  std::string output_name = prop_part;
  if (startsWith(output_name, "outputs:")) {
    output_name = output_name.substr(8);
  }

  Path target_path;
  bool found_conn = false;
  for (const auto &suffix : {"outputs:" + output_name + ".connect",
                              "outputs:" + output_name}) {
    auto it = ng->props.find(suffix);
    if (it != ng->props.end()) {
      if (it->second.is_attribute()) {
        const Attribute &attr = it->second.get_attribute();
        if (attr.has_connections() && !attr.connections().empty()) {
          target_path = attr.connections()[0];
          found_conn = true;
          break;
        }
      } else if (it->second.is_relationship()) {
        auto targets = it->second.get_relationTargets();
        if (!targets.empty()) {
          target_path = targets[0];
          found_conn = true;
          break;
        }
      }
    }
  }

  if (!found_conn) {
    return nonstd::make_unexpected("No output connection found");
  }

  if (FindNodeGraphPrimForPath(stage, ng_prim, target_path.prim_part())) {
    return ResolveNodeGraphTargetImpl(stage, target_path, target_out_name,
                                      max_depth - 1);
  }

  const Shader *target_shader =
      FindShaderInNodeGraph(stage, ng_prim, target_path.prim_part());
  if (!target_shader) {
    return nonstd::make_unexpected(
        fmt::format("Shader not found at {}", target_path.prim_part()));
  }

  if (target_out_name) {
    // The node output the NodeGraph output points at (e.g. "outr" on a separate
    // node), so a top-level separate/swizzle returns the right channel.
    std::string on = target_path.prop_part();
    if (on.compare(0, 8, "outputs:") == 0) on = on.substr(8);
    *target_out_name = on;
  }

  return std::make_pair(ng_prim, target_shader);
}

// Resolve a NodeGraph connection path to the NodeGraph prim and target shader.
// Shared logic for both float and color3 evaluation entry points.
static nonstd::expected<std::pair<const Prim *, const Shader *>, std::string>
ResolveNodeGraphTarget(const Stage &stage, const Path &connection_path,
                       std::string *target_out_name) {
  return ResolveNodeGraphTargetImpl(stage, connection_path, target_out_name,
                                    /*max_depth=*/kMtlxGraphEvaluationDepth);
}

// Evaluate a MaterialX NodeGraph connection as a constant value (float or color3).

}  // namespace

nonstd::expected<MtlxConstVal, std::string> EvaluateMtlxNodeGraphAsConstant(
    const Stage &stage, const Path &connection_path,
    const std::string &evaluation_space) {
  std::string target_out_name;
  auto target =
      ResolveNodeGraphTarget(stage, connection_path, &target_out_name);
  if (!target) return nonstd::make_unexpected(target.error());
  return EvaluateMtlxConstant(stage, target->first, target->second,
                              /*max_depth=*/kMtlxGraphEvaluationDepth,
                              evaluation_space,
                              target_out_name);
}

nonstd::expected<MtlxNodeGraphInfo, std::string> ExtractMtlxNodeGraphInfo(
    const Stage &stage,
    const Prim *material_prim,
    const std::vector<Path> &connections,
    std::string *err) {

  MtlxNodeGraphInfo info;

  if (connections.empty()) {
    return info;  // No connections, return default
  }

  // Follow the first connection (we only support single connection)
  Path current_path = connections[0];

  // Maximum depth to prevent infinite loops
  int max_depth = 15;

  while (max_depth-- > 0) {
    std::string current_prim_part = current_path.prim_part();
    std::string current_prop_part = current_path.prop_part();
    if (err) {
      *err += "DEBUG: current_prim_part=" + current_prim_part + ", prop_part=" + current_prop_part + "\n";
    }

    const Prim *current_prim{nullptr};

    // Try to find the prim in the stage
    std::string lookup_err;
    bool found = stage.find_prim_at_path(Path(current_prim_part, ""), current_prim, &lookup_err);

    // If not found in stage, try looking in material's children (NodeGraph case)
    if (!found || !current_prim) {
      if (err) {
        *err += "DEBUG: Not found in stage, looking in material children. material_prim=" + std::string(material_prim ? "valid" : "null") + "\n";
      }
      if (material_prim) {
        if (err) {
          *err += "DEBUG: material_prim has " + std::to_string(material_prim->children().size()) + " children\n";
        }
        // Look for NodeGraph child
        for (const auto& child : material_prim->children()) {
          if (err) {
            *err += "DEBUG: Checking child: '" + child.element_name() + "' type='" + child.type_name() + "' is_nodegraph=" + (child.as<NodeGraph>() ? "true" : "false") + "\n";
          }
          // Try to match by type if the path contains "NodeGraph" and this child is a NodeGraph
          if (current_prim_part.find("NodeGraph") != std::string::npos && child.as<NodeGraph>()) {
            if (err) {
              *err += "DEBUG: Found NodeGraph by type matching\n";
            }
            const NodeGraph* ng = child.as<NodeGraph>();

            // Extract target name from path
            size_t last_slash = current_prim_part.rfind('/');
            std::string target_name = (last_slash != std::string::npos)
                ? current_prim_part.substr(last_slash + 1)
                : current_prim_part;

            // If target_name matches the NodeGraph name OR is "NodeGraph" itself
            if (target_name == child.element_name() || target_name == "NodeGraph") {
              // The path points to the NodeGraph itself
              // Set current_prim to the NodeGraph's prim so we can handle it below
              if (err) {
                *err += "DEBUG: Path points to NodeGraph itself, setting current_prim\n";
              }
              current_prim = &child;
              break;
            }

            // Found NodeGraph, look for the target node in NodeGraph children
            if (err) {
              *err += "DEBUG: Looking for '" + target_name + "' in NodeGraph with " + std::to_string(child.children().size()) + " children\n";
            }
            for (const auto& ng_child : child.children()) {
              if (err) {
                *err += "DEBUG: NodeGraph child: '" + ng_child.element_name() + "'\n";
              }
              if (ng_child.element_name() == target_name) {
                if (err) {
                  *err += "DEBUG: Found target in NodeGraph children\n";
                }
                current_prim = &ng_child;
                break;
              }
            }
            (void)ng;  // suppress unused variable warning
            if (current_prim) break;
          }
        }
      }
    }

    if (!current_prim) {
      // Can't find the prim, stop traversal
      break;
    }

    // Check if it's a Shader
    const Shader *shader = current_prim->as<Shader>();
    if (err) {
      *err += "DEBUG: Checking prim type: type_name='" + current_prim->type_name() + "' is_shader=" + (shader ? "true" : "false") + "\n";
    }
    if (!shader) {
      // Not a shader, might be a NodeGraph - try to follow its output
      if (const NodeGraph *ng = current_prim->as<NodeGraph>()) {
        // Get the output property specified in the connection
        std::string prop_part = current_path.prop_part();
        if (err) {
          std::string props_list;
          for (const auto& kv : ng->props) {
            props_list += " '" + kv.first + "'";
          }
          *err += "DEBUG NodeGraph props:" + props_list + ", looking for: '" + prop_part + "'\n";
        }
        auto it = ng->props.find(prop_part);
        if (it != ng->props.end() && it->second.is_attribute()) {
          const Attribute &attr = it->second.get_attribute();
          if (attr.has_connections()) {
            const auto &conns = attr.connections();
            if (!conns.empty()) {
              if (err) {
                *err += "DEBUG: Found property, following connection to: " + conns[0].full_path_name() + "\n";
              }
              current_path = conns[0];
              continue;
            }
          }
        } else {
          if (err) {
            *err += "DEBUG: Property '" + prop_part + "' not found in ng->props\n";
          }
        }
      }
      break;
    }

    // Check for specific MaterialX node types
    const std::string &node_type = shader->info_id;
    if (err) {
      *err += "DEBUG: Shader info_id='" + node_type + "'\n";
    }

    // For generic shaders (MaterialX nodes), properties are stored in ShaderNode inside shader->value
    // We need to get the correct props map
    const std::map<std::string, Property> *shader_props = &shader->props;
    if (const ShaderNode *shader_node = shader->value.as<ShaderNode>()) {
      shader_props = &shader_node->props;
      DCOUT("Using ShaderNode props (size=" << shader_props->size() << ")");
    }

    // NOTE: this ~24-branch else-if chain on node_type was converted to
    // standalone ifs -- same MSVC C1061 ("blocks nested too deeply") risk
    // class already hit once and fixed in crate-writer-values.cc/
    // stage-converter.cc, where an else-if chain nests one level deeper per
    // link while a flat sequence of standalone ifs does not. A `matched`
    // guard is required (unlike some sibling fixes where it was optional):
    // the ND_rotate3d_vector3 branch below can fall through without a
    // break/continue when its `inputs:in` has no connection, and several
    // node_type conditions here structurally overlap (e.g. the literal
    // "ND_normalize_vector3" also satisfies the later "ND_normalize_"
    // prefix check) -- in the original chain, order + the fact that every
    // *other* branch always exits via break/continue made this safe; the
    // `matched` guard reproduces that same "first match wins, and nothing
    // after it runs" semantics explicitly and correctly regardless of
    // control flow within any one branch.
    bool matched = false;

    if (node_type == "ND_normalmap_float" || node_type == "ND_normalmap") {
      matched = true;
      info.has_normal_map = true;
      DCOUT("Found ND_normalmap shader, props=" << shader_props->size());

      // Extract scale input
      auto scale_it = shader_props->find("inputs:scale");
      if (scale_it != shader_props->end() && scale_it->second.is_attribute()) {
        const Attribute &scale_attr = scale_it->second.get_attribute();
        if (auto scale_val = scale_attr.get_value<float>()) {
          info.normal_map_scale = scale_val.value();
        }
      }

      // Follow inputs:in to find the texture
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          DCOUT("Following inputs:in connection to: " << current_path.full_path_name());
          continue;
        }
      }
      DCOUT("No inputs:in connection, breaking");
      break;  // No more connections to follow
    }
    if (node_type == "ND_rotate3d_vector3") {
      matched = true;
      info.has_tangent_rotation = true;

      // Extract amount input (rotation angle in degrees)
      auto amount_it = shader_props->find("inputs:amount");
      if (amount_it != shader_props->end() && amount_it->second.is_attribute()) {
        const Attribute &amount_attr = amount_it->second.get_attribute();
        if (auto amount_val = amount_attr.get_value<float>()) {
          info.tangent_rotation = amount_val.value();
        }
      }

      // Follow inputs:in to continue traversal
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
    }
    if (node_type == "ND_image_vector3" || node_type == "ND_image_vector4" ||
        node_type == "ND_image_color3" || node_type == "ND_image_color4") {
      matched = true;
      // Found the texture node - extract file path
      DCOUT("Found ND_image node, props=" << shader_props->size());
      auto file_it = shader_props->find("inputs:file");
      if (file_it != shader_props->end() && file_it->second.is_attribute()) {
        const Attribute &file_attr = file_it->second.get_attribute();
        if (auto asset_val = file_attr.get_value<value::AssetPath>()) {
          info.normal_map_texture = asset_val.value().GetAssetPath();
          DCOUT("Found normal_map_texture: " << info.normal_map_texture);
        }
      }
      Path texcoord_path;
      if (MtlxInputConnection(*shader_props, "inputs:texcoord",
                              &texcoord_path)) {
        ExtractMtlxTexcoordInfoFromConnection(stage, material_prim,
                                              texcoord_path, &info);
      }
      break;  // End of chain
    }
    if (node_type == "ND_normalize_vector3" ||
        node_type == "ND_convert_vector4_vector3" ||
        node_type == "ND_convert_color4_vector3") {
      matched = true;
      // Conversion/normalization nodes - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    if (node_type.find("ND_geompropvalue_") == 0) {
      matched = true;
      // GeomPropValue node - reads an arbitrary primvar from mesh geometry.
      // Extract the primvar name from inputs:geomprop.
      auto geom_it = shader_props->find("inputs:geomprop");
      if (geom_it != shader_props->end() && geom_it->second.is_attribute()) {
        const Attribute &geom_attr = geom_it->second.get_attribute();
        if (geom_attr.has_value()) {
          auto geom_val = geom_attr.get_value<std::string>();
          if (geom_val) {
            info.geomprop_name = *geom_val;
            info.has_geomprop = true;
          }
        }
      }
      break;  // Terminal node
    }
    if (node_type == "ND_tangent_vector3" || node_type == "ND_normal_vector3" ||
        node_type == "ND_position_vector3" || node_type == "ND_geomcolor_color3" ||
        node_type == "ND_geomcolor_color4" || node_type == "ND_bitangent_vector3" ||
        node_type == "ND_viewdirection_vector3") {
      matched = true;
      // Geometry nodes - end of chain (no input connections)
      break;
    }
    if (node_type == "ND_texcoord_vector2" || node_type == "ND_texcoord_vector3") {
      matched = true;
      // Texcoord node - extract inputs:index for UV set selection
      auto idx_it = shader_props->find("inputs:index");
      if (idx_it != shader_props->end() && idx_it->second.is_attribute()) {
        const Attribute &idx_attr = idx_it->second.get_attribute();
        if (idx_attr.has_value()) {
          auto idx_val = idx_attr.get_value<int>();
          if (idx_val) {
            info.texcoord_index = *idx_val;
          }
        }
      }
      break;
    }
    //
    // Unary operations (single input: inputs:in)
    // These nodes process a single input value
    //
    if (node_type.find("ND_sqrt_") == 0 ||
               node_type.find("ND_absval_") == 0 ||
               node_type.find("ND_sign_") == 0 ||
               node_type.find("ND_floor_") == 0 ||
               node_type.find("ND_ceil_") == 0 ||
               node_type.find("ND_round_") == 0 ||
               node_type.find("ND_sin_") == 0 ||
               node_type.find("ND_cos_") == 0 ||
               node_type.find("ND_tan_") == 0 ||
               node_type.find("ND_asin_") == 0 ||
               node_type.find("ND_acos_") == 0 ||
               node_type.find("ND_atan_") == 0 ||
               node_type.find("ND_exp_") == 0 ||
               node_type.find("ND_ln_") == 0 ||
               node_type.find("ND_log2_") == 0 ||
               node_type.find("ND_magnitude_") == 0 ||
               node_type.find("ND_luminance_") == 0 ||
               node_type.find("ND_normalize_") == 0 ||
               node_type.find("ND_invert_") == 0 ||
               node_type.find("ND_saturate_") == 0 ||
               node_type.find("ND_hueshift_") == 0) {
      matched = true;
      // Unary operations - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Binary operations (two inputs: inputs:in1, inputs:in2)
    // Typically follow in1 for texture chains
    //
    if (node_type.find("ND_add_") == 0 ||
               node_type.find("ND_subtract_") == 0 ||
               node_type.find("ND_multiply_") == 0 ||
               node_type.find("ND_divide_") == 0 ||
               node_type.find("ND_power_") == 0 ||
               node_type.find("ND_min_") == 0 ||
               node_type.find("ND_max_") == 0 ||
               node_type.find("ND_modulo_") == 0 ||
               node_type.find("ND_atan2_") == 0 ||
               node_type.find("ND_dotproduct_") == 0 ||
               node_type.find("ND_crossproduct_") == 0) {
      matched = true;
      // Binary operations - prefer following the input with a connection (likely leads to texture).
      // Try in1 first, fall back to in2. If neither has connections, break normally.
      auto in1_it = shader_props->find("inputs:in1");
      if (in1_it != shader_props->end() && in1_it->second.is_attribute()) {
        const Attribute &in1_attr = in1_it->second.get_attribute();
        if (in1_attr.has_connections()) {
          current_path = in1_attr.connections()[0];
          continue;
        }
      }
      // If in1 has no connection, try in2
      auto in2_it = shader_props->find("inputs:in2");
      if (in2_it != shader_props->end() && in2_it->second.is_attribute()) {
        const Attribute &in2_attr = in2_it->second.get_attribute();
        if (in2_attr.has_connections()) {
          current_path = in2_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Mix/blend operations (fg, bg, mix inputs)
    // Follow fg (foreground) as it typically has the texture
    //
    if (node_type.find("ND_mix_") == 0) {
      matched = true;
      // Mix nodes - follow inputs:fg (foreground typically has the texture)
      auto fg_it = shader_props->find("inputs:fg");
      if (fg_it != shader_props->end() && fg_it->second.is_attribute()) {
        const Attribute &fg_attr = fg_it->second.get_attribute();
        if (fg_attr.has_connections()) {
          current_path = fg_attr.connections()[0];
          continue;
        }
      }
      // If fg has no connection, try bg
      auto bg_it = shader_props->find("inputs:bg");
      if (bg_it != shader_props->end() && bg_it->second.is_attribute()) {
        const Attribute &bg_attr = bg_it->second.get_attribute();
        if (bg_attr.has_connections()) {
          current_path = bg_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Clamp operation (in, low, high inputs)
    //
    if (node_type.find("ND_clamp_") == 0) {
      matched = true;
      // Clamp nodes - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Remap/range operations (in, inlow, inhigh, outlow, outhigh inputs)
    //
    if (node_type.find("ND_remap_") == 0 ||
        node_type.find("ND_range_") == 0) {
      matched = true;
      // Remap/range nodes - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Extract operations (extracts component from color3/vector3)
    //
    if (node_type.find("ND_extract_") == 0) {
      matched = true;
      // Extract nodes - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // HSV/color adjustment operations
    //
    if (node_type.find("ND_hsvadjust_") == 0 ||
        node_type.find("ND_rgbtohsv_") == 0 ||
        node_type.find("ND_hsvtorgb_") == 0) {
      matched = true;
      // HSV adjust - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Type conversion operations (convert color3 to vector3 etc.)
    //
    if (node_type.find("ND_convert_") == 0) {
      matched = true;
      // Conversion nodes - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Combine operations (combines in1, in2, in3 to color3/vector3)
    // Terminal nodes - they produce values from scalars
    //
    if (node_type.find("ND_separate3_") == 0 ||
        node_type.find("ND_separate2_") == 0 ||
        node_type.find("ND_separate4_") == 0) {
      matched = true;
      // Separate (multi-output) nodes - split vector into components.
      // Outputs: outr, outg, outb (for separate3), outx, outy (for separate2), etc.
      // Follow inputs:in to continue traversal.
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    if (node_type.find("ND_combine3_") == 0 ||
        node_type.find("ND_combine2_") == 0 ||
        node_type.find("ND_combine4_") == 0) {
      matched = true;
      // Combine nodes - terminal (produce vector from components)
      // Could follow inputs if needed, but typically these are terminals
      break;
    }
    //
    // Constant nodes - terminal (provide constant values)
    //
    if (node_type.find("ND_constant_") == 0) {
      matched = true;
      // Constant nodes - terminal, extract the constant value
      auto val_it = shader_props->find("inputs:value");
      if (val_it != shader_props->end() && val_it->second.is_attribute()) {
        const Attribute &val_attr = val_it->second.get_attribute();
        std::array<float, 4> cv = {{0.0f, 0.0f, 0.0f, 0.0f}};
        if (auto vf = val_attr.get_value<float>()) {
          cv[0] = *vf;
          info.constant_components = 1;
          info.has_constant = true;
        } else if (auto vf3 = val_attr.get_value<value::float3>()) {
          cv[0] = (*vf3)[0]; cv[1] = (*vf3)[1]; cv[2] = (*vf3)[2];
          info.constant_components = 3;
          info.has_constant = true;
        } else if (auto vc3 = val_attr.get_value<value::color3f>()) {
          cv[0] = (*vc3)[0]; cv[1] = (*vc3)[1]; cv[2] = (*vc3)[2];
          info.constant_components = 3;
          info.has_constant = true;
        } else if (auto vf2 = val_attr.get_value<value::float2>()) {
          cv[0] = (*vf2)[0]; cv[1] = (*vf2)[1];
          info.constant_components = 2;
          info.has_constant = true;
        } else if (auto vc4 = val_attr.get_value<value::color4f>()) {
          cv[0] = (*vc4)[0]; cv[1] = (*vc4)[1]; cv[2] = (*vc4)[2]; cv[3] = (*vc4)[3];
          info.constant_components = 4;
          info.has_constant = true;
        } else if (auto vf4 = val_attr.get_value<value::float4>()) {
          cv[0] = (*vf4)[0]; cv[1] = (*vf4)[1]; cv[2] = (*vf4)[2]; cv[3] = (*vf4)[3];
          info.constant_components = 4;
          info.has_constant = true;
        }
        if (info.has_constant) {
          info.constant_value = cv;
        }
      }
      break;
    }
    //
    // Tiledimage/image nodes (texture sampling)
    //
    if (node_type.find("ND_tiledimage_") == 0) {
      matched = true;
      // Tiled image node - extract file path, uvtiling, uvoffset
      auto file_it = shader_props->find("inputs:file");
      if (file_it != shader_props->end() && file_it->second.is_attribute()) {
        const Attribute &file_attr = file_it->second.get_attribute();
        if (auto asset_val = file_attr.get_value<value::AssetPath>()) {
          info.normal_map_texture = asset_val.value().GetAssetPath();
        }
      }
      // Extract UV tiling (scale)
      auto tiling_it = shader_props->find("inputs:uvtiling");
      if (tiling_it != shader_props->end() && tiling_it->second.is_attribute()) {
        const Attribute &tiling_attr = tiling_it->second.get_attribute();
        if (auto tiling_val = tiling_attr.get_value<value::float2>()) {
          info.uvtiling[0] = (*tiling_val)[0];
          info.uvtiling[1] = (*tiling_val)[1];
          info.has_uvtransform = true;
        }
      }
      // Extract UV offset
      auto offset_it = shader_props->find("inputs:uvoffset");
      if (offset_it != shader_props->end() && offset_it->second.is_attribute()) {
        const Attribute &offset_attr = offset_it->second.get_attribute();
        if (auto offset_val = offset_attr.get_value<value::float2>()) {
          info.uvoffset[0] = (*offset_val)[0];
          info.uvoffset[1] = (*offset_val)[1];
          info.has_uvtransform = true;
        }
      }
      Path texcoord_path;
      if (MtlxInputConnection(*shader_props, "inputs:texcoord",
                              &texcoord_path)) {
        ExtractMtlxTexcoordInfoFromConnection(stage, material_prim,
                                              texcoord_path, &info);
      }
      break;  // End of chain
    }
    //
    // Swizzle operations
    //
    if (node_type.find("ND_swizzle_") == 0) {
      matched = true;
      // Swizzle - follow inputs:in
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Ifgreater/ifless/ifequal conditional operations
    //
    if (node_type.find("ND_ifgreater_") == 0 ||
        node_type.find("ND_ifgreatereq_") == 0 ||
        node_type.find("ND_ifless_") == 0 ||
        node_type.find("ND_iflesseq_") == 0 ||
        node_type.find("ND_ifequal_") == 0) {
      matched = true;
      // Conditional - follow inputs:in1 (value1)
      auto in1_it = shader_props->find("inputs:in1");
      if (in1_it != shader_props->end() && in1_it->second.is_attribute()) {
        const Attribute &in1_attr = in1_it->second.get_attribute();
        if (in1_attr.has_connections()) {
          current_path = in1_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    //
    // Noise operations
    //
    if (node_type.find("ND_noise2d_") == 0 ||
        node_type.find("ND_noise3d_") == 0 ||
        node_type.find("ND_cellnoise2d_") == 0 ||
        node_type.find("ND_cellnoise3d_") == 0 ||
        node_type.find("ND_worleynoise2d_") == 0 ||
        node_type.find("ND_worleynoise3d_") == 0 ||
        node_type.find("ND_fractal3d_") == 0) {
      matched = true;
      // Noise nodes - terminal (procedural generation)
      break;
    }
    //
    // Place2d texture coordinate transformation
    //
    if (node_type.find("ND_place2d_") == 0) {
      matched = true;
      // Place2d - follow inputs:texcoord if connected
      auto texcoord_it = shader_props->find("inputs:texcoord");
      if (texcoord_it != shader_props->end() && texcoord_it->second.is_attribute()) {
        const Attribute &texcoord_attr = texcoord_it->second.get_attribute();
        if (texcoord_attr.has_connections()) {
          current_path = texcoord_attr.connections()[0];
          continue;
        }
      }
      break;
    }
    if (!matched) {
      // Unknown node type, try to follow inputs:in if it exists
      auto in_it = shader_props->find("inputs:in");
      if (in_it != shader_props->end() && in_it->second.is_attribute()) {
        const Attribute &in_attr = in_it->second.get_attribute();
        if (in_attr.has_connections()) {
          current_path = in_attr.connections()[0];
          continue;
        }
      }
      // Also try inputs:in1 for binary-style nodes
      auto in1_it = shader_props->find("inputs:in1");
      if (in1_it != shader_props->end() && in1_it->second.is_attribute()) {
        const Attribute &in1_attr = in1_it->second.get_attribute();
        if (in1_attr.has_connections()) {
          current_path = in1_attr.connections()[0];
          continue;
        }
      }
      break;
    }
  }

  return info;
}



}  // namespace tydra
}  // namespace lightusd
