// SPDX-License-Identifier: Apache-2.0
#include "skinning.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "displacement_bake.hh"
#include "tydra/scene-access.hh"  // SkinPointsLBS, ConcatJointTransforms, ListPrims
#include "usdSkel.hh"             // SkelAnimation
#include "xform.hh"               // inverse, to_matrix

namespace lusdview {

namespace {

namespace tydra = lightusd::tydra;
using lightusd::legacy::value::matrix4d;
using lightusd::legacy::value::point3f;
using lightusd::legacy::value::quatf;

// Evaluate a flat keyframe sampler (comps floats per key) at time code `t`.
// Held or linear interpolation; clamps outside the key range.
void EvalSampler(const tydra::KeyframeSampler& s, double t, int comps,
                 float* out) {
  const size_t n = s.times.size();
  if (n == 0 || s.values.size() < n * static_cast<size_t>(comps)) {
    for (int i = 0; i < comps; ++i) out[i] = 0.0f;
    return;
  }
  auto key = [&](size_t k, int i) { return s.values[k * comps + i]; };
  if (t <= s.times[0]) {
    for (int i = 0; i < comps; ++i) out[i] = key(0, i);
    return;
  }
  if (t >= s.times[n - 1]) {
    for (int i = 0; i < comps; ++i) out[i] = key(n - 1, i);
    return;
  }
  size_t k0 = 0;
  while (k0 + 1 < n && s.times[k0 + 1] <= t) ++k0;
  const size_t k1 = k0 + 1;
  float a = 0.0f;
  if (s.interpolation != tydra::AnimationInterpolation::Step) {
    const float span = s.times[k1] - s.times[k0];
    a = span > 0.0f ? static_cast<float>((t - s.times[k0]) / span) : 0.0f;
  }
  for (int i = 0; i < comps; ++i) {
    out[i] = key(k0, i) * (1.0f - a) + key(k1, i) * a;
  }
}

// Build a joint-local transform from animated TRS (row-vector convention,
// matching SkelMakeTransform: translation in row 3, scale applied per row).
matrix4d MakeLocal(const float t[3], const quatf& r, const float s[3]) {
  matrix4d m = lightusd::legacy::to_matrix(r);
  m.m[0][0] *= s[0]; m.m[0][1] *= s[0]; m.m[0][2] *= s[0];
  m.m[1][0] *= s[1]; m.m[1][1] *= s[1]; m.m[1][2] *= s[1];
  m.m[2][0] *= s[2]; m.m[2][1] *= s[2]; m.m[2][2] *= s[2];
  m.m[3][0] = t[0]; m.m[3][1] = t[1]; m.m[3][2] = t[2];
  return m;
}

bool MatrixNearlyIdentity(const matrix4d& m) {
  const matrix4d ident = matrix4d::identity();
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (std::abs(m.m[r][c] - ident.m[r][c]) > 1e-8) return false;
    }
  }
  return true;
}

bool AllIdentity(const std::vector<matrix4d>& m) {
  if (m.empty()) return false;
  for (const matrix4d& v : m) {
    if (!MatrixNearlyIdentity(v)) return false;
  }
  return true;
}

bool IsPointJointSkeleton(const tydra::SkelHierarchy& skel) {
  const size_t nj = skel.num_joints();
  if (nj < 512 || skel.parent_joint_indices.empty()) return false;
  // Some exporters encode dense vertex/point deformation as a huge star-shaped
  // UsdSkel rig. Its rotation/scale channels are not meaningful for LBS, and
  // applying them collapses the mesh; translations carry the deformation.
  if (nj >= 1024) return true;
  if (skel.root_node.children.size() + 1 == nj) return true;
  size_t roots = 0;
  size_t rootChildren = 0;
  for (int parent : skel.parent_joint_indices) {
    if (parent < 0) {
      ++roots;
    } else if (parent == 0) {
      ++rootChildren;
    }
  }
  return roots == 1 && rootChildren + 1 == nj;
}

const tydra::AnimationClip* FindSkeletonClip(const tydra::RenderScene& render,
                                             const tydra::SkelHierarchy& skel,
                                             int skelId) {
  auto hasChannels = [&](const tydra::AnimationClip& clip) {
    for (const tydra::AnimationChannel& ch : clip.channels) {
      if (ch.target_type == tydra::ChannelTargetType::SkeletonJoint &&
          ch.skeleton_id == skelId) {
        return true;
      }
    }
    return false;
  };
  if (skel.anim_id >= 0 && skel.anim_id < static_cast<int>(render.animations.size())) {
    const auto& clip = render.animations[static_cast<size_t>(skel.anim_id)];
    if (hasChannels(clip)) return &clip;
  }
  for (const tydra::AnimationClip& clip : render.animations) {
    if (hasChannels(clip)) return &clip;
  }
  return nullptr;
}

double FirstSkeletonSampleTime(const tydra::AnimationClip& clip, int skelId) {
  double first = std::numeric_limits<double>::infinity();
  for (const tydra::AnimationChannel& ch : clip.channels) {
    if (ch.target_type != tydra::ChannelTargetType::SkeletonJoint) continue;
    if (ch.skeleton_id != skelId) continue;
    if (ch.sampler < 0 || ch.sampler >= static_cast<int>(clip.samplers.size())) {
      continue;
    }
    const auto& times = clip.samplers[static_cast<size_t>(ch.sampler)].times;
    if (!times.empty()) first = std::min(first, static_cast<double>(times.front()));
  }
  return std::isfinite(first) ? first : 0.0;
}

void BuildJointLocals(const tydra::AnimationClip* clip, int skelId, double t,
                      const std::vector<matrix4d>& baseLocal,
                      bool translationOnly,
                      std::vector<matrix4d>* localOut) {
  const size_t nj = baseLocal.size();
  std::vector<matrix4d> local = baseLocal;
  std::vector<bool> animated(nj, false);
  std::vector<float> T(nj * 3, 0.0f);
  std::vector<quatf> R(nj);
  std::vector<float> S(nj * 3, 1.0f);
  // Default each joint's TRS from its rest-pose local transform, so a joint
  // whose animation authors only some components (e.g. rotation, the common
  // Blender export) keeps its rest bone offset / scale instead of collapsing.
  for (size_t j = 0; j < nj; ++j) {
    R[j].imag[0] = R[j].imag[1] = R[j].imag[2] = 0.0f;
    R[j].real = 1.0f;
    lightusd::legacy::value::double3 dt, ds;
    lightusd::legacy::value::quatd dr;
    if (lightusd::legacy::decompose(baseLocal[j], &dt, &dr, &ds)) {
      T[j * 3 + 0] = static_cast<float>(dt[0]);
      T[j * 3 + 1] = static_cast<float>(dt[1]);
      T[j * 3 + 2] = static_cast<float>(dt[2]);
      R[j].imag[0] = static_cast<float>(dr.imag[0]);
      R[j].imag[1] = static_cast<float>(dr.imag[1]);
      R[j].imag[2] = static_cast<float>(dr.imag[2]);
      R[j].real = static_cast<float>(dr.real);
      S[j * 3 + 0] = static_cast<float>(ds[0]);
      S[j * 3 + 1] = static_cast<float>(ds[1]);
      S[j * 3 + 2] = static_cast<float>(ds[2]);
    }
  }

  // Override with animated TRS from any SkeletonJoint channel targeting this
  // skeleton (a present channel replaces that component; absent ones keep rest).
  if (clip) {
    for (const tydra::AnimationChannel& ch : clip->channels) {
      if (ch.target_type != tydra::ChannelTargetType::SkeletonJoint) continue;
      if (ch.skeleton_id != skelId) continue;
      if (ch.joint_id < 0 || ch.joint_id >= static_cast<int>(nj)) continue;
      if (ch.sampler < 0 ||
          ch.sampler >= static_cast<int>(clip->samplers.size())) {
        continue;
      }
      const tydra::KeyframeSampler& smp =
          clip->samplers[static_cast<size_t>(ch.sampler)];
      const size_t j = static_cast<size_t>(ch.joint_id);
      animated[j] = true;
      if (ch.path == tydra::AnimationPath::Translation) {
        // UsdSkelAnimation translations are joint-local components and replace
        // the corresponding rest-pose component at the evaluated time.
        EvalSampler(smp, t, 3, &T[j * 3]);
      } else if (!translationOnly && ch.path == tydra::AnimationPath::Scale) {
        EvalSampler(smp, t, 3, &S[j * 3]);
      } else if (!translationOnly && ch.path == tydra::AnimationPath::Rotation) {
        float q[4];
        EvalSampler(smp, t, 4, q);  // x, y, z, w
        float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; len = 1.0f; }
        R[j].imag[0] = q[0] / len;
        R[j].imag[1] = q[1] / len;
        R[j].imag[2] = q[2] / len;
        R[j].real = q[3] / len;
      }
    }
  }

  for (size_t j = 0; j < nj; ++j) {
    if (animated[j]) local[j] = MakeLocal(&T[j * 3], R[j], &S[j * 3]);
  }
  *localOut = std::move(local);
}

