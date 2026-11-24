#include <cmath>
#include <iostream>
#include <vector>

#include "tydra/render-data.hh"
#include "lightusd.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

static bool nearlyEqual(float a, float b, float eps = 1e-5f) {
  return std::fabs(a - b) <= eps;
}

static bool run_test(const char *label, bool use_sample_rate,
                     const char *scene_path = "tests/feat/value-clip/value_clip_main.usda") {
  std::cout << "Test: " << label << "\n";

  Stage stage;
  std::string warn;
  std::string err;

  bool ret = LoadUSDFromFile(scene_path, &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Load failed: " << err << "\n";
    return false;
  }

  RenderScene scene;
  RenderSceneConverterEnv env(stage);

  env.scene_config.enable_value_clips = true;
  env.scene_config.value_clip_sample_rate = use_sample_rate ? 4.0f : 0.0f;
  env.scene_config.value_clip_use_time_range = use_sample_rate;
  env.scene_config.value_clip_start_time = 0.0;
  env.scene_config.value_clip_end_time = 2.0;

  RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &scene)) {
    std::cerr << "ConvertToRenderScene failed: " << converter.GetError() << "\n";
    return false;
  }

  const auto &custom_conv_warn = converter.GetWarning();
  if (!custom_conv_warn.empty()) {
    std::cerr << "warning(custom): " << custom_conv_warn << "\n";
  }

  const auto &conv_warn = converter.GetWarning();
  if (!conv_warn.empty()) {
    std::cerr << "warning: " << conv_warn << "\n";
  }

  if (scene.animations.empty()) {
    std::cerr << "Expected 1 animation, got 0\n";
    return false;
  }

  if (scene.animations.size() != 1) {
    std::cerr << "Expected 1 animation, got " << scene.animations.size() << "\n";
    return false;
  }

  const AnimationClip &anim = scene.animations[0];
  if (!anim.has_value_clip || !anim.value_clip_baked) {
    std::cerr << "Expected value clip animation" << (use_sample_rate ? " (resampled)" : "")
              << "\n";
    return false;
  }

  if (anim.clip_asset_paths.size() != 2) {
    std::cerr << "Expected 2 clip assets, got " << anim.clip_asset_paths.size() << "\n";
    return false;
  }

  if (anim.samplers.size() != 3 || anim.channels.size() != 3) {
    std::cerr << "Expected 3 samplers + 3 channels, got " << anim.samplers.size()
              << " / " << anim.channels.size() << "\n";
    return false;
  }

  const auto &trans = anim.samplers[0];
  const auto &rot = anim.samplers[1];
  const auto &scale = anim.samplers[2];

  size_t expected_samples = use_sample_rate ? 9 : 3;
  if (trans.times.size() != expected_samples) {
    std::cerr << "Expected " << expected_samples << " translation samples, got "
              << trans.times.size() << "\n";
    return false;
  }

  if (trans.times.size() != rot.times.size() || trans.times.size() != scale.times.size()) {
    std::cerr << "Sample counts mismatch\n";
    return false;
  }

  if (trans.times.size() > 0) {
    if (!nearlyEqual(trans.times.front(), 0.0f) ||
        !nearlyEqual(trans.times.back(), 2.0f)) {
      std::cerr << "Unexpected sample time range: " << trans.times.front() << " - "
                << trans.times.back() << "\n";
      return false;
    }
  }

  // Clip 0 at start: translate = (0, 0, 0), scale = (1,1,1)
  if (trans.values.size() < 3 || scale.values.size() < 3 || rot.values.size() < 4) {
    std::cerr << "Unexpected sampler value size\n";
    return false;
  }

  if (!nearlyEqual(trans.values[0], 0.0f) || !nearlyEqual(trans.values[1], 0.0f) ||
      !nearlyEqual(trans.values[2], 0.0f) ||
      !nearlyEqual(scale.values[0], 1.0f) || !nearlyEqual(scale.values[1], 1.0f) ||
      !nearlyEqual(scale.values[2], 1.0f) ||
      !nearlyEqual(rot.values[3], 1.0f)) {
    std::cerr << "Unexpected first-frame sample\n";
    return false;
  }

  // Clip 1 near the end: translate x = 10, scale = (2,2,2)
  size_t end_base = 3 * (trans.times.size() - 1);
  size_t end_scale_base = 3 * (scale.times.size() - 1);
  size_t end_rot_base = 4 * (rot.times.size() - 1);

  if (!nearlyEqual(trans.values[end_base], 10.0f) ||
      !nearlyEqual(trans.values[end_base + 1], 0.0f) ||
      !nearlyEqual(trans.values[end_base + 2], 0.0f) ||
      !nearlyEqual(scale.values[end_scale_base], 2.0f) ||
      !nearlyEqual(scale.values[end_scale_base + 1], 2.0f) ||
      !nearlyEqual(scale.values[end_scale_base + 2], 2.0f) ||
      !nearlyEqual(rot.values[end_rot_base + 3], 1.0f)) {
    std::cerr << "Unexpected last-frame sample\n";
    return false;
  }

  if (use_sample_rate && anim.value_clip_sample_rate != 4.0f) {
    std::cerr << "Expected sample rate 4.0, got " << anim.value_clip_sample_rate << "\n";
    return false;
  }

  if (!use_sample_rate && anim.value_clip_sample_rate != 0.0f) {
    std::cerr << "Expected sample rate 0.0, got " << anim.value_clip_sample_rate
              << "\n";
    return false;
  }

  std::cout << "  pass\n\n";
  return true;
}

