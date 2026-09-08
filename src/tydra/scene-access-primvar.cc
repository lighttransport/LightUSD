// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "primvar.hh"
#include "scene-access.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"

#include <limits>
#include <string>
#include <vector>

namespace lightusd {
namespace tydra {

#define PushError(msg) \
  if (err) {           \
    *err += msg;       \
  }

bool GetGeomPrimvar(const Stage &stage, const GPrim *gprim,
                    const std::string &varname, GeomPrimvar *out_primvar,
                    std::string *err, std::string *warn) {
  if (!out_primvar) {
    PUSH_ERROR_AND_RETURN("Output GeomPrimvar is nullptr.");
  }

  if (!gprim) {
    PUSH_ERROR_AND_RETURN("Input `gprim` arg is nullptr.");
  }

  GeomPrimvar primvar;

  constexpr auto kPrimvars = "primvars:";
  constexpr auto kIndices = ":indices";

  std::string primvar_name = kPrimvars + varname;

  const auto it = gprim->props.find(primvar_name);
  if (it == gprim->props.end()) {
    return false;
  }

  // The order of Attribute value evaluation:
  // - default or timesamples
  // - connection


  if (it->second.is_attribute()) {
    const Attribute &attr = it->second.get_attribute();

    if (attr.has_connections()) { // a connection overrides the value (USD)
      // follow targetPath to get Attribute (GetTerminalAttribute falls back to
      // this attribute's own value if the connection cannot be resolved).
      Attribute terminal_attr;
      bool ret = tydra::GetTerminalAttribute(stage, attr, primvar_name,
                                             &terminal_attr, err);
      if (!ret) {
        return false;
      }

      primvar.set_value(terminal_attr);

    } else {
      // default, timeSamples
      primvar.set_value(attr);
    }

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
    // TODO: copy other attribute metas?

  } else {
    PUSH_ERROR_AND_RETURN(
        fmt::format("{} is not Attribute(Maybe Relationship?).", primvar_name));
  }

  // has indices?
  std::string index_name = primvar_name + kIndices;
  const auto indexIt = gprim->props.find(index_name);

  // Primvar indices are only relevant for non-constant interpolation modes
  bool constant_interpolation = primvar.get_interpolation() == lightusd::Interpolation::Constant;

  if (indexIt != gprim->props.end() && !constant_interpolation) {
    if (indexIt->second.is_attribute()) {
      const Attribute &indexAttr = indexIt->second.get_attribute();

      if (!(primvar.get_attribute().type_id() & value::TYPE_ID_1D_ARRAY_BIT)) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Indexed GeomPrimVar with scalar PrimVar Attribute is "
                        "not supported. PrimVar name: {}",
                        primvar_name));
      }

      if (indexAttr.has_connections()) { // a connection overrides the value (USD)
        // follow targetPath to get Attribute (falls back to this attribute's
        // own value if the connection cannot be resolved).
        Attribute terminal_indexAttr;
        bool ret = tydra::GetTerminalAttribute(stage, indexAttr, index_name,
                                               &terminal_indexAttr, err);
        if (!ret) {
          return false;
        }

        if (!terminal_indexAttr.has_value() && !terminal_indexAttr.has_timesamples()) {
          // No authored terminal indices value → treat as un-indexed (use the
          // primvar values directly) instead of failing, as above.
          DCOUT("primvars:" << varname
                << ":indices (terminal) declared with no authored value; "
                   "treating primvar as un-indexed.");
          if (warn) {
            (*warn) += fmt::format(
                "`primvars:{}:indices` (terminal) is declared with no authored "
                "value; treating the primvar as un-indexed.\n", varname);
          }
        }

        if (terminal_indexAttr.has_timesamples()) {
          primvar.set_timesampled_indices(
              terminal_indexAttr.get_var().ts_raw());
        }

        if (terminal_indexAttr.has_value()) {

          std::vector<int32_t> indices;
          if (!terminal_indexAttr.get_value(&indices)) {
            std::vector<uint32_t> uint_indices;
            if (!terminal_indexAttr.get_value(&uint_indices)) {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Index Attribute is not int[] or uint[] type. Got {}",
                              indexAttr.type_name()));
            }
            indices.reserve(uint_indices.size());
            for (uint32_t idx : uint_indices) {
              if (idx > uint32_t(std::numeric_limits<int32_t>::max())) {
                PUSH_ERROR_AND_RETURN(
                    fmt::format("Index Attribute contains value {} outside int32 range.",
                                idx));
              }
              indices.push_back(static_cast<int32_t>(idx));
            }
          }