bool BuildSkeletonPose(const tydra::RenderScene& render, int skelId, double t,
                       std::vector<matrix4d>* worldOut,
                       std::vector<matrix4d>* bindWorldOut) {
  if (!worldOut) return false;
  if (skelId < 0 || skelId >= static_cast<int>(render.skeletons.size())) {
    return false;
  }
  const tydra::SkelHierarchy& skel = render.skeletons[static_cast<size_t>(skelId)];
  const size_t nj = skel.num_joints();
  if (nj == 0 || skel.bind_transforms.size() != nj ||
      skel.rest_transforms.size() != nj) {
    return false;
  }

  const tydra::AnimationClip* clip = FindSkeletonClip(render, skel, skelId);
  const bool synthesizeBindFromAnimation =
      clip && nj > 1 && AllIdentity(skel.bind_transforms) &&
      AllIdentity(skel.rest_transforms);

  std::vector<matrix4d> bindWorld = skel.bind_transforms;
  std::vector<matrix4d> baseLocal = skel.rest_transforms;
  const bool pointJointSkeleton = IsPointJointSkeleton(skel);
  bool translationOnly = pointJointSkeleton;
  if (synthesizeBindFromAnimation) {
    const double bindTime = FirstSkeletonSampleTime(*clip, skelId);
    BuildJointLocals(clip, skelId, bindTime, skel.rest_transforms,
                     /*translationOnly=*/false, &baseLocal);
    if (!tydra::ConcatJointTransforms(skel.parent_joint_indices, baseLocal, &bindWorld)) {
      return false;
    }
  } else {
    std::vector<matrix4d> restWorld;
    if (tydra::ConcatJointTransforms(skel.parent_joint_indices,
                                     skel.rest_transforms, &restWorld)) {
      if (pointJointSkeleton) {
        bindWorld = std::move(restWorld);
      }
    }
  }

  std::vector<matrix4d> local;
  BuildJointLocals(clip, skelId, t, baseLocal, translationOnly, &local);

  if (!tydra::ConcatJointTransforms(skel.parent_joint_indices, local, worldOut)) {
    return false;
  }
  if (bindWorldOut) *bindWorldOut = std::move(bindWorld);
  return worldOut->size() == nj;
}

}  // namespace

bool BuildSkeletonJointWorlds(const tydra::RenderScene& render, int skelId,
                              double timecode,
                              std::vector<matrix4d>* worldOut) {
  return BuildSkeletonPose(render, skelId, timecode, worldOut, nullptr);
}

// Per-skeleton skinning matrices skinMat[j] = inverse(bind[j]) * posedWorld[j]
// (row-vector: a point in bind space maps to posed world). Joints without an
// animation channel keep their rest-pose local transform.
bool BuildSkinningMatrices(const tydra::RenderScene& render, int skelId,
                           double t, std::vector<matrix4d>* skinOut) {
  if (!skinOut) return false;
  if (skelId < 0 || skelId >= static_cast<int>(render.skeletons.size())) {
    return false;
  }
  const tydra::SkelHierarchy& skel = render.skeletons[static_cast<size_t>(skelId)];
  const size_t nj = skel.num_joints();
  if (nj == 0 || skel.bind_transforms.size() != nj) {
    return false;
  }

  std::vector<matrix4d> world;
  std::vector<matrix4d> bindWorld;
  if (!BuildSkeletonPose(render, skelId, t, &world, &bindWorld)) return false;

  skinOut->resize(nj);
  for (size_t j = 0; j < nj; ++j) {
    (*skinOut)[j] = lightusd::legacy::inverse(bindWorld[j]) * world[j];
  }
  return true;
}

namespace {

// Map of blendshape prim name -> animated weight at time `t`, gathered from all
// SkelAnimation prims in the stage (Tydra does not emit blendShapeWeights).
std::unordered_map<std::string, float> GatherBlendWeights(
    const lightusd::legacy::Stage& stage, double t) {
  std::unordered_map<std::string, float> out;
  tydra::PathPrimMap<lightusd::legacy::SkelAnimation> anims;
  if (!tydra::ListPrims(stage, anims)) return out;
  for (auto& kv : anims) {
    lightusd::legacy::SkelAnimation* sa = const_cast<lightusd::legacy::SkelAnimation*>(kv.second);
    if (!sa) continue;
    std::vector<lightusd::legacy::value::token> names;
    if (!sa->get_blendShapes(&names) || names.empty()) continue;
    std::vector<float> weights;
    if (!sa->get_blendShapeWeights(&weights, t)) continue;
    const size_t n = std::min(names.size(), weights.size());
    for (size_t i = 0; i < n; ++i) out[names[i].str()] = weights[i];
  }
  return out;
}

// Read a BlendShape prim's in-between shapes from its `inbetweens:*` attributes
// (vector3f[] offsets + a `weight` attr-meta). Sorted ascending by weight.
InbetweenSamples ReadInbetweensFromPrim(const lightusd::legacy::BlendShape& bs) {
  InbetweenSamples out;
  for (const auto& kv : bs.props) {
    if (kv.first.rfind("inbetweens:", 0) != 0) continue;  // namespace prefix
    const lightusd::legacy::Property& p = kv.second;
    if (!p.is_attribute()) continue;
    const lightusd::legacy::Attribute& a = p.get_attribute();
    if (!a.metas().has_weight()) continue;
    std::vector<lightusd::legacy::value::vector3f> offs;
    if (!a.get_value(&offs)) continue;
    out.emplace_back(static_cast<float>(a.metas().get_weight()), std::move(offs));
  }
  std::sort(out.begin(), out.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
  return out;
}

// Bracket a target weight `w` within the implied sample table {0, ibWeights..., 1}
// (ibWeights ascending). Returns table indices [lo, hi] (0 == implicit zero, last
// == primary) and the lerp parameter t (extrapolates outside [0,1]).
struct MorphBracket { int lo; int hi; float t; };
MorphBracket FindMorphBracket(const std::vector<float>& ibWeights, float w) {
  const int N = static_cast<int>(ibWeights.size());
  auto wAt = [&](int i) -> float {
    return i == 0 ? 0.0f : (i == N + 1 ? 1.0f : ibWeights[i - 1]);
  };
  int hi = 1;
  while (hi < N + 1 && w > wAt(hi)) ++hi;
  const int lo = hi - 1;
  const float denom = wAt(hi) - wAt(lo);
  const float t = denom > 1e-12f ? (w - wAt(lo)) / denom : 0.0f;
  return {lo, hi, t};
}

// Per-mesh GPU-morph channel coefficients: fills coeff[0..morphChannelCount-1]
// such that the vertex shader's sum_ch(coeff[ch] * delta_ch) reproduces
// ApplyMorphTarget for every target. For each target weight w, FindMorphBracket
// gives table indices {lo, hi} (0 == rest, last == primary) + lerp t; the bracket
// channels get (1-t) and t (rest index 0 has no channel). Overdrive (t outside
// [0,1]) extrapolates exactly, matching ApplyMorphTarget bit-for-bit.
void EvalMorphChannelCoeffs(const DrawMeshCPU& dm,
                            const std::unordered_map<std::string, float>& weights,
                            std::vector<float>* coeff) {
  coeff->assign(static_cast<size_t>(dm.morphChannelCount), 0.0f);
  for (const MorphTargetChannelsCPU& tc : dm.morphTargetChannels) {
    if (tc.usdWeights.empty()) continue;
    auto it = weights.find(tc.name);
    if (it == weights.end() || it->second == 0.0f) continue;
    // ibWeights = usdWeights without the trailing 1.0 (primary).
    std::vector<float> ibW(tc.usdWeights.begin(), tc.usdWeights.end() - 1);
    const MorphBracket br = FindMorphBracket(ibW, it->second);
    if (br.lo >= 1 && size_t(br.lo - 1) < tc.channelIds.size())
      (*coeff)[tc.channelIds[br.lo - 1]] += (1.0f - br.t);
    if (br.hi >= 1 && size_t(br.hi - 1) < tc.channelIds.size())
      (*coeff)[tc.channelIds[br.hi - 1]] += br.t;
  }
}

bool MeshIsSkinned(const tydra::RenderMesh& m) {
  return m.skel_id >= 0 && !m.joint_and_weights.jointIndices.empty();
}

void NormalizeSkinWeights(std::vector<float>* weights, size_t pointCount,
                          int influencesPerPoint) {
  if (!weights || influencesPerPoint < 1) return;
  const size_t infl = static_cast<size_t>(influencesPerPoint);
  if (weights->size() != pointCount * infl) return;
  for (size_t pi = 0; pi < pointCount; ++pi) {
    const size_t base = pi * infl;
    double sum = 0.0;
    for (size_t k = 0; k < infl; ++k) {
      float& w = (*weights)[base + k];
      if (!(w > 0.0f) || !std::isfinite(w)) {
        w = 0.0f;
        continue;
      }
      sum += static_cast<double>(w);
    }
    if (sum <= 0.0) continue;
    const float inv = static_cast<float>(1.0 / sum);
    for (size_t k = 0; k < infl; ++k) (*weights)[base + k] *= inv;
  }
}

}  // namespace

// BuildComposedSkinningMatrices/ApplySkinningToVertices are declared in
// skinning.hh (Gui::rebuildSubsetHighlight also calls them, to pose the
// Vulkan CPU-line selection highlight -- see gui.cc), so this stretch needs
// external linkage; everything above and below stays in the anonymous
// namespace as internal helpers.
matrix4d MatrixFromDraw(const float m[16]) {
  matrix4d out = matrix4d::identity();
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out.m[r][c] = static_cast<double>(m[r * 4 + c]);
    }
  }
  return out;
}

