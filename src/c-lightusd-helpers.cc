// SPDX-License-Identifier: Apache 2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Small C-callable helpers that complement c-lightusd.cc for the Python
// extension. Kept in a separate translation unit so the header surface for
// the Python extension stays tidy.

#include "c-lightusd-helpers.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lightusd.hh"
#include "tydra/scene-access.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"
#include "value-types.hh"
#include "core/model-scope.hh"
#include "usdGeom.hh"
#include "usdPhysics.hh"
#include "usdSkel.hh"
#include "usdShade.hh"
#include "pprint-enum.hh"
#include "pprinter.hh"

namespace {

using lightusd::Attribute;
using lightusd::Path;
using lightusd::Prim;
using lightusd::Property;
using lightusd::Relationship;
using lightusd::Stage;

inline const Stage *S(const CLightUSDStage *s) {
  return reinterpret_cast<const Stage *>(s);
}
inline Stage *Sm(CLightUSDStage *s) { return reinterpret_cast<Stage *>(s); }
inline const Prim *P(const CLightUSDPrim *p) {
  return reinterpret_cast<const Prim *>(p);
}
inline const Attribute *A(const CLightUSDAttribute *a) {
  return reinterpret_cast<const Attribute *>(a);
}
inline Attribute *Am(CLightUSDAttribute *a) {
  return reinterpret_cast<Attribute *>(a);
}
inline const lightusd::value::Value *V(const CLightUSDValue *v) {
  return reinterpret_cast<const lightusd::value::Value *>(v);
}

inline bool set_err(c_lightusd_string_t *err, const std::string &msg) {
  if (err) c_lightusd_string_replace(err, msg.c_str());
  return false;
}

//---------------------------------------------------------------------------
// Zero-copy array access.
//
// For each supported USD value type, we produce:
//   - a pointer into the underlying std::vector<T> storage
//   - the outer element count (N)
//   - the number of inner components per element (1 for scalar arrays,
//     2/3/4/16 for vectors, quats, matrices)
//   - the size of one inner component, in bytes
//   - a single-char Python struct format for the inner component
//
// The Python buffer-protocol path in module.c assembles shape/strides from
// those numbers. This lets us expose a `point3f[]` as a 2-D (N, 3) float32
// buffer with zero copies.
//
// Role types (point3f, color3f, normal3f, texCoord2f, etc.) are POD wrappers
// around 2/3/4 packed floats/doubles/halves; their std::vector<> data() is
// bit-compatible with a flat float[]/double[]/half[] of size N*K.
//
// std::vector<bool> is specialised and has no contiguous data(), so we skip
// it; bool[] can still be read via scalar value-conversion APIs.
//---------------------------------------------------------------------------

struct ArrayView {
  const void *ptr;
  uint64_t n_outer;
  uint32_t n_inner;
  uint32_t component_size;
  const char *format;
};

// Returns true if `v` holds a std::vector<T> and fills `out`.
template <typename T>
inline bool try_vector_raw(const lightusd::value::any_value *av,
                           const void **ptr, uint64_t *n) {
  auto pv = lightusd::value::any_value_cast<const std::vector<T>>(av);
  if (!pv) return false;
  *ptr = static_cast<const void *>(pv->data());
  *n = static_cast<uint64_t>(pv->size());
  return true;
}

template <typename T>
inline bool plain_scalar_array(const lightusd::value::any_value *av,
                               ArrayView *out, uint32_t comp_size,
                               const char *fmt) {
  const void *ptr = nullptr;
  uint64_t n = 0;
  if (!try_vector_raw<T>(av, &ptr, &n)) return false;
  out->ptr = ptr;
  out->n_outer = n;
  out->n_inner = 1;
  out->component_size = comp_size;
  out->format = fmt;
  return true;
}

template <typename T>
inline bool role_array(const lightusd::value::any_value *av, ArrayView *out,
                       uint32_t n_inner, uint32_t comp_size,
                       const char *fmt) {
  const void *ptr = nullptr;
  uint64_t n = 0;
  if (!try_vector_raw<T>(av, &ptr, &n)) return false;
  out->ptr = ptr;
  out->n_outer = n;
  out->n_inner = n_inner;
  out->component_size = comp_size;
  out->format = fmt;
  return true;
}

bool resolve_array_view(const lightusd::value::Value *v, ArrayView *out) {
  using namespace lightusd::value;
  if (!v->is_array()) return false;
  const any_value *av = &v->get_raw();

  uint32_t tyid = av->type_id() & (~TYPE_ID_1D_ARRAY_BIT);

  // Plain scalar element types (value type == element type with 1D bit on).
  switch (tyid) {
    case TYPE_ID_INT32:
      return plain_scalar_array<int32_t>(av, out, 4, "i");
    case TYPE_ID_UINT32:
      return plain_scalar_array<uint32_t>(av, out, 4, "I");
    case TYPE_ID_INT64:
      return plain_scalar_array<int64_t>(av, out, 8, "q");
    case TYPE_ID_UINT64:
      return plain_scalar_array<uint64_t>(av, out, 8, "Q");
    case TYPE_ID_FLOAT:
      return plain_scalar_array<float>(av, out, 4, "f");
    case TYPE_ID_DOUBLE:
      return plain_scalar_array<double>(av, out, 8, "d");
    case TYPE_ID_HALF:
      return plain_scalar_array<half>(av, out, 2, "e");
    default:
      break;
  }

  // Role / composite element types. We list every type the core supports so
  // the Python layer gets zero-copy NumPy access for all primvar-worthy USD
  // attributes.
  if (role_array<int2>   (av, out, 2, 4, "i")) return true;
  if (role_array<int3>   (av, out, 3, 4, "i")) return true;
  if (role_array<int4>   (av, out, 4, 4, "i")) return true;
  if (role_array<uint2>  (av, out, 2, 4, "I")) return true;
  if (role_array<uint3>  (av, out, 3, 4, "I")) return true;
  if (role_array<uint4>  (av, out, 4, 4, "I")) return true;

  if (role_array<half2>  (av, out, 2, 2, "e")) return true;
  if (role_array<half3>  (av, out, 3, 2, "e")) return true;
  if (role_array<half4>  (av, out, 4, 2, "e")) return true;

  if (role_array<float2> (av, out, 2, 4, "f")) return true;
  if (role_array<float3> (av, out, 3, 4, "f")) return true;
  if (role_array<float4> (av, out, 4, 4, "f")) return true;

  if (role_array<double2>(av, out, 2, 8, "d")) return true;
  if (role_array<double3>(av, out, 3, 8, "d")) return true;
  if (role_array<double4>(av, out, 4, 8, "d")) return true;

  // Quaternions: imag[3] + real = 4 scalars.
  if (role_array<quath>  (av, out, 4, 2, "e")) return true;
  if (role_array<quatf>  (av, out, 4, 4, "f")) return true;
  if (role_array<quatd>  (av, out, 4, 8, "d")) return true;

  // Colors.
  if (role_array<color3h>(av, out, 3, 2, "e")) return true;
  if (role_array<color3f>(av, out, 3, 4, "f")) return true;
  if (role_array<color3d>(av, out, 3, 8, "d")) return true;
  if (role_array<color4h>(av, out, 4, 2, "e")) return true;
  if (role_array<color4f>(av, out, 4, 4, "f")) return true;
  if (role_array<color4d>(av, out, 4, 8, "d")) return true;

  // Points / normals / vectors / tex-coords.
  if (role_array<point3h>(av, out, 3, 2, "e")) return true;
  if (role_array<point3f>(av, out, 3, 4, "f")) return true;
  if (role_array<point3d>(av, out, 3, 8, "d")) return true;
  if (role_array<normal3h>(av, out, 3, 2, "e")) return true;
  if (role_array<normal3f>(av, out, 3, 4, "f")) return true;
  if (role_array<normal3d>(av, out, 3, 8, "d")) return true;
  if (role_array<vector3h>(av, out, 3, 2, "e")) return true;
  if (role_array<vector3f>(av, out, 3, 4, "f")) return true;
  if (role_array<vector3d>(av, out, 3, 8, "d")) return true;
  if (role_array<texcoord2h>(av, out, 2, 2, "e")) return true;
  if (role_array<texcoord2f>(av, out, 2, 4, "f")) return true;
  if (role_array<texcoord2d>(av, out, 2, 8, "d")) return true;
  if (role_array<texcoord3h>(av, out, 3, 2, "e")) return true;
  if (role_array<texcoord3f>(av, out, 3, 4, "f")) return true;
  if (role_array<texcoord3d>(av, out, 3, 8, "d")) return true;

  // Matrices (exposed as flat component arrays; Python can reshape).
  if (role_array<matrix2d>(av, out, 4, 8, "d")) return true;
  if (role_array<matrix3d>(av, out, 9, 8, "d")) return true;
  if (role_array<matrix4d>(av, out, 16, 8, "d")) return true;

  return false;
}

}  // namespace

