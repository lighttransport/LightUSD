// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Converter state resolution, progress reporting, and streaming sink emission.

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tiny-format.hh"
#include "tydra/render-data.hh"
#include "tydra/shader-network.hh"

namespace lightusd {
namespace tydra {

bool RenderSceneConverter::ResolveBlendShapeAnimationTargets() {
  struct MeshNodeRef {
    int32_t node_index{-1};
    int32_t mesh_id{-1};
    std::string abs_path;
  };

  std::vector<MeshNodeRef> mesh_nodes;
  int32_t node_index = 0;
  std::function<void(const Node &, int)> collectMeshNodes =
      [&](const Node &node, int depth) {
        if (depth > 4096) return;
        const int32_t current_index = node_index++;
        if (node.nodeType == NodeType::Mesh && node.id >= 0 &&
            size_t(node.id) < meshes.size()) {
          MeshNodeRef ref;
          ref.node_index = current_index;
          ref.mesh_id = node.id;
          ref.abs_path = node.abs_path;
          mesh_nodes.push_back(std::move(ref));
        }
        for (const Node &child : node.children) {
          collectMeshNodes(child, depth + 1);
        }
      };

  for (const Node &root : root_nodes) {
    collectMeshNodes(root, /*depth*/ 0);
  }

  auto meshMatchesChannel = [&](const RenderMesh &mesh,
                                const AnimationChannel &channel) {
    if (mesh.skel_id != channel.skeleton_id) {
      return false;
    }
    if (channel.blendshape_target_names.empty()) {
      return !mesh.targets.empty();
    }
    for (const std::string &name : channel.blendshape_target_names) {
      if (mesh.targets.find(name) == mesh.targets.end()) {
        return false;
      }
    }
    return true;
  };

  size_t resolved_count = 0;
  for (AnimationClip &clip : animations) {
    const size_t original_channel_count = clip.channels.size();
    std::vector<AnimationChannel> extra_channels;

    for (size_t ch_idx = 0; ch_idx < original_channel_count; ch_idx++) {
      AnimationChannel &channel = clip.channels[ch_idx];
      if (channel.path != AnimationPath::Weights ||
          channel.target_type != ChannelTargetType::SceneNode ||
          channel.target_node >= 0 || channel.skeleton_id < 0) {
        continue;
      }

      std::vector<MeshNodeRef> matches;
      for (const MeshNodeRef &ref : mesh_nodes) {
        const RenderMesh &mesh = meshes[size_t(ref.mesh_id)];
        if (meshMatchesChannel(mesh, channel)) {
          matches.push_back(ref);
        }
      }

      if (matches.empty()) {
        PushWarn(fmt::format(
            "Could not resolve blendShapeWeights target for animation {} "
            "skeleton_id {}.",
            clip.abs_path, channel.skeleton_id));
        continue;
      }

      channel.target_node = matches[0].node_index;
      channel.target_prim_path = matches[0].abs_path;
      resolved_count++;

      for (size_t i = 1; i < matches.size(); i++) {
        AnimationChannel duplicate = channel;
        duplicate.target_node = matches[i].node_index;
        duplicate.target_prim_path = matches[i].abs_path;
        extra_channels.push_back(std::move(duplicate));
        resolved_count++;
      }
    }

    if (!extra_channels.empty()) {
      clip.channels.insert(clip.channels.end(), extra_channels.begin(),
                           extra_channels.end());
    }

    std::set<int32_t> animated_nodes;
    for (const AnimationChannel &channel : clip.channels) {
      if (channel.target_type == ChannelTargetType::SceneNode &&
          channel.target_node >= 0) {
        animated_nodes.insert(channel.target_node);
      }
    }
    if (!animated_nodes.empty()) {
      clip.num_animated_nodes = int32_t(animated_nodes.size());
    }
  }

  if (resolved_count > 0) {
    PushInfo("Resolved " + std::to_string(resolved_count) +
             " blendShapeWeights animation target(s).");
  }

  return true;
}

bool RenderSceneConverter::GetBoundMaterialCached(
    const Stage &stage, const Path &abs_path,
    const std::string &purpose, Path *materialPath,
    const Material **material, std::string *err) {
  // Build cache key: "prim_path\0purpose"
  std::string key = abs_path.full_path_name();
  key.push_back('\0');
  key += purpose;

  auto it = _materialBindingCache.find(key);
  if (it != _materialBindingCache.end()) {
    if (!it->second.error.empty()) {
      if (err) {
        (*err) += it->second.error;
      }
      return false;
    }

    if (it->second.found) {
      *materialPath = it->second.materialPath;
      *material = it->second.material;
    }
    return it->second.found;
  }

  std::string local_err;
  bool found = GetBoundMaterial(stage, abs_path, purpose,
                                materialPath, material, &local_err);

  MaterialBindingCacheEntry entry;
  entry.found = found;
  if (found) {
    entry.materialPath = *materialPath;
    entry.material = *material;
  }
  entry.error = local_err;
  _materialBindingCache[key] = entry;

  if (!local_err.empty() && err) {
    (*err) += local_err;
  }

  return found;
}


void RenderSceneConverter::SetProgressCallback(ProgressCallback callback, void *userptr) {
  _progress_callback = callback;
  _progress_userptr = userptr;
}

void RenderSceneConverter::SetDetailedProgressCallback(DetailedProgressCallback callback, void *userptr) {
  _detailed_progress_callback = callback;
  _detailed_progress_userptr = userptr;
}

bool RenderSceneConverter::CallProgressCallback(float progress) {
  if (_progress_callback) {
    return _progress_callback(progress, _progress_userptr);
  }
  return true; // Continue if no callback set
}

bool RenderSceneConverter::CallDetailedProgressCallback(const DetailedProgressInfo &info) {
  if (_detailed_progress_callback) {
    return _detailed_progress_callback(info, _detailed_progress_userptr);
  }
  return true; // Continue if no callback set
}

bool RenderSceneConverter::ReportMeshProgress(size_t meshes_processed, size_t meshes_total,
                                               const std::string& mesh_name, const std::string& message) {
  _progress_info.stage = DetailedProgressInfo::Stage::ConvertingMeshes;
  _progress_info.meshes_processed = meshes_processed;
  _progress_info.meshes_total = meshes_total;
  _progress_info.current_mesh_name = mesh_name;
  _progress_info.message = message;

  // Calculate progress: meshes are 20%-70% of total progress (50% range)
  float mesh_progress = 0.2f + (0.5f * float(meshes_processed) / float(std::max(size_t(1), meshes_total)));
  _progress_info.progress = mesh_progress;

  return CallDetailedProgressCallback(_progress_info);
}

// ---------------------------------------------------------------------------
// Streaming emit helpers (no-ops unless a sink is set via
// ConvertToRenderSceneStreaming). Each reads the just-appended element from the
// converter's member array by index and returns false to request cancellation.
// ---------------------------------------------------------------------------
bool RenderSceneConverter::EmitPhase(StreamPhase phase) {
  if (_sink && _sink->on_phase) {
    return _sink->on_phase(phase, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitImage(size_t index) {
  if (_sink && _sink->on_image) {
    return _sink->on_image(images[index], index, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitBuffer(size_t index) {
  if (_sink && _sink->on_buffer) {
    return _sink->on_buffer(buffers[index], index, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitTexture(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_texture) {
    return _sink->on_texture(textures[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitUdimTexture(size_t index) {
  if (_sink && _sink->on_udim_texture) {
    return _sink->on_udim_texture(udim_textures[index], index, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitMaterial(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_material) {
    return _sink->on_material(materials[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitMesh(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_mesh) {
    return _sink->on_mesh(meshes[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitLight(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_light) {
    return _sink->on_light(lights[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitCamera(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_camera) {
    return _sink->on_camera(cameras[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitRootNode(size_t index) {
  if (_sink && _sink->on_root_node) {
    return _sink->on_root_node(root_nodes[index], index, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitSkeleton(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_skeleton) {
    return _sink->on_skeleton(skeletons[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitAnimation(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_animation) {
    return _sink->on_animation(animations[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitInstance(size_t index, const std::string &abs_path) {
  if (_sink && _sink->on_instance) {
    return _sink->on_instance(instances[index], index, abs_path, _sink->userdata);
  }
  return true;
}
bool RenderSceneConverter::EmitComplete(const RenderScene &scene) {
  if (_sink && _sink->on_complete) {
    return _sink->on_complete(scene, _sink->userdata);
  }
  return true;
}


}  // namespace tydra
}  // namespace lightusd