void PackMatrix(const matrix4d& m, int matrixIndex, SkinningFrameCPU* frame) {
  if (!frame || matrixIndex < 0) return;
  const size_t base = static_cast<size_t>(matrixIndex) * 16;
  if (base + 16 > frame->rgba32f.size()) return;
  for (int row = 0; row < 4; ++row) {
    for (int c = 0; c < 4; ++c) {
      frame->rgba32f[base + static_cast<size_t>(row) * 4 + static_cast<size_t>(c)] =
          static_cast<float>(m.m[row][c]);
    }
  }
}

point3f TransformPointRow(const point3f& p, const matrix4d& m) {
  const double x = p.x, y = p.y, z = p.z;
  double ox = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
  double oy = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
  double oz = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
  double ow = x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + m.m[3][3];
  if (std::abs(ow) > 1e-10) {
    ox /= ow;
    oy /= ow;
    oz /= ow;
  }
  return point3f{static_cast<float>(ox), static_cast<float>(oy),
                 static_cast<float>(oz)};
}

void TransformPointWorld(const float m[16], const point3f& p, float out[3]) {
  out[0] = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
  out[1] = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
  out[2] = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
}

bool BuildComposedSkinningMatrices(
    const tydra::RenderScene& render, const DrawMeshCPU& dm, double timecode,
    std::unordered_map<int, std::vector<matrix4d>>* skinCache,
    std::vector<matrix4d>* composed) {
  if (!skinCache || !composed || dm.skelId < 0 || dm.skinMatrixBase < 0) {
    return false;
  }
  if (dm.skelId >= static_cast<int>(render.skeletons.size())) return false;
  auto cit = skinCache->find(dm.skelId);
  if (cit == skinCache->end()) {
    std::vector<matrix4d> sm;
    if (!BuildSkinningMatrices(render, dm.skelId, timecode, &sm)) return false;
    cit = skinCache->emplace(dm.skelId, std::move(sm)).first;
  }
  const matrix4d geomBind = MatrixFromDraw(dm.skinGeomBind);
  const matrix4d skeletonWorld = MatrixFromDraw(dm.skinSkeletonWorld);
  const matrix4d invMeshWorld = lightusd::legacy::inverse(MatrixFromDraw(dm.world));
  composed->clear();
  composed->reserve(cit->second.size());
  for (const matrix4d& m : cit->second) {
    composed->push_back(geomBind * m * skeletonWorld * invMeshWorld);
  }
  return !composed->empty();
}

bool ApplyMorphTargetsToVertices(
    const DrawMeshCPU& dm,
    const std::unordered_map<std::string, float>& blendWeights,
    std::vector<DrawVertex>* verts) {
  if (!verts || dm.morphs.empty()) return false;
  bool touched = false;
  for (const MorphTargetCPU& mt : dm.morphs) {
    auto wit = blendWeights.find(mt.name);
    if (wit == blendWeights.end() || wit->second == 0.0f) continue;
    std::vector<float> ibW;
    ibW.reserve(mt.inbetweens.size());
    for (const MorphInbetweenCPU& ib : mt.inbetweens) ibW.push_back(ib.weight);
    const MorphBracket br = FindMorphBracket(ibW, wit->second);
    const int last = static_cast<int>(mt.inbetweens.size()) + 1;
    auto offsetAt = [&](int si, size_t k, float out[3]) {
      out[0] = out[1] = out[2] = 0.0f;
      if (si == 0) return;
      const std::vector<float>* src = nullptr;
      if (si == last) {
        src = &mt.dpos;
      } else {
        const size_t ib = static_cast<size_t>(si - 1);
        if (ib < mt.inbetweens.size()) src = &mt.inbetweens[ib].dpos;
      }
      if (!src || k * 3 + 2 >= src->size()) return;
      out[0] = (*src)[k * 3 + 0];
      out[1] = (*src)[k * 3 + 1];
      out[2] = (*src)[k * 3 + 2];
    };
    for (size_t k = 0; k < mt.vtx.size(); ++k) {
      const uint32_t vi = mt.vtx[k];
      if (vi >= verts->size()) continue;
      float lo[3], hi[3];
      offsetAt(br.lo, k, lo);
      offsetAt(br.hi, k, hi);
      DrawVertex& v = (*verts)[vi];
      v.px += lo[0] + (hi[0] - lo[0]) * br.t;
      v.py += lo[1] + (hi[1] - lo[1]) * br.t;
      v.pz += lo[2] + (hi[2] - lo[2]) * br.t;
      touched = true;
    }
  }
  return touched;
}

bool ApplySkinningToVertices(const DrawMeshCPU& dm,
                             const std::vector<matrix4d>& mats,
                             std::vector<DrawVertex>* verts) {
  if (!verts || mats.empty()) return false;
  const bool skinned =
      dm.skelId >= 0 && dm.skinMatrixBase >= 0 &&
      dm.jointIdx.size() == verts->size() * 4 &&
      dm.jointWt.size() == verts->size() * 4;
  const bool extendedSkinned =
      skinned && dm.influenceOffsetCount.size() == verts->size() * 2 &&
      !dm.influenceTexels.empty() && dm.influenceTexels.size() % 4 == 0;
  if (!skinned && !extendedSkinned) return false;

  // Per-vertex independent (each vi reads and writes only its own element), so
  // range-split threading is bit-identical to the serial loop.
  DeformParallelFor(verts->size(), 16384, [&](size_t vBegin, size_t vEnd) {
  for (size_t vi = vBegin; vi < vEnd; ++vi) {
    const DrawVertex& in = (*verts)[vi];
    const point3f p{in.px, in.py, in.pz};
    point3f acc{0.0f, 0.0f, 0.0f};
    float sum = 0.0f;
    if (extendedSkinned) {
      const uint32_t offset = dm.influenceOffsetCount[vi * 2 + 0];
      const uint32_t count = dm.influenceOffsetCount[vi * 2 + 1];
      const size_t texelCount = dm.influenceTexels.size() / 4;
      for (uint32_t k = 0; k < count; ++k) {
        const size_t texel = static_cast<size_t>(offset) + k;
        if (texel >= texelCount) break;
        const size_t base = texel * 4;
        const float w = dm.influenceTexels[base + 1];
        if (w <= 0.0f) continue;
        const uint32_t absIdx =
            static_cast<uint32_t>(std::max(0.0f, dm.influenceTexels[base] + 0.5f));
        if (absIdx < static_cast<uint32_t>(dm.skinMatrixBase)) continue;
        const size_t localIdx = static_cast<size_t>(absIdx - dm.skinMatrixBase);
        if (localIdx >= mats.size()) continue;
        const point3f q = TransformPointRow(p, mats[localIdx]);
        acc.x += q.x * w;
        acc.y += q.y * w;
        acc.z += q.z * w;
        sum += w;
      }
    } else {
      for (size_t k = 0; k < 4; ++k) {
        const float w = dm.jointWt[vi * 4 + k];
        if (w <= 0.0f) continue;
        const uint32_t absIdx = dm.jointIdx[vi * 4 + k];
        if (absIdx < static_cast<uint32_t>(dm.skinMatrixBase)) continue;
        const size_t localIdx = static_cast<size_t>(absIdx - dm.skinMatrixBase);
        if (localIdx >= mats.size()) continue;
        const point3f q = TransformPointRow(p, mats[localIdx]);
        acc.x += q.x * w;
        acc.y += q.y * w;
        acc.z += q.z * w;
        sum += w;
      }
    }
    if (sum > 0.0f) {
      DrawVertex& out = (*verts)[vi];
      out.px = acc.x;
      out.py = acc.y;
      out.pz = acc.z;
    }
  }
  });
  return true;
}

