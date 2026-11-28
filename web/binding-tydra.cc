// SPDX-License-Identifier: Apache-2.0
//
// Dedicated Tydra render-conversion WASM feature.
//
// This module keeps its own heap and owns every C++ object it creates: USD
// bytes are copied in from the caller's heap, a Stage and RenderScene are
// built inside this module, and a bounded UTF-8 JSON summary is copied back
// out. No C++ object or pointer is shared with the core module, so the two
// heaps remain independent (see loadFeature in core-api.js).

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "load-usd.hh"
#include "tydra/render-converter.hh"

namespace {

constexpr uint32_t kMaxInputBytes = uint32_t{256} << 20;
constexpr uint32_t kMaxOutputBytes = uint32_t{64} << 20;

uint8_t* CopyOut(const std::string& text) {
  if (text.size() >= kMaxOutputBytes) return nullptr;
  auto* out = static_cast<uint8_t*>(std::malloc(text.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return out;
}

void AppendEscaped(std::string* out, const std::string& value) {
  static const char* kHex = "0123456789abcdef";
  out->push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          out->append("\\u00");
          out->push_back(kHex[c >> 4]);
          out->push_back(kHex[c & 0xf]);
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

// Stateful render streams. Each handle is owned by this module's heap; only
// bytes cross the boundary, never a C++ object from another WASM instance.
struct Stream {
  std::string summary;
};
std::unordered_map<uint32_t, Stream> streams;
uint32_t next_stream_handle = 1;

std::string Fail(const std::string& message) {
  std::string out = "{\"ok\":false,\"error\":";
  AppendEscaped(&out, message);
  out.push_back('}');
  return out;
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void* lu_feature_alloc(uint32_t size) {
  return std::malloc(size);
}
EMSCRIPTEN_KEEPALIVE void lu_feature_free(void* data) { std::free(data); }

// Convert one in-memory USD layer into a bounded JSON scene summary.
// Returns a NUL-terminated JSON string allocated with lu_feature_alloc (free
// it with lu_feature_free), or nullptr when the input is rejected outright.
static std::string ConvertSummary(const uint8_t* data, uint32_t size,
                                  uint32_t max_memory_mb) {
  if ((!data && size) || size > kMaxInputBytes) {
    return Fail("invalid or oversized input");
  }
  if (max_memory_mb < 8 || max_memory_mb > 4096) max_memory_mb = 512;

  const size_t memory_bytes = static_cast<size_t>(max_memory_mb) << 20;
  std::string warn, err;
  lightusd::Stage stage;
  lightusd::LoadUSDOptions options;
  options.limits.max_input_bytes = size;
  options.limits.max_resident_bytes = memory_bytes;
  if (!lightusd::LoadUSDFromMemory(data ? data : reinterpret_cast<const uint8_t*>(""),
                                   size, &stage, options, &warn, &err)) {
    return Fail(err.empty() ? "failed to load USD layer" : err);
  }

  lightusd::tydra::ConverterConfig config =
      lightusd::tydra::MakeHardenedConverterConfig(memory_bytes);
  lightusd::tydra::RenderSceneConverter converter(config);
  lightusd::tydra::ConvertResult converted = converter.Convert(stage);
  if (!converted.success) {
    return Fail(converted.error.empty() ? "render conversion failed"
                                         : converted.error);
  }

  const auto& scene = converted.scene;
  std::string out = "{\"ok\":true,\"meshes\":";
  out += std::to_string(scene.meshes.size());
  out += ",\"nodes\":";
  out += std::to_string(scene.nodes.size());
  out += ",\"materials\":";
  out += std::to_string(scene.materials.size());
  out += ",\"textures\":";
  out += std::to_string(scene.textures.size());
  out += ",\"images\":";
  out += std::to_string(scene.images.size());
  out += ",\"lights\":";
  out += std::to_string(scene.lights.size());
  out += ",\"cameras\":";
  out += std::to_string(scene.cameras.size());
  out += ",\"points\":";
  out += std::to_string(scene.points.size());
  out += ",\"curves\":";
  out += std::to_string(scene.curves.size());
  out += ",\"skeletons\":";
  out += std::to_string(scene.skeletons.size());
  out += ",\"animations\":";
  out += std::to_string(scene.animations.size());
  out += ",\"rootNodes\":";
  out += std::to_string(scene.root_nodes.size());
  out += ",\"warnings\":[";
  bool first = true;
  for (const auto& warning : converted.warnings) {
    if (!first) out.push_back(',');
    first = false;
    AppendEscaped(&out, warning);
  }
  out += "],\"firstMesh\":";
  if (scene.meshes.empty()) {
    out += "null";
  } else {
    const auto& mesh = scene.meshes.front();
    out += "{\"name\":";
    AppendEscaped(&out, mesh.name);
    out += ",\"primPath\":";
    AppendEscaped(&out, mesh.prim_path);
    out += ",\"pointCount\":";
    out += std::to_string(mesh.point_count());
    out += ",\"indexCount\":";
    out += std::to_string(mesh.face_vertex_indices.size());
    out.push_back('}');
  }
  if (!warn.empty()) {
    out += ",\"loadWarning\":";
    AppendEscaped(&out, warn);
  }
  out.push_back('}');
  return out;
}

// Stateless convenience: copy bytes in, copy the summary back out.
EMSCRIPTEN_KEEPALIVE uint8_t* lu_tydra_convert(const uint8_t* data,
                                               uint32_t size,
                                               uint32_t max_memory_mb) {
  return CopyOut(ConvertSummary(data, size, max_memory_mb));
}

// Stateful RenderStream owned entirely by this module. A handle is an index
// into this heap only; no pointer or C++ object is shared with the core.
EMSCRIPTEN_KEEPALIVE uint32_t lu_tydra_stream_create() {
  const uint32_t handle = next_stream_handle++;
  streams.emplace(handle, Stream{});
  return handle;
}
EMSCRIPTEN_KEEPALIVE void lu_tydra_stream_destroy(uint32_t handle) {
  streams.erase(handle);
}
EMSCRIPTEN_KEEPALIVE int lu_tydra_stream_begin(uint32_t handle,
                                               const uint8_t* data,
                                               uint32_t size,
                                               uint32_t max_memory_mb) {
  auto it = streams.find(handle);
  if (it == streams.end()) return -1;
  it->second.summary = ConvertSummary(data, size, max_memory_mb);
  return it->second.summary.rfind("{\"ok\":true", 0) == 0 ? 0 : -2;
}
EMSCRIPTEN_KEEPALIVE uint8_t* lu_tydra_stream_summary(uint32_t handle) {
  auto it = streams.find(handle);
  if (it == streams.end()) return nullptr;
  return CopyOut(it->second.summary);
}
EMSCRIPTEN_KEEPALIVE void lu_tydra_stream_end(uint32_t handle) {
  auto it = streams.find(handle);
  if (it != streams.end()) it->second.summary.clear();
}

}  // extern "C"
