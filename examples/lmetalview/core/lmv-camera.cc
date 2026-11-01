// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - C wrapper over the orbit camera shared with lusdview
// (examples/common/light3d/camera-nav.hh). Keeping one implementation means the
// viewer navigates, frames and clips exactly like lusdview.

#include "lmv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "light3d/camera-nav.hh"

namespace {
constexpr float kPi = 3.14159265358979323846f;

light3d::Vec3 ToVec3(const float v[3]) {
  return light3d::Vec3{v[0], v[1], v[2]};
}
}  // namespace

struct lmv_camera {
  light3d::OrbitCamera camera;
};

extern "C" {

lmv_camera* lmv_camera_create(void) { return new (std::nothrow) lmv_camera(); }

void lmv_camera_destroy(lmv_camera* camera) { delete camera; }

void lmv_camera_set_aspect(lmv_camera* camera, float aspect) {
  if (camera) camera->camera.setAspect(aspect);
}

void lmv_camera_set_up_axis(lmv_camera* camera, int up_axis) {
  if (!camera) return;
  camera->camera.setUpAxis(up_axis == 1 ? 2 : 1);
}

void lmv_camera_set_fov_y(lmv_camera* camera, float degrees) {
  if (camera) camera->camera.setFovYDeg(degrees);
}

void lmv_camera_set_exposure(lmv_camera* camera, float stops) {
  if (camera) camera->camera.setExposure(stops);
}

void lmv_camera_set_orthographic(lmv_camera* camera, int on) {
  if (!camera) return;
  camera->camera.setProjection(on ? light3d::CameraProjection::Orthographic
                                  : light3d::CameraProjection::Perspective);
}

void lmv_camera_set_orthographic_height(lmv_camera* camera, float height) {
  if (camera) camera->camera.setOrthographicHeight(height);
}

void lmv_camera_set_preset(lmv_camera* camera, int preset) {
  if (!camera) return;
  switch (preset) {
    case 0: camera->camera.setPreset(light3d::CameraViewPreset::Isometric); break;
    case 1: camera->camera.setPreset(light3d::CameraViewPreset::Front); break;
    case 2: camera->camera.setPreset(light3d::CameraViewPreset::Back); break;
    case 3: camera->camera.setPreset(light3d::CameraViewPreset::Right); break;
    case 4: camera->camera.setPreset(light3d::CameraViewPreset::Left); break;
    case 5: camera->camera.setPreset(light3d::CameraViewPreset::Top); break;
    case 6: camera->camera.setPreset(light3d::CameraViewPreset::Bottom); break;
    default: break;
  }
}

void lmv_camera_frame_bounds(lmv_camera* camera, const float bbox_min[3],
                             const float bbox_max[3]) {
  if (!camera || !bbox_min || !bbox_max) return;
  camera->camera.fitToScene(bbox_min, bbox_max);
}

void lmv_camera_orbit(lmv_camera* camera, float dx, float dy) {
  if (camera) camera->camera.orbit(dx, dy);
}

void lmv_camera_pan(lmv_camera* camera, float dx, float dy) {
  if (camera) camera->camera.pan(dx, dy);
}

void lmv_camera_dolly(lmv_camera* camera, float amount) {
  if (camera) camera->camera.dolly(amount);
}

void lmv_camera_set_orbit(lmv_camera* camera, const float target[3], float yaw,
                          float pitch, float distance) {
  if (!camera || !target) return;
  camera->camera.setOrbit(ToVec3(target), yaw, pitch, distance);
}

void lmv_camera_get_orbit(const lmv_camera* camera, float out_target[3],
                          float* yaw, float* pitch, float* distance) {
  if (!camera) return;
  if (out_target) {
    const light3d::Vec3& target = camera->camera.target();
    out_target[0] = target.x;
    out_target[1] = target.y;
    out_target[2] = target.z;
  }
  if (yaw) *yaw = camera->camera.yaw();
  if (pitch) *pitch = camera->camera.pitch();
  if (distance) *distance = camera->camera.distance();
}

void lmv_camera_set_from_camera(lmv_camera* camera,
                                const lmv_camera_view* authored) {
  if (!camera || !authored || !authored->transform) return;
  const float* t = authored->transform;  // column-major
  const light3d::Vec3 eye{t[12], t[13], t[14]};
  // USD cameras look down local -Z.
  light3d::Vec3 forward{-t[8], -t[9], -t[10]};
  const float flen = light3d::length(forward);
  if (flen < 1e-6f) forward = light3d::Vec3{0.0f, 0.0f, -1.0f};
  else forward = forward * (1.0f / flen);

  const float distance =
      std::max(authored->focus_distance > 0.0f ? authored->focus_distance : 5.0f,
               1e-3f);
  const light3d::Vec3 target = eye + forward * distance;
  const light3d::Vec3 dir = eye - target;  // target -> eye
  const int up_axis = camera->camera.upAxis();
  const float len = light3d::length(dir);
  float yaw = 0.0f;
  float pitch = 0.0f;
  if (len > 1e-6f) {
    const light3d::Vec3 n = dir * (1.0f / len);
    if (up_axis == 2) {
      pitch = std::asin(std::max(-1.0f, std::min(1.0f, n.z)));
      yaw = std::atan2(n.x, n.y);
    } else {
      pitch = std::asin(std::max(-1.0f, std::min(1.0f, n.y)));
      yaw = std::atan2(n.x, n.z);
    }
  }
  camera->camera.setOrbit(target, yaw, pitch, distance);

  if (authored->orthographic) {
    camera->camera.setProjection(light3d::CameraProjection::Orthographic);
    camera->camera.setOrthographicHeight(authored->ortho_width);
  } else {
    camera->camera.setProjection(light3d::CameraProjection::Perspective);
    const float focal = authored->focal_length > 0.0f ? authored->focal_length
                                                      : 50.0f;
    const float aperture = authored->vertical_aperture > 0.0f
                               ? authored->vertical_aperture
                               : 24.0f;
    const float fov_y = 2.0f * std::atan(0.5f * aperture / focal);
    camera->camera.setFovYDeg(fov_y * 180.0f / kPi);
  }
  camera->camera.setClipPlanes(authored->near_clip, authored->far_clip);
  camera->camera.setExposure(authored->exposure);
}

void lmv_camera_get_frame(const lmv_camera* camera, lmv_camera_frame* out) {
  if (!out) return;
  if (!camera) {
    std::memset(out, 0, sizeof(*out));
    return;
  }
  const light3d::Mat4 view = camera->camera.view();
  const light3d::Mat4 proj = camera->camera.proj(/*zeroToOneDepth=*/true);
  std::memcpy(out->view, view.m, sizeof(out->view));
  std::memcpy(out->proj, proj.m, sizeof(out->proj));
  const light3d::Vec3 eye = camera->camera.eye();
  const light3d::Vec3 target = camera->camera.target();
  out->eye[0] = eye.x;
  out->eye[1] = eye.y;
  out->eye[2] = eye.z;
  out->target[0] = target.x;
  out->target[1] = target.y;
  out->target[2] = target.z;
  out->near_clip = camera->camera.nearPlane();
  out->far_clip = camera->camera.farPlane();
  out->exposure = camera->camera.exposure();
  out->fov_y = camera->camera.fovYDeg() * kPi / 180.0f;
  out->orthographic =
      camera->camera.projection() == light3d::CameraProjection::Orthographic
          ? 1
          : 0;
  out->_pad = 0;
}

}  // extern "C"
