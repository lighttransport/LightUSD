// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// UsdGeom API implementations


#include <cstring>
#include <sstream>
#include <type_traits>

#include "value-types.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "xform.hh"
#include "usdGeom.hh"
//
//
#include "common-macros.inc"
#include "math-util.inc"
#include "str-util.hh"
#include "value-pprint.hh"
#include "core/safe-arithmetic.hh"

#define SET_ERROR_AND_RETURN(msg) \
  if (err) {                      \
    (*err) = (msg);               \
  }                               \
  return false

namespace lightusd { namespace legacy {

namespace {

constexpr auto kPrimvars = "primvars:";
constexpr auto kIndices = ":indices";


}  // namespace

// GeomPrimvar template bodies + index-expansion helpers. The per-type
// flatten_with_indices/get_value instantiations live in usdGeom-primvar-inst-*.cc;
// usdGeom.hh's EXTERN_TEMPLATE_GET_VALUE suppresses implicit instantiation here.
// Included for the in-file ExpandWithIndices use (GeomMesh::get_normals).
#include "usdGeom-primvar-impl.inc"

bool IsSupportedGeomPrimvarType(uint32_t tyid) {
  //
  // scalar and 1D
  //
#define SUPPORTED_TYPE_FUN(__ty)                                           \
  case value::TypeTraits<__ty>::type_id(): {                                 \
    return true;                                                           \
  }                                                                        \
  case (value::TypeTraits<__ty>::type_id() | value::TYPE_ID_1D_ARRAY_BIT): { \
    return true;                                                           \
  }

  switch (tyid) {
    APPLY_GEOMPRIVAR_TYPE(SUPPORTED_TYPE_FUN)
    default:
      return false;
  }

#undef SUPPORTED_TYPE_FUN
}

bool IsSupportedGeomPrimvarType(const std::string &type_name) {
  return IsSupportedGeomPrimvarType(value::GetTypeId(type_name));
}

bool GeomPrimvar::has_elementSize() const {
  return _elementSize.has_value();
}

uint32_t GeomPrimvar::get_elementSize() const {
  if (_elementSize.has_value()) {
    return _elementSize.value();
  }
  return 1;
}

bool GeomPrimvar::has_interpolation() const {
  return _interpolation.has_value();
}

Interpolation GeomPrimvar::get_interpolation() const {
  if (_interpolation.has_value()) {
    return _interpolation.value();
  }

  return Interpolation::Constant;  // unauthored
}

bool GPrim::has_primvar(const std::string &varname) const {
  std::string primvar_name = kPrimvars + varname;
  return props.count(primvar_name);
}

bool GPrim::get_primvar(const std::string &varname, GeomPrimvar *out_primvar,
                        std::string *err) const {
  if (!out_primvar) {
    SET_ERROR_AND_RETURN("Output GeomPrimvar is nullptr.");
  }

  GeomPrimvar primvar;

  std::string primvar_name = kPrimvars + varname;

  const auto it = props.find(primvar_name);
  if (it == props.end()) {
    return false;
  }

  if (it->second.is_attribute()) {
    const Attribute &attr = it->second.get_attribute();

    primvar.set_value(attr);
    primvar.set_name(varname);
    if (attr.metas().has_interpolation()) {
      primvar.set_interpolation(attr.metas().get_interpolation_enum());
    }
    if (attr.metas().has_elementSize()) {
      primvar.set_elementSize(attr.metas().get_elementSize());
    }
    if (attr.metas().has_unauthoredValuesIndex()) {
      primvar.set_unauthoredValuesIndex(attr.metas().get_unauthoredValuesIndex());
    }

  } else {
    SET_ERROR_AND_RETURN(fmt::format("{} is not Attribute. Maybe Relationship?", primvar_name));
  }

  // has indices?
  std::string index_name = primvar_name + kIndices;
  const auto indexIt = props.find(index_name);

  if (indexIt != props.end()) {
    if (indexIt->second.is_attribute()) {

      if (!(primvar.get_attribute().type_id() & value::TYPE_ID_1D_ARRAY_BIT)) {
        SET_ERROR_AND_RETURN(
            fmt::format("Indexed GeomPrimVar with scalar PrimVar Attribute is not supported. PrimVar name: {}", primvar_name));
      }

      const Attribute &indexAttr = indexIt->second.get_attribute();

      if (indexAttr.is_connection()) {
        SET_ERROR_AND_RETURN(
            "Attribute Connetion is not supported for index Attribute, since we need Stage info to find Prim referred by targetPath. Use Tydra API tydra::GetGeomPrimvar.");
      }

      if (indexAttr.is_blocked()) {
        // ignore Index attribute.
      } else {

        if (indexAttr.has_timesamples()) {
          // Indices timesamples are stored type-erased; the int[] element type
          // is validated lazily on read (resolve_indices_at).
          primvar.set_timesampled_indices(indexAttr.get_var().ts_raw());
        }

        if (indexAttr.has_value()) {
          // Check if int[] type.
          // Note: OpenUSD only uses int[] for primvar indices.
          std::vector<int32_t> indices;
          if (!indexAttr.get_value(&indices)) {
            SET_ERROR_AND_RETURN(
                fmt::format("Index Attribute is not int[] type. Got {}",
                            indexAttr.type_name()));
          }

          primvar.set_default_indices(indices);
        }
      }
    } else {
      // indices are optional, so ok to skip it.
    }
  }

  (*out_primvar) = primvar;

  return true;
}

const std::vector<int32_t> &GeomPrimvar::resolve_indices_at(
    double t, value::TimeSampleInterpolationType tinterp,
    std::vector<int32_t> &buf) const {
  if (value::TimeCode(t).is_default()) {
    if (has_default_indices()) {
      return _indices;  // zero-copy
    }
    if (has_timesampled_indices()) {
      _ts_indices.get(&buf, t, tinterp);
      return buf;
    }
  } else {
    if (has_timesampled_indices()) {
      _ts_indices.get(&buf, t, tinterp);
      return buf;
    }
  }
  static const std::vector<int32_t> empty;
  return empty;
}


std::vector<int32_t> GeomPrimvar::get_indices(const double t) const {
  if (value::TimeCode(t).is_default()) {
    if (has_default_indices()) {
      return get_default_indices();
    }
  }

  if (has_timesampled_indices()) {
    std::vector<int32_t> indices;
    if (get_timesampled_indices().get(&indices, t)) {
      return indices;
    }
  }

  if (has_default_indices()) {
    return get_default_indices();
  }

  return std::vector<int32_t>();
}

void GeomPrimvar::set_indices(const std::vector<int32_t> &indices, const double t) {
  if (value::TimeCode(t).is_default()) {
    _indices = indices;
  } else {
    // add_sample overwrites an existing sample at the same time.
    _ts_indices.add_sample(t, value::Value(indices));
  }
}


bool GeomPrimvar::flatten_with_indices(const double t, value::Value *dest, const value::TimeSampleInterpolationType tinterp, std::string *err) const {
  if (!dest) {
    if (err) { (*err) += "Output value is nullptr."; }
    return false;
  }

  if (!(_attr.has_value() || _attr.has_timesamples())) {
    return false;
  }

  // A primvar authored as a ValueBlock (`= None`, e.g. ALab GEO_PROXY meshes'
  // `float[] primvars:displayOpacity = None`) declares the type but carries no
  // usable data. Treat it as "no data": return false with NO error message so
  // the caller skips the primvar rather than failing the whole mesh conversion.
  if (_attr.is_blocked()) {
    return false;
  }

  if (!IsSupportedGeomPrimvarType(_attr.type_id())) {
    if (err) {
      (*err) += fmt::format("Unsupported type for GeomPrimvar. type = `{}`",
                            _attr.type_name());
    }
    return false;
  }

  // Scalar type: pass through (matches OpenUSD ComputeFlattened).
  if (!(_attr.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) {
    value::Value v;
    if (!_attr.get_var().get_interpolated_value(t, tinterp, &v)) {
      if (err) { (*err) += "Failed to evaluate Attribute value."; }
      return false;
    }
    (*dest) = std::move(v);
    return true;
  }

  // Array type: delegate to typed flatten_with_indices, then wrap in Value.
#define APPLY_FUN(__ty)                                                    \
  case value::TypeTraits<__ty>::type_id() | value::TYPE_ID_1D_ARRAY_BIT: { \
    std::vector<__ty> result;                                              \
    if (flatten_with_indices<__ty>(t, &result, tinterp, err)) {            \
      (*dest) = std::move(result);                                         \
      return true;                                                         \
    }                                                                      \
    return false;                                                          \
  }

  switch (_attr.type_id()) {
    APPLY_GEOMPRIVAR_TYPE(APPLY_FUN)
    default: break;
  }

#undef APPLY_FUN

  if (err) {
    (*err) += fmt::format("Unsupported array type for GeomPrimvar. type = `{}`",
                          _attr.type_name());
  }
  return false;
}

bool GeomPrimvar::flatten_with_indices(value::Value *dest, std::string *err) const {
  return flatten_with_indices(value::TimeCode::Default(), dest, value::TimeSampleInterpolationType::Linear, err);
}

// Non-template value extraction. The heavy logic lives here once; the templated
// GeomPrimvar::get_value<T> overloads are thin forwarders that call these and
// cast the result with value::Value::as<T>(). (flatten_with_indices keeps its own
// templated fast path.)
bool GeomPrimvar::get_value(value::Value *dest, std::string *err) const {
  if (!dest) {
    if (err) { (*err) += "Output value is nullptr."; }
    return false;
  }

  if (_attr.is_blocked()) {
    if (err) { (*err) += "Attribute is blocked."; }
    return false;
  }

  if (_attr.has_value()) {
    if (!IsSupportedGeomPrimvarType(_attr.type_id())) {
      if (err) {
        (*err) += fmt::format("Unsupported type for GeomPrimvar. type = `{}`",
                              _attr.type_name());
      }
      return false;
    }
    (*dest) = _attr.get_var().value_raw();
    return true;
  }

  if (_attr.has_timesamples()) {
    const value::TimeSamples &ts = _attr.get_var().ts_raw();
    if (ts.empty()) {
      if (err) { (*err) += "No TimeSample value in Attribute."; }
      return false;
    }
    // First sample (matches the previous templated get_value behavior).
    (*dest) = ts.get_samples().at(0).value;
    return true;
  }

  return false;
}

bool GeomPrimvar::get_value(double timecode, value::Value *dest,
                            value::TimeSampleInterpolationType interp,
                            std::string *err) const {
  if (!dest) {
    if (err) { (*err) += "Output value is nullptr."; }
    return false;
  }

  if (_attr.is_blocked()) {
    if (err) { (*err) += "Attribute is blocked."; }
    return false;
  }

  if (!IsSupportedGeomPrimvarType(_attr.type_id())) {
    if (err) {
      (*err) += fmt::format("Unsupported type for GeomPrimvar. type = `{}`",
                            _attr.type_name());
    }
    return false;
  }

  // Mirror Attribute::get(t, dst, interp): default time uses the default value,
  // otherwise interpolate timesamples, otherwise fall back to the default value.
  const primvar::PrimVar &pv = _attr.get_var();
  if (value::TimeCode(timecode).is_default() && _attr.has_value()) {
    (*dest) = pv.value_raw();
    return true;
  }
  if (_attr.has_timesamples()) {
    return pv.get_interpolated_value(timecode, interp, dest);
  }
  if (_attr.has_value()) {
    (*dest) = pv.value_raw();
    return true;
  }

  if (err) {
    (*err) += fmt::format("Get Attribute value at time {} failed.", timecode);
  }
  return false;
}


std::vector<GeomPrimvar> GPrim::get_primvars() const {
  std::vector<GeomPrimvar> gpvars;

  for (const auto &prop : props) {
    if (startsWith(prop.first, kPrimvars)) {
      // skip `:indices`. Attribute with `:indices` suffix is handled in
      // `get_primvar`
      // Also skips primvar attribute with `:indices` suffix only.
      if (endsWith(prop.first, kIndices)) {
        continue;
      }

      GeomPrimvar gprimvar;
      if (get_primvar(removePrefix(prop.first, kPrimvars), &gprimvar)) {
        gpvars.emplace_back(std::move(gprimvar));
      }
    }
  }

  return gpvars;
}

bool GPrim::set_primvar(const GeomPrimvar &primvar,
                        std::string *err) {
  if (primvar.name().empty()) {
    if (err) {
      (*err) += "GeomPrimvar.name is empty.";
    }
    return false;
  }

  if (startsWith(primvar.name(), "primvars:")) {
    if (err) {
      (*err) += "GeomPrimvar.name must not start with `primvars:` namespace. name = " + primvar.name();
    }
    return false;
  }

  std::string primvar_name = kPrimvars + primvar.name();

  // Overwrite existing primvar prop.
  DCOUT("Setting primvar `" << primvar_name << "`" <<
        (props.count(primvar_name) ? " (overwriting existing)" : ""));

  Attribute attr = primvar.get_attribute();

  if (primvar.has_interpolation()) {
    attr.metas().set_interpolation_enum(primvar.get_interpolation());
  }

  if (primvar.has_elementSize()) {
    attr.metas().set_elementSize(primvar.get_elementSize());
  }

  props[primvar_name] = attr;

  {
    primvar::PrimVar var;

    if (primvar.has_default_indices()) {
      var.set_value(primvar.get_default_indices());
    }

    if (primvar.has_timesampled_indices()) {
      for (const auto &sample : primvar.get_timesampled_indices().get_samples()) {
        var.set_timesample(sample.t, sample.value);
      }
    }

    if (primvar.has_default_indices() || primvar.has_timesampled_indices()) {
      Attribute indices;
      indices.set_var(var);
      std::string index_name = primvar_name + kIndices;
      props[index_name] = indices;
    }

  }

  return true;
}

bool GPrim::get_displayColor(value::color3f *dst, double t, const value::TimeSampleInterpolationType tinterp) const
{
  GeomPrimvar primvar;
  std::string err;
  if (!get_primvar("displayColor", &primvar, &err)) {
    return false;
  }

  return primvar.get_value(t, dst, tinterp);
}

bool GPrim::get_displayOpacity(float *dst, double t, const value::TimeSampleInterpolationType tinterp) const
{
  GeomPrimvar primvar;
  std::string err;
  if (!get_primvar("displayOpacity", &primvar, &err)) {
    return false;
  }

  return primvar.get_value(t, dst, tinterp);
}


}  // namespace legacy
}  // namespace lightusd
