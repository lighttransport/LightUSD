// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Lean Emscripten binding for the next-core + tydra-next render path.
// This intentionally avoids the legacy lightusd_static library and exposes the
// RenderStream contract consumed by web/js/src/lightusd/LightUSDLoader.js for
// first-stage `backend=next` browser coverage.

#include <emscripten/val.h>
#include <new>
#include "binding-next-api.h"
#include "binding-next-render.hh"
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
using lightusd::web_next::NextPropertyConnections;
using lightusd::web_next::NextConnectionPrimPath;
using lightusd::web_next::BuildNextNodeGraphJson;
using lightusd::web_next::CanonicalMaterialGraph;
using lightusd::web_next::AppendNextPhysicsPrimJSON;

using namespace lightusd::web_next;

// ===========================================================================
// SubdivStreamer.refineStream(...) refines a control mesh and delivers the
// refined surface to a JS callback in bounded batches (zero-copy heap views),
// so the full level-N output never resides in the wasm heap at once. Ported
// verbatim from the legacy module (pure tsd::, no legacy-core dependency).
// ===========================================================================
class SubdivStreamer {
 public:
  // points: Float32Array (xyz interleaved). fvc/fvi: Uint32Array.
  // scheme: 0=catmullClark, 1=loop, 2=bilinear.
  // boundary: 0=edgeAndCorner, 1=edgeOnly, 2=none.
  // uvValues: Float32Array (stride 2) or null/empty for no texturing.
  // uvIndices: Uint32Array (per face-corner) or null for identity.
  // uvInterp: 0 = linear ("all"); 1 = smooth seam-split ("cornersPlus1").
  // batchFaces: parent faces per output batch (0 => default).
  // blockFaces: >0 bounds the WORKING set; 0 => whole-mesh streaming.
  // haloRings: block halo radius (0 => library default).
  // onBatch(positions, normals|null, indices, faceSource, uv|null,
  //         numVertices, numFaces, batchIndex): views valid only for the call.
  // Returns "" on success, else an error message.
  std::string refineStream(const emscripten::val &points,
                           const emscripten::val &fvc,
                           const emscripten::val &fvi,
                           const emscripten::val &uvValues,
                           const emscripten::val &uvIndices, int uvInterp,
                           int scheme, int boundary, int level, int batchFaces,
                           int blockFaces, int haloRings, bool wantNormals,
                           emscripten::val onBatch) {
    namespace tsd = lightusd::tsd;

    std::vector<float> pts;
    std::vector<uint32_t> counts;
    std::vector<uint32_t> indices;
    CopyTypedArrayToVector(points, pts);
    CopyTypedArrayToVector(fvc, counts);
    CopyTypedArrayToVector(fvi, indices);
    if ((pts.size() % 3) != 0) {
      return "points length must be a multiple of 3";
    }
    if (counts.empty() || indices.empty()) {
      return "empty mesh";
    }

    std::vector<float> uvs;
    std::vector<uint32_t> uvidx;
    CopyTypedArrayToVector(uvValues, uvs);
    CopyTypedArrayToVector(uvIndices, uvidx);
    const bool has_uv = (uvs.size() >= 2) && ((uvs.size() % 2) == 0);

    tsd::MeshView mesh;
    mesh.points = pts.data();
    mesh.num_points = uint32_t(pts.size() / 3);
    mesh.face_vertex_counts = counts.data();
    mesh.num_faces = uint32_t(counts.size());
    mesh.face_vertex_indices = indices.data();
    mesh.num_face_vertex_indices = uint32_t(indices.size());

    tsd::FVarChannelView uvchan;
    if (has_uv) {
      uvchan.values = uvs.data();
      uvchan.num_values = uint32_t(uvs.size() / 2);
      uvchan.indices = uvidx.empty() ? nullptr : uvidx.data();
      uvchan.stride = 2;
      uvchan.interpolation = (uvInterp == 1)
                                 ? tsd::FVarLinearInterpolation::CornersPlus1
                                 : tsd::FVarLinearInterpolation::All;
    }

    tsd::Options opts;
    opts.scheme = (scheme == 1)   ? tsd::Scheme::Loop
                  : (scheme == 2) ? tsd::Scheme::Bilinear
                                  : tsd::Scheme::CatmullClark;
    opts.boundary = (boundary == 1)   ? tsd::BoundaryInterpolation::EdgeOnly
                    : (boundary == 2) ? tsd::BoundaryInterpolation::None
                                      : tsd::BoundaryInterpolation::EdgeAndCorner;
    opts.level = level;
    opts.remove_holes = true;

    tsd::StreamOptions so;
    so.batch_faces = (batchFaces > 0) ? uint32_t(batchFaces) : 4096u;
    so.emit_triangles = true;
    so.want_normals = wantNormals;
    so.dedup_within_batch = true;
    so.block_faces = (blockFaces > 0) ? uint32_t(blockFaces) : 0u;
    so.halo_rings = (haloRings > 0) ? uint32_t(haloRings) : 0u;

    struct SinkCtx {
      emscripten::val *cb;
      bool want_normals;
    } ctx{&onBatch, wantNormals};

    auto sink = [](void *user, const tsd::StreamBatch *b) -> bool {
      SinkCtx *c = static_cast<SinkCtx *>(user);
      emscripten::val pos(emscripten::typed_memory_view(
          size_t(b->num_vertices) * 3, const_cast<float *>(b->positions)));
      emscripten::val nrm =
          (c->want_normals && b->normals)
              ? emscripten::val(emscripten::typed_memory_view(
                    size_t(b->num_vertices) * 3, const_cast<float *>(b->normals)))
              : emscripten::val::null();
      emscripten::val idx(emscripten::typed_memory_view(
          size_t(b->num_indices), const_cast<uint32_t *>(b->indices)));
      emscripten::val fsrc(emscripten::typed_memory_view(
          size_t(b->num_faces), const_cast<uint32_t *>(b->face_source)));
      emscripten::val uv =
          (b->num_fvar == 1)
              ? emscripten::val(emscripten::typed_memory_view(
                    size_t(b->num_indices) * 2,
                    const_cast<float *>(b->fvar[0].values)))
              : emscripten::val::null();
      (*c->cb)(pos, nrm, idx, fsrc, uv, b->num_vertices, b->num_faces,
               b->batch_index);
      return true;
    };

    std::string err;
    const tsd::Result r = tsd::RefineStream(
        mesh, has_uv ? &uvchan : nullptr, has_uv ? 1u : 0u, nullptr, 0, opts, so,
        sink, &ctx, &err);
    if (r != tsd::Result::Success) {
      return std::string("RefineStream failed (") + tsd::to_string(r) +
             "): " + err;
    }
    return "";
  }

