// SPDX-License-Identifier: Apache-2.0
// Portable, authored light filters.
//
// A filter prim is bound from a light with `rel light:filters` (or `rel
// filters`) and carries a portable descriptor so that caches and every
// rendering backend agree without a plugin binary:
//
//   uniform token lightusd:filter:kind = "intensity"   # or "color"
//   float          lightusd:filter:intensity = 1       # intensity only
//   color3f        lightusd:filter:color = (1, 1, 1)   # color only
//
// Built-ins are receiver-independent and therefore fold into the light's
// effective intensity and color; a backend that applies them once at light
// setup inherits them everywhere. Additional kinds register a parser here.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
class Stage;
class UsdPrim;
}  // namespace next

namespace tydra {
namespace next {

struct LightFilter {
  std::string prim_path;
  std::string kind;
  float intensity = 1.0f;
  std::array<float, 3> color{1, 1, 1};
  bool operator==(const LightFilter& b) const {
    return prim_path == b.prim_path && kind == b.kind &&
           intensity == b.intensity && color == b.color;
  }
};

using LightFilterParser = std::function<bool(
    const ::lightusd::next::Stage&, const ::lightusd::next::UsdPrim&,
    double time_code, LightFilter*, std::string*)>;

// Registers the built-in `intensity` and `color` kinds. Idempotent and safe to
// call from multiple threads.
void RegisterBuiltinLightFilters();
// Returns false when the kind is already registered with another parser.
bool RegisterLightFilter(const std::string& kind, LightFilterParser parser);
bool UnregisterLightFilter(const std::string& kind);
std::vector<std::string> LightFilterKinds();
bool HasLightFilter(const std::string& kind);

// Parses one filter prim. On failure `error` explains why (missing or unknown
// `lightusd:filter:kind`, malformed or out-of-range parameters); callers keep
// the light unchanged and report a conversion warning.
bool ParseLightFilter(const ::lightusd::next::Stage& stage,
                      const ::lightusd::next::UsdPrim& prim, double time_code,
                      LightFilter* out, std::string* error);

// Folds receiver-independent filters into a light's effective intensity and
// color. Values are clamped at zero: a filter never makes a light negative.
void ApplyLightFilters(const std::vector<LightFilter>& filters,
                       float* intensity, std::array<float, 3>* color);

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
