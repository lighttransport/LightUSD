// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
#include "usdz/usdz-converter.hh"
#include "material-optimize.hh"
#include "material-atlas.hh"
#include "image-convert.hh"
#include "geometry-optimize.hh"
#include "udim-convert.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "image-loader.hh"
#include "image-types.hh"
#include "io-util.hh"
#include "layer/layer.hh"
#include "layer/prim-spec.hh"
#include "load-usd.hh"
#include "resolver/asset-resolver.hh"
#include "resolver/dependency-report.hh"
#include "stage/stage.hh"
#include "str-util.hh"
#include "tydra/resize-image.hh"
#include "tydra/texture-util.hh"
#include "types/value.hh"
#include "writer/usda-writer.hh"
#include "writer/usdc-writer.hh"
#include "writer/usdz-writer.hh"

namespace lightusd {
namespace usdz {

namespace {

constexpr size_t kMaxArchiveNameLen = 1024;
constexpr int kMaxDimension = 32768;

void Log(bool verbose, const std::string& msg) {
  if (verbose) std::fprintf(stderr, "[lusdzconvert] %s\n", msg.c_str());
}

std::string Slashes(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  return p;
}

std::string LowerExt(const std::string& path) {
  return to_lower(io::GetFileExtension(path));
}

bool IsTextureExt(const std::string& ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "exr" ||
         ext == "ktx2";
}

bool IsARKitTextureExt(const std::string& ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "exr";
}

bool IsAbsolutePath(const std::string& p) {
  return !p.empty() &&
         (p[0] == '/' || p[0] == '\\' || (p.size() >= 2 && p[1] == ':'));
}

std::string AbsolutePath(const std::string& p) {
  if (IsAbsolutePath(p)) return io::NormalizePath(Slashes(p));
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (ec) return io::NormalizePath(Slashes(p));
  return io::NormalizePath(Slashes(abs.lexically_normal().string()));
}

// Path of `target` relative to directory `base` (both absolute).
std::string RelativePath(const std::string& base, const std::string& target) {
  auto split = [](const std::string& p) {
    std::vector<std::string> parts;
    size_t s = 0;
    while (s < p.size()) {
      size_t e = p.find('/', s);
      if (e == std::string::npos) e = p.size();
      if (e > s) parts.push_back(p.substr(s, e - s));
      s = e + 1;
    }
    return parts;
  };
  const auto b = split(Slashes(base));
  const auto t = split(Slashes(target));
  size_t common = 0;
  while (common < b.size() && common < t.size() && b[common] == t[common])
    ++common;
  std::string rel;
  for (size_t i = common; i < b.size(); ++i) rel += "../";
  for (size_t i = common; i < t.size(); ++i) {
    if (i > common) rel += "/";
    rel += t[i];
  }
  return rel.empty() ? std::string(".") : rel;
}

bool IsSafeArchiveName(const std::string& name) {
  if (name.empty() || name.size() > kMaxArchiveNameLen) return false;
  if (name.find('\0') != std::string::npos ||
      name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos ||
      name.find("//") != std::string::npos || name[0] == '/' ||
      name.back() == '/')
    return false;
  size_t s = 0;
  while (s <= name.size()) {
    size_t e = name.find('/', s);
    if (e == std::string::npos) e = name.size();
    const std::string seg = name.substr(s, e - s);
    if (seg.empty() || seg == "." || seg == "..") return false;
    s = e + 1;
  }
  return true;
}

// Archive entry name for an asset. Paths inside the root directory keep their
// relative layout; anything else lands under "textures/<basename>".
std::string ArchiveNameFor(const std::string& root_dir,
                           const std::string& resolved) {
  std::string rel = RelativePath(root_dir, resolved);
  if (IsSafeArchiveName(rel)) return rel;
  std::string base = io::GetBaseFilename(Slashes(resolved));
  if (!IsSafeArchiveName(base)) base = "texture";
  return "textures/" + base;
}

std::string UniqueName(const std::string& name, std::set<std::string>* taken) {
  if (taken->insert(name).second) return name;
  const std::string ext = io::GetFileExtension(name);
  const std::string stem =
      ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
  for (int i = 1;; ++i) {
    std::string cand = stem + "_" + std::to_string(i) + (ext.empty() ? "" : "." + ext);
    if (taken->insert(cand).second) return cand;
  }
}

std::string ReplaceExtension(const std::string& name, const std::string& ext) {
  const std::string old = io::GetFileExtension(name);
  if (old.empty()) return name + "." + ext;
  return name.substr(0, name.size() - old.size()) + ext;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::string e;
  return io::ReadWholeFile(out, &e, path, /*filesize_max*/ 0, nullptr);
}

bool WriteFile(const std::string& path, const uint8_t* data, size_t size,
               std::string* err) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open output file: " + path;
    return false;
  }
  f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
  if (!f) {
    *err = "write failed: " + path;
    return false;
  }
  return true;
}

// Authored texture asset-path values on every prim of `stage` (defaults and
// time samples are both visited through the property value view).
std::set<std::string> CollectTexturePaths(const Stage& stage) {
  std::set<std::string> out;
  auto add = [&](StringView p) {
    if (p.empty() || p.find("://") != StringView::npos) return;
    std::string s(p.data(), p.size());
    if (IsTextureExt(LowerExt(s))) out.insert(std::move(s));
  };
  stage.Traverse([&](const UsdPrim& prim) {
    for (const std::string& name : prim.GetPropertyNames()) {
      const Value* v = prim.GetPropertyValue(name);
      if (!v || v->type_id() != TypeId::AssetPath) continue;
      if (v->is_array()) {
        if (const auto a = v->as_token_array())
          for (const auto p : *a) add(p);
      } else if (const auto p = v->as_asset_path()) {
        add(*p);
      }
    }
    return true;
  });
  return out;
}

void RemapLayer(Layer* layer, const std::map<std::string, std::string>& remap) {
  if (!layer || remap.empty()) return;
  for (size_t i = 0; i < layer->prim_count(); ++i)
    if (PrimSpec* p = layer->prim_mutable(static_cast<uint32_t>(i)))
      p->remap_asset_paths(remap);
}

struct Texture {
  std::string authored;
  std::string resolved;  // absolute file path
  std::string archive;   // archive entry name (USDZ)
  std::vector<uint8_t> bytes;
  std::string ext;
  bool generated{false};
};

void PassThrough(Texture* t, UsdzConvertStats* stats) {
  (void)t;
  stats->num_textures_passthrough++;
}

// Resize / transcode / re-encode one texture in place.
void ProcessTexture(const UsdzConvertOptions& opts, Texture* t,
                    UsdzConvertStats* stats, std::string* warn) {
  std::string target = t->ext == "ktx2" ? "png" : t->ext;
  if (opts.texture_format == OutputTextureFormat::PNG) target = "png";
  if (opts.texture_format == OutputTextureFormat::JPEG) target = "jpg";
  if (opts.arkit_compatible && !IsARKitTextureExt(target)) target = "png";
  const bool transcode =
      target != t->ext && !(target == "jpg" && t->ext == "jpeg");
  const bool resize = opts.max_texture_size > 0;
  const bool want = resize || transcode ||
                    (opts.reencode &&
                     (target == "png" || target == "jpg" || target == "jpeg"));
  if (!want) return PassThrough(t, stats);

  image::ConvertOptions convert;
  convert.max_size=opts.max_texture_size;
  convert.format=target=="jpg" || target=="jpeg"?"jpeg":target;
  convert.png_encoder=opts.png_encoder==image::PngEncoder::Fpng?"fpng":opts.png_encoder==image::PngEncoder::Fpnge?"fpnge":"auto";
  convert.jpeg_quality=opts.jpeg_quality;
  auto output=image::ConvertImage(t->bytes.data(),t->bytes.size(),convert,t->authored);
  if(!output) {
    *warn+="Texture conversion failed for "+t->authored+": "+output.error()+" - copying through unchanged.\n";
    return PassThrough(t,stats);
  }
  if(output->resized)stats->num_textures_resized++;
  stats->num_textures_reencoded++;
  t->bytes=std::move(output->data);t->ext=target;
}

// Fit the reencodable textures into `target_texture_bytes`.
bool FitTextures(const UsdzConvertOptions& opts, std::vector<Texture>* texs,
                 UsdzConvertStats* stats, std::string* warn, std::string* err) {
  std::vector<tydra::FitTextureInput> inputs;
  inputs.reserve(texs->size());
  for (Texture& t : *texs) {
    tydra::FitTextureInput in;
    in.ext = t.ext;
    in.original_bytes = t.bytes;
    in.reencodable = false;
    if (!t.generated && (t.ext == "png" || t.ext == "jpg" || t.ext == "jpeg")) {
      auto d = image::LoadImageFromMemory(t.bytes.data(), t.bytes.size(),
                                          t.authored);
      if (d && d.value().image.bpp == 8) {
        in.image = std::move(d.value().image);
        in.reencodable = true;
      }
    }
    inputs.push_back(std::move(in));
  }
  tydra::FitTextureOptions fo;
  fo.target_total_bytes = opts.target_texture_bytes;
  fo.strategy = opts.fit_strategy == FitStrategy::Quality
                    ? tydra::FitStrategy::Quality
                    : tydra::FitStrategy::Size;
  fo.start_max_size = opts.max_texture_size;
  fo.min_texture_size = opts.fit_min_texture_size;
  fo.min_jpeg_quality = opts.fit_min_jpeg_quality;
  fo.jpeg_quality = opts.jpeg_quality;
  fo.png_encoder = opts.png_encoder;
  std::vector<tydra::FitTextureOutput> outs;
  std::string fwarn, ferr;
  if (!tydra::FitTexturesToBudget(inputs, fo, &outs, &fwarn, &ferr)) {
    *err = "texture budget fit failed: " + ferr;
    return false;
  }
  *warn += fwarn;
  for (size_t i = 0; i < texs->size() && i < outs.size(); ++i) {
    Texture& t = (*texs)[i];
    if (!outs[i].changed) {
      if (!t.generated) stats->num_textures_passthrough++;
      continue;
    }
    t.bytes = std::move(outs[i].bytes);
    t.ext = outs[i].ext;
    stats->num_textures_reencoded++;
  }
  return true;
}

void ApplyMetadata(const UsdzConvertOptions& o, Stage* stage) {
  StageMeta& sm = stage->GetMeta();
  Layer* root = stage->GetRootLayer();
  const char* axis = nullptr;
  if (o.upAxis == UpAxis::X) axis = "X";
  if (o.upAxis == UpAxis::Y) axis = "Y";
  if (o.upAxis == UpAxis::Z) axis = "Z";
  if (!axis && o.arkit_compatible) axis = "Y";
  if (axis) {
    sm.upAxis = axis;
    sm.upAxis_set = true;
    if (root) {
      root->meta().upAxis = axis;
      root->meta().upAxis_set = true;
    }
  }
  if (o.metersPerUnit > 0.0) {
    sm.metersPerUnit = o.metersPerUnit;
    sm.metersPerUnit_set = true;
    if (root) {
      root->meta().metersPerUnit = o.metersPerUnit;
      root->meta().metersPerUnit_set = true;
    }
  }
  if (!o.copyright.empty() || !o.url.empty()) {
    std::string doc = o.copyright;
    if (!o.url.empty()) doc += (doc.empty() ? "" : "\n") + o.url;
    sm.doc = doc;
    sm.doc_set = true;
    if (root) {
      root->meta().doc = doc;
      root->meta().doc_set = true;
    }
  }
}

bool SerializeRoot(const Stage& stage, bool usda, std::vector<uint8_t>* out,
                   std::string* err) {
  if (usda) {
    const std::string text = WriteUSDAToString(stage);
    if (text.empty()) {
      *err = "USDA export failed";
      return false;
    }
    out->assign(text.begin(), text.end());
    return true;
  }
  USDCWriteResult r = WriteUSDCToMemory(*out, stage, USDCWriteOptions());
  if (!r.success) {
    *err = r.error.empty() ? "USDC export failed" : r.error;
    return false;
  }
  return true;
}

}  // namespace

