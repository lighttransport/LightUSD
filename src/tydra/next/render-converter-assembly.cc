// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "render-converter-assembly.hh"
#include <limits>
namespace lightusd { namespace tydra { namespace next {
void AssignNodeDataId(RenderScene* scene,
                      const std::string& prim_path,
                      int32_t data_id) {
  if (!scene) return;
  const auto node_it = scene->node_by_path.find(prim_path);
  if (node_it == scene->node_by_path.end()) return;
  const int32_t node_id = node_it->second;
  if (node_id < 0 || static_cast<size_t>(node_id) >= scene->nodes.size()) return;
  scene->nodes[static_cast<size_t>(node_id)].data_id = data_id;
}

namespace {
std::string LeafNameFromJointPath(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return "";
  return path.substr(pos + 1);
}

bool JointTokenMatches(const SkeletonJoint& joint, const std::string& token) {
  if (token.empty()) return false;
  if (joint.path == token || joint.name == token) return true;
  if (LeafNameFromJointPath(joint.path) == token) return true;
  if (joint.path.size() > token.size() &&
      joint.path.compare(joint.path.size() - token.size(), token.size(),
                         token) == 0) {
    const size_t sep = joint.path.size() - token.size();
    return sep == 0 || joint.path[sep - 1] == '/';
  }
  return false;
}

using JointIndexLookup = std::unordered_map<std::string, int32_t>;

JointIndexLookup BuildJointIndexLookup(const Skeleton& skeleton) {
  JointIndexLookup lookup;
  lookup.reserve(skeleton.joints.size() * 3);
  for (size_t i = 0; i < skeleton.joints.size(); ++i) {
    const SkeletonJoint& joint = skeleton.joints[i];
    const int32_t index = static_cast<int32_t>(i);
    lookup.emplace(joint.path, index);
    lookup.emplace(joint.name, index);
    lookup.emplace(LeafNameFromJointPath(joint.path), index);
  }
  return lookup;
}

int32_t FindJointIndex(const Skeleton& skeleton,
                       const JointIndexLookup& lookup,
                       const std::string& token) {
  const auto exact = lookup.find(token);
  if (exact != lookup.end()) return exact->second;
  // Preserve support for relative multi-segment tokens. This uncommon path
  // keeps the original matching semantics without penalizing exact paths and
  // leaf names used by ordinary UsdSkel exports.
  for (size_t i = 0; i < skeleton.joints.size(); ++i) {
    if (JointTokenMatches(skeleton.joints[i], token)) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}
}  // namespace

void ResolveSkeletalAnimationTargets(RenderScene* scene) {
  if (!scene) return;
  std::vector<JointIndexLookup> joint_lookups;
  joint_lookups.reserve(scene->skeletons.size());
  for (const Skeleton& skeleton : scene->skeletons) {
    joint_lookups.push_back(BuildJointIndexLookup(skeleton));
  }
  for (size_t ai = 0; ai < scene->animations.size(); ++ai) {
    AnimationClip& clip = scene->animations[ai];
    for (AnimationChannel& channel : clip.channels) {
      if (!channel.is_skeletal) continue;

      int32_t skeleton_id = -1;
      for (size_t si = 0; si < scene->skeletons.size(); ++si) {
        const Skeleton& skel = scene->skeletons[si];
        if (!skel.animation_source_path.empty() &&
            skel.animation_source_path == clip.prim_path) {
          skeleton_id = static_cast<int32_t>(si);
          break;
        }
      }
      if (skeleton_id < 0 && !channel.joint_order.empty()) {
        size_t best_matches = 0;
        for (size_t si = 0; si < scene->skeletons.size(); ++si) {
          const Skeleton& skel = scene->skeletons[si];
          size_t matches = 0;
          for (const std::string& token : channel.joint_order) {
            if (FindJointIndex(skel, joint_lookups[si], token) >= 0) ++matches;
          }
          if (matches > best_matches) {
            best_matches = matches;
            skeleton_id = static_cast<int32_t>(si);
          }
        }
      }

      channel.target_skeleton = skeleton_id;
      channel.joint_remap.clear();
      if (skeleton_id < 0 ||
          static_cast<size_t>(skeleton_id) >= scene->skeletons.size()) {
        continue;
      }
      const Skeleton& skel = scene->skeletons[static_cast<size_t>(skeleton_id)];
      const JointIndexLookup& lookup =
          joint_lookups[static_cast<size_t>(skeleton_id)];
      channel.target_skeleton_path = skel.prim_path;
      channel.joint_remap.reserve(channel.joint_order.size());
      for (const std::string& token : channel.joint_order) {
        channel.joint_remap.push_back(FindJointIndex(skel, lookup, token));
      }
      scene->skeletons[static_cast<size_t>(skeleton_id)].animation_id =
          static_cast<int32_t>(ai);
    }
  }
}

namespace {
bool WouldOverflowSizeMul(size_t a, size_t b) { return a != 0 && b > std::numeric_limits<size_t>::max() / a; }
Matrix4 MulMatrix4(const Matrix4& a, const Matrix4& b) {
  Matrix4 r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      r.m[i * 4 + j] =
          a.m[i * 4 + 0] * b.m[0 * 4 + j] +
          a.m[i * 4 + 1] * b.m[1 * 4 + j] +
          a.m[i * 4 + 2] * b.m[2 * 4 + j] +
          a.m[i * 4 + 3] * b.m[3 * 4 + j];
    }
  }
  return r;
}

void CollectMeshIdsUnderNode(const RenderScene& scene,
                             int32_t node_id,
                             const Matrix4& parent_relative,
                             std::vector<int32_t>* out_ids,
                             std::vector<Matrix4>* out_transforms) {
  if (!out_ids || !out_transforms || node_id < 0 ||
      static_cast<size_t>(node_id) >= scene.nodes.size()) {
    return;
  }

  const SceneNode& node = scene.nodes[static_cast<size_t>(node_id)];
  const Matrix4 relative = MulMatrix4(node.local_transform, parent_relative);
  if (node.type == NodeType::Mesh && node.data_id >= 0) {
    out_ids->push_back(node.data_id);
    out_transforms->push_back(relative);
  }

  for (int32_t child_id : node.children) {
    CollectMeshIdsUnderNode(scene, child_id, relative, out_ids, out_transforms);
  }
}

void ResolvePointInstancerPrototypeBindingsImpl(RenderScene* scene,
                                                RenderPointInstancer* instancer) {
  if (!scene || !instancer) return;

  instancer->prototype_node_ids.clear();
  instancer->prototype_mesh_offsets.clear();
  instancer->prototype_mesh_ids.clear();
  instancer->prototype_mesh_transforms.clear();
  instancer->prototype_node_ids.reserve(instancer->prototype_paths.size());
  instancer->prototype_mesh_offsets.reserve(instancer->prototype_paths.size() + 1);
  instancer->prototype_mesh_offsets.push_back(0);

  for (const std::string& path : instancer->prototype_paths) {
    int32_t node_id = -1;
    const auto node_it = scene->node_by_path.find(path);
    if (node_it != scene->node_by_path.end()) {
      node_id = node_it->second;
    }
    instancer->prototype_node_ids.push_back(node_id);
    CollectMeshIdsUnderNode(*scene, node_id, Matrix4::Identity(),
                            &instancer->prototype_mesh_ids,
                            &instancer->prototype_mesh_transforms);
    instancer->prototype_mesh_offsets.push_back(
        static_cast<uint32_t>(instancer->prototype_mesh_ids.size()));
  }
}

void AppendPointInstanceDrawsImpl(int32_t instancer_id,
                                  RenderPointInstancer* instancer,
                                  RenderScene* scene) {
  if (!scene || !instancer || instancer_id < 0) return;
  instancer->draw_start = static_cast<uint32_t>(scene->point_instance_draws.size());
  instancer->draw_count = 0;
  if (!instancer->valid) return;

  const size_t instance_count = instancer->instance_count();

  // Reserve up front: a RenderPointInstanceDraw is ~84 bytes (mostly a
  // Matrix4), so 1M instances grow ~84 MB by geometric doubling -- a ~1.5x
  // transient spike plus ~20 full reallocation memcpys. The upper bound is
  // instances x the widest prototype mesh run.
  {
    size_t max_run = 0;
    for (size_t i = 0; i + 1 < instancer->prototype_mesh_offsets.size(); ++i) {
      const size_t run = instancer->prototype_mesh_offsets[i + 1] -
                         instancer->prototype_mesh_offsets[i];
      if (run > max_run) max_run = run;
    }
    if (max_run > 0 && !WouldOverflowSizeMul(instance_count, max_run)) {
      const size_t upper = instance_count * max_run;
      // Only pre-size when the bound is realistic; a pathological prototype
      // table should not drive a huge speculative reservation.
      if (upper <= scene->point_instance_draws.size() + (size_t(1) << 22)) {
        scene->point_instance_draws.reserve(
            scene->point_instance_draws.size() + upper);
      }
    }
  }

  for (size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
    if (!instancer->instance_visible.empty() &&
        !instancer->instance_visible[instance_index]) {
      continue;
    }
    const int32_t proto_index = instancer->proto_indices[instance_index];
    if (proto_index < 0 ||
        static_cast<size_t>(proto_index + 1) >=
            instancer->prototype_mesh_offsets.size()) {
      continue;
    }

    const uint32_t begin =
        instancer->prototype_mesh_offsets[static_cast<size_t>(proto_index)];
    const uint32_t end =
        instancer->prototype_mesh_offsets[static_cast<size_t>(proto_index) + 1];
    for (uint32_t mesh_ref = begin; mesh_ref < end; ++mesh_ref) {
      if (mesh_ref >= instancer->prototype_mesh_ids.size()) continue;
      const int32_t mesh_id = instancer->prototype_mesh_ids[mesh_ref];
      if (mesh_id < 0) continue;

      RenderPointInstanceDraw draw;
      draw.point_instancer_id = instancer_id;
      draw.instance_index = static_cast<uint32_t>(instance_index);
      draw.prototype_index = static_cast<uint32_t>(proto_index);
      draw.mesh_id = mesh_id;
      if (static_cast<size_t>(mesh_id) < scene->meshes.size()) {
        draw.material_id = scene->meshes[static_cast<size_t>(mesh_id)].material_id;
      }
      Matrix4 instance_transform = Matrix4::Identity();
      if (instance_index < instancer->transforms.size()) {
        instance_transform = instancer->transforms[instance_index];
      }
      if (mesh_ref < instancer->prototype_mesh_transforms.size()) {
        draw.transform = MulMatrix4(instancer->prototype_mesh_transforms[mesh_ref],
                                    instance_transform);
      } else {
        draw.transform = instance_transform;
      }
      scene->point_instance_draws.push_back(draw);
      ++instancer->draw_count;
    }
  }
}
}  // namespace
void ResolvePointInstancerPrototypeBindings(RenderScene* scene,
                                            RenderPointInstancer* instancer) {
  ResolvePointInstancerPrototypeBindingsImpl(scene, instancer);
}
void AppendPointInstanceDraws(int32_t instancer_id,
                              RenderPointInstancer* instancer,
                              RenderScene* scene) {
  AppendPointInstanceDrawsImpl(instancer_id, instancer, scene);
}
}}}  // namespace lightusd::tydra::next
