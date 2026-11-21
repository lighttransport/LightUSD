// SPDX-License-Identifier: Apache-2.0
#include "light3d/camera-nav.hh"

#include <algorithm>
#include <cmath>

namespace light3d {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kPitchLimit = 89.0f * kDeg2Rad;

float Clamp(float v, float lo, float hi) {
  return std::max(lo, std::min(hi, v));
}

light3d::Vec3 DirFromAngles(float yaw, float pitch, int upAxis) {
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);
  const float sy = std::sin(yaw);
  const float cy = std::cos(yaw);
  // Azimuth rotates in the ground plane; elevation lifts toward the up axis.
  if (upAxis == 2) {  // +Z up: ground = XY plane
    return light3d::Vec3{sy * cp, cy * cp, sp};
  }
  return light3d::Vec3{sy * cp, sp, cy * cp};  // +Y up: ground = XZ plane
}

light3d::Vec3 RotateAroundAxis(light3d::Vec3 v, light3d::Vec3 axis,
                               float angle) {
  axis = light3d::normalize(axis);
  if (light3d::length(axis) < 1e-6f || !std::isfinite(angle)) return v;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return v * c + light3d::cross(axis, v) * s +
         axis * (light3d::dot(axis, v) * (1.0f - c));
}

void AnglesFromDir(const light3d::Vec3& dir, int upAxis, float* yaw,
                   float* pitch) {
  if (!yaw || !pitch) return;
  const light3d::Vec3 n = light3d::normalize(dir);
  if (light3d::length(n) < 1e-6f) return;
  if (upAxis == 2) {
    *pitch = std::asin(Clamp(n.z, -1.0f, 1.0f));
    *yaw = std::atan2(n.x, n.y);
  } else {
    *pitch = std::asin(Clamp(n.y, -1.0f, 1.0f));
    *yaw = std::atan2(n.x, n.z);
  }
  *pitch = Clamp(*pitch, -kPitchLimit, kPitchLimit);
}
}  // namespace

light3d::Vec3 OrbitCamera::eye() const {
  return target_ + (havePose_ ? poseBackward_ : DirFromAngles(yaw_, pitch_, upAxis_)) * distance_;
}

light3d::Mat4 OrbitCamera::view() const {
  return light3d::lookAt(eye(), target_, havePose_ ? poseUp_ : worldUp());
}

light3d::Mat4 OrbitCamera::proj(bool zeroToOneDepth) const {
  const float fovY = fovYDeg_ * kDeg2Rad;
  const float znear = nearPlane();
  const float zfar = farPlane();
  float projAspect = aspect();
  float verticalScale = 1.0f;
  if (aspectOverrideEnabled_ && conform_ != CameraConform::None) {
    const float filmbackAspect = aspectOverride_;
    projAspect = viewportAspect_;
    const bool preserveHorizontal =
        conform_ == CameraConform::Horizontal ||
        (conform_ == CameraConform::Fit && projAspect < filmbackAspect) ||
        (conform_ == CameraConform::Crop && projAspect > filmbackAspect);
    if (preserveHorizontal) verticalScale = filmbackAspect / projAspect;
  }
  if (projection_ == CameraProjection::Orthographic) {
    const float halfH =
        std::max(orthographicHeight_ * 0.5f * verticalScale, 1e-5f);
    const float halfW = halfH * projAspect;
    light3d::Mat4 r;
    r.m[0] = 1.0f / halfW;
    r.m[5] = 1.0f / halfH;
    r.m[12] = -lensShiftX_;
    r.m[13] = -lensShiftY_;
    if (zeroToOneDepth) {
      r.m[10] = -1.0f / (zfar - znear);
      r.m[14] = -znear / (zfar - znear);
    } else {
      r.m[10] = -2.0f / (zfar - znear);
      r.m[14] = -(zfar + znear) / (zfar - znear);
    }
    r.m[15] = 1.0f;
    return r;
  }
  const float conformedFovY =
      2.0f * std::atan(std::tan(0.5f * fovY) * verticalScale);
  light3d::Mat4 r =
      zeroToOneDepth
          ? light3d::perspectiveZeroOne(conformedFovY, projAspect, znear, zfar)
          : light3d::perspective(conformedFovY, projAspect, znear, zfar);
  r.m[8] += lensShiftX_;
  r.m[9] += lensShiftY_;
  return r;
}