bool Convert(const UsdzConvertOptions& opts, UsdzConvertStats* stats_out,
             std::string* warn_out, std::string* err_out) {
  UsdzConvertStats local_stats;
  UsdzConvertStats& stats = stats_out ? *stats_out : local_stats;
  std::string local_warn, local_err;
  std::string& warn = warn_out ? *warn_out : local_warn;
  std::string& err = err_out ? *err_out : local_err;

  if (opts.material_optimization != MaterialOptimizationMode::Off &&
      opts.material_optimization != MaterialOptimizationMode::Dedupe &&
      opts.material_optimization != MaterialOptimizationMode::Preview &&
      opts.material_optimization != MaterialOptimizationMode::Atlas) {
    err = "the requested material optimization mode has not yet been "
          "migrated to the core converter";
    return false;
  }
  if ((opts.material_optimization != MaterialOptimizationMode::Off ||
       opts.geometry_optimization != GeometryOptimizationMode::Off) && !opts.flatten) {
    err = "material/geometry optimization requires flattening";
    return false;
  }
  if (opts.input.empty() || opts.output.empty()) {
    err = "input and output paths are required";
    return false;
  }
  const bool to_usdz = opts.output_format == OutputFormat::USDZ;
  const std::string input_abs = AbsolutePath(opts.input);
  const std::string root_dir = io::GetBaseDir(input_abs);

  // --- Dependencies (resolved paths for every authored asset/layer). ---
  ResolverConfig config;
  config.enable_suffix_fallback = false;
  AssetResolver resolver(config);
  DependencyOptions dep_opts;
  const DependencyReport deps =
      CollectDependencies(input_abs, resolver, dep_opts);
  std::map<std::string, std::string> resolved_by_authored;
  for (const DependencyRecord& r : deps.records) {
    if (r.status != "resolved" || r.resolved_identifier.empty()) continue;
    resolved_by_authored.emplace(r.authored_path, r.resolved_identifier);
  }

  // --- Load (composed + flattened by default). ---
  Stage stage;
  std::string lwarn, lerr;
  bool ok = opts.flatten ? LoadUSDComposed(input_abs, &stage, &lwarn, &lerr)
                         : LoadUSD(input_abs, &stage, &lwarn, &lerr);
  if (!lwarn.empty()) warn += lwarn;
  if (!ok) {
    err = lerr.empty() ? "failed to load " + opts.input : lerr;
    return false;
  }
  if (!lerr.empty()) warn += lerr;  // composition errors are non-fatal
  if (opts.flatten) {
    Layer flat = stage.Flatten();
    Stage flat_stage;
    flat_stage.SetRootLayer(std::move(flat));
    flat_stage.GetMeta() = stage.GetMeta();
    stage = std::move(flat_stage);
  }
  Log(opts.verbose, "loaded " + opts.input);
  ApplyMetadata(opts, &stage);


  // --- UDIM layer edits and generated atlas assets. ---
  std::map<std::string, std::vector<uint8_t>> atlases;
  if (opts.udim_bake != UDIMBakeMode::Off &&
      !BakeUDIMs(*stage.GetRootLayer(), opts, root_dir, &atlases, &err)) return false;
  // Match the web converter: bake UV/texture edits before material optimization.
  if (opts.material_optimization == MaterialOptimizationMode::Dedupe ||
      opts.material_optimization == MaterialOptimizationMode::Preview ||
      opts.material_optimization == MaterialOptimizationMode::Atlas)
    DedupeMaterials(stage.GetRootLayer(), opts.material_optimization != MaterialOptimizationMode::Dedupe);
  if (opts.material_optimization == MaterialOptimizationMode::Atlas) {
    MaterialAtlasOptions atlas;
    atlas.edge = opts.material_atlas_size;
    atlas.tile = opts.max_texture_size > 0
        ? std::min(opts.material_atlas_tile_size, opts.max_texture_size)
        : opts.material_atlas_tile_size;
    atlas.padding = opts.material_atlas_padding;
    atlas.minimum = opts.material_atlas_min_group_size;
    if (opts.texture_memory_budget_bytes) atlas.memory_budget = opts.texture_memory_budget_bytes;
    const udim::Fetch fetch = [&](const std::string& path, std::vector<uint8_t>* bytes,
                                  std::string* message, size_t limit) {
      if (!limit) return false;
      const std::string resolved = IsAbsolutePath(path) ? path : io::JoinPath(root_dir, path);
      return ReadNativeAtlasImage(resolved, bytes, message, limit);
    };
    if (!BakeMaterialAtlases(*stage.GetRootLayer(), atlas, fetch, &atlases, &warn, &err))
      return false;
    if (!to_usdz) {
      const std::string out_dir = io::GetBaseDir(AbsolutePath(opts.output));
      for (const auto& entry : atlases) {
        const std::string path = io::JoinPath(out_dir, entry.first);
        if (!io::FileExists(path)) continue;
        std::vector<uint8_t> existing;
        if (!ReadNativeAtlasImage(path, &existing, &err, entry.second.size()) ||
            existing != entry.second) {
          err = "generated atlas would overwrite a different file: " + path;
          return false;
        }
      }
    }
  }
  if (opts.geometry_optimization == GeometryOptimizationMode::MergeMeshes) {
    MeshMergeOptions merge;
    merge.max_input_faces = static_cast<size_t>(opts.mesh_merge_max_input_faces);
    merge.max_input_points = static_cast<size_t>(opts.mesh_merge_max_input_points);
    merge.max_aggregate_faces = static_cast<size_t>(opts.mesh_merge_max_aggregate_faces);
    merge.min_group_size = static_cast<size_t>(opts.mesh_merge_min_group_size);
    if (!MergeMeshes(&stage, merge, nullptr, &warn, &err)) return false;
  }

  // --- Textures. ---
  std::vector<Texture> textures;
  for (const std::string& authored : CollectTexturePaths(stage)) {
    if (atlases.count(authored)) continue;
    if (authored.find("<UDIM>") != std::string::npos ||
        authored.find("<udim>") != std::string::npos) {
      warn += "UDIM texture left as an external reference: " + authored + "\n";
      continue;
    }
    std::string resolved;
    const std::string stripped =
        authored.compare(0, 2, "./") == 0 ? authored.substr(2) : authored;
    if (IsAbsolutePath(stripped) && io::FileExists(stripped)) {
      resolved = stripped;
    } else if (io::FileExists(io::JoinPath(root_dir, stripped))) {
      resolved = io::JoinPath(root_dir, stripped);
    } else {
      auto it = resolved_by_authored.find(authored);
      if (it != resolved_by_authored.end() && io::FileExists(it->second))
        resolved = it->second;
    }
    if (resolved.empty()) {
      warn += "texture not found: " + authored + "\n";
      continue;
    }
    Texture t;
    t.authored = authored;
    t.resolved = AbsolutePath(resolved);
    t.ext = LowerExt(t.resolved);
    textures.push_back(std::move(t));
  }

  if (opts.include_unused_textures && to_usdz) {
    std::set<std::string> known;
    for (const Texture& t : textures) known.insert(t.resolved);
    for (const std::string& rel : io::ListDir(root_dir, true)) {
      const std::string full = AbsolutePath(io::JoinPath(root_dir, rel));
      if (!IsTextureExt(LowerExt(rel)) || known.count(full)) continue;
      Texture t;
      t.resolved = full;
      t.ext = LowerExt(full);
      textures.push_back(std::move(t));  // empty `authored`: not remapped
      stats.num_unused_textures_included++;
    }
  }
  const size_t regular_textures = textures.size();
  for (auto& atlas : atlases) {
    Texture texture;
    texture.authored = atlas.first;
    texture.archive = atlas.first;
    texture.ext = LowerExt(atlas.first);
    texture.generated = true;
    texture.bytes = std::move(atlas.second);
    textures.push_back(std::move(texture));
    ++stats.num_textures_reencoded;
  }
  // Reserve generated names first so ordinary texture collisions cannot force
  // a second rename inside inline variant contents.
  std::rotate(textures.begin(), textures.begin() + static_cast<std::ptrdiff_t>(regular_textures), textures.end());
  stats.num_textures = textures.size();

  const bool process_textures = to_usdz && opts.flatten;
  if (!process_textures &&
      (opts.max_texture_size > 0 || opts.target_texture_bytes > 0 ||
       opts.texture_format != OutputTextureFormat::KeepOriginal)) {
    warn += "ordinary texture processing applies to flattened USDZ output only; "
            "UDIM tiles are handled separately by the baking options\n";
  }

  std::map<std::string, std::string> remap;
  if (!to_usdz) {
    // Flat USDC/USDA: textures stay external, rebased to the output directory.
    const std::string out_dir = io::GetBaseDir(AbsolutePath(opts.output));
    for (const Texture& t : textures) {
      if (t.authored.empty()) continue;
      if (t.generated) {
        const std::string path = io::JoinPath(out_dir, t.authored);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
        if (ec) { err = "cannot create atlas output directory: " + ec.message(); return false; }
        if (!WriteFile(path, t.bytes.data(), t.bytes.size(), &err)) return false;
        continue;
      }
      const std::string rel = RelativePath(out_dir, t.resolved);
      if (rel != t.authored) remap[t.authored] = rel;
      stats.num_textures_passthrough++;
    }
    RemapLayer(stage.GetRootLayer(), remap);
    std::vector<uint8_t> bytes;
    if (!SerializeRoot(stage, opts.output_format == OutputFormat::USDA, &bytes,
                       &err))
      return false;
    if (!WriteFile(opts.output, bytes.data(), bytes.size(), &err)) return false;
    stats.output_size = bytes.size();
    return true;
  }

  // Read and (optionally) process texture bytes.
  for (Texture& t : textures) {
    if (t.generated) continue;
    if (!ReadFile(t.resolved, &t.bytes)) {
      warn += "failed to read texture: " + t.resolved + "\n";
      t.bytes.clear();
    }
  }
  textures.erase(std::remove_if(textures.begin(), textures.end(),
                                [](const Texture& t) { return t.bytes.empty(); }),
                 textures.end());
  if (process_textures) {
    if (opts.target_texture_bytes > 0) {
      // The budget fitter accepts ordinary images, so decode KTX2 first.
      for (Texture& t : textures)
        if (t.ext == "ktx2") ProcessTexture(opts, &t, &stats, &warn);
      if (!FitTextures(opts, &textures, &stats, &warn, &err)) return false;
    } else {
      for (Texture& t : textures)
        if (!t.generated) ProcessTexture(opts, &t, &stats, &warn);
    }
    for (const Texture& t : textures) {
      if (t.ext == "ktx2") {
        err = "KTX2 texture could not be transcoded: " + t.authored +
              "; enable LIGHTUSD_WITH_TEXTOOLS and use a supported codec";
        return false;
      }
    }
  } else {
    for (const Texture& texture : textures)
      if (!texture.generated) ++stats.num_textures_passthrough;
  }

  std::map<std::string, std::vector<uint8_t>> assets;
  std::set<std::string> taken;
  std::map<std::string, std::string> by_resolved;  // resolved -> archive
  for (Texture& t : textures) {
    const std::string key = t.generated ? "generated:" + t.authored : t.resolved;
    auto seen = by_resolved.find(key);
    if (seen != by_resolved.end()) {
      if (!t.authored.empty()) remap[t.authored] = seen->second;
      continue;
    }
    std::string name = t.generated ? t.archive : ArchiveNameFor(root_dir, t.resolved);
    if (t.ext != LowerExt(name)) name = ReplaceExtension(name, t.ext);
    name = UniqueName(name, &taken);
    by_resolved.emplace(key, name);
    if (!t.authored.empty() && t.authored != name) remap[t.authored] = name;
    assets.emplace(name, std::move(t.bytes));
  }

  // -noFlatten: package the dependency layers next to the root.
  if (!opts.flatten) {
    for (const DependencyRecord& r : deps.records) {
      if (r.status != "resolved" || r.kind == "texture" || r.kind == "asset")
        continue;
      const std::string abs = AbsolutePath(r.resolved_identifier);
      const std::string rel = RelativePath(root_dir, abs);
      if (!IsSafeArchiveName(rel)) {
        warn += "dependency outside the package root left external: " +
                r.authored_path + "\n";
        continue;
      }
      if (assets.count(rel)) continue;
      std::vector<uint8_t> bytes;
      if (!ReadFile(abs, &bytes)) {
        warn += "failed to read dependency layer: " + abs + "\n";
        continue;
      }
      taken.insert(rel);
      assets.emplace(rel, std::move(bytes));
      stats.num_layers_packaged++;
    }
  }

  RemapLayer(stage.GetRootLayer(), remap);

  // Non-flattened roots keep their arcs; USDA preserves them verbatim.
  const bool usda_root =
      opts.usdz_root_layer_format == USDZRootLayerFormat::USDA;
  std::vector<uint8_t> root_bytes;
  if (!SerializeRoot(stage, usda_root, &root_bytes, &err)) return false;
  std::vector<uint8_t> package;
  USDZWriteOptions zopts;
  USDZWriteResult zr =
      usda_root ? WriteUSDZFromUSDAAndAssetsToMemory(
                      package, root_bytes.data(), root_bytes.size(), assets, zopts)
                : WriteUSDZFromUSDCAndAssetsToMemory(
                      package, root_bytes.data(), root_bytes.size(), assets, zopts);
  if (!zr.success) {
    err = zr.error.empty() ? "USDZ write failed" : zr.error;
    return false;
  }
  if (!WriteFile(opts.output, package.data(), package.size(), &err))
    return false;
  stats.output_size = package.size();

  // Validate: the package must load back.
  Stage check;
  std::string cwarn, cerr;
  if (!LoadUSDFromMemory(package.data(), package.size(), &check, &cwarn,
                         &cerr)) {
    err = "written USDZ does not load back: " + cerr;
    return false;
  }
  Log(opts.verbose, "wrote " + opts.output + " (" +
                        std::to_string(package.size()) + " bytes)");
  return true;
}