extern "C" {

/* ---- Stage ---- */

uint64_t c_lightusd_stage_num_root_prims(const CLightUSDStage *stage) {
  if (!stage) return 0;
  return static_cast<uint64_t>(S(stage)->root_prims().size());
}

int c_lightusd_stage_get_root_prim(const CLightUSDStage *stage, uint64_t index,
                                  const CLightUSDPrim **out) {
  if (!stage || !out) return 0;
  const auto &roots = S(stage)->root_prims();
  if (index >= roots.size()) return 0;
  *out = reinterpret_cast<const CLightUSDPrim *>(&roots[index]);
  return 1;
}

int c_lightusd_stage_get_prim_at_path(const CLightUSDStage *stage,
                                     const char *abs_path,
                                     const CLightUSDPrim **out,
                                     c_lightusd_string_t *err) {
  if (!stage || !abs_path || !out) return 0;
  Path p(abs_path, "");
  auto r = S(stage)->GetPrimAtPath(p);
  if (!r) {
    set_err(err, r.error());
    return 0;
  }
  *out = reinterpret_cast<const CLightUSDPrim *>(r.value());
  return 1;
}

int c_lightusd_stage_save_to_file(const CLightUSDStage *stage,
                                 const char *filename, CLightUSDFormat format,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err) {
  if (!stage || !filename) return 0;
  const Stage *s = S(stage);
  std::string w, e;

  if (format == C_LIGHTUSD_FORMAT_AUTO || format == C_LIGHTUSD_FORMAT_UNKNOWN) {
    std::string fn(filename);
    auto pos = fn.find_last_of('.');
    std::string ext = (pos == std::string::npos) ? "" : fn.substr(pos + 1);
    for (auto &c : ext) c = static_cast<char>(::tolower(c));
    if (ext == "usda") format = C_LIGHTUSD_FORMAT_USDA;
    else if (ext == "usdc") format = C_LIGHTUSD_FORMAT_USDC;
    else if (ext == "usdz") format = C_LIGHTUSD_FORMAT_USDZ;
    else format = C_LIGHTUSD_FORMAT_USDA;
  }

  bool ok = false;
  switch (format) {
    case C_LIGHTUSD_FORMAT_USDA:
      ok = lightusd::usda::SaveAsUSDA(filename, *s, &w, &e);
      break;
    case C_LIGHTUSD_FORMAT_USDC:
      ok = lightusd::usdc::SaveAsUSDCToFile(filename, *s, &w, &e);
      break;
    case C_LIGHTUSD_FORMAT_USDZ: {
      const std::map<std::string, std::vector<uint8_t>> assets;
      ok = lightusd::SaveAsUSDZToFile(filename, *s, assets, &w, &e);
      break;
    }
    default:
      e = "Unknown/unsupported format for save.";
      break;
  }

  if (warn && !w.empty()) c_lightusd_string_replace(warn, w.c_str());
  if (err && !e.empty()) c_lightusd_string_replace(err, e.c_str());
  return ok ? 1 : 0;
}

int c_lightusd_stage_save_as_usdz_with_assets(
    const CLightUSDStage *stage, const char *filename,
    const char *const *asset_names,
    const uint8_t *const *asset_data,
    const uint64_t *asset_sizes,
    uint64_t num_assets,
    c_lightusd_string_t *warn,
    c_lightusd_string_t *err) {
  if (!stage || !filename) {
    if (err) c_lightusd_string_replace(err, "stage or filename is null");
    return 0;
  }
  const lightusd::Stage *s =
      reinterpret_cast<const lightusd::Stage *>(stage);

  std::map<std::string, std::vector<uint8_t>> assets;
  for (uint64_t i = 0; i < num_assets; ++i) {
    if (!asset_names || !asset_names[i] || !asset_data || !asset_data[i] ||
        !asset_sizes) {
      if (err) c_lightusd_string_replace(
          err, "asset_names / asset_data / asset_sizes contain a null entry");
      return 0;
    }
    const char *name = asset_names[i];
    if (!*name) {
      if (err) c_lightusd_string_replace(err, "asset name must be non-empty");
      return 0;
    }
    std::vector<uint8_t> buf(asset_data[i], asset_data[i] + asset_sizes[i]);
    assets[std::string(name)] = std::move(buf);
  }

  std::string w, e;
  bool ok = lightusd::SaveAsUSDZToFile(filename, *s, assets, &w, &e);
  if (warn && !w.empty()) c_lightusd_string_replace(warn, w.c_str());
  if (err && !e.empty()) c_lightusd_string_replace(err, e.c_str());
  return ok ? 1 : 0;
}

namespace {

// Lightweight format sniff from the first bytes of a buffer. The C-API
// header declares c_lightusd_is_{usda,usdc,usdz}_memory but c-lightusd.cc
// leaves them unimplemented; we avoid them and sniff locally.
inline CLightUSDFormat sniff_memory_format(const uint8_t *data, size_t n) {
  if (n >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 &&
      data[3] == 0x04) {
    return C_LIGHTUSD_FORMAT_USDZ;  // ZIP magic
  }
  if (n >= 8 && data[0] == 'P' && data[1] == 'X' && data[2] == 'R' &&
      data[3] == '-' && data[4] == 'U' && data[5] == 'S' && data[6] == 'D' &&
      data[7] == 'C') {
    return C_LIGHTUSD_FORMAT_USDC;  // "PXR-USDC" crate magic
  }
  if (n >= 6 && data[0] == '#' && data[1] == 'u' && data[2] == 's' &&
      data[3] == 'd' && data[4] == 'a') {
    return C_LIGHTUSD_FORMAT_USDA;  // "#usda" header
  }
  return C_LIGHTUSD_FORMAT_UNKNOWN;
}

}  // namespace

int c_lightusd_stage_load_from_memory(CLightUSDStage *stage,
                                     const uint8_t *data, size_t nbytes,
                                     CLightUSDFormat format,
                                     c_lightusd_string_t *warn,
                                     c_lightusd_string_t *err) {
  if (!stage || !data || nbytes == 0) return 0;
  std::string w, e;
  bool ok = false;

  CLightUSDFormat fmt = format;
  if (fmt == C_LIGHTUSD_FORMAT_AUTO || fmt == C_LIGHTUSD_FORMAT_UNKNOWN) {
    fmt = sniff_memory_format(data, nbytes);
    if (fmt == C_LIGHTUSD_FORMAT_UNKNOWN) {
      set_err(err, "Unable to detect USD format from memory buffer.");
      return 0;
    }
  }

  Stage *s = Sm(stage);
  switch (fmt) {
    case C_LIGHTUSD_FORMAT_USDA:
      ok = lightusd::LoadUSDAFromMemory(data, nbytes, /*filename*/"", s, &w, &e);
      break;
    case C_LIGHTUSD_FORMAT_USDC:
      ok = lightusd::LoadUSDCFromMemory(data, nbytes, /*filename*/"", s, &w, &e);
      break;
    case C_LIGHTUSD_FORMAT_USDZ:
      ok = lightusd::LoadUSDZFromMemory(data, nbytes, /*filename*/"", s, &w, &e);
      break;
    default:
      e = "Unsupported format for load-from-memory.";
      break;
  }

  if (warn && !w.empty()) c_lightusd_string_replace(warn, w.c_str());
  if (err && !e.empty()) c_lightusd_string_replace(err, e.c_str());
  return ok ? 1 : 0;
}

/* ---- Prim ---- */

/* For Python-authored prims, the attribute is stored in the typed prim's
 * `props` map. tydra::GetProperty reads builtin attribute names from the
 * typed schema field first (which is unauthored for our path) and never
 * falls back to props for those names — so prefer props when present.
 *
 * Returns true if the lookup hit the props map and filled *out.
 */
static bool lookup_in_props(const Prim &prim, const std::string &name,
                            Property *out) {
  using namespace lightusd;
  uint32_t tid = prim.data().type_id();
#define LOOK(__TID, __TY)                                            \
  case __TID: {                                                      \
    const __TY *typed = prim.data().as<__TY>();                      \
    if (!typed) return false;                                        \
    auto it = typed->props.find(name);                               \
    if (it == typed->props.end()) return false;                      \
    *out = it->second;                                               \
    return true;                                                     \
  }
  switch (tid) {
    LOOK(value::TYPE_ID_GEOM_XFORM,    Xform)
    LOOK(value::TYPE_ID_GEOM_MESH,     GeomMesh)
    LOOK(value::TYPE_ID_GEOM_SPHERE,   GeomSphere)
    LOOK(value::TYPE_ID_GEOM_CUBE,     GeomCube)
    LOOK(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
    LOOK(value::TYPE_ID_GEOM_CONE,     GeomCone)
    LOOK(value::TYPE_ID_GEOM_CAPSULE,  GeomCapsule)
    LOOK(value::TYPE_ID_GEOM_CAMERA,   GeomCamera)
    LOOK(value::TYPE_ID_GEOM_POINTS,   GeomPoints)
    LOOK(value::TYPE_ID_GEOM_GEOMSUBSET,      GeomSubset)
    LOOK(value::TYPE_ID_GEOM_BASIS_CURVES,    GeomBasisCurves)
    LOOK(value::TYPE_ID_GEOM_NURBS_CURVES,    GeomNurbsCurves)
    LOOK(value::TYPE_ID_GEOM_HERMITE_CURVES,  GeomHermiteCurves)
    LOOK(value::TYPE_ID_GEOM_PLANE,           GeomPlane)
    LOOK(value::TYPE_ID_GEOM_CYLINDER_1,      GeomCylinder_1)
    LOOK(value::TYPE_ID_GEOM_CAPSULE_1,       GeomCapsule_1)
    LOOK(value::TYPE_ID_GEOM_TET_MESH,        GeomTetMesh)
    LOOK(value::TYPE_ID_GEOM_NURBS_PATCH,     GeomNurbsPatch)
    LOOK(value::TYPE_ID_GEOM_POINT_INSTANCER, GeomPointInstancer)
    LOOK(value::TYPE_ID_LUX_SPHERE,    SphereLight)
    LOOK(value::TYPE_ID_LUX_RECT,      RectLight)
    LOOK(value::TYPE_ID_LUX_DISK,      DiskLight)
    LOOK(value::TYPE_ID_LUX_DISTANT,   DistantLight)
    LOOK(value::TYPE_ID_LUX_CYLINDER,  CylinderLight)
    LOOK(value::TYPE_ID_LUX_DOME,      DomeLight)
    LOOK(value::TYPE_ID_LUX_DOME_1,    DomeLight_1)
    LOOK(value::TYPE_ID_LUX_GEOMETRY,  GeometryLight)
    LOOK(value::TYPE_ID_LUX_PORTAL,    PortalLight)
    LOOK(value::TYPE_ID_SCOPE,         Scope)
    LOOK(value::TYPE_ID_MODEL,         Model)
    LOOK(value::TYPE_ID_PHYSICS_SCENE,            PhysicsScene)
    LOOK(value::TYPE_ID_PHYSICS_JOINT,            PhysicsJoint)
    LOOK(value::TYPE_ID_PHYSICS_REVOLUTE_JOINT,   PhysicsRevoluteJoint)
    LOOK(value::TYPE_ID_PHYSICS_PRISMATIC_JOINT,  PhysicsPrismaticJoint)
    LOOK(value::TYPE_ID_PHYSICS_SPHERICAL_JOINT,  PhysicsSphericalJoint)
    LOOK(value::TYPE_ID_PHYSICS_FIXED_JOINT,      PhysicsFixedJoint)
    LOOK(value::TYPE_ID_PHYSICS_DISTANCE_JOINT,   PhysicsDistanceJoint)
    LOOK(value::TYPE_ID_PHYSICS_COLLISION_GROUP,  PhysicsCollisionGroup)
    LOOK(value::TYPE_ID_MATERIAL,                 Material)
    case value::TYPE_ID_SHADER: {
      const Shader *typed = prim.data().as<Shader>();
      if (!typed) return false;
      auto it = typed->props.find(name);
      if (it != typed->props.end()) {
        *out = it->second;
        return true;
      }
      // Generic Shader: ShaderNode value carries the props.
      if (const auto pnode = typed->value.as<ShaderNode>()) {
        auto it2 = pnode->props.find(name);
        if (it2 != pnode->props.end()) {
          *out = it2->second;
          return true;
        }
      }
      return false;
    }
    LOOK(value::TYPE_ID_NODEGRAPH,                NodeGraph)
    LOOK(value::TYPE_ID_SKEL_ROOT,                SkelRoot)
    LOOK(value::TYPE_ID_SKELETON,                 Skeleton)
    LOOK(value::TYPE_ID_SKELANIMATION,            SkelAnimation)
    LOOK(value::TYPE_ID_BLENDSHAPE,               BlendShape)
    default: return false;
  }
#undef LOOK
}

}  /* end extern "C" — templates need C++ linkage */

/* For UsdPhysics prim types, tydra::GetProperty has no specialization,
 * so reads of schema-builtin attributes (like physics:gravityDirection on
 * PhysicsScene) return false. Fill in the gap by reading the typed
 * builtin fields directly. Returns true if filled. */
template <typename T>
static bool fill_attr_from_typed(const T &input, const std::string &type_name,
                                 const std::string &attr_name,
                                 Property *out) {
  if (!input.authored()) return false;
  Attribute attr;
  attr.set_name(attr_name);
  attr.set_type_name(type_name);
  if (auto pv = input.get_value()) {
    lightusd::value::Value val(pv.value());
    lightusd::primvar::PrimVar pvar;
    pvar.set_value(val);
    attr.set_var(std::move(pvar));
  } else {
    return false;
  }
  *out = Property(std::move(attr), /* custom */ false);
  return true;
}

// TypedAttributeWithFallback variant: get_value() returns `const T&`, not
// optional, and the value should only be exposed when actually authored.
template <typename T>
static bool fill_attr_from_typed_fb(
    const lightusd::TypedAttributeWithFallback<T> &input,
    const std::string &type_name, const std::string &attr_name,
    Property *out) {
  if (!input.authored()) return false;
  Attribute attr;
  attr.set_name(attr_name);
  attr.set_type_name(type_name);
  lightusd::value::Value val(input.get_value());
  lightusd::primvar::PrimVar pvar;
  pvar.set_value(val);
  attr.set_var(std::move(pvar));
  *out = Property(std::move(attr), /* custom */ false);
  return true;
}

// Convert an enum-typed TypedAttributeWithFallback<E> to a token Property
// using a stringifier (e.g. lightusd::to_string(E)).
template <typename E, typename Stringify>
static bool fill_attr_from_typed_enum_token(
    const lightusd::TypedAttributeWithFallback<E> &input,
    const std::string &attr_name, Stringify s, Property *out) {
  if (!input.authored()) return false;
  Attribute attr;
  attr.set_name(attr_name);
  attr.set_type_name("token");
  lightusd::value::token tok(s(input.get_value()));
  lightusd::value::Value val(tok);
  lightusd::primvar::PrimVar pvar;
  pvar.set_value(val);
  attr.set_var(std::move(pvar));
  *out = Property(std::move(attr), /* custom */ false);
  return true;
}

static bool lookup_geom_typed(const Prim &prim, const std::string &name,
                              Property *out) {
  using namespace lightusd;
  uint32_t tid = prim.data().type_id();
  if (tid == value::TYPE_ID_GEOM_BASIS_CURVES) {
    const auto *c = prim.data().as<GeomBasisCurves>();
    if (!c) return false;
    if (name == "type")
      return fill_attr_from_typed_enum_token(
          c->type, name,
          [](const GeomBasisCurves::Type &v) { return lightusd::to_string(v); },
          out);
    if (name == "basis")
      return fill_attr_from_typed_enum_token(
          c->basis, name,
          [](const GeomBasisCurves::Basis &v) { return lightusd::to_string(v); },
          out);
    if (name == "wrap")
      return fill_attr_from_typed_enum_token(
          c->wrap, name,
          [](const GeomBasisCurves::Wrap &v) { return lightusd::to_string(v); },
          out);
  }
  return false;
}

static bool lookup_shade_typed(const Prim &prim, const std::string &name,
                               Property *out) {
  using namespace lightusd;
  uint32_t tid = prim.data().type_id();
  if (tid == value::TYPE_ID_MATERIAL) {
    const auto *m = prim.data().as<Material>();
    if (!m) return false;
    if (m->materialXConfig.has_value()) {
      const auto &cfg = m->materialXConfig.value();
      if (name == "config:mtlx:version")
        return fill_attr_from_typed_fb(cfg.mtlx_version, "string", name, out);
      if (name == "config:mtlx:namespace")
        return fill_attr_from_typed_fb(cfg.mtlx_namespace, "string", name, out);
      if (name == "config:mtlx:colorspace")
        return fill_attr_from_typed_fb(cfg.mtlx_colorspace, "string", name, out);
      if (name == "config:mtlx:sourceUri")
        return fill_attr_from_typed_fb(cfg.mtlx_sourceUri, "string", name, out);
    }
    if (m->materialUEConfig.has_value()) {
      const auto &cfg = m->materialUEConfig.value();
      if (name == "config:unreal:version")
        return fill_attr_from_typed_fb(cfg.version, "string", name, out);
      if (name == "config:unreal:engineVersion")
        return fill_attr_from_typed_fb(cfg.engineVersion, "string", name, out);
      if (name == "config:unreal:sourceAsset")
        return fill_attr_from_typed_fb(cfg.sourceAsset, "string", name, out);
      if (name == "config:unreal:graphAuthority")
        return fill_attr_from_typed_fb(cfg.graphAuthority, "string", name, out);
      if (name == "config:unreal:materialDomain")
        return fill_attr_from_typed_fb(cfg.materialDomain, "string", name, out);
      if (name == "config:unreal:blendMode")
        return fill_attr_from_typed_fb(cfg.blendMode, "string", name, out);
      if (name == "config:unreal:shadingModels")
        return fill_attr_from_typed_fb(cfg.shadingModels, "string", name, out);
      if (name == "config:unreal:twoSided")
        return fill_attr_from_typed_fb(cfg.twoSided, "bool", name, out);
      if (name == "config:unreal:opacityMaskClipValue")
        return fill_attr_from_typed_fb(cfg.opacityMaskClipValue, "float", name, out);
      if (name == "config:unreal:propertyArchive")
        return fill_attr_from_typed_fb(cfg.propertyArchive, "string", name, out);
    }
  }
  return false;
}

static bool lookup_physics_typed(const Prim &prim, const std::string &name,
                                 Property *out) {
  using namespace lightusd;
  uint32_t tid = prim.data().type_id();
  if (tid == value::TYPE_ID_PHYSICS_SCENE) {
    const auto *s = prim.data().as<PhysicsScene>();
    if (!s) return false;
    if (name == "physics:gravityDirection")
      return fill_attr_from_typed(s->gravityDirection, "vector3f", name, out);
    if (name == "physics:gravityMagnitude")
      return fill_attr_from_typed(s->gravityMagnitude, "float", name, out);
    return false;
  }
  // Joint base attributes apply to all PhysicsJoint* types.
  const PhysicsJointBase *jb = nullptr;
  switch (tid) {
    case value::TYPE_ID_PHYSICS_JOINT:
      jb = prim.data().as<PhysicsJoint>(); break;
    case value::TYPE_ID_PHYSICS_REVOLUTE_JOINT:
      jb = prim.data().as<PhysicsRevoluteJoint>(); break;
    case value::TYPE_ID_PHYSICS_PRISMATIC_JOINT:
      jb = prim.data().as<PhysicsPrismaticJoint>(); break;
    case value::TYPE_ID_PHYSICS_SPHERICAL_JOINT:
      jb = prim.data().as<PhysicsSphericalJoint>(); break;
    case value::TYPE_ID_PHYSICS_FIXED_JOINT:
      jb = prim.data().as<PhysicsFixedJoint>(); break;
    case value::TYPE_ID_PHYSICS_DISTANCE_JOINT:
      jb = prim.data().as<PhysicsDistanceJoint>(); break;
    default: return false;
  }
  if (!jb) return false;
  if (name == "physics:localPos0")
    return fill_attr_from_typed(jb->localPos0, "point3f", name, out);
  if (name == "physics:localPos1")
    return fill_attr_from_typed(jb->localPos1, "point3f", name, out);
  if (name == "physics:localRot0")
    return fill_attr_from_typed(jb->localRot0, "quatf", name, out);
  if (name == "physics:localRot1")
    return fill_attr_from_typed(jb->localRot1, "quatf", name, out);
  if (name == "physics:jointEnabled")
    return fill_attr_from_typed(jb->jointEnabled, "bool", name, out);
  if (name == "physics:collisionEnabled")
    return fill_attr_from_typed(jb->collisionEnabled, "bool", name, out);
  if (name == "physics:breakForce")
    return fill_attr_from_typed(jb->breakForce, "float", name, out);
  if (name == "physics:breakTorque")
    return fill_attr_from_typed(jb->breakTorque, "float", name, out);
  if (name == "physics:excludeFromArticulation")
    return fill_attr_from_typed(jb->excludeFromArticulation, "bool", name, out);
  // Joint-type-specific typed attributes.
  if (tid == value::TYPE_ID_PHYSICS_REVOLUTE_JOINT) {
    const auto *r = prim.data().as<PhysicsRevoluteJoint>();
    if (!r) return false;
    if (name == "physics:axis")
      return fill_attr_from_typed(r->axis, "token", name, out);
    if (name == "physics:lowerLimit")
      return fill_attr_from_typed(r->lowerLimit, "float", name, out);
    if (name == "physics:upperLimit")
      return fill_attr_from_typed(r->upperLimit, "float", name, out);
  } else if (tid == value::TYPE_ID_PHYSICS_PRISMATIC_JOINT) {
    const auto *r = prim.data().as<PhysicsPrismaticJoint>();
    if (!r) return false;
    if (name == "physics:axis")
      return fill_attr_from_typed(r->axis, "token", name, out);
    if (name == "physics:lowerLimit")
      return fill_attr_from_typed(r->lowerLimit, "float", name, out);
    if (name == "physics:upperLimit")
      return fill_attr_from_typed(r->upperLimit, "float", name, out);
  } else if (tid == value::TYPE_ID_PHYSICS_SPHERICAL_JOINT) {
    const auto *r = prim.data().as<PhysicsSphericalJoint>();
    if (!r) return false;
    if (name == "physics:axis")
      return fill_attr_from_typed(r->axis, "token", name, out);
    if (name == "physics:coneAngle0Limit")
      return fill_attr_from_typed(r->coneAngle0Limit, "float", name, out);
    if (name == "physics:coneAngle1Limit")
      return fill_attr_from_typed(r->coneAngle1Limit, "float", name, out);
  } else if (tid == value::TYPE_ID_PHYSICS_DISTANCE_JOINT) {
    const auto *r = prim.data().as<PhysicsDistanceJoint>();
    if (!r) return false;
    if (name == "physics:minDistance")
      return fill_attr_from_typed(r->minDistance, "float", name, out);
    if (name == "physics:maxDistance")
      return fill_attr_from_typed(r->maxDistance, "float", name, out);
  }
  return false;
}

extern "C" {  /* re-open for the rest of the C-callable helpers */

CLightUSDAttribute *
c_lightusd_prim_get_attribute(const CLightUSDPrim *prim, const char *name) {
  if (!prim || !name) return nullptr;
  Property prop;
  bool got = false;

  // 1) Prefer props map (covers Python-authored attributes).
  if (lookup_in_props(*P(prim), std::string(name), &prop)) {
    if (prop.is_attribute() && prop.get_attribute().has_value()) {
      got = true;
    }
  }

  // 2) Physics typed-builtin fallback (UsdPhysics has no tydra
  //    GetPrimProperty specialization).
  if (!got) {
    if (lookup_physics_typed(*P(prim), std::string(name), &prop)) {
      got = true;
    }
  }

  // 2b) Material/MaterialXConfigAPI typed-builtin fallback.
  if (!got) {
    if (lookup_shade_typed(*P(prim), std::string(name), &prop)) {
      got = true;
    }
  }

  // 2c) UsdGeom typed-builtin fallback for enum-typed token attributes
  //     (BasisCurves type/basis/wrap, ...) that tydra::GetProperty does
  //     not handle for non-Mesh geoms.
  if (!got) {
    if (lookup_geom_typed(*P(prim), std::string(name), &prop)) {
      got = true;
    }
  }

  // 3) Fall back to tydra (covers schema-builtin attributes parsed from
  //    files where the typed field is populated).
  if (!got) {
    std::string err;
    if (!lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop,
                                      &err)) {
      return nullptr;
    }
  }
  if (!prop.is_attribute()) return nullptr;
  Attribute *attr = new Attribute(prop.get_attribute());
  if (attr->name().empty()) attr->set_name(std::string(name));
  return reinterpret_cast<CLightUSDAttribute *>(attr);
}

CLightUSDRelationship *
c_lightusd_prim_get_relationship(const CLightUSDPrim *prim, const char *name) {
  if (!prim || !name) return nullptr;
  Property prop;
  std::string err;
  if (!lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop, &err)) {
    return nullptr;
  }
  if (!prop.is_relationship()) return nullptr;
  Relationship *rel = new Relationship(prop.get_relationship());
  return reinterpret_cast<CLightUSDRelationship *>(rel);
}

