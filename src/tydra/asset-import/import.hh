// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "minijson.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
using Json = ::lightusd::minijson::Value;
using AssetBytes = std::shared_ptr<const std::vector<uint8_t>>;
using AssetMap = std::map<std::string, AssetBytes>;
struct AssetSource {
  std::string identifier;
  /// Final identity after alias resolution (for example a shared texture
  /// payload); empty means `identifier`. Only byte caches key on this, so
  /// authored identifiers keep their spelling in USD output.
  std::string canonical;
  std::function<bool(std::vector<uint8_t>*, std::string*)> read;
};
enum class UnsupportedPolicy { Warn, Error, Preserve };
// Versioned, bounded import telemetry. Estimates describe importer accounting,
// not process RSS. A caller may reuse a collector between synchronous imports;
// each outermost ImportSession resets it. No resolver callbacks are retained.
struct ImportReport {
  Json diagnostics = Json::array(), dependencies = Json::array();
  Json features = Json::array(), counts = Json::object();
  std::string source, format, error, status = "pending";
  size_t input_bytes = 0, peak_memory = 0, retained_texture_bytes = 0;
  size_t dependency_reads = 0, dependency_bytes = 0, texture_cache_hits = 0;
  size_t dropped_records = 0, truncated_fields = 0;
  size_t max_memory = 0, max_input = 0, max_depth = 0;
  double elapsed_seconds = 0;
  void Diagnostic(const char* severity, const char* code,
                  const std::string& message, const std::string& location = {},
                  size_t line = 0, const std::string& prim_path = {});
  void Dependency(const std::string& name, const std::string& anchor,
                  const std::string& identifier, size_t bytes, bool read,
                  bool cached, bool success, bool texture);
  void Feature(const char* name, const char* disposition,
               const std::string& location = {}, size_t line = 0);
  void ObserveMemory(size_t bytes) { if (bytes > peak_memory) peak_memory = bytes; }
  Json ToJSON() const;
 private:
  friend struct ImportSession;
  size_t sessions_ = 0, storage_bytes_ = 0, storage_limit_ = 1024 * 1024;
  bool Reserve(size_t bytes, size_t entries);
  std::string Text(const std::string& text);
};
struct Options {
  std::string format = "auto";
  std::string up_axis = "Z";
  // USD output profile: "newton" (Newton 1.8 / newton-usd-schemas 0.6 /
  // MuJoCo 3.16 decoder parity), "openusd" (standard schemas only) or
  // "legacy" (historical output). See doc/usd-physics.md.
  std::string profile = "newton";
  // Keep mesh positions/normals/uvs/indices as typed buffers (Result::meshes)
  // instead of one JSON value per scalar. Consumers must pass them to the
  // converter; JSON dumps then carry only `meshRef`.
  bool typed_meshes = false;
  // URDF/MJCF link prims: "flat" (all under /World/Links) or "nested" (each
  // dynamic link under its parent link, following the kinematic tree).
  std::string layout = "flat";
  // Progress/cancellation: called with a phase name, bytes accounted so far
  // and the memory budget as the import proceeds; returning false cancels it
  // ("Import cancelled").
  std::function<bool(const char* phase, uint64_t done, uint64_t total)> progress;
  // OBJ import topology: "expanded" (split points per unique corner) or
  // "indexed" (source positions, face-varying indexed normals/UVs).
  std::string obj_topology = "expanded";
  bool allow_missing = false;
  bool tessellate_collision_shapes = false;
  UnsupportedPolicy unsupported_policy = UnsupportedPolicy::Warn;
  size_t max_memory = size_t(1024) * 1024 * 1024;
  size_t max_input = size_t(512) * 1024 * 1024;
  size_t max_depth = 64;
  std::vector<std::string> asset_dirs;
  std::map<std::string, std::string> packages;
  std::shared_ptr<ImportReport> report;
  // Resolve and read an asset, returning its resolved identifier. False may
  // carry a diagnostic; no direct filesystem fallback follows a callback.
  std::function<bool(const std::string&, const std::string&,
                     std::vector<uint8_t>*, std::string*, std::string*)>
      read;
  // Separate resolution from I/O so aliases can share retained texture bytes.
  // The source reader keeps any resolver identity/security checks captured by
  // resolution. When supplied, it is authoritative and takes precedence.
  std::function<bool(const std::string&, const std::string&, AssetSource*,
                     std::string*)> open;
};
// Nestable so file I/O, parsing and conversion contribute to one report.
struct ImportSession {
  ImportSession(Options* options, const std::string& source,
                const std::string& format, const std::string* error);
  ImportSession(const ImportSession&) = delete;
  ImportSession& operator=(const ImportSession&) = delete;
  ~ImportSession();
  void Succeed() { success_ = true; }
 private:
  std::shared_ptr<ImportReport> report_;
  const std::string* error_;
  std::chrono::steady_clock::time_point started_;
  bool success_ = false;
};
// Mesh arrays moved out of the payload JSON (Options::typed_meshes); the
// payload references them with `meshRef`.
struct MeshBuffer {
  std::vector<float> positions, normals, uvs;
  std::vector<int32_t> indices;
};
struct Result {
  Json payload;
  std::map<std::string, MeshBuffer> meshes;
  size_t memory_estimate = 0;  // OBJ conversion budget, including the input.
  AssetMap assets;
  std::string warnings;
  std::shared_ptr<ImportReport> report;
};
struct Reader {
  explicit Reader(const Options& o) : options(o) {}
  const Options& options;
  size_t bytes = 0;
  std::string warnings, failure, root_source;
  bool allow_unanchored = true;
  size_t reported_bytes = 0;  // last progress callback
  AssetMap textures;
  std::map<std::string, MeshBuffer> meshes;
  bool obj_indexed = false;  // set by the OBJ scene import (obj_topology)
  struct TextureRequest {
    std::string resolved;   // authored/resolved identifier for USD output
    std::string canonical;  // retained-payload cache key (alias target)
  };
  std::map<std::pair<std::string, std::string>, TextureRequest>
      texture_requests;
  Json preserved = Json::array();
  bool Open(const std::string& name, const std::string& anchor,
            AssetSource* source, std::string* err);
  bool Read(const std::string& name, const std::string& anchor,
            std::vector<uint8_t>* data, std::string* resolved,
            std::string* err);
  bool ReadTexture(const std::string& name, const std::string& anchor,
                   AssetBytes* data, std::string* resolved, std::string* err);
  bool Charge(size_t amount, std::string* err);
  bool ChargeArray(size_t count, size_t stride, std::string* err);
  void Warn(const std::string& message, const char* code = "import.warning",
            const std::string& source = {}, size_t line = 0);
  bool Unsupported(const std::string& message, const char* code,
                   const char* feature, const std::string& source,
                   size_t line = 0, std::string data = {},
                   const char* encoding = "xml", bool approximated = false);
};
struct Mesh {
  std::string name;
  std::string type = "Mesh";
  std::vector<float> positions, normals, uvs, colors;
  std::vector<int32_t> indices, counts, materials;
  std::vector<int32_t> source_vertices, joint_indices;
  // Compact OBJ topology: per-corner indices into the normals/uvs tables.
  std::vector<int32_t> normal_indices, uv_indices;
  std::vector<float> joint_weights;
  int32_t influences = 0;
};
struct OBJ {
  std::vector<Mesh> meshes;
  Json materials = Json::array();
};
template <class T>
Json Array(const std::vector<T>& values) {
  Json result = Json::array();
  result.reserve(values.size());
  for (const T& value : values) result.push_back(Json(value));
  return result;
}
bool ReadOBJ(std::string_view text, const std::string& source, Reader& reader,
             bool triangulate, OBJ* obj, std::string* err);
// COLLADA triangle meshes for URDF/MJCF assets (same layout as ReadOBJ).
bool ReadDAE(std::string_view text, const std::string& source, Reader& reader,
             OBJ* obj, std::string* err);
// Typed geometry path for large OBJ inputs; Result carries only materials/assets.
bool BuildOBJScene(std::string_view text, const std::string& source,
                   const Options& options, OBJ* obj, Result* result,
                   std::string* err);
bool BuildOBJPayload(std::string_view text, const std::string& source,
                     const Options& options, Result* result, std::string* err);
bool BuildOBJPayloadFile(const std::string& filename, const Options& options,
                         Result* result, std::string* err);
Json MeshJSON(const Mesh& mesh);
// MeshJSON, or with Options::typed_meshes the large arrays registered in
// reader.meshes and referenced by `meshRef`.
Json MeshRefJSON(const Mesh& mesh, Reader& reader);
bool BuildPayload(const std::string& text, const std::string& source,
                  const Options& options, Result* result, std::string* err);
bool BuildPayloadFile(const std::string& filename, const Options& options,
                      Result* result, std::string* err);
// Used by standalone OBJ imports and robot frontends to retain material maps.
bool GatherTextures(Reader& reader, Result* result, std::string* err);
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
