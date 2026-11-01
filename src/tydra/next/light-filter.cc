// SPDX-License-Identifier: Apache-2.0
// Portable, authored light filters (see light-filter.hh).
#include "light-filter.hh"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>

#include "next/eval/attribute-eval.hh"
#include "next/stage/stage.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace {

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, LightFilterParser>& Registry() {
  static std::map<std::string, LightFilterParser> registry;
  return registry;
}

bool EvalFloat(const ::lightusd::next::AttributeEval& eval,
               const ::lightusd::next::UsdPrim& prim, const char* name,
               float* out) {
  if (const auto result = eval.EvalFloat(prim, name)) {
    *out = *result;
    return true;
  }
  if (const auto result = eval.EvalDouble(prim, name)) {
    *out = static_cast<float>(*result);
    return true;
  }
  return false;
}

bool EvalColor(const ::lightusd::next::AttributeEval& eval,
               const ::lightusd::next::UsdPrim& prim, const char* name,
               std::array<float, 3>* out) {
  float value[3];
  if (eval.EvalFloat3(prim, name, value)) {
    *out = {value[0], value[1], value[2]};
    return true;
  }
  double wide[3];
  if (eval.EvalDouble3(prim, name, wide)) {
    *out = {static_cast<float>(wide[0]), static_cast<float>(wide[1]),
            static_cast<float>(wide[2])};
    return true;
  }
  return false;
}

bool ParseIntensity(const ::lightusd::next::Stage& stage,
                    const ::lightusd::next::UsdPrim& prim, double time_code,
                    LightFilter* out, std::string* error) {
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  float intensity = 1.0f;
  if (EvalFloat(eval, prim, "lightusd:filter:intensity", &intensity) &&
      (!std::isfinite(intensity) || intensity < 0.0f)) {
    *error = "invalid lightusd:filter:intensity";
    return false;
  }
  // A portable preview is not a place for unbounded amplifiers.
  out->intensity = std::min(std::max(0.0f, intensity), 64.0f);
  return true;
}

bool ParseColor(const ::lightusd::next::Stage& stage,
                const ::lightusd::next::UsdPrim& prim, double time_code,
                LightFilter* out, std::string* error) {
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  std::array<float, 3> color{1, 1, 1};
  if (EvalColor(eval, prim, "lightusd:filter:color", &color)) {
    for (float channel : color) {
      if (!std::isfinite(channel) || channel < 0.0f) {
        *error = "invalid lightusd:filter:color";
        return false;
      }
    }
  }
  for (float& channel : color) channel = std::min(channel, 16.0f);
  out->color = color;
  return true;
}

}  // namespace

void RegisterBuiltinLightFilters() {
  RegisterLightFilter("intensity", ParseIntensity);
  RegisterLightFilter("color", ParseColor);
}

bool RegisterLightFilter(const std::string& kind, LightFilterParser parser) {
  if (kind.empty() || !parser) return false;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto inserted = Registry().emplace(kind, std::move(parser));
  return inserted.second;
}

bool UnregisterLightFilter(const std::string& kind) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  return Registry().erase(kind) != 0;
}

std::vector<std::string> LightFilterKinds() {
  RegisterBuiltinLightFilters();
  std::vector<std::string> kinds;
  std::lock_guard<std::mutex> lock(RegistryMutex());
  kinds.reserve(Registry().size());
  for (const auto& entry : Registry()) kinds.push_back(entry.first);
  return kinds;
}

bool HasLightFilter(const std::string& kind) {
  RegisterBuiltinLightFilters();
  std::lock_guard<std::mutex> lock(RegistryMutex());
  return Registry().find(kind) != Registry().end();
}

bool ParseLightFilter(const ::lightusd::next::Stage& stage,
                      const ::lightusd::next::UsdPrim& prim, double time_code,
                      LightFilter* out, std::string* error) {
  if (!out) return false;
  RegisterBuiltinLightFilters();
  out->prim_path = prim.GetPath().str();
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  std::string kind;
  if (const auto token = eval.EvalToken(prim, "lightusd:filter:kind"))
    kind = *token;
  else if (const auto text = eval.EvalString(prim, "lightusd:filter:kind"))
    kind = *text;
  if (kind.empty()) {
    *error = "missing lightusd:filter:kind";
    return false;
  }
  LightFilterParser parser;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(kind);
    if (found == Registry().end()) {
      *error = "unknown light filter kind '" + kind + "'";
      return false;
    }
    parser = found->second;
  }
  out->kind = kind;
  return parser(stage, prim, time_code, out, error);
}

void ApplyLightFilters(const std::vector<LightFilter>& filters,
                       float* intensity, std::array<float, 3>* color) {
  if (intensity) {
    for (const auto& filter : filters)
      *intensity *= std::max(0.0f, filter.intensity);
  }
  if (color) {
    for (const auto& filter : filters)
      for (size_t channel = 0; channel < color->size(); ++channel)
        (*color)[channel] *= std::max(0.0f, filter.color[channel]);
  }
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
