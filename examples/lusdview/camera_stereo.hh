// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#include "gpu_scene.hh"

namespace lusdview {

struct StereoCameraPair {
  int left{-1};
  int right{-1};
  bool valid() const { return left >= 0 && right >= 0 && left != right; }
};

// Resolve a USD stereo pair from stereoRole metadata. An explicitly selected
// eye is paired with the opposite role under the same parent when possible;
// otherwise a unique scene-wide opposite eye is accepted. Without a selected
// camera, exactly one same-parent left/right pair must exist.
bool ResolveStereoCameraPair(const std::vector<DrawCameraCPU>& cameras,
                             const std::string& selectedCamera,
                             StereoCameraPair* pair, std::string* error);

}  // namespace lusdview
