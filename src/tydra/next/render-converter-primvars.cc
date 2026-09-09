// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Mesh primvar extraction

#include "render-converter.hh"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;

namespace {

Interpolation ParsePrimvarInterp(const std::string& s) {
  if (s == "constant") return Interpolation::Constant;
  if (s == "uniform") return Interpolation::Uniform;
  if (s == "faceVarying") return Interpolation::FaceVarying;
  if (s == "varying") return Interpolation::Varying;
  return Interpolation::Vertex;
}

// Flatten a primvar Value into floats (float/half/double backed, any comps).
// Returns comps per element (0 = unsupported/absent).
// `*view` is set to the float array to read from: the SOURCE array itself when
// the primvar is already float-backed (no copy), otherwise `scratch` holding
// the converted values. Copying unconditionally cost a full extra transient of
// every UV/color/normal primvar (40 MB for a 5M-vertex `st` set) on top of the
// copies the caller already makes.
uint32_t PrimvarToFloats(const Value& v, std::vector<float>* scratch,
                         const std::vector<float>** view) {
  if (!v.is_array()) return 0;
  const uint32_t comps =
      static_cast<uint32_t>(GetComponentCount(v.type_id()));
  if (comps == 0) return 0;
  if (const std::vector<float>* fa = v.as_float_array()) {
    *view = fa;
    return comps;
  }
  if (const std::vector<double>* da = v.as_double_array()) {
    scratch->reserve(da->size());
    for (double d : *da) scratch->push_back(static_cast<float>(d));
    *view = scratch;
    return comps;
  }
  return 0;
}

}  // namespace