static bool run_custom_property_test(const char *label, bool use_sample_rate,
                                    const char *scene_path) {
  std::cout << "Test: " << label << "\n";

  Stage stage;
  std::string warn;
  std::string err;

  bool ret = LoadUSDFromFile(scene_path, &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Load failed: " << err << "\n";
    return false;
  }

  RenderScene scene;
  RenderSceneConverterEnv env(stage);
  env.scene_config.enable_value_clips = true;
  env.scene_config.value_clip_sample_rate = use_sample_rate ? 4.0f : 0.0f;
  env.scene_config.value_clip_use_time_range = use_sample_rate;
  env.scene_config.value_clip_start_time = 0.0;
  env.scene_config.value_clip_end_time = 2.0;

  RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &scene)) {
    std::cerr << "ConvertToRenderScene failed: " << converter.GetError() << "\n";
    return false;
  }

  if (scene.animations.empty()) {
    std::cerr << "Expected 1 animation, got 0\n";
    return false;
  }

  if (scene.animations.size() != 1) {
    std::cerr << "Expected 1 animation, got " << scene.animations.size() << "\n";
    return false;
  }

  const AnimationClip &anim = scene.animations[0];
  if (!anim.has_value_clip || !anim.value_clip_baked) {
    std::cerr << "Expected value clip animation"
              << (use_sample_rate ? " (resampled)" : "") << "\n";
    return false;
  }

  if (anim.channels.size() != 5) {
    std::cerr << "Expected 5 channels (translation/rotation/scale + 2 custom), got "
              << anim.channels.size() << "\n";
    return false;
  }

  const size_t expected_samples = use_sample_rate ? 9 : 3;
  if (anim.samplers.size() < 5) {
    std::cerr << "Expected at least 5 samplers, got " << anim.samplers.size() << "\n";
    return false;
  }

  bool found_translation = false;
  bool found_rotation = false;
  bool found_scale = false;
  bool found_radius = false;
  bool found_traj = false;
  size_t radius_sampler = 0;
  size_t traj_sampler = 0;

  for (size_t i = 0; i < anim.channels.size(); i++) {
    const auto &ch = anim.channels[i];
    if (ch.path == lightusd::legacy::tydra::AnimationPath::Translation) {
      found_translation = true;
    } else if (ch.path == lightusd::legacy::tydra::AnimationPath::Rotation) {
      found_rotation = true;
    } else if (ch.path == lightusd::legacy::tydra::AnimationPath::Scale) {
      found_scale = true;
    } else if (ch.path == lightusd::legacy::tydra::AnimationPath::CustomProperty) {
      if (ch.property_name == "physics:radius") {
        found_radius = true;
        radius_sampler = static_cast<size_t>(ch.sampler);
      } else if (ch.property_name == "physics:traj") {
        found_traj = true;
        traj_sampler = static_cast<size_t>(ch.sampler);
      } else {
        std::cerr << "Unexpected custom property: " << ch.property_name << "\n";
        return false;
      }
    }
    if (ch.sampler < 0 || ch.sampler >= static_cast<int>(anim.samplers.size())) {
      std::cerr << "Invalid sampler index on channel " << i << ": " << ch.sampler << "\n";
      return false;
    }
  }

  if (!found_translation || !found_rotation || !found_scale ||
      !found_radius || !found_traj) {
    std::cerr << "Missing expected channels\n";
    return false;
  }

  if (anim.samplers[0].times.size() != expected_samples ||
      anim.samplers[1].times.size() != expected_samples ||
      anim.samplers[2].times.size() != expected_samples) {
    std::cerr << "Expected " << expected_samples
              << " transform samples, got " << anim.samplers[0].times.size()
              << "," << anim.samplers[1].times.size() << ","
              << anim.samplers[2].times.size() << "\n";
    return false;
  }

  const auto &radius_sampler_data = anim.samplers[radius_sampler];
  const auto &traj_sampler_data = anim.samplers[traj_sampler];
  if (radius_sampler_data.times.size() != expected_samples ||
      radius_sampler_data.values.size() != expected_samples ||
      traj_sampler_data.times.size() != expected_samples ||
      traj_sampler_data.values.size() != expected_samples * 3) {
    std::cerr << "Unexpected custom sampler size. radius=("
              << radius_sampler_data.times.size() << ","
              << radius_sampler_data.values.size() << ") traj=("
              << traj_sampler_data.times.size() << ","
              << traj_sampler_data.values.size() << ")\n";
    return false;
  }

  if (!nearlyEqual(radius_sampler_data.values[0], 1.0f) ||
      !nearlyEqual(radius_sampler_data.values.back(), 4.0f)) {
    std::cerr << "Unexpected trajectory radius sequence\n";
    return false;
  }

  const size_t last_traj = (expected_samples - 1) * 3;
  if (!nearlyEqual(traj_sampler_data.values[0], 0.0f) ||
      !nearlyEqual(traj_sampler_data.values[1], 0.0f) ||
      !nearlyEqual(traj_sampler_data.values[2], 0.0f) ||
      !nearlyEqual(traj_sampler_data.values[last_traj], 4.0f) ||
      !nearlyEqual(traj_sampler_data.values[last_traj + 1], 2.0f)) {
    std::cerr << "Unexpected trajectory sequence\n";
    return false;
  }

  if (use_sample_rate && !nearlyEqual(anim.value_clip_sample_rate, 4.0f)) {
    std::cerr << "Expected sample rate 4.0, got " << anim.value_clip_sample_rate
              << "\n";
    return false;
  }

  if (!use_sample_rate && !nearlyEqual(anim.value_clip_sample_rate, 0.0f)) {
    std::cerr << "Expected sample rate 0.0, got " << anim.value_clip_sample_rate
              << "\n";
    return false;
  }

  std::cout << "  pass\n\n";
  return true;
}

int main() {
  bool all_passed = true;

  all_passed &= run_test("value clip conversion (no resample)", false);
  all_passed &= run_test("value clip conversion (resample)", true);
  all_passed &= run_custom_property_test("value clip custom property conversion (no resample)",
                                        false,
                                        "tests/feat/value-clip/value_clip_main_custom.usda");
  all_passed &= run_custom_property_test(
      "value clip custom property conversion (resample)", true,
      "tests/feat/value-clip/value_clip_main_custom.usda");

  if (all_passed) {
    std::cout << "✓ all value clip feat tests passed\n";
    return 0;
  }

  std::cout << "✗ value clip feat test failed\n";
  return 1;
}
