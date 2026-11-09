// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "light-filter.hh"
#include "render-data-enums.hh"
#include "render-data-math.hh"
#include <string>
#include <vector>

namespace lightusd { namespace tydra {

//
// RenderLight
//
struct RenderLight {
  std::string name;
  std::string prim_path;
  LightType type = LightType::Point;

  // Common properties
  Float3 color = {1, 1, 1};
  float intensity = 1.0f;
  float exposure = 0.0f;
  bool normalize = false;
  bool enable_color_temperature = false;
  float color_temperature = 6500.0f;
  float diffuse = 1.0f;
  float specular = 1.0f;
  float shaping_cone_angle = 90.0f;
  float shaping_focus = 0.0f;
  Float3 shaping_focus_tint = {0, 0, 0};  // color3f per UsdLux ShapingAPI
  float shaping_cone_softness = 0.0f;
  std::string shaping_ies_file;
  // Authored texture asset on RectLight. Dome textures use the resolved image
  // table and params.dome.texture_id instead.
  std::string texture_file;
  float shaping_ies_angle_scale = 0.0f;
  bool shaping_ies_normalize = false;
  std::vector<std::string> light_link_targets;
  std::vector<std::string> shadow_link_targets;
  std::vector<std::string> filter_targets;
  // Parsed portable filters (rel light:filters). Receiver-independent kinds are
  // folded into the effective intensity/color at each backend's light setup.
  std::vector<LightFilter> filters;

  // Light/shadow linking resolved to RenderScene mesh indices (CollectionAPI
  // collection:lightLink / collection:shadowLink relationship form). When the
  // collection is unauthored the light links everything and *_links_all stays
  // true. Relationship and membershipExpression collection forms are both
  // resolved by the converter.
  bool light_links_all = true;
  std::vector<int32_t> light_link_mesh_indices;
  bool shadow_links_all = true;
  std::vector<int32_t> shadow_link_mesh_indices;

  // DomeLight inputs:texture:format token (matches UsdLux).
  enum class DomeTextureFormat : uint8_t {
    Automatic = 0,
    Latlong,
    MirroredBall,
    Angular,
  };

  // Transform
  Matrix4 transform;

  // Preserve the SphereLight radius when shaping makes its render type Spot.
  float spot_source_radius = 0.5f;
  float guide_radius = 1.0e5f;

  // Type-specific properties
  union {
    struct { float radius; } sphere;
    struct { float width, height; } rect;
    struct { float radius; } disk;
    struct { float angle; } spot;  // Cone angle in radians
    struct { int32_t texture_id; DomeTextureFormat texture_format; } dome;
    struct { float radius, length; } cylinder;
    struct { float angle; } distant;  // Angular size in degrees
  } params = {};

  // Shadow
  bool enable_shadow = true;
  Float3 shadow_color = {0, 0, 0};
  float shadow_distance = -1.0f;
  float shadow_falloff = -1.0f;
  float shadow_falloff_gamma = 1.0f;
};

//
// RenderCamera
//
struct RenderBackPlate {
  std::string instance_name;
  std::string image;
  std::string alpha_image;
  std::string depth_image;
  float depth_min_offset = 0.0f;
  float depth_normalizing_factor = 1.0f;
  float depth_camera_space_offset = 0.0f;
  Float2 scale_tweak = {1.0f, 1.0f};
  Float3 rotate_xyz_tweak = {0.0f, 0.0f, 0.0f};
  Float3 translate_tweak = {0.0f, 0.0f, 0.0f};
  Float3 luma_gain = {1.0f, 1.0f, 1.0f};
  Float3 luma_lift = {0.0f, 0.0f, 0.0f};
  Float3 luma_gamma = {1.0f, 1.0f, 1.0f};
  std::string plate_visibility = "solo";
};

struct RenderCamera {
  std::string name;
  std::string prim_path;
  CameraType type = CameraType::Perspective;

  // Transform
  Matrix4 transform;

  // Perspective params
  float focal_length = 50.0f;        // mm
  float horizontal_aperture = 36.0f; // mm
  float vertical_aperture = 24.0f;   // mm
  float horizontal_aperture_offset = 0.0f;
  float vertical_aperture_offset = 0.0f;

  // Orthographic params
  float ortho_width = 10.0f;

  // Clipping
  float near_clip = 0.1f;
  float far_clip = 10000.0f;

  // Depth of field / exposure
  float focus_distance = 5.0f;
  float fstop = 0.0f;           // 0 = no DoF
  float exposure = 0.0f;

  enum class StereoRole : uint8_t { Mono = 0, Left, Right };
  StereoRole stereo_role = StereoRole::Mono;
  std::vector<Float4> clipping_planes;
  std::vector<RenderBackPlate> back_plates;

  // Motion-blur shutter interval (UsdGeomCamera shutter:open/close), in
  // time-code offsets relative to the sample time.
  double shutter_open = 0.0;
  double shutter_close = 0.0;

  // Computed FOV
  float fov_y() const;
  float fov_x() const;
  float aspect_ratio() const;
};

//
// Scene Node (transform hierarchy)
//

}} // namespace lightusd::tydra
