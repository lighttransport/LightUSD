// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdSkel Schema Implementation

#include "usd-skel.hh"
#include "../layer/prim-spec.hh"
#include "../strfmt.hh"
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace lightusd {
namespace next {

// ============================================================
// Prim type checking
// ============================================================

bool IsSkelRoot(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "SkelRoot";
}

bool IsSkeleton(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Skeleton";
}

bool IsSkelAnimation(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "SkelAnimation";
}

bool IsBlendShape(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "BlendShape";
}

// ============================================================
// Skeleton data
// ============================================================

namespace {

// A Skeleton's bind/rest transforms: `matrix4d[]`, i.e. a flat DOUBLE array of
// 16 scalars per joint. Two traps this centralizes:
//   - UsdSkel authors them PLAIN on the Skeleton (`bindTransforms`), not under
//     the `primvars:skel:` prefix -- that prefix belongs on the skinned MESH.
//     Reading only the prefixed name left every skeleton with an identity bind
//     pose, which silently skews every skinning matrix.
//   - they are doubles; asking for a float array yields nothing.
// The prefixed name is still accepted as a fallback for scenes that author it.
void ReadMatrix4dArray(const UsdPrim& prim, const char* name,
                       std::vector<double>* out) {
  const Value* val = prim.GetPropertyValue(name);
  if (!val) {
    val = prim.GetPropertyValue(std::string("primvars:skel:") + name);
  }
  if (!val || !val->is_array()) return;
  if (const std::vector<double>* darray = val->as_double_array()) {
    *out = *darray;
    return;
  }
  if (const std::vector<float>* farray = val->as_float_array()) {
    out->resize(farray->size());
    for (size_t i = 0; i < farray->size(); ++i) {
      (*out)[i] = static_cast<double>((*farray)[i]);
    }
  }
}

}  // namespace

bool GetSkeletonData(const Stage& stage, const UsdPrim& prim,
                     SkeletonData* out) {
  if (!IsSkeleton(prim) || !out) return false;

  (void)stage;

  // joints (uniform token[])
  {
    const Value* val = prim.GetPropertyValue("primvars:skel:joints");
    if (!val) {
      val = prim.GetPropertyValue("joints");
    }
    if (val) {
      if (const std::vector<std::string>* toks = val->as_token_array()) {
        out->joints = *toks;
      }
    }
  }

  // jointNames (uniform token[]) - optional display names
  {
    const Value* val = prim.GetPropertyValue("jointNames");
    if (val) {
      if (const std::vector<std::string>* toks = val->as_token_array()) {
        out->jointNames = *toks;
      }
    }
  }

  // bindTransforms (uniform matrix4d[]).
  {
    ReadMatrix4dArray(prim, "bindTransforms", &out->bindTransforms);
  }

  // primvars:skel:jointNames fallback: only consulted when the plain
  // `jointNames` above yielded nothing (appending to an already-read list
  // duplicated names). Array authoring is the common form and was
  // previously dropped — only scalar string/token was accepted.
  if (out->jointNames.empty()) {
    const Value* val = prim.GetPropertyValue("primvars:skel:jointNames");
    if (val) {
      if (const std::vector<std::string>* toks = val->as_token_array()) {
        out->jointNames = *toks;
      } else if (const std::string* s = val->as_string()) {
        out->jointNames.push_back(*s);
      } else if (const std::string* tok = val->as_token()) {
        out->jointNames.push_back(*tok);
      }
    }
  }

  // restTransforms (uniform matrix4d[]) — same addressing as bindTransforms.
  {
    ReadMatrix4dArray(prim, "restTransforms", &out->restTransforms);
  }

  // animationSource (rel skel:animationSource)
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("skel:animationSource");
    if (targets && !targets->empty()) {
      out->animationSource = (*targets)[0].str();
      out->hasAnimationSource = true;
    }
  }

  return true;
}

// ============================================================
// SkelAnimation data helpers
// ============================================================

namespace {

bool ReadFloat3Array(const Value* val, std::vector<float>* out) {
  if (!val || !out) return false;
  const std::vector<float>* arr = val->as_float_array();
  if (arr) {
    *out = *arr;
    return true;
  }
  return false;
}

bool ReadQuatArray(const Value* val, std::vector<float>* out) {
  if (!val || !out) return false;
  // quatf[] is stored as a flat float4[] in REAL-FIRST order (w, x, y, z) --
  // next's canonical quat layout, which the crate reader swizzles disk's
  // imaginary-first order into (CrateReader::Impl::UnpackQuatf), and which USDA
  // text authors directly. SkelAnimationData keeps that layout; consumers that
  // need xyzw (GPU / three.js quaternion order) swizzle at their own boundary
  // -- tydra-next's AnimationChannel does, for one.
  const std::vector<float>* arr = val->as_float_array();
  if (arr) {
    *out = *arr;
    return true;
  }
  return false;
}

} // namespace

bool GetSkelAnimationData(const Stage& stage, const UsdPrim& prim,
                          SkelAnimationData* out, double time) {
  if (!IsSkelAnimation(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // blendShapes (uniform token[]; accept scalar string/token authoring too)
  {
    const Value* val = prim.GetPropertyValue("blendShapes");
    if (val) {
      if (const std::vector<std::string>* toks = val->as_token_array()) {
        out->blendShapes = *toks;
      } else if (const std::string* s = val->as_string()) {
        out->blendShapes.push_back(*s);
      } else if (const std::string* tok = val->as_token()) {
        out->blendShapes.push_back(*tok);
      }
    }
  }

  // blendShapeWeights (float[])
  {
    EvalResult result = eval.Eval(prim, "blendShapeWeights");
    if (result.success && result.value.is_array()) {
      const std::vector<float>* arr = result.value.as_float_array();
      if (arr) {
        out->blendShapeWeights = *arr;
        out->hasBlendShapes = true;
      }
    }
  }

  // joints (uniform token[]). An ARRAY -- reading it with only the scalar token
  // accessors leaves it empty, which silently drops the whole animation (every
  // joint keeps its rest transform). Scalar string/token authoring is accepted
  // as a fallback.
  {
    const Value* val = prim.GetPropertyValue("joints");
    if (val) {
      if (const std::vector<std::string>* toks = val->as_token_array()) {
        out->joints = *toks;
      } else if (const std::string* s = val->as_string()) {
        out->joints.push_back(*s);
      } else if (const std::string* tok = val->as_token()) {
        out->joints.push_back(*tok);
      }
    }
  }

  // rotations (quatf[])
  {
    EvalResult result = eval.Eval(prim, "rotations");
    if (result.success) {
      if (ReadQuatArray(&result.value, &out->rotations)) {
        out->hasRotations = true;
      }
    }
  }

  // scales (half3[] stored as float3[])
  {
    EvalResult result = eval.Eval(prim, "scales");
    if (result.success) {
      if (ReadFloat3Array(&result.value, &out->scales)) {
        out->hasScales = true;
      }
    }
  }

  // translations (float3[])
  {
    EvalResult result = eval.Eval(prim, "translations");
    if (result.success) {
      if (ReadFloat3Array(&result.value, &out->translations)) {
        out->hasTranslations = true;
      }
    }
  }

  return true;
}

bool GetSkelAnimationDataAtTime(const Stage& stage, const UsdPrim& prim,
                                SkelAnimationData* out, double time) {
  return GetSkelAnimationData(stage, prim, out, time);
}

// ============================================================
// BlendShape data
// ============================================================

bool GetBlendShapeData(const Stage& stage, const UsdPrim& prim,
                       BlendShapeData* out) {
  if (!IsBlendShape(prim) || !out) return false;

  (void)stage;

  // offsets (uniform vector3f[])
  {
    const Value* val = prim.GetPropertyValue("offsets");
    if (val && val->is_array()) {
      const std::vector<float>* arr = val->as_float_array();
      if (arr) {
        out->offsets = *arr;
      }
    }
  }

  // normalOffsets (uniform vector3f[])
  {
    const Value* val = prim.GetPropertyValue("normalOffsets");
    if (val && val->is_array()) {
      const std::vector<float>* arr = val->as_float_array();
      if (arr) {
        out->normalOffsets = *arr;
        out->hasNormalOffsets = true;
      }
    }
  }

  // pointIndices (uniform int[])
  {
    const Value* val = prim.GetPropertyValue("pointIndices");
    if (val && val->is_array()) {
      const std::vector<int32_t>* arr = val->as_int_array();
      if (arr) {
        out->pointIndices = *arr;
        out->hasPointIndices = true;
      }
    }
  }

  // In-between shapes are namespaced vector3f[] attributes with authored
  // `weight` metadata on the attribute.
  const PrimSpec* spec = prim.GetPrimSpec();
  if (spec) {
    for (const std::string& property : prim.GetPropertyNames()) {
      if (property.rfind("inbetweens:", 0) != 0) continue;
      const Value* value = prim.GetPropertyValue(property);
      const std::vector<float>* offsets =
          value && value->is_array() ? value->as_float_array() : nullptr;
      if (!offsets || offsets->empty()) continue;
      BlendShapeData::Inbetween inbetween;
      inbetween.name = property.substr(std::strlen("inbetweens:"));
      inbetween.offsets = *offsets;
      if (const PropMeta* meta = spec->property_meta(property)) {
        if (meta->authored & PropMeta::kWeight) {
          inbetween.weight = static_cast<float>(meta->weight);
          inbetween.has_weight = true;
        }
      }
      out->inbetweens.push_back(std::move(inbetween));
    }
    std::stable_sort(out->inbetweens.begin(), out->inbetweens.end(),
                     [](const auto& a, const auto& b) {
                       return a.weight < b.weight;
                     });
  }

  return true;
}

// ============================================================
// SkelRoot data
// ============================================================

bool GetSkelRootData(const Stage& stage, const UsdPrim& prim,
                     SkelRootData* out) {
  if (!IsSkelRoot(prim) || !out) return false;

  (void)stage;

  // skel:skeleton
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("skel:skeleton");
    if (targets && !targets->empty()) {
      out->skeleton = (*targets)[0].str();
      out->hasSkeleton = true;
    }
  }

  // skel:animationSource
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("skel:animationSource");
    if (targets && !targets->empty()) {
      out->animationSource = (*targets)[0].str();
      out->hasAnimationSource = true;
    }
  }

  return true;
}

// ============================================================
// Utility functions
// ============================================================

bool BuildSkelTopology(const std::vector<std::string>& joints,
                       std::vector<int>& dst, std::string* err) {
  dst.clear();

  if (joints.empty()) {
    if (err) *err = "Empty joints array";
    return false;
  }

  dst.resize(joints.size(), -1);

  std::unordered_map<std::string, int> joint_by_path;
  joint_by_path.reserve(joints.size());
  for (size_t i = 0; i < joints.size(); ++i) {
    joint_by_path.emplace(joints[i], static_cast<int>(i));
  }

  // Build parent index from joint paths
  for (size_t i = 0; i < joints.size(); ++i) {
    const std::string& path = joints[i];

    // Find last '/' to get parent path
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
      // Root joint (no parent)
      dst[i] = -1;
      continue;
    }

    // Find the nearest ANCESTOR present in the joint list: UsdSkel allows
    // sparse joint lists (e.g. ["Root", "Root/Pelvis/Spine"]), where the
    // parent is the closest listed ancestor, not necessarily the immediate
    // path prefix (pxr UsdSkelTopology semantics).
    bool found = false;
    std::string parentPath = path.substr(0, lastSlash);
    while (!parentPath.empty()) {
      const auto parent = joint_by_path.find(parentPath);
      if (parent != joint_by_path.end()) {
        dst[i] = parent->second;
        found = true;
      }
      if (found) break;
      const size_t up = parentPath.rfind('/');
      if (up == std::string::npos || up == 0) break;
      parentPath = parentPath.substr(0, up);
    }

    if (!found) {
      // No listed ancestor at all: treat the joint as an extra root (pxr
      // tolerates this rather than failing the topology).
      dst[i] = -1;
    }
  }

  return true;
}

