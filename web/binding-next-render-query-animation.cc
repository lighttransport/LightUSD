// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lightusd {
namespace web_next {
int RenderStream::skeletonField(int skeleton_id, uint8_t field) const {
    if (!render_scene_valid_ || skeleton_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size()) return -1;
    const tr::Skeleton& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    switch (field) {
      case 0: return skeleton.joints.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
                         ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(skeleton.joints.size());
      case 1: return skeleton.root_joint;
      case 2: return skeleton.animation_id;
      default: return -1;
    }
}

int RenderStream::skeletonJointBufferCopy(int skeleton_id, uint8_t kind,
                                          uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size() || kind > 2) return -1;
    const tr::Skeleton& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    size_t count = skeleton.joints.size();
    size_t element_size = kind == 2 ? sizeof(int32_t) : 16 * sizeof(double);
    size_t bytes = count * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    for (size_t i = 0; i < count; ++i) {
      if (kind == 2) {
        std::memcpy(out + i * element_size, &skeleton.joints[i].parent_id, element_size);
      } else {
        const tr::Matrix4d& matrix = kind == 0 ? skeleton.joints[i].bind_transform
                                               : skeleton.joints[i].rest_transform;
        std::memcpy(out + i * element_size, matrix.m, element_size);
      }
    }
    return required;
}

int RenderStream::skeletonJointChildrenCopy(int skeleton_id, int joint_id,
                                            uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 || joint_id < 0 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size())
      return -1;
    const auto& joints = render_scene_.skeletons[static_cast<size_t>(skeleton_id)].joints;
    if (static_cast<size_t>(joint_id) >= joints.size()) return -1;
    const auto& children = joints[static_cast<size_t>(joint_id)].children;
    if (children.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                              sizeof(int32_t)) return -1;
    const int required = static_cast<int>(children.size() * sizeof(int32_t));
    if (!out || cap < static_cast<uint32_t>(required) || children.empty())
      return required;
    std::memcpy(out, children.data(), static_cast<size_t>(required));
    return required;
}

int RenderStream::skeletonJointStringCopy(int skeleton_id, int joint_id,
                                          uint8_t kind, uint8_t* out,
                                          uint32_t cap) const {
    if (!render_scene_valid_ || skeleton_id < 0 || kind > 2 ||
        static_cast<size_t>(skeleton_id) >= render_scene_.skeletons.size()) return -1;
    const auto& skeleton = render_scene_.skeletons[static_cast<size_t>(skeleton_id)];
    const std::string* value_ptr = &skeleton.animation_source_path;
    if (kind != 2) {
      if (joint_id < 0 || static_cast<size_t>(joint_id) >= skeleton.joints.size())
        return -1;
      const auto& joint = skeleton.joints[static_cast<size_t>(joint_id)];
      value_ptr = kind == 0 ? &joint.name : &joint.path;
    }
    const std::string& value = *value_ptr;
    return CopyOutputString(value, out, cap);
}

