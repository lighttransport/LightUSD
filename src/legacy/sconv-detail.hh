// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Internal header for stage-converter split files.
// Contains template method definitions that must be visible at call sites.
//
#pragma once

#include "crate-writer.hh"
#include "timesamples.hh"  // value::TimeSamples (value-types.hh no longer pulls timesamples.hh transitively)
#include "common-macros.inc"
#include "pprinter.hh"  // For to_string(Specifier), to_string(GeomMesh enums), etc.

namespace lightusd {
namespace experimental {

namespace sconv_detail {

// Emit an attribute's OWN metadata block as `<name>.<key>` fields. The field
// router in ConvertSinglePrim (stage-converter.cc, kAttrMetaSuffixes) recognises
// that spelling and folds each one into the attribute's spec.
//
// The TYPED writers build their fields by hand and never emitted any of this, so
// an attribute's customData / comment / colorSpace / ... were dropped on write
// even though the generic attribute path (ConvertAttributeToFields) has handled
// them all along.
inline void EmitAttrMetas(const std::string &name, const AttrMeta &metas,
                          crate::FieldValuePairVector &fields) {
  if (metas.has_customData()) {
    crate::CrateValue v;
    v.Set(metas.get_customData());
    fields.push_back({name + ".customData", v});
  }

  // A bare string in an attribute's metadata block (`double x = 1 ( """m""" )`)
  // IS the comment in USD -- the two spellings are the same Sdf field, and only
  // the ASCII parser knows which was used (it parks the bare form in
  // AttrMeta::stringData). So both go out as the STANDARD `comment` field; a
  // private crate field would be unreadable to pxr. The ASCII spelling is not
  // preserved across a crate round-trip -- pxr does not preserve it either.
  //
  // Emit ONE comment field: two would corrupt the fieldset encoding.
  if (metas.has_comment() || !metas.stringData.empty()) {
    std::string comment_str;
    if (metas.has_comment()) {
      comment_str = metas.get_comment().value;
    } else {
      for (size_t i = 0; i < metas.stringData.size(); i++) {
        if (i > 0) comment_str += "\n";
        comment_str += metas.stringData[i].value;
      }
    }
    crate::CrateValue v;
    v.Set(comment_str);
    fields.push_back({name + ".comment", v});
  }

  if (metas.has_colorSpace()) {
    crate::CrateValue v;
    v.Set(metas.get_colorSpace());
    fields.push_back({name + ".colorSpace", v});
  }

  if (metas.has_displayName()) {
    crate::CrateValue v;
    v.Set(metas.get_displayName());
    fields.push_back({name + ".displayName", v});
  }

  if (metas.has_doc()) {
    crate::CrateValue v;
    v.Set(metas.get_doc().value);
    fields.push_back({name + ".documentation", v});
  }

  // Unregistered property metadata: emit as the crate UNREGISTERED_VALUE type
  // (what pxr writes for SdfUnregisteredValue), carrying the raw USDA text.
  // The field router recognises the dotted spelling by the UNREGISTERED marker
  // on the value (the suffix itself is by definition not a known meta key).
  for (const auto &kv : metas.unregisteredMetas) {
    crate::CrateValue v;
    v.SetUnregisteredValueString(kv.second);
    fields.push_back({name + "." + kv.first, v});
  }
}

// Emit a DECLARATION-ONLY attribute (`double radius`, no value).
//
// An attribute is `authored()` the moment it is DECLARED, with or without a
// value, and TypedAttributeWithFallback::get_value() silently returns the SCHEMA
// FALLBACK when no value was authored -- so a writer that calls get_value()
// straight off authored() invents a value the author never wrote (`double radius`
// came back as `double radius = 2`). That is not cosmetic: an authored opinion is
// a STRONG opinion, so a fabricated one WINS over the weaker opinions it should
// have deferred to during composition.
//
// The `.typeName` suffix is understood by the field router in ConvertSinglePrim
// (stage-converter.cc), which needs it because it otherwise infers the spec's
// type from the default value -- and here there is none.
inline void EmitAttrDeclaration(const std::string &name, const std::string &type_name,
                         crate::FieldValuePairVector &fields) {
  crate::CrateValue v;
  v.Set(value::token(type_name));
  fields.push_back({name + ".typeName", v});
}

// Emit `<name>.connect` for a typed attribute that carries connection targets.
// USD lets ANY attribute be connected -- not just shader inputs -- and the ASCII
// reader stores those targets on the TypedAttribute itself. The typed writers
// only ever emitted the value, so `double size.connect = </bora.value>` on a
// Cube was dropped. The field router in ConvertSinglePrim (stage-converter.cc)
// recognizes the `.connect` suffix and folds it into the attribute spec's
// `connectionPaths`.
// Carry an authored `uniform` qualifier from a typed attribute into the field
// stream. The field router (ConvertSinglePrim) folds `<name>.uniform` into the
// attribute spec's `variability`; without it, variability is guessed from the
// property name, which loses `uniform` for every schema property whose name is
// not on that list (and mis-stamps non-conformant authored `uniform`).
template <typename T>
void EmitAttrVariability(const std::string &name, const T &attr,
                         crate::FieldValuePairVector &fields) {
  if (!attr.has_variability()) {
    return;
  }
  // Emit BOTH values. `false` matters: without it the writer falls back to its
  // schema-uniform name list, which would over-stamp a schema-uniform property
  // the layer deliberately authored as `varying` (e.g. `token projection`).
  // The value is an inline bool, so the cost is one field index per spec.
  crate::CrateValue v;
  v.Set(attr.variability() == Variability::Uniform);
  fields.push_back({name + ".uniform", v});
}

template <typename T>
void EmitAttrConnections(const std::string &name, const T &attr,
                         crate::FieldValuePairVector &fields) {
  if (!attr.has_connections()) {
    return;
  }

  ListOp<Path> conn_listop;
  conn_listop.ClearAndMakeExplicit();
  conn_listop.SetExplicitItems(attr.connections());

  crate::CrateValue v;
  v.Set(conn_listop);
  fields.push_back({name + ".connect", v});
}

} // namespace sconv_detail

template <typename T>
bool CrateWriter::EmitTypedAnimatableAttr(
    const char *name, const TypedAttributeWithFallback<Animatable<T>> &attr,
    crate::FieldValuePairVector &fields, std::string *err) {
  if (attr.authored()) {
    // The authored typeName may be a role-spelling variant of T (e.g. authored
    // `float3` for a color3f field) -- declare what the author wrote, not the
    // schema type. Role types share the wire type, only the typeName differs.
    const std::string decl_type = attr.has_actual_type()
                                      ? attr.get_actual_type_name()
                                      : value::TypeTraits<T>::type_name();
    if (attr.is_value_empty()) {
      // DECLARED with no value. Emitting attr.get_value() here would emit the
      // schema fallback -- see EmitAttrDeclaration.
      sconv_detail::EmitAttrDeclaration(name, decl_type, fields);
    } else {
      if (!ExtractAnimatableDefault(attr.get_value(), name, fields, err)) {
        return false;
      }
      // A VALUED attribute normally gets its typeName inferred from the value;
      // override it when the authored spelling differs.
      if (attr.has_actual_type()) {
        sconv_detail::EmitAttrDeclaration(name, decl_type, fields);
      }
    }
    sconv_detail::EmitAttrMetas(name, attr.metas(), fields);
    sconv_detail::EmitAttrVariability(name, attr, fields);
  }

  // A connection is authored independently of a value, so this sits outside the
  // authored() branch -- `double radius.connect = </x.y>` with no value at all.
  sconv_detail::EmitAttrConnections(name, attr, fields);

  return true;
}

template<typename T>
bool CrateWriter::AddTypedArrayAttribute(
    const char* name, const T& typed_attr,
    crate::FieldValuePairVector& fields) {
  if (typed_attr.has_value()) {
    auto val_opt = typed_attr.get_value();
    if (val_opt) {
      crate::CrateValue crate_val;
      value::Value v(*val_opt);
      std::string conv_err;
      if (!ConvertValue(v, crate_val, &conv_err)) {
        DCOUT("WARNING: Skipping unsupported type for " << name << ": " << conv_err);
        return true;
      }
      fields.push_back({name, crate_val});
      // Preserve the role spelling degraded by ConvertValue (see
      // ExtractAnimatableDefault above).
      if (v.type_name() != crate_val.type_name()) {
        crate::CrateValue ty;
        ty.Set(value::token(v.type_name()));
        fields.push_back({std::string(name) + ".typeName", ty});
      }
    }
  }
  return true;
}

template<typename T>
bool CrateWriter::ExtractAnimatableDefault(
    const Animatable<T>& anim, const char* name,
    crate::FieldValuePairVector& fields, std::string* err) {
  if (anim.has_default()) {
    T val;
    if (anim.get_default(&val)) {
      crate::CrateValue crate_val;
      value::Value v(val);
      if (!ConvertValue(v, crate_val, err)) return false;
      fields.push_back({name, crate_val});
      // ConvertValue degrades role types (color3f -> float3 crate value); the
      // spec assembly infers typeName from the value, so declare the schema
      // spelling explicitly or the attribute is re-typed on the wire. An
      // authored-spelling override (has_actual_type) is emitted AFTER this by
      // EmitTypedAnimatableAttr and wins in the field router.
      if (value::TypeTraits<T>::type_name() != crate_val.type_name()) {
        crate::CrateValue ty;
        ty.Set(value::token(value::TypeTraits<T>::type_name()));
        fields.push_back({std::string(name) + ".typeName", ty});
      }
    }
  }
  if (anim.has_timesamples()) {
    // Value-type Animatable stores a type-erased value::TimeSamples directly.
    if (const value::TimeSamples *tsp = anim.get_timesamples_ptr()) {
      crate::CrateValue ts_crate_val;
      ts_crate_val.Set(*tsp);
      fields.push_back({std::string(name) + ".timeSamples", ts_crate_val});
    }
  }
  return true;
}


} // namespace experimental
} // namespace lightusd