bool SkelValidateTopology(const std::vector<int>& topology,
                          std::string* err) {
  if (topology.empty()) {
    if (err) *err = "Empty topology";
    return false;
  }

  // Check for single root
  int rootCount = 0;
  for (size_t i = 0; i < topology.size(); ++i) {
    if (topology[i] == -1) {
      ++rootCount;
    }
  }

  if (rootCount != 1) {
    if (err) {
      *err = "Expected 1 root joint, found " + IntToStr(rootCount);
    }
    return false;
  }

  // Check for valid parent indices
  for (size_t i = 0; i < topology.size(); ++i) {
    int parent = topology[i];
    if (parent < -1 || parent >= static_cast<int>(topology.size())) {
      if (err) {
        *err = "Invalid parent index " + IntToStr(parent) +
               " at joint " + UIntToStr(i);
      }
      return false;
    }
  }

  // Check for cycles. Each node has exactly one parent pointer, so the parent
  // chain from any node either reaches the root or revisits a node (a cycle).
  // With one root and valid indices (checked above), a cycle-free topology is
  // also reachable from the root, so this subsumes the reachability check.
  // O(n) via a 3-color walk: 0 = unvisited, 1 = on the current walk,
  // 2 = proven acyclic.
  std::vector<int> state(topology.size(), 0);
  for (size_t i = 0; i < topology.size(); ++i) {
    if (state[i] != 0) continue;
    size_t cur = i;
    while (topology[cur] != -1 && state[cur] != 2) {
      if (state[cur] == 1) {
        if (err) *err = "Cycle detected at joint " + UIntToStr(cur);
        return false;
      }
      state[cur] = 1;
      cur = topology[cur];
    }
    for (size_t w = i; topology[w] != -1 && state[w] != 2;) {
      state[w] = 2;
      w = topology[w];
    }
  }

  return true;
}

