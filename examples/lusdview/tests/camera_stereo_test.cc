// SPDX-License-Identifier: Apache-2.0
#include "camera_stereo.hh"

#include <cstdio>

namespace {

lusdview::DrawCameraCPU Camera(const char* name, const char* path,
                               lusdview::DrawCameraCPU::StereoRole role) {
  lusdview::DrawCameraCPU camera;
  camera.name = name;
  camera.displayName = name;
  camera.absPath = path;
  camera.stereoRole = role;
  return camera;
}

}  // namespace

int main() {
  using Role = lusdview::DrawCameraCPU::StereoRole;
  std::vector<lusdview::DrawCameraCPU> cameras = {
      Camera("Left", "/RigA/Left", Role::Left),
      Camera("Right", "/RigA/Right", Role::Right),
      Camera("Left", "/RigB/Left", Role::Left),
      Camera("Right", "/RigB/Right", Role::Right),
      Camera("Mono", "/RigA/Mono", Role::Mono)};
  lusdview::StereoCameraPair pair;
  std::string error;
  if (!lusdview::ResolveStereoCameraPair(cameras, "/RigA/Left", &pair,
                                         &error) ||
      pair.left != 0 || pair.right != 1) {
    std::fprintf(stderr, "same-parent stereo pairing failed: %s\n", error.c_str());
    return 1;
  }
  if (lusdview::ResolveStereoCameraPair(cameras, "", &pair, &error) ||
      error.find("multiple") == std::string::npos) {
    std::fprintf(stderr, "ambiguous automatic pairing was accepted\n");
    return 1;
  }
  if (lusdview::ResolveStereoCameraPair(cameras, "/RigA/Mono", &pair,
                                        &error) ||
      error.find("mono") == std::string::npos) {
    std::fprintf(stderr, "mono camera was accepted as a stereo eye\n");
    return 1;
  }
  cameras.resize(2);
  if (!lusdview::ResolveStereoCameraPair(cameras, "", &pair, &error) ||
      pair.left != 0 || pair.right != 1) {
    std::fprintf(stderr, "automatic unique pairing failed: %s\n", error.c_str());
    return 1;
  }
  cameras.push_back(Camera("FarRight", "/Other/FarRight", Role::Right));
  if (!lusdview::ResolveStereoCameraPair(cameras, "/RigA/Left", &pair,
                                         &error) ||
      pair.right != 1) {
    std::fprintf(stderr, "same-parent preference failed: %s\n", error.c_str());
    return 1;
  }
  return 0;
}