namespace {

void RecomputeSmoothNormals(std::vector<DrawVertex>* verts,
                            const std::vector<uint32_t>& indices,
                            float normalSign) {
  if (!verts || verts->empty()) return;
  // Three passes so the FP-heavy parts can thread while the accumulation stays
  // bit-identical to the old fused loop: (1) face normals, per-triangle
  // independent, parallel; (2) scatter-accumulate SERIAL in triangle order --
  // the same adds in the same order as before, so the per-vertex sums cannot
  // drift; (3) normalize, per-vertex independent, parallel.
  const size_t triCount = indices.size() / 3;
  std::vector<float> faceN(triCount * 3);
  DeformParallelFor(triCount, 65536, [&](size_t tBegin, size_t tEnd) {
    for (size_t t = tBegin; t < tEnd; ++t) {
      const uint32_t ia = indices[t * 3 + 0], ib = indices[t * 3 + 1],
                     ic = indices[t * 3 + 2];
      if (ia >= verts->size() || ib >= verts->size() || ic >= verts->size()) {
        faceN[t * 3 + 0] = faceN[t * 3 + 1] = faceN[t * 3 + 2] = 0.0f;
        continue;
      }
      const DrawVertex& a = (*verts)[ia];
      const DrawVertex& b = (*verts)[ib];
      const DrawVertex& c = (*verts)[ic];
      const float e1x = b.px - a.px, e1y = b.py - a.py, e1z = b.pz - a.pz;
      const float e2x = c.px - a.px, e2y = c.py - a.py, e2z = c.pz - a.pz;
      faceN[t * 3 + 0] = (e1y * e2z - e1z * e2y) * normalSign;
      faceN[t * 3 + 1] = (e1z * e2x - e1x * e2z) * normalSign;
      faceN[t * 3 + 2] = (e1x * e2y - e1y * e2x) * normalSign;
    }
  });
  for (DrawVertex& v : *verts) v.nx = v.ny = v.nz = 0.0f;
  for (size_t t = 0; t < triCount; ++t) {
    const uint32_t ia = indices[t * 3 + 0], ib = indices[t * 3 + 1],
                   ic = indices[t * 3 + 2];
    if (ia >= verts->size() || ib >= verts->size() || ic >= verts->size()) continue;
    const float nx = faceN[t * 3 + 0], ny = faceN[t * 3 + 1], nz = faceN[t * 3 + 2];
    DrawVertex* tri[3] = {&(*verts)[ia], &(*verts)[ib], &(*verts)[ic]};
    for (DrawVertex* v : tri) {
      v->nx += nx;
      v->ny += ny;
      v->nz += nz;
    }
  }
  DeformParallelFor(verts->size(), 262144, [&](size_t vBegin, size_t vEnd) {
    for (size_t vi = vBegin; vi < vEnd; ++vi) {
      DrawVertex& v = (*verts)[vi];
      const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
      if (len > 1e-12f) {
        const float inv = 1.0f / len;
        v.nx *= inv;
        v.ny *= inv;
        v.nz *= inv;
      }
    }
  });
}

void UpdateMeshBoundsFromVertices(DrawMeshCPU* dm,
                                  const std::vector<DrawVertex>& verts,
                                  bool updateSkinnedHelpers) {
  if (!dm) return;
  bool first = true;
  float mn[3] = {std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
  float mx[3] = {-std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max()};
  const size_t sampleStep =
      updateSkinnedHelpers && verts.size() > 8192
          ? (verts.size() + 8191) / 8192
          : 1;
  dm->skinnedHelperPoints.clear();
  if (updateSkinnedHelpers && dm->skelId >= 0) {
    dm->skinnedHelperPoints.reserve(((verts.size() + sampleStep - 1) / sampleStep) * 3);
  }
  for (size_t vi = 0; vi < verts.size(); ++vi) {
    const point3f local{verts[vi].px, verts[vi].py, verts[vi].pz};
    if (updateSkinnedHelpers && dm->skelId >= 0 && (vi % sampleStep) == 0) {
      dm->skinnedHelperPoints.push_back(local.x);
      dm->skinnedHelperPoints.push_back(local.y);
      dm->skinnedHelperPoints.push_back(local.z);
    }
    float w[3];
    TransformPointWorld(dm->world, local, w);
    for (int c = 0; c < 3; ++c) {
      mn[c] = std::min(mn[c], w[c]);
      mx[c] = std::max(mx[c], w[c]);
    }
    first = false;
  }
  if (!first) {
    for (int c = 0; c < 3; ++c) {
      dm->aabbMin[c] = mn[c];
      dm->aabbMax[c] = mx[c];
    }
  }
}

void RecomputeDrawSceneBounds(DrawScene* draw) {
  if (!draw) return;
  bool first = true;
  float mn[3] = {std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
  float mx[3] = {-std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max()};
  for (const DrawMeshCPU& dm : draw->meshes) {
    if (dm.aabbMin[0] > dm.aabbMax[0] || dm.aabbMin[1] > dm.aabbMax[1] ||
        dm.aabbMin[2] > dm.aabbMax[2]) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      mn[c] = std::min(mn[c], dm.aabbMin[c]);
      mx[c] = std::max(mx[c], dm.aabbMax[c]);
    }
    first = false;
  }
  draw->hasBounds = !first;
  if (draw->hasBounds) {
    for (int c = 0; c < 3; ++c) {
      draw->aabbMin[c] = mn[c];
      draw->aabbMax[c] = mx[c];
    }
  }
}


}  // namespace

std::map<std::string, InbetweenSamples> CollectBlendShapeInbetweens(
    const lightusd::legacy::Stage& stage) {
  std::map<std::string, InbetweenSamples> out;
  tydra::PathPrimMap<lightusd::legacy::BlendShape> bss;
  if (!tydra::ListPrims(stage, bss)) return out;
  for (auto& kv : bss) {
    const lightusd::legacy::BlendShape* bs = kv.second;
    if (!bs) continue;
    InbetweenSamples ibs = ReadInbetweensFromPrim(*bs);
    if (!ibs.empty()) out[bs->name] = std::move(ibs);
  }
  return out;
}

bool SceneHasDeformation(const tydra::RenderScene& render) {
  for (const tydra::RenderMesh& m : render.meshes) {
    if (MeshIsSkinned(m) || !m.targets.empty()) return true;
  }
  return false;
}

bool SceneHasSkeletalSkinning(const tydra::RenderScene& render) {
  for (const tydra::RenderMesh& m : render.meshes) {
    if (MeshIsSkinned(m)) return true;
  }
  return false;
}

bool SceneHasBlendShapes(const tydra::RenderScene& render) {
  for (const tydra::RenderMesh& m : render.meshes) {
    if (!m.targets.empty()) return true;
  }
  return false;
}

bool SceneHasNonSkeletalAnimation(const tydra::RenderScene& render) {
  for (const tydra::AnimationClip& clip : render.animations) {
    for (const tydra::AnimationChannel& ch : clip.channels) {
      if (ch.target_type == tydra::ChannelTargetType::SkeletonJoint) continue;
      if (ch.sampler < 0 ||
          ch.sampler >= static_cast<int>(clip.samplers.size())) {
        continue;
      }
      if (clip.samplers[static_cast<size_t>(ch.sampler)].times.size() > 1) {
        return true;
      }
    }
  }
  return false;
}

int MaxSkinInfluenceCount(const tydra::RenderScene& render) {
  int maxInfluences = 0;
  for (const tydra::RenderMesh& m : render.meshes) {
    if (MeshIsSkinned(m)) maxInfluences = std::max(maxInfluences, m.joint_and_weights.elementSize);
  }
  return maxInfluences;
}

// Apply the GPU-morph channels on the CPU: the same sum the vertex shader does
// (coeff[channelId] * delta), from the same half-precision buffers it reads. Used
// only for BOUNDS -- the raster path never morphs vertices on the CPU.
static void ApplyMorphChannelsToVertices(const DrawMeshCPU& dm,
                                         const std::vector<float>& coeff,
                                         std::vector<DrawVertex>* verts) {
  if (!verts) return;
  const size_t entries = dm.morphDeltaHalf.size() / 4;
  const bool haveIds = dm.morphChannelId.size() == entries;
  for (size_t v = 0; v < verts->size(); ++v) {
    const size_t base = dm.morphOffsetCount[v * 2 + 0];
    const size_t count = dm.morphOffsetCount[v * 2 + 1];
    for (size_t k = 0; k < count && base + k < entries; ++k) {
      const uint16_t* e = &dm.morphDeltaHalf[(base + k) * 4];
      auto f16 = [](uint16_t bits) {
        lightusd::legacy::value::half h;
        h.value = bits;
        return lightusd::legacy::value::half_to_float(h);
      };
      const size_t chan =
          haveIds ? dm.morphChannelId[base + k]
                  : static_cast<size_t>(f16(e[0]) + 0.5f);
      if (chan >= coeff.size()) continue;
      const float c = coeff[chan];
      if (std::fabs(c) < 1e-6f) continue;
      (*verts)[v].px += c * f16(e[1]);
      (*verts)[v].py += c * f16(e[2]);
      (*verts)[v].pz += c * f16(e[3]);
    }
  }
}

bool BuildGpuSkinningFrame(
    const tydra::RenderScene& render, DrawScene* draw, double timecode,
    SkinningFrameCPU* frame, bool updateSkinnedHelpers,
    const lightusd::legacy::Stage* stage,
    const std::unordered_map<std::string, float>* blendOverride) {
  if (!draw || !frame) return false;
  const int matrices = draw->boneMatrixCount;
  if (matrices > 0) {
    frame->matrixCount = matrices;
    const size_t floats = static_cast<size_t>(matrices) * 16;
    if (frame->rgba32f.size() != floats) {
      frame->rgba32f.assign(floats, 0.0f);
    } else {
      std::fill(frame->rgba32f.begin(), frame->rgba32f.end(), 0.0f);
    }
    frame->enabled = true;
  } else {
    *frame = SkinningFrameCPU{};  // morph-only scene: no bone texture
  }

  // The raster path applies blendshapes + skinning in the GPU vertex shader, so
  // the CPU keeps rest geometry. The BOUNDS below still have to account for the
  // morph -- they drive the ground grid, the depth normalization and the auto-fit
  // -- so morph a scratch copy of the vertices for that, exactly as the RT path
  // does (BuildRtSkinnedMeshVertices) before it re-derives its boxes.
  std::unordered_map<std::string, float> blendWeights;
  if (stage) blendWeights = GatherBlendWeights(*stage, timecode);
  if (blendOverride)
    for (const auto& kv : *blendOverride) blendWeights[kv.first] = kv.second;
  const bool morphBounds = stage != nullptr;
  std::vector<float> morphCoeff;

  std::unordered_map<int, std::vector<matrix4d>> skinCache;
  std::unordered_map<int, std::vector<matrix4d>> composedByBase;

  for (const DrawMeshCPU& dm : draw->meshes) {
    if (dm.skelId < 0 || dm.skinMatrixBase < 0 || dm.jointIdx.empty()) continue;
    if (dm.skelId >= static_cast<int>(render.skeletons.size())) continue;
    auto cit = skinCache.find(dm.skelId);
    if (cit == skinCache.end()) {
      std::vector<matrix4d> sm;
      if (!BuildSkinningMatrices(render, dm.skelId, timecode, &sm)) continue;
      cit = skinCache.emplace(dm.skelId, std::move(sm)).first;
    }
    const matrix4d geomBind = MatrixFromDraw(dm.skinGeomBind);
    const matrix4d skeletonWorld = MatrixFromDraw(dm.skinSkeletonWorld);
    const matrix4d invMeshWorld = lightusd::legacy::inverse(MatrixFromDraw(dm.world));
    std::vector<matrix4d> composed;
    composed.reserve(cit->second.size());
    for (size_t j = 0; j < cit->second.size(); ++j) {
      matrix4d m = geomBind * cit->second[j] * skeletonWorld * invMeshWorld;
      composed.push_back(m);
      const int row = dm.skinMatrixBase + static_cast<int>(j);
      if (row < matrices) PackMatrix(m, row, frame);
    }
    composedByBase[dm.skinMatrixBase] = std::move(composed);
  }

  constexpr size_t kMaxSkinnedHelperSamplesPerMesh = 8192;
  bool sceneFirst = true;
  float sceneMn[3] = {std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
  float sceneMx[3] = {-std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max()};
  auto updateScene = [&](const float mn[3], const float mx[3]) {
    for (int c = 0; c < 3; ++c) {
      sceneMn[c] = std::min(sceneMn[c], mn[c]);
      sceneMx[c] = std::max(sceneMx[c], mx[c]);
    }
    sceneFirst = false;
  };
  for (size_t mi = 0; mi < draw->meshes.size(); ++mi) {
    DrawMeshCPU& dm = draw->meshes[mi];
    // The morph is applied from the half-precision GPU channels, not from
    // dm.morphs -- mesh_build frees those once the channels are built (at facial
    // scale they are the dominant CPU copy), so they are empty here.
    std::vector<DrawVertex> morphed;
    if (morphBounds && dm.morphChannelCount > 0 &&
        !dm.morphTargetChannels.empty() &&
        dm.morphOffsetCount.size() == dm.vertices.size() * 2 &&
        !dm.morphDeltaHalf.empty()) {
      EvalMorphChannelCoeffs(dm, blendWeights, &morphCoeff);
      morphed = dm.vertices;
      ApplyMorphChannelsToVertices(dm, morphCoeff, &morphed);
    }
    const std::vector<DrawVertex>& verts =
        morphed.empty() ? dm.vertices : morphed;
    bool meshFirst = true;
    float mn[3] = {std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float mx[3] = {-std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max()};

    // An INSTANCED mesh's vertices are prototype-local and `world` is ignored (each
    // placement carries its own o2w, 3 rows of (x,y,z,tx)); bounding them through
    // `world` would stack every instance on top of the prototype's origin. A
    // PointInstancer of a deforming prototype is exactly that case.
    const size_t ninst = dm.instanceCount();
    auto grow = [&](const float w[3]) {
      for (int c = 0; c < 3; ++c) {
        mn[c] = std::min(mn[c], w[c]);
        mx[c] = std::max(mx[c], w[c]);
      }
      meshFirst = false;
    };
    auto update = [&](const point3f& local) {
      if (ninst > 0) {
        for (size_t k = 0; k < ninst; ++k) {
          const float* X = &dm.instanceXforms[k * 12];
          const float w[3] = {
              X[0] * local.x + X[1] * local.y + X[2] * local.z + X[3],
              X[4] * local.x + X[5] * local.y + X[6] * local.z + X[7],
              X[8] * local.x + X[9] * local.y + X[10] * local.z + X[11]};
          grow(w);
        }
        return;
      }
      float w[3];
      TransformPointWorld(dm.world, local, w);
      grow(w);
    };

    const bool skinned = dm.skelId >= 0 && dm.skinMatrixBase >= 0 &&
                         dm.jointIdx.size() == verts.size() * 4 &&
                         dm.jointWt.size() == verts.size() * 4;
    const auto bit = skinned ? composedByBase.find(dm.skinMatrixBase)
                             : composedByBase.end();
    const bool extendedSkinned =
        skinned && dm.influenceOffsetCount.size() == verts.size() * 2 &&
        !dm.influenceTexels.empty() && dm.influenceTexels.size() % 4 == 0;
    dm.skinnedHelperPoints.clear();
    if (!skinned && !extendedSkinned) {
      if (!morphed.empty()) {
        // Morph-only mesh: its box moves with the blendshape, so take it from the
        // morphed vertices rather than leaving the rest box standing.
        for (const DrawVertex& v : verts) update(point3f{v.px, v.py, v.pz});
        if (!meshFirst) {
          for (int c = 0; c < 3; ++c) { dm.aabbMin[c] = mn[c]; dm.aabbMax[c] = mx[c]; }
          updateScene(dm.aabbMin, dm.aabbMax);
        }
      } else if (dm.aabbMin[0] <= dm.aabbMax[0] && dm.aabbMin[1] <= dm.aabbMax[1] &&
                 dm.aabbMin[2] <= dm.aabbMax[2]) {
        updateScene(dm.aabbMin, dm.aabbMax);
      }
      continue;
    } else if (bit == composedByBase.end()) {
      for (const DrawVertex& v : verts) {
        update(point3f{v.px, v.py, v.pz});
      }
    } else {
      const size_t sampleStep =
          updateSkinnedHelpers && verts.size() > kMaxSkinnedHelperSamplesPerMesh
              ? (verts.size() + kMaxSkinnedHelperSamplesPerMesh - 1) /
                    kMaxSkinnedHelperSamplesPerMesh
              : 1;
      if (updateSkinnedHelpers) {
        dm.skinnedHelperPoints.reserve(((verts.size() + sampleStep - 1) / sampleStep) * 3);
      }
      const std::vector<matrix4d>& mats = bit->second;
      for (size_t vi = 0; vi < verts.size(); ++vi) {
        const DrawVertex& v = verts[vi];
        const point3f p{v.px, v.py, v.pz};
        point3f acc{0.0f, 0.0f, 0.0f};
        float sum = 0.0f;
        if (extendedSkinned) {
          const uint32_t offset = dm.influenceOffsetCount[vi * 2 + 0];
          const uint32_t count = dm.influenceOffsetCount[vi * 2 + 1];
          const size_t texelCount = dm.influenceTexels.size() / 4;
          for (uint32_t k = 0; k < count; ++k) {
            const size_t texel = static_cast<size_t>(offset) + k;
            if (texel >= texelCount) break;
            const size_t base = texel * 4;
            const float w = dm.influenceTexels[base + 1];
            if (w <= 0.0f) continue;
            const uint32_t absIdx =
                static_cast<uint32_t>(std::max(0.0f, dm.influenceTexels[base] + 0.5f));
            if (absIdx < static_cast<uint32_t>(dm.skinMatrixBase)) continue;
            const size_t localIdx = static_cast<size_t>(absIdx - dm.skinMatrixBase);
            if (localIdx >= mats.size()) continue;
            const point3f q = TransformPointRow(p, mats[localIdx]);
            acc.x += q.x * w;
            acc.y += q.y * w;
            acc.z += q.z * w;
            sum += w;
          }
        } else {
          for (size_t k = 0; k < 4; ++k) {
            const float w = dm.jointWt[vi * 4 + k];
            if (w <= 0.0f) continue;
            const uint32_t absIdx = dm.jointIdx[vi * 4 + k];
            if (absIdx < static_cast<uint32_t>(dm.skinMatrixBase)) continue;
            const size_t localIdx = static_cast<size_t>(absIdx - dm.skinMatrixBase);
            if (localIdx >= mats.size()) continue;
            const point3f q = TransformPointRow(p, mats[localIdx]);
            acc.x += q.x * w;
            acc.y += q.y * w;
            acc.z += q.z * w;
            sum += w;
          }
        }
        const point3f skinnedPoint = sum > 0.0f ? acc : p;
        if (updateSkinnedHelpers && (vi % sampleStep) == 0) {
          dm.skinnedHelperPoints.push_back(skinnedPoint.x);
          dm.skinnedHelperPoints.push_back(skinnedPoint.y);
          dm.skinnedHelperPoints.push_back(skinnedPoint.z);
        }
        update(skinnedPoint);
      }
    }
    if (!meshFirst) {
      for (int c = 0; c < 3; ++c) {
        dm.aabbMin[c] = mn[c];
        dm.aabbMax[c] = mx[c];
      }
      updateScene(dm.aabbMin, dm.aabbMax);
    }
  }
  draw->hasBounds = !sceneFirst;
  if (draw->hasBounds) {
    for (int c = 0; c < 3; ++c) {
      draw->aabbMin[c] = sceneMn[c];
      draw->aabbMax[c] = sceneMx[c];
    }
  }
  return true;
}

bool BuildRtSkinnedMeshVertices(
    const lightusd::legacy::Stage& stage, const tydra::RenderScene& render,
    DrawScene* draw, double timecode,
    const std::unordered_map<std::string, float>* blendOverride,
    bool updateSkinnedHelpers,
    std::vector<RtSkinnedMeshUpload>* outUploads,
    const std::unordered_set<int>* skipMeshes) {
  if (!draw || !outUploads) return false;
  outUploads->clear();
  if (draw->meshes.empty()) return true;

  const bool kPoseTiming = std::getenv("LUSDVIEW_RT_POSE_TIMING") != nullptr;
  auto tick = [&]() { return std::chrono::steady_clock::now(); };
  auto ms = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  auto t0 = tick();
  std::unordered_map<std::string, float> blendWeights =
      GatherBlendWeights(stage, timecode);
  if (blendOverride) {
    for (const auto& kv : *blendOverride) blendWeights[kv.first] = kv.second;
  }
  auto t1 = tick();
  if (kPoseTiming)
    std::fprintf(stderr, "[rt-pose] blend-weights %.2f ms\n", ms(t0, t1));

  std::unordered_map<int, std::vector<matrix4d>> skinCache;
  std::vector<matrix4d> composed;
  bool anyBoundsChanged = false;

  for (size_t mi = 0; mi < draw->meshes.size(); ++mi) {
    if (skipMeshes && skipMeshes->count(static_cast<int>(mi))) continue;
    DrawMeshCPU& dm = draw->meshes[mi];
    const bool hasMorph = !dm.morphs.empty();
    const bool hasSkinAttrs =
        dm.skelId >= 0 && dm.skinMatrixBase >= 0 &&
        dm.jointIdx.size() == dm.vertices.size() * 4 &&
        dm.jointWt.size() == dm.vertices.size() * 4;
    if (!hasMorph && !hasSkinAttrs) continue;

    auto tA = tick();
    std::vector<DrawVertex> verts = dm.vertices;
    auto tB = tick();
    bool deformed = false;
    if (hasMorph) {
      ApplyMorphTargetsToVertices(dm, blendWeights, &verts);
      deformed = true;  // upload rest pose too so stale morphs can be cleared.
    }
    auto tC = tick();
    if (hasSkinAttrs &&
        BuildComposedSkinningMatrices(render, dm, timecode, &skinCache, &composed)) {
      if (ApplySkinningToVertices(dm, composed, &verts)) deformed = true;
    }
    auto tD = tick();
    if (!deformed) continue;

    if (!dm.geometricNormal) RecomputeSmoothNormals(&verts, dm.indices, dm.normalSign);
    auto tE = tick();
    UpdateMeshBoundsFromVertices(&dm, verts, updateSkinnedHelpers);
    auto tF = tick();
    if (kPoseTiming)
      std::fprintf(stderr,
                   "[rt-pose] mesh %zu: copy %.2f morph %.2f skin %.2f "
                   "normals %.2f bounds %.2f ms\n",
                   mi, ms(tA, tB), ms(tB, tC), ms(tC, tD), ms(tD, tE), ms(tE, tF));
    anyBoundsChanged = true;

    RtSkinnedMeshUpload upload;
    upload.meshIndex = static_cast<int>(mi);
    // Only pay the whole-mesh deep copy (indices, weights, morphs, ...) when
    // there is actually displacement to bake -- for a plain skinned mesh this
    // copy was a fixed multi-MB tax on EVERY pose.
    if (MeshHasDisplacement(*draw, dm)) {
      DrawMeshCPU displacedMesh = dm;
      displacedMesh.vertices = verts;
      if (!BakeDisplacedVertices(*draw, displacedMesh, /*globalScale=*/1.0f,
                                 &upload.vertices)) {
        upload.vertices = std::move(verts);
      }
    } else {
      upload.vertices = std::move(verts);
    }
    outUploads->push_back(std::move(upload));
  }

  if (anyBoundsChanged) RecomputeDrawSceneBounds(draw);
  return true;
}

bool BuildRtGpuSkinUpdates(const tydra::RenderScene& render, DrawScene* draw,
                           double timecode,
                           std::vector<RtGpuSkinUpdate>* outUpdates,
                           std::unordered_set<int>* outHandled) {
  if (!draw || !outUpdates || !outHandled) return false;
  outUpdates->clear();
  outHandled->clear();

  std::unordered_map<int, std::vector<matrix4d>> skinCache;
  std::vector<matrix4d> composed;
  bool anyBoundsChanged = false;
  static const bool rtTiming = std::getenv("LUSDVIEW_RT_TIMING") != nullptr;
  const auto reject = [&](size_t mi, const char* why) {
    static bool once = false;
    if (rtTiming && !once) {
      std::fprintf(stderr, "[rt_skin] mesh %zu not GPU-eligible: %s\n", mi, why);
      once = true;
    }
  };

  for (size_t mi = 0; mi < draw->meshes.size(); ++mi) {
    DrawMeshCPU& dm = draw->meshes[mi];
    if (!dm.morphs.empty()) {  // blendshapes -> CPU path
      reject(mi, "blendshapes");
      continue;
    }
    const bool hasSkinAttrs =
        dm.skelId >= 0 && dm.skinMatrixBase >= 0 &&
        dm.jointIdx.size() == dm.vertices.size() * 4 &&
        dm.jointWt.size() == dm.vertices.size() * 4;
    if (!hasSkinAttrs) continue;
    // > 4 influences resolve through the influence texel table -> CPU path.
    // (The loader fills the table even for <= 4 influences; there its content
    // matches the top-4 joint/weight attributes the compute shader reads, so
    // only genuinely-extended meshes must stay on the CPU.)
    const bool extendedSkinned =
        dm.influenceOffsetCount.size() == dm.vertices.size() * 2 &&
        !dm.influenceTexels.empty();
    if (extendedSkinned && dm.maxInfluencesPerVertex > 4) {
      reject(mi, "extended influences (>4 per vertex)");
      continue;
    }
    // Normals are skinned in the compute shader with the weighted joint
    // matrices (the raster deform.glsl convention), so smooth-shaded meshes
    // are eligible; the CPU path instead REGENERATES smooth normals on the
    // posed surface (close, not bit-identical — one reason the GPU path is
    // opt-in, see App::updateGpuSkinningFrameIfNeeded).
    // Displacement bakes along (recomputed) normals on the CPU.
    if (MeshHasDisplacement(*draw, dm)) {
      reject(mi, "displacement");
      continue;
    }
    if (!BuildComposedSkinningMatrices(render, dm, timecode, &skinCache,
                                       &composed)) {
      reject(mi, "no composed skinning matrices");
      continue;
    }

    RtGpuSkinUpdate u;
    u.meshIndex = static_cast<int>(mi);
    u.matrixBase = dm.skinMatrixBase;
    u.jointCount = static_cast<int>(composed.size());
    u.mats.resize(composed.size() * 16);
    for (size_t j = 0; j < composed.size(); ++j) {
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          u.mats[j * 16 + static_cast<size_t>(r) * 4 + static_cast<size_t>(c)] =
              static_cast<float>(composed[j].m[r][c]);
        }
      }
    }

    // Conservative posed bounds: a skinned point is a convex combination of
    // per-joint transforms of its rest position, so the posed mesh lies inside
    // the union of the per-joint transformed REST prototype boxes (the rest box
    // itself is included for zero-weight vertices).
    float mn[3] = {dm.protoAabbMin[0], dm.protoAabbMin[1], dm.protoAabbMin[2]};
    float mx[3] = {dm.protoAabbMax[0], dm.protoAabbMax[1], dm.protoAabbMax[2]};
    for (const matrix4d& m : composed) {
      for (int corner = 0; corner < 8; ++corner) {
        const point3f p{(corner & 1) ? dm.protoAabbMax[0] : dm.protoAabbMin[0],
                        (corner & 2) ? dm.protoAabbMax[1] : dm.protoAabbMin[1],
                        (corner & 4) ? dm.protoAabbMax[2] : dm.protoAabbMin[2]};
        const point3f q = TransformPointRow(p, m);
        mn[0] = std::min(mn[0], q.x); mx[0] = std::max(mx[0], q.x);
        mn[1] = std::min(mn[1], q.y); mx[1] = std::max(mx[1], q.y);
        mn[2] = std::min(mn[2], q.z); mx[2] = std::max(mx[2], q.z);
      }
    }
    for (int c = 0; c < 3; ++c) {
      u.aabbMin[c] = mn[c];
      u.aabbMax[c] = mx[c];
      dm.aabbMin[c] = mn[c];
      dm.aabbMax[c] = mx[c];
    }
    anyBoundsChanged = true;

    outHandled->insert(u.meshIndex);
    outUpdates->push_back(std::move(u));
  }

  if (anyBoundsChanged) RecomputeDrawSceneBounds(draw);
  return !outUpdates->empty();
}

void BuildMorphChannelWeights(
    const lightusd::legacy::Stage& stage, const DrawScene& draw, double timecode,
    const std::unordered_map<std::string, float>* blendOverride,
    std::vector<std::pair<int, std::vector<float>>>* out) {
  if (!out) return;
  out->clear();
  std::unordered_map<std::string, float> weights =
      GatherBlendWeights(stage, timecode);
  if (blendOverride) {
    for (const auto& kv : *blendOverride) weights[kv.first] = kv.second;
  }
  std::vector<float> coeff;
  for (size_t mi = 0; mi < draw.meshes.size(); ++mi) {
    const DrawMeshCPU& dm = draw.meshes[mi];
    if (dm.morphChannelCount <= 0 || dm.morphTargetChannels.empty()) continue;
    EvalMorphChannelCoeffs(dm, weights, &coeff);
    out->emplace_back(static_cast<int>(mi), coeff);
  }
}

namespace {
void FlattenXformWorlds(const tydra::XformNode& n,
                        std::unordered_map<std::string, matrix4d>& out) {
  out[n.absolute_path.full_path_name()] = n.get_world_matrix();
  for (const tydra::XformNode& c : n.children) FlattenXformWorlds(c, out);
}
}  // namespace

bool UpdateAnimatedMeshWorlds(const lightusd::legacy::Stage& stage, DrawScene* draw,
                              double timecode) {
  if (!draw) return false;
  tydra::XformNode root;
  if (!tydra::BuildXformNodeFromStage(stage, &root, timecode)) return false;
  std::unordered_map<std::string, matrix4d> worlds;
  FlattenXformWorlds(root, worlds);
  bool changed = false;
  for (DrawMeshCPU& dm : draw->meshes) {
    auto it = worlds.find(dm.absPath);
    if (it == worlds.end()) continue;
    float w[16];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        // Match MatToColMajor/PlaceDrawMesh: USD row-major values are copied
        // element-wise into the renderer's column-vector storage layout.
        w[i * 4 + j] = static_cast<float>(it->second.m[i][j]);
      }
    }
    if (std::memcmp(dm.world, w, sizeof(w)) != 0) {
      std::memcpy(dm.world, w, sizeof(w));
      changed = true;
    }
    if (!dm.skinSkeletonPath.empty()) {
      auto sit = worlds.find(dm.skinSkeletonPath);
      if (sit != worlds.end()) {
        float sw[16];
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j)
            sw[i * 4 + j] = static_cast<float>(sit->second.m[i][j]);
        if (std::memcmp(dm.skinSkeletonWorld, sw, sizeof(sw)) != 0) {
          std::memcpy(dm.skinSkeletonWorld, sw, sizeof(sw));
          changed = true;
        }
      }
    }
  }
  if (changed) {
    bool has = false;
    float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    auto addBox = [&](const float lo[3], const float hi[3]) {
      if (!std::isfinite(lo[0]) || !std::isfinite(hi[0]) || hi[0] < lo[0])
        return;
      if (!has) {
        for (int k = 0; k < 3; ++k) { mn[k] = lo[k]; mx[k] = hi[k]; }
        has = true;
      } else {
        for (int k = 0; k < 3; ++k) {
          mn[k] = std::min(mn[k], lo[k]);
          mx[k] = std::max(mx[k], hi[k]);
        }
      }
    };
    for (const DrawMeshCPU& dm : draw->meshes)
      addBox(dm.aabbMin, dm.aabbMax);
    for (const DrawPointsCPU& dp : draw->points)
      addBox(dp.aabbMin, dp.aabbMax);
    for (const DrawCurvesCPU& dc : draw->curves)
      addBox(dc.aabbMin, dc.aabbMax);
    if (has) {
      for (int k = 0; k < 3; ++k) {
        draw->aabbMin[k] = mn[k];
        draw->aabbMax[k] = mx[k];
      }
      draw->hasBounds = true;
    }
  }
  return changed;
}

