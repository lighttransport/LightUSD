// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.

#include "usdz-convert.hh"
#include "safe-arithmetic.hh"

#include <algorithm>
#include <chrono>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <condition_variable>
#endif
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include <utility>

#include "lightusd.hh"
#include "composition.hh"
#include "asset-resolution.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"
#include "usdShade.hh"
#include "core/animatable.hh"
#include "image-loader.hh"
#include "image-writer.hh"
#include "io-util.hh"
#include "pprinter.hh"
#include "str-util.hh"
#include "tydra/texture-util.hh"
#include "usdz-geometry-optimize.hh"
#include "usdz-material-optimize.hh"
#include "usdz-udim-bake.hh"
#include <atomic>
#include <chrono>

// Emscripten's <stdio.h> defines `stdout` as a self-referential macro
// (`#define stdout (stdout)`) so the same identifier works both as a
// macro and as a real variable. The recursion trips clang's
// -Wdisabled-macro-expansion diagnostic at every call site that names
// `stdout` (e.g. `std::fflush(stdout)`). Undefining the macro here makes
// the bare token refer to the underlying `extern FILE *const stdout`
// variable; the FILE* value is identical. This is a no-op on platforms
// where `stdout` is not a macro (e.g. glibc, where it's a plain extern).
#ifdef __EMSCRIPTEN__
# undef stdout
#endif

namespace lightusd {
namespace usdz {

namespace {

// Sanity backstop for the recursive asset/texture collectors below. This is NOT
// a normal scene limit: it only exists to bound memory on pathological/corrupt
// input. It must clear realistic large scenes by a wide margin — a single large
// engine/city export can carry tens of thousands of UsdUVTexture prims (observed
// up to ~45k texture inputs) — because a collector that stops early silently
// drops textures/dependencies from the rename remap, leaving dangling references
// (e.g. an EXR transcoded to PNG whose inputs:file past the cutoff is never
// rewritten). 1e6 is ~20x the largest observed real scene yet still cheap
// (pointer/string per entry) and wasm32-heap-safe.
constexpr size_t kMaxAssetCollectCount = 1000000;

size_t FileSizeOrZero(const std::string &filename) {
  std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return 0;
  }
  const std::streampos pos = ifs.tellg();
  if (pos < 0) {
    return 0;
  }
  return static_cast<size_t>(pos);
}

void Log(bool verbose, const std::string &msg) {
  if (verbose) {
    std::printf("[lusdzconvert] %s\n", msg.c_str());
  }
}

// Coarse phase timer for progress reporting / profiling. Call begin("name") at
// the start of each phase; the elapsed time of the previous phase is printed
// when the next phase begins (and on end()). Active only when verbose.
class PhaseTimer {
 public:
  explicit PhaseTimer(bool verbose) : verbose_(verbose) {}
  void begin(const std::string &name) {
    flush();
    cur_ = name;
    start_ = std::chrono::steady_clock::now();
    active_ = true;
    if (verbose_) {
      std::printf("[lusdzconvert][phase] %-22s ...\n", name.c_str());
      std::fflush(stdout);
    }
  }
  void end() { flush(); }

 private:
  void flush() {
    if (active_ && verbose_) {
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start_)
                      .count();
      total_ms_ += ms;
      std::printf("[lusdzconvert][phase] %-22s %9.1f ms  (total %9.1f ms)\n",
                  cur_.c_str(), ms, total_ms_);
      std::fflush(stdout);
    }
    active_ = false;
  }
  bool verbose_;
  bool active_ = false;
  double total_ms_ = 0.0;
  std::string cur_;
  std::chrono::steady_clock::time_point start_;
};

// Compute a relative path from `base_dir` to `target`.
// Both paths should be normalized (forward slashes, no trailing slash).
// Returns `target` unchanged when paths cannot be relativized (e.g. different
// roots on Windows).
std::string ToRelativePath(const std::string &base_dir,
                           const std::string &target) {
  // Normalize backslashes.
  std::string b = base_dir;
  std::string t = target;
  std::replace(b.begin(), b.end(), '\\', '/');
  std::replace(t.begin(), t.end(), '\\', '/');
  // Strip trailing slashes.
  while (!b.empty() && b.back() == '/') b.pop_back();
  while (!t.empty() && t.back() == '/') t.pop_back();
  // If either is empty or not absolute, return target as-is.
  if (b.empty() || t.empty() || b[0] != '/' || t[0] != '/') {
    return target;
  }
  // Split into components.
  auto split = [](const std::string &p) -> std::vector<std::string> {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < p.size()) {
      size_t slash = p.find('/', start);
      if (slash == std::string::npos) {
        parts.push_back(p.substr(start));
        break;
      }
      if (slash > start) {
        parts.push_back(p.substr(start, slash - start));
      }
      start = slash + 1;
    }
    return parts;
  };
  std::vector<std::string> bparts = split(b);
  std::vector<std::string> tparts = split(t);
  // Find common prefix length.
  size_t common = 0;
  while (common < bparts.size() && common < tparts.size() &&
         bparts[common] == tparts[common]) {
    common++;
  }
  // Build relative path.
  std::string rel;
  for (size_t i = common; i < bparts.size(); i++) {
    rel += "../";
  }
  for (size_t i = common; i < tparts.size(); i++) {
    if (i > common) rel += "/";
    rel += tparts[i];
  }
  if (rel.empty()) {
    rel = ".";
  }
  return rel;
}

bool IsAllowedTextureExt(const std::string &ext_lower) {
  return ext_lower == "png" || ext_lower == "jpg" || ext_lower == "jpeg" ||
         ext_lower == "exr"
#if defined(LIGHTUSD_WITH_TEXTOOLS)
         // KTX2 is a valid *input* texture (the core loader decodes uni/BC7/ASTC
         // to RGBA8). A direct `inputs:file = @tex.ktx2@` reference is thus
         // collected and, when a legacy/ARKit target is requested, transcoded to
         // png/jpg by ProcessTexture. KTX2 is NOT ARKit/USDZ-legal (see
         // IsAllowedARKitTextureExt), so it is never emitted as a final USDZ
         // texture — only decoded away.
         || ext_lower == "ktx2"
#endif
      ;
}

bool IsAllowedARKitTextureExt(const std::string &ext_lower) {
  // Deliberately excludes ktx2: USDZ / AR Quick Look permit only png/jpg/exr
  // (+avif). A .ktx2 input must be transcoded to one of these before output.
  return ext_lower == "png" || ext_lower == "jpg" || ext_lower == "jpeg" ||
         ext_lower == "exr";
}

bool IsAllowedARKitPrimType(const std::string &type_name) {
  static const std::set<std::string> allowed = {
      "", "Scope", "Xform", "Camera", "Shader", "Material", "Mesh",
      "Sphere", "Cube", "Cylinder", "Cone", "Capsule", "GeomSubset",
      "Points", "SkelRoot", "Skeleton", "SkelAnimation", "BlendShape",
      "SpatialAudio", "PhysicsScene", "Preliminary_ReferenceImage",
      "Preliminary_Text", "Preliminary_Trigger"};
  return allowed.count(type_name) || lightusd::startsWith(type_name, "RealityKit");
}

bool IsAllowedARKitShaderId(const std::string &shader_id) {
  return shader_id == "UsdPreviewSurface" || shader_id == "UsdUVTexture" ||
         shader_id == "UsdTransform2d" ||
         lightusd::startsWith(shader_id, "UsdPrimvarReader") ||
         lightusd::startsWith(shader_id, "ND_");
}

// Turn an arbitrary asset path into a safe, relative USDZ archive name.
// Absolute paths and paths escaping the archive root are reduced to a
// "textures/<basename>" form.
bool IsUnsafeUnresolvedTexturePath(const std::string &assetPath) {
  std::string p = assetPath;
  std::replace(p.begin(), p.end(), '\\', '/');
  // Reject null bytes (could corrupt ZIP entry names).
  if (p.find('\0') != std::string::npos) return true;
  return p.empty() || (p[0] == '/') || (p.find(':') != std::string::npos) ||
         (p == "..") || lightusd::startsWith(p, "../") ||
         (p.find("/../") != std::string::npos) ||
         lightusd::endsWith(p, "/..");
}

std::string RelativeToSearchPath(const std::string &asset_path,
                                 const std::vector<std::string> &search_paths) {
  std::string p = asset_path;
  std::replace(p.begin(), p.end(), '\\', '/');

  for (std::string base : search_paths) {
    if (base.empty()) {
      continue;
    }
    std::replace(base.begin(), base.end(), '\\', '/');
    while (!base.empty() && base.back() == '/') {
      base.pop_back();
    }

    if (base.empty()) {
      continue;
    }

    if (io::IsAbsPath(p)) {
      continue;
    }

    if (p == base) {
      return io::GetBaseFilename(p);
    }
    if (lightusd::startsWith(p, base + "/")) {
      return p.substr(base.size() + 1);
    }
  }

  return asset_path;
}

std::string SafeMissingArchiveReference(
    const std::string &asset_path,
    const std::vector<std::string> &search_paths) {
  const std::string rel = RelativeToSearchPath(asset_path, search_paths);
  if (rel != asset_path) {
    return IsUnsafeUnresolvedTexturePath(rel) ? std::string() : rel;
  }

  if (!io::IsAbsPath(asset_path)) {
    return IsUnsafeUnresolvedTexturePath(asset_path) ? std::string()
                                                    : asset_path;
  }

  for (const std::string &base : search_paths) {
    if (base.empty() || !io::IsAbsPath(base)) {
      continue;
    }
    const std::string rel_to_base = ToRelativePath(base, asset_path);
    if (rel_to_base != asset_path && !IsUnsafeUnresolvedTexturePath(rel_to_base)) {
      return rel_to_base;
    }
  }

  return std::string();
}

// Maximum ZIP entry name length (ZIP spec allows 64 KB, but we cap much lower
// to prevent pathological entry names from causing issues).
constexpr size_t kMaxArchiveNameLen = 256;

bool IsSafeArchiveSegment(const std::string &segment) {
  return !segment.empty() && segment != "." && segment != ".." &&
         segment.find('\0') == std::string::npos &&
         segment.find('/') == std::string::npos &&
         segment.find('\\') == std::string::npos &&
         segment.find(':') == std::string::npos &&
         segment.find("..") == std::string::npos;
}

std::string SanitizeArchiveName(const std::string &assetPath) {
  std::string p = assetPath;
  // Normalize backslashes.
  std::replace(p.begin(), p.end(), '\\', '/');

  const bool needs_sanitize = IsUnsafeUnresolvedTexturePath(p) ||
                              lightusd::startsWith(p, "./");

  std::string name = p;
  if (needs_sanitize) {
    std::string base = io::GetBaseFilename(p);
    if (!IsSafeArchiveSegment(base)) {
      base = "texture";
    }
    name = "textures/" + base;
  }
  // Reject paths that still contain unsafe archive syntax after sanitization.
  // Use segment-based traversal check: any "/" separated segment that is
  // exactly ".." is rejected, regardless of its position in the path.
  {
    bool has_traversal = (name.find('\0') != std::string::npos) ||
                         (name.find('\\') != std::string::npos) ||
                         (name.find(':') != std::string::npos) ||
                         (name.find("//") != std::string::npos) ||
                         lightusd::startsWith(name, "/") ||
                         lightusd::startsWith(name, "./") ||
                         lightusd::startsWith(name, "../") ||
                         lightusd::endsWith(name, "/");
    if (!has_traversal) {
      // Also check for ".." as a standalone path segment.
      size_t pos = 0;
      while (pos < name.size()) {
        size_t next = name.find('/', pos);
        std::string seg = (next == std::string::npos)
                          ? name.substr(pos)
                          : name.substr(pos, next - pos);
        if (seg == "..") { has_traversal = true; break; }
        pos = (next == std::string::npos) ? name.size() : next + 1;
      }
    }
    if (has_traversal) {
      name = "textures/texture";
    }
  }
  // Enforce maximum archive name length.
  if (name.size() > kMaxArchiveNameLen) {
    name = "textures/texture";
  }
  return name;
}

