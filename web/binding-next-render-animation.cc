// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
emscripten::val RenderStream::getAnimation(int32_t anim_id) const {
    emscripten::val out = emscripten::val::object();
    if (!loaded_ || anim_id < 0 ||
        static_cast<size_t>(anim_id) >= render_scene_.animations.size()) {
      return out;
    }
    const tr::AnimationClip& clip = render_scene_.animations[static_cast<size_t>(anim_id)];

    const double duration = clip.end_time - clip.start_time;
    const double clamped_duration = std::isfinite(duration)
                                       ? std::max(0.0, duration)
                                       : 0.0;
    out.set("index", anim_id);
    out.set("name", clip.name.empty()
                           ? std::string("Animation") + std::to_string(anim_id)
                           : clip.name);
    out.set("primPath", clip.prim_path);
    out.set("startTime", clip.start_time);
    out.set("endTime", clip.end_time);
    out.set("duration", clamped_duration);

    emscripten::val channels = emscripten::val::array();
    emscripten::val samplers = emscripten::val::array();
    emscripten::val tracks = emscripten::val::array();

    for (size_t i = 0; i < clip.channels.size(); ++i) {
      const tr::AnimationChannel& channel = clip.channels[i];

      std::vector<float> times;
      times.reserve(channel.keyframes.size());
      std::vector<float> values;
      values.reserve(channel.keyframes.size() *
                     AnimationComponentCount(channel));

      for (const auto& keyframe : channel.keyframes) {
        times.push_back(static_cast<float>(keyframe.time));
        AppendAnimationKeyframeValues(channel, keyframe, &values);
      }

      const std::string path = AnimationPathName(channel.target_path);
      const std::string interpolation =
          AnimationInterpolationName(channel.interpolation);
      const int32_t sampler_id = static_cast<int32_t>(i);

      emscripten::val sampler = emscripten::val::object();
      sampler.set("index", sampler_id);
      sampler.set("interpolation", interpolation);
      sampler.set("times", VectorToArray(times));
      sampler.set("values", VectorToArray(values));
      sampler.set("valueStride", static_cast<int>(channel.value_stride));
      sampler.set("elementCount", static_cast<int>(channel.element_count));
      sampler.set("isSkeletal", channel.is_skeletal);
      if (!channel.array_values.empty()) {
        sampler.set("arrayValues", VectorToArray(channel.array_values));
      }
      samplers.set(static_cast<int>(sampler_id), sampler);

      emscripten::val ch = emscripten::val::object();
      ch.set("sampler", sampler_id);
      ch.set("target_node", channel.target_node);
      ch.set("target_prim_path", channel.target_prim_path);
      ch.set("target_type", channel.is_skeletal ? std::string("SkelAnimation")
                                                 : std::string("SceneNode"));
      ch.set("skeleton_id", channel.target_skeleton);
      ch.set("joint_id", -1);
      ch.set("path", path);
      ch.set("isCustomProperty",
             channel.target_path == tr::AnimationChannel::TargetPath::CustomProperty);
      ch.set("propertyName", channel.property_name);
      ch.set("isSkeletal", channel.is_skeletal);
      ch.set("targetSkeletonPath", channel.target_skeleton_path);
      ch.set("jointOrder", VectorToArray(channel.joint_order));
      ch.set("jointRemap", VectorToArray(channel.joint_remap));
      ch.set("blendShapeOrder", VectorToArray(channel.blend_shape_order));
      ch.set("valueStride", static_cast<int>(channel.value_stride));
      ch.set("elementCount", static_cast<int>(channel.element_count));
      channels.set(static_cast<int>(i), ch);

      emscripten::val track = emscripten::val::object();
      track.set("sampler", sampler_id);
      track.set("target_node", channel.target_node);
      track.set("path", path);
      track.set("interpolation", interpolation);
      track.set("times", VectorToArray(times));
      track.set("values", VectorToArray(values));
      track.set("isSkeletal", channel.is_skeletal);
      track.set("propertyName", channel.property_name);
      track.set("targetSkeletonId", channel.target_skeleton);
      track.set("targetSkeletonPath", channel.target_skeleton_path);
      track.set("jointRemap", VectorToArray(channel.joint_remap));
      track.set("valueStride", static_cast<int>(channel.value_stride));
      track.set("elementCount", static_cast<int>(channel.element_count));
      if (!channel.array_values.empty()) {
        track.set("arrayValues", VectorToArray(channel.array_values));
      }

      std::string track_type = "number";
      switch (channel.target_path) {
        case tr::AnimationChannel::TargetPath::Translation:
          track.set("name", path);
          track_type = channel.is_skeletal ? "vector3Array" : "vector3";
          break;
        case tr::AnimationChannel::TargetPath::Rotation:
          track.set("name", path);
          track_type = channel.is_skeletal ? "quaternionArray" : "quaternion";
          break;
        case tr::AnimationChannel::TargetPath::Scale:
          track.set("name", path);
          track_type = channel.is_skeletal ? "vector3Array" : "vector3";
          break;
        case tr::AnimationChannel::TargetPath::Weights:
          track.set("name", path);
          track_type = channel.is_skeletal ? "weightArray" : "number";
          break;
        case tr::AnimationChannel::TargetPath::CustomProperty:
        default:
          track.set("name", path);
          track_type = "number";
          break;
      }
      track.set("type", track_type);
      tracks.set(static_cast<int>(i), track);
    }

    out.set("channels", channels);
    out.set("samplers", samplers);
    out.set("tracks", tracks);
    out.set("numChannels", static_cast<int>(clip.channels.size()));
    out.set("numSamplers", static_cast<int>(clip.channels.size()));
    out.set("has_skeletal_animation", AnimationHasSkeletalChannels(clip));
    out.set("has_node_animation", static_cast<bool>(!clip.channels.empty()));

    if (clip.channels.empty()) {
      out.set("tracks", emscripten::val::array());
    }

    return out;
  }

