// SPDX-License-Identifier: Apache-2.0
// App-side adapter over the shared preview photometry parser. The GL and RT
// paths consume raw LM-63 tables and normalize by maximum candela, so this file
// keeps that storage contract while delegating parsing (LM-63, TILT, limits) to
// src/tydra/next/scene-preview-photometry.cc: one parser for the app and the
// preview caches.
#include "lighting_ies.hh"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tydra/scene-preview-photometry.hh"

namespace lusdview {
namespace {

constexpr size_t kMaxIesCacheEntries = 32u;

struct CachedIesProfile {
  float maxCandela{0.0f};
  std::vector<float> verticalAngles;
  std::vector<float> horizontalAngles;
  std::vector<float> candela;
};

std::mutex& IesCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, CachedIesProfile>& IesCache() {
  static std::unordered_map<std::string, CachedIesProfile> cache;
  return cache;
}

void ResetProfile(DrawLightCPU* light) {
  light->iesValid = false;
  light->iesVerticalAngles.clear();
  light->iesHorizontalAngles.clear();
  light->iesCandela.clear();
  light->iesMaxCandela = 0.0f;
}

// Parse through the shared photometry parser, then bake TILT into the candela
// table: the shaders and EvaluateIesProfile below expect tilt applied at load,
// while the shared profile keeps it as a separate multiplier.
bool ReadText(const std::string& path, std::string* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size < 0 || size > (4 << 20)) return false;
  out->resize(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  return bool(file.read(out->data(), size));
}

std::string TrimLine(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                           line.back() == '\t'))
    line.pop_back();
  size_t start = 0;
  while (start < line.size() &&
         (line[start] == ' ' || line[start] == '\t'))
    ++start;
  return line.substr(start);
}

// The app historically accepted two TILT=INCLUDE layouts the shared parser does
// not need to support directly: a count-first internal table without LM-63's
// leading lamp-to-luminaire geometry field, and a quoted filename on the line
// after TILT=INCLUDE. Normalise both to standard LM-63 forms.
std::string NormalizeTilt(std::string text) {
  std::vector<std::string> lines;
  for (size_t start = 0; start <= text.size();) {
    const auto end = text.find('\n', start);
    lines.push_back(text.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    if (TrimLine(lines[i]) != "TILT=INCLUDE") continue;
    const std::string next =
        i + 1 < lines.size() ? TrimLine(lines[i + 1]) : std::string();
    if (!next.empty() && next.front() == '"') {
      lines[i] = "TILT=" + next;
      lines.erase(lines.begin() + static_cast<long>(i + 1));
    } else {
      lines.insert(lines.begin() + static_cast<long>(i + 1), "1");
    }
    std::string joined;
    for (size_t k = 0; k < lines.size(); ++k) {
      if (k) joined += '\n';
      joined += lines[k];
    }
    return joined;
  }
  return {};
}

bool BuildProfile(const std::string& path, CachedIesProfile* out,
                  std::string* err) {
  lightusd::tydra::next::PreviewIesProfile profile;
  std::string error;
  bool ok = lightusd::tydra::next::LoadPreviewIes(path, &profile, &error);
  if (!ok) {
    std::string text;
    const auto normalized =
        ReadText(path, &text) ? NormalizeTilt(text) : std::string();
    if (!normalized.empty() &&
        lightusd::tydra::next::ParsePreviewIes(normalized, &profile, &error)) {
      ok = true;
    } else if (!normalized.empty()) {
      // External TILT tables use the same count-first layout.
      ok = lightusd::tydra::next::ParsePreviewIes(
          normalized, &profile, &error,
          [&](const std::string& name, std::string* data, std::string* e) {
            const auto file = (std::filesystem::path(path).parent_path() /
                               name)
                                  .lexically_normal()
                                  .string();
            std::string raw;
            if (!ReadText(file, &raw)) {
              *e = "cannot load external IES TILT data";
              return false;
            }
            *data = "1\n" + raw;
            return true;
          });
    }
  }
  if (!ok) {
    if (err) *err = error.empty() ? "cannot load IES profile" : error;
    return false;
  }
  const size_t nv = profile.vertical.size();
  const size_t nh = profile.horizontal.size();
  if (!nv || !nh || profile.candela.size() != nv * nh) {
    if (err) *err = "invalid or unordered IES profile table";
    return false;
  }
  CachedIesProfile result;
  result.verticalAngles = profile.vertical;
  result.horizontalAngles = profile.horizontal;
  result.candela = profile.candela;
  // LM-63 type-1 half profiles (0..180) rely on rotational symmetry. The app
  // shaders and evaluator expect the wrap column at 360 with the h=0 values,
  // which is how the previous in-app parser stored them.
  if (profile.type == 1 && !result.horizontalAngles.empty() &&
      std::abs(result.horizontalAngles.back() - 180.0f) < 1.0e-3f) {
    result.horizontalAngles.push_back(360.0f);
    for (size_t v = 0; v < nv; ++v) result.candela.push_back(result.candela[v]);
  }
  if (!profile.tilt_angles.empty() && !profile.tilt_factors.empty()) {
    for (size_t h = 0; h < nh; ++h)
      for (size_t v = 0; v < nv; ++v)
        result.candela[h * nv + v] *= profile.tiltMultiplier(profile.vertical[v]);
  }
  for (const float value : result.candela)
    result.maxCandela = std::max(result.maxCandela, value);
  if (!(result.maxCandela > 0.0f)) {
    if (err) *err = "IES profile contains no positive candela values";
    return false;
  }
  *out = std::move(result);
  return true;
}

using SampleFn = float (*)(void*, size_t);

}  // namespace