std::string SanitizeLayerArchiveName(const std::string &assetPath) {
  std::string p = assetPath;
  std::replace(p.begin(), p.end(), '\\', '/');

  const bool needs_sanitize = IsUnsafeUnresolvedTexturePath(p) ||
                              lightusd::startsWith(p, "./");

  std::string name = p;
  if (needs_sanitize) {
    std::string base = io::GetBaseFilename(p);
    if (!IsSafeArchiveSegment(base)) {
      base = "layer";
    }
    name = "layers/" + base;
  }

  if (name.find('\0') != std::string::npos ||
      name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos ||
      name.find("..") != std::string::npos ||
      name.find("//") != std::string::npos ||
      lightusd::startsWith(name, "/") ||
      lightusd::startsWith(name, "./") ||
      lightusd::startsWith(name, "../") ||
      lightusd::endsWith(name, "/")) {
    name = "layers/layer";
  }
  if (name.size() > kMaxArchiveNameLen) {
    name = "layers/layer";
  }
  return name;
}

std::string MakeCollisionArchiveName(const std::string &archive_name,
                                     uint32_t suffix) {
  const std::string ext = io::GetFileExtension(archive_name);
  const size_t base_len = (!ext.empty() && archive_name.size() > ext.size() + 1)
      ? archive_name.size() - ext.size() - 1
      : archive_name.size();
  std::string base = archive_name.substr(0, base_len);
  const std::string suffix_part =
      "_" + std::to_string(suffix) + (ext.empty() ? std::string() : "." + ext);
  if (base.size() + suffix_part.size() > kMaxArchiveNameLen) {
    if (suffix_part.size() >= kMaxArchiveNameLen) {
      return "textures/texture";
    }
    base.resize(kMaxArchiveNameLen - suffix_part.size());
    if (base.empty() || lightusd::endsWith(base, "/")) {
      base += "texture";
      if (base.size() + suffix_part.size() > kMaxArchiveNameLen) {
        base.resize(kMaxArchiveNameLen - suffix_part.size());
      }
    }
  }
  return base + suffix_part;
}

std::string ReplaceExtension(const std::string &name, const std::string &newExtNoDot) {
  std::string ext = io::GetFileExtension(name);
  if (ext.empty()) {
    return name + "." + newExtNoDot;
  }
  return name.substr(0, name.size() - ext.size()) + newExtNoDot;
}

// Read the (default, scalar) asset path string from a UsdUVTexture.file attr.
// Path length is capped to prevent excessive memory usage downstream.
bool ReadTextureFilePath(const UsdUVTexture &tex, std::string *out) {
  const auto av = tex.file.get_value();  // optional<Animatable<AssetPath>>
  if (!av) {
    return false;
  }
  if (!av.value().is_scalar()) {
    return false;
  }
  value::AssetPath ap;
  if (!av.value().get_scalar(&ap)) {
    return false;
  }
  std::string path = ap.GetAssetPath();
  constexpr size_t kMaxPathLength = 4096;
  if (path.size() > kMaxPathLength) {
    return false;
  }
  (*out) = path;
  return !out->empty();
}

void WriteTextureFilePath(UsdUVTexture *tex, const std::string &path) {
  value::AssetPath ap(path);
  Animatable<value::AssetPath> anim(ap);
  tex->file.set_value(anim);
}

// Collect mutable UsdUVTexture pointers (paired with their current file path)
// by recursively walking the stage's Prim tree.
// Guards against excessive texture counts to prevent memory exhaustion.
void CollectTextures(Prim &prim,
                     std::vector<std::pair<UsdUVTexture *, std::string>> *out,
                     int depth = 0) {
  if (depth > 512) return;  // guard against stack overflow
  constexpr size_t kMaxTextureCount = kMaxAssetCollectCount;
  if (out->size() >= kMaxTextureCount) return;  // guard against memory exhaustion
  Shader *shd = prim.get_data().as<Shader>();
  if (shd && shd->info_id == "UsdUVTexture") {
    UsdUVTexture *tex = shd->value.as<UsdUVTexture>();
    if (tex) {
      std::string path;
      if (ReadTextureFilePath(*tex, &path)) {
        out->emplace_back(tex, path);
      }
    }
  }
  for (auto &child : prim.children()) {
    CollectTextures(child, out, depth + 1);
  }
}

void CollectLayerTextureAssetPaths(const PrimSpec &primspec,
                                   std::vector<std::string> *out,
                                   int depth = 0) {
  if (depth > 512) return;
  constexpr size_t kMaxTextureCount = kMaxAssetCollectCount;
  if (out->size() >= kMaxTextureCount) return;

  for (const auto &item : primspec.props()) {
    const Attribute *attr = item.second.get_attribute_or_null();
    if (!attr || !attr->has_value()) {
      continue;
    }

    value::AssetPath asset_path;
    if (!attr->get_value(&asset_path)) {
      continue;
    }

    const std::string path = asset_path.GetAssetPath();
    const std::string ext = to_lower(io::GetFileExtension(path));
    if (!path.empty() && IsAllowedTextureExt(ext)) {
      out->push_back(path);
      if (out->size() >= kMaxTextureCount) return;
    }
  }

  for (const PrimSpec &child : primspec.children()) {
    CollectLayerTextureAssetPaths(child, out, depth + 1);
    if (out->size() >= kMaxTextureCount) return;
  }
}

void CollectLayerTextureAssetPaths(const Layer &layer,
                                   std::vector<std::string> *out) {
  for (const auto &item : layer.primspecs()) {
    CollectLayerTextureAssetPaths(item.second, out);
  }
}

std::string TexturePathKey(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  // Strip leading "./" prefix repetitions in a single pass.
  size_t skip = 0;
  while (skip + 1 < path.size() && path[skip] == '.' && path[skip + 1] == '/') {
    skip += 2;
  }
  if (skip > 0) {
    path = path.substr(skip);
  }
  return io::NormalizePath(path);
}

void CollectUnusedTextureFilePaths(const std::vector<std::string> &inputs,
                                   const std::vector<std::string> &texture_paths,
                                   std::vector<std::string> *out) {
  if (!out) return;
  std::set<std::string> known;
  for (const std::string &path : texture_paths) {
    known.insert(TexturePathKey(path));
  }

  std::set<std::string> dirs;
  for (const std::string &input : inputs) {
    std::string dir = io::GetBaseDir(input);
    if (dir.empty()) dir = ".";
    dirs.insert(io::NormalizePath(dir));
  }

  std::set<std::string> emitted;
  for (const std::string &dir : dirs) {
    if (out->size() >= kMaxAssetCollectCount) return;
    if (!io::IsDirectory(dir)) continue;
    const std::vector<std::string> entries = io::ListDir(dir, true);
    for (const std::string &rel : entries) {
      if (out->size() >= kMaxAssetCollectCount) return;
      const std::string ext = to_lower(io::GetFileExtension(rel));
      if (!IsAllowedTextureExt(ext)) continue;
      const std::string asset_path = TexturePathKey(rel);
      const std::string full_path = TexturePathKey(io::JoinPath(dir, rel));
      if (known.count(asset_path) || known.count(full_path) ||
          emitted.count(asset_path)) {
        continue;
      }
      emitted.insert(asset_path);
      out->push_back(asset_path);
    }
  }
}

void CollectARKitCompatibilityWarnings(const PrimSpec &primspec,
                                       std::string *warn,
                                       int depth = 0) {
  if (depth > 512) return;

  if (!IsAllowedARKitPrimType(primspec.typeName()) && warn) {
    *warn += "ARKit compatibility: prim '" + primspec.name() +
             "' has unsupported type '" + primspec.typeName() + "'.\n";
  }

  if (primspec.typeName() == "Shader") {
    const auto prop_it = primspec.props().find("info:id");
    if (prop_it != primspec.props().end()) {
      const Attribute *attr = prop_it->second.get_attribute_or_null();
      value::token shader_id;
      if (attr && attr->get_value(&shader_id) &&
          !IsAllowedARKitShaderId(shader_id.str()) && warn) {
        *warn += "ARKit compatibility: shader '" + primspec.name() +
                 "' has unsupported info:id '" + shader_id.str() + "'.\n";
      }
    }
  }

  for (const auto &variant_set : primspec.variantSets()) {
    for (const auto &variant : variant_set.second.variantSet) {
      CollectARKitCompatibilityWarnings(variant.second, warn, depth + 1);
    }
  }
  for (const PrimSpec &child : primspec.children()) {
    CollectARKitCompatibilityWarnings(child, warn, depth + 1);
  }
}

void CollectARKitCompatibilityWarnings(const Layer &layer, std::string *warn) {
  for (const auto &item : layer.primspecs()) {
    CollectARKitCompatibilityWarnings(item.second, warn);
  }
}

void CollectCompositionAssetPaths(const PrimSpec &primspec,
                                  std::vector<std::string> *out,
                                  int depth = 0) {
  if (depth > 512) return;
  constexpr size_t kMaxLayerDependencyCount = kMaxAssetCollectCount;
  if (out->size() >= kMaxLayerDependencyCount) return;

  const PrimMeta &metas = primspec.metas();
  if (metas.references) {
    for (const auto &op : metas.references.value()) {
      for (const Reference &ref : op.second) {
        const std::string path = ref.asset_path.GetAssetPath();
        if (!path.empty()) {
          out->push_back(path);
          if (out->size() >= kMaxLayerDependencyCount) return;
        }
      }
    }
  }
  if (metas.payload) {
    for (const auto &op : metas.payload.value()) {
      for (const Payload &payload : op.second) {
        const std::string path = payload.asset_path.GetAssetPath();
        if (!path.empty()) {
          out->push_back(path);
          if (out->size() >= kMaxLayerDependencyCount) return;
        }
      }
    }
  }

  for (const auto &variant_set : primspec.variantSets()) {
    for (const auto &variant : variant_set.second.variantSet) {
      CollectCompositionAssetPaths(variant.second, out, depth + 1);
      if (out->size() >= kMaxLayerDependencyCount) return;
    }
  }

  for (const PrimSpec &child : primspec.children()) {
    CollectCompositionAssetPaths(child, out, depth + 1);
    if (out->size() >= kMaxLayerDependencyCount) return;
  }
}

void CollectCompositionAssetPaths(const Layer &layer,
                                  std::vector<std::string> *out) {
  constexpr size_t kMaxLayerDependencyCount = kMaxAssetCollectCount;
  for (const SubLayer &sublayer : layer.metas().subLayers) {
    const std::string path = sublayer.assetPath.GetAssetPath();
    if (!path.empty()) {
      out->push_back(path);
      if (out->size() >= kMaxLayerDependencyCount) return;
    }
  }
  for (const auto &item : layer.primspecs()) {
    CollectCompositionAssetPaths(item.second, out);
    if (out->size() >= kMaxLayerDependencyCount) return;
  }
}

size_t RemapCompositionAssetPaths(
    PrimSpec &primspec, const std::map<std::string, std::string> &remap,
    int depth = 0) {
  if (depth > 512) return 0;

  size_t n = 0;
  PrimMeta &metas = primspec.metas();
  if (metas.references) {
    for (auto &op : metas.references.value()) {
      for (Reference &ref : op.second) {
        const auto it = remap.find(ref.asset_path.GetAssetPath());
        if (it != remap.end() && it->second != ref.asset_path.GetAssetPath()) {
          ref.asset_path = value::AssetPath(it->second);
          n++;
        }
      }
    }
  }
  if (metas.payload) {
    for (auto &op : metas.payload.value()) {
      for (Payload &payload : op.second) {
        const auto it = remap.find(payload.asset_path.GetAssetPath());
        if (it != remap.end() &&
            it->second != payload.asset_path.GetAssetPath()) {
          payload.asset_path = value::AssetPath(it->second);
          n++;
        }
      }
    }
  }

  for (auto &variant_set : primspec.variantSets()) {
    for (auto &variant : variant_set.second.variantSet) {
      n += RemapCompositionAssetPaths(variant.second, remap, depth + 1);
    }
  }

  for (PrimSpec &child : primspec.children()) {
    n += RemapCompositionAssetPaths(child, remap, depth + 1);
  }
  return n;
}

size_t RemapCompositionAssetPaths(
    Layer &layer, const std::map<std::string, std::string> &remap) {
  size_t n = 0;
  for (SubLayer &sublayer : layer.metas().subLayers) {
    const auto it = remap.find(sublayer.assetPath.GetAssetPath());
    if (it != remap.end() && it->second != sublayer.assetPath.GetAssetPath()) {
      sublayer.assetPath = value::AssetPath(it->second);
      n++;
    }
  }
  for (auto &item : layer.primspecs()) {
    n += RemapCompositionAssetPaths(item.second, remap);
  }
  return n;
}