  // Total wasm linear-memory bytes (grow-only => heap high-water mark).
  double heapBytes() const {
    return emscripten::val::module_property("HEAPU8")["length"].as<double>();
  }
};

// Resumable multi-layer flatten session: JS drives a need-layer loop,
// providing dependency layer bytes (USDA / USDC / USDZ, fetched over HTTP or
// pulled from a package) until the flatten converges, then receives a
// flattened USDC buffer. One instance = one session; mirrors the legacy
// module's nextFlattenAsyncBegin/ProvideLayer/Step/End protocol with the
// session id replaced by the instance.
class NextFlattenSession {
 public:
  NextFlattenSession() = default;

  emscripten::val begin(emscripten::val rootBytes, const std::string& rootName,
                        bool lazyArrays) {
    emscripten::val result = emscripten::val::object();
    reset_();
    std::string copy_error;
    root_ = CopyUint8ArrayToString(rootBytes, &copy_error);
    if (root_.empty()) {
      result.set("success", false);
      result.set("error", copy_error.empty()
                              ? std::string("empty root layer buffer")
                              : copy_error);
      return result;
    }
    root_name_ = rootName;
    lazy_arrays_ = lazyArrays;
    began_ = true;
    result.set("success", true);
    result.set("status", "ready");
    return result;
  }

  // Key is a variant-set name ("shape", applies stage-wide) or the
  // prim-scoped form "<primPath>{<set>}" (wins over the bare-set key).
  emscripten::val setVariantOverride(const std::string& set_or_scoped_key,
                                     const std::string& selection) {
    emscripten::val result = emscripten::val::object();
    variant_overrides_[set_or_scoped_key] = selection;
    result.set("success", true);
    return result;
  }

  emscripten::val provideLayer(const std::string& key, emscripten::val data) {
    emscripten::val result = emscripten::val::object();
    if (!began_) {
      result.set("success", false);
      result.set("error", "session not started (call begin first)");
      return result;
    }
    std::string copy_error;
    std::string bytes = CopyUint8ArrayToString(data, &copy_error);
    if (bytes.empty()) {
      result.set("success", false);
      result.set("error", "Invalid or empty layer data for: " + key);
      return result;
    }
    std::string norm_key = tn::AssetResolver::NormalizePath(key);
    while (norm_key.rfind("./", 0) == 0) norm_key = norm_key.substr(2);
    layers_[norm_key] = std::move(bytes);
    parsed_layers_.erase(norm_key);
    result.set("success", true);
    return result;
  }

