// SPDX-License-Identifier: Apache-2.0
// Backend-neutral controls for lusdview's production path tracer.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lusdview {

enum class PathTraceQuality : uint8_t { Interactive = 0, Final = 1 };
enum class PathTraceDenoise : uint8_t { Off = 0, Auto = 1, On = 2 };

struct PathTraceSettings {
  bool enabled{false};
  PathTraceQuality quality{PathTraceQuality::Interactive};
  PathTraceDenoise denoise{PathTraceDenoise::Auto};
  uint32_t targetSamples{0};
  uint32_t minSamples{8};
  uint32_t maxDepth{6};
  uint32_t russianRouletteDepth{3};
  uint32_t maxSubsurfaceEvents{16};
  uint32_t maxVolumeEvents{16};
  uint32_t motionSegments{2};
  // True when --pt-motion-segments was given. Otherwise, when path tracing,
  // an authored camera shutter raises motionSegments to kShutterMotionSegments (two segments
  // ghost visibly), with or without Gaussian splats.
  bool motionSegmentsExplicit{false};
  static constexpr uint32_t kShutterMotionSegments = 16;
  uint32_t seed{1};
  float varianceThreshold{0.02f};
  // Display-side robust-deviation threshold for isolated luminance samples.
  // Zero disables suppression. Scene-linear EXR output always retains the
  // unbiased samples.
  float fireflyClamp{8.0f};

  static PathTraceSettings Interactive() { return PathTraceSettings{}; }

  static PathTraceSettings Final() {
    PathTraceSettings s;
    s.quality = PathTraceQuality::Final;
    s.denoise = PathTraceDenoise::On;
    s.targetSamples = 1024;
    s.minSamples = 64;
    s.maxDepth = 12;
    s.russianRouletteDepth = 5;
    s.maxSubsurfaceEvents = 64;
    s.maxVolumeEvents = 64;
    s.motionSegments = 8;
    s.varianceThreshold = 0.005f;
    return s;
  }

  void sanitize() {
    maxDepth = std::max(1u, std::min(maxDepth, 64u));
    russianRouletteDepth = std::min(russianRouletteDepth, maxDepth);
    maxSubsurfaceEvents = std::max(1u, std::min(maxSubsurfaceEvents, 256u));
    maxVolumeEvents = std::max(1u, std::min(maxVolumeEvents, 256u));
    motionSegments = std::max(1u, std::min(motionSegments, 64u));
    minSamples = std::max(1u, minSamples);
    varianceThreshold = std::max(0.0f, varianceThreshold);
    if (!std::isfinite(fireflyClamp)) fireflyClamp = 8.0f;
    fireflyClamp = std::max(0.0f, std::min(fireflyClamp, 1024.0f));
  }
};

inline const char* PathTraceQualityLabel(PathTraceQuality quality) {
  return quality == PathTraceQuality::Final ? "final" : "interactive";
}

inline const char* PathTraceDenoiseLabel(PathTraceDenoise mode) {
  switch (mode) {
    case PathTraceDenoise::Off: return "off";
    case PathTraceDenoise::Auto: return "auto";
    case PathTraceDenoise::On: return "on";
  }
  return "off";
}

// Relative RMS change between two scene-linear RGBA images. Alpha is ignored.
// Used as a backend-neutral batch convergence criterion; an empty/mismatched
// history deliberately reports infinity so the first checkpoint never stops.
inline double PathTraceRelativeChange(const std::vector<float>& current,
                                      const std::vector<float>& previous) {
  if (current.size() != previous.size() || current.empty())
    return std::numeric_limits<double>::infinity();
  double delta2 = 0.0;
  double signal2 = 0.0;
  for (size_t i = 0; i + 2 < current.size(); i += 4) {
    for (size_t channel = 0; channel < 3; ++channel) {
      const double value = current[i + channel];
      const double delta = value - previous[i + channel];
      delta2 += delta * delta;
      signal2 += value * value;
    }
  }
  return std::sqrt(delta2 / std::max(signal2, 1.0e-20));
}

}  // namespace lusdview
