// SPDX-License-Identifier: Apache-2.0
// lusdview — view-dependent district LOD pre-pass (--lod-stream), the lusdview
// counterpart of lusdrender's -lodStream. Whole-island districtLod=full does not
// fit one host/GPU, so this composes the scene cheaply in proxy LOD, ranks the
// districts by view importance from a camera, and promotes the nearest to the
// `full` variant under host-RSS / GPU-VRAM budgets via a generated wrapper layer.
// The viewer then loads the wrapper (through the --core path).
#pragma once

#include <string>

namespace lusdview {

struct LodStreamOptions {
  std::string container = "mp_wz_island_geo";  // districts are its direct children
  std::string camera;        // USD camera name to rank against ("" = scene centre)
  double maxMemGiB = 0.0;     // host budget; 0 = 50% of MemAvailable
  double maxVramGiB = 0.0;    // GPU budget; 0 = 50% of device VRAM (HIP query)
  double districtMemGiB = 10.0;   // flat per-district host charge
  double districtVramGiB = 3.0;   // flat per-district VRAM charge
  double minVerts = 1000.0;  // skip trivial districts (triggers/volumes/bounds)
  double time = 0.0;         // timecode for camera + composition
};

// Compose the proxy scene, rank/promote districts under the budgets, and write a
// wrapper .usda next to the temp dir. Returns the wrapper path to load instead of
// `input`, or "" on any failure (the caller should then load `input` unchanged).
std::string PrepareLodStream(const std::string& input, const LodStreamOptions& o);

}  // namespace lusdview