void DeformSkinnedMeshes(
    const lightusd::legacy::Stage& stage, tydra::RenderScene& render, double timecode,
    const std::unordered_map<std::string, float>* blendOverride) {
  if (!SceneHasDeformation(render)) return;
  const double t = timecode;  // time codes (matches Tydra sampler times)

  std::unordered_map<std::string, float> blendWeights;
  bool gatheredBlend = false;

  // Cache skinning matrices per skeleton (shared across meshes).
  std::unordered_map<int, std::vector<matrix4d>> skinCache;

  // Current placement spaces. SkinPointsLBS returns through inverse geomBind,
  // but a USD skinned result is placed through the Skeleton prim, not through
  // the mesh prim. Pre-compose the conversion back to each mesh's local space.
  std::unordered_map<int, matrix4d> meshWorlds;
  std::unordered_map<std::string, matrix4d> nodeWorlds;
  std::function<void(const tydra::Node&)> collectWorlds =
      [&](const tydra::Node& node) {
        nodeWorlds.emplace(node.abs_path, node.global_matrix);
        if (node.nodeType == tydra::NodeType::Mesh && node.id >= 0)
          meshWorlds.emplace(node.id, node.global_matrix);
        for (const tydra::Node& child : node.children) collectWorlds(child);
      };
  for (const tydra::Node& root : render.nodes) collectWorlds(root);

  for (size_t meshIndex = 0; meshIndex < render.meshes.size(); ++meshIndex) {
    tydra::RenderMesh& mesh = render.meshes[meshIndex];
    const bool skinned = MeshIsSkinned(mesh);
    const bool morphed = !mesh.targets.empty();
    if (!skinned && !morphed) continue;

    const size_t np = mesh.points.size();
    if (np == 0) continue;

    // Working copy of points (rest pose) to deform.
    std::vector<point3f> pts(np);
    for (size_t i = 0; i < np; ++i) {
      pts[i].x = mesh.points[i][0];
      pts[i].y = mesh.points[i][1];
      pts[i].z = mesh.points[i][2];
    }

    // (1) Blendshapes: accumulate interpolated offsets onto the rest points.
    if (morphed) {
      if (!gatheredBlend) {
        blendWeights = GatherBlendWeights(stage, t);
        // Manual weights (blend editor) override the animation per name.
        if (blendOverride) {
          for (const auto& kv : *blendOverride) blendWeights[kv.first] = kv.second;
        }
        gatheredBlend = true;
      }
      for (const auto& kv : mesh.targets) {
        auto wit = blendWeights.find(kv.first);
        if (wit == blendWeights.end()) continue;
        const float w = wit->second;
        if (w == 0.0f) continue;
        const tydra::ShapeTarget& tgt = kv.second;
        const size_t no = tgt.pointOffsets.size();

        // In-between samples carried by tydra, reordered parallel to this
        // target's pointIndices (single-indexable and facevarying alike).
        std::vector<float> ibW;
        std::vector<const tydra::InbetweenShapeTarget*> ibOff;
        {
          std::vector<const tydra::InbetweenShapeTarget*> sorted;
          sorted.reserve(tgt.inbetweens.size());
          for (const auto& s : tgt.inbetweens) sorted.push_back(&s.second);
          std::sort(sorted.begin(), sorted.end(),
                    [](const tydra::InbetweenShapeTarget* a,
                       const tydra::InbetweenShapeTarget* b) {
                      return a->weight < b->weight;
                    });
          for (const tydra::InbetweenShapeTarget* s : sorted) {
            if (s->pointOffsets.size() != tgt.pointIndices.size()) continue;
            ibW.push_back(s->weight);
            ibOff.push_back(s);
          }
        }
        const MorphBracket br = FindMorphBracket(ibW, w);
        const int last = static_cast<int>(ibOff.size()) + 1;  // primary index
        // Offset of table-index `si` at point `k` (0 -> zero, last -> primary).
        auto off = [&](int si, size_t k, float o[3]) {
          if (si == 0) { o[0] = o[1] = o[2] = 0.0f; }
          else if (si == last) {
            o[0] = tgt.pointOffsets[k][0]; o[1] = tgt.pointOffsets[k][1];
            o[2] = tgt.pointOffsets[k][2];
          } else {
            const auto& a = ibOff[si - 1]->pointOffsets[k];
            o[0] = a[0]; o[1] = a[1]; o[2] = a[2];
          }
        };
        for (size_t k = 0; k < tgt.pointIndices.size() && k < no; ++k) {
          const uint32_t vid = tgt.pointIndices[k];
          if (vid >= np) continue;
          float lo[3], hi[3];
          off(br.lo, k, lo);
          off(br.hi, k, hi);
          pts[vid].x += lo[0] + (hi[0] - lo[0]) * br.t;
          pts[vid].y += lo[1] + (hi[1] - lo[1]) * br.t;
          pts[vid].z += lo[2] + (hi[2] - lo[2]) * br.t;
        }
      }
    }

    // (2) Linear-blend skinning of the (blendshaped) points.
    if (skinned) {
      const auto& jw = mesh.joint_and_weights;
      const int infl = jw.elementSize < 1 ? 1 : jw.elementSize;
      const size_t expect = np * static_cast<size_t>(infl);
      if (jw.jointIndices.size() == expect && jw.jointWeights.size() == expect) {
        std::vector<float> weights(jw.jointWeights.size());
        for (size_t i = 0; i < weights.size(); ++i) weights[i] = jw.jointWeights[i];
        NormalizeSkinWeights(&weights, np, infl);
        auto cit = skinCache.find(mesh.skel_id);
        if (cit == skinCache.end()) {
          std::vector<matrix4d> sm;
          if (BuildSkinningMatrices(render, mesh.skel_id, t, &sm)) {
            cit = skinCache.emplace(mesh.skel_id, std::move(sm)).first;
          }
        }
        if (cit != skinCache.end()) {
          std::vector<matrix4d> meshLocalSkin = cit->second;
          const auto mw = meshWorlds.find(static_cast<int>(meshIndex));
          const auto& skel = render.skeletons[static_cast<size_t>(mesh.skel_id)];
          const auto sw = nodeWorlds.find(skel.abs_path);
          if (mw != meshWorlds.end() && sw != nodeWorlds.end()) {
            const matrix4d skeletonToMesh =
                sw->second * lightusd::legacy::inverse(mw->second) * jw.geomBindTransform;
            for (size_t j = 0; j < meshLocalSkin.size(); ++j)
              meshLocalSkin[j] = meshLocalSkin[j] * skeletonToMesh;
          }
          std::vector<point3f> skinned_pts;
          if (tydra::SkinPointsLBS(pts, jw.geomBindTransform, meshLocalSkin,
                                   jw.jointIndices, weights, infl,
                                   &skinned_pts) &&
              skinned_pts.size() == np) {
            pts.swap(skinned_pts);
          }
        }
      }
    }

    // Write deformed points back; clear normals so the packer regenerates
    // smooth normals from the posed geometry.
    for (size_t i = 0; i < np; ++i) {
      mesh.points[i][0] = pts[i].x;
      mesh.points[i][1] = pts[i].y;
      mesh.points[i][2] = pts[i].z;
    }
    mesh.normals = tydra::VertexAttribute();
  }
}

}  // namespace lusdview
