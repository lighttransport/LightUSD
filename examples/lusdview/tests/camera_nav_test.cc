// SPDX-License-Identifier: Apache-2.0
#include "camera_nav.hh"
#include "rt_camera.hh"

#include <cmath>
#include <cstdio>

namespace {

bool Near(float a, float b, float eps = 1.0e-5f) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  const lusdview::RtCameraLens dof =
      lusdview::MakeRtCameraLens(50.0f, 8.0f, 2.0f, true);
  if (!dof.enabled() || !Near(dof.focusDistance, 8.0f) ||
      !Near(dof.apertureRadius, 1.25f)) {
    std::fprintf(stderr, "thin-lens camera conversion is incorrect\n");
    return 1;
  }
  if (lusdview::MakeRtCameraLens(50.0f, 8.0f, 0.0f, true).enabled() ||
      lusdview::MakeRtCameraLens(50.0f, 8.0f, 2.0f, false).enabled()) {
    std::fprintf(stderr, "pinhole/orthographic camera enabled depth of field\n");
    return 1;
  }
  const float pickedFocus = lusdview::RtFocusDistanceToPoint(
      {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -2.0f}, {5.0f, -4.0f, -7.0f});
  if (!Near(pickedFocus, 10.0f) ||
      lusdview::RtFocusDistanceToPoint(
          {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
          {0.0f, 0.0f, -1.0f}) != 0.0f) {
    std::fprintf(stderr, "picked-point focus-plane distance is incorrect\n");
    return 1;
  }
  const lusdview::RtCameraLens autoMeters =
      lusdview::MakeAutoRtCameraLens(0.72f, 4.0f, 1.0);
  const lusdview::RtCameraLens autoCentimeters =
      lusdview::MakeAutoRtCameraLens(72.0f, 4.0f, 0.01);
  if (!autoMeters.enabled() || !autoCentimeters.enabled() ||
      !Near(autoMeters.apertureRadius, 0.00625f) ||
      !Near(autoCentimeters.apertureRadius, 0.625f)) {
    std::fprintf(stderr, "auto-camera physical aperture scale is incorrect\n");
    return 1;
  }
  const lusdview::RtCameraShutter shutter{-0.25, 0.5};
  if (!shutter.enabled() || std::fabs(shutter.width() - 0.75) > 1.0e-12 ||
      std::fabs(lusdview::RtShutterSampleTime(10.0, shutter, 0, 3) -
                9.875) > 1.0e-12 ||
      std::fabs(lusdview::RtShutterSampleTime(10.0, shutter, 2, 3) -
                10.375) > 1.0e-12 ||
      lusdview::RtShutterSampleTime(
          10.0, lusdview::RtCameraShutter{0.0, 0.0}, 0, 8) != 10.0) {
    std::fprintf(stderr, "camera shutter stratification is incorrect\n");
    return 1;
  }

  lusdview::OrbitCamera camera;
  const float boundsMin[3] = {-1.0f, -1.0f, -1.0f};
  const float boundsMax[3] = {1.0f, 1.0f, 1.0f};
  camera.setSceneBounds(boundsMin, boundsMax);

  camera.setAutoClip(false);
  camera.setClipPlanes(1.0f, 11.0f);
  camera.setAspect(2.0f);
  camera.setProjection(lusdview::CameraProjection::Orthographic);
  camera.setOrthographicHeight(4.0f);
  camera.setLensShift(0.25f, -0.5f);
  camera.setExposure(1.25f);
  if (!Near(camera.exposure(), 1.25f)) {
    std::fprintf(stderr, "authored camera exposure was not retained\n");
    return 1;
  }
  const light3d::Mat4 orthoGl = camera.proj(false);
  const light3d::Mat4 orthoVk = camera.proj(true);
  if (!Near(orthoGl.m[0], 0.25f) || !Near(orthoGl.m[5], 0.5f) ||
      !Near(orthoGl.m[12], -0.25f) || !Near(orthoGl.m[13], 0.5f) ||
      !Near(orthoGl.m[10], -0.2f) || !Near(orthoGl.m[14], -1.2f) ||
      !Near(orthoVk.m[10], -0.1f) || !Near(orthoVk.m[14], -0.1f)) {
    std::fprintf(stderr, "authored orthographic projection is incorrect\n");
    return 1;
  }
  camera.setProjection(lusdview::CameraProjection::Perspective);
  const light3d::Mat4 shiftedPerspective = camera.proj(false);
  if (!Near(shiftedPerspective.m[8], 0.25f) ||
      !Near(shiftedPerspective.m[9], -0.5f)) {
    std::fprintf(stderr, "perspective filmback offset is incorrect\n");
    return 1;
  }
  camera.setLensShift(0.0f, 0.0f);
  camera.setAspect(1.0f);
  camera.setAspectOverride(2.0f);
  camera.setAspectOverrideEnabled(true);
  camera.setConform(lusdview::CameraConform::Fit);
  const light3d::Mat4 fitProjection = camera.proj(false);
  camera.setConform(lusdview::CameraConform::Vertical);
  const light3d::Mat4 verticalProjection = camera.proj(false);
  camera.setConform(lusdview::CameraConform::None);
  const light3d::Mat4 noneProjection = camera.proj(false);
  if (!Near(fitProjection.m[5] * 2.0f, verticalProjection.m[5]) ||
      !Near(noneProjection.m[0] * 2.0f, noneProjection.m[5])) {
    std::fprintf(stderr, "filmback conform policy is incorrect\n");
    return 1;
  }
  camera.setAspectOverrideEnabled(false);
  camera.setAutoClip(true);

  // Outside a compact scene, auto-clip should spend depth precision on the
  // occupied range instead of retaining an inspection-scale near plane.
  camera.setOrbit(light3d::Vec3{0.0f, 0.0f, 0.0f}, 0.6f, 0.35f, 100.0f);
  if (!(camera.nearPlane() > 1.0f) || !(camera.farPlane() < 110.0f) ||
      !(camera.farPlane() > camera.nearPlane())) {
    std::fprintf(stderr, "outside-scene auto clip is not bounds-aware\n");
    return 1;
  }

  // Inside architecture, retain a close inspection plane even if the orbit
  // pivot is not on the nearest surface.
  camera.setOrbit(light3d::Vec3{0.0f, 0.0f, 0.0f}, 0.6f, 0.35f, 0.001f);
  if (!(camera.nearPlane() <= 0.001f)) {
    std::fprintf(stderr, "inside-scene near plane clips close geometry\n");
    return 1;
  }


  // Room-scale inspection should not retain an unnecessarily microscopic near
  // plane, which destroys separation for nearly-coplanar panels and decals.
  camera.setOrbit(light3d::Vec3{0.0f, 0.0f, 0.0f}, 0.6f, 0.35f, 1.0f);
  if (!(camera.nearPlane() >= 1.0e-4f)) {
    std::fprintf(stderr, "inside-scene near plane wastes depth precision\n");
    return 1;
  }
  camera.setOrbit(light3d::Vec3{0.0f, 0.0f, 0.0f}, 0.6f, 0.35f, 0.001f);

  const float yaw = camera.yaw();
  const float pitch = camera.pitch();
  const float distance = camera.distance();
  const light3d::Vec3 oldEye = camera.eye();
  const light3d::Vec3 oldTarget = camera.target();
  const light3d::Vec3 oldBack = light3d::normalize(oldEye - camera.target());
  camera.dolly(11.0f);
  const light3d::Vec3 newEye = camera.eye();
  const light3d::Vec3 eyeDelta = newEye - oldEye;
  const light3d::Vec3 targetDelta = camera.target() - oldTarget;
  if (!(light3d::dot(newEye, oldBack) < 0.0f) ||
      !(light3d::dot(camera.target(), oldBack) <
        light3d::dot(newEye, oldBack)) ||
      light3d::length(eyeDelta - targetDelta) > 1e-5f ||
      !Near(camera.distance(), distance) || !Near(camera.yaw(), yaw) ||
      !Near(camera.pitch(), pitch)) {
    std::fprintf(stderr, "dolly did not cross the pivot continuously\n");
    return 1;
  }

  return 0;
}