bool LoadIesProfile(const std::string& path, DrawLightCPU* light,
                    std::string* err) {
  if (err) err->clear();
  if (!light) {
    if (err) *err = "empty IES profile path";
    return false;
  }
  // Loading is transactional from the caller's perspective: a reload after an
  // asset edit must never leave the previous profile active when the
  // replacement cannot be read.
  ResetProfile(light);
  if (path.empty()) {
    if (err) *err = "empty IES profile path";
    return false;
  }

  CachedIesProfile cached;
  {
    std::lock_guard<std::mutex> lock(IesCacheMutex());
    const auto it = IesCache().find(path);
    if (it != IesCache().end()) cached = it->second;
  }
  if (cached.candela.empty()) {
    if (!BuildProfile(path, &cached, err)) return false;
    std::lock_guard<std::mutex> lock(IesCacheMutex());
    auto& cache = IesCache();
    if (cache.size() >= kMaxIesCacheEntries && !cache.count(path))
      cache.erase(cache.begin());
    cache[path] = cached;
  }

  light->iesMaxCandela = cached.maxCandela;
  light->iesVerticalAngles = cached.verticalAngles;
  light->iesHorizontalAngles = cached.horizontalAngles;
  light->iesCandela = cached.candela;
  light->iesValid = true;
  return true;
}

namespace {

float InterpolateSamples(const std::vector<float>& x, float value,
                         void* context, SampleFn sample) {
  if (x.empty()) return 0.0f;
  if (x.size() == 1) return sample(context, 0);
  if (value <= x.front()) return sample(context, 0);
  if (value >= x.back()) return sample(context, x.size() - 1);
  const auto it = std::upper_bound(x.begin(), x.end(), value);
  const size_t hi = static_cast<size_t>(it - x.begin());
  const size_t lo = hi - 1;
  const float span = x[hi] - x[lo];
  const float t = span > 1.0e-8f ? (value - x[lo]) / span : 0.0f;
  const float loValue = sample(context, lo);
  return loValue + (sample(context, hi) - loValue) * t;
}

struct VerticalContext {
  const DrawLightCPU* light;
  size_t horizontalIndex;
  size_t verticalCount;
};

float VerticalSample(void* context, size_t index) {
  const auto* c = static_cast<const VerticalContext*>(context);
  return c->light->iesCandela[c->horizontalIndex * c->verticalCount + index];
}

struct HorizontalContext {
  const DrawLightCPU* light;
  float vertical;
};

float HorizontalSample(void* context, size_t index) {
  const auto* c = static_cast<const HorizontalContext*>(context);
  VerticalContext vertical{c->light, index,
                           c->light->iesVerticalAngles.size()};
  return InterpolateSamples(c->light->iesVerticalAngles, c->vertical, &vertical,
                            VerticalSample);
}

}  // namespace

float EvaluateIesProfile(const DrawLightCPU& light, float verticalDeg,
                         float horizontalDeg) {
  if (!light.iesValid || light.iesVerticalAngles.empty() ||
      light.iesHorizontalAngles.empty()) return 1.0f;
  const size_t nv = light.iesVerticalAngles.size();
  const size_t nh = light.iesHorizontalAngles.size();
  if (light.iesCandela.size() != nv * nh || light.iesMaxCandela <= 0.0f)
    return 1.0f;

  const float v = std::max(light.iesVerticalAngles.front(),
                           std::min(light.iesVerticalAngles.back(),
                                    std::fabs(verticalDeg)));
  float h = horizontalDeg;
  const float hMin = light.iesHorizontalAngles.front();
  const float hMax = light.iesHorizontalAngles.back();
  const float span = hMax - hMin;
  if (span > 1.0e-6f) {
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    // LM-63 profiles commonly author one half (0..180) or one quadrant (0..90)
    // and rely on rotational symmetry for the remainder.
    if (hMax <= 180.0f && h > 180.0f) h = 360.0f - h;
    if (h > hMax) h = hMax;
  }
  h = std::max(hMin, std::min(hMax, h));

  HorizontalContext context{&light, v};
  const float candela = InterpolateSamples(light.iesHorizontalAngles, h,
                                           &context, HorizontalSample);
  const float normalized =
      light.shapingIesNormalize ? candela / light.iesMaxCandela : candela;
  return std::max(0.0f, normalized *
      (light.shapingIesAngleScale > 0.0f ? light.shapingIesAngleScale : 1.0f));
}

}  // namespace lusdview