int RenderStream::animationInfo(int animation_id,
                                lightusd_next_animation_info* out) const {
    if (!out || out->struct_size < sizeof(lightusd_next_animation_info) ||
        !render_scene_valid_ || animation_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->id = animation_id;
    const tr::AnimationClip& clip =
        render_scene_.animations[static_cast<size_t>(animation_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    if (clip.channels.size() > limit || clip.clip_asset_paths.size() > limit)
      return -1;
    out->start_time = clip.start_time;
    out->end_time = clip.end_time;
    out->track_count = static_cast<int32_t>(clip.channels.size());
    out->clip_asset_count = static_cast<int32_t>(clip.clip_asset_paths.size());
    int32_t flags = clip.value_clip_baked ? 4 : 0;
    for (const tr::AnimationChannel& channel : clip.channels) {
      if (channel.is_skeletal) flags |= 1;
    }
    if (!clip.channels.empty()) flags |= 2;
    out->target_node_count = AnimationTargetNodeCount(clip);
    out->flags = flags;
    return 0;
}

int RenderStream::animationClipAssetCopy(int animation_id, int asset_id,
                                         uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || asset_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size())
      return -1;
    const auto& paths = render_scene_.animations[static_cast<size_t>(animation_id)].clip_asset_paths;
    if (static_cast<size_t>(asset_id) >= paths.size()) return -1;
    const std::string& path = paths[static_cast<size_t>(asset_id)];
    if (path.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(path.size());
    if (!out || cap < path.size() || path.empty()) return required;
    std::memcpy(out, path.data(), path.size());
    return required;
}

int RenderStream::animationChannelCount(int animation_id) const {
    if (!render_scene_valid_ || animation_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    return static_cast<int>(render_scene_.animations[static_cast<size_t>(animation_id)].channels.size());
}

int RenderStream::animationChannelField(int animation_id, int channel_id,
                                        uint8_t field) const {
    if (animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    switch (field) {
      case 0: return channel.target_node;
      case 1: return channel.target_skeleton;
      case 2: return static_cast<int>(channel.keyframes.size());
      case 3: return static_cast<int>(channel.element_count);
      case 4: return static_cast<int>(channel.value_stride);
      case 5: return static_cast<int>(channel.interpolation);
      case 6: return channel.is_skeletal ? 1 : 0;
      case 7: return static_cast<int>(channel.target_path);
      case 8: return static_cast<int>(channel.joint_order.size());
      case 9: return static_cast<int>(channel.blend_shape_order.size());
      case 10: return static_cast<int>(channel.joint_remap.size());
      default: return -1;
    }
}

int RenderStream::animationChannelBufferCopy(int animation_id, int channel_id,
                                              uint8_t kind, uint8_t* out,
                                              uint32_t cap) const {
    if (animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size() || kind > 3) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    size_t count = 0;
    size_t element_size = 0;
    const void* contiguous = nullptr;
    if (kind == 0) {
      count = channel.keyframes.size();
      element_size = sizeof(double);
    } else if (kind == 1 &&
               channel.target_path == tr::AnimationChannel::TargetPath::CustomProperty &&
               !channel.array_values.empty()) {
      // Custom-property keys are compact (value_stride x element_count lanes
      // per key), matching the legacy sampler values.
      count = channel.array_values.size();
      element_size = sizeof(float);
      contiguous = channel.array_values.data();
    } else if (kind == 1) {
      count = channel.keyframes.size() * AnimationComponentCount(channel);
      element_size = sizeof(float);
    } else if (kind == 2) {
      count = channel.array_values.size();
      element_size = sizeof(float);
      contiguous = channel.array_values.data();
    } else {
      count = channel.joint_remap.size();
      element_size = sizeof(int32_t);
      contiguous = channel.joint_remap.data();
    }
    const size_t bytes = count * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (contiguous) {
      std::memcpy(out, contiguous, bytes);
    } else if (kind == 0) {
      for (size_t i = 0; i < channel.keyframes.size(); ++i)
        std::memcpy(out + i * sizeof(double), &channel.keyframes[i].time, sizeof(double));
    } else {
      const size_t components = AnimationComponentCount(channel);
      for (size_t i = 0; i < channel.keyframes.size(); ++i)
        std::memcpy(out + i * components * sizeof(float),
                    &channel.keyframes[i].value, components * sizeof(float));
    }
    return required;
}

int RenderStream::animationArrayView(int animation_id, int channel_id,
                                     lightusd_next_animation_array_view* out) const {
    if (!out || out->struct_size < sizeof(*out) || !render_scene_valid_ ||
        animation_id < 0 || channel_id < 0 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    out->ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(channel.array_values.data()));
    out->length = static_cast<uint32_t>(channel.array_values.size());
    out->comps = channel.value_stride;
    return 0;
}

int RenderStream::animationChannelStringCopy(int animation_id, int channel_id,
                                             uint8_t kind, uint8_t* out,
                                             uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || channel_id < 0 || kind > 2 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& channel = channels[static_cast<size_t>(channel_id)];
    const std::string* value = kind == 0 ? &channel.target_prim_path
                              : kind == 1 ? &channel.property_name
                                          : &channel.target_skeleton_path;
    return CopyOutputString(*value, out, cap);
}

int RenderStream::animationChannelOrderStringCopy(int animation_id, int channel_id,
                                                  uint8_t kind, int order_id,
                                                  uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || animation_id < 0 || channel_id < 0 || order_id < 0 || kind > 1 ||
        static_cast<size_t>(animation_id) >= render_scene_.animations.size()) return -1;
    const auto& channels = render_scene_.animations[static_cast<size_t>(animation_id)].channels;
    if (static_cast<size_t>(channel_id) >= channels.size()) return -1;
    const auto& values = kind == 0 ? channels[static_cast<size_t>(channel_id)].joint_order
                                   : channels[static_cast<size_t>(channel_id)].blend_shape_order;
    if (static_cast<size_t>(order_id) >= values.size()) return -1;
    const std::string& value = values[static_cast<size_t>(order_id)];
    return CopyOutputString(value, out, cap);
}

}  // namespace web_next
}  // namespace lightusd
