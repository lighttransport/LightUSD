// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage-converter: Shader/Material property extraction.
//

#include "sconv-detail.hh"
#include "usdShade.hh"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {
namespace experimental {

// ============================================================================
// Material Property Extraction
// ============================================================================

bool CrateWriter::ExtractMaterialProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const Material* material = prim.data().as<Material>();
  if (!material) {
    if (err) *err = "Failed to cast prim to Material";
    return false;
  }

  // Material is imageable: `purpose` is a typed field (UsdShadePrim::purpose),
  // so it has to be written explicitly or it vanishes on write.
  if (material->purpose.authored()) {
    const Purpose purpose_val = material->purpose.get_value();
    if (purpose_val != Purpose::Default) {
      crate::CrateValue purpose_crate_val;
      purpose_crate_val.Set(value::token(to_string(purpose_val)));
      fields.push_back({"purpose", purpose_crate_val});
    }
  }


  // Material outputs (surface, displacement, volume) are handled separately
  // by AddMaterialOutputSpecs() which is called AFTER the Material prim spec is added
  // This ensures correct ordering in the spec list

  // MaterialXConfigAPI properties
  if (material->materialXConfig) {
    const auto &cfg = *material->materialXConfig;
    if (cfg.mtlx_version.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.mtlx_version.get_value());
      fields.push_back({"config:mtlx:version", cv});
    }
    if (cfg.mtlx_namespace.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.mtlx_namespace.get_value());
      fields.push_back({"config:mtlx:namespace", cv});
    }
    if (cfg.mtlx_colorspace.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.mtlx_colorspace.get_value());
      fields.push_back({"config:mtlx:colorspace", cv});
    }
    if (cfg.mtlx_sourceUri.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.mtlx_sourceUri.get_value());
      fields.push_back({"config:mtlx:sourceUri", cv});
    }
  }

  if (material->materialUEConfig) {
    const auto &cfg = *material->materialUEConfig;
    auto add_string = [&fields](const char *name, const std::string &value,
                                bool authored) {
      if (!authored) return;
      crate::CrateValue cv;
      cv.Set(value);
      fields.push_back({name, cv});
    };
    add_string("config:unreal:version", cfg.version.get_value(), cfg.version.authored());
    add_string("config:unreal:engineVersion", cfg.engineVersion.get_value(), cfg.engineVersion.authored());
    add_string("config:unreal:sourceAsset", cfg.sourceAsset.get_value(), cfg.sourceAsset.authored());
    add_string("config:unreal:graphAuthority", cfg.graphAuthority.get_value(), cfg.graphAuthority.authored());
    add_string("config:unreal:materialDomain", cfg.materialDomain.get_value(), cfg.materialDomain.authored());
    add_string("config:unreal:blendMode", cfg.blendMode.get_value(), cfg.blendMode.authored());
    add_string("config:unreal:shadingModels", cfg.shadingModels.get_value(), cfg.shadingModels.authored());
    if (cfg.twoSided.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.twoSided.get_value());
      fields.push_back({"config:unreal:twoSided", cv});
    }
    if (cfg.opacityMaskClipValue.authored()) {
      crate::CrateValue cv;
      cv.Set(cfg.opacityMaskClipValue.get_value());
      fields.push_back({"config:unreal:opacityMaskClipValue", cv});
    }
    add_string("config:unreal:propertyArchive", cfg.propertyArchive.get_value(), cfg.propertyArchive.authored());
  }

  return true;
}

// ============================================================================
// Material Output Specs (called AFTER Material prim spec is added)
// ============================================================================