emscripten::val RenderStream::getAnimationView(int32_t anim_id) const {
    emscripten::val out = emscripten::val::object();
    if (!loaded_ || anim_id < 0 ||
        static_cast<size_t>(anim_id) >= render_scene_.animations.size()) {
      return out;
    }
    const tr::AnimationClip& clip =
        render_scene_.animations[static_cast<size_t>(anim_id)];

    const double duration = clip.end_time - clip.start_time;
    const double clamped_duration = std::isfinite(duration)
                                        ? std::max(0.0, duration)
                                        : 0.0;
    out.set("index", anim_id);
    out.set("name", clip.name.empty()
                        ? std::string("Animation") + std::to_string(anim_id)
                        : clip.name);
    out.set("primPath", clip.prim_path);
    out.set("startTime", clip.start_time);
    out.set("endTime", clip.end_time);
    out.set("duration", clamped_duration);

    emscripten::val channels = emscripten::val::array();
    emscripten::val samplers = emscripten::val::array();
    for (size_t i = 0; i < clip.channels.size(); ++i) {
      const tr::AnimationChannel& channel = clip.channels[i];
      std::vector<float> times;
      times.reserve(channel.keyframes.size());
      std::vector<float> values;
      values.reserve(channel.keyframes.size() *
                     AnimationComponentCount(channel));
      for (const auto& keyframe : channel.keyframes) {
        times.push_back(static_cast<float>(keyframe.time));
        AppendAnimationKeyframeValues(channel, keyframe, &values);
      }

      const std::string path = AnimationPathName(channel.target_path);
      const std::string interpolation =
          AnimationInterpolationName(channel.interpolation);
      const int32_t sampler_id = static_cast<int32_t>(i);

      emscripten::val sampler = emscripten::val::object();
      sampler.set("index", sampler_id);
      sampler.set("interpolation", interpolation);
      sampler.set("times", VectorToArray(times));
      sampler.set("values", VectorToArray(values));
      sampler.set("valueStride", static_cast<int>(channel.value_stride));
      sampler.set("elementCount", static_cast<int>(channel.element_count));
      sampler.set("isSkeletal", channel.is_skeletal);
      if (!channel.array_values.empty()) {
        sampler.set("arrayValues", heapF_(channel.array_values,
                                          static_cast<int>(channel.value_stride)));
      }
      samplers.set(sampler_id, sampler);

      emscripten::val ch = emscripten::val::object();
      ch.set("sampler", sampler_id);
      ch.set("target_node", channel.target_node);
      ch.set("target_prim_path", channel.target_prim_path);
      ch.set("target_type", channel.is_skeletal ? std::string("SkelAnimation")
                                                  : std::string("SceneNode"));
      ch.set("skeleton_id", channel.target_skeleton);
      ch.set("joint_id", -1);
      ch.set("path", path);
      ch.set("isCustomProperty",
             channel.target_path ==
                 tr::AnimationChannel::TargetPath::CustomProperty);
      ch.set("propertyName", channel.property_name);
      ch.set("isSkeletal", channel.is_skeletal);
      ch.set("targetSkeletonPath", channel.target_skeleton_path);
      ch.set("jointOrder", VectorToArray(channel.joint_order));
      ch.set("jointRemap", VectorToArray(channel.joint_remap));
      ch.set("blendShapeOrder", VectorToArray(channel.blend_shape_order));
      ch.set("valueStride", static_cast<int>(channel.value_stride));
      ch.set("elementCount", static_cast<int>(channel.element_count));
      channels.set(static_cast<int>(i), ch);
    }

    out.set("channels", channels);
    out.set("samplers", samplers);
    out.set("numChannels", static_cast<int>(clip.channels.size()));
    out.set("numSamplers", static_cast<int>(clip.channels.size()));
    out.set("has_skeletal_animation", AnimationHasSkeletalChannels(clip));
    out.set("has_node_animation", static_cast<bool>(!clip.channels.empty()));
    return out;
  }