float OrbitCamera::nearPlane() const {
  if (!autoClip_) return nearClip_;
  // Orbit distance measures the camera to its navigation pivot, not the nearest
  // visible surface. In a large building the pivot can remain hundreds of units
  // behind a wall while the camera is centimeters from it; using 1% of that
  // distance sliced away the wall and produced an apparent section plane.
  // Use the scene bounding sphere when available. Inside/near the sphere we need
  // an inspection-scale near plane; outside it, the empty distance before the
  // nearest possible surface can safely move the plane out and recover depth
  // precision for hidden-line wireframe rendering.
  const float d = std::abs(distance_);
  const float r = sceneRadius_ > 1e-4f ? sceneRadius_ : 1.0f;
  // Preserve useful D32 separation for nearly-coplanar architectural panels,
  // labels and decals. Scale with the scene and inspection distance, but keep a
  // genuinely close plane when the camera itself is at sub-unit distance.
  const float inspectionNear = std::max(
      1e-4f, std::min({r * 1e-4f, std::max(d * 1e-4f, 1e-4f), 0.1f}));
  if (haveSceneBounds_) {
    const float outside = light3d::length(eye() - sceneCenter_) - r;
    if (outside > 0.0f) return std::max(inspectionNear, outside * 0.05f);
    return inspectionNear;
  }
  return std::max(d * 0.001f, inspectionNear);
}

float OrbitCamera::farPlane() const {
  if (!autoClip_) return farClip_;
  const float r = sceneRadius_ > 1e-4f ? sceneRadius_ : 1.0f;
  float distant;
  if (haveSceneBounds_) {
    distant = (light3d::length(eye() - sceneCenter_) + r) * 1.05f;
  } else {
    distant = std::max(distance_ + r * 3.0f, distance_ * 2.0f);
  }
  // The ground grid and world axes are drawn around the WORLD ORIGIN and are
  // deliberately padded out beyond the subject's own bounds (see the helper
  // extent in Gui::renderViewportScene), so on a small subject they reach well
  // past the scene bounding sphere -- a far plane derived from that sphere
  // alone sliced the grid off a few rows out from the subject instead of
  // letting it run to a horizon. Extend to cover them when they are shown.
  // Left at 0 (no effect, so the tight bounds-aware range is preserved for
  // depth precision) when there is nothing extra to reach.
  if (helperRadius_ > 0.0f) {
    distant = std::max(distant, (light3d::length(eye()) + helperRadius_) * 1.05f);
  }
  return std::max(distant, nearPlane() * 2.0f);
}

void OrbitCamera::setFovYDeg(float deg) {
  if (!std::isfinite(deg)) return;
  fovYDeg_ = std::max(5.0f, std::min(175.0f, deg));
}

void OrbitCamera::setOrthographicHeight(float height) {
  if (!std::isfinite(height)) return;
  orthographicHeight_ = std::max(height, 1e-5f);
}

void OrbitCamera::setLensShift(float horizontal, float vertical) {
  if (!std::isfinite(horizontal) || !std::isfinite(vertical)) return;
  lensShiftX_ = Clamp(horizontal, -10.0f, 10.0f);
  lensShiftY_ = Clamp(vertical, -10.0f, 10.0f);
}

void OrbitCamera::setAspectOverride(float aspectValue) {
  if (!std::isfinite(aspectValue)) return;
  aspectOverride_ = std::max(0.05f, std::min(100.0f, aspectValue));
}

void OrbitCamera::setClipPlanes(float nearPlaneValue, float farPlaneValue) {
  if (!std::isfinite(nearPlaneValue) || !std::isfinite(farPlaneValue)) return;
  nearClip_ = std::max(1e-5f, nearPlaneValue);
  farClip_ = std::max(nearClip_ + 1e-4f, farPlaneValue);
}

