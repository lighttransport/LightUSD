// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
//
// Animation, skeleton, and light conversion routines split from render-data.cc
//
#include <algorithm>
#include <numeric>
#include <set>
#include <limits>
#include <unordered_map>

#include "common-utils.hh"
#include "common-types.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#include "safe-arithmetic.hh"
#include "linear-algebra.hh"
#include "math-util.inc"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "value-clip-utils.hh"
#include "value-pprint.hh"
#include "bone-util.hh"
#include "shape-to-mesh.hh"
#include "materialx-to-json.hh"
#include "mmap-array-ref.hh"

//
#include "common-macros.inc"
#include "math-util.inc"


//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

namespace lightusd {

namespace tydra {

// Helper: populate flat topology/transform arrays on SkelHierarchy from a Skeleton prim.
static bool PopulateSkelFlatArrays(const Skeleton &skel, SkelHierarchy &dst, std::string *err) {
  std::vector<value::token> joints;
  if (!skel.joints.get_value(&joints) || joints.empty()) {
    return true;  // No joints authored; leave flat arrays empty
  }

  // Build topology
  if (!BuildSkelTopology(joints, dst.parent_joint_indices, err)) {
    return false;
  }

  // Bind transforms
  if (skel.bindTransforms.authored()) {
    if (!skel.bindTransforms.get_value(&dst.bind_transforms) ||
        dst.bind_transforms.empty()) {
      dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
    }
  } else {
    dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
  }
  if (dst.bind_transforms.size() != joints.size()) {
    dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
  }

  // Rest transforms
  bool restAuthored = skel.restTransforms.authored();
  if (skel.restTransforms.authored()) {
    if (!skel.restTransforms.get_value(&dst.rest_transforms) ||
        dst.rest_transforms.empty()) {
      restAuthored = false;
    }
  }
  if (!restAuthored && dst.bind_transforms.size() == joints.size()) {
    dst.rest_transforms.resize(joints.size());
    for (size_t i = 0; i < joints.size(); i++) {
      int parentIdx = dst.parent_joint_indices[i];
      if (parentIdx < 0) {
        dst.rest_transforms[i] = dst.bind_transforms[i];
      } else {
        value::matrix4d parentInverse;
        if (inverse(dst.bind_transforms[size_t(parentIdx)], parentInverse)) {
          dst.rest_transforms[i] = dst.bind_transforms[i] * parentInverse;
        } else {
          dst.rest_transforms[i] = value::matrix4d::identity();
        }
      }
    }
  } else if (dst.rest_transforms.size() != joints.size()) {
    dst.rest_transforms.assign(joints.size(), value::matrix4d::identity());
  }

  return true;
}

bool RenderSceneConverter::ConvertSkeletonFromPtr(const RenderSceneConverterEnv &env,
                       const Path &skelPath,
                       const Skeleton &skel,
                       const std::string &primName,
                       SkelHierarchy *out_skel) {
  (void)env;

  if (!out_skel) {
    return false;
  }

  SkelHierarchy dst;
  SkelNode root;
  if (!BuildSkelHierarchy(skel, root, &_err)) {
    return false;
  }
  dst.abs_path = skelPath.prim_part();
  dst.prim_name = primName;
  dst.display_name = skel.metas().has_displayName() ? skel.metas().get_displayName() : "";
  dst.root_node = root;

  PopulateSkelFlatArrays(skel, dst, &_err);

  (*out_skel) = std::move(dst);
  return true;
}

bool RenderSceneConverter::ConvertSkeletonImplWithPath(const RenderSceneConverterEnv &env, const Path &skelPath,
                       SkelHierarchy *out_skel) {

  if (!out_skel) {
    return false;
  }

  if (skelPath.is_valid()) {
    const Prim *skelPrim{nullptr};
    if (!env.stage.find_prim_at_path(skelPath, skelPrim, &_err)) {
      return false;
    }

    SkelHierarchy dst;
    if (const auto pskel = skelPrim->as<Skeleton>()) {
      SkelNode root;
      if (!BuildSkelHierarchy((*pskel), root, &_err)) {
        return false;
      }
      dst.abs_path = skelPath.prim_part();
      dst.prim_name = skelPrim->element_name();
      dst.display_name = pskel->metas().has_displayName() ? pskel->metas().get_displayName() : "";
      dst.root_node = root;

      PopulateSkelFlatArrays(*pskel, dst, &_err);
    } else {
      PUSH_ERROR_AND_RETURN("Prim is not Skeleton.");
    }

    (*out_skel) = std::move(dst);
    return true;
  }

  PUSH_ERROR_AND_RETURN("`skel:skeleton` path is invalid.");
}

bool RenderSceneConverter::ConvertAllSkelAnimations(const RenderSceneConverterEnv &env) {
  // This method processes all SkelAnimation prims discovered during pre-processing.
  // For each SkelAnimation, we find which Skeleton it belongs to via:
  //   1. Skeleton's skel:animationSource relationship
  //   2. SkelRoot's skel:animationSource relationship (inherited per USD spec)
  //   3. Parent path hierarchy (SkelAnimation as child of Skeleton)

  if (!_allAnimations || _allAnimations->empty()) {
    return true; // No animations to process
  }

  DCOUT("ConvertAllSkelAnimations: processing " << _allAnimations->size() << " SkelAnimation prims");

  // Build reverse map: animationPath -> list of skeleton_ids that reference it
  std::unordered_map<std::string, std::vector<int32_t>, FNV1StringHash>
      animPathToSkelIds;
  animPathToSkelIds.reserve(_allAnimations->size());

  // Helper: extract animation paths from a Relationship
  auto extractAnimPaths = [](const Relationship &rel, std::vector<Path> &out) {
    if (rel.is_path()) {
      out.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      out.insert(out.end(), rel.targetPathVector.begin(), rel.targetPathVector.end());
    }
  };

  // 1. Check Skeleton prims for skel:animationSource
  for (const auto &skelEntry : _skelPathToIndex) {
    const std::string &skelPathStr = skelEntry.first;
    const int32_t skel_id = skelEntry.second;

    Path skelPath(skelPathStr, "");
    const Prim *skelPrim{nullptr};
    if (!env.stage.find_prim_at_path(skelPath, skelPrim, &_err)) {
      continue;
    }

    const auto *pskel = skelPrim->as<Skeleton>();
    if (!pskel) continue;

    std::vector<Path> animPaths;

    if (pskel->animationSource.has_value()) {
      extractAnimPaths(pskel->animationSource.value(), animPaths);
    }

    // 2. If Skeleton has no animationSource, check ancestor SkelRoot prims
    //    (implements USD SkelBindingAPI inheritance)
    if (animPaths.empty() && _allSkelRoots) {
      // Walk up the path hierarchy to find a SkelRoot with animationSource
      size_t iter = 0;
      std::string parentPath = skelPathStr;
      while (!parentPath.empty()) {
        if (iter++ >= kMaxDefaultTraversalLimit) break;
        size_t lastSlash = parentPath.rfind('/');
        if (lastSlash == 0 || lastSlash == std::string::npos) {
          parentPath = "/";  // root
        } else {
          parentPath = parentPath.substr(0, lastSlash);
        }

        auto rootIt = _allSkelRoots->find(parentPath);
        if (rootIt != _allSkelRoots->end() && rootIt->second) {
          const SkelRoot *pskelRoot = rootIt->second;
          if (pskelRoot->animationSource.has_value()) {
            extractAnimPaths(pskelRoot->animationSource.value(), animPaths);
            DCOUT("Inherited animationSource from SkelRoot " << parentPath
                  << " for Skeleton " << skelPathStr);
            break;
          }
        }
        if (parentPath == "/") break;
      }
    }

    for (const Path &animPath : animPaths) {
      std::string ap = animPath.prim_part();
      animPathToSkelIds[ap].push_back(skel_id);
    }
  }

  DCOUT("Built reverse map: " << animPathToSkelIds.size() << " animations referenced by skeletons");

  // 3. For SkelAnimation prims not referenced by any animationSource,
  //    associate them with a parent Skeleton by path hierarchy.
  //    This enables multi-clip workflows where SkelAnimation prims are children
  //    of a Skeleton but not all are the active animationSource.
  for (const auto &animEntry : *_allAnimations) {
    const std::string &animPathStr = animEntry.first;

    // Skip if already referenced
    if (animPathToSkelIds.find(animPathStr) != animPathToSkelIds.end()) {
      continue;
    }

    // Walk up parent path to find a Skeleton
    size_t iter = 0;
    std::string parentPath = animPathStr;
    while (!parentPath.empty()) {
      if (iter++ >= kMaxDefaultTraversalLimit) break;
      size_t lastSlash = parentPath.rfind('/');
      if (lastSlash == 0 || lastSlash == std::string::npos) {
        parentPath.clear();
        break;
      }
      parentPath = parentPath.substr(0, lastSlash);

      auto skelIt = _skelPathToIndex.find(parentPath);
      if (skelIt != _skelPathToIndex.end()) {
        animPathToSkelIds[animPathStr].push_back(skelIt->second);
        DCOUT("Associated SkelAnimation " << animPathStr
              << " with parent Skeleton " << parentPath
              << " (skeleton_id=" << skelIt->second << ")");
        break;
      }
    }

    if (animPathToSkelIds.find(animPathStr) == animPathToSkelIds.end()) {
      DCOUT("SkelAnimation " << animPathStr << " has no associated skeleton (skipping)");
    }
  }

  // Now convert each SkelAnimation prim
  for (const auto &animEntry : *_allAnimations) {
    const std::string &animPathStr = animEntry.first;
    const SkelAnimation *panimPtr = animEntry.second;

    if (!panimPtr) {
      PUSH_WARN("Null SkelAnimation pointer for path: " + animPathStr);
      continue;
    }

    auto it = animPathToSkelIds.find(animPathStr);
    if (it == animPathToSkelIds.end() || it->second.empty()) {
      DCOUT("SkelAnimation " << animPathStr << " not associated with any skeleton (skipping)");
      continue;
    }

    // Convert the animation for each skeleton that references it
    for (int32_t skeleton_id : it->second) {
      std::string cacheKey = animPathStr + ":" + std::to_string(skeleton_id);
      if (_animPathToIndex.find(cacheKey) != _animPathToIndex.end()) {
        DCOUT("Animation " << animPathStr << " already converted for skeleton " << skeleton_id);
        continue;
      }

      Path animPath(animPathStr, "");
      AnimationClip anim;

      if (!ConvertSkelAnimation(env, animPath, *panimPtr, skeleton_id, &anim)) {
        PushWarn(fmt::format(
            "Skipping invalid SkelAnimation {} for skeleton {}: {}\n",
            animPathStr, skeleton_id, GetError()));
        _err.clear();
        continue;
      }

      DCOUT("Converted SkelAnimation " << animPathStr << " for skeleton " << skeleton_id);

      // Add to animations vector
      int32_t anim_id = int32_t(animations.size());
      _animPathToIndex[cacheKey] = anim_id;
      animations.emplace_back(std::move(anim));

      // Update skeleton's animation IDs.
      if (skeleton_id >= 0 && skeleton_id < int32_t(skeletons.size())) {
        auto &skel = skeletons[static_cast<size_t>(skeleton_id)];
        // animPath+skeleton_id is deduplicated via _animPathToIndex cache.
        skel.anim_ids.push_back(anim_id);

        // Keep legacy default animation field for backward compatibility.
        if (skeletons[static_cast<size_t>(skeleton_id)].anim_id < 0) {
          skeletons[static_cast<size_t>(skeleton_id)].anim_id = anim_id;
          DCOUT("Set skeleton " << skeleton_id << " anim_id to " << anim_id);
        }
      }
    }
  }

  DCOUT("ConvertAllSkelAnimations: converted " << animations.size() << " animation clips");
  return true;
}

size_t ResolveLightLinking(const Stage &stage, RenderScene *scene) {
  if (!scene) {
    return 0;
  }

  size_t resolved = 0;

  for (size_t li = 0; li < scene->lights.size(); li++) {
    RenderLight &light = scene->lights[li];

    Path lpath(light.abs_path, "");
    if (!lpath.is_valid()) {
      continue;
    }
    auto pret = stage.GetPrimAtPath(lpath);
    if (!pret || !pret.value()) {
      continue;
    }
    const Prim &light_prim = *pret.value();
    const Collection *coll = nullptr;
    GetCollection(light_prim, &coll);

    // Resolve one link collection instance ("lightLink" / "shadowLink").
    auto resolve_link = [&](const std::string &inst_name, bool *links_all,
                            std::vector<int> *mesh_indices) -> bool {
      CollectionMembershipQuery q;
      const CollectionInstance *inst = nullptr;
      if (coll && coll->get_instance(inst_name, &inst) && inst) {
        const bool authored = inst->has_membershipExpression() ||
                              inst->includes.authored() ||
                              inst->excludes.authored();
        if (!authored) return false;
        q = BuildCollectionMembershipQuery(stage, *inst, light.abs_path);
      } else {
        // Older/legacy prim reconstruction may retain multi-apply CollectionAPI
        // properties as generic properties rather than a typed Collection.
        // Resolve relationship mode directly so light linking does not depend
        // on whether `apiSchemas = ["CollectionAPI:<name>"]` was authored.
        const std::string base = "collection:" + inst_name + ":";
        Relationship includes_rel;
        Relationship excludes_rel;
        std::string includes_err;
        std::string excludes_err;
        const bool has_includes = GetRelationship(
            light_prim, base + "includes", &includes_rel, &includes_err);
        const bool has_excludes = GetRelationship(
            light_prim, base + "excludes", &excludes_rel, &excludes_err);
        if (!has_includes && !has_excludes) return false;
        q.mode = CollectionMembershipQuery::Mode::Relationship;
        q.owner_prim_path = light.abs_path;
        if (has_includes) {
          if (includes_rel.is_path()) {
            q.includes.push_back(includes_rel.targetPath);
          }
          if (includes_rel.is_pathvector()) {
            q.includes = includes_rel.targetPathVector;
          }
        }
        if (has_excludes) {
          if (excludes_rel.is_path()) {
            q.excludes.push_back(excludes_rel.targetPath);
          }
          if (excludes_rel.is_pathvector()) {
            q.excludes = excludes_rel.targetPathVector;
          }
        }
        Attribute attr;
        std::string attr_err;
        if (GetAttribute(light_prim, base + "includeRoot", &attr, &attr_err)) {
          if (auto value = attr.get_value<bool>()) q.include_root = *value;
        }
        attr_err.clear();
        if (GetAttribute(light_prim, base + "expansionRule", &attr,
                         &attr_err)) {
          if (auto value = attr.get_value<std::string>()) {
            if (*value == "explicitOnly") {
              q.expansion_rule =
                  CollectionInstance::ExpansionRule::ExplicitOnly;
            } else if (*value == "expandPrimsAndProperties") {
              q.expansion_rule =
                  CollectionInstance::ExpansionRule::ExpandPrimsAndProperties;
            }
          }
        }
      }

      *links_all = false;
      mesh_indices->clear();
      for (size_t mi = 0; mi < scene->meshes.size(); mi++) {
        const std::string &mpath = scene->meshes[mi].abs_path;
        if (mpath.empty()) {
          continue;
        }
        if (IsPathIncluded(q, stage, Path(mpath, ""))) {
          mesh_indices->push_back(static_cast<int>(mi));
        }
      }
      return true;
    };

    bool any = false;
    if (resolve_link("lightLink", &light.light_links_all,
                     &light.light_link_mesh_indices)) {
      any = true;
    }
    if (resolve_link("shadowLink", &light.shadow_links_all,
                     &light.shadow_link_mesh_indices)) {
      any = true;
    }
    if (any) {
      resolved++;
    }
  }

  return resolved;
}

}  // namespace tydra
}  // namespace lightusd