  emscripten::val step(emscripten::val chunkCb) {
    emscripten::val result = emscripten::val::object();
    if (!began_) {
      result.set("success", false);
      result.set("error", "session not started (call begin first)");
      return result;
    }

    tn::pipeline::FlattenOptions opts;
    opts.read.lazy_arrays = lazy_arrays_;
    opts.root_anchor_path = root_name_;
    opts.fail_on_composition_error = true;
    opts.composition.variant_overrides = variant_overrides_;

    using tn::AssetResolver;
    AssetResolver resolver;
    std::string missing_key;
    auto consumed = std::make_shared<std::set<std::string>>();
    auto resolved_cache =
        std::make_shared<std::map<std::string, std::string>>();
    resolver.SetCustomResolver(
        [this, consumed, resolved_cache](
            const std::string& asset, const std::string& anchor) -> std::string {
          const std::string cache_key = anchor + "\n" + asset;
          auto hit = resolved_cache->find(cache_key);
          if (hit != resolved_cache->end()) return hit->second;
          auto try_key = [this, &consumed](std::string key) -> std::string {
            key = AssetResolver::NormalizePath(key);
            while (key.rfind("./", 0) == 0) key = key.substr(2);
            return (layers_.count(key) || consumed->count(key))
                       ? key
                       : std::string();
          };
          if (!anchor.empty()) {
            std::string k = try_key(AssetResolver::JoinPath(
                AssetResolver::GetDirectory(anchor), asset));
            if (!k.empty()) {
              (*resolved_cache)[cache_key] = k;
              return k;
            }
          }
          {
            std::string k = try_key(asset);
            if (!k.empty()) {
              (*resolved_cache)[cache_key] = k;
              return k;
            }
          }
          for (const auto& cand : AssetResolver::SuffixCandidates(asset)) {
            std::string k = try_key(cand);
            if (!k.empty()) {
              (*resolved_cache)[cache_key] = k;
              return k;
            }
          }
          // Return the best normalized candidate so the loader can surface
          // exactly which layer JS should fetch.
          std::string request = asset;
          if (!anchor.empty()) {
            request = AssetResolver::JoinPath(
                AssetResolver::GetDirectory(anchor), asset);
          }
          request = AssetResolver::NormalizePath(request);
          while (request.rfind("./", 0) == 0) request = request.substr(2);
          (*resolved_cache)[cache_key] = request;
          return request;
        });
    opts.resolver = &resolver;

    const tn::CrateReadOptions read_opts = opts.read;
    opts.layer_loader = [this, read_opts, consumed, &missing_key](
                            const std::string& key,
                            std::string* error) -> std::unique_ptr<tn::Layer> {
      auto cached = parsed_layers_.find(key);
      if (cached != parsed_layers_.end() && cached->second) {
        consumed->insert(key);
        std::unique_ptr<tn::Layer> layer(
            new tn::Layer(cached->second->Clone()));
        layer->build_path_index();
        return layer;
      }

      auto it = layers_.find(key);
      if (it == layers_.end()) {
        missing_key = key;
        if (error) *error = "NEED_LAYER:" + key;
        return nullptr;
      }
      consumed->insert(key);
      const std::string& src = it->second;
      std::unique_ptr<tn::Layer> layer = ParseNextLayerBytes(
          reinterpret_cast<const uint8_t*>(src.data()), src.size(), key,
          read_opts, error);
      if (!layer) return nullptr;
      parsed_layers_[key] =
          std::shared_ptr<tn::Layer>(new tn::Layer(layer->Clone()));
      return layer;
    };

    const bool buffered = chunkCb.isNull() || chunkCb.isUndefined();
    tn::pipeline::FlattenStats stats;
    std::string err;
    bool ok = false;
    std::vector<uint8_t> out;
    bool aborted = false;
    const uint8_t* root_data = reinterpret_cast<const uint8_t*>(root_.data());
    const size_t root_size = root_.size();
    if (buffered) {
      ok = tn::pipeline::FlattenUSDMemoryToUSDC(root_name_, root_data,
                                                root_size, out, opts, &stats,
                                                &err);
    } else {
      opts.write.streaming = true;
      tn::CrateWriteSink sink = [&](const uint8_t* data, size_t size) -> bool {
        emscripten::val view(emscripten::typed_memory_view(size, data));
        emscripten::val r = chunkCb(view);
        if (r.isFalse()) {
          aborted = true;
          return false;
        }
        return true;
      };
      ok = tn::pipeline::FlattenUSDMemoryToUSDCToSink(
          root_name_, root_data, root_size, sink, opts, &stats, &err);
    }

    if (!missing_key.empty()) {
      result.set("success", true);
      result.set("status", "need-layer");
      result.set("key", missing_key);
      return result;
    }
    result.set("success", ok);
    if (!ok) {
      if (aborted) {
        result.set("success", true);
        result.set("status", "ready");
        return result;
      }
      result.set("status", "error");
      result.set("error", err);
      return result;
    }

    result.set("status", "done");
    if (buffered) result.set("data", Uint8ArrayFromVector(out));
    result.set("inputBytes", static_cast<double>(stats.input_bytes));
    result.set("outputBytes", static_cast<double>(stats.output_bytes));
    result.set("primCount", static_cast<double>(stats.prim_count));
    result.set("arraysPassedThrough",
               static_cast<double>(stats.arrays_passed_through));
    result.set("arraysReencoded", static_cast<double>(stats.arrays_reencoded));
    result.set("readMs", stats.read_ms);
    result.set("composeMs", stats.compose_ms);
    result.set("writeMs", stats.write_ms);
    {
      emscripten::val assets = emscripten::val::array();
      for (const auto& path : stats.referenced_assets) {
        assets.call<void>("push", path);
      }
      result.set("assetPaths", assets);
      result.set("assetPathCount",
                 static_cast<double>(stats.referenced_assets.size()));
    }
    return result;
  }

  void end() { reset_(); }

 private:
  void reset_() {
    root_.clear();
    root_.shrink_to_fit();
    root_name_.clear();
    lazy_arrays_ = true;
    began_ = false;
    layers_.clear();
    parsed_layers_.clear();
    variant_overrides_.clear();
  }

  std::string root_;
  std::string root_name_;
  bool lazy_arrays_ = true;
  bool began_ = false;
  std::map<std::string, std::string> layers_;
  std::map<std::string, std::shared_ptr<tn::Layer>> parsed_layers_;
  std::map<std::string, std::string> variant_overrides_;
};

class NextUSDZConverterNative {
 public:
  NextUSDZConverterNative() = default;

  std::string error() const { return error_; }
  std::string warn() const { return warn_; }

  void clearURDFMeshBuffers() { urdf_mesh_buffers_.clear(); }

