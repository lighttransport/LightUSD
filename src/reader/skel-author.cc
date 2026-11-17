// SPDX-License-Identifier: Apache-2.0
#include "skel-author.hh"

#include <cmath>
#include <map>

#include "c/lightusd-core-c.h"  // lu_float_to_half

namespace lightusd {
namespace skel {
namespace {
using ::lightusd::Value;

void Set(PrimSpec* p, const std::string& name, Value value, const std::string& type,
         bool uniform = false) {
  p->upsert_property(name, std::move(value), uniform ? PropSlot::kFlagUniform : 0);
  p->set_property_type_name(name, type);
}

Value Matrices(const std::vector<Mat>& m) {
  std::vector<double> flat;
  flat.reserve(m.size() * 16);
  for (const Mat& x : m) flat.insert(flat.end(), x.begin(), x.end());
  return Value::MakeArrayFromRaw(TypeId::Matrix4d, flat.data(), m.size());
}

std::string Identifier(const std::string& s) {
  std::string out;
  for (unsigned char c : s) out += (std::isalnum(c) || c == '_') ? char(c) : '_';
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
  return out;
}
}  // namespace

Mat Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}

bool Invert(const Mat& m, Mat* out) {
  // Gauss-Jordan on [m | I].
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    if (std::fabs(a[piv][c]) < 1e-300) return false;
    if (piv != c)
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) (*out)[i * 4 + j] = a[i][j + 4];
  return true;
}

void Decompose(const Mat& m, float t[3], float q[4], float s[3]) {
  // Rows 0..2 are the scaled basis vectors, row 3 the translation.
  for (int k = 0; k < 3; ++k) t[k] = static_cast<float>(m[12 + k]);
  double r[3][3];
  for (int i = 0; i < 3; ++i) {
    const double len = std::sqrt(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] +
                                 m[i * 4 + 2] * m[i * 4 + 2]);
    s[i] = static_cast<float>(len);
    for (int j = 0; j < 3; ++j) r[i][j] = len > 0 ? m[i * 4 + j] / len : (i == j ? 1 : 0);
  }
  // Row-vector rotation matrix R (v' = v R) is the transpose of the column
  // form; quaternion from the column form M = R^T.
  const double m00 = r[0][0], m11 = r[1][1], m22 = r[2][2];
  const double m01 = r[1][0], m10 = r[0][1], m02 = r[2][0], m20 = r[0][2], m12 = r[2][1],
               m21 = r[1][2];
  double w, x, y, z;
  const double tr = m00 + m11 + m22;
  if (tr > 0) {
    const double k = 0.5 / std::sqrt(tr + 1.0);
    w = 0.25 / k;
    x = (m21 - m12) * k;
    y = (m02 - m20) * k;
    z = (m10 - m01) * k;
  } else if (m00 > m11 && m00 > m22) {
    const double k = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
    w = (m21 - m12) / k;
    x = 0.25 * k;
    y = (m01 + m10) / k;
    z = (m02 + m20) / k;
  } else if (m11 > m22) {
    const double k = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
    w = (m02 - m20) / k;
    x = (m01 + m10) / k;
    y = 0.25 * k;
    z = (m12 + m21) / k;
  } else {
    const double k = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
    w = (m10 - m01) / k;
    x = (m02 + m20) / k;
    y = (m12 + m21) / k;
    z = 0.25 * k;
  }
  q[0] = static_cast<float>(w);
  q[1] = static_cast<float>(x);
  q[2] = static_cast<float>(y);
  q[3] = static_cast<float>(z);
}

std::vector<std::string> JointPaths(const Skeleton& s) {
  std::vector<std::string> paths(s.names.size());
  std::map<std::pair<int, std::string>, int> used;  // (parent, name) -> count
  for (size_t i = 0; i < s.names.size(); ++i) {
    const int p = s.parent[i];
    std::string name = Identifier(s.names[i]);
    int& n = used[{p, name}];
    if (n++) name += "_" + std::to_string(n);
    paths[i] = p >= 0 ? paths[size_t(p)] + "/" + name : name;
  }
  return paths;
}

