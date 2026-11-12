// SPDX-License-Identifier: Apache-2.0
// Sample MaterialX volume graphs using the shared evaluator.
#include "materialx.hh"
#include "core/mtlx-dom.hh"
#include "core/security-policy.hh"
#include "resolver/asset-resolver.hh"
#include <cmath>
#include <cstdint>
#include <cstring>
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

struct VolumeGeomPropReader {
  MtlxGeomPropFn callback = nullptr;
  void* user = nullptr;
  bool invalid = false;
};

struct VolumeFieldReader {
  const RenderVolume* volume = nullptr;
  vol::FieldCache* cache = nullptr;
  const MtlxVolumeContext* context = nullptr;
  std::vector<float>* values = nullptr;
  std::string error;
};

bool ReadVolumeField(void* user, const char* name, MtlxGeomPropType type,
                     float out[16]) {
  auto* reader = static_cast<VolumeFieldReader*>(user);
  if (reader->context->geomprop && reader->context->geomprop(
          reader->context->geomprop_user, name, type, out)) return true;
  const RenderVolumeField* field = reader->volume->Field(name);
  if (!field) return false;
  if (!field->resolution_error.empty()) {
    reader->error = "Cannot resolve MaterialX volume field " + std::string(name) +
                    ": " + field->resolution_error;
    return false;
  }
  const int expected = type == MtlxGeomPropType::Float ? 1 :
      (type == MtlxGeomPropType::Vector3 || type == MtlxGeomPropType::Color3 ? 3 : 0);
  if (!expected) {
    reader->error = "Unsupported type for MaterialX volume field: " + std::string(name);
    return false;
  }
  const double p[3] = {reader->context->position[0], reader->context->position[1],
                       reader->context->position[2]};
  int components = 0;
  std::string error;
  if (!reader->cache->SampleWorld(field->field, p, 1, true, reader->values,
                                   &components, &error) ||
      components != expected || reader->values->size() < static_cast<size_t>(expected)) {
    reader->error = "Cannot sample MaterialX volume field " + std::string(name);
    if (!error.empty()) reader->error += ": " + error;
    return false;
  }
  for (int i = 0; i < expected; ++i) out[i] = (*reader->values)[static_cast<size_t>(i)];
  return true;
}

int ReadVolumeGeomProp(void* user, const char* name, MtlxType type,
                       ::MtlxValue* out) {
  auto* reader = static_cast<VolumeGeomPropReader*>(user);
  MtlxGeomPropType public_type;
  size_t count = 0;
  switch (type) {
    case MV_FLOAT: public_type = MtlxGeomPropType::Float; count = 1; break;
    case MV_COLOR3: public_type = MtlxGeomPropType::Color3; count = 3; break;
    case MV_COLOR4: public_type = MtlxGeomPropType::Color4; count = 4; break;
    case MV_VEC2: public_type = MtlxGeomPropType::Vector2; count = 2; break;
    case MV_VEC3: public_type = MtlxGeomPropType::Vector3; count = 3; break;
    case MV_VEC4: public_type = MtlxGeomPropType::Vector4; count = 4; break;
    case MV_MATRIX33: public_type = MtlxGeomPropType::Matrix33; count = 9; break;
    case MV_MATRIX44: public_type = MtlxGeomPropType::Matrix44; count = 16; break;
    case MV_INT: public_type = MtlxGeomPropType::Integer; count = 1; break;
    case MV_BOOL: public_type = MtlxGeomPropType::Boolean; count = 1; break;
    default: return 0;
  }
  float values[16] = {};
  if (!reader->callback(reader->user, name, public_type, values)) return 0;
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i])) { reader->invalid = true; return 0; }
  }
  *out = ::MtlxValue{};
  out->type = type;
  for (size_t i = 0; i < count; ++i) out->v[i] = values[i];
  return 1;
}

}  // namespace

bool MtlxConverter::EvaluateVolumeMaterial(const std::string& mtlx_content,
                                           const std::string& material_name,
                                           const float uv[2],
                                           MtlxVolumeSample* out,
                                           const std::string& anchor_path) {
  if (!uv) {
    error_ = "Volume UV must be provided";
    return false;
  }
  MtlxVolumeContext context;
  context.uv[0] = uv[0];
  context.uv[1] = uv[1];
  return EvaluateVolumeMaterial(mtlx_content, material_name, context, out,
                                anchor_path);
}

struct MtlxVolumeEvaluator::Impl {
  std::unique_ptr<MtlxDoc, decltype(&mtlx_free)> doc{nullptr, &mtlx_free};
  std::unique_ptr<TextureCache, decltype(&texcache_free)> cache{nullptr, &texcache_free};
  MtlxVolumeAssetReader reader;
  std::vector<::MtlxValue> memo;
  std::vector<char> done;
  std::vector<float> field_values;
  int node = -1;
  std::string error;
};

