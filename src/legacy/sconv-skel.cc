// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage-converter: Skeleton prim property extraction.
//

#include "sconv-detail.hh"
#include "usdSkel.hh"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {
namespace experimental {

bool CrateWriter::ExtractSkeletonProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const Skeleton* skel = prim.data().as<Skeleton>();
  if (!skel) {
    if (err) *err = "Failed to cast prim to Skeleton";
    return false;
  }

  // Extract xformOps from the Xformable base class
  ExtractXformOpsFromXformable(prim, prim_path, fields, err);

  // joints (uniform token[])
  if (!AddTypedArrayAttribute("joints", skel->joints, fields)) return false;

  // jointNames (uniform token[])
  if (!AddTypedArrayAttribute("jointNames", skel->jointNames, fields)) return false;

  // bindTransforms (uniform matrix4d[])
  if (skel->bindTransforms.authored()) {
    auto bt_opt = skel->bindTransforms.get_value();
    if (bt_opt.has_value()) {
      const auto &bt_val = bt_opt.value();
      crate::CrateValue crate_val;
      value::Value val(bt_val);
      if (ConvertValue(val, crate_val, err)) {
        fields.push_back({"bindTransforms", crate_val});
      }
    }
  }

  // restTransforms (uniform matrix4d[])
  if (skel->restTransforms.authored()) {
    auto rt_opt = skel->restTransforms.get_value();
    if (rt_opt.has_value()) {
      const auto &rt_val = rt_opt.value();
      crate::CrateValue crate_val;
      value::Value val(rt_val);
      if (ConvertValue(val, crate_val, err)) {
        fields.push_back({"restTransforms", crate_val});
      }
    }
  }

  // visibility
  if (skel->visibility.authored()) {
    const auto& visibility_anim = skel->visibility.get_value();
    if (visibility_anim.has_default()) {
      Visibility vis_val;
      if (visibility_anim.get_default(&vis_val)) {
        crate::CrateValue crate_val;
        value::token tok(to_string(vis_val));
        crate_val.Set(tok);
        fields.push_back({"visibility", crate_val});
      }
    }
  }

  // purpose
  if (skel->purpose.authored()) {
    crate::CrateValue crate_val;
    value::token tok(to_string(skel->purpose.get_value()));
    crate_val.Set(tok);
    fields.push_back({"purpose", crate_val});
  }

  return true;
}

// ============================================================================
// SkelAnimation Property Extraction
// ============================================================================

bool CrateWriter::ExtractSkelAnimationProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const SkelAnimation* anim = prim.data().as<SkelAnimation>();
  if (!anim) {
    if (err) *err = "Failed to cast prim to SkelAnimation";
    return false;
  }

  // joints (uniform token[])
  if (!AddTypedArrayAttribute("joints", anim->joints, fields)) return false;

  // blendShapes (uniform token[])
  if (!AddTypedArrayAttribute("blendShapes", anim->blendShapes, fields)) return false;

  // rotations (quatf[], Animatable)
  if (anim->rotations.has_value()) {
    auto opt = anim->rotations.get_value();
    if (opt) {
      std::string conv_err;
      if (!ExtractAnimatableDefault(*opt, "rotations", fields, &conv_err)) {
        DCOUT("WARNING: Skipping unsupported Animatable type for rotations: " << conv_err);
      }
    }
  }

  // translations (float3[], Animatable)
  if (anim->translations.has_value()) {
    auto opt = anim->translations.get_value();
    if (opt) {
      std::string conv_err;
      if (!ExtractAnimatableDefault(*opt, "translations", fields, &conv_err)) {
        DCOUT("WARNING: Skipping unsupported Animatable type for translations: " << conv_err);
      }
    }
  }

  // scales (half3[], Animatable)
  if (anim->scales.has_value()) {
    auto opt = anim->scales.get_value();
    if (opt) {
      std::string conv_err;
      if (!ExtractAnimatableDefault(*opt, "scales", fields, &conv_err)) {
        DCOUT("WARNING: Skipping unsupported Animatable type for scales: " << conv_err);
      }
    }
  }

  // blendShapeWeights (float[], Animatable)
  if (anim->blendShapeWeights.has_value()) {
    auto opt = anim->blendShapeWeights.get_value();
    if (opt) {
      std::string conv_err;
      if (!ExtractAnimatableDefault(*opt, "blendShapeWeights", fields, &conv_err)) {
        DCOUT("WARNING: Skipping unsupported Animatable type for blendShapeWeights: " << conv_err);
      }
    }
  }

  return true;
}

// ============================================================================
// SkelRoot Property Extraction
// ============================================================================

bool CrateWriter::ExtractSkelRootProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const SkelRoot* skel_root = prim.data().as<SkelRoot>();
  if (!skel_root) {
    if (err) *err = "Failed to cast prim to SkelRoot";
    return false;
  }

  // SkelRoot is Xformable -- it is the transform root of the rig -- but this
  // writer never emitted its xformOps, so the whole rig snapped back to the
  // origin on write. ExtractSkeletonProperties already does this.
  ExtractXformOpsFromXformable(prim, prim_path, fields, err);

  // SkelRoot has no dedicated attributes beyond visibility, purpose, and extent
  // Extract visibility
  if (skel_root->visibility.authored()) {
    const auto& visibility_anim = skel_root->visibility.get_value();
    if (visibility_anim.has_default()) {
      Visibility visibility_val;
      if (visibility_anim.get_default(&visibility_val)) {
        crate::CrateValue crate_val;
        value::token tok(to_string(visibility_val));
        crate_val.Set(tok);
        fields.push_back({"visibility", crate_val});
      }
    }
  }

  // Extract purpose
  if (skel_root->purpose.authored()) {
    const auto& purpose_val = skel_root->purpose.get_value();
    crate::CrateValue crate_val;
    value::token tok(to_string(purpose_val));
    crate_val.Set(tok);
    fields.push_back({"purpose", crate_val});
  }

  // Extract extent
  if (skel_root->extent.has_value()) {
    const auto& extent_opt = skel_root->extent.get_value();
    if (extent_opt) {
      const auto& extent_animatable = extent_opt.value();
      if (extent_animatable.has_default()) {
        Extent extent_val;
        if (extent_animatable.get_default(&extent_val)) {
          crate::CrateValue crate_val;
          std::vector<value::float3> extent_vec = {extent_val.lower, extent_val.upper};
          value::Value val(extent_vec);
          if (ConvertValue(val, crate_val, err)) {
            fields.push_back({"extent", crate_val});
          }
        }
      }

      // ANIMATED extent. This branch did not exist, so `extent.timeSamples` was
      // dropped wholesale. Samples hold an Extent struct in memory and float3[2]
      // on disk -- the same shape conversion the default above does.
      if (extent_animatable.has_timesamples()) {
        const value::TimeSamples *ext_ts = extent_animatable.get_timesamples_ptr();

        value::TimeSamples ts;
        const auto &samples = ext_ts->get_samples();
        for (size_t i = 0; i < samples.size(); i++) {
          if (samples[i].blocked) {
            ts.add_blocked_sample(samples[i].t, value::Value());
          } else if (const Extent *ev = samples[i].value.as<Extent>()) {
            std::vector<value::float3> ev_vec = {ev->lower, ev->upper};
            ts.add_sample(samples[i].t, value::Value(ev_vec));
          } else {
            ts.add_sample(samples[i].t, samples[i].value);
          }
        }

        crate::CrateValue ts_crate_val;
        ts_crate_val.Set(ts);
        fields.push_back({"extent.timeSamples", ts_crate_val});
      }
    }
  }

  return true;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