size_t RemapLayerAssetPaths(PrimSpec &primspec,
                            const std::map<std::string, std::string> &remap,
                            int depth = 0) {
  if (depth > 512) return 0;

  size_t n = 0;
  for (auto &item : primspec.props()) {
    Attribute *attr = item.second.get_attribute_or_null();
    if (!attr || !attr->has_value()) {
      continue;
    }

    value::AssetPath asset_path;
    if (!attr->get_value(&asset_path)) {
      continue;
    }

    const auto it = remap.find(asset_path.GetAssetPath());
    if (it == remap.end() || it->second == asset_path.GetAssetPath()) {
      continue;
    }

    attr->set_value(value::AssetPath(it->second));
    n++;
  }

  for (PrimSpec &child : primspec.children()) {
    n += RemapLayerAssetPaths(child, remap, depth + 1);
  }
  return n;
}

size_t RemapLayerAssetPaths(Layer &layer,
                            const std::map<std::string, std::string> &remap) {
  size_t n = 0;
  for (auto &item : layer.primspecs()) {
    n += RemapLayerAssetPaths(item.second, remap);
  }
  return n;
}

bool ComposeLayerToFixedPoint(AssetResolutionResolver &resolver,
                              const Layer &root_layer,
                              Layer *composited_layer,
                              std::string *warn,
                              std::string *err) {
  if (!composited_layer) {
    if (err) {
      (*err) = "composited_layer is null.";
    }
    return false;
  }

  Layer src_layer = root_layer;

  // Like lusdcat, assets resolve against the local filesystem where USD's
  // parent-relative references (`@../common/foo.usd@`) are legitimate; the
  // resolver suffix-fallback additionally rebases paths authored against
  // another machine's layout (e.g. UE exports).
  lightusd::SublayersCompositionOptions sublayer_options;
  sublayer_options.allow_parent_relative_paths = true;
  lightusd::ReferencesCompositionOptions references_options;
  references_options.allow_parent_relative_paths = true;
  lightusd::PayloadCompositionOptions payload_options;
  payload_options.allow_parent_relative_paths = true;

  // Parse each referenced file once across the whole fixed-point loop; all
  // arcs to the same file share one copy of the heavy attribute data (COW).
  std::map<std::string, Layer> layer_cache;
  references_options.layer_cache = &layer_cache;
  payload_options.layer_cache = &layer_cache;

  if (!src_layer.metas().subLayers.empty()) {
    Layer tmp;
    if (!lightusd::CompositeSublayers(resolver, src_layer, &tmp, warn, err,
                                      sublayer_options)) {
      return false;
    }
    src_layer = std::move(tmp);
  }

  // NOTE: this loop applies R -> P -> I -> V rather than routing through
  // CompositeAllArcs (LIVRPS strength order) because the InPlace variants
  // below halve peak memory — this converter's job is multi-GB scenes.
  // Deferred variant evaluation (ShouldDeferVariantComposition) covers the
  // common selection-vs-late-variantSet hazard; the residual divergence is
  // the inherits-vs-references strength edge, which lusdcat/web/binding
  // handle via CompositeAllArcs.
  constexpr int kMaxIteration = 64;
  for (int i = 0; i < kMaxIteration; i++) {
    bool has_unresolved = false;

    if (src_layer.check_unresolved_references()) {
      has_unresolved = true;
      Layer tmp;
      // InPlace: consumes src_layer (no internal arcs) instead of holding
      // input + output copies — halves the peak of the pass.
      if (!lightusd::CompositeReferencesInPlace(
              resolver, std::make_unique<Layer>(std::move(src_layer)), &tmp,
              warn, err, references_options)) {
        return false;
      }
      src_layer = std::move(tmp);
    }

    if (src_layer.check_unresolved_payload()) {
      has_unresolved = true;
      Layer tmp;
      if (!lightusd::CompositePayloadInPlace(
              resolver, std::make_unique<Layer>(std::move(src_layer)), &tmp,
              warn, err, payload_options)) {
        return false;
      }
      src_layer = std::move(tmp);
    }

    if (src_layer.check_unresolved_inherits()) {
      has_unresolved = true;
      Layer tmp;
      if (!lightusd::CompositeInherits(src_layer, &tmp, warn, err)) {
        return false;
      }
      src_layer = std::move(tmp);
    }

    if (src_layer.check_unresolved_variant()) {
      has_unresolved = true;
      // AOUSD Core Spec 10.3.2.5: defer variant composition until references and
      // payloads are resolved (this loop always resolves both), so a strong
      // local variant selection is not consumed against an empty/placeholder
      // variantSet from a weaker layer reached via reference/payload.
      if (!lightusd::ShouldDeferVariantComposition(src_layer)) {
        Layer tmp;
        if (!lightusd::CompositeVariant(src_layer, &tmp, warn, err)) {
          return false;
        }
        src_layer = std::move(tmp);
      }
    }

    if (!has_unresolved) {
      *composited_layer = std::move(src_layer);
      return true;
    }
  }

  if (err) {
    (*err) += "Composition did not converge before the iteration limit.";
  }
  return false;
}

bool ReadAssetBytes(AssetResolutionResolver &resolver, const std::string &assetPath,
                    std::vector<uint8_t> *out, std::string *warn,
                    std::string *err, size_t max_bytes = size_t(256) << 20) {
  const std::string resolved = resolver.resolve(assetPath);
  if (resolved.empty()) {
    // The resolver may not handle absolute filesystem paths.  Fall back to
    // reading the path directly if it is already a filesystem path.
    std::string ioerr;
    if (io::ReadWholeFile(out, &ioerr, assetPath, max_bytes, nullptr)) {
      return true;
    }
    if (err) (*err) = "Asset not found: " + assetPath;
    return false;
  }
  Asset asset;
  std::string owarn;
  if (!resolver.open_asset(resolved, assetPath, &asset, &owarn, err, max_bytes)) {
    return false;
  }
  if (warn && !owarn.empty()) {
    (*warn) += owarn;
  }
  // Cap asset size to avoid unbounded allocation from malicious assets.
  if (asset.size() > max_bytes) {
    if (err) {
      (*err) = "Asset size exceeds configured byte limit.";
    }
    return false;
  }
  // The resolver has already materialized the asset in a vector. Move it into
  // the job instead of copying it and briefly holding two full texture buffers.
  *out = asset.release_buffer();
  return true;
}

bool ReadAssetBytesWithBase(AssetResolutionResolver &resolver,
                            const std::string &assetPath,
                            const std::string &base_dir,
                            std::vector<uint8_t> *out, std::string *warn,
                            std::string *err, size_t max_bytes = size_t(256) << 20) {
  if (!base_dir.empty() && !io::IsAbsPath(assetPath)) {
    const std::string candidate = io::JoinPath(base_dir, assetPath);
    // A dependency's relative assets belong to that layer. A root-layer tile
    // with the same filename must not win through the shared search paths.
    if (io::FileExists(candidate)) {
      return io::ReadWholeFile(out, err, candidate, max_bytes, nullptr);
    }
  }
  return ReadAssetBytes(resolver, assetPath, out, warn, err, max_bytes);
}

// Process one texture's bytes: optionally decode -> resize -> re-encode.
// On success fills `out_bytes` and `out_ext` (lowercase, no dot) and sets the
// resize/reencode flags. Falls back to passthrough on decode failure.
// Bounded producer/consumer pool for texture jobs. `produce(job)` runs on the
// calling thread (asset resolvers are not thread-safe). Without a memory
// budget it runs at most 2*num_threads jobs ahead; with a budget it permits at
// most num_threads jobs in flight so prefetched source buffers do not consume
// another full worker window. Returns false when `produce` reports a hard
// error (workers are drained first).
template <typename Job, typename ProduceFn, typename RunFn>
bool RunBoundedTextureJobs(std::vector<Job> &jobs, int num_threads_option,
                           size_t memory_budget_bytes,
                           const ProduceFn &produce, const RunFn &run) {
#if !defined(LIGHTUSD_ENABLE_THREAD)
  (void)num_threads_option;
  (void)memory_budget_bytes;
  for (Job &job : jobs) {
    if (!produce(job)) {
      return false;
    }
    run(job);
  }
  return true;
#else
  size_t num_threads =
      (num_threads_option > 0)
          ? size_t(num_threads_option)
          : size_t((std::max)(1u, std::thread::hardware_concurrency()));
  num_threads = (std::min)(num_threads, jobs.size());

  // Decoded RGBA pixels, resize scratch, encoder state, source bytes and output
  // can coexist in a worker. Exact dimensions are intentionally unknown until
  // decode, so use the same kind of conservative best-effort worker budgeting
  // as the WASM/JS CLI. The default budget is zero and changes no behavior.
  if (memory_budget_bytes > 0) {
    constexpr size_t kEstimatedBytesPerWorker =
        size_t(192) * 1024 * 1024;
    const size_t budget_threads =
        (std::max)(size_t{1}, memory_budget_bytes / kEstimatedBytesPerWorker);
    num_threads = (std::min)(num_threads, budget_threads);
  }

  if (num_threads <= 1) {
    for (Job &job : jobs) {
      if (!produce(job)) {
        return false;
      }
      run(job);
    }
    return true;
  }

  const size_t window =
      memory_budget_bytes > 0
          ? num_threads
          : (num_threads > (std::numeric_limits<size_t>::max)() / 2
                 ? (std::numeric_limits<size_t>::max)()
                 : num_threads * 2);
  std::mutex mtx;
  std::condition_variable cv;
  size_t produced = 0;   // jobs whose bytes are ready
  size_t completed = 0;  // jobs workers have finished
  std::atomic<size_t> next_job{0};
  bool produce_failed = false;

  std::vector<std::thread> workers;
  workers.reserve(num_threads);
  for (size_t t = 0; t < num_threads; t++) {
    workers.emplace_back([&]() {
      while (true) {
        size_t idx = next_job.fetch_add(1);
        if (idx >= jobs.size()) {
          break;
        }
        {
          std::unique_lock<std::mutex> lock(mtx);
          cv.wait(lock, [&] { return produced > idx || produce_failed; });
          if (idx >= produced) {
            break;  // producer bailed before reaching this job
          }
        }
        run(jobs[idx]);
        {
          std::lock_guard<std::mutex> lock(mtx);
          completed++;
        }
        cv.notify_all();
      }
    });
  }

  bool ok = true;
  for (size_t i = 0; i < jobs.size() && ok; i++) {
    {
      std::unique_lock<std::mutex> lock(mtx);
      cv.wait(lock, [&] { return produced - completed < window; });
    }
    ok = produce(jobs[i]);
    {
      std::lock_guard<std::mutex> lock(mtx);
      if (ok) {
        produced++;
      } else {
        produce_failed = true;
      }
    }
    cv.notify_all();
  }
  if (!ok) {
    {
      std::lock_guard<std::mutex> lock(mtx);
      produce_failed = true;
    }
    cv.notify_all();
  }
  for (std::thread &worker : workers) {
    worker.join();
  }
  return ok;
#endif
}