MtlxVolumeEvaluator::MtlxVolumeEvaluator(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
MtlxVolumeEvaluator::~MtlxVolumeEvaluator() = default;
const std::string& MtlxVolumeEvaluator::GetError() const { return impl_->error; }

std::unique_ptr<MtlxVolumeEvaluator> MtlxConverter::CreateVolumeEvaluator(
    const RenderVolume& volume) {
  if (!volume.materialx_binding || !volume.materialx_binding->resolved_xml) {
    error_ = "Render volume has no MaterialX volume binding";
    return nullptr;
  }
  const auto& binding = *volume.materialx_binding;
  return CreateVolumeEvaluator(*binding.resolved_xml, binding.material_name,
                               binding.anchor_path);
}

std::unique_ptr<MtlxVolumeEvaluator> MtlxConverter::CreateVolumeEvaluator(
    const std::string& mtlx_content, const std::string& material_name,
    const std::string& anchor_path) {
  error_.clear();
  mtlx::MtlxDocument parsed;
  if (!(anchor_path.empty() ? parsed.ParseFromXML(mtlx_content)
                            : parsed.ParseFromXML(mtlx_content, anchor_path))) {
    error_ = "Failed to parse MaterialX: " + parsed.GetError();
    return nullptr;
  }
  const std::string resolved = parsed.GetResolvedXML();
  auto impl = std::unique_ptr<MtlxVolumeEvaluator::Impl>(new MtlxVolumeEvaluator::Impl);
  impl->doc.reset(mtlx_load_string(resolved.c_str()));
  auto& doc = impl->doc;
  if (!doc) {
    error_ = "Failed to load MaterialX volume graph";
    return nullptr;
  }
  const int material = material_name.empty()
      ? (doc->nmat == 1 ? 0 : -1)
      : mtlx_find_material(doc.get(), material_name.c_str());
  if (material < 0 || doc->mats[material].volume_node < 0 ||
      doc->mats[material].volume_node >= doc->nnode ||
      std::strcmp(doc->nodes[doc->mats[material].volume_node].category, "volume") != 0) {
    error_ = "MaterialX volume material or shader not found: " + material_name;
    return nullptr;
  }
  const size_t slash = anchor_path.find_last_of("/\\");
  const std::string directory = slash == std::string::npos ? "." :
      (slash == 0 ? anchor_path.substr(0, 1) : anchor_path.substr(0, slash));
  impl->cache.reset(texcache_create(directory.c_str()));
  auto& cache = impl->cache;
  if (!cache) {
    error_ = "Failed to create MaterialX texture cache";
    return nullptr;
  }
  auto& reader = impl->reader;
  reader.anchor = anchor_path;
  reader.resolver = options_.volume_texture_resolver
      ? options_.volume_texture_resolver : &reader.default_resolver;
  reader.allow_parent_paths = options_.volume_texture_resolver &&
      options_.volume_texture_resolver->GetConfig().allow_parent_paths;
  texcache_set_reader(cache.get(), &ReadMtlxVolumeTexture, &reader,
                      options_.volume_texture_max_decoded_bytes);
  texcache_set_auto_mips(cache.get(), options_.volume_texture_mipmaps);
  impl->node = doc->mats[material].volume_node;
  impl->memo.resize(static_cast<size_t>(doc->nnode));
  impl->done.resize(static_cast<size_t>(doc->nnode));
  return std::unique_ptr<MtlxVolumeEvaluator>(new MtlxVolumeEvaluator(std::move(impl)));
}


bool MtlxConverter::EvaluateVolumeMaterial(const std::string& mtlx_content,
                                           const std::string& material_name,
                                           const MtlxVolumeContext& input,
                                           MtlxVolumeSample* out,
                                           const std::string& anchor_path) {
  auto evaluator = CreateVolumeEvaluator(mtlx_content, material_name, anchor_path);
  if (!evaluator) return false;
  if (!evaluator->Evaluate(input, out)) {
    error_ = evaluator->GetError();
    return false;
  }
  return true;
}

bool MtlxVolumeEvaluator::Evaluate(const MtlxVolumeContext& input,
                                   MtlxVolumeSample* out) {
  impl_->error.clear();
  impl_->reader.error.clear();
  texcache_clear_load_failure(impl_->cache.get());
  const auto finite = [](const float* values, size_t count) {
    for (size_t i = 0; i < count; ++i)
      if (!std::isfinite(values[i])) return false;
    return true;
  };
  if (!out || !finite(input.uv, 2) || !finite(input.position, 3) ||
      !finite(input.shading_normal, 3) || !finite(input.geometric_normal, 3) ||
      !finite(input.tangent, 3) || !finite(input.bitangent, 3) ||
      !finite(input.view, 3) || !std::isfinite(input.time) ||
      !std::isfinite(input.frame) ||
      (input.has_uv_derivatives && (!finite(input.uv_dx, 2) || !finite(input.uv_dy, 2))) ||
      (input.has_position_derivatives && (!finite(input.position_dx, 3) || !finite(input.position_dy, 3))) ||
      (input.has_view_derivatives && (!finite(input.view_dx, 3) || !finite(input.view_dy, 3))) ||
      (input.has_space_transforms &&
       (!finite(input.object_to_world, 16) || !finite(input.world_to_object, 16) ||
        !finite(input.world_to_view, 16) || !finite(input.view_to_world, 16)))) {
    impl_->error = "Volume output and finite sample context must be provided";
    return false;
  }
  ShadeContext context{};
  VolumeGeomPropReader geomprop{input.geomprop, input.geomprop_user, false};
  if (input.geomprop) {
    context.geomprop = &ReadVolumeGeomProp;
    context.geomprop_user = &geomprop;
  }
  context.doc = impl_->doc.get();
  context.tex = impl_->cache.get();
  for (size_t i = 0; i < 2; ++i) {
    context.uv[i] = input.uv[i];
    context.uv_dx[i] = input.uv_dx[i];
    context.uv_dy[i] = input.uv_dy[i];
  }
  const auto vec = [](const float v[3]) { return v3_make(v[0], v[1], v[2]); };
  context.has_uv_derivatives = input.has_uv_derivatives;
  context.P = vec(input.position);
  context.dPdx = vec(input.position_dx);
  context.dPdy = vec(input.position_dy);
  context.has_position_derivatives = input.has_position_derivatives;
  context.Ns = vec(input.shading_normal);
  context.Ng = vec(input.geometric_normal);
  context.dpdu = vec(input.tangent);
  context.dpdv = vec(input.bitangent);
  context.V = vec(input.view);
  context.dVdx = vec(input.view_dx);
  context.dVdy = vec(input.view_dy);
  context.has_view_derivatives = input.has_view_derivatives;
  context.time = input.time;
  context.frame = input.frame;
  context.has_space_transforms = input.has_space_transforms;
  if (input.has_space_transforms) {
    for (size_t i = 0; i < 16; ++i) {
      context.object_to_world[i] = input.object_to_world[i];
      context.world_to_object[i] = input.world_to_object[i];
      context.world_to_view[i] = input.world_to_view[i];
      context.view_to_world[i] = input.view_to_world[i];
    }
  }
  context.memo = impl_->memo.data();
  context.memo_done = impl_->done.data();
  MtlxVolumeParams volume{};
  if (mtlx_eval_volume(&context, impl_->node, &volume)) {
    impl_->error = "Unsupported or invalid MaterialX volume graph";
    return false;
  }
  if (geomprop.invalid) {
    impl_->error = "MaterialX geometry property returned a non-finite value";
    return false;
  }
  if (texcache_had_load_failure(impl_->cache.get())) {
    impl_->error = impl_->reader.error.empty() ? "Failed to decode MaterialX volume texture" : impl_->reader.error;
    return false;
  }
  const float values[] = {volume.absorption.x, volume.absorption.y,
      volume.absorption.z, volume.scattering.x, volume.scattering.y,
      volume.scattering.z, volume.emission.x, volume.emission.y,
      volume.emission.z, volume.anisotropy};
  for (float value : values) {
    if (!std::isfinite(value)) {
      impl_->error = "MaterialX volume graph produced a non-finite value";
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
  static_assert(kMtlxVolumeMaxScatteringLobes == MTLX_MAX_VOLUME_SCATTERING_LOBES,
                "MaterialX volume lobe limits must match");
  sample.lobe_count = static_cast<size_t>(volume.lobe_count);
  for (size_t i = 0; i < sample.lobe_count; ++i) {
    const auto& lobe = volume.lobes[i];
    if (!std::isfinite(lobe.scattering.x) || !std::isfinite(lobe.scattering.y) ||
        !std::isfinite(lobe.scattering.z) || !std::isfinite(lobe.anisotropy)) {
      impl_->error = "MaterialX volume scattering lobe produced a non-finite value";
      return false;
    }
    sample.lobes[i].scattering[0] = lobe.scattering.x;
    sample.lobes[i].scattering[1] = lobe.scattering.y;
    sample.lobes[i].scattering[2] = lobe.scattering.z;
    sample.lobes[i].anisotropy = lobe.anisotropy;
  }
  *out = sample;
  return true;
}

bool MtlxVolumeEvaluator::EvaluateWithFields(const RenderVolume& volume,
                                              vol::FieldCache& cache,
                                              const MtlxVolumeContext& input,
                                              MtlxVolumeSample* out) {
  if (!out) { impl_->error = "Volume output must be provided"; return false; }
  VolumeFieldReader reader;
  reader.volume = &volume;
  reader.cache = &cache;
  reader.context = &input;
  reader.values = &impl_->field_values;
  MtlxVolumeContext context = input;
  context.geomprop = &ReadVolumeField;
  context.geomprop_user = &reader;
  MtlxVolumeSample sample;
  const bool evaluated = Evaluate(context, &sample);
  if (!reader.error.empty()) { impl_->error = reader.error; return false; }
  if (!evaluated) return false;
  *out = sample;
  return true;
}

}  // namespace tydra
}  // namespace lightusd