bool CrateWriter::AddMaterialOutputSpecs(
  const Material* material,
  const Path& prim_path,
  std::string* err
) {

  // A Material terminal is a TypedConnection: `authored()` means the output was
  // declared, and its "value" IS its connection (has_value() is literally
  // _targetPaths.size()). A terminal can legally be DECLARED but left
  // unconnected --
  //
  //   def Material "mymat" { token outputs:surface }
  //
  // -- which set_empty() records as authored-with-no-targets. Gating the spec on
  // has_value()/non-empty connections therefore dropped that declaration on write
  // entirely. Emit the spec whenever the terminal is authored, and attach
  // connectionPaths only when there is something to connect.
  auto add_terminal = [&](const std::string& output_name,
                          const TypedConnection<value::token>& terminal) -> bool {
    if (!terminal.authored()) {
      return true;
    }

    Path output_path = prim_path.AppendProperty(output_name);
    crate::FieldValuePairVector output_fields;

    crate::CrateValue type_value;
    value::token type_tok("token");
    type_value.Set(type_tok);
    output_fields.push_back({"typeName", type_value});

    // "connectionPaths" for Attribute connections ("targetPaths" is the
    // Relationship spelling).
    const std::vector<Path>& connections = terminal.get_connections();
    if (!connections.empty()) {
      ListOp<Path> connection_paths_listop;
      connection_paths_listop.ClearAndMakeExplicit();
      connection_paths_listop.SetExplicitItems(connections);

      crate::CrateValue conn_value;
      conn_value.Set(connection_paths_listop);
      output_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(output_path, SpecType::Attribute, output_fields, err);
  };

  if (!add_terminal("outputs:surface", material->surface)) return false;
  if (!add_terminal("outputs:displacement", material->displacement)) return false;
  if (!add_terminal("outputs:volume", material->volume)) return false;

  return true;
}

// ============================================================================
// UsdPreviewSurface Shader Input Specs (called AFTER Shader prim spec is added)
// ============================================================================

bool CrateWriter::AddUsdPreviewSurfaceInputSpecs(
  const UsdPreviewSurface* preview_surface,
  const Path& prim_path,
  std::string* err
) {

  // Helper lambda to add an input attribute spec
  auto add_input_spec = [&](const std::string& input_name, const std::string& type_name, const crate::CrateValue& value, const std::vector<Path>* connections = nullptr) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    // Add typeName
    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});

    // Add default value
    input_fields.push_back({"default", value});

    // USD permits an attribute to author BOTH a value and a connection; emit
    // connectionPaths alongside the default so value+connect coexistence
    // survives the crate write (matches usdcat). See add_input_connection_spec
    // for the connection-only path.
    if (connections && !connections->empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(*connections);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper: emit a typed-shader-input spec for an attribute that holds a
  // connection (no concrete value). Without this, typed UsdPreviewSurface
  // inputs that are wired to upstream shader outputs (e.g.
  // `inputs:diffuseColor.connect = </tex.outputs:rgb>`) drop their connection
  // during USDC writing because the regular add_input_spec path requires a
  // scalar value. Emits typeName + connectionPaths (matching what
  // ConvertAttributeToFields does for generic attribute connections).
  auto add_input_connection_spec = [&](const std::string& input_name,
                                        const std::string& type_name,
                                        const std::vector<Path>& conn_paths) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});

    // Empty => a DECLARED-but-unconnected, value-less input (`token inputs:wrapS`
    // on its own). It still has to be written, as typeName alone: dropping it
    // loses the declaration, and writing a `default` would invent the schema
    // fallback as an authored value -- the bug this whole change is about.
    if (!conn_paths.empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(conn_paths);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper to convert OpacityMode enum to token string
  auto opacity_mode_to_string = [](UsdPreviewSurface::OpacityMode mode) -> std::string {
    switch (mode) {
      case UsdPreviewSurface::OpacityMode::Opacity: return "opacity";
      case UsdPreviewSurface::OpacityMode::Transparent: return "transparent";
      case UsdPreviewSurface::OpacityMode::Presence: return "presence";
    }
    return "transparent"; // fallback
  };

  // Helper to handle timesampled float shader inputs
  auto add_input_spec_with_timesamples = [&](
      const std::string& input_name,
      const std::string& type_name,
      const crate::CrateValue& default_value,
      const Animatable<float>* animatable,
      const std::vector<Path>* connections = nullptr) -> bool {
    // First add the default value spec (with connectionPaths if also connected)
    if (!add_input_spec(input_name, type_name, default_value, connections)) {
      return false;
    }

    // If there are timesamples, create a separate timeSamples spec
    if (animatable && animatable->has_timesamples()) {
      // Value-type Animatable stores a type-erased value::TimeSamples directly.
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = animatable->get_timesamples_ptr()) {
        ts = *_tsp;
      }

      // Create timeSamples spec
      Path ts_path = prim_path.AppendProperty(input_name + ".timeSamples");
      crate::FieldValuePairVector ts_fields;

      crate::CrateValue ts_value;
      ts_value.Set(ts);
      ts_fields.push_back({"default", ts_value});

      if (!AddSpec(ts_path, SpecType::Attribute, ts_fields, err)) {
        return false;
      }
    }

    return true;
  };

  // Helper to handle timesampled color3f shader inputs
  auto add_input_spec_with_timesamples_color3f = [&](
      const std::string& input_name,
      const std::string& type_name,
      const crate::CrateValue& default_value,
      const Animatable<value::color3f>* animatable,
      const std::vector<Path>* connections = nullptr) -> bool {
    // First add the default value spec (with connectionPaths if also connected)
    if (!add_input_spec(input_name, type_name, default_value, connections)) {
      return false;
    }

    // If there are timesamples, create a separate timeSamples spec
    if (animatable && animatable->has_timesamples()) {
      // color3f store -> float3 crate values (crate stores the underlying type).
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = animatable->get_timesamples_ptr()) {
        for (const auto &_s : _tsp->get_samples()) {
          if (_s.blocked) { ts.add_blocked_sample(_s.t, value::Value()); continue; }
          if (const value::color3f *cv = _s.value.as<value::color3f>()) {
            value::float3 color_as_float3 = {cv->r, cv->g, cv->b};
            ts.add_sample(_s.t, value::Value(color_as_float3));
          }
        }
      }

      // Create timeSamples spec
      Path ts_path = prim_path.AppendProperty(input_name + ".timeSamples");
      crate::FieldValuePairVector ts_fields;

      crate::CrateValue ts_value;
      ts_value.Set(ts);
      ts_fields.push_back({"default", ts_value});

      if (!AddSpec(ts_path, SpecType::Attribute, ts_fields, err)) {
        return false;
      }
    }

    return true;
  };

  // Extract and add common PBR inputs

  // inputs:diffuseColor (color3f)
  if (preview_surface->diffuseColor.authored()) {
    const std::vector<Path>* conns = preview_surface->diffuseColor.has_connections()
        ? &preview_surface->diffuseColor.connections() : nullptr;
    value::color3f color;
    if (preview_surface->diffuseColor.has_value() &&
        preview_surface->diffuseColor.get_value().get_scalar(&color)) {
      value::float3 color_as_float3 = {color.r, color.g, color.b};
      crate::CrateValue diffuse_value;
      diffuse_value.Set(color_as_float3);
      if (!add_input_spec_with_timesamples_color3f("inputs:diffuseColor", preview_surface->diffuseColor.has_actual_type() ? preview_surface->diffuseColor.get_actual_type_name() : "color3f", diffuse_value, &preview_surface->diffuseColor.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:diffuseColor", "color3f",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // inputs:emissiveColor (color3f)
  if (preview_surface->emissiveColor.authored()) {
    const std::vector<Path>* conns = preview_surface->emissiveColor.has_connections()
        ? &preview_surface->emissiveColor.connections() : nullptr;
    value::color3f color;
    if (preview_surface->emissiveColor.has_value() &&
        preview_surface->emissiveColor.get_value().get_scalar(&color)) {
      value::float3 color_as_float3 = {color.r, color.g, color.b};
      crate::CrateValue emissive_value;
      emissive_value.Set(color_as_float3);
      if (!add_input_spec_with_timesamples_color3f("inputs:emissiveColor", preview_surface->emissiveColor.has_actual_type() ? preview_surface->emissiveColor.get_actual_type_name() : "color3f", emissive_value, &preview_surface->emissiveColor.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:emissiveColor", "color3f",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // inputs:useSpecularWorkflow (int)
  if (preview_surface->useSpecularWorkflow.authored()) {
    const std::vector<Path>* conns =
        preview_surface->useSpecularWorkflow.has_connections()
            ? &preview_surface->useSpecularWorkflow.connections() : nullptr;
    bool emitted = false;
    if (!preview_surface->useSpecularWorkflow.is_value_empty() &&
        !preview_surface->useSpecularWorkflow.get_value().is_timesamples()) {
      int use_spec;
      if (preview_surface->useSpecularWorkflow.get_value().get_scalar(&use_spec)) {
        crate::CrateValue use_spec_value;
        use_spec_value.Set(use_spec);
        if (!add_input_spec("inputs:useSpecularWorkflow", "int", use_spec_value,
                            conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:useSpecularWorkflow", "int",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // inputs:specularColor (color3f) - for specular workflow
  if (preview_surface->specularColor.authored()) {
    const std::vector<Path>* conns = preview_surface->specularColor.has_connections()
        ? &preview_surface->specularColor.connections() : nullptr;
    value::color3f color;
    if (preview_surface->specularColor.has_value() &&
        preview_surface->specularColor.get_value().get_scalar(&color)) {
      value::float3 color_as_float3 = {color.r, color.g, color.b};
      crate::CrateValue spec_color_value;
      spec_color_value.Set(color_as_float3);
      if (!add_input_spec_with_timesamples_color3f("inputs:specularColor", preview_surface->specularColor.has_actual_type() ? preview_surface->specularColor.get_actual_type_name() : "color3f", spec_color_value, &preview_surface->specularColor.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:specularColor", "color3f",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // Helper: connection-or-scalar dispatch for float inputs.
#define EMIT_FLOAT_INPUT(NAME, MEMBER)                                          \
  if (preview_surface->MEMBER.authored()) {                                     \
    const std::vector<Path>* _conns = preview_surface->MEMBER.has_connections() \
        ? &preview_surface->MEMBER.connections() : nullptr;                     \
    crate::CrateValue v;                                                        \
    float scalar = 0.0f;                                                        \
    if (preview_surface->MEMBER.has_value() &&                                  \
        preview_surface->MEMBER.get_value().get_scalar(&scalar)) {              \
      v.Set(scalar);                                                            \
      if (!add_input_spec_with_timesamples(NAME, "float", v,                    \
              &preview_surface->MEMBER.get_value(), _conns)) return false;      \
    } else if (_conns) {                                                        \
      if (!add_input_connection_spec(NAME, "float", *_conns)) return false;     \
    }                                                                           \
  }

  EMIT_FLOAT_INPUT("inputs:metallic",          metallic)
  EMIT_FLOAT_INPUT("inputs:roughness",         roughness)
  EMIT_FLOAT_INPUT("inputs:clearcoat",         clearcoat)
  EMIT_FLOAT_INPUT("inputs:clearcoatRoughness", clearcoatRoughness)
  EMIT_FLOAT_INPUT("inputs:opacity",           opacity)

  // inputs:opacityMode (token) - Controls transparency behavior (transparent or presence)
  if (preview_surface->opacityMode.authored()) {
    const std::vector<Path>* conns = preview_surface->opacityMode.has_connections()
        ? &preview_surface->opacityMode.connections() : nullptr;
    bool emitted = false;
    if (!preview_surface->opacityMode.is_value_empty() &&
        !preview_surface->opacityMode.get_value().is_timesamples()) {
      UsdPreviewSurface::OpacityMode mode;
      if (preview_surface->opacityMode.get_value().get_scalar(&mode)) {
        crate::CrateValue opacity_mode_value;
        opacity_mode_value.Set(value::token(opacity_mode_to_string(mode)));
        if (!add_input_spec("inputs:opacityMode", "token", opacity_mode_value,
                            conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:opacityMode", "token",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  EMIT_FLOAT_INPUT("inputs:opacityThreshold", opacityThreshold)
  EMIT_FLOAT_INPUT("inputs:ior",              ior)
  EMIT_FLOAT_INPUT("inputs:displacement",     displacement)
  EMIT_FLOAT_INPUT("inputs:occlusion",        occlusion)
#undef EMIT_FLOAT_INPUT

  // inputs:normal (normal3f)
  if (preview_surface->normal.authored()) {
    const std::vector<Path>* conns = preview_surface->normal.has_connections()
        ? &preview_surface->normal.connections() : nullptr;
    value::normal3f normal;
    if (preview_surface->normal.has_value() &&
        !preview_surface->normal.get_value().is_timesamples() &&
        preview_surface->normal.get_value().get_scalar(&normal)) {
      value::float3 normal_as_float3 = {normal.x, normal.y, normal.z};
      crate::CrateValue normal_value;
      normal_value.Set(normal_as_float3);
      if (!add_input_spec("inputs:normal", "normal3f", normal_value, conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:normal", "normal3f",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  return true;
}

// ============================================================================
// MtlxOpenPBRSurface Shader Input Specs (called AFTER Shader prim spec is added)
//
// MaterialX `ND_open_pbr_surface_surfaceshader` is reconstructed into the typed
// MtlxOpenPBRSurface; without this writer its ~50 typed inputs are dropped on
// the stage->USDC write. Mirrors AddUsdPreviewSurfaceInputSpecs, including
// value+connection coexistence.
// ============================================================================
bool CrateWriter::AddMtlxOpenPBRSurfaceInputSpecs(
    const MtlxOpenPBRSurface* surface, const Path& prim_path, std::string* err) {

  auto add_input_spec = [&](const std::string& input_name, const std::string& type_name,
                            const crate::CrateValue& value,
                            const std::vector<Path>* connections) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;
    crate::CrateValue type_value; value::token type_tok(type_name); type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});
    input_fields.push_back({"default", value});
    if (connections && !connections->empty()) {
      ListOp<Path> conn_listop; conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(*connections);
      crate::CrateValue conn_value; conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }
    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };
  auto add_input_connection_spec = [&](const std::string& input_name,
                                       const std::string& type_name,
                                       const std::vector<Path>& conn_paths) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;
    crate::CrateValue type_value; value::token type_tok(type_name); type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});
    ListOp<Path> conn_listop; conn_listop.ClearAndMakeExplicit();
    conn_listop.SetExplicitItems(conn_paths);
    crate::CrateValue conn_value; conn_value.Set(conn_listop);
    input_fields.push_back({"connectionPaths", conn_value});
    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Bare declaration (authored but no value and no connection, e.g.
  // `float inputs:coat_darkening`) — MaterialX authors many OpenPBR inputs this
  // way; emit just the typeName so the declaration survives.
  auto add_input_decl = [&](const std::string& input_name, const std::string& type_name) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;
    crate::CrateValue type_value; value::token type_tok(type_name); type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});
    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // float / color3f / bool inputs use TypedAttributeWithFallback<Animatable<T>>.
#define MTLX_CONNS(MEMBER) (surface->MEMBER.has_connections() ? &surface->MEMBER.connections() : nullptr)
#define EMIT_F(NAME, MEMBER) \
  if (surface->MEMBER.authored()) { \
    const std::vector<Path>* _c = MTLX_CONNS(MEMBER); float _s = 0.0f; \
    if (surface->MEMBER.has_value() && surface->MEMBER.get_value().get_scalar(&_s)) { \
      crate::CrateValue _v; _v.Set(_s); \
      if (!add_input_spec(NAME, "float", _v, _c)) return false; \
    } else if (_c) { if (!add_input_connection_spec(NAME, "float", *_c)) return false; } \
    else { if (!add_input_decl(NAME, "float")) return false; } \
  }
#define EMIT_C3(NAME, MEMBER) \
  if (surface->MEMBER.authored()) { \
    const std::vector<Path>* _c = MTLX_CONNS(MEMBER); value::color3f _col; \
    if (surface->MEMBER.has_value() && surface->MEMBER.get_value().get_scalar(&_col)) { \
      value::float3 _f3 = {_col.r, _col.g, _col.b}; crate::CrateValue _v; _v.Set(_f3); \
      if (!add_input_spec(NAME, "color3f", _v, _c)) return false; \
    } else if (_c) { if (!add_input_connection_spec(NAME, "color3f", *_c)) return false; } \
    else { if (!add_input_decl(NAME, "color3f")) return false; } \
  }
#define EMIT_B(NAME, MEMBER) \
  if (surface->MEMBER.authored()) { \
    const std::vector<Path>* _c = MTLX_CONNS(MEMBER); bool _b = false; \
    if (surface->MEMBER.has_value() && surface->MEMBER.get_value().get_scalar(&_b)) { \
      crate::CrateValue _v; _v.Set(_b); \
      if (!add_input_spec(NAME, "bool", _v, _c)) return false; \
    } else if (_c) { if (!add_input_connection_spec(NAME, "bool", *_c)) return false; } \
    else { if (!add_input_decl(NAME, "bool")) return false; } \
  }
  // geometry_normal/tangent use the non-fallback TypedAttribute (get_value() -> optional).
#define EMIT_V3(NAME, MEMBER, TYPENAME, VT) \
  if (surface->MEMBER.authored()) { \
    const std::vector<Path>* _c = MTLX_CONNS(MEMBER); auto _ov = surface->MEMBER.get_value(); VT _vv; \
    if (_ov.has_value() && _ov.value().get_scalar(&_vv)) { \
      value::float3 _f3 = {_vv.x, _vv.y, _vv.z}; crate::CrateValue _v; _v.Set(_f3); \
      if (!add_input_spec(NAME, TYPENAME, _v, _c)) return false; \
    } else if (_c) { if (!add_input_connection_spec(NAME, TYPENAME, *_c)) return false; } \
    else { if (!add_input_decl(NAME, TYPENAME)) return false; } \
  }

  EMIT_F("inputs:base_weight", base_weight)
  EMIT_C3("inputs:base_color", base_color)
  EMIT_F("inputs:base_metalness", base_metalness)
  EMIT_F("inputs:base_diffuse_roughness", base_diffuse_roughness)
  EMIT_F("inputs:specular_weight", specular_weight)
  EMIT_C3("inputs:specular_color", specular_color)
  EMIT_F("inputs:specular_roughness", specular_roughness)
  EMIT_F("inputs:specular_ior", specular_ior)
  EMIT_F("inputs:specular_anisotropy", specular_anisotropy)
  EMIT_F("inputs:specular_rotation", specular_rotation)
  EMIT_F("inputs:specular_roughness_anisotropy", specular_roughness_anisotropy)
  EMIT_F("inputs:transmission_weight", transmission_weight)
  EMIT_C3("inputs:transmission_color", transmission_color)
  EMIT_F("inputs:transmission_depth", transmission_depth)
  EMIT_C3("inputs:transmission_scatter", transmission_scatter)
  EMIT_F("inputs:transmission_scatter_anisotropy", transmission_scatter_anisotropy)
  EMIT_F("inputs:transmission_dispersion", transmission_dispersion)
  EMIT_F("inputs:transmission_dispersion_abbe_number", transmission_dispersion_abbe_number)
  EMIT_F("inputs:transmission_dispersion_scale", transmission_dispersion_scale)
  EMIT_F("inputs:subsurface_weight", subsurface_weight)
  EMIT_C3("inputs:subsurface_color", subsurface_color)
  EMIT_F("inputs:subsurface_radius", subsurface_radius)
  EMIT_C3("inputs:subsurface_radius_scale", subsurface_radius_scale)
  EMIT_F("inputs:subsurface_scale", subsurface_scale)
  EMIT_F("inputs:subsurface_anisotropy", subsurface_anisotropy)
  EMIT_F("inputs:subsurface_scatter_anisotropy", subsurface_scatter_anisotropy)
  EMIT_F("inputs:coat_weight", coat_weight)
  EMIT_C3("inputs:coat_color", coat_color)
  EMIT_F("inputs:coat_roughness", coat_roughness)
  EMIT_F("inputs:coat_anisotropy", coat_anisotropy)
  EMIT_F("inputs:coat_rotation", coat_rotation)
  EMIT_F("inputs:coat_roughness_anisotropy", coat_roughness_anisotropy)
  EMIT_F("inputs:coat_ior", coat_ior)
  EMIT_F("inputs:coat_darkening", coat_darkening)
  EMIT_F("inputs:coat_affect_color", coat_affect_color)
  EMIT_F("inputs:coat_affect_roughness", coat_affect_roughness)
  EMIT_F("inputs:fuzz_weight", fuzz_weight)
  EMIT_C3("inputs:fuzz_color", fuzz_color)
  EMIT_F("inputs:fuzz_roughness", fuzz_roughness)
  EMIT_F("inputs:thin_film_thickness", thin_film_thickness)
  EMIT_F("inputs:thin_film_ior", thin_film_ior)
  EMIT_F("inputs:thin_film_weight", thin_film_weight)
  EMIT_F("inputs:emission_luminance", emission_luminance)
  EMIT_C3("inputs:emission_color", emission_color)
  EMIT_F("inputs:geometry_opacity", geometry_opacity)
  EMIT_B("inputs:geometry_thin_walled", geometry_thin_walled)
  EMIT_V3("inputs:geometry_normal", geometry_normal, "normal3f", value::normal3f)
  EMIT_V3("inputs:geometry_tangent", geometry_tangent, "vector3f", value::vector3f)
  EMIT_V3("inputs:geometry_coat_normal", geometry_coat_normal, "normal3f", value::normal3f)
  EMIT_V3("inputs:geometry_coat_tangent", geometry_coat_tangent, "vector3f", value::vector3f)

#undef EMIT_F
#undef EMIT_C3
#undef EMIT_B
#undef EMIT_V3
#undef MTLX_CONNS

  // Terminal output (token outputs:surface) — no value, just the declaration.
  if (surface->surface.authored()) {
    Attribute a; a.set_type_name("token");
    ConvertAttributeToFields("outputs:surface", a, prim_path, false, err);
  }

  return true;
}

// ============================================================================
// UsdUVTexture Shader Input Specs (called AFTER Shader prim spec is added)
// ============================================================================

bool CrateWriter::AddUsdUVTextureInputSpecs(
  const UsdUVTexture* uv_texture,
  const Path& prim_path,
  std::string* err
) {

  // Helper lambda to add an input spec as a separate attribute
  auto add_input_spec = [&](const std::string& input_name, const std::string& type_name, const crate::CrateValue& value, const std::vector<Path>* connections = nullptr, const AttrMeta* attr_metas = nullptr) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    // typeName field
    crate::CrateValue typename_value;
    value::token typename_tok(type_name);
    typename_value.Set(typename_tok);
    input_fields.push_back({"typeName", typename_value});

    // The input's OWN metadata block, e.g.
    //   asset inputs:file = @f.png@ ( colorSpace = "Raw" )
    // The shader writer builds its specs by hand and emitted no attribute
    // metadata at all, so colorSpace was dropped on write.
    if (attr_metas && attr_metas->has_colorSpace()) {
      crate::CrateValue cs_value;
      cs_value.Set(attr_metas->get_colorSpace());
      input_fields.push_back({"colorSpace", cs_value});
    }

    // default field (the value)
    input_fields.push_back({"default", value});

    // USD permits an attribute to author BOTH a value and a connection; emit
    // connectionPaths alongside the default so value+connect coexistence
    // survives the crate write (matches usdcat).
    if (connections && !connections->empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(*connections);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper: emit a connection-only attribute spec (typeName + connectionPaths).
  // Mirrors AddUsdPreviewSurfaceInputSpecs::add_input_connection_spec — see
  // that function for rationale.
  auto add_input_connection_spec = [&](const std::string& input_name,
                                        const std::string& type_name,
                                        const std::vector<Path>& conn_paths) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;
    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});

    ListOp<Path> conn_listop;
    conn_listop.ClearAndMakeExplicit();
    conn_listop.SetExplicitItems(conn_paths);
    crate::CrateValue conn_value;
    conn_value.Set(conn_listop);
    input_fields.push_back({"connectionPaths", conn_value});
    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper to convert Wrap enum to token string
  auto wrap_to_string = [](UsdUVTexture::Wrap wrap) -> std::string {
    switch (wrap) {
      case UsdUVTexture::Wrap::UseMetadata: return "useMetadata";
      case UsdUVTexture::Wrap::Black: return "black";
      case UsdUVTexture::Wrap::Clamp: return "clamp";
      case UsdUVTexture::Wrap::Repeat: return "repeat";
      case UsdUVTexture::Wrap::Mirror: return "mirror";
      default: return "useMetadata";
    }
  };

  // Helper to convert SourceColorSpace enum to token string
  auto colorspace_to_string = [](UsdUVTexture::SourceColorSpace cs) -> std::string {
    switch (cs) {
      case UsdUVTexture::SourceColorSpace::Auto: return "auto";
      case UsdUVTexture::SourceColorSpace::Raw: return "raw";
      case UsdUVTexture::SourceColorSpace::SRGB: return "sRGB";
      default: return "auto";
    }
  };

  // Helper to handle timesampled float2 inputs (for st coordinates)
  auto add_input_spec_with_timesamples_float2 = [&](
      const std::string& input_name,
      const crate::CrateValue& default_value,
      const Animatable<value::texcoord2f>* animatable,
      const std::vector<Path>* connections = nullptr) -> bool {
    // First add the default value spec (with connectionPaths if also connected)
    if (!add_input_spec(input_name, "texCoord2f", default_value, connections)) {
      return false;
    }

    // If there are timesamples, create a separate timeSamples spec
    if (animatable && animatable->has_timesamples()) {
      // texcoord2f store -> float2 crate values (crate stores the underlying type).
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = animatable->get_timesamples_ptr()) {
        for (const auto &_s : _tsp->get_samples()) {
          if (_s.blocked) { ts.add_blocked_sample(_s.t, value::Value()); continue; }
          if (const value::texcoord2f *tv = _s.value.as<value::texcoord2f>()) {
            value::float2 sample_as_float2 = {tv->s, tv->t};
            ts.add_sample(_s.t, value::Value(sample_as_float2));
          }
        }
      }

      // Create timeSamples spec
      Path ts_path = prim_path.AppendProperty(input_name + ".timeSamples");
      crate::FieldValuePairVector ts_fields;

      crate::CrateValue ts_value;
      ts_value.Set(ts);
      ts_fields.push_back({"default", ts_value});

      if (!AddSpec(ts_path, SpecType::Attribute, ts_fields, err)) {
        return false;
      }
    }

    return true;
  };

  // Extract and add texture inputs

  // inputs:file (asset) - texture file path
  if (uv_texture->file.authored()) {
    const std::vector<Path>* conns = uv_texture->file.has_connections()
        ? &uv_texture->file.connections() : nullptr;
    bool emitted_value = false;
    auto file_opt = uv_texture->file.get_value();
    if (file_opt.has_value()) {
      const auto& file_animatable = file_opt.value();
      if (file_animatable.is_timesamples()) {
        // An ANIMATED texture path (`asset inputs:file.timeSamples = {...}`).
        // There is no default here, so the samples must live in the SAME spec as
        // the typeName: the reader builds one Property per spec, and a spec that
        // carries nothing but a typeName has no value to construct it from
        // ("Failed to construct Property `inputs:file`"). The separate
        // `<name>.timeSamples` spec that add_input_spec_with_timesamples_float2
        // uses above works only because its base spec also has a default.
        if (const value::TimeSamples* tsp =
                file_animatable.get_timesamples_ptr()) {
          Path input_path = prim_path.AppendProperty("inputs:file");
          crate::FieldValuePairVector input_fields;

          crate::CrateValue type_value;
          value::token type_tok("asset");
          type_value.Set(type_tok);
          input_fields.push_back({"typeName", type_value});

          crate::CrateValue ts_value;
          ts_value.Set(*tsp);
          input_fields.push_back({"timeSamples", ts_value});

          if (conns && !conns->empty()) {
            ListOp<Path> conn_listop;
            conn_listop.ClearAndMakeExplicit();
            conn_listop.SetExplicitItems(*conns);
            crate::CrateValue conn_value;
            conn_value.Set(conn_listop);
            input_fields.push_back({"connectionPaths", conn_value});
          }

          if (!AddSpec(input_path, SpecType::Attribute, input_fields, err)) {
            return false;
          }
          emitted_value = true;
        }
      } else {
        value::AssetPath file_path;
        if (file_animatable.get_scalar(&file_path)) {
          crate::CrateValue file_value;
          file_value.Set(file_path);
          if (!add_input_spec("inputs:file", "asset", file_value, conns,
                              &uv_texture->file.metas())) {
            return false;
          }
          emitted_value = true;
        }
      }
    }
    if (!emitted_value && conns) {
      if (!add_input_connection_spec("inputs:file", "asset", *conns)) return false;
    }
  }

  // inputs:st (texcoord2f) - texture coordinates
  if (uv_texture->st.authored()) {
    const std::vector<Path>* conns = uv_texture->st.has_connections()
        ? &uv_texture->st.connections() : nullptr;
    value::texcoord2f st = {0.0f, 0.0f};
    if (uv_texture->st.has_value() && uv_texture->st.get_value().get_scalar(&st)) {
      value::float2 st_as_float2 = {st.s, st.t};
      crate::CrateValue st_value;
      st_value.Set(st_as_float2);
      if (!add_input_spec_with_timesamples_float2("inputs:st", st_value, &uv_texture->st.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:st", "float2",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:wrapS (token) - S axis wrap mode
  if (uv_texture->wrapS.authored()) {
    const std::vector<Path>* conns = uv_texture->wrapS.has_connections()
        ? &uv_texture->wrapS.connections() : nullptr;
    bool emitted = false;
    if (!uv_texture->wrapS.is_value_empty() &&
        !uv_texture->wrapS.get_value().is_timesamples()) {
      UsdUVTexture::Wrap wrap_s;
      if (uv_texture->wrapS.get_value().get_scalar(&wrap_s)) {
        crate::CrateValue wraps_value;
        wraps_value.Set(value::token(wrap_to_string(wrap_s)));
        if (!add_input_spec("inputs:wrapS", "token", wraps_value, conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:wrapS", "token",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:wrapT (token) - T axis wrap mode
  if (uv_texture->wrapT.authored()) {
    const std::vector<Path>* conns = uv_texture->wrapT.has_connections()
        ? &uv_texture->wrapT.connections() : nullptr;
    bool emitted = false;
    if (!uv_texture->wrapT.is_value_empty() &&
        !uv_texture->wrapT.get_value().is_timesamples()) {
      UsdUVTexture::Wrap wrap_t;
      if (uv_texture->wrapT.get_value().get_scalar(&wrap_t)) {
        crate::CrateValue wrapt_value;
        wrapt_value.Set(value::token(wrap_to_string(wrap_t)));
        if (!add_input_spec("inputs:wrapT", "token", wrapt_value, conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:wrapT", "token",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:fallback (float4) - fallback color when texture is missing
  if (uv_texture->fallback.authored()) {
    const std::vector<Path>* conns = uv_texture->fallback.has_connections()
        ? &uv_texture->fallback.connections() : nullptr;
    bool emitted = false;
    if (!uv_texture->fallback.is_value_empty()) {
      const Animatable<value::float4> &fallback =
          uv_texture->fallback.get_value();
      if (fallback.has_default()) {
        crate::CrateValue fallback_value;
        fallback_value.Set(fallback.get_scalar_ref());
        if (!add_input_spec("inputs:fallback", "float4", fallback_value, conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:fallback", "float4",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:sourceColorSpace (token) - color space
  if (uv_texture->sourceColorSpace.authored()) {
    const std::vector<Path>* conns = uv_texture->sourceColorSpace.has_connections()
        ? &uv_texture->sourceColorSpace.connections() : nullptr;
    bool emitted = false;
    if (!uv_texture->sourceColorSpace.is_value_empty() &&
        !uv_texture->sourceColorSpace.get_value().is_timesamples()) {
      UsdUVTexture::SourceColorSpace cs;
      if (uv_texture->sourceColorSpace.get_value().get_scalar(&cs)) {
        crate::CrateValue colorspace_value;
        colorspace_value.Set(value::token(colorspace_to_string(cs)));
        if (!add_input_spec("inputs:sourceColorSpace", "token", colorspace_value,
                            conns)) {
          return false;
        }
        emitted = true;
      }
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:sourceColorSpace", "token",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // inputs:scale (float4) - scale factor
  if (uv_texture->scale.authored()) {
    const std::vector<Path>* conns = uv_texture->scale.has_connections()
        ? &uv_texture->scale.connections() : nullptr;
    if (!uv_texture->scale.is_value_empty()) {
      crate::CrateValue scale_value;
      scale_value.Set(uv_texture->scale.get_value());
      if (!add_input_spec("inputs:scale", "float4", scale_value, conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:scale", "float4",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:bias (float4) - bias offset
  if (uv_texture->bias.authored()) {
    const std::vector<Path>* conns = uv_texture->bias.has_connections()
        ? &uv_texture->bias.connections() : nullptr;
    if (!uv_texture->bias.is_value_empty()) {
      crate::CrateValue bias_value;
      bias_value.Set(uv_texture->bias.get_value());
      if (!add_input_spec("inputs:bias", "float4", bias_value, conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:bias", "float4",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:uv_set (int) - UV set index (lightusd extension; mirrors the reader
  // in prim-reconstruct-shader3.cc and the USDA printer in pprint-shader.cc).
  if (uv_texture->uv_set.authored()) {
    const std::vector<Path>* conns = uv_texture->uv_set.has_connections()
        ? &uv_texture->uv_set.connections() : nullptr;
    if (!uv_texture->uv_set.is_value_empty()) {
      crate::CrateValue uv_set_value;
      uv_set_value.Set(uv_texture->uv_set.get_value());
      if (!add_input_spec("inputs:uv_set", "int", uv_set_value, conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:uv_set", "int",
                                      conns ? *conns : std::vector<Path>{})) return false;
    }
  }

  // inputs:uv_set_name (token) - UV set name (lightusd extension).
  if (uv_texture->uv_set_name.authored()) {
    const std::vector<Path>* conns = uv_texture->uv_set_name.has_connections()
        ? &uv_texture->uv_set_name.connections() : nullptr;
    bool emitted = false;
    value::token uv_set_name_tok;
    if (!uv_texture->uv_set_name.is_value_empty() &&
        uv_texture->uv_set_name.get_value(&uv_set_name_tok)) {
      crate::CrateValue uv_set_name_value;
      uv_set_name_value.Set(uv_set_name_tok);
      if (!add_input_spec("inputs:uv_set_name", "token", uv_set_name_value,
                          conns)) {
        return false;
      }
      emitted = true;
    }
    if (!emitted) {
      if (!add_input_connection_spec("inputs:uv_set_name", "token",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  return true;
}

// ============================================================================
// UsdPrimvarReader Shader Input Specs (called AFTER Shader prim spec is added)
// ============================================================================

bool CrateWriter::AddUsdPrimvarReaderInputSpecs(
  const value::Value& shader_value,
  const std::string& reader_type,
  const Path& prim_path,
  std::string* err
) {
  DCOUT("[AddUsdPrimvarReaderInputSpecs] prim_path: " << prim_path.full_path_name()
            << ", type: " << reader_type);

  // Helper lambda to add an input spec as a separate attribute
  auto add_input_spec = [&](const std::string& input_name, const std::string& type_name, const crate::CrateValue& value, const std::vector<Path>* connections = nullptr) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    // typeName field
    crate::CrateValue typename_value;
    value::token typename_tok(type_name);
    typename_value.Set(typename_tok);
    input_fields.push_back({"typeName", typename_value});

    // default field (the value)
    input_fields.push_back({"default", value});

    // USD permits an attribute to author BOTH a value and a connection; emit
    // connectionPaths alongside the default so value+connect coexistence
    // survives the crate write (matches usdcat).
    if (connections && !connections->empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(*connections);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper: emit an input that holds a connection (no concrete value), e.g.
  // `inputs:varname.connect = </Material.inputs:stPrimvarName>`. Without this,
  // a connected varname (the common case for UV-coordinate readers wired to the
  // material's primvar-name interface input) is dropped on USDC write, which
  // later breaks UsdUVTexture evaluation (missing st reader varname).
  auto add_input_connection_spec = [&](const std::string& input_name,
                                        const std::string& type_name,
                                        const std::vector<Path>& conn_paths) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});

    // Empty => a DECLARED-but-unconnected, value-less input (`token inputs:wrapS`
    // on its own). It still has to be written, as typeName alone: dropping it
    // loses the declaration, and writing a `default` would invent the schema
    // fallback as an authored value -- the bug this whole change is about.
    if (!conn_paths.empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(conn_paths);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Extract varname (string) - common to all UsdPrimvarReader variants
  // Try to get varname from the shader value using type-erased access
  std::string varname_str;
  bool has_varname = false;
  std::vector<Path> varname_conns;
  bool has_varname_conn = false;
  // Authored typeName spelling: the older UsdPrimvarReader spec used `token`,
  // and OpenUSD preserves whichever the author wrote.
  std::string varname_type = "string";

  // The varname field is TypedAttribute<Animatable<std::string>>
  // We need to extract it generically from the shader_value

  // Helper macro to try extracting varname (value or connection) from a
  // specific UsdPrimvarReader type.
  #define TRY_EXTRACT_VARNAME(ReaderType) \
    if (!has_varname && !has_varname_conn) { \
      if (auto* reader = shader_value.as<ReaderType>()) { \
        if (reader->varname.authored()) { \
          if (reader->varname.has_actual_type()) { \
            varname_type = reader->varname.get_actual_type_name(); \
          } \
          if (reader->varname.has_connections()) { \
            varname_conns = reader->varname.connections(); \
            has_varname_conn = !varname_conns.empty(); \
          } \
          /* USD allows a value AND a connection; capture the value too. */ \
          auto varname_opt = reader->varname.get_value(); \
          if (varname_opt.has_value()) { \
            const auto& varname_anim = varname_opt.value(); \
            if (!varname_anim.is_timesamples()) { \
              std::string vn; \
              if (varname_anim.get_scalar(&vn)) { \
                varname_str = vn; \
                has_varname = true; \
              } \
            } \
          } \
        } \
      } \
    }

  // Try all supported UsdPrimvarReader types
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_float)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_float2)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_float3)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_float4)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_int)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_string)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_normal)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_vector)
  TRY_EXTRACT_VARNAME(UsdPrimvarReader_point)

  #undef TRY_EXTRACT_VARNAME

  // Add inputs:varname - value and/or connection (USD allows both). Declared
  // with the AUTHORED type: `token` (older spec) or `string`.
  if (has_varname) {
    crate::CrateValue varname_value;
    if (varname_type == "token") {
      varname_value.Set(value::token(varname_str));
    } else {
      varname_value.Set(varname_str);
    }
    if (!add_input_spec("inputs:varname", varname_type, varname_value,
                        has_varname_conn ? &varname_conns : nullptr)) {
      return false;
    }
  } else if (has_varname_conn) {
    if (!add_input_connection_spec("inputs:varname", varname_type, varname_conns)) {
      return false;
    }
  }

  // Note: We don't extract the fallback value because it's type-specific
  // and would require templated handling for each type variant.
  // The varname is the most important input for UsdPrimvarReader.
  // If fallback support is needed, it can be added later with type-specific handlers.

  return true;
}

bool CrateWriter::AddUsdTransform2dInputSpecs(
  const UsdTransform2d* transform2d,
  const Path& prim_path,
  std::string* err
) {

  // Helper lambda to add an input spec as a separate attribute
  auto add_input_spec = [&](const std::string& input_name, const std::string& type_name, const crate::CrateValue& value, const std::vector<Path>* connections = nullptr) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    // typeName field
    crate::CrateValue typename_value;
    value::token typename_tok(type_name);
    typename_value.Set(typename_tok);
    input_fields.push_back({"typeName", typename_value});

    // default field (the value)
    input_fields.push_back({"default", value});

    // USD permits an attribute to author BOTH a value and a connection; emit
    // connectionPaths alongside the default so value+connect coexistence
    // survives the crate write (matches usdcat).
    if (connections && !connections->empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(*connections);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper: emit an input that holds a connection (no concrete value), e.g.
  // `inputs:in.connect = </.../PrimvarReader.outputs:result>`. UsdTransform2d's
  // `in` is normally wired to a UsdPrimvarReader's st output; without this the
  // connection is dropped on USDC write and UsdUVTexture evaluation fails
  // (`inputs:in` must be a connection).
  auto add_input_connection_spec = [&](const std::string& input_name,
                                        const std::string& type_name,
                                        const std::vector<Path>& conn_paths) -> bool {
    Path input_path = prim_path.AppendProperty(input_name);
    crate::FieldValuePairVector input_fields;

    crate::CrateValue type_value;
    value::token type_tok(type_name);
    type_value.Set(type_tok);
    input_fields.push_back({"typeName", type_value});

    // Empty => a DECLARED-but-unconnected, value-less input (`token inputs:wrapS`
    // on its own). It still has to be written, as typeName alone: dropping it
    // loses the declaration, and writing a `default` would invent the schema
    // fallback as an authored value -- the bug this whole change is about.
    if (!conn_paths.empty()) {
      ListOp<Path> conn_listop;
      conn_listop.ClearAndMakeExplicit();
      conn_listop.SetExplicitItems(conn_paths);
      crate::CrateValue conn_value;
      conn_value.Set(conn_listop);
      input_fields.push_back({"connectionPaths", conn_value});
    }

    return AddSpec(input_path, SpecType::Attribute, input_fields, err);
  };

  // Helper to handle timesampled float2 inputs
  auto add_input_spec_with_timesamples_float2 = [&](
      const std::string& input_name,
      const crate::CrateValue& default_value,
      const Animatable<value::float2>* animatable,
      const std::vector<Path>* connections = nullptr) -> bool {
    // First add the default value spec (with connectionPaths if also connected)
    if (!add_input_spec(input_name, "float2", default_value, connections)) {
      return false;
    }

    // If there are timesamples, create a separate timeSamples spec
    if (animatable && animatable->has_timesamples()) {
      // Value-type Animatable stores a type-erased value::TimeSamples directly.
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = animatable->get_timesamples_ptr()) {
        ts = *_tsp;
      }

      // Create timeSamples spec
      Path ts_path = prim_path.AppendProperty(input_name + ".timeSamples");
      crate::FieldValuePairVector ts_fields;

      crate::CrateValue ts_value;
      ts_value.Set(ts);
      ts_fields.push_back({"default", ts_value});

      if (!AddSpec(ts_path, SpecType::Attribute, ts_fields, err)) {
        return false;
      }
    }

    return true;
  };

  // Helper to handle timesampled float inputs
  auto add_input_spec_with_timesamples_float = [&](
      const std::string& input_name,
      const crate::CrateValue& default_value,
      const Animatable<float>* animatable,
      const std::vector<Path>* connections = nullptr) -> bool {
    // First add the default value spec (with connectionPaths if also connected)
    if (!add_input_spec(input_name, "float", default_value, connections)) {
      return false;
    }

    // If there are timesamples, create a separate timeSamples spec
    if (animatable && animatable->has_timesamples()) {
      // Value-type Animatable stores a type-erased value::TimeSamples directly.
      value::TimeSamples ts;
      if (const value::TimeSamples *_tsp = animatable->get_timesamples_ptr()) {
        ts = *_tsp;
      }

      // Create timeSamples spec
      Path ts_path = prim_path.AppendProperty(input_name + ".timeSamples");
      crate::FieldValuePairVector ts_fields;

      crate::CrateValue ts_value;
      ts_value.Set(ts);
      ts_fields.push_back({"default", ts_value});

      if (!AddSpec(ts_path, SpecType::Attribute, ts_fields, err)) {
        return false;
      }
    }

    return true;
  };

  // Extract inputs:in (float2) - as a connection (the common case: wired to a
  // UsdPrimvarReader's st output) or a concrete value.
  if (transform2d->in.authored()) {
    const std::vector<Path>* conns = transform2d->in.has_connections()
        ? &transform2d->in.connections() : nullptr;
    value::float2 in_value = {0.0f, 0.0f};
    if (transform2d->in.has_value() && transform2d->in.get_value().get_scalar(&in_value)) {
      crate::CrateValue in_crate_value;
      in_crate_value.Set(in_value);
      if (!add_input_spec_with_timesamples_float2("inputs:in", in_crate_value, &transform2d->in.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:in", "float2",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // Extract inputs:rotation (float) - in degrees, CCW
  if (transform2d->rotation.authored()) {
    const std::vector<Path>* conns = transform2d->rotation.has_connections()
        ? &transform2d->rotation.connections() : nullptr;
    float rotation_value = 0.0f;
    if (!transform2d->rotation.is_value_empty() &&
        transform2d->rotation.get_value().get_scalar(&rotation_value)) {
      crate::CrateValue rotation_crate_value;
      rotation_crate_value.Set(rotation_value);
      if (!add_input_spec_with_timesamples_float("inputs:rotation", rotation_crate_value, &transform2d->rotation.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:rotation", "float",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // Extract inputs:scale (float2)
  if (transform2d->scale.authored()) {
    const std::vector<Path>* conns = transform2d->scale.has_connections()
        ? &transform2d->scale.connections() : nullptr;
    value::float2 scale_value = {1.0f, 1.0f};
    if (!transform2d->scale.is_value_empty() &&
        transform2d->scale.get_value().get_scalar(&scale_value)) {
      crate::CrateValue scale_crate_value;
      scale_crate_value.Set(scale_value);
      if (!add_input_spec_with_timesamples_float2("inputs:scale", scale_crate_value, &transform2d->scale.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:scale", "float2",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  // Extract inputs:translation (float2)
  if (transform2d->translation.authored()) {
    const std::vector<Path>* conns = transform2d->translation.has_connections()
        ? &transform2d->translation.connections() : nullptr;
    value::float2 translation_value = {0.0f, 0.0f};
    if (!transform2d->translation.is_value_empty() &&
        transform2d->translation.get_value().get_scalar(&translation_value)) {
      crate::CrateValue translation_crate_value;
      translation_crate_value.Set(translation_value);
      if (!add_input_spec_with_timesamples_float2("inputs:translation", translation_crate_value, &transform2d->translation.get_value(), conns)) {
        return false;
      }
    } else {
      if (!add_input_connection_spec("inputs:translation", "float2",
                                      conns ? *conns : std::vector<Path>{})) {
        return false;
      }
    }
  }

  return true;
}

// ============================================================================
// Shader Property Extraction
// ============================================================================

bool CrateWriter::ExtractShaderProperties(
  const Prim& prim,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const Shader* shader = prim.data().as<Shader>();
  if (!shader) {
    if (err) *err = "Failed to cast prim to Shader";
    return false;
  }


  // Add info:id (shader type identifier)
  if (!shader->info_id.empty()) {
    crate::CrateValue info_id_value;
    value::token tok(shader->info_id);
    info_id_value.Set(tok);
    fields.push_back({"info:id", info_id_value});
  }

  // Shader inputs and outputs are handled through the props map
  // which will be processed by ConvertPropertyToFields

  return true;
}

bool CrateWriter::ExtractNodeGraphProperties(
  const Prim& prim,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  const NodeGraph* node_graph = prim.data().as<NodeGraph>();
  if (!node_graph) {
    if (err) *err = "Failed to cast prim to NodeGraph";
    return false;
  }

  // Same typed `purpose` as Material (both derive from UsdShadePrim).
  if (node_graph->purpose.authored()) {
    const Purpose purpose_val = node_graph->purpose.get_value();
    if (purpose_val != Purpose::Default) {
      crate::CrateValue purpose_crate_val;
      purpose_crate_val.Set(value::token(to_string(purpose_val)));
      fields.push_back({"purpose", purpose_crate_val});
    }
  }

  // NodeGraph is primarily a container for organizing shader nodes and interfaces.
  // All specific properties (inputs, outputs, and child nodes) are handled through
  // the generic property system via the props map.
  // This function acts as a type-specific handler but defers to the generic system.

  return true;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