bool AuthorSkeleton(Layer* layer, const std::string& skel_path, const Skeleton& s,
                    const Animation* anim, std::string* err) {
  const size_t n = s.names.size();
  if (s.parent.size() != n || s.bind.size() != n || s.rest.size() != n) {
    if (err) *err = "skeleton arrays have different sizes";
    return false;
  }
  const auto skel_id = layer->define_prim_at_path(skel_path, "Skeleton");
  auto id = skel_id;
  PrimSpec* p = id == UINT32_MAX ? nullptr : layer->prim_mutable(id);
  if (!p) {
    if (err) *err = "cannot define Skeleton " + skel_path;
    return false;
  }
  const std::vector<std::string> paths = JointPaths(s);
  Set(p, "joints", Value::MakeTokenArray(paths), "token[]", true);
  Set(p, "jointNames", Value::MakeTokenArray(s.names), "token[]", true);
  Set(p, "bindTransforms", Matrices(s.bind), "matrix4d[]", true);
  Set(p, "restTransforms", Matrices(s.rest), "matrix4d[]", true);
  if (!anim || (anim->times.empty() && anim->blend_shapes.empty())) return true;

  const std::string anim_path = skel_path + "/Anim";
  id = layer->define_prim_at_path(anim_path, "SkelAnimation");
  PrimSpec* a = id == UINT32_MAX ? nullptr : layer->prim_mutable(id);
  if (!a) {
    if (err) *err = "cannot define SkelAnimation " + anim_path;
    return false;
  }
  Set(a, "joints", Value::MakeTokenArray(paths), "token[]", true);
  const uint16_t ts = PropSlot::kFlagTimeSampled | PropSlot::kFlagArray;
  if (!anim->blend_shapes.empty()) {
    const size_t nb = anim->blend_shapes.size();
    Set(a, "blendShapes", Value::MakeTokenArray(anim->blend_shapes), "token[]", true);
    if (!anim->blend_weights.empty()) {
      const PropNameId wid = GetPropNameTable().intern("blendShapeWeights");
      a->add_property_slot(wid, TypeId::Float, ts);
      a->set_property_type_name("blendShapeWeights", "float[]");
      for (size_t k = 0; k < anim->times.size() && k < anim->blend_weights.size(); ++k) {
        if (anim->blend_weights[k].size() != nb) {
          if (err) *err = "blend shape weights have the wrong width";
          return false;
        }
        a->add_time_sample(wid, anim->times[k],
                           Value::MakeArrayFromRaw(TypeId::Float, anim->blend_weights[k].data(), nb));
      }
    } else {
      std::vector<float> w = anim->blend_default;
      w.resize(nb, 0.0f);
      Set(a, "blendShapeWeights", Value::MakeFloatArray(std::move(w)), "float[]");
    }
  }
  if (!n || anim->times.empty()) {  // no joint channels to sample
    p = layer->prim_mutable(skel_id);
    p->meta().apiSchemas().push_back("SkelBindingAPI");
    p->meta().apiSchemasQualifier() = "prepend";
    p->meta().setApiSchemasAuthored();
    p->set_relationship_targets("skel:animationSource", {Path(anim_path)});
    if (!anim->times.empty()) {
      auto& meta = layer->meta();
      meta.timeCodesPerSecond = anim->fps;
      meta.timeCodesPerSecond_set = true;
      meta.startTimeCode = anim->times.front();
      meta.startTimeCode_set = true;
      meta.endTimeCode = anim->times.back();
      meta.endTimeCode_set = true;
    }
    return true;
  }
  const PropNameId tid = GetPropNameTable().intern("translations");
  const PropNameId rid = GetPropNameTable().intern("rotations");
  const PropNameId sid = GetPropNameTable().intern("scales");
  a->add_property_slot(tid, TypeId::Float3, ts);
  a->add_property_slot(rid, TypeId::Quatf, ts);
  a->add_property_slot(sid, TypeId::Half3, ts);
  a->set_property_type_name("translations", "float3[]");
  a->set_property_type_name("rotations", "quatf[]");
  a->set_property_type_name("scales", "half3[]");
  for (size_t k = 0; k < anim->times.size(); ++k) {
    const double t = anim->times[k];
    a->add_time_sample(tid, t,
                       Value::MakeArrayFromRaw(TypeId::Float3, anim->translations[k].data(), n));
    a->add_time_sample(rid, t,
                       Value::MakeArrayFromRaw(TypeId::Quatf, anim->rotations[k].data(), n));
    // half3[]: LightUSD stores half element types as floats (value-storage.c).
    a->add_time_sample(sid, t,
                       Value::MakeArrayFromRaw(TypeId::Half3, anim->scales[k].data(), n));
  }
  // Defining the animation prim may have moved prim storage: look up again.
  p = layer->prim_mutable(skel_id);
  // skel:animationSource is a SkelBindingAPI property: OpenUSD resolves it only
  // when the API is applied to the Skeleton.
  p->meta().apiSchemas().push_back("SkelBindingAPI");
  p->meta().apiSchemasQualifier() = "prepend";
  p->meta().setApiSchemasAuthored();
  p->set_relationship_targets("skel:animationSource", {Path(anim_path)});
  auto& meta = layer->meta();
  meta.timeCodesPerSecond = anim->fps;
  meta.timeCodesPerSecond_set = true;
  meta.startTimeCode = anim->times.front();
  meta.startTimeCode_set = true;
  meta.endTimeCode = anim->times.back();
  meta.endTimeCode_set = true;
  return true;
}