  bool setVisualMesh(const std::string& name, const emscripten::val& positions,
                     const emscripten::val& normals,
                     const emscripten::val& uvs,
                     const emscripten::val& indices) {
    return setURDFMeshBuffer(name, positions, normals, uvs, indices);
  }

  bool setCollisionMesh(const std::string& name,
                        const emscripten::val& positions,
                        const emscripten::val& normals,
                        const emscripten::val& uvs,
                        const emscripten::val& indices) {
    return setURDFMeshBuffer(name, positions, normals, uvs, indices);
  }

  bool createURDFPhysicsScene(const std::string& robot_json) {
    tn::Stage stage;
    std::string warn;
    std::string err;
    if (!tr::ConvertURDFJsonToUSDStage(robot_json, &urdf_mesh_buffers_,
                                       &stage, &warn, &err)) {
      warn_ = std::move(warn);
      error_ = err.empty() ? "URDF/MJCF conversion failed" : std::move(err);
      has_stage_ = false;
      return false;
    }
    stage_ = std::move(stage);
    warn_ = std::move(warn);
    error_.clear();
    has_stage_ = true;
    return true;
  }

  bool loadFromBinary(const emscripten::val& bytes,
                      const std::string& filename) {
    std::string copy_error;
    std::string input = CopyUint8ArrayToString(bytes, &copy_error);
    if (!copy_error.empty()) {
      error_ = copy_error;
      has_stage_ = false;
      return false;
    }
    tn::LoadUSDOptions options;
    options.usda_options.parse_options.enable_usda_lazy_arrays = true;
    tn::Stage stage;
    std::string warn;
    std::string err;
    if (!tn::LoadUSDFromMemoryOwned(std::move(input), &stage, options,
                                    &warn, &err)) {
      error_ = err.empty() ? "Failed to load " + filename : std::move(err);
      warn_ = std::move(warn);
      has_stage_ = false;
      return false;
    }
    stage_ = std::move(stage);
    warn_ = std::move(warn);
    error_.clear();
    has_stage_ = true;
    return true;
  }

  void setAsset(const std::string& name, const emscripten::val& bytes) {
    std::string copy_error;
    std::string data = CopyUint8ArrayToString(bytes, &copy_error);
    if (!copy_error.empty()) {
      error_ = copy_error;
      return;
    }
    assets_[name] = std::vector<uint8_t>(data.begin(), data.end());
    error_.clear();
  }

  void setUSDCExportLimitMB(int file_size_mb, int memory_mb) {
    (void)file_size_mb;
    (void)memory_mb;
  }

