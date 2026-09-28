// SPDX-License-Identifier: Apache-2.0
// Compiled formatting and schema helpers for the portable C boundary.
#include "c-internal.hh"

#include <cmath>
#include <new>
#include <vector>

#include "next/schema/usd-skel.hh"
#include "next/writer/value-printer.hh"

namespace n = lightusd::next;
using lightusd_internal::Fail;
using lightusd_internal::FromC;

struct lightusd_skel_sample {
  n::SkeletonData skeleton;
  n::SkelAnimationData animation;
  std::vector<int32_t> parents;
};

extern "C" {

lightusd_status lightusd_value_to_usda(const lightusd_value* value,
                                        lightusd_string** out) {
  if (!value || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "value/out is null");
  *out = nullptr;
  lightusd_string* result = new (std::nothrow) lightusd_string();
  if (!result) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "string alloc failed");
  result->s = n::PrintValue(value->v);
  *out = result;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_skel_animation_joint_count_at_time(
    const lightusd_stage* stage, lightusd_prim prim, double time, size_t* out) {
  if (!stage || !out || !std::isfinite(time)) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid stage/count/time");
  }
  *out = 0;
  if (prim._owner != stage) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "prim belongs to another stage");
  }
  const n::UsdPrim native = FromC(prim);
  if (!native.IsValid()) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid prim");
  }
  if (!n::IsSkelAnimation(native)) {
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH, "prim is not SkelAnimation");
  }
  n::SkelAnimationData data;
  if (!n::GetSkelAnimationDataAtTime(stage->ReadStage(), native, &data, time)) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "SkelAnimation sample unavailable");
  }
  *out = data.joints.size();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_skel_sample_create(
    const lightusd_stage* stage, lightusd_prim skeleton,
    lightusd_prim animation, double time, lightusd_skel_sample** out) {
  if (out) *out = nullptr;
  if (!stage || !out || !std::isfinite(time) || skeleton._owner != stage ||
      (animation._owner && animation._owner != stage))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid skeleton sample arguments");
  const n::UsdPrim skel = FromC(skeleton);
  if (!skel.IsValid())
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid skeleton prim");
  if (!n::IsSkeleton(skel))
    return Fail(LIGHTUSD_ERR_TYPE_MISMATCH, "prim is not Skeleton");
  auto* sample = new (std::nothrow) lightusd_skel_sample;
  if (!sample)
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "skeleton sample allocation failed");
  if (!n::GetSkeletonData(stage->ReadStage(), skel, &sample->skeleton) ||
      sample->skeleton.joints.empty()) {
    delete sample;
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "Skeleton joints unavailable");
  }
  std::vector<int> topology;
  std::string error;
  if (!n::BuildSkelTopology(sample->skeleton.joints, topology, &error) ||
      topology.size() != sample->skeleton.joints.size()) {
    delete sample;
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid Skeleton topology");
  }
  sample->parents.assign(topology.begin(), topology.end());
  if (animation._owner == stage) {
    const n::UsdPrim anim = FromC(animation);
    if (anim.IsValid() && n::IsSkelAnimation(anim) &&
        !n::GetSkelAnimationData(stage->ReadStage(), anim,
                                 &sample->animation, time))
      sample->animation = {};
  }
  *out = sample;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_skel_sample_get_info(
    const lightusd_skel_sample* sample, lightusd_skel_sample_info* out) {
  if (out) *out = {};
  if (!sample || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "skeleton sample/out is null");
  const auto& skel = sample->skeleton;
  const auto& anim = sample->animation;
  out->joint_count = skel.joints.size();
  out->animation_joint_count = anim.joints.size();
  out->parent_indices = sample->parents.data();
  out->rest_transforms = skel.restTransforms.data();
  out->rest_transform_count = skel.restTransforms.size();
  out->bind_transforms = skel.bindTransforms.data();
  out->bind_transform_count = skel.bindTransforms.size();
  out->translations = anim.translations.data();
  out->translation_count = anim.translations.size();
  out->rotations = anim.rotations.data();
  out->rotation_count = anim.rotations.size();
  out->scales = anim.scales.data();
  out->scale_count = anim.scales.size();
  out->has_translations = anim.hasTranslations;
  out->has_rotations = anim.hasRotations;
  out->has_scales = anim.hasScales;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_skel_sample_joint_name(
    const lightusd_skel_sample* sample, size_t index, lightusd_sv* out) {
  if (out) *out = {};
  if (!sample || !out || index >= sample->skeleton.joints.size())
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid Skeleton joint index");
  *out = lightusd_internal::SV(sample->skeleton.joints[index]);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_skel_sample_animation_joint_name(
    const lightusd_skel_sample* sample, size_t index, lightusd_sv* out) {
  if (out) *out = {};
  if (!sample || !out || index >= sample->animation.joints.size())
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid SkelAnimation joint index");
  *out = lightusd_internal::SV(sample->animation.joints[index]);
  return LIGHTUSD_OK;
}

void lightusd_skel_sample_destroy(lightusd_skel_sample* sample) {
  delete sample;
}

}  // extern "C"
