// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Render Scene Converter Implementation

#include "safe-arithmetic.hh"
#include "../../next/layer/prim-spec.hh"
#include "next/core/path-expression-eval.hh"
#include "next/schema/usd-vol.hh"
#include "next/schema/usd-geom-camera.hh"
#include "render-converter-internal.hh"
#include "render-converter-assembly.hh"
#include "render-converter-material-finalize.hh"
#include "render-converter-light-linking.hh"
#include "next/schema/color-space.hh"
#include "next/eval/value-clip.hh"
#include "next/resolver/asset-resolver.hh"
#include "materialx.hh"
#include "next/layer/asset-anchor.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/usdPhysics.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/usd-skel.hh"
#include "next/types/type-info.hh"
#include "tydra/fast-mikktspace.hh"
#include "tydra/mikktspace-tangent.hh"
#include "tydra/shape-to-mesh.hh"
#include "tydra/tangent-quantize.hh"
#include "tsd/tinysubdiv.hh"
#include "external/mapbox/earcut/earcut.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <deque>
#include <memory>
#include <unordered_map>
#include <optional>
#include <set>
#include <sstream>
#include <iomanip>
#include <atomic>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include <unordered_set>

namespace lightusd {
namespace tydra {
namespace next {

using ::lightusd::next::Stage;
using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;


//
// Constructor / Destructor
//

ConverterConfig MakeHardenedConverterConfig(size_t max_memory) {
  ConverterConfig config;
  // Zero must never turn a hardened request into an unlimited mode.
  max_memory = std::max<size_t>(max_memory, 1);
  config.limits.max_resident_bytes = max_memory;
  config.execution.max_threads = 1;
  config.execution.max_in_flight_bytes = max_memory;
  config.execution.callback_concurrency =
      ::lightusd::next::CallbackConcurrency::Serialized;
  return config;
}

RenderSceneConverter::RenderSceneConverter(const ConverterConfig& config)
    : impl_(new Impl(config)) {}
RenderSceneConverter::~RenderSceneConverter() = default;

ConvertResult RenderSceneConverter::Convert(const ::lightusd::next::Stage& stage) {
  return impl_->Convert(stage);
}

StreamConvertResult RenderSceneConverter::ConvertToSink(::lightusd::next::Stage& stage,
                                    SceneSink* sink) {
  return impl_->ConvertToSink(stage, sink);
}

GeometryInfo RenderSceneConverter::GetGeometryInfo(const UsdPrim& prim, GeometryKind kind,
                               int32_t id) const {
  return impl_->GetGeometryInfo(prim, kind, id);
}

bool RenderSceneConverter::ConvertExtentProxy(const UsdPrim& prim, RenderMesh* out) {
  return impl_->ConvertExtentProxy(prim, out);
}

bool RenderSceneConverter::ConvertBoundsProxy(const UsdPrim& prim, const Float3& minimum,
                          const Float3& maximum, RenderMesh* out) {
  return impl_->ConvertBoundsProxy(prim, minimum, maximum, out);
}

bool RenderSceneConverter::ConvertRenderableMesh(const Stage& stage, const UsdPrim& prim,
                             RenderMesh* out) {
  return impl_->ConvertRenderableMesh(stage, prim, out);
}

bool RenderSceneConverter::ConvertMesh(const Stage& stage, const UsdPrim& prim, RenderMesh* out) {
  return impl_->ConvertMesh(stage, prim, out);
}

bool RenderSceneConverter::ConvertPoints(const Stage& stage, const UsdPrim& prim,
                     RenderPoints* out) {
  return impl_->ConvertPoints(stage, prim, out);
}

bool RenderSceneConverter::ConvertCurves(const UsdPrim& prim, RenderCurves* out) {
  return impl_->ConvertCurves(prim, out);
}

bool RenderSceneConverter::ConvertPointInstancer(const UsdPrim& prim, RenderPointInstancer* out) {
  return impl_->ConvertPointInstancer(prim, out);
}

bool RenderSceneConverter::ConvertMaterial(const ::lightusd::next::Stage& stage, const UsdPrim& prim, RenderMaterial* out) {
  return impl_->ConvertMaterial(stage, prim, out);
}

bool RenderSceneConverter::ConvertMaterial(const ::lightusd::next::Stage& stage,
                       const UsdPrim& prim, RenderMaterial* out,
                       RenderScene* scene) {
  return impl_->ConvertMaterial(stage, prim, out, scene);
}

bool RenderSceneConverter::ConvertLight(const ::lightusd::next::Stage& stage,
                                       const UsdPrim& prim, RenderLight* out) {
  return impl_->ConvertLight(stage, prim, out);
}

bool RenderSceneConverter::ConvertCamera(const ::lightusd::next::Stage& stage,
                     const UsdPrim& prim, RenderCamera* out) {
  return impl_->ConvertCamera(stage, prim, out);
}

bool RenderSceneConverter::ConvertSkeleton(const UsdPrim& prim, Skeleton* out) {
  return impl_->ConvertSkeleton(prim, out);
}

bool RenderSceneConverter::ConvertAnimation(const ::lightusd::next::Stage& stage,
                        const UsdPrim& prim, AnimationClip* out) {
  return impl_->ConvertAnimation(stage, prim, out);
}

bool RenderSceneConverter::TriangulateMesh(RenderMesh* mesh) {
  return impl_->TriangulateMesh(mesh);
}

bool RenderSceneConverter::LoadTexture(const std::string& asset_path, TextureImage* out) {
  return impl_->LoadTexture(asset_path, out);
}

std::string RenderSceneConverter::GetLastError() const {
  return impl_->GetLastError();
}

void RenderSceneConverter::SetTimeCode(double time_code) {
  impl_->SetTimeCode(time_code);
}

double RenderSceneConverter::GetTimeCode() const {
  return impl_->GetTimeCode();
}

RenderSceneConverter::Impl::Impl(const ConverterConfig& config)
    : config_(config) {}

RenderSceneConverter::Impl::~Impl() = default;

bool RenderSceneConverter::Impl::ConvertRenderableMesh(const Stage& stage,
                                                 const UsdPrim& prim,
                                                 RenderMesh* out) {
  if (!prim.IsValid() || !IsMeshRenderableTypeName(prim.GetTypeName())) {
    SetLastError("Invalid renderable mesh prim");
    return false;
  }
  return prim.GetTypeName() == "Mesh" ? ConvertMesh(stage, prim, out)
                                      : ConvertGeomPrimitive(prim, out);
}



// Cumulative memory guard for expensive conversion phases. This is an
// estimate-based early gate; ChunkAllocationBudget enforces actual retained
// chunk bytes independently for every concurrent conversion.


//
// Triangulation
//




//
// Camera conversion
//


//
// Skeleton conversion
//

bool RenderSceneConverter::Impl::LoadTexture(const std::string& asset_path, TextureImage* out) {
  if (!out) return false;

  // Use custom loader if provided
  if (config_.material.custom_texture_loader) {
    return config_.material.custom_texture_loader(asset_path, out);
  }

  // Built-in loader is metadata-only by design. Applications that need decoded
  // pixels should provide `MaterialConfig::custom_texture_loader`.
  out->resolved_path = asset_path;

  return true;
}

//
// Utility functions
//

void ComputeTriangleNormal(const float* p0, const float* p1, const float* p2, float* normal) {
  float e1[3] = {p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]};
  float e2[3] = {p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2]};

  normal[0] = e1[1]*e2[2] - e1[2]*e2[1];
  normal[1] = e1[2]*e2[0] - e1[0]*e2[2];
  normal[2] = e1[0]*e2[1] - e1[1]*e2[0];

  float len = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
  if (len > 1e-8f) {
    normal[0] /= len;
    normal[1] /= len;
    normal[2] /= len;
  }
}


}  // namespace next
}  // namespace tydra
}  // namespace lightusd