  std::string extractPhysicsSceneJSON() {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return std::string();
    }
    lightusd::minijson::Value root;
    root["name"] = stage_.GetMeta().defaultPrim;
    root["upAxis"] = stage_.GetMeta().upAxis;
    root["prims"] = lightusd::minijson::Value::array();
    for (const tn::UsdPrim& prim : stage_.GetRootPrims()) {
      AppendNextPhysicsPrimJSON(prim, &root["prims"]);
    }
    error_.clear();
    return root.dump();
  }

  std::string exportAsUSDA() {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return std::string();
    }
    std::string output = tn::WriteUSDAToString(stage_);
    if (output.empty()) error_ = "USDA export failed";
    else error_.clear();
    return output;
  }

  emscripten::val exportAsUSDC() {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return emscripten::val::null();
    }
    std::vector<uint8_t> output;
    tn::USDCWriteResult result = tn::WriteUSDCToMemory(output, stage_);
    if (!result.success) {
      error_ = result.error.empty() ? "USDC export failed" : result.error;
      return emscripten::val::null();
    }
    error_.clear();
    return Uint8ArrayFromVector(output);
  }

  emscripten::val exportAsUSDZ() {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return emscripten::val::null();
    }
    std::vector<uint8_t> usdc;
    tn::USDCWriteResult usdc_result = tn::WriteUSDCToMemory(usdc, stage_);
    if (!usdc_result.success) {
      error_ = usdc_result.error.empty() ? "USDC export failed"
                                         : usdc_result.error;
      return emscripten::val::null();
    }
    std::vector<uint8_t> output;
    tn::USDZWriteResult result = tn::WriteUSDZFromUSDCAndAssetsToMemory(
        output, usdc.data(), usdc.size(), assets_);
    if (!result.success) {
      error_ = result.error.empty() ? "USDZ export failed" : result.error;
      return emscripten::val::null();
    }
    error_.clear();
    return Uint8ArrayFromVector(output);
  }

  emscripten::val rewriteRoot(emscripten::val bytes, const std::string& filename,
                              emscripten::val options) {
    error_.clear();
    warn_.clear();

    std::string copy_error;
    std::string input = CopyUint8ArrayToString(bytes, &copy_error);
    if (!copy_error.empty()) return ErrorResult(copy_error);

    tn::Stage stage;
    tn::LoadUSDOptions load_opts;
    load_opts.usda_options.parse_options.enable_usda_lazy_arrays = true;
    if (!options.isNull() && !options.isUndefined()) {
      emscripten::val max_memory = options["maxMemory"];
      if (!max_memory.isUndefined() && !max_memory.isNull()) {
        load_opts.max_memory = max_memory.as<size_t>();
      }
      emscripten::val usda_lazy = options["usdaLazy"];
      if (!usda_lazy.isUndefined() && !usda_lazy.isNull()) {
        load_opts.usda_options.parse_options.enable_usda_lazy_arrays =
            usda_lazy.as<bool>();
      }
    }

    const bool ok = tn::LoadUSDFromMemoryOwned(
        std::move(input), &stage, load_opts, &warn_, &error_);
    if (!ok) {
      return ErrorResult(error_.empty() ? "next-core USD load failed" : error_);
    }

    std::string root_format = "usdc";
    if (!options.isNull() && !options.isUndefined()) {
      emscripten::val fmt = options["rootLayerFormat"];
      if (!fmt.isUndefined() && !fmt.isNull()) {
        root_format = fmt.as<std::string>();
      }
    }
    std::transform(root_format.begin(), root_format.end(), root_format.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    emscripten::val out = emscripten::val::object();
    out.set("success", true);
    out.set("sourcePath", filename);

    if (root_format == "usda") {
      std::string text = tn::WriteUSDAToString(stage);
      out.set("rootName", std::string("root.usda"));
      out.set("rootLayerFormat", std::string("usda"));
      out.set("data", Uint8ArrayFromString(text));
      out.set("size", static_cast<double>(text.size()));
      return out;
    }

    std::vector<uint8_t> usdc;
    tn::USDCWriteResult wr = tn::WriteUSDCToMemory(usdc, stage);
    if (!wr.success) {
      return ErrorResult(wr.error.empty() ? "next-core USDC write failed"
                                          : wr.error);
    }
    out.set("rootName", std::string("root.usdc"));
    out.set("rootLayerFormat", std::string("usdc"));
    out.set("data", Uint8ArrayFromVector(usdc));
    out.set("size", static_cast<double>(usdc.size()));
    out.set("tokenCount", static_cast<double>(wr.token_count));
    out.set("pathCount", static_cast<double>(wr.path_count));
    out.set("specCount", static_cast<double>(wr.spec_count));
    return out;
  }

 private:
  bool setURDFMeshBuffer(const std::string& name,
                         const emscripten::val& positions,
                         const emscripten::val& normals,
                         const emscripten::val& uvs,
                         const emscripten::val& indices) {
    if (name.empty()) {
      error_ = "setVisualMesh/setCollisionMesh requires a non-empty name";
      return false;
    }
    tr::URDFMeshBuffer buffer;
    CopyTypedArrayToVector(positions, buffer.positions);
    CopyTypedArrayToVector(normals, buffer.normals);
    CopyTypedArrayToVector(uvs, buffer.uvs);
    std::vector<uint32_t> unsigned_indices;
    CopyTypedArrayToVector(indices, unsigned_indices);
    buffer.indices.reserve(unsigned_indices.size());
    for (uint32_t index : unsigned_indices) {
      if (index > static_cast<uint32_t>(INT32_MAX)) {
        error_ = "Mesh index exceeds int32 range";
        return false;
      }
      buffer.indices.push_back(static_cast<int32_t>(index));
    }
    if (buffer.positions.size() < 9 || buffer.positions.size() % 3 != 0) {
      error_ = "Mesh positions must contain at least three xyz points";
      return false;
    }
    if (!buffer.normals.empty() &&
        buffer.normals.size() != buffer.positions.size()) {
      error_ = "Mesh normals length must match positions length";
      return false;
    }
    if (!buffer.uvs.empty() &&
        buffer.uvs.size() != (buffer.positions.size() / 3) * 2) {
      error_ = "Mesh UV length must equal vertex count * 2";
      return false;
    }
    if (!buffer.indices.empty() && buffer.indices.size() % 3 != 0) {
      error_ = "Mesh indices must contain triangles";
      return false;
    }
    urdf_mesh_buffers_[name] = std::move(buffer);
    error_.clear();
    return true;
  }

  emscripten::val ErrorResult(const std::string& error) {
    error_ = error;
    emscripten::val out = emscripten::val::object();
    out.set("success", false);
    out.set("error", error_);
    return out;
  }

  std::string error_;
  std::string warn_;
  tn::Stage stage_;
  bool has_stage_ = false;
  std::map<std::string, tr::URDFMeshBuffer> urdf_mesh_buffers_;
  std::map<std::string, std::vector<uint8_t>> assets_;
};



namespace {

int OptInt(const emscripten::val& opts, const char* key, int def) {
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return def;
  return v.as<int>();
}

double OptDouble(const emscripten::val& opts, const char* key, double def) {
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return def;
  return v.as<double>();
}

bool OptBool(const emscripten::val& opts, const char* key, bool def) {
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return def;
  return v.as<bool>();
}

std::string OptStr(const emscripten::val& opts, const char* key,
                   const char* def) {
  emscripten::val v = opts[key];
  if (v.isUndefined() || v.isNull()) return def;
  return v.as<std::string>();
}

}  // namespace