bool RenderSceneConverter::ExtractMeshPrimvars(const UsdPrim& prim, RenderMesh* mesh) {
  std::vector<Primvar> primvars = GetPrimvars(prim);

  // The primary UV set is the first configured name this mesh actually authors.
  // Without the fallback, a Blender-exported "UVMap" mesh reads as having no UVs
  // at all.
  std::string uv_base;
  for (const std::string& candidate : config_.mesh.uv_primvar_names) {
    for (const Primvar& pv : primvars) {
      if (pv.name == candidate) {
        uv_base = candidate;
        break;
      }
    }
    if (!uv_base.empty()) break;
  }
  if (uv_base.empty()) {
    uv_base = config_.mesh.uv_primvar_names.empty()
                  ? std::string("st")
                  : config_.mesh.uv_primvar_names.front();
  }

  // The SECONDARY UV set. This used to be hard-coded to `uv_base + "1"`, so only
  // st1 / UVMap1 / uv1 were ever extracted -- a texture whose UsdPrimvarReader
  // names `uvSet1`, `map2` or `UVMap.001` (all common) referenced a set the
  // converter never built, and RenderTexture::uv_primvar pointed at nothing.
  //
  // Take any second 2-component primvar instead, preferring the conventional
  // names so existing assets keep their slot assignment: uv_base + "1" first,
  // then the other configured UV names, then any remaining float2 primvar (in
  // name order, so the choice is deterministic rather than dependent on authoring
  // order). Skinning primvars are consumed by the skin binding, not here.
  auto two_component = [&](const std::string& name) -> bool {
    for (const Primvar& pv : primvars) {
      if (pv.name != name || !pv.value || !pv.value->is_array()) continue;
      return GetComponentCount(pv.value->type_id()) == 2;
    }
    return false;
  };
  std::string uv_second;
  if (two_component(uv_base + "1")) {
    uv_second = uv_base + "1";
  }
  if (uv_second.empty()) {
    for (const std::string& candidate : config_.mesh.uv_primvar_names) {
      if (candidate != uv_base && two_component(candidate)) {
        uv_second = candidate;
        break;
      }
    }
  }
  if (uv_second.empty()) {
    std::vector<std::string> others;
    for (const Primvar& pv : primvars) {
      if (pv.name == uv_base || !pv.value || !pv.value->is_array()) continue;
      if (GetComponentCount(pv.value->type_id()) != 2) continue;
      if (pv.name.rfind("skel:", 0) == 0) continue;
      others.push_back(pv.name);
    }
    if (!others.empty()) {
      std::sort(others.begin(), others.end());
      uv_second = others.front();
    }
  }

  const size_t npoints = mesh->point_count();
  const size_t nfaces = mesh->face_count();
  const size_t ncorners = mesh->face_vertex_indices.size();

  auto expected_elems = [&](Interpolation it) -> size_t {
    switch (it) {
      case Interpolation::Constant: return 1;
      case Interpolation::Uniform: return nfaces;
      case Interpolation::FaceVarying: return ncorners;
      case Interpolation::Vertex:
      case Interpolation::Varying:
      default: return npoints;
    }
  };

  // Expand an indexed primvar to direct form; false on any out-of-range index.
  auto expand_indexed = [](const std::vector<float>& data, uint32_t comps,
                           const std::vector<int32_t>& idxs,
                           std::vector<float>* out) -> bool {
    const size_t elems = comps ? data.size() / comps : 0;
    out->clear();
    out->reserve(idxs.size() * comps);
    for (int32_t raw : idxs) {
      if (raw < 0 || static_cast<size_t>(raw) >= elems) return false;
      const float* src = data.data() + static_cast<size_t>(raw) * comps;
      out->insert(out->end(), src, src + comps);
    }
    return true;
  };

  // Authored `normals` attribute (primvars:normals, handled in the loop
  // below, takes precedence per USD).
  {
    ValueArrayRead<float> normals;
    if (ReadFloatArray(prim, "normals", config_.time_code, &normals) &&
        !normals.empty()) {
      std::string interp_tok = "vertex";
      if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
        if (const ::lightusd::next::PropMeta* pm =
                spec->property_meta("normals")) {
          if (pm->authored & ::lightusd::next::PropMeta::kInterpolation) {
            interp_tok = pm->interpolation;
          }
        }
      }
      const Interpolation ni = ParsePrimvarInterp(interp_tok);
      const size_t elems = normals.view.size / 3;
      if (elems == expected_elems(ni)) {
        mesh->normals.append(normals.view.data, normals.view.size);
        mesh->normals_interp = ni;
      } else {
        AddWarning("Mesh '" + mesh->prim_path +
                            "': authored normals element count does not match "
                            "their interpolation; ignoring (normals will be "
                            "computed)");
      }
    }
  }

  for (Primvar& pv : primvars) {
    if (!pv.value) continue;
    // Skinning primvars are consumed by the skin binding (GetSkinBinding), not
    // by the generic vertex-attribute channel. Skip BEFORE flattening: the
    // check used to sit below, after a full copy of the array had been made.
    if (pv.name.rfind("skel:", 0) == 0) continue;

    std::vector<float> data;
    const std::vector<float>* fdata = nullptr;
    const uint32_t comps = PrimvarToFloats(*pv.value, &data, &fdata);
    const bool is_uv0 = (pv.name == uv_base);
    const bool is_uv1 = (!uv_second.empty() && pv.name == uv_second);
    const bool is_color = (pv.name == "displayColor");
    const bool is_opacity = (pv.name == "displayOpacity");
    const bool is_normals = (pv.name == "normals");
    const bool builtin =
        is_uv0 || is_uv1 || is_color || is_opacity || is_normals;

    // Unauthored interpolation defaults to `constant` per the USD spec
    // (pxr UsdGeomPrimvar / legacy GeomPrimvar parity). Unauthored arrays
    // sized per-point/per-corner/per-face are common in the wild though, so
    // infer the mode from the LOGICAL element count for those.
    auto resolve_interp = [&](size_t elems) -> Interpolation {
      if (!pv.interpolation_authored && elems > 1) {
        if (elems == npoints) return Interpolation::Vertex;
        if (elems == ncorners) return Interpolation::FaceVarying;
        if (elems == nfaces) return Interpolation::Uniform;
      }
      return ParsePrimvarInterp(pv.interpolation);
    };

    if (comps == 0) {
      // Non-float primvar: only representable as a generic int attribute.
      if (builtin) continue;
      const std::vector<int32_t>* ia = pv.value->as_int_array();
      if (!ia || ia->empty()) continue;
      VertexAttribute attr;
      attr.name = pv.name;
      attr.format = VertexFormat::Int;
      attr.interpolation = resolve_interp(
          pv.indices().empty() ? ia->size() : pv.indices().size());
      attr.int_data.append(ia->data(), ia->size());
      bool idx_ok = true;
      for (int32_t raw : pv.indices()) {
        if (raw < 0 || static_cast<size_t>(raw) >= ia->size()) {
          idx_ok = false;
          break;
        }
        attr.indices.push_back(static_cast<uint32_t>(raw));
      }
      if (!idx_ok) {
        AddWarning("Mesh '" + mesh->prim_path + "': primvar '" +
                            pv.name + "' has out-of-range indices; dropped");
        continue;
      }
      mesh->primvars.push_back(std::move(attr));
      continue;
    }

    // Indexed builtin primvars are expanded to direct form (the builtin
    // buffers carry no index channel).
    if (!pv.indices().empty() && builtin) {
      std::vector<float> expanded;
      if (!expand_indexed(*fdata, comps, pv.indices(), &expanded)) {
        AddWarning("Mesh '" + mesh->prim_path + "': primvar '" +
                            pv.name + "' has out-of-range indices; dropped");
        continue;
      }
      data = std::move(expanded);
      fdata = &data;
    }

    if (builtin) {
      // Size must match the declared interpolation or a consumer indexes OOB.
      const size_t elems = fdata->size() / comps;
      const Interpolation interp = resolve_interp(elems);
      if (elems != expected_elems(interp)) {
        AddWarning(
            "Mesh '" + mesh->prim_path + "': primvar '" + pv.name +
            "' element count does not match its interpolation; dropped");
        continue;
      }
      if (is_uv0 && comps == 2) {
        mesh->texcoords_0.append(fdata->data(), fdata->size());
        mesh->texcoords_0_interp = interp;
        mesh->texcoords_0_name = pv.name;
      } else if (is_uv1 && comps == 2) {
        mesh->texcoords_1.append(fdata->data(), fdata->size());
        mesh->texcoords_1_interp = interp;
        mesh->texcoords_1_name = pv.name;
      } else if (is_color && (comps == 3 || comps == 4)) {
        mesh->colors.append(fdata->data(), fdata->size());
        mesh->colors_interp = interp;
      } else if (is_opacity && comps == 1) {
        // displayOpacity as a render channel (legacy exposes it alongside
        // displayColor; consumers combine it as the vertex-color alpha).
        mesh->opacities.append(fdata->data(), fdata->size());
        mesh->opacities_interp = interp;
      } else if (is_normals && comps == 3) {
        // primvars:normals takes precedence over the raw `normals` attribute.
        mesh->normals.clear();
        mesh->normals.append(fdata->data(), fdata->size());
        mesh->normals_interp = interp;
      }
      continue;
    }

    // Generic primvar: keep indices as an index channel (validated).
    VertexAttribute attr;
    attr.name = pv.name;
    attr.format = comps == 1   ? VertexFormat::Float
                  : comps == 2 ? VertexFormat::Vec2
                  : comps == 3 ? VertexFormat::Vec3
                  : comps == 4 ? VertexFormat::Vec4
                  : comps == 9 ? VertexFormat::Matrix33
                                : comps == 16 ? VertexFormat::Matrix44
                                              : VertexFormat::Vec4;
    if (comps != 1 && comps != 2 && comps != 3 && comps != 4 &&
        comps != 9 && comps != 16) continue;
    attr.interpolation = resolve_interp(
        pv.indices().empty() ? (fdata->size() / comps) : pv.indices().size());
    attr.float_data.append(fdata->data(), fdata->size());
    bool idx_ok = true;
    const size_t elems = fdata->size() / comps;
    for (int32_t raw : pv.indices()) {
      if (raw < 0 || static_cast<size_t>(raw) >= elems) {
        idx_ok = false;
        break;
      }
      attr.indices.push_back(static_cast<uint32_t>(raw));
    }
    if (!idx_ok) {
      AddWarning("Mesh '" + mesh->prim_path + "': primvar '" +
                          pv.name + "' has out-of-range indices; dropped");
      continue;
    }
    mesh->primvars.push_back(std::move(attr));
  }

  return true;
}


} } }  // namespace lightusd::tydra::next