/* ---- Attribute ---- */

CLightUSDAttribute *c_lightusd_attribute_new(void) {
  return reinterpret_cast<CLightUSDAttribute *>(new Attribute());
}

int c_lightusd_attribute_free(CLightUSDAttribute *attr) {
  if (!attr) return 0;
  delete Am(attr);
  return 1;
}

int c_lightusd_attribute_get_value(const CLightUSDAttribute *attr,
                                  const CLightUSDValue **out) {
  if (!attr || !out) return 0;
  const Attribute *a = A(attr);
  if (!a->has_value()) return 0;
  const lightusd::value::Value &v = a->get_var().value_raw();
  *out = reinterpret_cast<const CLightUSDValue *>(&v);
  return 1;
}

int c_lightusd_attribute_get_name(const CLightUSDAttribute *attr,
                                 c_lightusd_string_t *out) {
  if (!attr || !out) return 0;
  return c_lightusd_string_replace(out, A(attr)->name().c_str());
}

int c_lightusd_attribute_get_type_name(const CLightUSDAttribute *attr,
                                      c_lightusd_string_t *out) {
  if (!attr || !out) return 0;
  return c_lightusd_string_replace(out, A(attr)->type_name().c_str());
}

/* ---- Value ---- */

int c_lightusd_value_array_data(const CLightUSDValue *value,
                               const void **out_ptr,
                               uint64_t *out_n_outer,
                               uint32_t *out_n_inner,
                               uint32_t *out_component_size,
                               const char **out_format) {
  if (!value || !out_ptr || !out_n_outer || !out_n_inner ||
      !out_component_size || !out_format) {
    return 0;
  }
  ArrayView view;
  if (!resolve_array_view(V(value), &view)) return 0;
  *out_ptr = view.ptr;
  *out_n_outer = view.n_outer;
  *out_n_inner = view.n_inner;
  *out_component_size = view.component_size;
  *out_format = view.format;
  return 1;
}

int c_lightusd_value_as_double(const CLightUSDValue *value, double *out) {
  if (!value || !out) return 0;
  const lightusd::value::Value *v = V(value);
  if (auto r = v->get_value<double>()) { *out = *r; return 1; }
  if (auto r = v->get_value<float>()) { *out = static_cast<double>(*r); return 1; }
  if (auto r = v->get_value<int32_t>()) { *out = static_cast<double>(*r); return 1; }
  if (auto r = v->get_value<int64_t>()) { *out = static_cast<double>(*r); return 1; }
  return 0;
}

int c_lightusd_value_as_int64(const CLightUSDValue *value, int64_t *out) {
  if (!value || !out) return 0;
  const lightusd::value::Value *v = V(value);
  if (auto r = v->get_value<int64_t>()) { *out = *r; return 1; }
  if (auto r = v->get_value<int32_t>()) { *out = *r; return 1; }
  if (auto r = v->get_value<uint32_t>()) { *out = *r; return 1; }
  return 0;
}

int c_lightusd_value_as_bool(const CLightUSDValue *value, int *out) {
  if (!value || !out) return 0;
  const lightusd::value::Value *v = V(value);
  if (auto r = v->get_value<bool>()) { *out = *r ? 1 : 0; return 1; }
  return 0;
}

int c_lightusd_value_get_string(const CLightUSDValue *value,
                               c_lightusd_string_t *out) {
  if (!value || !out) return 0;
  const lightusd::value::Value *v = V(value);
  if (auto r = v->get_value<std::string>())
    return c_lightusd_string_replace(out, r->c_str());
  if (auto r = v->get_value<lightusd::value::token>())
    return c_lightusd_string_replace(out, r->str().c_str());
  return 0;
}

int c_lightusd_value_is_array(const CLightUSDValue *value) {
  if (!value) return 0;
  return V(value)->is_array() ? 1 : 0;
}

/* ---- Path ---- */

int c_lightusd_path_to_string(const CLightUSDPath *path, c_lightusd_string_t *out) {
  if (!path || !out) return 0;
  const Path *pp = reinterpret_cast<const Path *>(path);
  return c_lightusd_string_replace(out, pp->full_path_name().c_str());
}

/* ---- Tydra visit ---- */

namespace {

struct VisitBridge {
  CLightUSDVisitFunction cb;
  void *ud;
  const char *type_filter;  // nullptr == no filter
};

bool VisitBridgeFn(const Path &abs_path, const Prim &prim,
                   const int32_t depth, void *userdata, std::string *err) {
  (void)err;
  VisitBridge *b = static_cast<VisitBridge *>(userdata);
  if (b->type_filter) {
    const std::string &tn = prim.type_name();
    if (tn != b->type_filter) return true;  // continue
  }
  int ret = b->cb(reinterpret_cast<const CLightUSDPrim *>(&prim),
                  reinterpret_cast<const CLightUSDPath *>(&abs_path),
                  static_cast<uint32_t>(depth < 0 ? 0 : depth), b->ud);
  return ret ? true : false;
}

}  // namespace

int c_lightusd_stage_visit_prims(const CLightUSDStage *stage,
                                CLightUSDVisitFunction cb, void *userdata,
                                c_lightusd_string_t *err) {
  if (!stage || !cb) return 0;
  VisitBridge b{cb, userdata, nullptr};
  std::string e;
  bool ok = lightusd::tydra::VisitPrims(*S(stage), VisitBridgeFn, &b, &e);
  if (!ok && err && !e.empty()) c_lightusd_string_replace(err, e.c_str());
  return ok ? 1 : 0;
}

int c_lightusd_stage_list_prims_by_type(const CLightUSDStage *stage,
                                       const char *type_name,
                                       CLightUSDVisitFunction cb, void *userdata,
                                       c_lightusd_string_t *err) {
  if (!stage || !cb) return 0;
  VisitBridge b{cb, userdata, type_name};
  std::string e;
  bool ok = lightusd::tydra::VisitPrims(*S(stage), VisitBridgeFn, &b, &e);
  if (!ok && err && !e.empty()) c_lightusd_string_replace(err, e.c_str());
  return ok ? 1 : 0;
}

/* ---- Authoring helpers ---- */

int c_lightusd_prim_set_element_name(CLightUSDPrim *prim, const char *name) {
  if (!prim || !name) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  std::string s(name);
  // Update both the element_path and the typed prim's `name` field, since
  // the USDA/USDC writers read the name from the typed prim.
  lightusd::SetPrimElementName(p->get_data(), s);
  p->element_path() = Path(s, "");
  return 1;
}

int c_lightusd_stage_add_root_prim(CLightUSDStage *stage, CLightUSDPrim *prim,
                                  c_lightusd_string_t *err) {
  if (!stage || !prim) {
    if (err) c_lightusd_string_replace(err, "stage or prim is null");
    return 0;
  }
  Stage *s = Sm(stage);
  Prim *p = reinterpret_cast<Prim *>(prim);
  // Copy the prim into the stage's roots so the caller can still free its
  // input pointer. add_root_prim takes an rvalue.
  Prim copy(*p);
  bool ok = s->add_root_prim(std::move(copy), /* rename */ true);
  if (!ok) {
    if (err) c_lightusd_string_replace(err, "Stage::add_root_prim failed");
    return 0;
  }
  return 1;
}