bool ProcessTexture(const std::vector<uint8_t> &src_bytes,
                    const std::string &src_ext_lower,
                    const UsdzConvertOptions &opts, const std::string &uri,
                    std::vector<uint8_t> *out_bytes, std::string *out_ext,
                    bool *resized, bool *reencoded, std::string *warn) {
  *resized = false;
  *reencoded = false;

  // Decide the target extension.
  std::string target_ext = src_ext_lower;
  if (opts.texture_format == OutputTextureFormat::PNG) {
    target_ext = "png";
  } else if (opts.texture_format == OutputTextureFormat::JPEG) {
    target_ext = "jpg";
  }
  if (opts.arkit_compatible && !IsAllowedARKitTextureExt(target_ext)) {
    target_ext = "png";
  }
  const bool transcode = (target_ext != src_ext_lower) &&
                         !(target_ext == "jpg" && src_ext_lower == "jpeg");

  const bool need_resize = opts.max_texture_size > 0;
  const bool want_process =
      need_resize || transcode || (opts.reencode && (target_ext == "png" ||
                                                     target_ext == "jpg" ||
                                                     target_ext == "jpeg"));

  if (!want_process) {
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  // Decode.
  auto decoded = image::LoadImageFromMemory(src_bytes.data(), src_bytes.size(), uri);
  if (!decoded) {
    if (warn) {
      (*warn) += "Could not decode texture (" + uri + "): " + decoded.error() +
                 " - copying through unchanged.\n";
    }
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  Image img = std::move(decoded.value().image);

  // EXR / non-8bit: don't attempt resize/transcode in v1; pass through.
  if (img.bpp != 8) {
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  // Validate decoded image dimensions (guard against corrupt/malicious headers).
  const int kMaxDimension = 32768;
  if (img.width <= 0 || img.height <= 0 || img.channels < 1 || img.channels > 4) {
    if (warn) {
      (*warn) += "Invalid image dimensions for " + uri +
                 " - copying through unchanged.\n";
    }
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }
  if (img.width > kMaxDimension || img.height > kMaxDimension) {
    if (warn) {
      (*warn) += "Image " + uri + " exceeds max dimension (" +
                 std::to_string(img.width) + "x" + std::to_string(img.height) +
                 ") - copying through unchanged.\n";
    }
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  // Resize (cap longest edge).
  if (need_resize) {
    const int longest = (std::max)(img.width, img.height);
    if (longest > opts.max_texture_size) {
      const double scale = double(opts.max_texture_size) / double(longest);
      int nw = (std::max)(1, int(img.width * scale + 0.5));
      int nh = (std::max)(1, int(img.height * scale + 0.5));
      Image resized_img;
      std::string rerr;
      if (tydra::ResizeImage(img, nw, nh, &resized_img, tydra::ResizeFilter::Auto,
                             &rerr)) {
        img = std::move(resized_img);
        *resized = true;
      } else if (warn) {
        (*warn) += "Resize failed for " + uri + ": " + rerr + "\n";
      }
    }
  }

  // Encode.
  image::WriteOption wopt;
  wopt.png_encoder = opts.png_encoder;
  wopt.jpeg_quality = opts.jpeg_quality;
  if (target_ext == "png") {
    wopt.format = image::WriteImageFormat::PNG;
  } else if (target_ext == "jpg" || target_ext == "jpeg") {
    wopt.format = image::WriteImageFormat::JPEG;
    // JPEG cannot carry alpha; drop it.
    if (img.channels == 4) {
      size_t npix;
      if (!safe::mul(size_t(img.width), size_t(img.height), &npix)) {
        if (warn) {
          (*warn) += "Image dimensions too large for " + uri + ".\n";
        }
        *out_bytes = src_bytes;
        *out_ext = src_ext_lower;
        return true;
      }
      size_t npix4;
      if (!safe::mul(npix, size_t(4), &npix4) || img.data.size() < npix4) {
        if (warn) {
          (*warn) += "Undersized image buffer for " + uri +
                     " - copying through unchanged.\n";
        }
        *out_bytes = src_bytes;
        *out_ext = src_ext_lower;
        return true;
      }
      Image rgb;
      rgb.width = img.width;
      rgb.height = img.height;
      rgb.channels = 3;
      rgb.bpp = 8;
      rgb.format = img.format;
      rgb.colorspace = img.colorspace;
      size_t npix3;
      if (!safe::mul(npix, size_t(3), &npix3)) {
        if (warn) {
          (*warn) += "Image dimensions too large for " + uri + ".\n";
        }
        *out_bytes = src_bytes;
        *out_ext = src_ext_lower;
        return true;
      }
      rgb.data.resize(npix3);
      for (size_t i = 0; i < npix; i++) {
        rgb.data[3 * i + 0] = img.data[4 * i + 0];
        rgb.data[3 * i + 1] = img.data[4 * i + 1];
        rgb.data[3 * i + 2] = img.data[4 * i + 2];
      }
      img = std::move(rgb);
    }
  } else {
    // Unsupported target for re-encode; pass through original bytes.
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  auto enc = image::WriteImageToMemory(img, wopt);
  if (!enc) {
    if (warn) {
      (*warn) += "Encode failed for " + uri + ": " + enc.error() +
                 " - copying through unchanged.\n";
    }
    *out_bytes = src_bytes;
    *out_ext = src_ext_lower;
    return true;
  }

  *out_bytes = std::move(enc.value());
  *out_ext = target_ext;
  *reencoded = true;
  return true;
}

// Expand a UDIM texture asset path (e.g. `diffuse.<UDIM>.png`), pack each
// resolved tile (1001..1100) into `assets`, and produce the archive-side
// `<UDIM>` pattern (e.g. `textures/diffuse.<UDIM>.png`). Each tile is run
// through ProcessTexture (resize/re-encode) like an ordinary texture.
//
// Returns false only on a hard error (caller should abort). On success,
// `*packed` indicates whether at least one tile was packed and
// `*archive_pattern` holds the rewritten `<UDIM>` reference.
bool ExpandAndPackUDIM(const std::string &udim_path,
                       AssetResolutionResolver &resolver,
                       const std::vector<std::string> &search_paths,
                       const std::string &base_dir,
                       const UsdzConvertOptions &options,
                       std::map<std::string, std::vector<uint8_t>> *assets,
                       std::string *archive_pattern, bool *packed,
                       UsdzConvertStats *stats, std::string *warn,
                       std::string *err) {
  (void)err;
  *packed = false;

  std::string prefix, suffix;
  if (!io::SplitUDIMPath(udim_path, &prefix, &suffix)) {
    return true;  // not a UDIM path
  }

  std::string pattern_archive;
  for (uint32_t id = 1001; id <= 1100; id++) {
    const std::string tile_orig = prefix + std::to_string(id) + suffix;

    std::vector<uint8_t> src_bytes;
    std::string rerr;
    if (!ReadAssetBytesWithBase(resolver, tile_orig, base_dir, &src_bytes, warn,
                                &rerr)) {
      continue;  // tile not present
    }

    const std::string src_ext = to_lower(io::GetFileExtension(tile_orig));
    std::vector<uint8_t> out_bytes;
    std::string out_ext;
    bool resized = false, reencoded = false;
    std::string pwarn;
    if (!ProcessTexture(src_bytes, src_ext, options, tile_orig, &out_bytes,
                        &out_ext, &resized, &reencoded, &pwarn)) {
      if (warn) *warn += "Failed to process UDIM tile " + tile_orig + "\n";
      continue;
    }
    if (warn) *warn += pwarn;

    if (!IsAllowedTextureExt(out_ext)) {
      out_ext = "png";
    }

    std::string tile_archive =
        SanitizeArchiveName(RelativeToSearchPath(tile_orig, search_paths));
    if (tile_archive == tile_orig) {
      tile_archive = SanitizeArchiveName(tile_orig);
    }
    if (to_lower(io::GetFileExtension(tile_archive)) != out_ext) {
      tile_archive = ReplaceExtension(tile_archive, out_ext);
    }

    // Avoid collisions across distinct tile bytes.
    std::string unique_name = tile_archive;
    uint32_t collision_suffix = 1;
    while (assets->count(unique_name) &&
           (*assets)[unique_name] != out_bytes) {
      unique_name = MakeCollisionArchiveName(tile_archive, collision_suffix++);
    }
    tile_archive = unique_name;

    (*assets)[tile_archive] = std::move(out_bytes);

    if (pattern_archive.empty()) {
      // Derive the archive `<UDIM>` pattern from the first packed tile by
      // replacing the tile id with the `<UDIM>` token.
      const std::string idstr = std::to_string(id);
      const size_t pos = tile_archive.rfind(idstr);
      if (pos != std::string::npos) {
        pattern_archive = tile_archive.substr(0, pos) + "<UDIM>" +
                          tile_archive.substr(pos + idstr.size());
      } else {
        pattern_archive = tile_archive;
      }
    }

    if (stats) {
      stats->num_textures++;
      if (resized) stats->num_textures_resized++;
      if (reencoded) {
        stats->num_textures_reencoded++;
      } else {
        stats->num_textures_passthrough++;
      }
    }

    Log(options.verbose, "  UDIM tile " + tile_orig + " -> " + tile_archive +
                             (resized ? " [resized]" : "") +
                             (reencoded ? " [reencoded]" : " [passthrough]"));

    *packed = true;
  }

  if (*packed) {
    *archive_pattern = pattern_archive;
  }
  return true;
}

struct NonFlattenLayerPackage {
  Layer layer;
  std::string source_path;
  std::string base_dir;
  std::string archive_name;
};

std::string ResolveDependencySourcePath(AssetResolutionResolver &resolver,
                                        const std::string &asset_path,
                                        const std::string &base_dir) {
  std::string result;
  if (asset_path.empty()) {
    return result;
  }
  if (io::IsAbsPath(asset_path)) {
    result = asset_path;
    return result;
  }
  if (!base_dir.empty()) {
    result = io::JoinPath(base_dir, asset_path);
    if (io::FileExists(result)) {
      return result;
    }
  }
  result = resolver.resolve(asset_path);
  if (result.empty()) {
    result = asset_path;
  }
  return result;
}

bool LoadLayerForPackaging(AssetResolutionResolver &resolver,
                           const std::string &source_path,
                           const std::string &authored_path,
                           Layer *layer, std::string *warn,
                           std::string *err) {
  if (io::FileExists(source_path)) {
    return lightusd::LoadLayerFromFile(source_path, layer, warn, err);
  }

  const std::string resolved = resolver.resolve(source_path);
  if (!resolved.empty()) {
    return lightusd::LoadLayerFromAsset(resolver, resolved, layer, warn, err);
  }

  const std::string authored_resolved = resolver.resolve(authored_path);
  if (!authored_resolved.empty()) {
    return lightusd::LoadLayerFromAsset(resolver, authored_resolved, layer, warn,
                                        err);
  }

  if (err) {
    *err = "Failed to resolve layer asset: " + authored_path;
  }
  return false;
}

std::string MakeLayerArchiveName(const std::string &source_path,
                                 const std::string &authored_path,
                                 const std::vector<std::string> &search_paths,
                                 USDZRootLayerFormat format) {
  std::string name = RelativeToSearchPath(source_path, search_paths);
  if (name == source_path && !authored_path.empty()) {
    name = authored_path;
  }
  name = SanitizeLayerArchiveName(name);
  const std::string ext = (format == USDZRootLayerFormat::USDA) ? "usda" : "usdc";
  if (to_lower(io::GetFileExtension(name)) != ext) {
    name = ReplaceExtension(name, ext);
  }
  return name;
}

bool SerializeLayerForUSDZ(const Layer &layer, USDZRootLayerFormat format,
                           std::vector<uint8_t> *out, std::string *warn,
                           std::string *err) {
  if (format == USDZRootLayerFormat::USDA) {
    const std::string text = lightusd::print_layer(layer, 0);
    if (text.empty()) {
      if (err) *err = "USDA serialization produced empty data.";
      return false;
    }
    out->assign(text.begin(), text.end());
    return true;
  }

  return lightusd::usdc::SaveAsUSDCToMemory(layer, out, warn, err);
}

bool ConvertNonFlattenUSDZ(const UsdzConvertOptions &options,
                           AssetResolutionResolver &resolver,
                           const std::vector<std::string> &search_paths,
                           UsdzConvertStats *stats, std::string *warn,
                           std::string *err) {
  const std::string &root_input = options.inputs[0];
  Log(options.verbose, "Loading + packaging layers (no flatten): " + root_input);

  std::vector<NonFlattenLayerPackage> packages;
  packages.push_back(NonFlattenLayerPackage{});
  std::string lwarn, lerr;
  if (!lightusd::LoadLayerFromFile(root_input, &packages[0].layer, &lwarn,
                                   &lerr)) {
    if (err) *err = "Failed to load root layer: " + lerr;
    return false;
  }
  if (warn) *warn += lwarn;
  packages[0].source_path = root_input;
  packages[0].base_dir = io::GetBaseDir(root_input);

  if (options.upAxis != Axis::Invalid) {
    packages[0].layer.metas().upAxis.set_value(options.upAxis);
  } else if (options.arkit_compatible) {
    packages[0].layer.metas().upAxis.set_value(Axis::Y);
  }
  if (options.metersPerUnit > 0.0) {
    packages[0].layer.metas().metersPerUnit.set_value(options.metersPerUnit);
  }
  if (!options.copyright.empty() || !options.url.empty()) {
    std::string doc;
    if (!options.copyright.empty()) doc += options.copyright;
    if (!options.url.empty()) {
      if (!doc.empty()) doc += "\n";
      doc += options.url;
    }
    packages[0].layer.metas().doc = doc;
  }

  std::map<std::string, size_t> source_to_package;
  source_to_package[root_input] = 0;
  std::vector<std::map<std::string, std::string>> composition_remaps;
  composition_remaps.resize(1);

  std::deque<size_t> queue;
  queue.push_back(0);
  constexpr size_t kMaxLayerCount = 1000;
  while (!queue.empty()) {
    const size_t package_index = queue.front();
    queue.pop_front();

    std::vector<std::string> deps;
    CollectCompositionAssetPaths(packages[package_index].layer, &deps);
    for (const std::string &dep : deps) {
      const std::string source = ResolveDependencySourcePath(
          resolver, dep, packages[package_index].base_dir);

      auto seen = source_to_package.find(source);
      if (seen == source_to_package.end()) {
        if (packages.size() >= kMaxLayerCount) {
          if (err) *err = "Too many layer dependencies while packaging USDZ.";
          return false;
        }

        NonFlattenLayerPackage pkg;
        pkg.source_path = source;
        pkg.base_dir = io::GetBaseDir(source);
        pkg.archive_name = MakeLayerArchiveName(source, dep, search_paths,
                                                USDZRootLayerFormat::USDA);

        uint32_t suffix = 1;
        bool archive_collision = true;
        while (archive_collision) {
          archive_collision = false;
          for (const NonFlattenLayerPackage &existing : packages) {
            if (existing.archive_name == pkg.archive_name) {
              archive_collision = true;
              pkg.archive_name = MakeCollisionArchiveName(pkg.archive_name,
                                                          suffix++);
              break;
            }
          }
        }

        std::string dwarn, derr;
        if (!LoadLayerForPackaging(resolver, source, dep, &pkg.layer, &dwarn,
                                   &derr)) {
          if (err) *err = "Failed to load dependency layer: " + derr;
          return false;
        }
        if (warn) *warn += dwarn;

        const size_t new_index = packages.size();
        source_to_package[source] = new_index;
        composition_remaps.push_back(std::map<std::string, std::string>());
        packages.push_back(std::move(pkg));
        queue.push_back(new_index);
        seen = source_to_package.find(source);
      }

      composition_remaps[package_index][dep] =
          packages[seen->second].archive_name;
    }
  }

  for (size_t i = 0; i < packages.size(); i++) {
    if (!composition_remaps[i].empty()) {
      const size_t n = RemapCompositionAssetPaths(packages[i].layer,
                                                  composition_remaps[i]);
      Log(options.verbose, "Remapped " + std::to_string(n) +
                               " composition path(s) in package layer " +
                               std::to_string(i) + ".");
    }
  }

  struct TextureRef {
    size_t package_index{0};
    std::string authored_path;
    std::string source_path;
  };

  std::map<std::string, std::vector<uint8_t>> assets;
  if (options.udim_bake != udim::BakeMode::Off) {
    for (auto& package : packages) {
      const auto fetch = [&](const std::string& path, std::vector<uint8_t>* bytes, std::string* error, size_t limit) {
        return ReadAssetBytesWithBase(resolver, path, package.base_dir, bytes, warn, error, limit);
      };
      const auto exists = [&](const std::string& path) {
        return io::FileExists(io::JoinPath(package.base_dir, path)) || resolver.find(path);
      };
      if (!BakeUDIMInLayer(options, &package.layer, fetch, exists, &assets, stats, err)) return false;
    }
  }
  std::vector<TextureRef> texture_refs;
  for (size_t i = 0; i < packages.size(); i++) {
    std::vector<std::string> paths;
    CollectLayerTextureAssetPaths(packages[i].layer, &paths);
    for (const std::string &path : paths) {
      TextureRef ref;
      ref.package_index = i;
      ref.authored_path = path;
      ref.source_path = ResolveDependencySourcePath(resolver, path,
                                                    packages[i].base_dir);
      texture_refs.push_back(std::move(ref));
    }
  }
  Log(options.verbose, "Found " + std::to_string(texture_refs.size()) +
                           " texture reference(s).");

  std::map<std::string, std::string> source_to_archive;
  std::vector<std::map<std::string, std::string>> texture_remaps;
  texture_remaps.resize(packages.size());

  // Texture packing runs in three phases so the heavy per-texture
  // decode/resize/encode can use a worker pool:
  //   1. (sequential) dedupe, UDIM expansion, asset reads -> job list
  //   2. (parallel)   ProcessTexture per job
  //   3. (sequential, in job order) archive naming, stats, remaps
  // Phase 1/3 stay sequential because they touch the resolver and the shared
  // assets/remap maps; ProcessTexture is a pure transform of the input bytes.
  struct TextureJob {
    std::string authored_path;  // representative authored path (first ref)
    std::string source_path;
    std::string archive_name;  // pre-collision-resolution name
    std::vector<uint8_t> src_bytes;
    std::string src_ext;
    // refs (package_index, authored_path) to remap to this job's result
    std::vector<std::pair<size_t, std::string>> refs;
    size_t package_index{0};  // for the lazy phase-2 read (base_dir)
    bool skipped{false};      // unreadable-but-safe path; left as missing ref
    // results (phase 2)
    std::vector<uint8_t> out_bytes;
    std::string out_ext;
    bool resized{false};
    bool reencoded{false};
    bool ok{false};
    std::string pwarn;
  };

  std::vector<TextureJob> jobs;
  std::map<std::string, size_t> source_to_job;

  for (const TextureRef &ref : texture_refs) {
    if (assets.count(ref.authored_path)) {
      texture_remaps[ref.package_index][ref.authored_path] = ref.authored_path;
      if (stats) { ++stats->num_textures; ++stats->num_textures_reencoded; }
      continue;
    }
    auto existing = source_to_archive.find(ref.source_path);
    if (existing != source_to_archive.end()) {
      texture_remaps[ref.package_index][ref.authored_path] = existing->second;
      continue;
    }

    auto pending = source_to_job.find(ref.source_path);
    if (pending != source_to_job.end()) {
      jobs[pending->second].refs.emplace_back(ref.package_index,
                                              ref.authored_path);
      continue;
    }

    // UDIM texture: expand to tiles, pack each, and rewrite the authored
    // `<UDIM>` reference to the archive-side `<UDIM>` pattern.
    if (io::IsUDIMPath(ref.authored_path)) {
      std::string udim_pattern;
      bool udim_packed = false;
      if (!ExpandAndPackUDIM(ref.authored_path, resolver, search_paths,
                             packages[ref.package_index].base_dir, options,
                             &assets, &udim_pattern, &udim_packed, stats, warn,
                             err)) {
        return false;
      }
      if (udim_packed) {
        source_to_archive[ref.source_path] = udim_pattern;
        texture_remaps[ref.package_index][ref.authored_path] = udim_pattern;
      } else if (warn) {
        *warn += "No UDIM tiles found for '" + ref.authored_path + "'.\n";
      }
      continue;
    }

    std::string archive_name = SanitizeArchiveName(
        RelativeToSearchPath(ref.source_path, search_paths));
    if (archive_name == ref.source_path) {
      archive_name = SanitizeArchiveName(ref.authored_path);
    }

    TextureJob job;
    job.authored_path = ref.authored_path;
    job.source_path = ref.source_path;
    job.package_index = ref.package_index;
    job.archive_name = std::move(archive_name);
    // Asset bytes are read lazily by the phase-2 producer (bounded window).
    job.src_ext = to_lower(io::GetFileExtension(ref.authored_path));
    job.refs.emplace_back(ref.package_index, ref.authored_path);
    source_to_job[ref.source_path] = jobs.size();
    jobs.push_back(std::move(job));
  }

  // Phase 2: bounded producer/consumer — the main thread reads asset bytes
  // (the resolver is not thread-safe), workers decode/resize/encode; at most
  // num_threads source buffers are in flight when a memory budget is set
  // (2*num_threads otherwise).
  {
    auto run_job = [&options](TextureJob &job) {
      if (job.skipped) {
        std::vector<uint8_t>().swap(job.src_bytes);
        return;
      }
      job.ok = ProcessTexture(job.src_bytes, job.src_ext, options,
                              job.authored_path, &job.out_bytes, &job.out_ext,
                              &job.resized, &job.reencoded, &job.pwarn);
      std::vector<uint8_t>().swap(job.src_bytes);
    };

    auto produce_job = [&](TextureJob &job) -> bool {
      std::string rerr;
      if (!ReadAssetBytesWithBase(resolver, job.authored_path,
                                  packages[job.package_index].base_dir,
                                  &job.src_bytes, warn, &rerr)) {
        const std::string missing_ref =
            SafeMissingArchiveReference(job.authored_path, search_paths);
        if (missing_ref.empty()) {
          if (err) {
            *err = "Unsafe texture path could not be packed: " +
                   job.authored_path + " (" + rerr + ")";
          }
          return false;
        }
        for (const auto &r : job.refs) {
          texture_remaps[r.first][r.second] = missing_ref;
        }
        if (warn) {
          *warn += "Skipping unreadable texture '" + job.authored_path + "': " +
                   rerr + "\n";
        }
        job.skipped = true;
      }
      return true;
    };

    if (!RunBoundedTextureJobs(jobs, options.num_threads,
                               options.texture_memory_budget_bytes,
                               produce_job, run_job)) {
      return false;
    }
  }

  // Phase 3: archive naming, stats and remaps, deterministically in job order.
  for (TextureJob &job : jobs) {
    if (job.skipped) {
      continue;  // unreadable; already warned + safe missing reference left
    }
    if (!job.ok) {
      if (warn) {
        *warn += "Failed to process texture " + job.authored_path + "\n";
      }
      continue;
    }
    if (warn) *warn += job.pwarn;

    std::string archive_name = std::move(job.archive_name);
    std::string out_ext = std::move(job.out_ext);

    if (options.arkit_compatible && !IsAllowedARKitTextureExt(out_ext)) {
      if (err) {
        *err = "ARKit-compatible USDZ cannot package texture '" +
               job.authored_path + "' with unsupported output extension '" +
               out_ext + "'.";
      }
      return false;
    }

    if (!IsAllowedTextureExt(out_ext)) {
      out_ext = "png";
    }
    if (to_lower(io::GetFileExtension(archive_name)) != out_ext) {
      archive_name = ReplaceExtension(archive_name, out_ext);
    }

    std::string unique_name = archive_name;
    constexpr uint32_t kMaxSuffix = 100000;
    uint32_t suffix = 1;
    while (assets.count(unique_name) && assets[unique_name] != job.out_bytes) {
      if (suffix >= kMaxSuffix) {
        unique_name = archive_name;
        break;
      }
      unique_name = MakeCollisionArchiveName(archive_name, suffix++);
    }
    archive_name = unique_name;

    assets[archive_name] = std::move(job.out_bytes);
    source_to_archive[job.source_path] = archive_name;
    for (const auto &r : job.refs) {
      texture_remaps[r.first][r.second] = archive_name;
    }

    if (stats) {
      stats->num_textures++;
      if (job.resized) stats->num_textures_resized++;
      if (job.reencoded) {
        stats->num_textures_reencoded++;
      } else {
        stats->num_textures_passthrough++;
      }
    }
    Log(options.verbose, "  " + job.authored_path + " -> " + archive_name +
                             (job.resized ? " [resized]" : "") +
                             (job.reencoded ? " [reencoded]" : " [passthrough]"));
  }

  for (size_t i = 0; i < packages.size(); i++) {
    if (!texture_remaps[i].empty()) {
      RemapLayerAssetPaths(packages[i].layer, texture_remaps[i]);
    }
  }

  for (size_t i = 1; i < packages.size(); i++) {
    std::vector<uint8_t> layer_bytes;
    std::string swarn, serr;
    if (!SerializeLayerForUSDZ(packages[i].layer, USDZRootLayerFormat::USDA,
                               &layer_bytes,
                               &swarn, &serr)) {
      if (err) *err = "Failed to serialize dependency layer: " + serr;
      return false;
    }
    if (warn) *warn += swarn;
    assets[packages[i].archive_name] = std::move(layer_bytes);
  }

  lightusd::USDZWriteOptions write_options;
  write_options.root_layer_format = lightusd::USDZRootLayerFormat::USDA;
  if (options.usdz_root_layer_format == USDZRootLayerFormat::USDC && warn) {
    *warn += "Non-flatten USDZ uses a USDA root layer to preserve composition "
             "arcs for downstream loaders.\n";
  }

  std::string swarn, serr;
  if (!lightusd::SaveAsUSDZToFile(options.output, packages[0].layer, assets,
                                  write_options, &swarn, &serr)) {
    if (err) *err = "Failed to write USDZ: " + serr;
    return false;
  }
  if (warn) *warn += swarn;

  std::vector<uint8_t> usdz_bytes;
  std::string ioerr;
  if (io::ReadWholeFile(&usdz_bytes, &ioerr, options.output,
                        /*max*/ 512 * 1024 * 1024)) {
    std::string vwarn, verr;
    if (!lightusd::ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(), &vwarn,
                                &verr)) {
      if (warn) *warn += "USDZ validation reported issues: " + verr + "\n";
    } else {
      if (warn) *warn += vwarn;
    }
    if (stats) stats->output_size = usdz_bytes.size();
  } else if (warn) {
    *warn += "Could not read output USDZ for validation: " + ioerr + "\n";
  }

  Log(options.verbose, "Packaged " + std::to_string(packages.size()) +
                           " layer(s) without flattening.");
  return true;
}

}  // namespace

size_t RemapTextureAssetPaths(Stage &stage,
                              const std::map<std::string, std::string> &remap) {
  std::vector<std::pair<UsdUVTexture *, std::string>> textures;
  for (auto &p : stage.root_prims()) {
    CollectTextures(p, &textures);
  }
  size_t n = 0;
  for (auto &kv : textures) {
    auto it = remap.find(kv.second);
    if (it != remap.end() && it->second != kv.second) {
      WriteTextureFilePath(kv.first, it->second);
      n++;
    }
  }
  return n;
}

bool Convert(const UsdzConvertOptions &options, UsdzConvertStats *stats,
             std::string *warn, std::string *err) {
  if (options.inputs.empty()) {
    if (err) (*err) = "No input USD file specified.";
    return false;
  }
  if (options.output.empty()) {
    if (err) (*err) = "No output USDZ file specified.";
    return false;
  }
  if (options.max_texture_size < 0) {
    if (err) (*err) = "max_texture_size must be non-negative.";
    return false;
  }

  if (!udim::ValidateOptions(UDIMOptions(options), err)) return false;
  if (options.udim_bake == udim::BakeMode::Dense && !options.flatten) {
    if (err) *err = "Dense UDIM baking requires flattened output.";
    return false;
  }

  PhaseTimer timer(options.verbose);

  const std::string &root_input = options.inputs[0];

  // --- Asset resolution setup (handles on-disk and in-USDZ assets) ---
  AssetResolutionResolver resolver;
  USDZAsset usdz_asset;  // kept alive for the resolver's lifetime
  bool root_is_usdz = false;
  {
    std::string fmt;
    if (lightusd::IsUSD(root_input, &fmt) && fmt == "usdz") {
      root_is_usdz = true;
    }
  }

  std::vector<std::string> search_paths;
  for (const auto &in : options.inputs) {
    const std::string dir = io::GetBaseDir(in);
    if (!dir.empty()) {
      search_paths.push_back(dir);
    }
  }
  if (search_paths.empty()) {
    search_paths.push_back("./");
  }
  resolver.set_search_paths(search_paths);

  if (root_is_usdz) {
    std::string w;
    if (!lightusd::ReadUSDZAssetInfoFromFile(root_input, &usdz_asset, &w, err)) {
      if (err) (*err) = "Failed to read USDZ asset info: " + (err ? *err : std::string());
      return false;
    }
    if (warn) (*warn) += w;
    if (!lightusd::SetupUSDZAssetResolution(resolver, &usdz_asset)) {
      if (err) (*err) = "Failed to set up USDZ asset resolution for: " + root_input;
      return false;
    }
  }

  const bool material_optimization_enabled =
      options.material_optimization != MaterialOptimizationMode::Off;
  const bool geometry_optimization_enabled =
      options.geometry_optimization != GeometryOptimizationMode::Off;
  const bool use_flattened_output =
      options.flatten || options.arkit_compatible ||
      material_optimization_enabled || geometry_optimization_enabled;

  if (!options.flatten && options.arkit_compatible && warn) {
    *warn += "ARKit-compatible USDZ requires a flattened package; ignoring "
             "-noFlatten.\n";
  }
  if (!options.flatten && material_optimization_enabled && warn) {
    *warn += "Material optimization requires a flattened package; ignoring "
             "-noFlatten.\n";
  }
  if (!options.flatten && geometry_optimization_enabled && warn) {
    *warn += "Geometry optimization requires a flattened package; ignoring "
             "-noFlatten.\n";
  }

  if (!use_flattened_output &&
      options.output_format == OutputFormat::USDZ) {
    return ConvertNonFlattenUSDZ(options, resolver, search_paths, stats, warn,
                                 err);
  }

  // --- Load + (optionally) flatten the stage ---
  Stage stage;
  Layer layer_for_write;
  bool has_layer_for_write = false;
  std::string lwarn, lerr;

  if (use_flattened_output) {
    Log(options.verbose, "Loading + compositing (flatten): " + root_input);
    timer.begin("load-layer");
    Layer root_layer;
    if (!lightusd::LoadLayerFromFile(root_input, &root_layer, &lwarn, &lerr)) {
      if (err) (*err) = "Failed to load layer: " + lerr;
      return false;
    }
    // Additional inputs become (weaker) sublayers.
    for (size_t i = 1; i < options.inputs.size(); i++) {
      SubLayer sl;
      sl.assetPath = value::AssetPath(options.inputs[i]);
      root_layer.metas().subLayers.push_back(sl);
    }

    timer.begin("compose-fixedpoint");
    Layer composited;
    if (!ComposeLayerToFixedPoint(resolver, root_layer, &composited, &lwarn,
                                  &lerr)) {
      if (err) (*err) = "Composition failed: " + lerr;
      return false;
    }
    layer_for_write = std::move(composited);
    has_layer_for_write = true;
    timer.begin("layer-to-stage");
    if (!lightusd::LayerToStage(Layer(layer_for_write), &stage, &lwarn, &lerr)) {
      if (err) (*err) = "LayerToStage failed: " + lerr;
      return false;
    }
  } else {
    Log(options.verbose, "Loading (no flatten): " + root_input);
    timer.begin("load-layer");
    // Load as a Layer (no composition) so usdc/usda output goes through the
    // faithful PrimSpec-based writers. Loading into a Stage here would route
    // output through the typed Stage->crate reconstruction, which injects
    // schema fallbacks (purpose/visibility), drops relationships not modeled by
    // the typed schema (e.g. skel:animationSource), and loses `uniform`
    // variability. (USDZ -noFlatten is handled earlier by ConvertNonFlattenUSDZ;
    // all downstream `stage` uses below are guarded by !has_layer_for_write.)
    if (!lightusd::LoadLayerFromFile(root_input, &layer_for_write, &lwarn,
                                     &lerr)) {
      if (err) (*err) = "Failed to load layer: " + lerr;
      return false;
    }
    has_layer_for_write = true;
  }
  if (warn) (*warn) += lwarn;

  if (options.arkit_compatible) {
    if (has_layer_for_write) {
      CollectARKitCompatibilityWarnings(layer_for_write, warn);
    } else if (warn) {
      *warn += "ARKit compatibility: unable to inspect Layer-level prim "
               "metadata for warnings.\n";
    }
  }

  // --- ARKit / metadata fixups ---
  if (options.upAxis != Axis::Invalid) {
    stage.metas().upAxis.set_value(options.upAxis);
    if (has_layer_for_write) {
      layer_for_write.metas().upAxis.set_value(options.upAxis);
    }
  } else if (options.arkit_compatible) {
    // ARKit expects Y-up.
    stage.metas().upAxis.set_value(Axis::Y);
    if (has_layer_for_write) {
      layer_for_write.metas().upAxis.set_value(Axis::Y);
    }
  }
  if (options.metersPerUnit > 0.0) {
    stage.metas().metersPerUnit.set_value(options.metersPerUnit);
    if (has_layer_for_write) {
      layer_for_write.metas().metersPerUnit.set_value(options.metersPerUnit);
    }
  }
  if (!options.copyright.empty() || !options.url.empty()) {
    std::string doc;
    if (!options.copyright.empty()) doc += options.copyright;
    if (!options.url.empty()) {
      if (!doc.empty()) doc += "\n";
      doc += options.url;
    }
    stage.metas().doc = doc;
    if (has_layer_for_write) {
      layer_for_write.metas().doc = doc;
    }
  }

  // --- Optional material/shader optimization ---
  if (options.material_optimization != MaterialOptimizationMode::Off) {
    if (!has_layer_for_write) {
      if (warn) {
        *warn += "Material optimization requires Layer-based conversion; "
                 "skipping.\n";
      }
    } else {
      Log(options.verbose, "Optimizing materials.");
      timer.begin("optimize-materials");
      MaterialOptimizationStats opt_stats;
      std::string owarn, oerr;
      if (!OptimizeMaterialsInLayer(options, &layer_for_write, &opt_stats,
                                    &owarn, &oerr)) {
        if (err) {
          *err = "Material optimization failed: " + oerr;
        }
        return false;
      }
      if (warn) {
        *warn += owarn;
      }
      if (stats) {
        stats->num_materials_before = opt_stats.num_materials_before;
        stats->num_materials_after = opt_stats.num_materials_after;
        stats->num_materials_deduped = opt_stats.num_materials_deduped;
        stats->num_materials_preview_converted =
            opt_stats.num_materials_preview_converted;
        stats->num_materials_skipped = opt_stats.num_materials_skipped;
        stats->num_material_atlas_materials = opt_stats.num_atlas_materials;
        stats->num_material_atlas_textures = opt_stats.num_atlas_textures;
      }
      Log(options.verbose,
          "Materials: " + std::to_string(opt_stats.num_materials_before) +
              " -> " + std::to_string(opt_stats.num_materials_after) +
              ", deduped " +
              std::to_string(opt_stats.num_materials_deduped) + ".");
      if (options.arkit_compatible) {
        std::string rwarn, rerr;
        if (!lightusd::LayerToStage(Layer(layer_for_write), &stage, &rwarn,
                                    &rerr)) {
          if (err) {
            *err = "LayerToStage after material optimization failed: " + rerr;
          }
          return false;
        }
        if (warn) {
          *warn += rwarn;
        }
      }
    }
  }

  // --- Optional geometry optimization ---
  if (options.geometry_optimization != GeometryOptimizationMode::Off) {
    if (!has_layer_for_write) {
      if (warn) {
        *warn += "Geometry optimization requires Layer-based conversion; "
                 "skipping.\n";
      }
    } else {
      Log(options.verbose, "Optimizing geometry.");
      timer.begin("optimize-geometry");
      GeometryOptimizationStats opt_stats;
      std::string owarn, oerr;
      if (!OptimizeGeometryInLayer(options, &layer_for_write, &opt_stats,
                                   &owarn, &oerr)) {
        if (err) {
          *err = "Geometry optimization failed: " + oerr;
        }
        return false;
      }
      if (warn) {
        *warn += owarn;
      }
      if (stats) {
        stats->num_meshes_before = opt_stats.num_meshes_before;
        stats->num_meshes_after = opt_stats.num_meshes_after;
        stats->num_meshes_eligible = opt_stats.num_meshes_eligible;
        stats->num_meshes_merged = opt_stats.num_meshes_merged;
        stats->num_meshes_skipped = opt_stats.num_meshes_skipped;
        stats->num_mesh_aggregates = opt_stats.num_mesh_aggregates;
        stats->num_faces_merged = opt_stats.num_faces_merged;
        stats->num_points_merged = opt_stats.num_points_merged;
      }
      Log(options.verbose,
          "Meshes: " + std::to_string(opt_stats.num_meshes_before) + " -> " +
              std::to_string(opt_stats.num_meshes_after) + ", merged " +
              std::to_string(opt_stats.num_meshes_merged) + " into " +
              std::to_string(opt_stats.num_mesh_aggregates) +
              " aggregate(s).");
    }
  }

  std::map<std::string, std::vector<uint8_t>> assets;
  std::map<std::string, std::string> udim_sidecars;
  std::string udim_sidecar_directory;
  if (options.udim_bake != udim::BakeMode::Off) {
    const auto fetch = [&](const std::string& path, std::vector<uint8_t>* bytes, std::string* error, size_t limit) {
      return ReadAssetBytes(resolver, path, bytes, warn, error, limit);
    };
    const auto exists = [&](const std::string& path) {
      return io::FileExists(path) || resolver.find(path);
    };
    if (!BakeUDIMInLayer(options, &layer_for_write, fetch, exists, &assets, stats, err)) return false;
    if (options.output_format != OutputFormat::USDZ && !assets.empty()) {
      std::string dir = io::GetBaseFilename(options.output) + "_textures";
      const std::string out_dir = io::GetBaseDir(options.output);
      udim_sidecar_directory = io::JoinPath(out_dir, dir);
      std::map<std::string, std::string> remap;
      for (const auto& asset : assets) {
        const std::string rel = dir + "/" + io::GetBaseFilename(asset.first);
        udim_sidecars[asset.first] = io::JoinPath(out_dir, rel);
        remap[asset.first] = rel;
      }
      RemapLayerAssetPaths(layer_for_write, remap);
    }
  }

  // --- Enumerate textures ---
  timer.begin("enumerate-textures");
  std::vector<std::pair<UsdUVTexture *, std::string>> textures;
  std::vector<std::string> texture_paths;
  if (has_layer_for_write) {
    CollectLayerTextureAssetPaths(layer_for_write, &texture_paths);
  } else {
    for (auto &p : stage.root_prims()) {
      CollectTextures(p, &textures);
    }
    for (const auto &texture : textures) {
      texture_paths.push_back(texture.second);
    }
  }
  Log(options.verbose,
      "Found " + std::to_string(texture_paths.size()) + " texture reference(s).");
  if (options.output_format == OutputFormat::USDZ &&
      options.include_unused_textures) {
    std::vector<std::string> unused_texture_paths;
    CollectUnusedTextureFilePaths(options.inputs, texture_paths,
                                  &unused_texture_paths);
    if (!unused_texture_paths.empty()) {
      texture_paths.insert(texture_paths.end(), unused_texture_paths.begin(),
                           unused_texture_paths.end());
      if (stats) {
        stats->num_unused_textures_included = unused_texture_paths.size();
      }
      Log(options.verbose,
          "Including " + std::to_string(unused_texture_paths.size()) +
              " unreferenced texture file(s).");
    }
  }

  // Texture packing is only relevant when writing a USDZ archive.
  // For flat USDC/USDA output, textures remain as external references.
  std::map<std::string, std::string> path_to_archive;  // original -> archive name

  if (options.output_format == OutputFormat::USDZ) {
    Log(options.verbose,
        "Processing textures for USDZ packaging.");
    timer.begin("process-textures");

  const bool budget_mode = options.target_texture_bytes > 0;

  // Budget mode: decode all unique textures and shrink them globally to fit
  // options.target_texture_bytes before packing.
  std::map<std::string, std::pair<std::vector<uint8_t>, std::string>> fitted;  // orig -> (bytes, ext)
  if (budget_mode) {
    std::vector<std::string> fit_paths;
    std::vector<tydra::FitTextureInput> fit_inputs;
    std::set<std::string> seen;
    for (const std::string &orig : texture_paths) {
      if (seen.count(orig)) continue;
      seen.insert(orig);

      // UDIM textures are expanded and packed per-tile in the main loop below
      // (the budget fitter operates on single decoded images).
      if (io::IsUDIMPath(orig)) continue;

      std::vector<uint8_t> src_bytes;
      std::string rerr;
      if (assets.count(orig)) src_bytes = assets.at(orig);
      if (src_bytes.empty() && !ReadAssetBytes(resolver, orig, &src_bytes, warn, &rerr)) {
        const std::string missing_ref =
            SafeMissingArchiveReference(orig, search_paths);
        if (missing_ref.empty()) {
          if (err) {
            (*err) = "Unsafe texture path could not be packed: " + orig +
                     " (" + rerr + ")";
          }
          return false;
        }
        path_to_archive[orig] = missing_ref;
        if (warn) (*warn) += "Skipping unreadable texture '" + orig + "': " + rerr + "\n";
        continue;
      }

      tydra::FitTextureInput fi;
      fi.ext = to_lower(io::GetFileExtension(orig));
      auto dec = image::LoadImageFromMemory(src_bytes.data(), src_bytes.size(), orig);
      if (dec && dec.value().image.bpp == 8) {
        fi.image = std::move(dec.value().image);
        fi.reencodable = true;
      } else {
        fi.reencodable = false;  // EXR/16-bit: keep as-is (fixed overhead)
        fi.original_bytes = src_bytes;  // only needed when not reencodable
      }
      fit_paths.push_back(orig);
      fit_inputs.push_back(std::move(fi));
    }

    tydra::FitTextureOptions fopts;
    fopts.target_total_bytes = options.target_texture_bytes;
    fopts.strategy = (options.fit_strategy == FitStrategy::Quality)
                         ? tydra::FitStrategy::Quality
                         : tydra::FitStrategy::Size;
    fopts.start_max_size = options.max_texture_size;
    fopts.min_texture_size = options.fit_min_texture_size;
    fopts.min_jpeg_quality = options.fit_min_jpeg_quality;
    fopts.jpeg_quality = options.jpeg_quality;
    fopts.png_encoder = options.png_encoder;

    std::vector<tydra::FitTextureOutput> fouts;
    std::string fwarn, ferr;
    if (!tydra::FitTexturesToBudget(fit_inputs, fopts, &fouts, &fwarn, &ferr)) {
      if (err) (*err) = "Texture budget fit failed: " + ferr;
      return false;
    }
    if (warn) (*warn) += fwarn;
    if (fouts.size() != fit_paths.size()) {
      if (err) (*err) = "Internal error: FitTexturesToBudget returned wrong number of outputs.";
      return false;
    }
    for (size_t i = 0; i < fit_paths.size(); i++) {
      fitted[fit_paths[i]] = {std::move(fouts[i].bytes), fouts[i].ext};
    }
  }

  // Texture packing runs in three phases so the heavy per-texture
  // decode/resize/encode can use a worker pool (options.num_threads):
  //   1. (sequential) dedupe, UDIM expansion, budget lookups, asset reads
  //   2. (parallel)   ProcessTexture per job
  //   3. (sequential, in job order) archive naming, stats, remaps
  // Phase 1/3 stay sequential because they touch the resolver and the shared
  // assets/remap maps; ProcessTexture is a pure transform of the input bytes.
  struct FlattenTextureJob {
    std::string orig;
    std::string archive_name;  // pre-collision-resolution name
    std::vector<uint8_t> src_bytes;
    std::string src_ext;
    bool needs_process{false};  // false: out_* prefilled (budget mode)
    bool skipped{false};        // unreadable-but-safe path; left as missing ref
    // results
    std::vector<uint8_t> out_bytes;
    std::string out_ext;
    bool resized{false};
    bool reencoded{false};
    bool ok{false};
    std::string pwarn;
  };

  std::vector<FlattenTextureJob> texture_jobs;
  std::set<std::string> queued_paths;

  for (const std::string &orig : texture_paths) {
    const std::string orig_key = TexturePathKey(orig);
    if (path_to_archive.count(orig) || queued_paths.count(orig_key)) {
      continue;  // already processed or queued
    }

    // UDIM texture: expand to tiles, pack each, and rewrite the authored
    // `<UDIM>` reference to the archive-side `<UDIM>` pattern.
    if (io::IsUDIMPath(orig)) {
      std::string udim_pattern;
      bool udim_packed = false;
      if (!ExpandAndPackUDIM(orig, resolver, search_paths, /*base_dir*/ "",
                             options, &assets, &udim_pattern, &udim_packed,
                             stats, warn, err)) {
        return false;
      }
      if (udim_packed) {
        path_to_archive[orig] = udim_pattern;
        Log(options.verbose, "  " + orig + " -> " + udim_pattern + " [UDIM]");
      } else if (warn) {
        (*warn) += "No UDIM tiles found for '" + orig + "'.\n";
      }
      continue;
    }

    if (!budget_mode && assets.count(orig)) {
      // These images were already decoded, resized, and encoded by the baker.
      path_to_archive[orig] = orig;
      if (stats) { ++stats->num_textures; ++stats->num_textures_reencoded; }
      continue;
    }
    FlattenTextureJob job;
    job.orig = orig;
    job.archive_name =
        SanitizeArchiveName(RelativeToSearchPath(orig, search_paths));

    if (budget_mode) {
      auto it = fitted.find(orig);
      if (it == fitted.end()) {
        // Unreadable safe relative path; leave the original reference unchanged.
        continue;
      }
      job.out_bytes = it->second.first;
      job.out_ext = it->second.second;
      job.resized = (options.fit_strategy == FitStrategy::Size);
      job.reencoded = true;
      job.ok = true;
    } else {
      job.src_ext = to_lower(io::GetFileExtension(orig));
      // Asset bytes are read lazily by the phase-2 producer (bounded window)
      // so peak memory holds a handful of textures, not the whole set.
      job.needs_process = true;
    }

    queued_paths.insert(orig_key);
    texture_jobs.push_back(std::move(job));
  }

  // Phase 2: bounded producer/consumer — the main thread reads asset bytes
  // (the resolver is not thread-safe), workers decode/resize/encode; at most
  // num_threads source buffers are in flight when a memory budget is set
  // (2*num_threads otherwise).
  {
    auto run_job = [&options](FlattenTextureJob &job) {
      if (!job.needs_process || job.skipped) {
        std::vector<uint8_t>().swap(job.src_bytes);
        return;  // budget mode prefilled the result, or the read was skipped
      }
      job.ok = ProcessTexture(job.src_bytes, job.src_ext, options, job.orig,
                              &job.out_bytes, &job.out_ext, &job.resized,
                              &job.reencoded, &job.pwarn);
      std::vector<uint8_t>().swap(job.src_bytes);
    };

    // Returns false only on the hard "unsafe path" error; unreadable-but-safe
    // paths mark the job skipped (reference left pointing at the missing name).
    auto produce_job = [&](FlattenTextureJob &job) -> bool {
      if (!job.needs_process) {
        return true;
      }
      std::string rerr;
      if (assets.count(job.orig)) job.src_bytes = assets.at(job.orig);
      if (job.src_bytes.empty() && !ReadAssetBytes(resolver, job.orig, &job.src_bytes, warn, &rerr)) {
        const std::string missing_ref =
            SafeMissingArchiveReference(job.orig, search_paths);
        if (missing_ref.empty()) {
          if (err) {
            (*err) = "Unsafe texture path could not be packed: " + job.orig +
                     " (" + rerr + ")";
          }
          return false;
        }
        path_to_archive[job.orig] = missing_ref;
        if (warn) {
          (*warn) += "Skipping unreadable texture '" + job.orig + "': " + rerr + "\n";
        }
        job.skipped = true;
      }
      return true;
    };

    if (!RunBoundedTextureJobs(texture_jobs, options.num_threads,
                               options.texture_memory_budget_bytes,
                               produce_job, run_job)) {
      return false;
    }
  }

  // Phase 3: archive naming, stats and remaps, deterministically in job order.
  for (FlattenTextureJob &job : texture_jobs) {
    if (job.skipped) {
      continue;  // unreadable; already warned + safe missing reference left
    }
    if (!job.ok) {
      if (warn) (*warn) += "Failed to process texture " + job.orig + "\n";
      continue;
    }
    if (warn) (*warn) += job.pwarn;

    std::string archive_name = std::move(job.archive_name);
    std::string out_ext = std::move(job.out_ext);
    std::vector<uint8_t> out_bytes = std::move(job.out_bytes);

    if (options.arkit_compatible && !IsAllowedARKitTextureExt(out_ext)) {
      if (err) {
        *err = "ARKit-compatible USDZ cannot package texture '" + job.orig +
               "' with unsupported output extension '" + out_ext + "'.";
      }
      return false;
    }

    if (!IsAllowedTextureExt(out_ext)) {
      // Should not happen, but guard the archive against invalid extensions.
      out_ext = "png";
    }
    if (to_lower(io::GetFileExtension(archive_name)) != out_ext) {
      archive_name = ReplaceExtension(archive_name, out_ext);
    }

    // Avoid archive name collisions for distinct sources.
    std::string unique_name = archive_name;
    uint32_t suffix = 1;
    const uint32_t kMaxSuffix = 100000;
    bool collision_ok = true;
    while (assets.count(unique_name) &&
           assets[unique_name] != out_bytes) {
      if (suffix > kMaxSuffix) {
        if (warn) {
          (*warn) += "Too many archive name collisions for " + archive_name +
                     "; skipping texture to avoid overwrite.\n";
        }
        // Exhausted suffixes: do NOT proceed with a name that already exists
        // with different bytes (that would overwrite the existing entry in
        // the ZIP archive). Skip this texture instead.
        collision_ok = false;
        break;
      }
      unique_name = MakeCollisionArchiveName(archive_name, suffix);
      suffix++;
    }
    if (!collision_ok) {
      continue;  // skip this texture; do NOT add to assets
    }
    archive_name = unique_name;

    assets[archive_name] = std::move(out_bytes);
    path_to_archive[job.orig] = archive_name;

    if (stats) {
      stats->num_textures++;
      if (job.resized) stats->num_textures_resized++;
      if (job.reencoded) {
        stats->num_textures_reencoded++;
      } else {
        stats->num_textures_passthrough++;
      }
    }
    Log(options.verbose, "  " + job.orig + " -> " + archive_name +
                             (job.resized ? " [resized]" : "") +
                             (job.reencoded ? " [reencoded]" : " [passthrough]"));
  }

  // --- Rewrite texture file paths to the archive-relative names ---
  timer.begin("rewrite-texture-paths");
  if (has_layer_for_write) {
    RemapLayerAssetPaths(layer_for_write, path_to_archive);
    if (options.arkit_compatible) {
      std::string rwarn, rerr;
      if (!lightusd::LayerToStage(Layer(layer_for_write), &stage, &rwarn,
                                  &rerr)) {
        if (err) *err = "LayerToStage after ARKit remap failed: " + rerr;
        return false;
      }
      if (warn) *warn += rwarn;
    }
  } else {
    for (auto &kv : textures) {
      UsdUVTexture *tex = kv.first;
      const std::string &orig = kv.second;
      auto it = path_to_archive.find(orig);
      if (it == path_to_archive.end()) continue;
      if (it->second != orig) {
        WriteTextureFilePath(tex, it->second);
      }
    }
  }

  } else {
    Log(options.verbose,
        "Rebasing external texture paths for flat output.");
    std::string out_dir = io::GetBaseDir(options.output);
    if (out_dir.empty()) {
      out_dir = ".";
    }
    std::map<std::string, std::string> remap;
    for (const std::string &orig : texture_paths) {
      if (remap.count(orig)) continue;
      if (orig.empty()) continue;
      std::string source = orig;
      std::string pre, post, marker;
      if (udim::SplitPattern(orig, &pre, &post, &marker)) {
        std::string resolved_pattern;
        for (uint32_t id = 1001; id <= 9999; ++id) {
          const std::string tile = pre + std::to_string(id) + post;
          std::string resolved = io::IsAbsPath(tile) ? tile : resolver.resolve(tile);
          if (resolved.empty() || !io::FileExists(resolved)) continue;
          resolved = io::AbsPath(resolved);
          if (resolved.size() < post.size() + 4 || resolved.compare(resolved.size() - post.size(), post.size(), post) != 0) continue;
          const std::string candidate = resolved.substr(0, resolved.size() - post.size() - 4) + marker + post;
          if (!resolved_pattern.empty() && resolved_pattern != candidate) {
            if (err) *err = "UDIM tiles resolve to different filesystem patterns: " + orig;
            return false;
          }
          resolved_pattern = candidate;
        }
        if (resolved_pattern.empty()) continue;
        const std::string rel = io::RelativePath(resolved_pattern, io::AbsPath(out_dir));
        remap[orig] = rel.empty() ? resolved_pattern : rel;
        continue;
      }
      if (!io::IsAbsPath(source)) {
        source = resolver.resolve(orig);
      }
      // Preserve unresolved and archive-only references. A filesystem asset
      // must remain reachable when the flat layer moves to another directory.
      if (source.empty() || !io::FileExists(source)) continue;
      source = io::AbsPath(source);
      std::string rel = io::RelativePath(source, io::AbsPath(out_dir));
      if (rel.empty()) rel = source;
      if (rel != orig) {
        remap[orig] = rel;
      }
    }
    if (!remap.empty()) {
      size_t n = has_layer_for_write
                     ? RemapLayerAssetPaths(layer_for_write, remap)
                     : RemapTextureAssetPaths(stage, remap);
      Log(options.verbose,
          "Relativized " + std::to_string(n) + " texture path(s).");
    }
  }  // if USDZ

  // --- Write output ---
  timer.begin("write-output");
  struct UDIMOutputGuard {
    std::string temporary;
    std::vector<std::string> created;
    bool committed{false};
    ~UDIMOutputGuard() {
      if (!temporary.empty()) io::RemoveFile(temporary);
      if (!committed) for (const auto& path : created) io::RemoveFile(path);
    }
  } udim_output_guard;
  std::string write_output = options.output;
  if (!udim_sidecars.empty()) {
    static std::atomic<uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    write_output += ".udim-tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1));
    udim_output_guard.temporary = write_output;
  }
  bool write_ok = false;
  std::string swarn, serr;
  switch (options.output_format) {
    case OutputFormat::USDZ: {
      Log(options.verbose, "Writing USDZ: " + options.output);
      lightusd::USDZWriteOptions write_options;
      write_options.root_layer_format =
          (options.arkit_compatible)
              ? lightusd::USDZRootLayerFormat::USDC
          : (options.usdz_root_layer_format == USDZRootLayerFormat::USDA)
              ? lightusd::USDZRootLayerFormat::USDA
              : lightusd::USDZRootLayerFormat::USDC;
      if (options.arkit_compatible &&
          options.usdz_root_layer_format != USDZRootLayerFormat::USDC &&
          warn) {
        *warn += "ARKit-compatible USDZ requires a USDC root layer; ignoring "
                 "--rootLayerFormat.\n";
      }
      write_ok = has_layer_for_write
          ? (options.arkit_compatible
                 ? lightusd::SaveAsUSDZToFile(options.output, stage, assets,
                                              write_options, &swarn, &serr)
                 : lightusd::SaveAsUSDZToFile(options.output, layer_for_write,
                                              assets, write_options, &swarn,
                                              &serr))
          : lightusd::SaveAsUSDZToFile(options.output, stage, assets,
                                       write_options, &swarn, &serr);
      if (!write_ok && err) (*err) = "Failed to write USDZ: " + serr;
      break;
    }
    case OutputFormat::USDC: {
      Log(options.verbose, "Writing USDC: " + options.output);
      write_ok = has_layer_for_write
          ? lightusd::usdc::SaveAsUSDCToFile(write_output, layer_for_write,
                                             &swarn, &serr)
          : lightusd::usdc::SaveAsUSDCToFile(write_output, stage, &swarn, &serr);
      if (!write_ok && err) (*err) = "Failed to write USDC: " + serr;
      break;
    }
    case OutputFormat::USDA: {
      Log(options.verbose, "Writing USDA: " + options.output);
      if (has_layer_for_write) {
        std::ofstream ofs(write_output);
        if (ofs) {
          ofs << lightusd::print_layer(layer_for_write, 0);
          write_ok = bool(ofs);
        } else {
          serr = "Failed to open output file.";
          write_ok = false;
        }
      } else {
        write_ok = lightusd::usda::SaveAsUSDA(write_output, stage, &swarn, &serr);
      }
      if (!write_ok && err) (*err) = "Failed to write USDA: " + serr;
      break;
    }
  }
  if (!write_ok) {
    return false;
  }
  if (!udim_sidecars.empty()) {
    if (!io::CreateDirectories(udim_sidecar_directory)) {
      if (err) *err = "Cannot create UDIM sidecar directory.";
      return false;
    }
    for (const auto& sidecar : udim_sidecars) {
      const auto& bytes = assets.at(sidecar.first);
      if (io::FileExists(sidecar.second)) {
        std::vector<uint8_t> existing;
        if (!io::ReadWholeFile(&existing, err, sidecar.second, bytes.size() + 1, nullptr) || existing != bytes) {
          if (err) *err = "UDIM sidecar name collides with different existing data.";
          return false;
        }
      } else {
        udim_output_guard.created.push_back(sidecar.second);
        if (!io::WriteWholeFile(sidecar.second, bytes.data(), bytes.size(), err)) return false;
      }
    }
    if (std::rename(write_output.c_str(), options.output.c_str()) != 0) {
      if (err) *err = "Cannot publish staged UDIM root layer.";
      return false;
    }
    udim_output_guard.committed = true;
  }
  if (warn) (*warn) += swarn;

  // When writing USDZ, read back and validate.
  if (options.output_format == OutputFormat::USDZ) {
    timer.begin("validate-usdz");
    std::vector<uint8_t> usdz_bytes;
    std::string ioerr;
    if (io::ReadWholeFile(&usdz_bytes, &ioerr, options.output,
                          /*max*/ 512 * 1024 * 1024)) {
      std::string vwarn, verr;
      if (!lightusd::ValidateUSDZ(usdz_bytes.data(), usdz_bytes.size(), &vwarn,
                                  &verr)) {
        if (warn) {
          (*warn) += "USDZ validation reported issues: " + verr + "\n";
        }
      } else {
        Log(options.verbose, "USDZ validation: OK");
      }
      if (stats) stats->output_size = usdz_bytes.size();
    }
  } else {
    if (stats) {
      stats->output_size = FileSizeOrZero(options.output);
    }
  }

  timer.end();
  return true;
}

bool RepackTextureFiles(const RepackSpec &spec, const std::string &outputFile,
                        image::PngEncoder png_encoder, std::string *warn,
                        std::string *err) {
  if (spec.out_channels < 1 || spec.out_channels > 4) {
    if (err) (*err) = "RepackTextureFiles: out_channels must be 1-4.";
    return false;
  }

  // Gather unique input files and load them once.
  const RepackChannel *chans[4] = {&spec.r, &spec.g, &spec.b, &spec.a};
  std::vector<Image> images;
  std::map<std::string, int> file_to_index;

  for (int c = 0; c < spec.out_channels; c++) {
    const RepackChannel &rc = *chans[c];
    if (rc.input_file.empty()) continue;
    if (file_to_index.count(rc.input_file)) continue;
    auto loaded = image::LoadImageFromFile(rc.input_file);
    if (!loaded) {
      if (err) (*err) = "RepackTextureFiles: failed to load '" + rc.input_file +
                        "': " + loaded.error();
      return false;
    }
    file_to_index[rc.input_file] = int(images.size());
    images.push_back(std::move(loaded.value().image));
  }

  // Build the channel-pack spec referencing loaded images by index.
  tydra::ChannelPackSpec pspec;
  pspec.out_channels = spec.out_channels;
  pspec.out_width = spec.out_width;
  pspec.out_height = spec.out_height;
  tydra::ChannelSource *dst[4] = {&pspec.r, &pspec.g, &pspec.b, &pspec.a};
  for (int c = 0; c < spec.out_channels; c++) {
    const RepackChannel &rc = *chans[c];
    if (rc.input_file.empty()) {
      dst[c]->input_index = -1;
      dst[c]->constant = rc.constant;
    } else {
      dst[c]->input_index = file_to_index[rc.input_file];
      dst[c]->channel = rc.channel;
    }
  }

  Image packed;
  std::string perr;
  if (!tydra::PackChannels(images, pspec, &packed, &perr)) {
    if (err) (*err) = "RepackTextureFiles: " + perr;
    return false;
  }

  image::WriteOption wopt;
  wopt.png_encoder = png_encoder;
  auto ret = image::WriteImageToFile(outputFile, packed, wopt);
  if (!ret) {
    if (err) (*err) = "RepackTextureFiles: failed to write '" + outputFile +
                      "': " + ret.error();
    return false;
  }
  // No warnings to emit; PackChannels returns errors only.
  (void)warn;
  return true;
}

size_t RemapLayerTextureAssetPaths(
    Layer &layer, const std::map<std::string, std::string> &remap) {
  return RemapLayerAssetPaths(layer, remap);
}

}  // namespace usdz
}  // namespace lightusd
