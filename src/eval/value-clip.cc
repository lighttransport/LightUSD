// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "core/native-io.hh"
#include "value-clip.hh"
#include "layer/prim-spec.hh"
#include "layer/asset-anchor.hh"
#include "resolver/asset-resolver.hh"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
#include <filesystem>
#else
#include <sys/stat.h>
#endif
#endif

namespace lightusd {
namespace next {

namespace {

/// File identity used to decide whether a cached clip stage is still current.
struct FileStamp {
  bool valid = false;
  int64_t mtime_ticks = 0;
  uint64_t size_bytes = 0;
  bool matches(const FileStamp& other) const {
    return valid && other.valid && mtime_ticks == other.mtime_ticks &&
           size_bytes == other.size_bytes;
  }
};

FileStamp StampFile(const std::string& path) {
  if (!::lightusd::NativeIOEnabled()) return {};

  FileStamp stamp;
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  // One stat() per check: this runs on every revalidating cache hit.
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return stamp;
  stamp.valid = true;
#if defined(__APPLE__)
  stamp.mtime_ticks = int64_t(st.st_mtimespec.tv_sec) * 1000000000LL +
                      int64_t(st.st_mtimespec.tv_nsec);
#else
  stamp.mtime_ticks = int64_t(st.st_mtim.tv_sec) * 1000000000LL +
                      int64_t(st.st_mtim.tv_nsec);
#endif
  stamp.size_bytes = static_cast<uint64_t>(st.st_size);
#elif !defined(__EMSCRIPTEN__)
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return stamp;
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) return stamp;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return stamp;
  stamp.valid = true;
  stamp.mtime_ticks = static_cast<int64_t>(mtime.time_since_epoch().count());
  stamp.size_bytes = static_cast<uint64_t>(size);
#else
  (void)path;
#endif
  return stamp;
}

constexpr size_t kMaxTemplateClipCount = 1000000;

bool ToDouble(ValueView value, double* out) {
  if (!value || !out) return false;
  if (const double* v = value->as_double()) *out = *v;
  else if (const float* v = value->as_float()) *out = *v;
  else if (const int32_t* v = value->as_int()) *out = *v;
  else if (const int64_t* v = value->as_int64()) *out = static_cast<double>(*v);
  else return false;
  return true;
}

bool ToString(ValueView value, std::string* out) {
  if (!value || !out) return false;
  if (auto v = value->as_string()) *out = *v;
  else if (auto v = value->as_token()) *out = *v;
  else if (auto v = value->as_asset_path()) *out = *v;
  else return false;
  return true;
}

std::vector<std::pair<double, double>> PairArray(ValueView value) {
  std::vector<std::pair<double, double>> out;
  if (!value || !value->is_array()) return out;
  if (auto flat = value->as_double_array()) {
    for (size_t i = 0; i + 1 < flat->size(); i += 2)
      out.emplace_back((*flat)[i], (*flat)[i + 1]);
  } else if (auto flat = value->as_float_array()) {
    for (size_t i = 0; i + 1 < flat->size(); i += 2)
      out.emplace_back((*flat)[i], (*flat)[i + 1]);
  }
  return out;
}

bool ExpandTemplate(DictView  dict, ValueClipSet* out, std::string* error) {
  if (!out) return false;
  std::string pattern;
  if (!ToString(dict.find("templateAssetPath"), &pattern)) {
    if (error) *error = "Invalid templateAssetPath";
    return false;
  }
  double start = 0.0, end = 0.0, stride = 1.0, active_offset = 0.0;
  if (!ToDouble(dict.find("templateStartTime"), &start) ||
      !ToDouble(dict.find("templateEndTime"), &end)) {
    if (error) *error = "Missing value-clip template time range";
    return false;
  }
  auto stride_value = dict.find("templateStride");
  if (stride_value && !ToDouble(stride_value, &stride)) {
    if (error) *error = "Invalid templateStride";
    return false;
  }
  auto offset_value = dict.find("templateActiveOffset");
  if (offset_value && !ToDouble(offset_value, &active_offset)) {
    if (error) *error = "Invalid templateActiveOffset";
    return false;
  }
  if (!std::isfinite(start) || !std::isfinite(end) ||
      !std::isfinite(stride) || stride <= 0.0 || end < start) {
    if (error) *error = "Invalid value-clip template time range";
    return false;
  }
  if (!std::isfinite(active_offset) ||
      std::fabs(active_offset) > stride) {
    if (error) *error = "Invalid templateActiveOffset";
    return false;
  }
  const size_t first = pattern.find('#');
  if (first == std::string::npos) {
    if (error) *error = "templateAssetPath has no # placeholder";
    return false;
  }
  size_t integer_end = first;
  while (integer_end < pattern.size() && pattern[integer_end] == '#')
    ++integer_end;
  size_t fraction = std::string::npos, fraction_end = std::string::npos;
  if (integer_end + 1 < pattern.size() && pattern[integer_end] == '.' &&
      pattern[integer_end + 1] == '#') {
    fraction = integer_end + 1;
    fraction_end = fraction;
    while (fraction_end < pattern.size() && pattern[fraction_end] == '#')
      ++fraction_end;
  }
  const size_t placeholder_end =
      fraction == std::string::npos ? integer_end : fraction_end;
  if (pattern.find('#', placeholder_end) != std::string::npos) {
    if (error) *error = "templateAssetPath has invalid # placeholders";
    return false;
  }
  const size_t integer_width = integer_end - first;
  const size_t fractional_width =
      fraction == std::string::npos ? 0 : fraction_end - fraction;
  if (integer_width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      fractional_width >
          static_cast<size_t>(std::numeric_limits<int>::max())) {
    if (error) *error = "templateAssetPath placeholder is too wide";
    return false;
  }
  const std::string prefix = pattern.substr(0, first);
  const std::string suffix = fraction == std::string::npos
                                 ? pattern.substr(integer_end)
                                 : pattern.substr(fraction_end);

  // NOTE: this used to compute span/step_count/tolerance/boundary_*/active_*
  // in `long double` for extra headroom before the overflow/tolerance checks
  // below. That headroom is real on GCC/Clang/x86 (80-bit extended
  // precision) but MSVC's `long double` is bit-identical to `double` -- the
  // intended safety margin silently vanished there, so the same USD file
  // could round/reject differently per compiler. Do the checks in plain
  // `double` instead, made correct by construction: IEEE-754 already turns
  // an overflowing add/sub/div into +-inf (caught by std::isfinite) rather
  // than silently wrapping, and the boundary/active overflow guard uses an
  // explicit pre-check (the standard `a > MAX - b` idiom) instead of
  // computing-then-comparing-to-max, so no compiler-specific extra
  // precision is required for correctness on any platform.
  const double span = end - start;
  const double step_count = span / stride;
  const double rounded_steps = std::round(step_count);
  const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() *
                            std::max(1.0, std::fabs(step_count));
  const double normalized_steps =
      std::fabs(step_count - rounded_steps) <= tolerance
          ? rounded_steps
          : std::floor(step_count);
  if (!std::isfinite(step_count) || normalized_steps < 0.0 ||
      normalized_steps >= static_cast<double>(kMaxTemplateClipCount)) {
    if (error) *error = "Value-clip template exceeds expansion limit";
    return false;
  }
  const double kMaxD = std::numeric_limits<double>::max();
  const double absolute_offset = std::fabs(active_offset);
  auto safe_add = [kMaxD](double a, double b, double* out) -> bool {
    if (b > 0.0 && a > kMaxD - b) return false;
    if (b < 0.0 && a < -kMaxD - b) return false;
    *out = a + b;
    return std::isfinite(*out);
  };
  double boundary_start, boundary_end, active_start, active_end;
  if (!safe_add(start, -absolute_offset, &boundary_start) ||
      !safe_add(end, absolute_offset, &boundary_end) ||
      !safe_add(start, active_offset, &active_start) ||
      !safe_add(end, active_offset, &active_end)) {
    if (error) *error = "Value-clip template time overflow";
    return false;
  }
  const size_t clip_count = static_cast<size_t>(normalized_steps) + 1;
  out->asset_paths.clear();
  out->times.clear();
  out->active.clear();
  out->asset_paths.reserve(clip_count);
  out->times.reserve(clip_count + (active_offset == 0.0 ? 0 : 2));
  out->active.reserve(clip_count);

  for (size_t index = 0; index < clip_count; ++index) {
    double t = start + static_cast<double>(index) * stride;
    const double time_tolerance = tolerance * std::fabs(stride);
    if (t > end && (t - end) <= time_tolerance) {
      t = end;
    }
    const double integer_part = std::trunc(t);
    std::string integer_text;
    std::string fractional_text;
    if (fraction != std::string::npos) {
      std::ostringstream time_digits;
      time_digits << std::fixed
                  << std::setprecision(static_cast<int>(fractional_width))
                  << std::fabs(t);
      const std::string time_text = time_digits.str();
      const size_t dot = time_text.find('.');
      integer_text = time_text.substr(0, dot);
      fractional_text =
          dot == std::string::npos ? std::string(fractional_width, '0')
                                   : time_text.substr(dot + 1);
    } else {
      std::ostringstream integer_digits;
      integer_digits << std::fixed << std::setprecision(0)
                     << std::fabs(integer_part);
      integer_text = integer_digits.str();
    }
    if (integer_text.size() < integer_width) {
      integer_text.insert(0, integer_width - integer_text.size(), '0');
    }
    std::ostringstream path;
    path << prefix;
    const bool negative = fraction == std::string::npos
                              ? integer_part < 0.0
                              : std::signbit(t) && t != 0.0;
    if (negative) path << '-';
    path << integer_text;
    if (fraction != std::string::npos) {
      path << '.' << fractional_text;
    }
    path << suffix;
    out->asset_paths.push_back(path.str());
    out->times.emplace_back(t, t);
    out->active.emplace_back(t + active_offset, static_cast<int>(index));
  }
  if (active_offset != 0.0 && !out->times.empty()) {
    const double offset = std::fabs(active_offset);
    out->times.insert(out->times.begin(), {start - offset, start - offset});
    out->times.emplace_back(end + offset, end + offset);
  }
  return !out->asset_paths.empty();
}

}  // namespace

namespace value_clip_detail {

int ActiveIndex(const ValueClipSet& set, double time) {
  if (set.active.empty()) return set.asset_paths.empty() ? -1 : 0;
  int result = set.active.front().second;
  for (const auto& entry : set.active) {
    if (entry.first > time) break;
    result = entry.second;
  }
  return result;
}

double ClipTime(const ValueClipSet& set, double time) {
  if (set.times.empty()) return time;
  // Stage times strictly OUTSIDE the authored `times` range clamp to the
  // boundary CLIP time (spec L6983-6990, OpenUSD Usd_Clip).
  if (time < set.times.front().first) return set.times.front().second;
  if (time > set.times.back().first) return set.times.back().second;
  auto hi = std::lower_bound(
      set.times.begin(), set.times.end(), time,
      [](const std::pair<double, double>& p, double t) { return p.first < t; });
  if (hi != set.times.end() && hi->first == time) {
    // At a jump, the last authored mapping at the exact stage time wins.
    auto after = std::upper_bound(
        hi, set.times.end(), time,
        [](double t, const std::pair<double, double>& p) { return t < p.first; });
    return std::prev(after)->second;
  }
  if (hi == set.times.begin()) return hi->second;
  if (hi == set.times.end()) return set.times.back().second;
  const auto& b = *hi;       // first mapping at the next stage time
  const auto& a = *std::prev(hi);
  const double alpha = (time - a.first) / (b.first - a.first);
  return a.second + (b.second - a.second) * alpha;
}

}  // namespace value_clip_detail

namespace {
using value_clip_detail::ActiveIndex;
using value_clip_detail::ClipTime;
}  // namespace

bool ParseValueClipSets(const UsdPrim& prim, std::vector<ValueClipSet>* out,
                        std::string* error) {
  if (!out || !prim.GetPrimSpec()) return false;
  auto clips = prim.GetPrimSpec()->meta().clips().as_dictionary();
  if (!clips) return false;
  for (const auto& entry : clips->entries()) {
    auto dict = entry.second.as_dictionary();
    if (!dict) continue;
    ValueClipSet set;
    set.name = entry.first;
    if (dict->find("templateAssetPath")) {
      if (!ExpandTemplate(*dict, &set, error)) return false;
    } else {
      if (auto assets = dict->find("assetPaths")) {
        if (auto paths = assets->as_token_array())
          set.asset_paths.assign(paths->begin(), paths->end());
      }
      set.times = PairArray(dict->find("times"));
      for (const auto& pair : PairArray(dict->find("active"))) {
        if (!std::isfinite(pair.first) || !std::isfinite(pair.second) ||
            std::floor(pair.second) != pair.second || pair.second < 0.0 ||
            pair.second > static_cast<double>(std::numeric_limits<int>::max())) {
          if (error) *error = "Invalid value-clip active entry in set `" +
                              set.name + "`";
          return false;
        }
        set.active.emplace_back(pair.first, static_cast<int>(pair.second));
      }
    }
    ToString(dict->find("primPath"), &set.prim_path);
    if (!ToString(dict->find("manifestAssetPath"),
                  &set.manifest_asset_path)) {
      ToString(dict->find("manifestPath"), &set.manifest_asset_path);
    }
    auto interpolate = dict->find("interpolateMissingClipValues");
    if (interpolate && interpolate->as_bool())
      set.interpolate_missing = *interpolate->as_bool();
    const uint32_t anchor_id = prim.GetPrimSpec()->asset_anchor_id();
    if (AssetResolver::IsPackagePath(AssetAnchorPath(anchor_id))) {
      for (auto& path : set.asset_paths) path = ResolveAssetAnchorPath(path, anchor_id);
      if (!set.manifest_asset_path.empty())
        set.manifest_asset_path = ResolveAssetAnchorPath(set.manifest_asset_path, anchor_id);
    }
    for (const std::string& path : set.asset_paths) {
      if (path.empty()) {
        if (error) *error = "Empty value-clip asset path in set `" + set.name + "`";
        return false;
      }
    }
    for (const auto& pair : set.times) {
      if (!std::isfinite(pair.first) || !std::isfinite(pair.second)) {
        if (error) *error = "Non-finite value-clip time mapping in set `" +
                            set.name + "`";
        return false;
      }
    }
    if (set.active.empty() && !set.asset_paths.empty())
      set.active.emplace_back(0.0, 0);
    std::stable_sort(set.active.begin(), set.active.end(),
                     [](const std::pair<double, int>& a,
                        const std::pair<double, int>& b) {
                       return a.first < b.first;
                     });
    for (size_t i = 1; i < set.active.size(); ++i) {
      if (set.active[i - 1].first == set.active[i].first) {
        if (error) *error = "Duplicate value-clip active time in set `" +
                            set.name + "`";
        return false;
      }
    }
    // Jump discontinuity: two consecutive authored entries with the SAME
    // stage time mean "clip time approaches the first entry's value up to
    // (not including) the stage time, then jumps to the second's". Use a
    // stable sort on the time key to preserve the original order of equal-
    // time entries, avoiding mutation of the authored data.
    std::stable_sort(set.times.begin(), set.times.end(),
                     [](const std::pair<double, double>& a,
                        const std::pair<double, double>& b) {
                       return a.first < b.first;
                     });
    for (const auto& active : set.active) {
      if (active.second < 0 ||
          static_cast<size_t>(active.second) >= set.asset_paths.size()) {
        if (error) *error = "Value-clip active index is out of range in set `" +
                            set.name + "`";
        return false;
      }
    }
    if (!set.asset_paths.empty()) out->push_back(std::move(set));
  }
  // Dictionary storage order is not strength order. Use name order as the
  // deterministic weaker baseline, then apply the separately-authored
  // clipSets list-op to select/order strongest-to-weakest traversal.
  std::sort(out->begin(), out->end(),
            [](const ValueClipSet& a, const ValueClipSet& b) {
              return a.name < b.name;
            });
  const StringListOps& edits = prim.GetPrimSpec()->meta().clipSetEdits();
  if (edits.authored) {
    std::vector<std::string> weaker;
    weaker.reserve(out->size());
    for (const ValueClipSet& set : *out) weaker.push_back(set.name);
    const std::vector<std::string> order = ApplyStringListOp(edits, weaker);
    std::vector<ValueClipSet> ordered;
    ordered.reserve(out->size());
    for (const std::string& name : order) {
      auto it = std::find_if(out->begin(), out->end(), [&](const ValueClipSet& s) {
        return s.name == name;
      });
      if (it != out->end()) ordered.push_back(std::move(*it));
    }
    *out = std::move(ordered);
  }
  return !out->empty();
}

bool ResolveValueClip(const UsdPrim& prim, const std::string& property,
                      double stage_time, const ValueClipStageLoader& loader,
                      Value* out, std::string* source_asset,
                      std::string* error, std::string* source_clip_set,
                      ValueClipStageCache* stage_cache,
                      ValueClipResolutionInfo* resolution_info,
                      TimeInterpolation interpolation) {
  std::vector<ValueClipSet> sets;
  if (!ParseValueClipSets(prim, &sets, error)) return false;
  return ResolveValueClipFromSets(sets, prim, property, stage_time, loader,
                                  out, source_asset, error, source_clip_set,
                                  stage_cache, resolution_info, interpolation);
}

bool ResolveValueClipFromSets(const std::vector<ValueClipSet>& sets,
                              const UsdPrim& prim, const std::string& property,
                              double stage_time,
                              const ValueClipStageLoader& loader, Value* out,
                              std::string* source_asset, std::string* error,
                              std::string* source_clip_set,
                              ValueClipStageCache* stage_cache,
                              ValueClipResolutionInfo* resolution_info,
                      TimeInterpolation interpolation) {
  if (!out || sets.empty()) return false;
  if (!loader) {
    if (error) *error = "Value clips require a clip_stage_loader";
    return false;
  }
  ValueClipStageCache query_cache;
  ValueClipStageCache* cache = stage_cache ? stage_cache : &query_cache;
  // Nested-resolution state is per call path, so it cannot live in a cache that
  // concurrent queries may share.
  thread_local std::vector<std::string> resolution_stack;
  // A cache may opt into file-stamp revalidation so a rewritten clip is
  // observed by the next query, matching the per-query loader the cache
  // replaces. Without it the caller owns freshness. Entries without a stamp are
  // always trusted: pre-seeded caches (tydra's bake) loaded them themselves.
  const bool revalidate = cache->revalidate_file_stamps;
  // Insert/refresh one entry, evicting the least recently used one when the
  // cache is full. Callers hold a shared_ptr for the duration of their query,
  // so eviction never invalidates a stage that is still in use.
  // `evict` is false for negative entries: a failing clip must not displace a
  // successfully parsed one.
  auto store = [&](const std::string& key, ValueClipStageCache::Entry entry,
                   bool evict = true) {
    std::lock_guard<std::mutex> lock(cache->mutex);
    entry.use_tick = ++cache->clock;
    auto found = cache->entries.find(key);
    if (found != cache->entries.end()) {
      found->second = std::move(entry);
      return;
    }
    if (cache->max_entries == 0) return;
    if (cache->entries.size() >= cache->max_entries) {
      if (!evict) return;
      auto oldest = cache->entries.begin();
      for (auto it = cache->entries.begin(); it != cache->entries.end(); ++it) {
        if (it->second.use_tick < oldest->second.use_tick) oldest = it;
      }
      cache->entries.erase(oldest);
    }
    cache->entries.emplace(key, std::move(entry));
  };
  auto load_stage = [&](const std::string& asset) -> std::shared_ptr<Stage> {
    const FileStamp before = revalidate ? StampFile(asset) : FileStamp{};
    {
      std::lock_guard<std::mutex> lock(cache->mutex);
      auto found = cache->entries.find(asset);
      if (found != cache->entries.end()) {
        ValueClipStageCache::Entry& entry = found->second;
        const bool current = !entry.stamped || !revalidate ||
            (before.valid && entry.mtime_ticks == before.mtime_ticks &&
             entry.size_bytes == before.size_bytes);
        if (current) {
          entry.use_tick = ++cache->clock;
          if (!entry.stage && error && error->empty()) {
            *error = entry.error;
          }
          return entry.stage;
        }
      }
    }
    std::shared_ptr<Stage> stage = std::make_shared<Stage>();
    std::string warn, load_error;
    if (!loader(asset, stage.get(), &warn, &load_error)) {
      std::string message = "Failed to load value-clip asset `" + asset + "`";
      if (!load_error.empty()) message += ": " + load_error;
      // A revalidating cache only records a failure it can later invalidate.
      const bool cacheable = !revalidate || before.valid;
      if (cacheable) {
        ValueClipStageCache::Entry entry;
        entry.error = message;
        entry.stamped = revalidate;
        entry.mtime_ticks = before.mtime_ticks;
        entry.size_bytes = before.size_bytes;
        store(asset, std::move(entry), /*evict=*/false);
      }
      if (error && error->empty()) *error = std::move(message);
      return nullptr;
    }
    bool cacheable = true;
    FileStamp after;
    if (revalidate) {
      after = StampFile(asset);
      cacheable = before.valid && after.valid && before.matches(after);
    }
    if (cacheable) {
      ValueClipStageCache::Entry entry;
      entry.stage = stage;
      entry.stamped = revalidate;
      entry.mtime_ticks = after.mtime_ticks;
      entry.size_bytes = after.size_bytes;
      store(asset, std::move(entry));
    }
    return stage;
  };
  for (const ValueClipSet& set : sets) {
    const int index = ActiveIndex(set, stage_time);
    if (index < 0 || static_cast<size_t>(index) >= set.asset_paths.size())
      continue;
    const std::string clip_path =
        set.prim_path.empty() ? prim.GetPath().str() : set.prim_path;
    if (resolution_info) {
      *resolution_info = ValueClipResolutionInfo();
      resolution_info->active_index = index;
      resolution_info->active_asset = set.asset_paths[size_t(index)];
      resolution_info->clip_time = ClipTime(set, stage_time);
    }

    // Manifest gating (pxr semantics): when a manifest is authored and
    // loadable, a property resolves through clips only if it is DECLARED
    // (authored as a spec) in the manifest. HasProperty() must NOT be used
    // here — it also reports schema-fallback properties (visibility, radius,
    // ...) that are not in the manifest, which would leak them into clips.
    if (!set.manifest_asset_path.empty()) {
      if (const std::shared_ptr<Stage> manifest_stage =
              load_stage(set.manifest_asset_path)) {
        const UsdPrim mprim = manifest_stage->GetPrimAtPath(clip_path);
        const PrimSpec* mspec = mprim.GetPrimSpec();
        if (!mspec || !mspec->property(property)) continue;
      }
      // Unloadable manifest: fall through without gating (pxr degrades the
      // same way when the manifest layer cannot be opened).
    }

    // Sample one clip of this set, mapping `at_time` through the set's time
    // function. Returns an empty Value when the clip is unloadable or carries
    // no opinion for the property.
    auto sample_clip_at = [&](size_t clip_index, double at_time,
                              std::string* asset_out) {
      Value none;
      if (clip_index >= set.asset_paths.size()) return none;
      const std::string& asset = set.asset_paths[clip_index];
      const std::shared_ptr<Stage> clip_stage = load_stage(asset);
      if (!clip_stage) return none;
      const UsdPrim clip_prim = clip_stage->GetPrimAtPath(clip_path);
      if (!clip_prim.IsValid() || !clip_prim.HasProperty(property)) {
        return none;
      }
      Value v;
      if (interpolation == TimeInterpolation::Held) {
        const Value* sample = clip_prim.GetValueAtTime(property, ClipTime(set, at_time));
        if (!sample) sample = clip_prim.GetPropertyValue(property);
        if (sample) v = *sample;
      } else v = clip_prim.GetInterpolatedValue(property, ClipTime(set, at_time));
      if ((v.is_empty() || v.is_block()) && clip_prim.GetPrimSpec() &&
          clip_prim.GetPrimSpec()->meta().clips().is_dictionary()) {
        const std::string key = asset + "|" + clip_path + "." + property;
        if (std::find(resolution_stack.begin(), resolution_stack.end(), key) !=
            resolution_stack.end()) {
          if (error && error->empty())
            *error = "Value-clip cycle detected at `" + key + "`";
          return none;
        }
        if (resolution_stack.size() >= cache->max_recursion_depth) {
          if (error && error->empty())
            *error = "Value-clip recursion depth exceeded at `" + key + "`";
          return none;
        }
        resolution_stack.push_back(key);
        Value nested;
        std::string nested_asset;
        const bool resolved = ResolveValueClip(
            clip_prim, property, ClipTime(set, at_time), loader, &nested,
            &nested_asset, error, nullptr, cache, nullptr, interpolation);
        resolution_stack.pop_back();
        if (resolved) {
          if (asset_out)
            *asset_out = nested_asset.empty() ? asset : nested_asset;
          return nested;
        }
      }
      if (v.is_empty() || v.is_block()) return none;
      if (asset_out) *asset_out = asset;
      return v;
    };

    std::string asset;
    Value value =
        sample_clip_at(static_cast<size_t>(index), stage_time, &asset);

    // The clip's active start is an implicit sample. After its last sample,
    // linear resolution brackets against the NEXT clip's active start, rather
    // than clamping the current file. This is independent of the option for
    // interpolating clips that omit the property entirely.
    if (!value.is_empty() && interpolation == TimeInterpolation::Linear &&
        set.active.size() > 1) {
      const auto next = std::upper_bound(
          set.active.begin(), set.active.end(), stage_time,
          [](double t, const std::pair<double, int>& entry) {
            return t < entry.first;
          });
      if (next != set.active.end() && next != set.active.begin() &&
          stage_time > std::prev(next)->first) {
        const auto current = std::prev(next);
        const double start = current->first, end = next->first;
        const auto mapping_at_end = std::lower_bound(
            set.times.begin(), set.times.end(), end,
            [](const std::pair<double, double>& mapping, double t) {
              return mapping.first < t;
            });
        const auto mapping_after_end = std::upper_bound(
            mapping_at_end, set.times.end(), end,
            [](double t, const std::pair<double, double>& mapping) {
              return t < mapping.first;
            });
        const bool jumps_at_end = mapping_at_end != mapping_after_end &&
            mapping_at_end->second != std::prev(mapping_after_end)->second;
        const std::shared_ptr<Stage> active_stage =
            load_stage(set.asset_paths[size_t(index)]);
        const UsdPrim active_prim = active_stage
            ? active_stage->GetPrimAtPath(clip_path) : UsdPrim();
        double lower = start;
        bool has_upper = false;
        auto consider = [&](double t) {
          if (!std::isfinite(t) || t < start || t >= end) return;
          if (t <= stage_time) lower = std::max(lower, t);
          if (t >= stage_time) has_upper = true;
        };
        for (const auto& mapping : set.times) consider(mapping.first);
        // Invert each piece of the stage-to-clip time function. A frozen
        // segment has no additional sample locations; its endpoints above
        // already bracket its constant value. Reversed segments are valid.
        const auto samples = active_prim.GetTimeSampleTimes(property);
        if (set.times.empty()) {
          const auto upper = std::lower_bound(samples.begin(), samples.end(), stage_time);
          if (upper != samples.end()) consider(*upper);
          if (upper != samples.begin()) consider(*std::prev(upper));
        } else if (!samples.empty()) {
          for (size_t i = 1; i < set.times.size(); ++i) {
            const auto& a = set.times[i - 1];
            const auto& b = set.times[i];
            if (b.first <= a.first || b.second == a.second ||
                b.first < start || a.first >= end) continue;
            const double query = std::max(a.first, std::min(b.first, stage_time));
            const double clip_query = a.second + (b.second - a.second) *
                ((query - a.first) / (b.first - a.first));
            const auto upper = std::lower_bound(samples.begin(), samples.end(), clip_query);
            auto invert = [&](double t) {
              const double alpha = (t - a.second) / (b.second - a.second);
              if (alpha >= 0.0 && alpha <= 1.0)
                consider(a.first + alpha * (b.first - a.first));
            };
            if (upper != samples.end()) invert(*upper);
            if (upper != samples.begin()) invert(*std::prev(upper));
          }
        }
        // A time-map jump at the boundary isolates its two sides. Never
        // blend the left clip toward the right-hand value of that jump.
        // Default-only compatibility opinions also remain held.
        if (!has_upper && !jumps_at_end && stage_time > lower && !samples.empty()) {
          std::string lower_asset, upper_asset;
          Value lo = sample_clip_at(size_t(index), lower, &lower_asset);
          Value hi = sample_clip_at(size_t(next->second), end, &upper_asset);
          if (!lo.is_empty() && !hi.is_empty()) {
            value = LerpValue(lo, hi, (stage_time - lower) / (end - lower));
            asset = lower_asset;
            if (resolution_info) {
              resolution_info->lower_asset = lower_asset;
              resolution_info->upper_asset = upper_asset;
              resolution_info->lower_stage_time = lower;
              resolution_info->upper_stage_time = end;
            }
          }
        }
      }
    }

    // interpolateMissingClipValues: when the ACTIVE clip has no opinion, pxr
    // treats all clips as one merged timeline and LINEARLY INTERPOLATES the
    // property between the nearest earlier and nearest later clips that do
    // carry a value (valueClips.md, "Interpolating Missing Values"). We anchor
    // each bracketing clip at its own active stage-time (exact for the common
    // one-sample-per-clip authoring); non-interpolatable types hold the
    // earlier neighbor.
    if (value.is_empty() && set.interpolate_missing && !set.active.empty()) {
      // Index of the active entry selected for stage_time (active is sorted).
      size_t cur = 0;
      for (size_t i = 0; i < set.active.size(); ++i) {
        if (set.active[i].first > stage_time) break;
        cur = i;
      }
      double t_lo = 0.0, t_hi = 0.0;
      std::string a_lo, a_hi;
      Value v_lo, v_hi;
      // Earlier neighbors: active entries cur-1, cur-2, ..., 0 (cur is the
      // empty active clip that put us here, so it is skipped).
      for (size_t i = cur; i-- > 0;) {
        Value v = sample_clip_at(static_cast<size_t>(set.active[i].second),
                                 set.active[i].first, &a_lo);
        if (!v.is_empty()) {
          v_lo = std::move(v);
          t_lo = set.active[i].first;
          break;
        }
      }
      for (size_t i = cur + 1; i < set.active.size(); ++i) {  // later neighbors
        Value v = sample_clip_at(static_cast<size_t>(set.active[i].second),
                                 set.active[i].first, &a_hi);
        if (!v.is_empty()) {
          v_hi = std::move(v);
          t_hi = set.active[i].first;
          break;
        }
      }

      if (!v_lo.is_empty() && !v_hi.is_empty() && t_hi > t_lo) {
        const double alpha = (stage_time - t_lo) / (t_hi - t_lo);
        // LerpValue interpolates every linearly-interpolatable type (scalars,
        // vectors, colors, matrices, quats, half, and their arrays) and HOLDS
        // (returns the earlier value) for non-interpolatable types or a
        // cross-clip type mismatch — exactly pxr's clip-value semantics.
        value = interpolation == TimeInterpolation::Held ? v_lo : LerpValue(v_lo, v_hi, alpha);
        asset = a_lo;
        if (resolution_info) {
          resolution_info->interpolated_missing = interpolation != TimeInterpolation::Held;
          resolution_info->lower_asset = a_lo;
          resolution_info->upper_asset = a_hi;
          resolution_info->lower_stage_time = t_lo;
          resolution_info->upper_stage_time = t_hi;
        }
      } else if (!v_lo.is_empty()) {
        value = std::move(v_lo);
        asset = a_lo;
        if (resolution_info) {
          resolution_info->interpolated_missing = true;
          resolution_info->lower_asset = a_lo;
          resolution_info->lower_stage_time = t_lo;
        }
      } else if (!v_hi.is_empty()) {
        value = std::move(v_hi);
        asset = a_hi;
        if (resolution_info) {
          resolution_info->interpolated_missing = true;
          resolution_info->upper_asset = a_hi;
          resolution_info->upper_stage_time = t_hi;
        }
      }
    }

    if (value.is_empty()) continue;
    *out = std::move(value);
    if (source_asset) *source_asset = asset;
    if (source_clip_set) *source_clip_set = set.name;
    return true;
  }
  return false;
}

}  // namespace next
}  // namespace lightusd