bool SkelNormalizeWeights(std::vector<float>& weights,
                          int numInfluencesPerComponent, float eps) {
  if (weights.empty() || numInfluencesPerComponent <= 0) return false;

  size_t numComponents = weights.size() /
    static_cast<size_t>(numInfluencesPerComponent);

  for (size_t i = 0; i < numComponents; ++i) {
    float sum = 0.0f;
    for (int j = 0; j < numInfluencesPerComponent; ++j) {
      sum += weights[i * numInfluencesPerComponent + j];
    }

    if (sum > eps) {
      float invSum = 1.0f / sum;
      for (int j = 0; j < numInfluencesPerComponent; ++j) {
        weights[i * numInfluencesPerComponent + j] *= invSum;
      }
    }
  }

  return true;
}

bool SkelSortInfluences(std::vector<int>& indices,
                        std::vector<float>& weights,
                        int numInfluencesPerComponent) {
  if (indices.empty() || weights.empty() ||
      numInfluencesPerComponent <= 0) {
    return false;
  }

  size_t numComponents = weights.size() /
    static_cast<size_t>(numInfluencesPerComponent);

  for (size_t i = 0; i < numComponents; ++i) {
    // Create index array for sorting
    std::vector<std::pair<float, int>> influences;
    influences.reserve(numInfluencesPerComponent);

    for (int j = 0; j < numInfluencesPerComponent; ++j) {
      size_t offset = i * numInfluencesPerComponent + j;
      influences.emplace_back(weights[offset], indices[offset]);
    }

    // Sort by weight descending
    std::sort(influences.begin(), influences.end(),
              [](const std::pair<float, int>& a,
                 const std::pair<float, int>& b) {
                return a.first > b.first;
              });

    // Write back sorted
    for (int j = 0; j < numInfluencesPerComponent; ++j) {
      size_t offset = i * numInfluencesPerComponent + j;
      weights[offset] = influences[j].first;
      indices[offset] = influences[j].second;
    }
  }

  return true;
}

} // namespace next
} // namespace lightusd