void OrbitCamera::orbit(float dxPix, float dyPix) {
  // Maya-like tumble rate: roughly 0.18 degrees per pixel at the default
  // sensitivity, so a full-viewport drag turns about half a revolution instead
  // of whipping around after a short gesture. navigationScale_ and the
  // user-facing sensitivity slider remain relative multipliers on top.
  constexpr float kOrbitRadiansPerPixel = 0.0032f;  // ~0.183 deg/px
  const float k =
      kOrbitRadiansPerPixel * navigationScale_ * orbitSensitivity_;
  if (!std::isfinite(dxPix) || !std::isfinite(dyPix)) return;

  if (havePose_) {
    const float yawAngle = (invertYaw_ ? -dxPix : dxPix) * k;
    poseBackward_ = RotateAroundAxis(poseBackward_, worldUp(), yawAngle);
    poseUp_ = RotateAroundAxis(poseUp_, worldUp(), yawAngle);
    const auto right = light3d::normalize(light3d::cross(poseUp_, poseBackward_));
    poseBackward_ = light3d::normalize(RotateAroundAxis(poseBackward_, right, -dyPix * k));
    poseUp_ = light3d::normalize(RotateAroundAxis(poseUp_, right, -dyPix * k));
    return;
  }
  light3d::Vec3 viewFromTarget = eye() - target_;
  if (light3d::length(viewFromTarget) < 1e-6f) {
    viewFromTarget = DirFromAngles(yaw_, pitch_, upAxis_) * distance_;
  }

  // Maya-style tumble: horizontal motion yaws around the scene up axis; vertical
  // motion pitches around the current camera-right axis. This keeps the pivot
  // fixed and makes the drag operate in the current view basis instead of just
  // accumulating Euler components.
  const light3d::Vec3 up = worldUp();
  const float yawPixels = invertYaw_ ? -dxPix : dxPix;
  viewFromTarget = RotateAroundAxis(viewFromTarget, up, yawPixels * k);

  const light3d::Vec3 fwd = light3d::normalize(-viewFromTarget);
  light3d::Vec3 right = light3d::normalize(light3d::cross(fwd, up));
  if (light3d::length(right) < 1e-6f) {
    right = light3d::Vec3{1, 0, 0};
  }
  viewFromTarget = RotateAroundAxis(viewFromTarget, right, -dyPix * k);

  AnglesFromDir(viewFromTarget, upAxis_, &yaw_, &pitch_);
}

// Reference distance for pan/dolly speed. Pan is proportional to distance so a
// drag moves a consistent screen fraction -- but on a large scene the framed
// distance is huge (pan feels hyper-fast), and after dollying in close it goes
// near zero (pan appears frozen). Clamp the reference into a band around the
// scene radius so the speed stays usable at both ends of the zoom range.
float OrbitCamera::moveRefDistance() const {
  if (!(sceneRadius_ > 1e-4f)) return distance_;
  // Pan/dolly are distance-proportional so a drag pans a consistent screen
  // fraction at any zoom. Clamp only the extremes relative to the scene: a tiny
  // floor avoids freezing at extreme zoom-in, and a ceiling near one scene
  // diameter keeps a framed (zoomed-way-out) drag from flinging across a huge,
  // far-from-origin scene like the Caldera districts.
  const float lo = sceneRadius_ * 0.001f;
  const float hi = sceneRadius_ * 1.5f;
  return std::max(lo, std::min(distance_, hi));
}

void OrbitCamera::pan(float dxPix, float dyPix) {
  // Camera basis (relative to the world up axis). Near the pole the forward axis
  // approaches worldUp; pitch is clamped to <90 so the cross stays well-defined.
  light3d::Vec3 fwd = light3d::normalize(target_ - eye());
  light3d::Vec3 right = light3d::normalize(light3d::cross(fwd, havePose_ ? poseUp_ : worldUp()));
  light3d::Vec3 up = light3d::cross(right, fwd);
  const float scale = moveRefDistance() * 0.0015f * 0.65f * navigationScale_ *
                      panSensitivity_;
  target_ = target_ - right * (dxPix * scale) + up * (dyPix * scale);
}

void OrbitCamera::moveForward(float amount) {
  if (!std::isfinite(amount) || amount == 0.0f) return;
  // Symmetric exponential response retains fine control for wheel notches and
  // bounds a large mouse delta. Keep enough headroom to move the navigation
  // pivot through the eye: a strict one-reference-distance cap asymptotically
  // approached the pivot but could never cross it.
  // One wheel notch (amount 1) moves about 8% of the reference distance; a
  // right-drag of ~100 px is scaled to roughly a fifth of the distance. The
  // exponential keeps fine control for small gestures and bounds large ones.
  const float magnitude =
      1.25f *
      (1.0f - std::exp(-std::abs(amount) * 0.065f *
                         navigationScale_ * dollySensitivity_));
  const float signedMagnitude = amount > 0.0f ? magnitude : -magnitude;
  const light3d::Vec3 forward = light3d::normalize(target_ - eye());
  target_ = target_ + forward * (moveRefDistance() * signedMagnitude);
}

void OrbitCamera::dolly(float amount) {
  moveForward(invertDolly_ ? -amount : amount);
}