bool RepackTextureFiles(const RepackSpec& spec, const std::string& output_file,
                        image::PngEncoder png_encoder, std::string* warn,
                        std::string* err) {
  (void)warn;
  if (spec.out_channels < 1 || spec.out_channels > 4) {
    if (err) *err = "RepackTextureFiles: out_channels must be 1-4.";
    return false;
  }
  const RepackChannel* chans[4] = {&spec.r, &spec.g, &spec.b, &spec.a};
  std::vector<Image> images;
  std::map<std::string, int> index_of;
  for (int c = 0; c < spec.out_channels; ++c) {
    const RepackChannel& rc = *chans[c];
    if (rc.input_file.empty() || index_of.count(rc.input_file)) continue;
    auto loaded = image::LoadImageFromFile(rc.input_file);
    if (!loaded) {
      if (err)
        *err = "RepackTextureFiles: failed to load '" + rc.input_file +
               "': " + loaded.error();
      return false;
    }
    index_of[rc.input_file] = int(images.size());
    images.push_back(std::move(loaded.value().image));
  }
  tydra::ChannelPackSpec ps;
  ps.out_channels = spec.out_channels;
  ps.out_width = spec.out_width;
  ps.out_height = spec.out_height;
  tydra::ChannelSource* dst[4] = {&ps.r, &ps.g, &ps.b, &ps.a};
  for (int c = 0; c < spec.out_channels; ++c) {
    const RepackChannel& rc = *chans[c];
    if (rc.input_file.empty()) {
      dst[c]->input_index = -1;
      dst[c]->constant = rc.constant;
    } else {
      dst[c]->input_index = index_of[rc.input_file];
      dst[c]->channel = rc.channel;
    }
  }
  Image packed;
  std::string perr;
  if (!tydra::PackChannels(images, ps, &packed, &perr)) {
    if (err) *err = "RepackTextureFiles: " + perr;
    return false;
  }
  image::WriteOption wopt;
  wopt.png_encoder = png_encoder;
  auto ret = image::WriteImageToFile(output_file, packed, wopt);
  if (!ret) {
    if (err)
      *err = "RepackTextureFiles: failed to write '" + output_file +
             "': " + ret.error();
    return false;
  }
  return true;
}

}  // namespace usdz
}  // namespace lightusd