bool AuthorBlendShapes(Layer* layer, const std::string& mesh_path,
                       const std::vector<BlendShape>& shapes, std::string* err) {
  std::vector<std::string> tokens;
  std::vector<Path> targets;
  for (const BlendShape& b : shapes) {
    const size_t np = b.point_indices.size();
    if (b.offsets.size() != 3 * np) {
      if (err) *err = "blend shape " + b.name + ": offsets do not match the point indices";
      return false;
    }
    const std::string path = mesh_path + "/" + b.prim;
    const auto id = layer->define_prim_at_path(path, "BlendShape");
    PrimSpec* p = id == UINT32_MAX ? nullptr : layer->prim_mutable(id);
    if (!p) {
      if (err) *err = "cannot define BlendShape " + path;
      return false;
    }
    Set(p, "offsets", Value::MakeFloatCompArray(b.offsets, TypeId::Vector3f, 3), "vector3f[]",
        true);
    Set(p, "pointIndices", Value::MakeIntArray(b.point_indices), "int[]", true);
    for (const auto& in : b.inbetweens) {
      if (in.offsets.size() != 3 * np) {
        if (err) *err = "blend shape " + b.name + ": in-between size mismatch";
        return false;
      }
      const std::string name = "inbetweens:" + in.name;
      Set(p, name, Value::MakeFloatCompArray(in.offsets, TypeId::Vector3f, 3), "vector3f[]",
          true);
      PropMeta& meta = p->ensure_property_meta(name);
      meta.weight = in.weight;
      meta.authored |= PropMeta::kWeight;
    }
    tokens.push_back(b.name);
    targets.emplace_back(path);
  }
  const auto mid = layer->index_at_path(mesh_path);
  PrimSpec* m = mid == UINT32_MAX ? nullptr : layer->prim_mutable(mid);
  if (!m) {
    if (err) *err = "no mesh " + mesh_path;
    return false;
  }
  Set(m, "skel:blendShapes", Value::MakeTokenArray(tokens), "token[]", true);
  m->set_relationship_targets("skel:blendShapeTargets", targets);
  return true;
}

void BindMesh(PrimSpec* mesh, const std::string& skel_path) {
  auto& schemas = mesh->meta().apiSchemas();
  bool has = false;
  for (const auto& s : schemas) has |= s == "SkelBindingAPI";
  if (!has) {
    schemas.push_back("SkelBindingAPI");
    mesh->meta().apiSchemasQualifier() = "prepend";
    mesh->meta().setApiSchemasAuthored();
  }
  mesh->set_relationship_targets("skel:skeleton", {Path(skel_path)});
}

}  // namespace skel
}  // namespace lightusd
