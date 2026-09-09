// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <emscripten/val.h>
#include <new>
#include "binding-next-api.h"
#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "next/diff/layer-diff.hh"
#include "next/pcp/layer-registry.hh"
#include "next/pipeline/flatten.hh"
#include "next/validation/usd-validation.hh"
#include "tsd/tinysubdiv.hh"

#include "minijson.hh"
#include "binding-next-scene.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-xform.hh"
#include "next/schema/color-space.hh"
#include "next/schema/usd-shade.hh"
#include "next/stage/stage.hh"
#include "next/lightusd-next.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "next/writer/value-printer.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/render-data.hh"
#include "tydra/next/render-extract.hh"
#include "tydra/next/urdf-to-usd.hh"


namespace tn = lightusd::next;
namespace tr = lightusd::tydra::next;
namespace lightusd {
namespace web_next {
extern "C" void reportNextCrateProgress(const char* phase, double current, double total)
    EM_IMPORT(reportNextCrateProgress);
std::string CopyUint8ArrayToString(const emscripten::val& bytes,
                                   std::string* error);
bool IsUSDCBytes(const std::string& bytes);
emscripten::val Uint8ArrayFromBytes(const uint8_t* data, size_t size);
emscripten::val Uint8ArrayFromString(const std::string& s);
emscripten::val Uint8ArrayFromVector(const std::vector<uint8_t>& v);
std::array<double, 16> IdentityMatrix();
std::array<double, 16> MatrixToArray(const tr::Matrix4& m);
const char* NodeTypeName(tr::NodeType type);
const char* InterpolationName(tr::Interpolation interpolation);
const char* CurveTypeName(tr::CurveType type);
const char* CurveBasisName(tr::CurveBasis basis);
const char* CurveWrapName(tr::CurveWrap wrap);
const char* LightTypeName(tr::LightType type);
const char* AnimationPathName(tr::AnimationChannel::TargetPath path);
const char* AnimationInterpolationName(
    tr::AnimationChannel::Interpolation interp);
const char* CameraTypeName(tr::CameraType type);
emscripten::val MatrixValue(const std::array<double, 16>& m);
emscripten::val Matrix3Value(const float m[9]);
emscripten::val Float3Value(const tr::Float3& c);
emscripten::val Float4Value(const tr::Float4& c);
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
void AppendAnimationKeyframeValues(const tr::AnimationChannel& channel,
                                        const tr::Keyframe& keyframe,
                                        std::vector<float>* values);
int AnimationTargetNodeCount(const tr::AnimationClip& clip);
bool AnimationHasSkeletalChannels(const tr::AnimationClip& clip);
std::unique_ptr<tn::Layer> ParseNextLayerBytes(
    const uint8_t* data, size_t size, const std::string& key,
    const tn::CrateReadOptions& read_opts, std::string* error);
template <typename T>
void ClearVector(std::vector<T>* v) {
  if (!v) return;
  std::vector<T>().swap(*v);
}

template <typename T>
emscripten::val VectorToArray(const std::vector<T>& values) {
  emscripten::val array = emscripten::val::array();
  for (const auto& v : values) array.call<void>("push", v);
  return array;
}

template <typename Chunked>
float ChunkedFloatAt(const Chunked& values, size_t i, float fallback = 0.0f) {
  return i < values.size() ? values[i] : fallback;
}

template <typename T>
void CopyTypedArrayToVector(const emscripten::val& v, std::vector<T>& out) {
  out.clear();
  if (v.isNull() || v.isUndefined()) return;
  const size_t len = v["length"].as<size_t>();
  if (!len) return;
  out.resize(len);
  emscripten::val heap =
      emscripten::val(emscripten::typed_memory_view(len, out.data()));
  heap.call<void>("set", v);
}
}  // namespace web_next
}  // namespace lightusd