void OrbitCamera::setPreset(CameraViewPreset preset) {
  havePose_ = false;
  switch (preset) {
    case CameraViewPreset::Isometric:
      yaw_ = 0.6f;
      pitch_ = 0.35f;
      break;
    case CameraViewPreset::Front:
      yaw_ = 0.0f;
      pitch_ = 0.0f;
      break;
    case CameraViewPreset::Back:
      yaw_ = kPi;
      pitch_ = 0.0f;
      break;
    case CameraViewPreset::Right:
      yaw_ = 0.5f * kPi;
      pitch_ = 0.0f;
      break;
    case CameraViewPreset::Left:
      yaw_ = -0.5f * kPi;
      pitch_ = 0.0f;
      break;
    case CameraViewPreset::Top:
      yaw_ = 0.0f;
      pitch_ = kPitchLimit;
      break;
    case CameraViewPreset::Bottom:
      yaw_ = 0.0f;
      pitch_ = -kPitchLimit;
      break;
  }
}

void OrbitCamera::fitToScene(const float aabbMin[3], const float aabbMax[3]) {
  setSceneBounds(aabbMin, aabbMax);
  light3d::Vec3 mn{aabbMin[0], aabbMin[1], aabbMin[2]};
  light3d::Vec3 mx{aabbMax[0], aabbMax[1], aabbMax[2]};
  light3d::Vec3 center = (mn + mx) * 0.5f;
  float radius = 0.5f * light3d::length(mx - mn);
  if (!(radius > 1e-4f)) radius = 1.0f;  // also catches NaN

  target_ = center;
  const float halfV = 0.5f * fovYDeg_ * kDeg2Rad;
  const float halfH = std::atan(std::tan(halfV) * aspect());
  const float halfMin = std::max(1e-3f, std::min(halfV, halfH));
  distance_ = (radius / std::sin(halfMin)) * 1.1f;
  // Near/far are derived dynamically in proj() from the recorded scene bounds.
}

void OrbitCamera::setSceneBounds(const float aabbMin[3],
                                 const float aabbMax[3]) {
  if (!aabbMin || !aabbMax) return;
  const light3d::Vec3 mn{aabbMin[0], aabbMin[1], aabbMin[2]};
  const light3d::Vec3 mx{aabbMax[0], aabbMax[1], aabbMax[2]};
  const float radius = 0.5f * light3d::length(mx - mn);
  if (!(radius > 1e-4f) || !std::isfinite(radius)) return;
  sceneCenter_ = (mn + mx) * 0.5f;
  sceneRadius_ = radius;
  haveSceneBounds_ = true;
}

bool OrbitCamera::setPose(const float transform[16], float pivotDistance) {
  if (!transform || !std::isfinite(pivotDistance) || pivotDistance < 1e-3f) return false;
  for (int i = 0; i < 16; ++i) if (!std::isfinite(transform[i])) return false;
  const light3d::Vec3 right{transform[0], transform[1], transform[2]};
  const light3d::Vec3 up{transform[4], transform[5], transform[6]};
  const light3d::Vec3 back{transform[8], transform[9], transform[10]};
  if (std::abs(light3d::length(right) - 1.0f) > 1e-3f ||
      std::abs(light3d::length(up) - 1.0f) > 1e-3f ||
      std::abs(light3d::length(back) - 1.0f) > 1e-3f ||
      std::abs(light3d::dot(up, back)) > 1e-3f ||
      light3d::dot(light3d::cross(up, back), right) < 0.999f ||
      std::abs(transform[3]) > 1e-6f || std::abs(transform[7]) > 1e-6f ||
      std::abs(transform[11]) > 1e-6f || std::abs(transform[15] - 1.0f) > 1e-6f) return false;
  poseBackward_ = light3d::normalize(back);
  poseUp_ = light3d::normalize(up);
  distance_ = pivotDistance;
  target_ = light3d::Vec3{transform[12], transform[13], transform[14]} - poseBackward_ * distance_;
  havePose_ = true;
  return true;
}

void OrbitCamera::setOrbit(const light3d::Vec3& target, float yawRad, float pitchRad,
                           float dist) {
  havePose_ = false;
  target_ = target;
  yaw_ = yawRad;
  pitch_ = std::max(-kPitchLimit, std::min(kPitchLimit, pitchRad));
  distance_ = std::max(dist, 1e-3f);
}

}  // namespace light3d