int c_lightusd_attribute_set_name(CLightUSDAttribute *attr, const char *name) {
  if (!attr || !name) return 0;
  Am(attr)->set_name(std::string(name));
  return 1;
}

int c_lightusd_attribute_set_type_name(CLightUSDAttribute *attr,
                                      const char *type_name) {
  if (!attr || !type_name) return 0;
  Am(attr)->set_type_name(std::string(type_name));
  return 1;
}

int c_lightusd_attribute_set_value(CLightUSDAttribute *attr,
                                  const CLightUSDValue *value) {
  if (!attr || !value) return 0;
  Attribute *a = Am(attr);
  const lightusd::value::Value *v = V(value);
  lightusd::primvar::PrimVar pv;
  pv.set_value(*v);
  a->set_var(std::move(pv));
  // If type_name not yet set, derive from the value.
  if (a->type_name().empty()) {
    a->set_type_name(v->type_name());
  }
  return 1;
}

int c_lightusd_prim_add_attribute(CLightUSDPrim *prim,
                                 const CLightUSDAttribute *attr,
                                 c_lightusd_string_t *err) {
  if (!prim || !attr) {
    if (err) c_lightusd_string_replace(err, "prim or attr is null");
    return 0;
  }
  Prim *p = reinterpret_cast<Prim *>(const_cast<CLightUSDPrim *>(prim));
  const Attribute *a = A(attr);
  if (a->name().empty()) {
    if (err) c_lightusd_string_replace(err, "attribute name is empty");
    return 0;
  }

  using namespace lightusd;
  uint32_t tid = p->get_data().type_id();

#define INSERT_INTO(__TID, __TY)                                       \
  case __TID: {                                                        \
    __TY *typed = p->get_data().as<__TY>();                            \
    if (!typed) {                                                      \
      if (err) c_lightusd_string_replace(err,                           \
        "internal: typed prim cast failed");                           \
      return 0;                                                        \
    }                                                                  \
    typed->props[a->name()] = Property(*a, false);                     \
    return 1;                                                          \
  }

  switch (tid) {
    INSERT_INTO(value::TYPE_ID_GEOM_XFORM,    Xform)
    INSERT_INTO(value::TYPE_ID_GEOM_MESH,     GeomMesh)
    INSERT_INTO(value::TYPE_ID_GEOM_SPHERE,   GeomSphere)
    INSERT_INTO(value::TYPE_ID_GEOM_CUBE,     GeomCube)
    INSERT_INTO(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
    INSERT_INTO(value::TYPE_ID_GEOM_CONE,     GeomCone)
    INSERT_INTO(value::TYPE_ID_GEOM_CAPSULE,  GeomCapsule)
    INSERT_INTO(value::TYPE_ID_GEOM_CAMERA,   GeomCamera)
    INSERT_INTO(value::TYPE_ID_GEOM_POINTS,   GeomPoints)
    INSERT_INTO(value::TYPE_ID_GEOM_GEOMSUBSET,      GeomSubset)
    INSERT_INTO(value::TYPE_ID_GEOM_BASIS_CURVES,    GeomBasisCurves)
    INSERT_INTO(value::TYPE_ID_GEOM_NURBS_CURVES,    GeomNurbsCurves)
    INSERT_INTO(value::TYPE_ID_GEOM_HERMITE_CURVES,  GeomHermiteCurves)
    INSERT_INTO(value::TYPE_ID_GEOM_PLANE,           GeomPlane)
    INSERT_INTO(value::TYPE_ID_GEOM_CYLINDER_1,      GeomCylinder_1)
    INSERT_INTO(value::TYPE_ID_GEOM_CAPSULE_1,       GeomCapsule_1)
    INSERT_INTO(value::TYPE_ID_GEOM_TET_MESH,        GeomTetMesh)
    INSERT_INTO(value::TYPE_ID_GEOM_NURBS_PATCH,     GeomNurbsPatch)
    INSERT_INTO(value::TYPE_ID_GEOM_POINT_INSTANCER, GeomPointInstancer)
    INSERT_INTO(value::TYPE_ID_LUX_SPHERE,    SphereLight)
    INSERT_INTO(value::TYPE_ID_LUX_RECT,      RectLight)
    INSERT_INTO(value::TYPE_ID_LUX_DISK,      DiskLight)
    INSERT_INTO(value::TYPE_ID_LUX_DISTANT,   DistantLight)
    INSERT_INTO(value::TYPE_ID_LUX_CYLINDER,  CylinderLight)
    INSERT_INTO(value::TYPE_ID_LUX_DOME,      DomeLight)
    INSERT_INTO(value::TYPE_ID_LUX_DOME_1,    DomeLight_1)
    INSERT_INTO(value::TYPE_ID_LUX_GEOMETRY,  GeometryLight)
    INSERT_INTO(value::TYPE_ID_LUX_PORTAL,    PortalLight)
    INSERT_INTO(value::TYPE_ID_SCOPE,         Scope)
    INSERT_INTO(value::TYPE_ID_MODEL,         Model)
    INSERT_INTO(value::TYPE_ID_PHYSICS_SCENE,            PhysicsScene)
    INSERT_INTO(value::TYPE_ID_PHYSICS_JOINT,            PhysicsJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_REVOLUTE_JOINT,   PhysicsRevoluteJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_PRISMATIC_JOINT,  PhysicsPrismaticJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_SPHERICAL_JOINT,  PhysicsSphericalJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_FIXED_JOINT,      PhysicsFixedJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_DISTANCE_JOINT,   PhysicsDistanceJoint)
    INSERT_INTO(value::TYPE_ID_PHYSICS_COLLISION_GROUP,  PhysicsCollisionGroup)
    INSERT_INTO(value::TYPE_ID_MATERIAL,                 Material)
    INSERT_INTO(value::TYPE_ID_SHADER,                   Shader)
    INSERT_INTO(value::TYPE_ID_NODEGRAPH,                NodeGraph)
    INSERT_INTO(value::TYPE_ID_SKEL_ROOT,                SkelRoot)
    INSERT_INTO(value::TYPE_ID_SKELETON,                 Skeleton)
    INSERT_INTO(value::TYPE_ID_SKELANIMATION,            SkelAnimation)
    INSERT_INTO(value::TYPE_ID_BLENDSHAPE,               BlendShape)
    default: {
      if (err) {
        std::string msg = "unsupported prim type for attribute authoring: " +
                          p->type_name();
        c_lightusd_string_replace(err, msg.c_str());
      }
      return 0;
    }
  }

#undef INSERT_INTO
}

/* ---- token[] / string[] value constructors ---- */

CLightUSDValue *c_lightusd_value_new_array_token(uint64_t n,
                                               const char *const *toks) {
  std::vector<lightusd::value::token> v;
  v.reserve(size_t(n));
  for (uint64_t i = 0; i < n; ++i) {
    v.emplace_back(toks && toks[i] ? toks[i] : "");
  }
  auto *vp = new lightusd::value::Value(std::move(v));
  return reinterpret_cast<CLightUSDValue *>(vp);
}

CLightUSDValue *c_lightusd_value_new_array_string(uint64_t n,
                                                const char *const *strs) {
  std::vector<std::string> v;
  v.reserve(size_t(n));
  for (uint64_t i = 0; i < n; ++i) {
    v.emplace_back(strs && strs[i] ? strs[i] : "");
  }
  auto *vp = new lightusd::value::Value(std::move(v));
  return reinterpret_cast<CLightUSDValue *>(vp);
}

/* ---- API schemas ---- */

int c_lightusd_prim_apply_api_schema(CLightUSDPrim *prim,
                                    const char *schema_name,
                                    const char *instance_name) {
  if (!prim || !schema_name) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  auto &apis = p->metas().get_apiSchemas_mutable();
  // Always use prepend list-edit qualifier (USD canonical).
  apis.listOpQual = lightusd::ListEditQual::Prepend;
  apis.unknownSchemas.emplace_back(
      std::string(schema_name),
      std::string(instance_name ? instance_name : ""));
  return 1;
}

int c_lightusd_prim_get_api_schemas(const CLightUSDPrim *prim,
                                   c_lightusd_string_t *out) {
  if (!prim || !out) return 0;
  const Prim *p = P(prim);
  if (!p->metas().has_apiSchemas()) return 0;
  const auto &apis = p->metas().get_apiSchemas();
  std::string joined;
  for (const auto &kv : apis.names) {
    if (!joined.empty()) joined += ",";
    joined += lightusd::to_string(kv.first);
    if (!kv.second.empty()) { joined += ":"; joined += kv.second; }
  }
  for (const auto &kv : apis.unknownSchemas) {
    if (!joined.empty()) joined += ",";
    joined += kv.first;
    if (!kv.second.empty()) { joined += ":"; joined += kv.second; }
  }
  return c_lightusd_string_replace(out, joined.c_str());
}

/* ---- Prim metadata ---- */

int c_lightusd_prim_meta_set_string(CLightUSDPrim *prim, const char *meta_name,
                                   const char *value) {
  if (!prim || !meta_name || !value) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  std::string key(meta_name);
  // Alias: USD canonical key is "documentation"; accept "doc" for ergonomics.
  if (key == "doc") key = "documentation";
  std::string val(value);
  auto &m = p->metas();
  if (key == "specifier") {
    lightusd::Specifier sp;
    if (val == "def") sp = lightusd::Specifier::Def;
    else if (val == "over") sp = lightusd::Specifier::Over;
    else if (val == "class") sp = lightusd::Specifier::Class;
    else return 0;
    p->specifier() = sp;
    /* Also stamp the typed prim's `spec` field so the USDA pprint, which
     * reads from `xform.spec` / `mesh.spec` / etc., honors it. */
    using namespace lightusd;
    uint32_t tid = p->get_data().type_id();
#define SET_SPEC(__TID, __TY)                              \
  case __TID: {                                            \
    auto *typed = p->get_data().as<__TY>();                \
    if (typed) typed->spec = sp;                           \
    return 1;                                              \
  }
    switch (tid) {
      SET_SPEC(value::TYPE_ID_GEOM_XFORM, Xform)
      SET_SPEC(value::TYPE_ID_GEOM_MESH, GeomMesh)
      SET_SPEC(value::TYPE_ID_GEOM_SPHERE, GeomSphere)
      SET_SPEC(value::TYPE_ID_GEOM_CUBE, GeomCube)
      SET_SPEC(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
      SET_SPEC(value::TYPE_ID_GEOM_CONE, GeomCone)
      SET_SPEC(value::TYPE_ID_GEOM_CAPSULE, GeomCapsule)
      SET_SPEC(value::TYPE_ID_GEOM_CAMERA, GeomCamera)
      SET_SPEC(value::TYPE_ID_GEOM_POINTS, GeomPoints)
      SET_SPEC(value::TYPE_ID_GEOM_GEOMSUBSET, GeomSubset)
      SET_SPEC(value::TYPE_ID_GEOM_BASIS_CURVES, GeomBasisCurves)
      SET_SPEC(value::TYPE_ID_GEOM_NURBS_CURVES, GeomNurbsCurves)
      SET_SPEC(value::TYPE_ID_GEOM_HERMITE_CURVES, GeomHermiteCurves)
      SET_SPEC(value::TYPE_ID_GEOM_PLANE, GeomPlane)
      SET_SPEC(value::TYPE_ID_GEOM_CYLINDER_1, GeomCylinder_1)
      SET_SPEC(value::TYPE_ID_GEOM_CAPSULE_1, GeomCapsule_1)
      SET_SPEC(value::TYPE_ID_GEOM_TET_MESH, GeomTetMesh)
      SET_SPEC(value::TYPE_ID_GEOM_NURBS_PATCH, GeomNurbsPatch)
      SET_SPEC(value::TYPE_ID_GEOM_POINT_INSTANCER, GeomPointInstancer)
      SET_SPEC(value::TYPE_ID_LUX_SPHERE, SphereLight)
      SET_SPEC(value::TYPE_ID_LUX_RECT, RectLight)
      SET_SPEC(value::TYPE_ID_LUX_DISK, DiskLight)
      SET_SPEC(value::TYPE_ID_LUX_DISTANT, DistantLight)
      SET_SPEC(value::TYPE_ID_LUX_CYLINDER, CylinderLight)
      SET_SPEC(value::TYPE_ID_LUX_DOME, DomeLight)
      SET_SPEC(value::TYPE_ID_LUX_DOME_1, DomeLight_1)
      SET_SPEC(value::TYPE_ID_LUX_GEOMETRY, GeometryLight)
      SET_SPEC(value::TYPE_ID_LUX_PORTAL, PortalLight)
      SET_SPEC(value::TYPE_ID_SCOPE, Scope)
      SET_SPEC(value::TYPE_ID_MODEL, Model)
      SET_SPEC(value::TYPE_ID_MATERIAL, Material)
      SET_SPEC(value::TYPE_ID_SHADER, Shader)
      SET_SPEC(value::TYPE_ID_NODEGRAPH, NodeGraph)
      SET_SPEC(value::TYPE_ID_SKEL_ROOT, SkelRoot)
      SET_SPEC(value::TYPE_ID_SKELETON, Skeleton)
      SET_SPEC(value::TYPE_ID_SKELANIMATION, SkelAnimation)
      SET_SPEC(value::TYPE_ID_BLENDSHAPE, BlendShape)
      SET_SPEC(value::TYPE_ID_PHYSICS_SCENE, PhysicsScene)
      SET_SPEC(value::TYPE_ID_PHYSICS_JOINT, PhysicsJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_REVOLUTE_JOINT, PhysicsRevoluteJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_PRISMATIC_JOINT, PhysicsPrismaticJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_SPHERICAL_JOINT, PhysicsSphericalJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_FIXED_JOINT, PhysicsFixedJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_DISTANCE_JOINT, PhysicsDistanceJoint)
      SET_SPEC(value::TYPE_ID_PHYSICS_COLLISION_GROUP, PhysicsCollisionGroup)
      default: return 1;
    }
#undef SET_SPEC
  }
  if (key == "kind") {
    m.set(key, lightusd::value::token(val));
  } else if (key == "sceneName") {
    // pxrUSD's USDZ scene-library extension stores sceneName as
    // `string`. PrimMetas::get_sceneName() looks up via get<std::string>,
    // so storing it as a token here would silently lose the value.
    m.set_sceneName(val);
  } else if (key == "documentation" || key == "comment") {
    m.set(key, lightusd::value::StringData(val));
  } else if (key == "displayName") {
    m.set_displayName(val);
  } else {
    // generic string fallback
    m.set(key, val);
  }
  return 1;
}

int c_lightusd_prim_meta_set_bool(CLightUSDPrim *prim, const char *meta_name,
                                 int value) {
  if (!prim || !meta_name) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  std::string key(meta_name);
  bool b = value ? true : false;
  auto &m = p->metas();
  if (key == "active") m.set_active(b);
  else if (key == "hidden") m.set_hidden(b);
  else m.set(key, b);
  return 1;
}

int c_lightusd_prim_meta_get_bool(const CLightUSDPrim *prim,
                                 const char *meta_name, int *out) {
  if (!prim || !meta_name || !out) return 0;
  const Prim *p = P(prim);
  const auto &m = p->metas();
  std::string key(meta_name);
  if (key == "active") {
    if (!m.has_active()) return 0;
    *out = m.get_active() ? 1 : 0;
    return 1;
  }
  if (key == "hidden") {
    if (!m.has_hidden()) return 0;
    *out = m.get_hidden() ? 1 : 0;
    return 1;
  }
  if (auto v = m.get<bool>(key)) {
    *out = v.value() ? 1 : 0;
    return 1;
  }
  return 0;
}

int c_lightusd_prim_meta_get_string(const CLightUSDPrim *prim,
                                   const char *meta_name,
                                   c_lightusd_string_t *out) {
  if (!prim || !meta_name || !out) return 0;
  const Prim *p = P(prim);
  const auto &m = p->metas();
  std::string key(meta_name);
  if (key == "specifier") {
    /* Read from typed prim's `spec` first (canonical for pprint+writer);
     * fall back to wrapper specifier. */
    using namespace lightusd;
    Specifier sp = p->specifier();
    uint32_t tid = p->data().type_id();
#define READ_SPEC(__TID, __TY) \
  case __TID: if (auto *t = p->data().as<__TY>()) { sp = t->spec; } break;
    switch (tid) {
      READ_SPEC(value::TYPE_ID_GEOM_XFORM, Xform)
      READ_SPEC(value::TYPE_ID_GEOM_MESH, GeomMesh)
      READ_SPEC(value::TYPE_ID_GEOM_SPHERE, GeomSphere)
      READ_SPEC(value::TYPE_ID_GEOM_CUBE, GeomCube)
      READ_SPEC(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
      READ_SPEC(value::TYPE_ID_GEOM_CONE, GeomCone)
      READ_SPEC(value::TYPE_ID_GEOM_CAPSULE, GeomCapsule)
      READ_SPEC(value::TYPE_ID_GEOM_CAMERA, GeomCamera)
      READ_SPEC(value::TYPE_ID_SCOPE, Scope)
      READ_SPEC(value::TYPE_ID_MODEL, Model)
      READ_SPEC(value::TYPE_ID_MATERIAL, Material)
      READ_SPEC(value::TYPE_ID_SHADER, Shader)
      default: break;
    }
#undef READ_SPEC
    const char *s = "def";
    switch (sp) {
      case lightusd::Specifier::Def: s = "def"; break;
      case lightusd::Specifier::Over: s = "over"; break;
      case lightusd::Specifier::Class: s = "class"; break;
      default: s = "def"; break;
    }
    return c_lightusd_string_replace(out, s);
  }
  if (key == "doc") key = "documentation";  /* alias */
  if (auto v = m.get<lightusd::value::token>(key)) {
    return c_lightusd_string_replace(out, v.value().str().c_str());
  }
  if (auto v = m.get<std::string>(key)) {
    return c_lightusd_string_replace(out, v.value().c_str());
  }
  if (auto v = m.get<lightusd::value::StringData>(key)) {
    return c_lightusd_string_replace(out, v.value().value.c_str());
  }
  return 0;
}

/* ---- Double-precision value constructors ---- */

CLightUSDValue *c_lightusd_value_new_double(double v) {
  auto *p = new lightusd::value::Value(v);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_double2(c_lightusd_double2_t v) {
  lightusd::value::double2 d{v.x, v.y};
  auto *p = new lightusd::value::Value(d);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_double3(c_lightusd_double3_t v) {
  lightusd::value::double3 d{v.x, v.y, v.z};
  auto *p = new lightusd::value::Value(d);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_double4(c_lightusd_double4_t v) {
  lightusd::value::double4 d{v.x, v.y, v.z, v.w};
  auto *p = new lightusd::value::Value(d);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_array_double(uint64_t n, const double *v) {
  if (!v && n) return nullptr;
  std::vector<double> arr(v, v + n);
  auto *p = new lightusd::value::Value(arr);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_array_double3(uint64_t n,
                                                 const c_lightusd_double3_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::double3> arr(n);
  for (uint64_t i = 0; i < n; ++i) {
    arr[i] = lightusd::value::double3{v[i].x, v[i].y, v[i].z};
  }
  auto *p = new lightusd::value::Value(arr);
  return reinterpret_cast<CLightUSDValue *>(p);
}
CLightUSDValue *c_lightusd_value_new_matrix4d(const double v[16]) {
  if (!v) return nullptr;
  lightusd::value::matrix4d m;
  /* matrix4d stores as 4x4 of doubles */
  std::memcpy(&m, v, sizeof(double) * 16);
  auto *p = new lightusd::value::Value(m);
  return reinterpret_cast<CLightUSDValue *>(p);
}

CLightUSDValue *c_lightusd_value_new_matrix2d_t(c_lightusd_matrix2d_t v) {
  lightusd::value::matrix2d m;
  std::memcpy(&m, &v, sizeof(double) * 4);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(m));
}
CLightUSDValue *c_lightusd_value_new_matrix3d_t(c_lightusd_matrix3d_t v) {
  lightusd::value::matrix3d m;
  std::memcpy(&m, &v, sizeof(double) * 9);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(m));
}
CLightUSDValue *c_lightusd_value_new_matrix4d_t(c_lightusd_matrix4d_t v) {
  lightusd::value::matrix4d m;
  std::memcpy(&m, &v, sizeof(double) * 16);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(m));
}

CLightUSDValue *c_lightusd_value_new_array_double2(
    uint64_t n, const c_lightusd_double2_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::double2> arr(n);
  for (uint64_t i = 0; i < n; ++i) arr[i] = {v[i].x, v[i].y};
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_double4(
    uint64_t n, const c_lightusd_double4_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::double4> arr(n);
  for (uint64_t i = 0; i < n; ++i)
    arr[i] = {v[i].x, v[i].y, v[i].z, v[i].w};
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_matrix2d(
    uint64_t n, const c_lightusd_matrix2d_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::matrix2d> arr(n);
  if (n) std::memcpy(arr.data(), v, sizeof(double) * 4 * n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_matrix3d(
    uint64_t n, const c_lightusd_matrix3d_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::matrix3d> arr(n);
  if (n) std::memcpy(arr.data(), v, sizeof(double) * 9 * n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_matrix4d(
    uint64_t n, const c_lightusd_matrix4d_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::matrix4d> arr(n);
  if (n) std::memcpy(arr.data(), v, sizeof(double) * 16 * n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}

/* ---- bool ---- */
CLightUSDValue *c_lightusd_value_new_bool(int v) {
  bool b = (v != 0);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(b));
}
CLightUSDValue *c_lightusd_value_new_array_bool(uint64_t n, const int *v) {
  if (!v && n) return nullptr;
  std::vector<bool> arr;
  arr.reserve(n);
  for (uint64_t i = 0; i < n; ++i) arr.push_back(v[i] != 0);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}

/* ---- Typed float3/double3 alias scalars + arrays ----
 * Identical memory layout to float3/double3 but author as the typed
 * alias so the writer emits color3f/point3f/normal3f/vector3f (and
 * their double counterparts).
 */
#define TYPED_VEC_NEW(__name, __cppty, __cty)                                  \
  CLightUSDValue *c_lightusd_value_new_##__name(__cty v) {                       \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");      \
    lightusd::value::__cppty x;                                                \
    std::memcpy(&x, &v, sizeof(__cty));                                        \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(x));   \
  }                                                                            \
  CLightUSDValue *c_lightusd_value_new_array_##__name(                           \
      uint64_t n, const __cty *vals) {                                         \
    if (!vals && n) return nullptr;                                            \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");      \
    std::vector<lightusd::value::__cppty> arr(n);                              \
    if (n) std::memcpy(arr.data(), vals, sizeof(__cty) * size_t(n));           \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr)); \
  }

TYPED_VEC_NEW(color3f,  color3f,  c_lightusd_color3f_t)
TYPED_VEC_NEW(point3f,  point3f,  c_lightusd_point3f_t)
TYPED_VEC_NEW(normal3f, normal3f, c_lightusd_normal3f_t)
TYPED_VEC_NEW(vector3f, vector3f, c_lightusd_float3_t)
TYPED_VEC_NEW(color3d,  color3d,  c_lightusd_color3d_t)
TYPED_VEC_NEW(point3d,  point3d,  c_lightusd_point3d_t)
TYPED_VEC_NEW(normal3d, normal3d, c_lightusd_normal3d_t)
TYPED_VEC_NEW(vector3d, vector3d, c_lightusd_double3_t)

#undef TYPED_VEC_NEW

c_lightusd_half_t c_lightusd_float_to_half(float f) {
  return lightusd::value::float_to_half_full(f).value;
}

/* ---- Half precision scalars ----
 * c_lightusd_half_t is a uint16_t bit pattern. value::half wraps it.
 */
CLightUSDValue *c_lightusd_value_new_half(c_lightusd_half_t v) {
  lightusd::value::half h{v};
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(h));
}
#define HALF_VEC_NEW(__name, __cppty, __cty)                                 \
  CLightUSDValue *c_lightusd_value_new_##__name(__cty v) {                     \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");    \
    lightusd::value::__cppty x;                                              \
    std::memcpy(&x, &v, sizeof(__cty));                                      \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(x)); \
  }                                                                          \
  CLightUSDValue *c_lightusd_value_new_array_##__name(                         \
      uint64_t n, const __cty *vals) {                                       \
    if (!vals && n) return nullptr;                                          \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");    \
    std::vector<lightusd::value::__cppty> arr(n);                            \
    if (n) std::memcpy(arr.data(), vals, sizeof(__cty) * size_t(n));         \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr)); \
  }

HALF_VEC_NEW(half2, half2, c_lightusd_half2_t)
HALF_VEC_NEW(half3, half3, c_lightusd_half3_t)
HALF_VEC_NEW(half4, half4, c_lightusd_half4_t)
#undef HALF_VEC_NEW

CLightUSDValue *c_lightusd_value_new_array_half(uint64_t n,
                                              const c_lightusd_half_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::half> arr(n);
  for (uint64_t i = 0; i < n; ++i) arr[i] = lightusd::value::half{v[i]};
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}

/* ---- Wide integers ---- */
CLightUSDValue *c_lightusd_value_new_uint(uint32_t v) {
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(v));
}
CLightUSDValue *c_lightusd_value_new_uint64(uint64_t v) {
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(v));
}
CLightUSDValue *c_lightusd_value_new_int64(int64_t v) {
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(v));
}
CLightUSDValue *c_lightusd_value_new_array_uint(uint64_t n, const uint32_t *v) {
  if (!v && n) return nullptr;
  std::vector<uint32_t> arr(v, v + n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_uint64(uint64_t n, const uint64_t *v) {
  if (!v && n) return nullptr;
  std::vector<uint64_t> arr(v, v + n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}
CLightUSDValue *c_lightusd_value_new_array_int64(uint64_t n, const int64_t *v) {
  if (!v && n) return nullptr;
  std::vector<int64_t> arr(v, v + n);
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}

/* ---- Quaternions ----
 * Memory layout is {imag[3], real} for both C and C++ structs, so memcpy
 * is safe.
 */
#define QUAT_NEW(__name, __cppty, __cty)                                     \
  CLightUSDValue *c_lightusd_value_new_##__name(__cty v) {                     \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");    \
    lightusd::value::__cppty x;                                              \
    std::memcpy(&x, &v, sizeof(__cty));                                      \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(x)); \
  }                                                                          \
  CLightUSDValue *c_lightusd_value_new_array_##__name(                         \
      uint64_t n, const __cty *vals) {                                       \
    if (!vals && n) return nullptr;                                          \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");    \
    std::vector<lightusd::value::__cppty> arr(n);                            \
    if (n) std::memcpy(arr.data(), vals, sizeof(__cty) * size_t(n));         \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr)); \
  }
QUAT_NEW(quath, quath, c_lightusd_quath_t)
QUAT_NEW(quatf, quatf, c_lightusd_quatf_t)
QUAT_NEW(quatd, quatd, c_lightusd_quatd_t)
#undef QUAT_NEW

/* ---- TexCoord role types (float2/double2/float3/double3 aliases) ----
 * These use the same TYPED_VEC_NEW macro pattern as color3f, etc.
 */
#define TEXCOORD_NEW(__name, __cppty, __cty)                                   \
  CLightUSDValue *c_lightusd_value_new_##__name(__cty v) {                       \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");      \
    lightusd::value::__cppty x;                                                \
    std::memcpy(&x, &v, sizeof(__cty));                                        \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(x));   \
  }                                                                            \
  CLightUSDValue *c_lightusd_value_new_array_##__name(                           \
      uint64_t n, const __cty *vals) {                                         \
    if (!vals && n) return nullptr;                                            \
    static_assert(sizeof(lightusd::value::__cppty) == sizeof(__cty), "");      \
    std::vector<lightusd::value::__cppty> arr(n);                              \
    if (n) std::memcpy(arr.data(), vals, sizeof(__cty) * size_t(n));           \
    return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr)); \
  }

