// SPDX-License-Identifier: Apache-2.0
#include "camera_stereo.hh"

#include <map>

namespace lusdview {
namespace {

std::string ParentPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool MatchesCamera(const DrawCameraCPU& camera, const std::string& selected) {
  if (selected.empty()) return false;
  if (camera.name == selected || camera.displayName == selected ||
      camera.absPath == selected) {
    return true;
  }
  return camera.absPath.size() > selected.size() &&
         camera.absPath.compare(camera.absPath.size() - selected.size(),
                                selected.size(), selected) == 0 &&
         camera.absPath[camera.absPath.size() - selected.size() - 1] == '/';
}

void SetError(std::string* error, const std::string& value) {
  if (error) *error = value;
}

}  // namespace

bool ResolveStereoCameraPair(const std::vector<DrawCameraCPU>& cameras,
                             const std::string& selectedCamera,
                             StereoCameraPair* pair, std::string* error) {
  if (!pair) return false;
  *pair = StereoCameraPair{};
  if (error) error->clear();

  int selected = -1;
  if (!selectedCamera.empty()) {
    for (size_t i = 0; i < cameras.size(); ++i) {
      if (!MatchesCamera(cameras[i], selectedCamera)) continue;
      if (selected >= 0) {
        SetError(error, "selected camera name is ambiguous");
        return false;
      }
      selected = static_cast<int>(i);
    }
    if (selected < 0) {
      SetError(error, "selected stereo camera was not found");
      return false;
    }
    const DrawCameraCPU::StereoRole role = cameras[static_cast<size_t>(selected)].stereoRole;
    if (role == DrawCameraCPU::StereoRole::Mono) {
      SetError(error, "selected camera has stereoRole=mono");
      return false;
    }
    const DrawCameraCPU::StereoRole opposite =
        role == DrawCameraCPU::StereoRole::Left
            ? DrawCameraCPU::StereoRole::Right
            : DrawCameraCPU::StereoRole::Left;
    const std::string parent = ParentPath(cameras[static_cast<size_t>(selected)].absPath);
    std::vector<int> local;
    std::vector<int> global;
    for (size_t i = 0; i < cameras.size(); ++i) {
      if (static_cast<int>(i) == selected || cameras[i].stereoRole != opposite)
        continue;
      global.push_back(static_cast<int>(i));
      if (ParentPath(cameras[i].absPath) == parent)
        local.push_back(static_cast<int>(i));
    }
    const std::vector<int>& candidates = local.empty() ? global : local;
    if (candidates.size() != 1) {
      SetError(error, candidates.empty() ? "opposite stereo eye was not found"
                                         : "opposite stereo eye is ambiguous");
      return false;
    }
    if (role == DrawCameraCPU::StereoRole::Left) {
      pair->left = selected;
      pair->right = candidates[0];
    } else {
      pair->left = candidates[0];
      pair->right = selected;
    }
    return true;
  }

  struct ParentEyes {
    std::vector<int> left;
    std::vector<int> right;
  };
  std::map<std::string, ParentEyes> byParent;
  for (size_t i = 0; i < cameras.size(); ++i) {
    ParentEyes& eyes = byParent[ParentPath(cameras[i].absPath)];
    if (cameras[i].stereoRole == DrawCameraCPU::StereoRole::Left)
      eyes.left.push_back(static_cast<int>(i));
    else if (cameras[i].stereoRole == DrawCameraCPU::StereoRole::Right)
      eyes.right.push_back(static_cast<int>(i));
  }
  std::vector<StereoCameraPair> candidates;
  for (const auto& entry : byParent) {
    if (entry.second.left.size() == 1 && entry.second.right.size() == 1)
      candidates.push_back({entry.second.left[0], entry.second.right[0]});
  }
  if (candidates.size() != 1) {
    SetError(error, candidates.empty()
                        ? "no unambiguous left/right camera pair was found"
                        : "multiple stereo pairs exist; select one eye with --camera");
    return false;
  }
  *pair = candidates[0];
  return true;
}

}  // namespace lusdview
