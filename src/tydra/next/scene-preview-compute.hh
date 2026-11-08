// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "scene-preview.hh"
namespace lightusd {
namespace tydra {
namespace next {
ScenePreviewResult OpenScenePreviewVolume(const std::string&,
                                          const ScenePreviewOptions&);
bool ValidatePreviewDispatch(const std::vector<uint32_t>&, size_t,
                             std::string*, bool debug_validate = false);
class PreviewCompute {
 public:
  virtual ~PreviewCompute() = default;
  virtual const char* name() const = 0;
  virtual bool dispatch(const std::vector<uint32_t>& input,
                        size_t output_floats, std::vector<float>* output,
                        std::string* error, bool camera_only = false) = 0;
};
std::unique_ptr<PreviewCompute> CreatePreviewCompute(
    PreviewComputeBackend backend, uint64_t budget, std::string* error,
    const PreviewComputeOptions& options = {});
// Camera rays: origin + x*right + y*up for orthographic; forward + x*right +
// y*up for perspective. right/up include the half-width/height at unit depth.
struct PreviewVolumeCamera {
  std::array<float, 3> eye{}, forward{0, 0, -1}, right{1, 0, 0}, up{0, 1, 0};
  std::array<float, 3> background{.06f, .06f, .06f};
  bool orthographic = false;
  float exposure = 0;
  PreviewVolumeSettings settings;
  float near_plane = .01f, far_plane = 1e30f;
};
class PreviewVolumeRenderer {
 public:
  PreviewVolumeRenderer();
  ~PreviewVolumeRenderer();
  bool load(const ScenePreviewResult&, PreviewComputeBackend, uint64_t budget,
            std::string* error, const PreviewComputeOptions& options = {});
  bool render(const PreviewVolumeCamera&, int width, int height,
              std::vector<uint8_t>* rgba, std::string* error,
              bool advance = true);
  const char* backend() const;
  uint32_t accumulatedFrames() const;
  void bounds(double* lo, double* hi, double* cell) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
