// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - resolved UsdPhysics object model (UsdPhysicsLoadStageFromPrimRange
// parity for ownership and relationship semantics; no simulation).
#pragma once

#include <string>
#include <vector>

namespace lightusd {
class Stage;

struct ResolvedPhysics {
  struct Scene {
    std::string path;
    float gravity_direction[3] = {0, 0, 0};
    float gravity_magnitude = 0;
  };
  struct Body {
    std::string path;
    bool enabled = true;
    std::vector<std::string> simulation_owners;
    std::vector<std::string> collisions;  // colliders owned by this body
  };
  struct Collider {
    std::string path;
    std::string rigid_body;  // nearest enabled body ancestor-or-self, "" = static
    bool collision_enabled = true;
    std::vector<std::string> simulation_owners;
  };
  struct Joint {
    std::string path, type;
    std::string rel0, rel1;    // authored body0/body1 targets
    std::string body0, body1;  // resolved bodies
    bool joint_enabled = true;
    bool collision_enabled = false;
    bool exclude_from_articulation = false;
  };
  struct CollisionGroup {
    std::string path;  // representative of a merge set
    std::vector<std::string> filtered_groups;
    std::vector<std::string> merged_groups;
    bool invert_filtered_groups = false;
    std::string merge_group;
  };
  std::vector<Scene> scenes;
  std::vector<Body> bodies;
  std::vector<Collider> colliders;
  std::vector<Joint> joints;
  std::vector<CollisionGroup> collision_groups;
};

/// Resolve physics objects on the composed stage:
///  - scene gravity: authored nonzero direction (normalized) else -upAxis;
///    authored magnitude >= 0 else 9.81 / metersPerUnit (any negative value
///    requests earth gravity, per the schema doc);
///  - colliders belong to the nearest *enabled* RigidBodyAPI ancestor-or-self
///    (disabled bodies are skipped), else they are static;
///  - joint bodies: walking up from the target, the first enabled rigid body,
///    else the highest CollisionAPI ancestor-or-self, else "";
///  - collision groups sharing a mergeGroup collapse onto the first group in
///    traversal order, concatenating filteredGroups; the representative keeps
///    its own invertFilteredGroups (unauthored = false).
ResolvedPhysics ResolvePhysics(const Stage& stage);

}  // namespace lightusd