TEXCOORD_NEW(texcoord2f, float2, c_lightusd_float2_t)
TEXCOORD_NEW(texcoord2d, double2, c_lightusd_double2_t)
TEXCOORD_NEW(texcoord3f, float3, c_lightusd_float3_t)
TEXCOORD_NEW(texcoord3d, double3, c_lightusd_double3_t)
#undef TEXCOORD_NEW

/* ---- frame4d (semantic alias for matrix4d) ----
 * frame4d has identical memory layout to matrix4d (double m[4][4], stored
 * as double m[16] in the C API). We construct frame4d properly and copy data.
 */
CLightUSDValue *c_lightusd_value_new_frame4d(c_lightusd_matrix4d_t v) {
  // Create a frame4d and copy matrix data into its m member
  lightusd::value::frame4d f;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      f.m[i][j] = v.m[i * 4 + j];
    }
  }
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(f));
}

CLightUSDValue *c_lightusd_value_new_array_frame4d(uint64_t n, const c_lightusd_matrix4d_t *v) {
  if (!v && n) return nullptr;
  std::vector<lightusd::value::frame4d> arr;
  arr.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    lightusd::value::frame4d f;
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        f.m[row][col] = v[i].m[row * 4 + col];
      }
    }
    arr.emplace_back(f);
  }
  return reinterpret_cast<CLightUSDValue *>(new lightusd::value::Value(arr));
}

}  /* end extern "C" — templates need C++ linkage */