namespace {

tn::ValidationOptions ParseValidationOptionsJSONForWeb(
    const std::string& options_json) {
  tn::ValidationOptions opts;
  if (options_json.empty()) return opts;

  lightusd::minijson::Value args;
  lightusd::minijson::ParseOptions parse_options;
  // Match the previous options parser: the last duplicate member wins.
  parse_options.reject_duplicate_keys = false;
  if (!lightusd::minijson::Parse(options_json, &args, nullptr, parse_options) ||
      !args.is_object() || !args.contains("groups") ||
      !args["groups"].is_array()) {
    return opts;
  }

  opts.core = false;
  opts.geom = false;
  opts.shade = false;
  opts.lux = false;
  opts.physics = false;
  opts.crate = false;
  for (const auto& group : args["groups"]) {
    if (!group.is_string()) continue;
    const std::string name = group.get_string();
    if (name == "core") {
      opts.core = true;
    } else if (name == "geom") {
      opts.geom = true;
    } else if (name == "shade") {
      opts.shade = true;
    } else if (name == "lux") {
      opts.lux = true;
    } else if (name == "physics") {
      opts.physics = true;
    } else if (name == "render") {
      opts.render = true;
    } else if (name == "crate") {
      opts.crate = true;
    } else if (name == "all") {
      opts = tn::MakeValidateAllOptions();
    }
  }
  if (!opts.core && !opts.geom && !opts.shade && !opts.lux && !opts.physics &&
      !opts.render && !opts.crate) {
    opts.core = true;
  }
  return opts;
}

lightusd::minijson::Value ValidationResultToJSON(const tn::USDValidationResult& v) {
  lightusd::minijson::Value result;
  result["parse_ok"] = true;
  result["ok"] = v.ok();
  result["error_count"] = v.error_count();
  result["warning_count"] = v.warning_count();
  result["spec_version"] = tn::GetAOUSDCoreSpecVersionString();
  {
    lightusd::minijson::Value groups = lightusd::minijson::Value::array();
    for (const std::string& name :
         tn::GetValidationGroupNames(v.checked_groups)) {
      groups.push_back(name);
    }
    result["checked_groups"] = groups;
  }
  lightusd::minijson::Value issues = lightusd::minijson::Value::array();
  for (const tn::USDValidationIssue* issue :
       tn::GetOrderedValidationIssues(v)) {
    lightusd::minijson::Value item;
    item["severity"] =
        issue->severity == tn::USDValidationSeverity::Error ? "error"
                                                            : "warning";
    item["rule_id"] = issue->rule_id;
    item["location"] = issue->location;
    item["message"] = issue->message;
    issues.push_back(item);
  }
  result["issues"] = issues;
  return result;
}

}  // namespace

// validateFromBinary(bytes, filename, optionsJson) -> JSON string, matching
// the legacy LightUSDLoaderNative.validateFromBinary contract consumed by
// web/js/validation.js. Runs AOUSD-core validation over next::Layer.
static std::string validateFromBinary(const emscripten::val& data,
                                      const std::string& filename,
                                      const std::string& options_json) {
  std::string copy_error;
  std::string bytes = CopyUint8ArrayToString(data, &copy_error);
  const tn::ValidationOptions options =
      ParseValidationOptionsJSONForWeb(options_json);

  lightusd::minijson::Value result;
  tn::USDValidationResult validation;
  std::string warn, err;
  const bool loaded = tn::ValidateUSDFromMemoryAgainstAOUSDCore(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), filename,
      options, &validation, &warn, &err);
  if (!loaded) {
    result["parse_ok"] = false;
    result["ok"] = false;
    result["error"] = err.empty() ? copy_error : err;
    if (!warn.empty()) result["warn"] = warn;
    return result.dump();
  }

  result = ValidationResultToJSON(validation);
  if (!warn.empty()) result["warn"] = warn;
  return result.dump();
}

// usddiff(opts) -> { success, hasDiffs, text?, json?, error?, warn? }
// opts: { left:{data:Uint8Array, name?}, right:{data:Uint8Array, name?},
//         format?:"text"|"json"|"both", ulps?, eps?, compareMetadata?,
//         fuzzyAssetPaths? }
// Pre-composition layer diff over next::Layer, mirroring the legacy module's
// usddiff / native lusddiff contract.
static emscripten::val usddiff(const emscripten::val& opts) {
  emscripten::val result = emscripten::val::object();

  if (opts.isUndefined() || opts.isNull()) {
    result.set("success", false);
    result.set("error", std::string("usddiff: missing options"));
    return result;
  }
  emscripten::val left = opts["left"];
  emscripten::val right = opts["right"];
  if (left.isUndefined() || left.isNull() || right.isUndefined() ||
      right.isNull()) {
    result.set("success", false);
    result.set("error",
               std::string("usddiff: 'left' and 'right' are required"));
    return result;
  }

  std::string copy_error;
  std::string lhsBuf = CopyUint8ArrayToString(left["data"], &copy_error);
  std::string rhsBuf = CopyUint8ArrayToString(right["data"], &copy_error);
  const std::string lhsName = OptStr(left, "name", "left");
  const std::string rhsName = OptStr(right, "name", "right");
  const std::string format = OptStr(opts, "format", "text");

  tn::DiffOptions diffOpts;
  {
    const int ulps = OptInt(opts, "ulps", -1);
    if (ulps >= 0) {
      diffOpts.floatUlps = static_cast<uint32_t>(ulps);
      diffOpts.doubleUlps = static_cast<uint64_t>(ulps);
    }
    diffOpts.absEps = OptDouble(opts, "eps", diffOpts.absEps);
    diffOpts.compareMetadata =
        OptBool(opts, "compareMetadata", diffOpts.compareMetadata);
    diffOpts.fuzzyAssetPaths =
        OptBool(opts, "fuzzyAssetPaths", diffOpts.fuzzyAssetPaths);
  }

  std::string warn, err;
  std::shared_ptr<tn::Layer> lhs = tn::pcp::LoadLayerFromMemory(
      lhsName, reinterpret_cast<const uint8_t*>(lhsBuf.data()), lhsBuf.size(),
      &warn, &err);
  if (!lhs) {
    result.set("success", false);
    result.set("error", "Error loading " + lhsName + ": " + err);
    return result;
  }
  err.clear();
  std::shared_ptr<tn::Layer> rhs = tn::pcp::LoadLayerFromMemory(
      rhsName, reinterpret_cast<const uint8_t*>(rhsBuf.data()), rhsBuf.size(),
      &warn, &err);
  if (!rhs) {
    result.set("success", false);
    result.set("error", "Error loading " + rhsName + ": " + err);
    return result;
  }

  std::unordered_map<std::string, tn::PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, tn::PropDiff> propDiffs;
  tn::LayerMetaDiff layerMetaDiff;
  tn::Diff(*lhs, *rhs, psDiffs, propDiffs, diffOpts, &layerMetaDiff);
  const bool hasDiffs =
      !psDiffs.empty() || !propDiffs.empty() || layerMetaDiff.changed();

  result.set("success", true);
  result.set("hasDiffs", hasDiffs);
  if (!warn.empty()) result.set("warn", warn);
  if (format == "text" || format == "both") {
    result.set("text", hasDiffs
                           ? tn::DiffToText(*lhs, *rhs, lhsName, rhsName,
                                            diffOpts)
                           : std::string("No differences found.\n"));
  }
  if (format == "json" || format == "both") {
    result.set("json", tn::DiffToJSON(*lhs, *rhs, lhsName, rhsName, diffOpts));
  }
  return result;
}

