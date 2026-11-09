// SPDX-License-Identifier: Apache-2.0
// Geometry foundation (extent/size estimation) shared by the retained and
// streaming conversion drivers. Split from render-converter.cc.
#include "core/safe-arithmetic.hh"
#include "layer/prim-spec.hh"
#include "../core/path-expression-eval.hh"
#include "schema/usd-vol.hh"
#include "schema/usd-geom-camera.hh"
#include "render-converter-internal.hh"
#include "render-converter-prop-ids.hh"
#include "render-converter-assembly.hh"
#include "render-converter-material-finalize.hh"
#include "render-converter-light-linking.hh"
#include "schema/color-space.hh"
#include "eval/value-clip.hh"
#include "resolver/asset-resolver.hh"
#include "materialx.hh"
#include "layer/asset-anchor.hh"
#include "resolver/asset-resolver.hh"
#include "schema/usdPhysics.hh"
#include "schema/usd-shade.hh"
#include "schema/usd-skel.hh"
#include "types/type-info.hh"
#include "fast-mikktspace.hh"
#include "mikktspace-tangent.hh"
#include "shape-to-mesh.hh"
#include "tangent-quantize.hh"
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

namespace {

















size_t AuthoredArraySize(const UsdPrim& prim,
                        const ::lightusd::next::PropNameId& name_id) {
  const Value* value = prim.GetPropertyValue(name_id);
  return value && value->is_array() ? value->array_size() : 0;
}

bool HasAuthoredExtent(const UsdPrim& prim) {
  return AuthoredArraySize(prim, kIdExtent()) >= 2;
}
}  // namespace

GeometryInfo BuildGeometryInfo(const UsdPrim& prim, GeometryKind kind,
                               int32_t id) {
  GeometryInfo info;
  info.kind = kind;
  info.id = id;
  info.prim_path = prim.GetPath().str();
  info.type_name = prim.GetTypeName();
  info.point_count = AuthoredArraySize(prim, kIdPoints());
  info.has_authored_extent = HasAuthoredExtent(prim);

  size_t estimate = 0;
  if (kind == GeometryKind::Mesh) {
    info.index_count = AuthoredArraySize(prim, kIdFaceVertexIndices());
    // Account for decoded source arrays, render arrays, triangulation, and
    // face-varying remaps. This intentionally errs high for residency policy.
    estimate = SaturatingMul(info.point_count, 24);
    estimate = SaturatingAdd(estimate,
                             SaturatingMul(info.index_count, 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdFaceVertexCounts()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdNormals()), 24));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdPrimvarsSt()), 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 24));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdTetVertexIndices()), 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdHoleIndices()), 8));
  } else if (kind == GeometryKind::Points) {
    estimate = SaturatingMul(info.point_count, 48);
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdWidths()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 16));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
  } else {
    estimate = SaturatingMul(info.point_count, 40);
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdCurveVertexCounts()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdWidths()), 12));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 20));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
  }
  info.estimated_resident_bytes = estimate;
  return info;
}

GeometryInfo RenderSceneConverter::Impl::GetGeometryInfo(const UsdPrim& prim,
                                                    GeometryKind kind,
                                                    int32_t id) const {
  return BuildGeometryInfo(prim, kind, id);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
