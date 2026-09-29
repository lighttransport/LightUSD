// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "binding-next-api.h"
#include <emscripten/emscripten.h>
#include "tydra/next/render-data.hh"

namespace lightusd {
namespace next { class Layer; struct CrateReadOptions; }
}

namespace tn = lightusd::next;
namespace tr = lightusd::tydra::next;
namespace lightusd {
namespace web_next {
extern "C" bool reportNextLoadProgress(const char* phase, double current, double total)
    EM_IMPORT(reportNextLoadProgress);
extern "C" bool reportNextLayerLoadProgress(const char* phase, double current,
                                             double total)
    EM_IMPORT(reportNextLayerLoadProgress);
bool IsUSDCBytes(const std::string& bytes);
std::array<double, 16> IdentityMatrix();
std::array<double, 16> MatrixToArray(const tr::Matrix4& m);
const char* LightTypeName(tr::LightType type);
uint32_t ChunkedU32At(const tr::UInt32Chunked& values, size_t i,
                      uint32_t fallback = 0);
const tr::RenderTexture* TextureAt(const tr::RenderScene& scene, int32_t id);
std::string TexturePath(const tr::RenderScene& scene, const tr::ShaderParam& p);
std::string MaterialKey(const tr::RenderScene& scene, int32_t material_id);
std::string JsonEscape(const std::string& s);
const char* RenderMaterialShaderTypeName(tr::RenderMaterial::ShaderType type);
std::string ShaderParamJson(const tr::RenderScene& scene,
                            const tr::ShaderParam& p);
std::string RenderMaterialJson(const tr::RenderScene& scene,
                               const tr::RenderMaterial& mat);
size_t AnimationComponentCount(const tr::AnimationChannel& channel);
int AnimationTargetNodeCount(const tr::AnimationClip& clip);
std::unique_ptr<tn::Layer> ParseNextLayerBytes(
    const uint8_t* data, size_t size, const std::string& key,
    const tn::CrateReadOptions& read_opts, std::string* error);
template <typename T>
void ClearVector(std::vector<T>* v) {
  if (!v) return;
  std::vector<T>().swap(*v);
}

template <typename Chunked>
float ChunkedFloatAt(const Chunked& values, size_t i, float fallback = 0.0f) {
  return i < values.size() ? values[i] : fallback;
}

}  // namespace web_next
}  // namespace lightusd
