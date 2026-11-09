// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-enums.hh"
#include "render-data-math.hh"
#include "chunked-array.hh"
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

//
// Vertex Attribute



struct SceneNode {
  std::string name;
  std::string prim_path;
  std::string prototype_path;
  NodeType type = NodeType::Xform;
  bool has_reset_xform = false;
  bool is_instance = false;

  // Transform
  Matrix4 local_transform;
  Matrix4 world_transform;

  // Reference to data (based on type)
  int32_t data_id = -1;  // Index into appropriate array

  // Hierarchy
  int32_t parent_id = -1;
  std::vector<int32_t> children;

  // Visibility
  bool visible = true;
};

//
// Animation Keyframe
//
struct Keyframe {
  double time;
  Float4 value;  // Supports up to vec4
};

//
// Animation Channel
//
struct AnimationChannel {
  enum class TargetPath : uint8_t {
    Translation = 0,
    Rotation,
    Scale,
    Weights,  // Blend shape weights
    CustomProperty
  };

  TargetPath target_path = TargetPath::Translation;
  int32_t target_node = -1;
  int32_t target_skeleton = -1;
  std::string target_prim_path;
  std::string property_name;
  std::string target_skeleton_path;
  std::vector<std::string> joint_order;
  std::vector<std::string> blend_shape_order;
  // Maps each SkelAnimation joint_order element to a Skeleton::joints index.
  // -1 means the animation joint could not be resolved on the target skeleton.
  std::vector<int32_t> joint_remap;

  // Keyframes (sorted by time)
  std::vector<Keyframe> keyframes;

  // Optional full array payload for UsdSkelAnimation channels. For frame i,
  // slice array_values at:
  //   i * element_count * value_stride
  // with element_count values, each value_stride floats wide.
  // Existing scalar keyframes keep a first-element preview for compatibility.
  std::vector<float> array_values;
  uint32_t element_count = 1;
  uint32_t value_stride = 4;
  bool is_skeletal = false;

  // Interpolation
  enum class Interpolation : uint8_t {
    Step = 0,
    Linear,
    CubicSpline
  };
  Interpolation interpolation = Interpolation::Linear;
};

//
// Animation Clip
//
struct AnimationClip {
  std::string name;
  std::string prim_path;

  double start_time = 0.0;
  double end_time = 0.0;

  std::vector<AnimationChannel> channels;

  // Source value-clip metadata retained after baking for diagnostics and web
  // feature-parity reporting.
  std::vector<std::string> clip_asset_paths;
  bool value_clip_baked = false;
};

//
// Skeleton Joint
//
struct SkeletonJoint {
  std::string name;
  std::string path;
  int32_t parent_id = -1;

  Matrix4d bind_transform;
  Matrix4d rest_transform;

  std::vector<int32_t> children;
};

//
// Skeleton
//
struct Skeleton {
  std::string name;
  std::string display_name;
  std::string prim_path;

  std::vector<SkeletonJoint> joints;
  int32_t root_joint = 0;

  // Animation reference
  int32_t animation_id = -1;
  std::string animation_source_path;
};

//
// USD Physics annotations extracted from next::Stage.
// These are descriptive data for render/tool consumers; tydra-next does not
// simulate physics.
//
struct PhysicsProperty {
  std::string name;
  std::string value;
};

struct PhysicsSceneAnnotation {
  std::string prim_path;
  Float3 gravity_direction{0.0f, -1.0f, 0.0f};
  float gravity_magnitude = 9.81f;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsRigidBodyAnnotation {
  std::string prim_path;
  bool rigid_body_enabled = true;
  bool kinematic_enabled = false;
  std::string simulation_owner;
  Float3 velocity{0.0f, 0.0f, 0.0f};
  Float3 angular_velocity{0.0f, 0.0f, 0.0f};
  bool starts_asleep = false;
  bool has_mass = false;
  float mass = 0.0f;
  float density = 0.0f;
  Float3 center_of_mass{0.0f, 0.0f, 0.0f};
  Float3 diagonal_inertia{0.0f, 0.0f, 0.0f};
  Float4 principal_axes{0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsColliderAnnotation {
  std::string prim_path;
  bool collision_enabled = true;
  std::string simulation_owner;
  bool has_mesh_collision = false;
  std::string approximation = "none";
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsJointAnnotation {
  std::string prim_path;
  std::string type_name;
  std::string body0;
  std::string body1;
  bool has_body0 = false;
  bool has_body1 = false;
  Float3 local_pos0{0.0f, 0.0f, 0.0f};
  Float3 local_pos1{0.0f, 0.0f, 0.0f};
  Float4 local_rot0{1.0f, 0.0f, 0.0f, 0.0f};
  Float4 local_rot1{1.0f, 0.0f, 0.0f, 0.0f};
  Float3 axis{1.0f, 0.0f, 0.0f};
  float lower_limit = 0.0f;
  float upper_limit = 0.0f;
  float cone_angle0_limit = -1.0f;
  float cone_angle1_limit = -1.0f;
  float min_distance = -1.0f;
  float max_distance = -1.0f;
  bool collision_enabled = false;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsMaterialAnnotation {
  std::string prim_path;
  float static_friction = 0.0f;
  float dynamic_friction = 0.0f;
  float restitution = 0.0f;
  float density = 0.0f;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsFilteredPairsAnnotation {
  std::string prim_path;
  std::vector<std::string> filtered_pair_paths;
};

struct PhysicsAnnotations {
  std::vector<PhysicsSceneAnnotation> scenes;
  std::vector<PhysicsRigidBodyAnnotation> rigid_bodies;
  std::vector<PhysicsColliderAnnotation> colliders;
  std::vector<PhysicsJointAnnotation> joints;
  std::vector<PhysicsMaterialAnnotation> materials;
  std::vector<PhysicsFilteredPairsAnnotation> filtered_pairs;
  std::vector<std::string> articulation_roots;
};

struct UnsupportedRenderable {
  std::string prim_path;
  std::string type_name;
  std::string reason;
};

enum class ConversionDisposition : uint8_t {
  PreservedUnsupported,
  Approximated,
  Failed
};

}}} // namespace lightusd::tydra::next