emscripten::val RenderStream::getAllAnimations() const {
    emscripten::val animations = emscripten::val::array();
    if (!loaded_) {
      return animations;
    }
    for (int i = 0; i < static_cast<int>(render_scene_.animations.size()); ++i) {
      animations.call<void>("push", getAnimation(i));
    }
    return animations;
  }

emscripten::val RenderStream::getAnimationInfo(int32_t anim_id) const {
    emscripten::val info = emscripten::val::object();
    if (!loaded_ || anim_id < 0 ||
        static_cast<size_t>(anim_id) >= render_scene_.animations.size()) {
      return info;
    }

    const tr::AnimationClip& clip = render_scene_.animations[static_cast<size_t>(anim_id)];
    info.set("id", anim_id);
    info.set("name", clip.name.empty()
                          ? std::string("Animation") + std::to_string(anim_id)
                          : clip.name);
    info.set("duration", clip.end_time - clip.start_time);
    info.set("numTracks", static_cast<int>(clip.channels.size()));
    info.set("numSamplers", static_cast<int>(clip.channels.size()));
    info.set("numTargetNodes", AnimationTargetNodeCount(clip));
    const bool has_skel = AnimationHasSkeletalChannels(clip);
    info.set("has_skeletal_animation", has_skel);
    info.set("has_node_animation", static_cast<bool>(!clip.channels.empty()));
    info.set("startTime", clip.start_time);
    info.set("endTime", clip.end_time);
    info.set("clipAssetPaths", VectorToArray(clip.clip_asset_paths));
    info.set("numClipAssetPaths",
             static_cast<int>(clip.clip_asset_paths.size()));
    info.set("valueClipBaked", clip.value_clip_baked);
    info.set("sourceType", clip.value_clip_baked
                               ? std::string("ValueClip")
                               : (has_skel ? std::string("SkelAnimation")
                                           : std::string("XformOp")));
    return info;
  }

emscripten::val RenderStream::getAllAnimationInfos() const {
    emscripten::val infos = emscripten::val::array();
    if (!loaded_) {
      return infos;
    }
    for (int i = 0; i < static_cast<int>(render_scene_.animations.size()); ++i) {
      infos.call<void>("push", getAnimationInfo(i));
    }
    return infos;
  }

emscripten::val RenderStream::getUnsupportedRenderables() const {
    emscripten::val out = emscripten::val::array();
    if (!render_scene_valid_) return out;
    for (size_t i = 0; i < render_scene_.unsupported_renderables.size(); ++i) {
      const tr::UnsupportedRenderable& unsupported =
          render_scene_.unsupported_renderables[static_cast<size_t>(i)];
      emscripten::val item = emscripten::val::object();
      item.set("index", static_cast<int>(i));
      item.set("primPath", unsupported.prim_path);
      item.set("type", unsupported.type_name);
      item.set("reason", unsupported.reason);
      out.set(static_cast<int>(i), item);
    }
    return out;
  }
}  // namespace web_next
}  // namespace lightusd
