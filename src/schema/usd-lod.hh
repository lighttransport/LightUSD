// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - usdLod heuristics (UsdLodDistanceHeuristicQuery and
// UsdLodOverrideAPI::ComputeLODOverride parity, OpenUSD 26.11).
#pragma once

#include <string>
#include <vector>

namespace lightusd {
class Stage;
class UsdPrim;

struct LodDistanceQuery {
  std::string lod_domain;
  double center[3] = {0, 0, 0};
  bool has_extent = false;  // from the first Boundable lod:boundingVolume target
  float extent_min[3] = {0, 0, 0}, extent_max[3] = {0, 0, 0};
  std::vector<float> thresholds, blend_thresholds;
};

/// UsdLodDistanceHeuristic::CreateDistanceHeuristicQuery at `time`.
bool CreateLodDistanceQuery(const Stage& stage, const UsdPrim& heuristic, double time,
                            LodDistanceQuery* out);

/// Distance from `viewpoint` to the clamp of the viewpoint into the (local)
/// extent, else to the center, under the row-vector `transform` (Gf layout);
/// 0 inside the extent. With hysteresis, lags `prev` by `hysteresis`.
double ComputeLodDistance(const LodDistanceQuery& q, const double viewpoint[3],
                          const double transform[16]);
double ComputeLodDistance(const LodDistanceQuery& q, const double viewpoint[3],
                          const double transform[16], double prev, double hysteresis);

/// LOD index for a distance: count of thresholds <= distance, with a
/// fractional blend inside [threshold, blendThreshold).
float ComputeLodIndex(const LodDistanceQuery& q, double distance);

/// A view frustum as GfFrustum exposes it: row-vector view and projection
/// matrices, the six bounding planes (normal, distance; inside is >= 0),
/// eye position, projection type, reference-plane depth and window size.
struct LodFrustum {
  double view[16], projection[16];
  double planes[6][4];
  double position[3];
  bool perspective = true;
  double reference_plane_depth = 1.0;
  double window_size[2] = {2.0, 2.0};
};

struct LodScreenSizeQuery {
  std::string lod_domain;
  bool has_extent = false;
  float extent_min[3] = {0, 0, 0}, extent_max[3] = {0, 0, 0};
  std::string projection_method = "projectedSphere";
  std::vector<float> thresholds, blend_thresholds;
};

/// UsdLodScreenSizeHeuristic::CreateScreenSizeHeuristicQuery: extent
/// attribute (with its schema fallback), then the first Boundable
/// boundingVolume target's extent; projectionMethod; thresholds.
bool CreateLodScreenSizeQuery(const Stage& stage, const UsdPrim& heuristic, double time,
                              LodScreenSizeQuery* out);
/// Fraction of the window covered: projectedSphere (circle of the extent's
/// bounding sphere) or projectedExtent (convex hull of the clipped, projected
/// box); 0 when culled.
double ComputeLodScreenSize(const LodScreenSizeQuery& q, const LodFrustum& f,
                            const double transform[16]);
/// LOD index for a screen size (thresholds descend).
float ComputeLodIndex(const LodScreenSizeQuery& q, double size);

/// UsdLodOverrideAPI::ComputeLODOverride: walks up to the first OverrideAPI
/// prim with an authored non-"inherited" mode; "indexedLOD" needs a readable
/// index (else keeps walking). Returns "noOverride" when none applies.
std::string ComputeLodOverride(const Stage& stage, const UsdPrim& prim, double time,
                               float* index);

}  // namespace lightusd