/* ---- Composition arc authoring ---- */

namespace {
inline lightusd::ListEditQual to_listedit_qual(CLightUSDListEditQual q) {
  switch (q) {
    case C_LIGHTUSD_LISTEDITQUAL_RESETTOEXPLICIT: return lightusd::ListEditQual::ResetToExplicit;
    case C_LIGHTUSD_LISTEDITQUAL_APPEND:          return lightusd::ListEditQual::Append;
    case C_LIGHTUSD_LISTEDITQUAL_ADD:             return lightusd::ListEditQual::Add;
    case C_LIGHTUSD_LISTEDITQUAL_DELETE:          return lightusd::ListEditQual::Delete;
    case C_LIGHTUSD_LISTEDITQUAL_PREPEND:         return lightusd::ListEditQual::Prepend;
    case C_LIGHTUSD_LISTEDITQUAL_ORDER:           return lightusd::ListEditQual::Order;
  }
  return lightusd::ListEditQual::ResetToExplicit;
}

template <typename Item>
void append_listop(
    nonstd::optional<std::vector<std::pair<lightusd::ListEditQual,
                                           std::vector<Item>>>> &slot,
    lightusd::ListEditQual q,
    Item item) {
  if (!slot.has_value()) {
    slot = std::vector<std::pair<lightusd::ListEditQual, std::vector<Item>>>{};
  }
  // Find an existing entry with the same qualifier; append in place.
  for (auto &entry : slot.value()) {
    if (entry.first == q) {
      entry.second.emplace_back(std::move(item));
      return;
    }
  }
  std::vector<Item> v;
  v.emplace_back(std::move(item));
  slot.value().emplace_back(q, std::move(v));
}

inline lightusd::Path make_optional_path(const char *prim_path) {
  if (!prim_path || !*prim_path) return lightusd::Path();
  return lightusd::Path(std::string(prim_path), "");
}
}  // namespace

extern "C" {

int c_lightusd_prim_add_reference(CLightUSDPrim *prim,
                                 CLightUSDListEditQual qualifier,
                                 const char *asset_path,
                                 const char *prim_path,
                                 double offset,
                                 double scale) {
  if (!prim) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Reference ref;
  if (asset_path && *asset_path) {
    ref.asset_path = lightusd::value::AssetPath(std::string(asset_path));
  }
  ref.prim_path = make_optional_path(prim_path);
  ref.layerOffset._offset = offset;
  ref.layerOffset._scale = scale;
  append_listop(p->metas().references, to_listedit_qual(qualifier),
                std::move(ref));
  return 1;
}

int c_lightusd_prim_add_payload(CLightUSDPrim *prim,
                               CLightUSDListEditQual qualifier,
                               const char *asset_path,
                               const char *prim_path,
                               double offset,
                               double scale) {
  if (!prim) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Payload pl;
  if (asset_path && *asset_path) {
    pl.asset_path = lightusd::value::AssetPath(std::string(asset_path));
  }
  pl.prim_path = make_optional_path(prim_path);
  pl.layerOffset._offset = offset;
  pl.layerOffset._scale = scale;
  append_listop(p->metas().payload, to_listedit_qual(qualifier),
                std::move(pl));
  return 1;
}

int c_lightusd_prim_add_inherit(CLightUSDPrim *prim,
                               CLightUSDListEditQual qualifier,
                               const char *prim_path) {
  if (!prim || !prim_path) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Path path(std::string(prim_path), "");
  append_listop(p->metas().inherits, to_listedit_qual(qualifier),
                std::move(path));
  return 1;
}

int c_lightusd_prim_add_specialize(CLightUSDPrim *prim,
                                  CLightUSDListEditQual qualifier,
                                  const char *prim_path) {
  if (!prim || !prim_path) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Path path(std::string(prim_path), "");
  append_listop(p->metas().specializes, to_listedit_qual(qualifier),
                std::move(path));
  return 1;
}

int c_lightusd_prim_clear_references(CLightUSDPrim *prim) {
  if (!prim) return 0;
  reinterpret_cast<lightusd::Prim *>(prim)->metas().references.reset();
  return 1;
}
int c_lightusd_prim_clear_payload(CLightUSDPrim *prim) {
  if (!prim) return 0;
  reinterpret_cast<lightusd::Prim *>(prim)->metas().payload.reset();
  return 1;
}
int c_lightusd_prim_clear_inherits(CLightUSDPrim *prim) {
  if (!prim) return 0;
  reinterpret_cast<lightusd::Prim *>(prim)->metas().inherits.reset();
  return 1;
}
int c_lightusd_prim_clear_specializes(CLightUSDPrim *prim) {
  if (!prim) return 0;
  reinterpret_cast<lightusd::Prim *>(prim)->metas().specializes.reset();
  return 1;
}

/* ---- Variant authoring (metadata-level) ---- */

int c_lightusd_prim_add_variant_set_name(CLightUSDPrim *prim,
                                        CLightUSDListEditQual qualifier,
                                        const char *name) {
  if (!prim || !name) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  append_listop(p->metas().variantSets, to_listedit_qual(qualifier),
                std::string(name));
  return 1;
}

int c_lightusd_prim_clear_variant_set_names(CLightUSDPrim *prim) {
  if (!prim) return 0;
  reinterpret_cast<lightusd::Prim *>(prim)->metas().variantSets.reset();
  return 1;
}

int c_lightusd_prim_set_variant_selection(CLightUSDPrim *prim,
                                         const char *variant_set_name,
                                         const char *variant_name) {
  if (!prim || !variant_set_name || !variant_name) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  if (!p->metas().variants.has_value()) {
    p->metas().variants = lightusd::VariantSelectionMap{};
  }
  (*p->metas().variants)[std::string(variant_set_name)] =
      std::string(variant_name);
  return 1;
}

int c_lightusd_prim_clear_variant_selection(CLightUSDPrim *prim,
                                           const char *variant_set_name) {
  if (!prim) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  if (!variant_set_name || !*variant_set_name) {
    p->metas().variants.reset();
    return 1;
  }
  if (p->metas().variants.has_value()) {
    p->metas().variants->erase(std::string(variant_set_name));
    if (p->metas().variants->empty()) {
      p->metas().variants.reset();
    }
  }
  return 1;
}

/* Enumerate the keys of the prim's variants={...} selection map. */
int c_lightusd_prim_get_variant_selection_keys(const CLightUSDPrim *prim,
                                              c_lightusd_string_t **out_names,
                                              uint64_t cap,
                                              uint64_t *out_count) {
  if (!prim) return 0;
  const auto *p = reinterpret_cast<const lightusd::Prim *>(prim);
  if (!p->metas().variants.has_value()) {
    if (out_count) *out_count = 0;
    return 1;
  }
  const auto &vmap = *p->metas().variants;
  if (out_count) *out_count = static_cast<uint64_t>(vmap.size());
  uint64_t i = 0;
  for (const auto &kv : vmap) {
    if (out_names && i < cap) {
      c_lightusd_string_replace(out_names[i], kv.first.c_str());
    }
    ++i;
  }
  return 1;
}

/* ---- Variant content authoring ---- */

namespace {

lightusd::Variant *get_or_create_variant(lightusd::Prim *p,
                                         const std::string &vs_name,
                                         const std::string &v_name) {
  auto &vsets = p->variantSets();
  auto it = vsets.find(vs_name);
  if (it == vsets.end()) {
    lightusd::VariantSet vs;
    vs.name = vs_name;
    auto inserted = vsets.emplace(vs_name, std::move(vs));
    it = inserted.first;
  }
  auto &vmap = it->second.variantSet;
  auto vit = vmap.find(v_name);
  if (vit == vmap.end()) {
    auto inserted = vmap.emplace(v_name, lightusd::Variant{});
    vit = inserted.first;
  }
  return &vit->second;
}

}  // namespace

int c_lightusd_prim_define_variant(CLightUSDPrim *prim,
                                  const char *variant_set_name,
                                  const char *variant_name) {
  if (!prim || !variant_set_name || !variant_name) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  return get_or_create_variant(p, variant_set_name, variant_name) ? 1 : 0;
}

int c_lightusd_prim_variant_add_child(CLightUSDPrim *prim,
                                     const char *variant_set_name,
                                     const char *variant_name,
                                     const CLightUSDPrim *child) {
  if (!prim || !variant_set_name || !variant_name || !child) return 0;
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  const auto *c = reinterpret_cast<const lightusd::Prim *>(child);
  if (p == c) return 0;
  auto *v = get_or_create_variant(p, variant_set_name, variant_name);
  if (!v) return 0;
  v->primChildren().push_back(*c);
  return 1;
}

int c_lightusd_prim_variant_add_attribute(CLightUSDPrim *prim,
                                         const char *variant_set_name,
                                         const char *variant_name,
                                         const CLightUSDAttribute *attr,
                                         c_lightusd_string_t *err) {
  if (!prim || !variant_set_name || !variant_name || !attr) {
    if (err) c_lightusd_string_replace(err, "null arg");
    return 0;
  }
  auto *p = reinterpret_cast<lightusd::Prim *>(prim);
  const auto *a = reinterpret_cast<const lightusd::Attribute *>(attr);
  if (a->name().empty()) {
    if (err) c_lightusd_string_replace(err, "attribute name is empty");
    return 0;
  }
  auto *v = get_or_create_variant(p, variant_set_name, variant_name);
  if (!v) return 0;
  v->properties()[a->name()] = lightusd::Property(*a, /* custom */ false);
  return 1;
}

int c_lightusd_prim_get_variant_set_names(const CLightUSDPrim *prim,
                                         c_lightusd_string_t **out_names,
                                         uint64_t cap,
                                         uint64_t *out_count) {
  if (!prim) return 0;
  const auto *p = reinterpret_cast<const lightusd::Prim *>(prim);
  const auto &vsets = p->variantSets();
  if (out_count) *out_count = static_cast<uint64_t>(vsets.size());
  uint64_t i = 0;
  for (const auto &kv : vsets) {
    if (out_names && i < cap) {
      c_lightusd_string_replace(out_names[i], kv.first.c_str());
    }
    ++i;
  }
  return 1;
}

int c_lightusd_prim_get_variant_names(const CLightUSDPrim *prim,
                                     const char *variant_set_name,
                                     c_lightusd_string_t **out_names,
                                     uint64_t cap,
                                     uint64_t *out_count) {
  if (!prim || !variant_set_name) return 0;
  const auto *p = reinterpret_cast<const lightusd::Prim *>(prim);
  const auto &vsets = p->variantSets();
  auto it = vsets.find(std::string(variant_set_name));
  if (it == vsets.end()) {
    if (out_count) *out_count = 0;
    return 1;  // not an error; just empty.
  }
  const auto &vmap = it->second.variantSet;
  if (out_count) *out_count = static_cast<uint64_t>(vmap.size());
  uint64_t i = 0;
  for (const auto &kv : vmap) {
    if (out_names && i < cap) {
      c_lightusd_string_replace(out_names[i], kv.first.c_str());
    }
    ++i;
  }
  return 1;
}

int c_lightusd_prim_get_variant_selection(const CLightUSDPrim *prim,
                                         const char *variant_set_name,
                                         c_lightusd_string_t *out) {
  if (!prim || !variant_set_name || !out) return 0;
  const auto *p = reinterpret_cast<const lightusd::Prim *>(prim);
  if (!p->metas().variants.has_value()) return 0;
  const auto &vmap = p->metas().variants.value();
  auto it = vmap.find(std::string(variant_set_name));
  if (it == vmap.end()) return 0;
  c_lightusd_string_replace(out, it->second.c_str());
  return 1;
}

/* ---- Stage default-prim convenience ---- */
int c_lightusd_stage_set_default_prim(CLightUSDStage *stage, const char *name) {
  if (!stage || !name) return 0;
  auto *s = reinterpret_cast<lightusd::Stage *>(stage);
  s->metas().defaultPrim = lightusd::value::token(name);
  return 1;
}
int c_lightusd_stage_get_default_prim(const CLightUSDStage *stage,
                                     c_lightusd_string_t *out_name) {
  if (!stage || !out_name) return 0;
  const auto *s = reinterpret_cast<const lightusd::Stage *>(stage);
  return c_lightusd_string_replace(out_name,
                                  s->metas().defaultPrim.str().c_str());
}

CLightUSDValue *c_lightusd_value_new_asset(const char *asset_path) {
  if (!asset_path) return nullptr;
  lightusd::value::AssetPath ap{std::string(asset_path)};
  auto *p = new lightusd::value::Value(ap);
  return reinterpret_cast<CLightUSDValue *>(p);
}

CLightUSDValue *c_lightusd_value_new_array_asset(uint64_t n,
                                               const char *const *paths) {
  if (n && !paths) return nullptr;
  std::vector<lightusd::value::AssetPath> arr;
  arr.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    arr.emplace_back(paths[i] ? std::string(paths[i]) : std::string());
  }
  auto *p = new lightusd::value::Value(arr);
  return reinterpret_cast<CLightUSDValue *>(p);
}

