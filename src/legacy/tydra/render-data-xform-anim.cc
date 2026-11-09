// SPDX-License-Identifier: Apache-2.0
// Xform animation extraction split from render-data.cc.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "common-macros.inc"
#include "lightusd.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "usdGeom.hh"

namespace lightusd {
namespace tydra {

void RenderSceneConverter::ExtractXformAnimations(
    const RenderSceneConverterEnv &env, const XformNode &xform_node) {
  // Single-pass depth-first traversal with stable node indices.
  // This avoids repeatedly counting subtree sizes.
  std::function<void(const XformNode&, int32_t&, int32_t)> extractAnimationsFromNode;
  extractAnimationsFromNode = [&](const XformNode& node, int32_t& next_node_index, int32_t depth) {
    if (size_t(depth) >= kMaxDefaultTraversalLimit) return;
    const int32_t node_index = next_node_index++;

    if (node.prim) {
      const Path &prim_path = node.absolute_path;

      // Check if this node has a prim with xformOps.
      if (IsXformablePrim(*node.prim)) {
        const Xformable *xformable = nullptr;
        if (CastToXformable(*node.prim, &xformable) && xformable) {
          AnimationClip anim;
          bool converted = false;

          // Prefer value clip animation baking when enabled.
          if (env.scene_config.enable_value_clips &&
              ConvertValueClipAnimation(env, *node.prim, prim_path,
                                        node_index, &anim)) {
            converted = true;
          }

          // Fallback to direct xformOp sampling when no clip animation exists.
          if (!converted && xformable->has_timesamples()) {
            if (ExtractXformOpAnimation(env, prim_path, node.element_name,
                                        *xformable, node_index, &anim)) {
              converted = true;
            }
          }

          if (converted) {
            // Check if animation with this path already exists via O(1) lookup
            const auto &anim_abs_path = anim.abs_path;
            if (_animPathToIndex.find(anim_abs_path) ==
                _animPathToIndex.end()) {
              DCOUT("Extracted animation from: " << anim_abs_path);
              _animPathToIndex[anim_abs_path] =
                  int32_t(animations.size());
              animations.emplace_back(std::move(anim));
            }
          }
        }
      }

      AnimationClip property_anim;
      if (ExtractPrimPropertyAnimation(env, *node.prim, prim_path,
                                       node_index, &property_anim)) {
        const std::string property_anim_key =
            property_anim.abs_path + "#properties";
        if (_animPathToIndex.find(property_anim_key) ==
            _animPathToIndex.end()) {
          DCOUT("Extracted property animation from: "
                << property_anim.abs_path);
          _animPathToIndex[property_anim_key] =
              int32_t(animations.size());
          animations.emplace_back(std::move(property_anim));
        }
      }
    }

    for (const auto& child : node.children) {
      extractAnimationsFromNode(child, next_node_index, depth + 1);
    }
  };

  int32_t current_node_index = 0;
  for (const auto& root : xform_node.children) {
    extractAnimationsFromNode(root, current_node_index, 0);
  }
 }

}  // namespace tydra
}  // namespace lightusd