// Direct C dispatch: one compiled call boundary, no class_/function binding
// templates or per-signature invoker registration.
namespace lightusd {
namespace web_next {

EM_JS(uint32_t, NextArgumentHandle, (uint32_t args, uint32_t index), {
  return Emval.toHandle(Emval.toValue(args)[index]);
});

static emscripten::val NextArgument(uint32_t args, uint32_t index) {
  return emscripten::val::take_ownership(
      reinterpret_cast<emscripten::EM_VAL>(static_cast<uintptr_t>(NextArgumentHandle(args, index))));
}
static uint32_t NextReturn(emscripten::val value) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value.release_ownership()));
}

void* NextCreateObject(uint32_t kind) {
  switch (kind) {
    case 1: return new (std::nothrow) NextUSDZConverterNative;
    case 2: return new (std::nothrow) SubdivStreamer;
    case 3: return new (std::nothrow) NextFlattenSession;
    case 4: return new (std::nothrow) RenderStream;
    default: return nullptr;
  }
}
void NextDestroyObject(uint32_t kind, void* object) {
  switch (kind) {
    case 1: delete static_cast<NextUSDZConverterNative*>(object); return;
    case 2: delete static_cast<SubdivStreamer*>(object); return;
    case 3: delete static_cast<NextFlattenSession*>(object); return;
    case 4: delete static_cast<RenderStream*>(object); return;
    default: return;
  }
}
uint32_t NextInvokeObject(uint32_t kind, void* object, uint32_t method, uint32_t args) {
  switch (kind) {
    case 0:
      switch (method) {
        case 1: return NextReturn(usddiff(NextArgument(args, 0)));
        case 2: return NextReturn(emscripten::val(validateFromBinary(
            NextArgument(args, 0), NextArgument(args, 1).as<std::string>(),
            NextArgument(args, 2).as<std::string>())));
        default: return 0;
      }
    case 1:
      switch (method) {
        case 3: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->rewriteRoot(NextArgument(args, 0), NextArgument(args, 1).as<std::string>(), NextArgument(args, 2))));
        case 4: static_cast<NextUSDZConverterNative*>(object)->clearURDFMeshBuffers(); return NextReturn(emscripten::val::undefined());
        case 5: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->setVisualMesh(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1), NextArgument(args, 2), NextArgument(args, 3), NextArgument(args, 4))));
        case 6: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->setCollisionMesh(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1), NextArgument(args, 2), NextArgument(args, 3), NextArgument(args, 4))));
        case 7: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->createURDFPhysicsScene(NextArgument(args, 0).as<std::string>())));
        case 8: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->loadFromBinary(NextArgument(args, 0), NextArgument(args, 1).as<std::string>())));
        case 9: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->extractPhysicsSceneJSON()));
        case 10: static_cast<NextUSDZConverterNative*>(object)->setAsset(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1)); return NextReturn(emscripten::val::undefined());
        case 11: static_cast<NextUSDZConverterNative*>(object)->setUSDCExportLimitMB(NextArgument(args, 0).as<int>(), NextArgument(args, 1).as<int>()); return NextReturn(emscripten::val::undefined());
        case 12: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->exportAsUSDA()));
        case 13: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->exportAsUSDC()));
        case 14: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->exportAsUSDZ()));
        case 15: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->error()));
        case 16: return NextReturn(emscripten::val(static_cast<NextUSDZConverterNative*>(object)->warn()));
        default: return 0;
      }
    case 2:
      switch (method) {
        case 17: return NextReturn(emscripten::val(static_cast<SubdivStreamer*>(object)->refineStream(NextArgument(args, 0), NextArgument(args, 1), NextArgument(args, 2), NextArgument(args, 3), NextArgument(args, 4), NextArgument(args, 5).as<int>(), NextArgument(args, 6).as<int>(), NextArgument(args, 7).as<int>(), NextArgument(args, 8).as<int>(), NextArgument(args, 9).as<int>(), NextArgument(args, 10).as<int>(), NextArgument(args, 11).as<int>(), NextArgument(args, 12).as<bool>(), NextArgument(args, 13))));
        case 18: return NextReturn(emscripten::val(static_cast<SubdivStreamer*>(object)->heapBytes()));
        default: return 0;
      }
    case 3:
      switch (method) {
        case 19: return NextReturn(emscripten::val(static_cast<NextFlattenSession*>(object)->begin(NextArgument(args, 0), NextArgument(args, 1).as<std::string>(), NextArgument(args, 2).as<bool>())));
        case 20: return NextReturn(emscripten::val(static_cast<NextFlattenSession*>(object)->setVariantOverride(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1).as<std::string>())));
        case 21: return NextReturn(emscripten::val(static_cast<NextFlattenSession*>(object)->provideLayer(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1))));
        case 22: return NextReturn(emscripten::val(static_cast<NextFlattenSession*>(object)->step(NextArgument(args, 0))));
        case 23: static_cast<NextFlattenSession*>(object)->end(); return NextReturn(emscripten::val::undefined());
        default: return 0;
      }
    case 4:
      switch (method) {
        case 24: static_cast<RenderStream*>(object)->setMaterialDedup(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 25: static_cast<RenderStream*>(object)->setMeshMerge(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 26: static_cast<RenderStream*>(object)->setMeshMergeBakeTransform(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 27: static_cast<RenderStream*>(object)->setFlattenRenderTree(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 28: static_cast<RenderStream*>(object)->setMeshOnly(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 29: static_cast<RenderStream*>(object)->setComputeTangents(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 30: static_cast<RenderStream*>(object)->setRenderSettingsPath(NextArgument(args, 0).as<std::string>()); return NextReturn(emscripten::val::undefined());
        case 31: static_cast<RenderStream*>(object)->setBuildVertexIndices(NextArgument(args, 0).as<bool>()); return NextReturn(emscripten::val::undefined());
        case 32: static_cast<RenderStream*>(object)->setTangentMethod(NextArgument(args, 0).as<std::string>()); return NextReturn(emscripten::val::undefined());
        case 33: static_cast<RenderStream*>(object)->provideAsset(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1)); return NextReturn(emscripten::val::undefined());
        case 34: static_cast<RenderStream*>(object)->clearAssets(); return NextReturn(emscripten::val::undefined());
        case 35: static_cast<RenderStream*>(object)->setVariantOverride(NextArgument(args, 0).as<std::string>(), NextArgument(args, 1).as<std::string>()); return NextReturn(emscripten::val::undefined());
        case 36: static_cast<RenderStream*>(object)->clearVariantOverrides(); return NextReturn(emscripten::val::undefined());
        case 37: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->listVariants()));
        case 38: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->begin(NextArgument(args, 0))));
        case 39: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->beginOwned(NextArgument(args, 0).as<std::string>())));
        case 40: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->meshCount()));
        case 41: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->nodeCount()));
        case 42: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->lightCount()));
        case 43: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->pointsCount()));
        case 44: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->curvesCount()));
        case 45: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->cameraCount()));
        case 46: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->pointInstancerCount()));
        case 47: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->pointInstanceDrawCount()));
        case 48: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->skeletonCount()));
        case 49: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->unsupportedRenderableCount()));
        case 50: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->animationCount()));
        case 51: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getAnimation(NextArgument(args, 0).as<int32_t>())));
        case 52: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getAnimationView(NextArgument(args, 0).as<int32_t>())));
        case 53: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getAllAnimations()));
        case 54: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getAnimationInfo(NextArgument(args, 0).as<int32_t>())));
        case 55: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getAllAnimationInfos()));
        case 56: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getNode(NextArgument(args, 0).as<int32_t>())));
        case 57: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getLight(NextArgument(args, 0).as<int32_t>())));
        case 58: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getPoints(NextArgument(args, 0).as<int32_t>())));
        case 59: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getCurves(NextArgument(args, 0).as<int32_t>())));
        case 60: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getCamera(NextArgument(args, 0).as<int32_t>())));
        case 61: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getPointInstancer(NextArgument(args, 0).as<int32_t>())));
        case 62: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getPointInstanceDraw(NextArgument(args, 0).as<int32_t>())));
        case 63: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getSkeleton(NextArgument(args, 0).as<int32_t>())));
        case 64: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getUnsupportedRenderables()));
        case 65: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getSceneMetadata()));
        case 66: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getStats()));
        case 67: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->getMesh(NextArgument(args, 0).as<int>())));
        case 68: return NextReturn(emscripten::val(static_cast<RenderStream*>(object)->error()));
        case 69: static_cast<RenderStream*>(object)->end(); return NextReturn(emscripten::val::undefined());
        default: return 0;
      }
    default: return 0;
  }
}
}  // namespace web_next
}  // namespace lightusd