/* ---- Attribute connections / metadata ---- */

/* Helper: locate or insert a Property attribute in the prim's props map.
 * Returns pointer to the Property entry, or nullptr if the prim type does
 * not expose a writable props map. */
static lightusd::Property *locate_or_insert_attr_prop(
    Prim *p, const std::string &name) {
  using namespace lightusd;
  uint32_t tid = p->get_data().type_id();
#define LOCATE(__TID, __TY)                                          \
  case __TID: {                                                      \
    auto *typed = p->get_data().as<__TY>();                          \
    if (!typed) return nullptr;                                      \
    return &typed->props[name];                                      \
  }
  switch (tid) {
    LOCATE(value::TYPE_ID_GEOM_XFORM, Xform)
    LOCATE(value::TYPE_ID_GEOM_MESH, GeomMesh)
    LOCATE(value::TYPE_ID_GEOM_SPHERE, GeomSphere)
    LOCATE(value::TYPE_ID_GEOM_CUBE, GeomCube)
    LOCATE(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
    LOCATE(value::TYPE_ID_GEOM_CONE, GeomCone)
    LOCATE(value::TYPE_ID_GEOM_CAPSULE, GeomCapsule)
    LOCATE(value::TYPE_ID_GEOM_CAMERA, GeomCamera)
    LOCATE(value::TYPE_ID_GEOM_POINTS, GeomPoints)
    LOCATE(value::TYPE_ID_GEOM_GEOMSUBSET, GeomSubset)
    LOCATE(value::TYPE_ID_GEOM_BASIS_CURVES, GeomBasisCurves)
    LOCATE(value::TYPE_ID_GEOM_NURBS_CURVES, GeomNurbsCurves)
    LOCATE(value::TYPE_ID_GEOM_HERMITE_CURVES, GeomHermiteCurves)
    LOCATE(value::TYPE_ID_GEOM_PLANE, GeomPlane)
    LOCATE(value::TYPE_ID_GEOM_CYLINDER_1, GeomCylinder_1)
    LOCATE(value::TYPE_ID_GEOM_CAPSULE_1, GeomCapsule_1)
    LOCATE(value::TYPE_ID_GEOM_TET_MESH, GeomTetMesh)
    LOCATE(value::TYPE_ID_GEOM_NURBS_PATCH, GeomNurbsPatch)
    LOCATE(value::TYPE_ID_GEOM_POINT_INSTANCER, GeomPointInstancer)
    LOCATE(value::TYPE_ID_LUX_SPHERE, SphereLight)
    LOCATE(value::TYPE_ID_LUX_RECT, RectLight)
    LOCATE(value::TYPE_ID_LUX_DISK, DiskLight)
    LOCATE(value::TYPE_ID_LUX_DISTANT, DistantLight)
    LOCATE(value::TYPE_ID_LUX_CYLINDER, CylinderLight)
    LOCATE(value::TYPE_ID_LUX_DOME, DomeLight)
    LOCATE(value::TYPE_ID_LUX_DOME_1, DomeLight_1)
    LOCATE(value::TYPE_ID_LUX_GEOMETRY, GeometryLight)
    LOCATE(value::TYPE_ID_LUX_PORTAL, PortalLight)
    LOCATE(value::TYPE_ID_SCOPE, Scope)
    LOCATE(value::TYPE_ID_MODEL, Model)
    LOCATE(value::TYPE_ID_MATERIAL, Material)
    LOCATE(value::TYPE_ID_SHADER, Shader)
    LOCATE(value::TYPE_ID_NODEGRAPH, NodeGraph)
    LOCATE(value::TYPE_ID_SKEL_ROOT, SkelRoot)
    LOCATE(value::TYPE_ID_SKELETON, Skeleton)
    LOCATE(value::TYPE_ID_SKELANIMATION, SkelAnimation)
    LOCATE(value::TYPE_ID_BLENDSHAPE, BlendShape)
    LOCATE(value::TYPE_ID_PHYSICS_SCENE, PhysicsScene)
    LOCATE(value::TYPE_ID_PHYSICS_JOINT, PhysicsJoint)
    LOCATE(value::TYPE_ID_PHYSICS_REVOLUTE_JOINT, PhysicsRevoluteJoint)
    LOCATE(value::TYPE_ID_PHYSICS_PRISMATIC_JOINT, PhysicsPrismaticJoint)
    LOCATE(value::TYPE_ID_PHYSICS_SPHERICAL_JOINT, PhysicsSphericalJoint)
    LOCATE(value::TYPE_ID_PHYSICS_FIXED_JOINT, PhysicsFixedJoint)
    LOCATE(value::TYPE_ID_PHYSICS_DISTANCE_JOINT, PhysicsDistanceJoint)
    LOCATE(value::TYPE_ID_PHYSICS_COLLISION_GROUP, PhysicsCollisionGroup)
    default: return nullptr;
  }
#undef LOCATE
}

int c_lightusd_prim_add_attribute_connection(CLightUSDPrim *prim,
                                            const char *name,
                                            const char *type_name,
                                            uint64_t n_targets,
                                            const char *const *target_paths,
                                            c_lightusd_string_t *err) {
  using namespace lightusd;
  if (!prim || !name || n_targets == 0 || !target_paths) {
    if (err) c_lightusd_string_replace(err, "invalid args");
    return 0;
  }
  Prim *p = reinterpret_cast<Prim *>(prim);
  Attribute attr;
  attr.set_name(std::string(name));
  if (type_name && *type_name) attr.set_type_name(std::string(type_name));
  std::vector<Path> paths;
  paths.reserve(n_targets);
  for (uint64_t i = 0; i < n_targets; ++i) {
    paths.emplace_back(std::string(target_paths[i]), "");
  }
  if (paths.size() == 1) {
    attr.set_connection(paths[0]);
  } else {
    attr.set_connections(paths);
  }
  Property *slot = locate_or_insert_attr_prop(p, std::string(name));
  if (!slot) {
    if (err) c_lightusd_string_replace(err,
        "Attribute connection unsupported for this prim type");
    return 0;
  }
  *slot = Property(std::move(attr), /*custom*/ false);
  return 1;
}

int c_lightusd_prim_get_attribute_connections(const CLightUSDPrim *prim,
                                             const char *name,
                                             c_lightusd_string_t *out_csv) {
  using namespace lightusd;
  if (!prim || !name || !out_csv) return 0;
  Property prop;
  bool got = lookup_in_props(*P(prim), std::string(name), &prop);
  if (!got) {
    std::string err;
    got = lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop,
                                       &err);
  }
  if (!got) return 0;
  if (!prop.is_attribute()) return 0;
  const Attribute &a = prop.get_attribute();
  if (!a.has_connections()) return 0;
  std::string s;
  const auto &paths = a.connections();
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) s += ",";
    s += paths[i].full_path_name();
  }
  return c_lightusd_string_replace(out_csv, s.c_str());
}

int c_lightusd_prim_attribute_meta_set_string(CLightUSDPrim *prim,
                                             const char *attr_name,
                                             const char *meta_key,
                                             const char *value) {
  if (!prim || !attr_name || !meta_key || !value) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  Property *slot = locate_or_insert_attr_prop(p, std::string(attr_name));
  if (!slot || !slot->is_attribute()) return 0;
  Attribute &a = slot->attribute();
  std::string k(meta_key);
  std::string v(value);
  if (k == "displayName") a.metas().set_displayName(v);
  else if (k == "doc" || k == "documentation") a.metas().set_doc(v);
  else if (k == "displayGroup") a.metas().set_displayGroup(v);
  else if (k == "interpolation") a.metas().set_interpolation(v);
  else if (k == "colorSpace") a.metas().set(k, lightusd::value::token(v));
  else if (k == "variability") {
    if (v == "uniform") a.variability() = lightusd::Variability::Uniform;
    else if (v == "varying") a.variability() = lightusd::Variability::Varying;
    else return 0;
  }
  else a.metas().set(k, v);
  return 1;
}

int c_lightusd_prim_attribute_meta_set_bool(CLightUSDPrim *prim,
                                           const char *attr_name,
                                           const char *meta_key, int value) {
  if (!prim || !attr_name || !meta_key) return 0;
  Prim *p = reinterpret_cast<Prim *>(prim);
  Property *slot = locate_or_insert_attr_prop(p, std::string(attr_name));
  if (!slot || !slot->is_attribute()) return 0;
  Attribute &a = slot->attribute();
  std::string k(meta_key);
  bool b = value ? true : false;
  if (k == "hidden") a.metas().set_hidden(b);
  else if (k == "custom") slot->set_custom(b);
  else a.metas().set(k, b);
  return 1;
}

int c_lightusd_prim_attribute_meta_get_string(const CLightUSDPrim *prim,
                                             const char *attr_name,
                                             const char *meta_key,
                                             c_lightusd_string_t *out) {
  using namespace lightusd;
  if (!prim || !attr_name || !meta_key || !out) return 0;
  Property prop;
  bool got = lookup_in_props(*P(prim), std::string(attr_name), &prop);
  if (!got) {
    std::string err;
    got = lightusd::tydra::GetProperty(*P(prim), std::string(attr_name), &prop,
                                       &err);
  }
  if (!got) return 0;
  if (!prop.is_attribute()) return 0;
  const auto &m = prop.get_attribute().metas();
  std::string k(meta_key);
  if ((k == "displayName") && m.has_displayName())
    return c_lightusd_string_replace(out, m.get_displayName().c_str());
  if (k == "doc" || k == "documentation") {
    /* Try canonical typed (StringData) first, then plain std::string
     * (USDA parser falls back to a generic MetaVariable for any meta
     * without a hand-coded case). */
    if (m.has_doc()) {
      const auto &sd = m.get_doc();
      if (!sd.value.empty())
        return c_lightusd_string_replace(out, sd.value.c_str());
    }
    if (auto v = m.get<std::string>("documentation"))
      return c_lightusd_string_replace(out, v.value().c_str());
    if (auto v = m.get<std::string>("doc"))
      return c_lightusd_string_replace(out, v.value().c_str());
  }
  if (k == "displayGroup" && m.has_displayGroup())
    return c_lightusd_string_replace(out, m.get_displayGroup().c_str());
  if (k == "interpolation" && m.has_interpolation())
    return c_lightusd_string_replace(out, m.get_interpolation().str().c_str());
  if (k == "variability") {
    const Attribute &a = prop.get_attribute();
    const char *vs = a.is_uniform() ? "uniform" : "varying";
    return c_lightusd_string_replace(out, vs);
  }
  if (auto v = m.get<std::string>(k))
    return c_lightusd_string_replace(out, v.value().c_str());
  if (auto v = m.get<lightusd::value::token>(k))
    return c_lightusd_string_replace(out, v.value().str().c_str());
  if (auto v = m.get<lightusd::value::StringData>(k))
    return c_lightusd_string_replace(out, v.value().value.c_str());
  return 0;
}

/* ---- Stage metadata ---- */

int c_lightusd_stage_meta_set_string(CLightUSDStage *stage, const char *key_,
                                    const char *value) {
  if (!stage || !key_ || !value) return 0;
  std::string key(key_);
  std::string val(value);
  auto &m = Sm(stage)->metas();
  if (key == "defaultPrim") {
    m.defaultPrim = lightusd::value::token(val);
    return 1;
  } else if (key == "upAxis") {
    lightusd::Axis ax = lightusd::Axis::Y;
    if (val == "X") ax = lightusd::Axis::X;
    else if (val == "Z") ax = lightusd::Axis::Z;
    else if (val == "Y") ax = lightusd::Axis::Y;
    else return 0;
    m.upAxis = ax;
    return 1;
  } else if (key == "doc" || key == "documentation") {
    m.doc = lightusd::value::StringData(val);
    return 1;
  } else if (key == "comment") {
    m.comment = lightusd::value::StringData(val);
    return 1;
  }
  return 0;
}

int c_lightusd_stage_meta_get_string(const CLightUSDStage *stage, const char *key_,
                                    c_lightusd_string_t *out) {
  if (!stage || !key_ || !out) return 0;
  std::string key(key_);
  const auto &m = S(stage)->metas();
  if (key == "defaultPrim") {
    if (m.defaultPrim.str().empty()) return 0;
    return c_lightusd_string_replace(out, m.defaultPrim.str().c_str());
  } else if (key == "upAxis") {
    if (!m.upAxis.authored()) return 0;
    const char *s = "Y";
    switch (m.upAxis.get_value()) {
      case lightusd::Axis::X: s = "X"; break;
      case lightusd::Axis::Y: s = "Y"; break;
      case lightusd::Axis::Z: s = "Z"; break;
      default: return 0;
    }
    return c_lightusd_string_replace(out, s);
  } else if (key == "doc" || key == "documentation") {
    if (m.doc.value.empty()) return 0;
    return c_lightusd_string_replace(out, m.doc.value.c_str());
  } else if (key == "comment") {
    if (m.comment.value.empty()) return 0;
    return c_lightusd_string_replace(out, m.comment.value.c_str());
  }
  return 0;
}