          primvar.set_default_indices(indices);

        }

      } else if (indexAttr.is_blocked()) {
        // Value blocked. e.g. `float2[] primvars:st:indices = None`
        // We can simply skip reading indices.
      } else {

        if (!indexAttr.has_value() && !indexAttr.has_timesamples()) {
          // No authored indices value — e.g. the empty `int[] primvars:st:indices`
          // in usd-wg TextureTransformTest. Treat the primvar as un-indexed (use
          // its values directly) instead of failing, matching the blocked-indices
          // case above and OpenUSD.
          DCOUT("primvars:" << varname
                << ":indices declared with no authored value; treating primvar "
                   "as un-indexed.");
          if (warn) {
            (*warn) += fmt::format(
                "`primvars:{}:indices` is declared with no authored value; "
                "treating the primvar as un-indexed.\n", varname);
          }
        }

        if (indexAttr.has_value()) {
          // Check if int[] or uint[] type.
          std::vector<int32_t> indices;
          if (!indexAttr.get_value(&indices)) {
            std::vector<uint32_t> uint_indices;
            if (!indexAttr.get_value(&uint_indices)) {
              PUSH_ERROR_AND_RETURN(
                  fmt::format("Index Attribute is not int[] or uint[] type. Got {}",
                              indexAttr.type_name()));
            }
            indices.reserve(uint_indices.size());
            for (uint32_t idx : uint_indices) {
              if (idx > uint32_t(std::numeric_limits<int32_t>::max())) {
                PUSH_ERROR_AND_RETURN(
                    fmt::format("Index Attribute contains value {} outside int32 range.",
                                idx));
              }
              indices.push_back(static_cast<int32_t>(idx));
            }
          }


          primvar.set_default_indices(indices);
        }

        if (indexAttr.has_timesamples()) {
          primvar.set_timesampled_indices(indexAttr.get_var().ts_raw());
        }

      }
    } else {
      // indices are optional, so ok to skip it.
    }
  }

  (*out_primvar) = primvar;

  return true;
}

bool FindPrimvarWithInheritance(const Stage &stage, const Path &prim_path,
    const std::string &primvar_name, GeomPrimvar *out,
    std::string *err) {
  if (!out) {
    if (err) (*err) = "Output GeomPrimvar is nullptr.\n";
    return false;
  }

  if (!prim_path.is_valid() || !prim_path.is_absolute_path()) {
    if (err) (*err) = "Input path must be a valid absolute path.\n";
    return false;
  }

  Path current = prim_path;

  while (true) {
    auto ret = stage.GetPrimAtPath(current);
    if (!ret) {
      break;
    }

    const Prim *prim = ret.value();
    const GPrim *gprim = nullptr;

    // Try to get GPrim from prim. Use value::TypeTraits approach.
    // GPrim is the base for geometry types, so we need to check the actual type.
    if (auto p = prim->as<GeomMesh>()) {
      gprim = p;
    } else if (auto p2 = prim->as<GeomPoints>()) {
      gprim = p2;
    } else if (auto p3 = prim->as<GeomBasisCurves>()) {
      gprim = p3;
    } else if (auto p4 = prim->as<GeomNurbsCurves>()) {
      gprim = p4;
    } else if (auto p5 = prim->as<GeomSphere>()) {
      gprim = p5;
    } else if (auto p6 = prim->as<GeomCube>()) {
      gprim = p6;
    } else if (auto p7 = prim->as<GeomCone>()) {
      gprim = p7;
    } else if (auto p8 = prim->as<GeomCylinder>()) {
      gprim = p8;
    } else if (auto p9 = prim->as<GeomCapsule>()) {
      gprim = p9;
    } else if (auto p10 = prim->as<Xform>()) {
      gprim = p10;
    }

    if (gprim) {
      GeomPrimvar primvar;
      if (GetGeomPrimvar(stage, gprim, primvar_name, &primvar)) {
        *out = primvar;
        return true;
      }
    }

    if (current.is_root_prim() || current.is_root_path()) {
      break;
    }
    current = current.get_parent_prim_path();
  }

  return false;
}

#undef PushError

}  // namespace tydra
}  // namespace lightusd
