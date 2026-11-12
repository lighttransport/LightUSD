// SPDX-License-Identifier: Apache-2.0
// Sample MaterialX volume graphs using the shared evaluator.
#include "materialx.hh"
#include "core/mtlx-dom.hh"
#include "core/security-policy.hh"
#include "resolver/asset-resolver.hh"
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "texture.h"
}

namespace lightusd {
namespace tydra {
namespace {

struct MtlxVolumeAssetReader {
  AssetResolver default_resolver;
  const AssetResolver* resolver = nullptr;
  bool allow_parent_paths = false;
  std::string anchor;
  std::vector<uint8_t> bytes;
  std::string error;
};

int ReadMtlxVolumeTexture(void* user, const char* path,
                          const unsigned char** data, size_t* size) {
  auto* reader = static_cast<MtlxVolumeAssetReader*>(user);
  std::string normalized;
  if (!path || !security_policy::ValidateAndNormalizeAssetPath(
                   path, &normalized, reader->allow_parent_paths)) {
    reader->error = "Invalid MaterialX texture path";
    return 0;
  }
  const ResolvedAsset asset = reader->resolver->Resolve(normalized, reader->anchor, false);
  if (!asset.exists || !reader->resolver->ReadAsset(asset, &reader->bytes, &reader->error)) {
    if (reader->error.empty()) reader->error = "MaterialX texture not found: " + normalized;
    return 0;
  }
  *data = reader->bytes.data();
  *size = reader->bytes.size();
  return 1;
}

}  // namespace

bool MtlxConverter::EvaluateVolumeMaterial(const std::string& mtlx_content,
                                           const std::string& material_name,
                                           const float uv[2],
                                           MtlxVolumeSample* out,
                                           const std::string& anchor_path) {
  error_.clear();
  if (!out || !uv || !std::isfinite(uv[0]) || !std::isfinite(uv[1])) {
    error_ = "Volume output and finite UV must be provided";
    return false;
  }
  mtlx::MtlxDocument parsed;
  if (!(anchor_path.empty() ? parsed.ParseFromXML(mtlx_content)
                            : parsed.ParseFromXML(mtlx_content, anchor_path))) {
    error_ = "Failed to parse MaterialX: " + parsed.GetError();
    return false;
  }
  const std::string resolved = parsed.GetResolvedXML();
  std::unique_ptr<MtlxDoc, decltype(&mtlx_free)> doc(
      mtlx_load_string(resolved.c_str()), &mtlx_free);
  if (!doc) {
    error_ = "Failed to load MaterialX volume graph";
    return false;
  }
  const int material = material_name.empty()
      ? (doc->nmat == 1 ? 0 : -1)
      : mtlx_find_material(doc.get(), material_name.c_str());
  if (material < 0 || doc->mats[material].volume_node < 0) {
    error_ = "MaterialX volume material or shader not found: " + material_name;
    return false;
  }
  const size_t slash = anchor_path.find_last_of("/\\");
  const std::string directory = slash == std::string::npos ? "." :
      (slash == 0 ? anchor_path.substr(0, 1) : anchor_path.substr(0, slash));
  std::unique_ptr<TextureCache, decltype(&texcache_free)> cache(
      texcache_create(directory.c_str()), &texcache_free);
  if (!cache) {
    error_ = "Failed to create MaterialX texture cache";
    return false;
  }
  MtlxVolumeAssetReader reader;
  reader.anchor = anchor_path;
  reader.resolver = options_.volume_texture_resolver
      ? options_.volume_texture_resolver : &reader.default_resolver;
  reader.allow_parent_paths = options_.volume_texture_resolver &&
      options_.volume_texture_resolver->GetConfig().allow_parent_paths;
  texcache_set_reader(cache.get(), &ReadMtlxVolumeTexture, &reader,
                      options_.volume_texture_max_decoded_bytes);
  std::vector<::MtlxValue> memo(static_cast<size_t>(doc->nnode));
  std::vector<char> done(static_cast<size_t>(doc->nnode), 0);
  ShadeContext context{};
  context.doc = doc.get();
  context.tex = cache.get();
  context.uv[0] = uv[0];
  context.uv[1] = uv[1];
  context.Ns = v3_make(0, 0, 1);
  context.Ng = context.Ns;
  context.V = context.Ns;
  context.memo = memo.data();
  context.memo_done = done.data();
  MtlxVolumeParams volume{};
  if (mtlx_eval_volume(&context, doc->mats[material].volume_node, &volume)) {
    error_ = "Unsupported or invalid MaterialX volume graph";
    return false;
  }
  if (texcache_had_load_failure(cache.get())) {
    error_ = reader.error.empty() ? "Failed to decode MaterialX volume texture" : reader.error;
    return false;
  }
  const float values[] = {volume.absorption.x, volume.absorption.y,
      volume.absorption.z, volume.scattering.x, volume.scattering.y,
      volume.scattering.z, volume.emission.x, volume.emission.y,
      volume.emission.z, volume.anisotropy};
  for (float value : values) {
    if (!std::isfinite(value)) {
      error_ = "MaterialX volume graph produced a non-finite value";
      return false;
    }
  }
  MtlxVolumeSample sample;
  sample.absorption[0] = volume.absorption.x;
  sample.absorption[1] = volume.absorption.y;
  sample.absorption[2] = volume.absorption.z;
  sample.scattering[0] = volume.scattering.x;
  sample.scattering[1] = volume.scattering.y;
  sample.scattering[2] = volume.scattering.z;
  sample.emission[0] = volume.emission.x;
  sample.emission[1] = volume.emission.y;
  sample.emission[2] = volume.emission.z;
  sample.anisotropy = volume.anisotropy;
  *out = sample;
  return true;
}

}  // namespace tydra
}  // namespace lightusd