int c_lightusd_stage_meta_set_double(CLightUSDStage *stage, const char *key_,
                                    double value) {
  if (!stage || !key_) return 0;
  std::string key(key_);
  auto &m = Sm(stage)->metas();
  if (key == "metersPerUnit") { m.metersPerUnit = value; return 1; }
  if (key == "timeCodesPerSecond") { m.timeCodesPerSecond = value; return 1; }
  if (key == "framesPerSecond") { m.framesPerSecond = value; return 1; }
  if (key == "startTimeCode") { m.startTimeCode = value; return 1; }
  if (key == "endTimeCode") { m.endTimeCode = value; return 1; }
  return 0;
}

int c_lightusd_stage_meta_get_double(const CLightUSDStage *stage, const char *key_,
                                    double *out) {
  if (!stage || !key_ || !out) return 0;
  std::string key(key_);
  const auto &m = S(stage)->metas();
#define RET_IF(KEY, FIELD)                          \
  if (key == KEY) {                                 \
    if (!m.FIELD.authored()) return 0;              \
    *out = m.FIELD.get_value();                     \
    return 1;                                       \
  }
  RET_IF("metersPerUnit", metersPerUnit)
  RET_IF("timeCodesPerSecond", timeCodesPerSecond)
  RET_IF("framesPerSecond", framesPerSecond)
  RET_IF("startTimeCode", startTimeCode)
  RET_IF("endTimeCode", endTimeCode)
#undef RET_IF
  return 0;
}

/* ---- Relationship author / read ---- */

int c_lightusd_prim_add_relationship(CLightUSDPrim *prim, const char *name,
                                    uint64_t n_targets,
                                    const char *const *target_paths,
                                    c_lightusd_string_t *err) {
  using namespace lightusd;
  if (!prim || !name) {
    if (err) c_lightusd_string_replace(err, "null prim or name");
    return 0;
  }
  Prim *p = reinterpret_cast<Prim *>(prim);
  lightusd::Relationship rel;
  if (n_targets == 0 || !target_paths) {
    rel.set_novalue();
  } else if (n_targets == 1) {
    rel.set(lightusd::Path(std::string(target_paths[0]), ""));
  } else {
    std::vector<lightusd::Path> pv;
    pv.reserve(n_targets);
    for (uint64_t i = 0; i < n_targets; ++i) {
      pv.emplace_back(std::string(target_paths[i]), "");
    }
    rel.set(std::move(pv));
  }
  Property prop(rel, /*custom*/ false);

  std::string nm(name);
  uint32_t tid = p->data().type_id();
#define INSERT_REL(__TID, __TY)                              \
  case __TID: {                                              \
    auto *typed = p->get_data().as<__TY>();                  \
    if (!typed) { if (err) c_lightusd_string_replace(err, "null typed"); return 0; } \
    typed->props[nm] = prop;                                 \
    return 1;                                                \
  }
  switch (tid) {
    INSERT_REL(value::TYPE_ID_GEOM_XFORM, Xform)
    INSERT_REL(value::TYPE_ID_GEOM_MESH, GeomMesh)
    INSERT_REL(value::TYPE_ID_GEOM_SPHERE, GeomSphere)
    INSERT_REL(value::TYPE_ID_GEOM_CUBE, GeomCube)
    INSERT_REL(value::TYPE_ID_GEOM_CYLINDER, GeomCylinder)
    INSERT_REL(value::TYPE_ID_GEOM_CONE, GeomCone)
    INSERT_REL(value::TYPE_ID_GEOM_CAPSULE, GeomCapsule)
    INSERT_REL(value::TYPE_ID_GEOM_CAMERA, GeomCamera)
    INSERT_REL(value::TYPE_ID_GEOM_POINTS, GeomPoints)
    INSERT_REL(value::TYPE_ID_GEOM_GEOMSUBSET, GeomSubset)
    INSERT_REL(value::TYPE_ID_GEOM_BASIS_CURVES, GeomBasisCurves)
    INSERT_REL(value::TYPE_ID_GEOM_NURBS_CURVES, GeomNurbsCurves)
    INSERT_REL(value::TYPE_ID_GEOM_HERMITE_CURVES, GeomHermiteCurves)
    INSERT_REL(value::TYPE_ID_GEOM_PLANE, GeomPlane)
    INSERT_REL(value::TYPE_ID_GEOM_CYLINDER_1, GeomCylinder_1)
    INSERT_REL(value::TYPE_ID_GEOM_CAPSULE_1, GeomCapsule_1)
    INSERT_REL(value::TYPE_ID_GEOM_TET_MESH, GeomTetMesh)
    INSERT_REL(value::TYPE_ID_GEOM_NURBS_PATCH, GeomNurbsPatch)
    INSERT_REL(value::TYPE_ID_GEOM_POINT_INSTANCER, GeomPointInstancer)
    INSERT_REL(value::TYPE_ID_LUX_SPHERE, SphereLight)
    INSERT_REL(value::TYPE_ID_LUX_RECT, RectLight)
    INSERT_REL(value::TYPE_ID_LUX_DISK, DiskLight)
    INSERT_REL(value::TYPE_ID_LUX_DISTANT, DistantLight)
    INSERT_REL(value::TYPE_ID_LUX_CYLINDER, CylinderLight)
    INSERT_REL(value::TYPE_ID_LUX_DOME, DomeLight)
    INSERT_REL(value::TYPE_ID_LUX_DOME_1, DomeLight_1)
    INSERT_REL(value::TYPE_ID_LUX_GEOMETRY, GeometryLight)
    INSERT_REL(value::TYPE_ID_LUX_PORTAL, PortalLight)
    INSERT_REL(value::TYPE_ID_SCOPE, Scope)
    INSERT_REL(value::TYPE_ID_MODEL, Model)
    INSERT_REL(value::TYPE_ID_MATERIAL, Material)
    INSERT_REL(value::TYPE_ID_SHADER, Shader)
    INSERT_REL(value::TYPE_ID_NODEGRAPH, NodeGraph)
    INSERT_REL(value::TYPE_ID_SKEL_ROOT, SkelRoot)
    INSERT_REL(value::TYPE_ID_SKELETON, Skeleton)
    INSERT_REL(value::TYPE_ID_SKELANIMATION, SkelAnimation)
    INSERT_REL(value::TYPE_ID_BLENDSHAPE, BlendShape)
    INSERT_REL(value::TYPE_ID_PHYSICS_SCENE, PhysicsScene)
    INSERT_REL(value::TYPE_ID_PHYSICS_JOINT, PhysicsJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_REVOLUTE_JOINT, PhysicsRevoluteJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_PRISMATIC_JOINT, PhysicsPrismaticJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_SPHERICAL_JOINT, PhysicsSphericalJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_FIXED_JOINT, PhysicsFixedJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_DISTANCE_JOINT, PhysicsDistanceJoint)
    INSERT_REL(value::TYPE_ID_PHYSICS_COLLISION_GROUP, PhysicsCollisionGroup)
    default: {
      if (err) c_lightusd_string_replace(
          err, "Relationship author not supported for this prim type");
      return 0;
    }
  }
#undef INSERT_REL
}

int c_lightusd_prim_get_relationship_targets(const CLightUSDPrim *prim,
                                            const char *name,
                                            c_lightusd_string_t *out_csv) {
  using namespace lightusd;
  if (!prim || !name || !out_csv) return 0;
  Property prop;
  bool got = lookup_in_props(*P(prim), std::string(name), &prop);
  if (!got) {
    std::string err;
    got = lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop,
                                       &err);
  }
  /* GPrim-derived prims store material:binding in a typed RelationshipProperty
   * field outside the props map; surface that to callers. */
  if (!got) {
    const std::string nm(name);
    auto try_gprim_binding = [&](const auto *g) -> bool {
      if (!g) return false;
      if (nm == "material:binding" && g->materialBinding.authored()) {
        prop = Property(g->materialBinding.relationship(), false);
        return true;
      }
      if (nm == "material:binding:preview" &&
          g->materialBindingPreview.authored()) {
        prop = Property(g->materialBindingPreview.relationship(), false);
        return true;
      }
      if (nm == "material:binding:full" &&
          g->materialBindingFull.authored()) {
        prop = Property(g->materialBindingFull.relationship(), false);
        return true;
      }
      return false;
    };
    const Prim &pp = *P(prim);
    uint32_t tid = pp.data().type_id();
    switch (tid) {
      case value::TYPE_ID_GEOM_MESH:
        got = try_gprim_binding(pp.data().as<GeomMesh>()); break;
      case value::TYPE_ID_GEOM_SPHERE:
        got = try_gprim_binding(pp.data().as<GeomSphere>()); break;
      case value::TYPE_ID_GEOM_CUBE:
        got = try_gprim_binding(pp.data().as<GeomCube>()); break;
      case value::TYPE_ID_GEOM_CONE:
        got = try_gprim_binding(pp.data().as<GeomCone>()); break;
      case value::TYPE_ID_GEOM_CYLINDER:
        got = try_gprim_binding(pp.data().as<GeomCylinder>()); break;
      case value::TYPE_ID_GEOM_CAPSULE:
        got = try_gprim_binding(pp.data().as<GeomCapsule>()); break;
      case value::TYPE_ID_GEOM_POINTS:
        got = try_gprim_binding(pp.data().as<GeomPoints>()); break;
      case value::TYPE_ID_GEOM_BASIS_CURVES:
        got = try_gprim_binding(pp.data().as<GeomBasisCurves>()); break;
      case value::TYPE_ID_GEOM_NURBS_CURVES:
        got = try_gprim_binding(pp.data().as<GeomNurbsCurves>()); break;
      case value::TYPE_ID_GEOM_HERMITE_CURVES:
        got = try_gprim_binding(pp.data().as<GeomHermiteCurves>()); break;
      case value::TYPE_ID_GEOM_PLANE:
        got = try_gprim_binding(pp.data().as<GeomPlane>()); break;
      case value::TYPE_ID_GEOM_TET_MESH:
        got = try_gprim_binding(pp.data().as<GeomTetMesh>()); break;
      case value::TYPE_ID_GEOM_NURBS_PATCH:
        got = try_gprim_binding(pp.data().as<GeomNurbsPatch>()); break;
      default: break;
    }
  }
  if (!got) return 0;
  if (!prop.is_relationship()) return 0;
  const auto &rel = prop.get_relationship();
  std::string s;
  if (rel.is_path()) {
    s = rel.targetPath.full_path_name();
  } else if (rel.is_pathvector()) {
    for (size_t i = 0; i < rel.targetPathVector.size(); ++i) {
      if (i) s += ",";
      s += rel.targetPathVector[i].full_path_name();
    }
  } else {
    return 0;
  }
  return c_lightusd_string_replace(out_csv, s.c_str());
}

/* ---- TimeSamples authoring ---- */

int c_lightusd_prim_set_attribute_timesample(CLightUSDPrim *prim,
                                            const char *name, double time,
                                            const CLightUSDValue *value,
                                            const char *type_name,
                                            c_lightusd_string_t *err) {
  using namespace lightusd;
  if (!prim || !name || !value) {
    if (err) c_lightusd_string_replace(err, "null arg");
    return 0;
  }
  Prim *p = reinterpret_cast<Prim *>(prim);
  Property *slot = locate_or_insert_attr_prop(p, std::string(name));
  if (!slot) {
    if (err) c_lightusd_string_replace(err,
        "TimeSamples not supported for this prim type");
    return 0;
  }
  Attribute attr;
  if (slot->is_attribute()) attr = slot->get_attribute();
  if (attr.name().empty()) attr.set_name(std::string(name));

  const value::Value *val = reinterpret_cast<const value::Value *>(value);
  std::string ty = (type_name && *type_name) ? std::string(type_name)
                                             : val->type_name();
  if (type_name && *type_name) {
    attr.set_type_name(ty);
  } else if (attr.type_name().empty()) {
    attr.set_type_name(ty);
  }

  std::string aerr;
  if (!attr.get_var().ts_raw().add_sample(time, *val, &aerr)) {
    if (err) c_lightusd_string_replace(err, aerr.c_str());
    return 0;
  }
  *slot = Property(std::move(attr), /*custom*/ false);
  return 1;
}

uint64_t c_lightusd_prim_get_attribute_timesample_count(
    const CLightUSDPrim *prim, const char *name) {
  if (!prim || !name) return 0;
  Property prop;
  if (!lookup_in_props(*P(prim), std::string(name), &prop)) {
    std::string err;
    if (!lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop, &err)) {
      return 0;
    }
  }
  if (!prop.is_attribute()) return 0;
  const auto &pv = prop.get_attribute().get_var();
  if (!pv.has_timesamples()) return 0;
  return static_cast<uint64_t>(pv.ts_raw().size());
}

int c_lightusd_prim_get_attribute_timesample(const CLightUSDPrim *prim,
                                            const char *name, uint64_t index,
                                            double *out_time,
                                            CLightUSDValue **out_value) {
  if (!prim || !name || !out_time || !out_value) return 0;
  Property prop;
  if (!lookup_in_props(*P(prim), std::string(name), &prop)) {
    std::string err;
    if (!lightusd::tydra::GetProperty(*P(prim), std::string(name), &prop, &err)) {
      return 0;
    }
  }
  if (!prop.is_attribute()) return 0;
  const auto &pv = prop.get_attribute().get_var();
  if (!pv.has_timesamples()) return 0;
  const auto &ts = pv.ts_raw();
  if (index >= ts.size()) return 0;
  const auto &samples = ts.get_samples();
  *out_time = samples[size_t(index)].t;
  *out_value = reinterpret_cast<CLightUSDValue *>(
      new lightusd::value::Value(samples[size_t(index)].value));
  return 1;
}

}  // extern "C"
