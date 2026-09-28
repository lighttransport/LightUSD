// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Lean Emscripten binding for the next-core + tydra-next render path.
// This intentionally avoids the legacy lightusd_static library and exposes the
// RenderStream contract consumed by web/js/src/lightusd/LightUSDLoader.js for
// first-stage `backend=next` browser coverage.

#include <new>
#include "binding-next-api.h"
#include "binding-next-assets.hh"
#include "binding-next-layer.hh"
#include "binding-next-render.hh"
#include "binding-next-common.hh"
#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iterator>
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
#include "next/crate/crate-reader.hh"
#include "tsd/tinysubdiv.hh"

#include "minijson.hh"
#include "binding-next-scene.hh"
#include "next/load-usd.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-xform.hh"
#include "next/schema/color-space.hh"
#include "next/schema/usd-shade.hh"
#include "next/stage/stage.hh"
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
// SubdivStreamer refines a control mesh and delivers the surface through a
// synchronous callback in bounded batches. Input and output arrays cross the
// browser boundary as counted buffers, without emval handles.
// ===========================================================================
EM_JS(int, NextSubdivEmit,
      (uint32_t callback_id, const float* positions, uint32_t position_count,
       const float* normals, uint32_t normal_count,
       const uint32_t* indices, uint32_t index_count,
       const uint32_t* face_source, uint32_t face_count,
       const float* uvs, uint32_t uv_count,
       uint32_t num_vertices, uint32_t num_faces, uint32_t batch_index), {
  return Module['__lightusdNextSubdivEmit'](
      callback_id, Number(positions), position_count, Number(normals),
      normal_count, Number(indices), index_count, Number(face_source),
      face_count, Number(uvs), uv_count, num_vertices, num_faces, batch_index);
});
class SubdivStreamer {
 public:
  int refineStream(const float* points, uint32_t point_values,
                   const uint32_t* fvc, uint32_t face_count,
                   const uint32_t* fvi, uint32_t index_count,
                   const float* uv_values, uint32_t uv_value_count,
                   const uint32_t* uv_indices, uint32_t uv_index_count,
                   const lightusd_next_subdiv_options& options,
                   uint32_t callback_id) {
    namespace tsd = lightusd::tsd;
    error_.clear();
    if ((point_values % 3) != 0) {
      error_ = "points length must be a multiple of 3";
      return 1;
    }
    if (!face_count || !index_count) {
      error_ = "empty mesh";
      return 1;
    }
    const bool has_uv = uv_value_count >= 2 && (uv_value_count % 2) == 0;

    tsd::MeshView mesh;
    mesh.points = points;
    mesh.num_points = point_values / 3;
    mesh.face_vertex_counts = fvc;
    mesh.num_faces = face_count;
    mesh.face_vertex_indices = fvi;
    mesh.num_face_vertex_indices = index_count;

    tsd::FVarChannelView uvchan;
    if (has_uv) {
      uvchan.values = uv_values;
      uvchan.num_values = uv_value_count / 2;
      uvchan.indices = uv_index_count ? uv_indices : nullptr;
      uvchan.stride = 2;
      uvchan.interpolation = (options.uv_interpolation == 1)
                                 ? tsd::FVarLinearInterpolation::CornersPlus1
                                 : tsd::FVarLinearInterpolation::All;
    }

    tsd::Options opts;
    opts.scheme = (options.scheme == 1)   ? tsd::Scheme::Loop
                  : (options.scheme == 2) ? tsd::Scheme::Bilinear
                                  : tsd::Scheme::CatmullClark;
    opts.boundary = (options.boundary == 1)   ? tsd::BoundaryInterpolation::EdgeOnly
                    : (options.boundary == 2) ? tsd::BoundaryInterpolation::None
                                      : tsd::BoundaryInterpolation::EdgeAndCorner;
    opts.level = options.level;
    opts.remove_holes = true;

    tsd::StreamOptions so;
    so.batch_faces = (options.batch_faces > 0) ? uint32_t(options.batch_faces) : 4096u;
    so.emit_triangles = true;
    so.want_normals = options.want_normals != 0;
    so.dedup_within_batch = true;
    so.block_faces = (options.block_faces > 0) ? uint32_t(options.block_faces) : 0u;
    so.halo_rings = (options.halo_rings > 0) ? uint32_t(options.halo_rings) : 0u;

    struct SinkCtx { uint32_t callback_id; bool want_normals; };
    SinkCtx ctx{callback_id, so.want_normals};

    auto sink = [](void *user, const tsd::StreamBatch *b) -> bool {
      SinkCtx *c = static_cast<SinkCtx *>(user);
      const bool have_normals = c->want_normals && b->normals;
      const bool have_uv = b->num_fvar == 1 && b->fvar[0].values;
      return NextSubdivEmit(
          c->callback_id, b->positions, b->num_vertices * 3,
          have_normals ? b->normals : nullptr,
          have_normals ? b->num_vertices * 3 : 0,
          b->indices, b->num_indices, b->face_source, b->num_faces,
          have_uv ? b->fvar[0].values : nullptr,
          have_uv ? b->num_indices * 2 : 0,
          b->num_vertices, b->num_faces, b->batch_index) != 0;
    };

    const tsd::Result r = tsd::RefineStream(
        mesh, has_uv ? &uvchan : nullptr, has_uv ? 1u : 0u, nullptr, 0, opts, so,
        sink, &ctx, &error_);
    if (r != tsd::Result::Success) {
      error_ = std::string("RefineStream failed (") + tsd::to_string(r) +
               "): " + error_;
      return 1;
    }
    return 0;
  }
  const std::string& error() const { return error_; }
 private:
  std::string error_;
};

// Resumable multi-layer flatten session: JS drives a need-layer loop,
// providing dependency layer bytes (USDA / USDC / USDZ, fetched over HTTP or
// pulled from a package) until the flatten converges, then receives a
// flattened USDC buffer. One instance = one session; mirrors the legacy
// module's nextFlattenAsyncBegin/ProvideLayer/Step/End protocol with the
// session id replaced by the instance.
EM_JS(int, NextFlattenEmitChunk,
      (uint32_t callback_id, const uint8_t* data, uint32_t size), {
  return Module['__lightusdNextFlattenEmit'](
      callback_id, Number(data), size);
});
class NextFlattenSession {
 public:
  NextFlattenSession() = default;

  bool beginBytes(const uint8_t* root_bytes, uint32_t root_size,
                  const uint8_t* root_name, uint32_t root_name_size,
                  bool lazyArrays) {
    // Variant overrides are session configuration: the documented workflow
    // sets them before begin(), so keep them across the reset (end() clears).
    std::map<std::string, std::string> variant_overrides =
        std::move(variant_overrides_);
    reset_();
    variant_overrides_ = std::move(variant_overrides);
    if (root_size > (uint32_t{1} << 30) ||
        (root_size && !root_bytes) || (root_name_size && !root_name)) {
      error_ = "Invalid root layer buffer";
      return false;
    }
    if (root_size > max_input_bytes_) {
      error_ = "Root exceeds configured aggregate input byte limit";
      return false;
    }
    if (root_size) root_.assign(reinterpret_cast<const char*>(root_bytes), root_size);
    if (root_.empty()) {
      error_ = "empty root layer buffer";
      return false;
    }
    if (root_name_size) {
      root_name_.assign(reinterpret_cast<const char*>(root_name), root_name_size);
    }
    lazy_arrays_ = lazyArrays;
    began_ = true;
    return true;
  }

  // Key is a variant-set name ("shape", applies stage-wide) or the
  // prim-scoped form "<primPath>{<set>}" (wins over the bare-set key).
  void setVariantOverride(const std::string& set_or_scoped_key,
                          const std::string& selection) {
    variant_overrides_[set_or_scoped_key] = selection;
  }

  int setAssetPathRemapJSON(const uint8_t* data, uint32_t size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return 0;
    }
    if ((!data && size) || size > (uint32_t{1} << 30)) {
      error_ = "Invalid asset path remap input";
      return 0;
    }
    lightusd::minijson::Value parsed;
    lightusd::minijson::ParseOptions options;
    options.reject_duplicate_keys = false;
    if (!lightusd::minijson::Parse(
            size ? std::string(reinterpret_cast<const char*>(data), size)
                 : std::string("{}"),
            &parsed, nullptr, options) || !parsed.is_object()) {
      error_ = "Asset path remap must be a JSON object of strings";
      return 0;
    }
    std::map<std::string, std::string> candidate;
    size_t candidate_bytes = 0;
    const auto* members = parsed.object_items();
    if (!members) {
      error_ = "Asset path remap must be a JSON object";
      return 0;
    }
    for (const auto& member : *members) {
      const std::string* replacement = member.value().string_ptr();
      if (member.key.empty() || !replacement ||
          replacement->empty() ||
          member.key.find('\0') != std::string::npos ||
          replacement->find('\0') != std::string::npos ||
          member.key.size() > (1u << 20) || replacement->size() > (1u << 20)) {
        error_ = "Asset remap keys must be non-empty and paths must be bounded";
        return 0;
      }
      const size_t pair_bytes = member.key.size() + replacement->size();
      auto existing = candidate.find(member.key);
      if (existing != candidate.end()) {
        candidate_bytes -= existing->first.size() + existing->second.size();
      }
      if (pair_bytes > max_input_bytes_ ||
          candidate_bytes > max_input_bytes_ - pair_bytes) {
        error_ = "Asset path remap exceeds aggregate input byte limit";
        return 0;
      }
      candidate_bytes += pair_bytes;
      candidate[member.key] = *replacement;
    }
    const size_t retained = root_.size() + provided_layer_bytes_;
    if (retained > max_input_bytes_ ||
        candidate_bytes > max_input_bytes_ - retained) {
      error_ = "Asset path remap exceeds aggregate input byte limit";
      return 0;
    }
    asset_path_remap_.swap(candidate);
    asset_path_remap_bytes_ = candidate_bytes;
    error_.clear();
    return 1;
  }

  int remapLayerAssetPathsJSON(const uint8_t* data, uint32_t size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return -1;
    }
    auto retained_remap = std::move(asset_path_remap_);
    const size_t retained_remap_bytes = asset_path_remap_bytes_;
    asset_path_remap_bytes_ = 0;
    const int parsed = setAssetPathRemapJSON(data, size);
    if (parsed != 1) {
      asset_path_remap_ = std::move(retained_remap);
      asset_path_remap_bytes_ = retained_remap_bytes;
      return -1;
    }
    const size_t input_without_root = provided_layer_bytes_ +
        retained_remap_bytes + asset_path_remap_bytes_;
    if (root_.size() > max_input_bytes_ ||
        input_without_root > max_input_bytes_ - root_.size()) {
      asset_path_remap_ = std::move(retained_remap);
      asset_path_remap_bytes_ = retained_remap_bytes;
      error_ = "Asset path remap exceeds aggregate input byte limit";
      return -1;
    }

    tn::CrateReadOptions read_options;
    std::string parse_error;
    std::unique_ptr<tn::Layer> layer = ParseNextLayerBytes(
        reinterpret_cast<const uint8_t*>(root_.data()), root_.size(), root_name_,
        read_options, &parse_error);
    if (!layer) {
      asset_path_remap_ = std::move(retained_remap);
      asset_path_remap_bytes_ = retained_remap_bytes;
      error_ = parse_error.empty() ? "Failed to parse root layer" : parse_error;
      return -1;
    }
    size_t remapped = 0;
    for (uint32_t i = 0; i < layer->prim_count(); ++i) {
      tn::PrimSpec* prim = layer->prim_mutable(i);
      if (prim) remapped += prim->remap_asset_paths(asset_path_remap_);
    }
    if (remapped > static_cast<size_t>((std::numeric_limits<int>::max)())) {
      asset_path_remap_ = std::move(retained_remap);
      asset_path_remap_bytes_ = retained_remap_bytes;
      error_ = "Asset path remap count exceeds int range";
      return -1;
    }
    if (remapped == 0) {
      asset_path_remap_ = std::move(retained_remap);
      asset_path_remap_bytes_ = retained_remap_bytes;
      error_.clear();
      return 0;
    }
    std::string replacement = tn::WriteLayerToString(*layer);
    asset_path_remap_ = std::move(retained_remap);
    asset_path_remap_bytes_ = retained_remap_bytes;
    if (replacement.size() > max_input_bytes_ ||
        provided_layer_bytes_ + asset_path_remap_bytes_ >
            max_input_bytes_ - replacement.size()) {
      error_ = "Remapped root exceeds configured aggregate input byte limit";
      return -1;
    }
    root_.swap(replacement);
    error_.clear();
    return static_cast<int>(remapped);
  }

  bool addSublayerBytes(const uint8_t* path_bytes, uint32_t path_size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return false;
    }
    if (!path_bytes || path_size == 0 || path_size > (1u << 20) ||
        std::memchr(path_bytes, 0, path_size)) {
      error_ = "Invalid sublayer path";
      return false;
    }
    const std::string path(reinterpret_cast<const char*>(path_bytes), path_size);
    tn::CrateReadOptions read_options;
    std::string parse_error;
    std::unique_ptr<tn::Layer> layer = ParseNextLayerBytes(
        reinterpret_cast<const uint8_t*>(root_.data()), root_.size(), root_name_,
        read_options, &parse_error);
    if (!layer) {
      error_ = parse_error.empty() ? "Failed to parse root layer" : parse_error;
      return false;
    }
    layer->meta().subLayers.push_back(path);
    layer->meta().subLayers_set = true;
    std::string replacement = tn::WriteLayerToString(*layer);
    const size_t retained_limit =
        max_input_bytes_ - provided_layer_bytes_ - asset_path_remap_bytes_;
    if (replacement.size() > retained_limit) {
      error_ = "Authored root exceeds configured aggregate input byte limit";
      return false;
    }
    root_.swap(replacement);
    error_.clear();
    return true;
  }

  bool addPrimArcBytes(uint8_t kind, uint8_t list_op,
                       const uint8_t* prim_bytes,
                       uint32_t prim_size, const uint8_t* asset_bytes,
                       uint32_t asset_size, const uint8_t* target_bytes,
                       uint32_t target_size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return false;
    }
    if (kind > 3 || list_op > 5 || !prim_bytes || prim_size == 0 || !target_bytes ||
        target_size == 0 || (asset_size && !asset_bytes) ||
        prim_size > (1u << 20) || asset_size > (1u << 20) ||
        target_size > (1u << 20) ||
        std::memchr(prim_bytes, 0, prim_size) ||
        (asset_size && std::memchr(asset_bytes, 0, asset_size)) ||
        std::memchr(target_bytes, 0, target_size)) {
      error_ = "Invalid prim composition arc";
      return false;
    }
    if ((kind <= 1 && asset_size &&
         std::memchr(asset_bytes, '@', asset_size)) ||
        (kind >= 2 && asset_size != 0)) {
      error_ = "Invalid asset path for prim composition arc";
      return false;
    }
    const std::string prim_path(reinterpret_cast<const char*>(prim_bytes), prim_size);
    const std::string asset = asset_size
        ? std::string(reinterpret_cast<const char*>(asset_bytes), asset_size)
        : std::string();
    const std::string target(reinterpret_cast<const char*>(target_bytes), target_size);
    const tn::Path site = tn::Path::Parse(prim_path);
    const tn::Path target_path = tn::Path::Parse(target);
    if (!site.is_absolute() || site.is_root() || site.has_property() ||
        !target_path.is_absolute() || target_path.is_root() ||
        target_path.has_property()) {
      error_ = "Prim composition arc paths must be absolute prim paths";
      return false;
    }
    tn::CrateReadOptions read_options;
    std::string parse_error;
    std::unique_ptr<tn::Layer> layer = ParseNextLayerBytes(
        reinterpret_cast<const uint8_t*>(root_.data()), root_.size(), root_name_,
        read_options, &parse_error);
    if (!layer) {
      error_ = parse_error.empty() ? "Failed to parse root layer" : parse_error;
      return false;
    }
    tn::PrimSpec* prim = layer->prim_at_path_mutable(prim_path);
    if (!prim) {
      error_ = "Prim composition arc site does not exist: " + prim_path;
      return false;
    }
    std::string encoded;
    if (!asset.empty()) encoded = "@" + asset + "@";
    encoded += "<" + target + ">";
    tn::PrimSpecMeta& meta = prim->meta();
    std::vector<std::string>* arcs = nullptr;
    tn::ArcEdit* edit = nullptr;
    tn::ArcListOpEdits& all_edits = meta.ensure_arc_edits();
    if (kind == 0) { arcs = &meta.references; edit = &all_edits.references; }
    else if (kind == 1) { arcs = &meta.payloads; edit = &all_edits.payloads; }
    else if (kind == 2) { arcs = &meta.inherits; edit = &all_edits.inherits; }
    else { arcs = &meta.specializes; edit = &all_edits.specializes; }
    if (list_op != 0 && edit->authored && edit->is_explicit) {
      error_ = "Cannot mix explicit and qualified list-op edits for one arc field";
      return false;
    }
    if (list_op == 0) {
      *arcs = {encoded};
      *edit = tn::ArcEdit();
      edit->authored = true;
    } else {
      edit->authored = true;
      edit->is_explicit = false;
      std::vector<std::string>* authored = nullptr;
      switch (list_op) {
        case 1: authored = &edit->added; break;
        case 2: authored = &edit->prepended; break;
        case 3: authored = &edit->appended; break;
        case 4: authored = &edit->deleted; break;
        default: authored = &edit->ordered; break;
      }
      authored->push_back(encoded);
      if (list_op == 1 || list_op == 3) {
        arcs->push_back(encoded);
      } else if (list_op == 2) {
        arcs->insert(arcs->begin(), encoded);
      } else if (list_op == 4) {
        arcs->erase(std::remove(arcs->begin(), arcs->end(), encoded), arcs->end());
      } else {
        tn::ApplyStringListOrder(edit->ordered, arcs);
      }
    }
    std::string replacement = tn::WriteLayerToString(*layer);
    const size_t retained_limit =
        max_input_bytes_ - provided_layer_bytes_ - asset_path_remap_bytes_;
    if (replacement.size() > retained_limit) {
      error_ = "Authored root exceeds configured aggregate input byte limit";
      return false;
    }
    root_.swap(replacement);
    error_.clear();
    return true;
  }

  bool provideLayerBytes(const uint8_t* key_bytes, uint32_t key_size,
                         const uint8_t* data, uint32_t data_size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return false;
    }
    if ((key_size && !key_bytes) || (data_size && !data) ||
        data_size > (uint32_t{1} << 30)) {
      error_ = "Invalid layer buffer";
      return false;
    }
    std::string key;
    if (key_size) key.assign(reinterpret_cast<const char*>(key_bytes), key_size);
    if (!data_size) {
      error_ = "Invalid or empty layer data for: " + key;
      return false;
    }
    if (preflightLayerBytes(key_bytes, key_size, data_size) != 0) return false;
    const std::string norm_key = tn::AssetResolver::NormalizePath(key);
    std::string normalized = norm_key;
    while (normalized.rfind("./", 0) == 0) normalized = normalized.substr(2);
    const auto old = layers_.find(normalized);
    const size_t old_size = old == layers_.end() ? 0 : old->second.size();
    const size_t retained = root_.size() + provided_layer_bytes_ +
                            asset_path_remap_bytes_ - old_size;
    std::string bytes(reinterpret_cast<const char*>(data), data_size);
    provided_layer_bytes_ = provided_layer_bytes_ - old_size + data_size;
    layers_[normalized] = std::move(bytes);
    parsed_layers_.erase(normalized);
    return true;
  }

  int setMaxInputBytes(uint32_t value) {
    if (value == 0 || value > (uint32_t{1} << 30) ||
        value < root_.size() ||
        value - root_.size() < provided_layer_bytes_ + asset_path_remap_bytes_) {
      error_ = "Input byte limit is below retained root and layer bytes";
      return -1;
    }
    max_input_bytes_ = value;
    error_.clear();
    return 0;
  }
  uint32_t maxInputBytes() const { return max_input_bytes_; }
  int setMaxOutputBytes(uint32_t value) {
    if (value == 0 || value > (uint32_t{1} << 30)) {
      error_ = "Output byte limit must be between 1 and 1 GiB";
      return -1;
    }
    max_output_bytes_ = value;
    error_.clear();
    return 0;
  }
  uint32_t maxOutputBytes() const { return max_output_bytes_; }
  uint32_t inputBytes() const {
    return static_cast<uint32_t>(root_.size() + provided_layer_bytes_ +
                                 asset_path_remap_bytes_);
  }
  int preflightLayerBytes(const uint8_t* key_bytes, uint32_t key_size,
                          uint32_t data_size) {
    if (!began_) {
      error_ = "session not started (call begin first)";
      return -1;
    }
    if ((key_size && !key_bytes) || data_size == 0) {
      error_ = "Invalid layer input preflight";
      return -1;
    }
    const std::string key = key_size
        ? std::string(reinterpret_cast<const char*>(key_bytes), key_size)
        : std::string();
    std::string normalized = tn::AssetResolver::NormalizePath(key);
    while (normalized.rfind("./", 0) == 0) normalized = normalized.substr(2);
    const auto old = layers_.find(normalized);
    const size_t old_size = old == layers_.end() ? 0 : old->second.size();
    const size_t retained = root_.size() + provided_layer_bytes_ +
                            asset_path_remap_bytes_ - old_size;
    if (retained > max_input_bytes_ || data_size > max_input_bytes_ - retained) {
      error_ = "Layers exceed configured aggregate input byte limit";
      return -1;
    }
    error_.clear();
    return 0;
  }

  const std::string& error() const { return error_; }

  int step(uint32_t callback_id, lightusd_next_flatten_step_info* result) {
    releaseStep();
    error_.clear();
    *result = lightusd_next_flatten_step_info{};
    result->struct_size = sizeof(lightusd_next_flatten_step_info);
    if (!began_) {
      error_ = "session not started (call begin first)";
      result->status = -1;
      return 0;
    }

    tn::pipeline::FlattenOptions opts;
    opts.read.lazy_arrays = lazy_arrays_;
    opts.root_anchor_path = root_name_;
    opts.fail_on_composition_error = true;
    opts.composition.variant_overrides = variant_overrides_;
    opts.asset_path_remap = asset_path_remap_;
    opts.write.max_file_size_bytes = max_output_bytes_;

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

    const bool buffered = callback_id == 0;
    tn::pipeline::FlattenStats stats;
    std::string err;
    bool ok = false;
    std::vector<uint8_t> output_bytes;
    bool aborted = false;
    const uint8_t* root_data = reinterpret_cast<const uint8_t*>(root_.data());
    const size_t root_size = root_.size();
    if (buffered) {
      ok = tn::pipeline::FlattenUSDMemoryToUSDC(root_name_, root_data,
                                                root_size, output_bytes, opts, &stats,
                                                &err);
    } else {
      opts.write.streaming = true;
      tn::CrateWriteSink sink = [&](const uint8_t* data, size_t size) -> bool {
        if (size > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
            !NextFlattenEmitChunk(callback_id, data, static_cast<uint32_t>(size))) {
          aborted = true;
          return false;
        }
        return true;
      };
      ok = tn::pipeline::FlattenUSDMemoryToUSDCToSink(
          root_name_, root_data, root_size, sink, opts, &stats, &err);
    }
    last_step_layer_dependencies_ = std::move(consumed);

    if (!missing_key.empty()) {
      last_step_key_ = std::move(missing_key);
      result->status = 2;
      return 0;
    }
    if (!ok) {
      last_step_composition_errors_ = std::move(stats.composition_errors);
      if (last_step_composition_errors_.size() >
          static_cast<size_t>((std::numeric_limits<uint32_t>::max)()))
        return -1;
      result->composition_error_count =
          static_cast<uint32_t>(last_step_composition_errors_.size());
      if (aborted) {
        result->status = 1;
        return 0;
      }
      error_ = std::move(err);
      result->status = 0;
      return 0;
    }

    result->status = 3;
    if (buffered) last_step_data_ = std::move(output_bytes);
    last_step_assets_ = std::move(stats.referenced_assets);
    last_step_composition_errors_ = std::move(stats.composition_errors);
    if (last_step_assets_.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()))
      return -1;
    if (last_step_composition_errors_.size() >
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)()))
      return -1;
    result->asset_path_count = static_cast<uint32_t>(last_step_assets_.size());
    result->composition_error_count =
        static_cast<uint32_t>(last_step_composition_errors_.size());
    result->input_bytes = static_cast<double>(stats.input_bytes);
    result->output_bytes = static_cast<double>(stats.output_bytes);
    result->prim_count = static_cast<double>(stats.prim_count);
    result->arrays_passed_through = static_cast<double>(stats.arrays_passed_through);
    result->arrays_reencoded = static_cast<double>(stats.arrays_reencoded);
    result->read_ms = stats.read_ms;
    result->compose_ms = stats.compose_ms;
    result->write_ms = stats.write_ms;
    return 0;
  }

  int stepBuffer(uint8_t kind, uint32_t index, uint8_t* out,
                 uint32_t cap) const {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    const std::string* value = nullptr;
    if (kind == 0) {
      bytes = last_step_data_.data();
      size = last_step_data_.size();
    } else if (kind == 1) {
      value = &last_step_key_;
    } else if (kind == 2 && index < last_step_assets_.size()) {
      value = &last_step_assets_[index];
    } else if (kind == 3 && index < last_step_composition_errors_.size()) {
      value = &last_step_composition_errors_[index];
    } else if (kind == 4 && last_step_layer_dependencies_ &&
               index < last_step_layer_dependencies_->size()) {
      auto dependency = last_step_layer_dependencies_->begin();
      std::advance(dependency, index);
      value = &*dependency;
    } else {
      return -1;
    }
    if (value) {
      bytes = reinterpret_cast<const uint8_t*>(value->data());
      size = value->size();
    }
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    if (out && cap >= size && size) std::memcpy(out, bytes, size);
    return static_cast<int>(size);
  }

  int layerDependencyCount() const {
    const size_t count = last_step_layer_dependencies_
        ? last_step_layer_dependencies_->size() : 0;
    if (count >
        static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    return static_cast<int>(count);
  }

  void releaseStep() {
    std::vector<uint8_t>().swap(last_step_data_);
    std::vector<std::string>().swap(last_step_assets_);
    std::vector<std::string>().swap(last_step_composition_errors_);
    last_step_layer_dependencies_.reset();
    last_step_key_.clear();
  }

  void end() { reset_(); }

 private:
  void reset_() {
    releaseStep();
    root_.clear();
    root_.shrink_to_fit();
    root_name_.clear();
    lazy_arrays_ = true;
    began_ = false;
    layers_.clear();
    provided_layer_bytes_ = 0;
    parsed_layers_.clear();
    variant_overrides_.clear();
    asset_path_remap_.clear();
    asset_path_remap_bytes_ = 0;
    error_.clear();
  }

  std::string root_;
  std::string root_name_;
  bool lazy_arrays_ = true;
  bool began_ = false;
  std::string error_;
  uint32_t max_input_bytes_ = 512u * 1024u * 1024u;
  uint32_t max_output_bytes_ = 512u * 1024u * 1024u;
  size_t provided_layer_bytes_ = 0;
  std::vector<uint8_t> last_step_data_;
  std::vector<std::string> last_step_assets_;
  std::vector<std::string> last_step_composition_errors_;
  std::shared_ptr<std::set<std::string>> last_step_layer_dependencies_;
  std::string last_step_key_;
  std::map<std::string, std::string> layers_;
  std::map<std::string, std::shared_ptr<tn::Layer>> parsed_layers_;
  std::map<std::string, std::string> variant_overrides_;
  std::map<std::string, std::string> asset_path_remap_;
  size_t asset_path_remap_bytes_ = 0;
};

namespace lightusd {
namespace web_next {
int NextSubdivRefine(void* object, const float* points, uint32_t point_values,
                    const uint32_t* face_counts, uint32_t face_count,
                    const uint32_t* face_indices, uint32_t index_count,
                    const float* uv_values, uint32_t uv_value_count,
                    const uint32_t* uv_indices, uint32_t uv_index_count,
                    const lightusd_next_subdiv_options* options,
                    uint32_t callback_id) {
  return static_cast<SubdivStreamer*>(object)->refineStream(
      points, point_values, face_counts, face_count, face_indices, index_count,
      uv_values, uv_value_count, uv_indices, uv_index_count, *options,
      callback_id);
}
int NextSubdivError(void* object, uint8_t* out, uint32_t cap) {
  const std::string& error = static_cast<SubdivStreamer*>(object)->error();
  if (error.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return -1;
  if (out && cap >= error.size()) std::memcpy(out, error.data(), error.size());
  return static_cast<int>(error.size());
}
int NextFlattenBegin(void* object, const uint8_t* root, uint32_t root_size,
                     const uint8_t* name, uint32_t name_size, bool lazy_arrays) {
  return static_cast<NextFlattenSession*>(object)->beginBytes(
             root, root_size, name, name_size, lazy_arrays) ? 1 : 0;
}

int NextFlattenProvideLayer(void* object, const uint8_t* key,
                            uint32_t key_size, const uint8_t* data,
                            uint32_t data_size) {
  return static_cast<NextFlattenSession*>(object)->provideLayerBytes(
             key, key_size, data, data_size) ? 1 : 0;
}

int NextFlattenAddSublayer(void* object, const uint8_t* path,
                           uint32_t path_size) {
  return static_cast<NextFlattenSession*>(object)->addSublayerBytes(
      path, path_size) ? 1 : 0;
}

int NextFlattenAddPrimArc(void* object, uint8_t kind, uint8_t list_op,
                          const uint8_t* prim_path, uint32_t prim_size,
                          const uint8_t* asset_path, uint32_t asset_size,
                          const uint8_t* target_path, uint32_t target_size) {
  return static_cast<NextFlattenSession*>(object)->addPrimArcBytes(
      kind, list_op, prim_path, prim_size, asset_path, asset_size, target_path,
      target_size) ? 1 : 0;
}

int NextFlattenSetAssetPathRemap(void* object, const uint8_t* json,
                                uint32_t json_size) {
  if (json_size && !json) return -1;
  return static_cast<NextFlattenSession*>(object)->setAssetPathRemapJSON(
      json, json_size);
}

int NextFlattenRemapLayerAssetPaths(void* object, const uint8_t* json,
                                   uint32_t json_size) {
  if (json_size && !json) return -1;
  return static_cast<NextFlattenSession*>(object)->remapLayerAssetPathsJSON(
      json, json_size);
}

  int NextFlattenSetMaxInputBytes(void* object, uint32_t limit_bytes) {
  return static_cast<NextFlattenSession*>(object)->setMaxInputBytes(limit_bytes);
}

int NextFlattenMaxInputBytes(void* object) {
  return static_cast<int>(static_cast<NextFlattenSession*>(object)->maxInputBytes());
}

int NextFlattenSetMaxOutputBytes(void* object, uint32_t limit_bytes) {
  return static_cast<NextFlattenSession*>(object)->setMaxOutputBytes(limit_bytes);
}

int NextFlattenMaxOutputBytes(void* object) {
  return static_cast<int>(static_cast<NextFlattenSession*>(object)->maxOutputBytes());
}

int NextFlattenInputBytes(void* object) {
  return static_cast<int>(static_cast<NextFlattenSession*>(object)->inputBytes());
}

int NextFlattenPreflightLayer(void* object, const uint8_t* key,
                              uint32_t key_size, uint32_t data_size) {
  return static_cast<NextFlattenSession*>(object)->preflightLayerBytes(
      key, key_size, data_size);
}

int NextFlattenSetVariant(void* object, const uint8_t* key,
                          uint32_t key_size, const uint8_t* selection,
                          uint32_t selection_size) {
  if ((key_size && !key) || (selection_size && !selection)) return -1;
  const std::string key_text = key_size
      ? std::string(reinterpret_cast<const char*>(key), key_size) : std::string();
  const std::string selection_text = selection_size
      ? std::string(reinterpret_cast<const char*>(selection), selection_size)
      : std::string();
  static_cast<NextFlattenSession*>(object)->setVariantOverride(
      key_text, selection_text);
  return 0;
}

void NextFlattenEnd(void* object) {
  static_cast<NextFlattenSession*>(object)->end();
}

int NextFlattenError(void* object, uint8_t* out, uint32_t cap) {
  const std::string& error = static_cast<NextFlattenSession*>(object)->error();
  if (error.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
    return -1;
  if (out && cap >= error.size() && !error.empty())
    std::memcpy(out, error.data(), error.size());
  return static_cast<int>(error.size());
}

int NextFlattenStep(void* object, uint32_t callback_id,
                    lightusd_next_flatten_step_info* out) {
  return static_cast<NextFlattenSession*>(object)->step(callback_id, out);
}

int NextFlattenStepBuffer(void* object, uint8_t kind, uint32_t index,
                          uint8_t* out, uint32_t cap) {
  return static_cast<NextFlattenSession*>(object)->stepBuffer(
      kind, index, out, cap);
}

int NextFlattenLayerDependencyCount(void* object) {
  return static_cast<NextFlattenSession*>(object)->layerDependencyCount();
}

void NextFlattenReleaseStep(void* object) {
  static_cast<NextFlattenSession*>(object)->releaseStep();
}
}  // namespace web_next
}  // namespace lightusd
class NextUSDZConverterNative {
 public:
  NextUSDZConverterNative() = default;

  std::string error() const { return error_; }
  std::string warn() const { return warn_; }

  void clearURDFMeshBuffers() { urdf_mesh_buffers_.clear(); mesh_bytes_ = 0; }

  bool setMeshBuffer(const std::string& name,
                     const float* positions, uint32_t position_count,
                     const float* normals, uint32_t normal_count,
                     const float* uvs, uint32_t uv_count,
                     const uint32_t* indices, uint32_t index_count) {
    return setURDFMeshBuffer(name, positions, position_count, normals,
                             normal_count, uvs, uv_count, indices, index_count);
  }

  bool createURDFPhysicsScene(const std::string& robot_json) {
    if (!canReplaceRootBytes_(robot_json.size())) return false;
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
    root_bytes_ = robot_json.size();
    warn_ = std::move(warn);
    error_.clear();
    has_stage_ = true;
    return true;
  }

  bool loadFromBinary(const uint8_t* bytes, uint32_t size,
                      const std::string& filename) {
    if (!canReplaceRootBytes_(size)) return false;
    std::string input;
    if (size) input.assign(reinterpret_cast<const char*>(bytes), size);
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
    root_bytes_ = size;
    warn_ = std::move(warn);
    error_.clear();
    has_stage_ = true;
    return true;
  }

  bool setAsset(const std::string& name, const uint8_t* bytes, uint32_t size) {
    const auto existing = assets_.find(name);
    const size_t old_size = existing == assets_.end() ? 0 : existing->second.size();
    const size_t retained = asset_bytes_ - old_size;
    if (retained > max_asset_bytes_ || size > max_asset_bytes_ - retained) {
      error_ = "Packaged assets exceed configured aggregate byte limit";
      return false;
    }
    if (!canReplaceRetainedPayload_(old_size, size)) return false;
    std::vector<uint8_t> replacement;
    if (size) replacement.assign(bytes, bytes + size);
    assets_[name] = std::move(replacement);
    asset_bytes_ = retained + size;
    error_.clear();
    return true;
  }

  int preflightAsset(const std::string& name, uint32_t size) {
    const auto existing = assets_.find(name);
    const size_t old_size = existing == assets_.end() ? 0 : existing->second.size();
    const size_t retained = asset_bytes_ - old_size;
    if (retained > max_asset_bytes_ || size > max_asset_bytes_ - retained) {
      error_ = "Packaged assets exceed configured aggregate byte limit";
      return -1;
    }
    if (!canReplaceRetainedPayload_(old_size, size)) return -1;
    error_.clear();
    return 0;
  }
  int preflightAsset(const uint8_t* name, uint32_t name_size,
                     uint32_t size) {
    if (name_size && !name) {
      error_ = "Invalid packaged asset name";
      return -1;
    }
    const std::string key = name_size
        ? std::string(reinterpret_cast<const char*>(name), name_size)
        : std::string();
    return preflightAsset(key, size);
  }

  int setMaxAssetBytes(uint32_t limit) {
    if (limit == 0 || limit > (uint32_t{1} << 30) || limit < asset_bytes_) {
      error_ = "Asset byte limit is invalid or below retained payload bytes";
      return -1;
    }
    max_asset_bytes_ = limit;
    error_.clear();
    return 0;
  }
  uint32_t maxAssetBytes() const { return max_asset_bytes_; }
  uint32_t assetBytes() const { return static_cast<uint32_t>(asset_bytes_); }

  int setMaxRetainedPayloadBytes(uint32_t limit) {
    if (limit == 0 || limit > (uint32_t{1} << 30) ||
        retainedPayloadBytes() > limit) {
      error_ = "Retained payload limit is invalid or below current payload bytes";
      return -1;
    }
    max_retained_payload_bytes_ = limit;
    error_.clear();
    return 0;
  }
  uint32_t maxRetainedPayloadBytes() const { return max_retained_payload_bytes_; }
  uint32_t retainedPayloadBytes() const {
    return static_cast<uint32_t>(root_bytes_ + asset_bytes_ + mesh_bytes_);
  }
  int preflightRoot(uint32_t size) {
    return canReplaceRootBytes_(size) ? 0 : -1;
  }

  int setMaxMeshBytes(uint32_t limit) {
    if (limit == 0 || limit > (uint32_t{1} << 30) || limit < mesh_bytes_) {
      error_ = "Mesh byte limit is invalid or below retained payload bytes";
      return -1;
    }
    max_mesh_bytes_ = limit;
    error_.clear();
    return 0;
  }
  uint32_t maxMeshBytes() const { return max_mesh_bytes_; }
  uint32_t meshBytes() const { return static_cast<uint32_t>(mesh_bytes_); }
  int preflightMesh(const uint8_t* name, uint32_t name_size,
                    uint32_t position_count, uint32_t normal_count,
                    uint32_t uv_count, uint32_t index_count) {
    if (name_size && !name) {
      error_ = "Invalid mesh name";
      return -1;
    }
    const std::string key = name_size
        ? std::string(reinterpret_cast<const char*>(name), name_size)
        : std::string();
    return preflightMesh(key, position_count, normal_count, uv_count,
                         index_count);
  }

  bool setUSDCExportLimitMB(int file_size_mb, int memory_mb) {
    usdc_max_file_size_bytes_ = file_size_mb > 0
        ? static_cast<uint64_t>(file_size_mb) * (uint64_t{1} << 20) : 0;
    usdc_max_memory_bytes_ = memory_mb > 0
        ? static_cast<uint64_t>(memory_mb) * (uint64_t{1} << 20) : 0;
    return true;
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

  bool exportAsUSDC(std::vector<uint8_t>* output) {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return false;
    }
    tn::USDCWriteOptions options;
    options.crate_options.max_file_size_bytes = usdc_max_file_size_bytes_;
    options.crate_options.max_memory_bytes = usdc_max_memory_bytes_;
    tn::USDCWriteResult result = tn::WriteUSDCToMemory(*output, stage_, options);
    if (!result.success) {
      error_ = result.error.empty() ? "USDC export failed" : result.error;
      return false;
    }
    error_.clear();
    return true;
  }

  bool exportAsUSDZ(std::vector<uint8_t>* output, bool usda_root = false) {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return false;
    }
    return exportAsUSDZFrom_(stage_, assets_, output, usda_root);
  }

  bool exportAsUSDZWithRemapJSON(const uint8_t* data, uint32_t size,
                                 bool usda_root) {
    if (!has_stage_) {
      error_ = "No stage loaded";
      return false;
    }
    if ((!data && size) || size > max_retained_payload_bytes_) {
      error_ = "Invalid asset path remap input";
      return false;
    }
    lightusd::minijson::Value parsed;
    lightusd::minijson::ParseOptions parse_options;
    parse_options.reject_duplicate_keys = false;
    if (!lightusd::minijson::Parse(
            size ? std::string(reinterpret_cast<const char*>(data), size)
                 : std::string("{}"),
            &parsed, nullptr, parse_options) || !parsed.is_object()) {
      error_ = "Asset path remap must be a JSON object of strings";
      return false;
    }
    std::map<std::string, std::string> remap;
    const auto* members = parsed.object_items();
    if (!members) {
      error_ = "Asset path remap must be a JSON object";
      return false;
    }
    size_t remap_bytes = 0;
    for (const auto& member : *members) {
      const std::string* replacement = member.value().string_ptr();
      if (member.key.empty() || !replacement ||
          member.key.find('\0') != std::string::npos ||
          replacement->find('\0') != std::string::npos ||
          member.key.size() > (1u << 20) || replacement->size() > (1u << 20)) {
        error_ = "Asset remap keys must be non-empty and paths must be bounded";
        return false;
      }
      const size_t pair_bytes = member.key.size() + replacement->size();
      if (pair_bytes > max_retained_payload_bytes_ ||
          remap_bytes > max_retained_payload_bytes_ - pair_bytes) {
        error_ = "Asset path remap exceeds aggregate input byte limit";
        return false;
      }
      remap_bytes += pair_bytes;
      remap.emplace(member.key, *replacement);
    }

    size_t estimated_working_set = stage_.GetMemoryUsage();
    const size_t working_budget = max_retained_payload_bytes_;
    const auto charge = [&](size_t bytes) {
      if (estimated_working_set > working_budget ||
          bytes > working_budget - estimated_working_set) {
        return false;
      }
      estimated_working_set += bytes;
      return true;
    };
    // The remap export keeps the original stage/assets alive while cloning the
    // stage and constructing a renamed asset map for the writer.
    if (!charge(asset_bytes_) || !charge(mesh_bytes_) ||
        !charge(asset_bytes_) || !charge(remap_bytes)) {
      error_ = "USDZ remap working set exceeds configured retained-payload limit";
      return false;
    }

    tn::Stage remapped = stage_.Clone();
    tn::Layer* root = remapped.GetRootLayer();
    if (root) {
      for (size_t i = 0; i < root->prim_count(); ++i) {
        if (tn::PrimSpec* prim = root->prim(static_cast<uint32_t>(i))) {
          prim->remap_asset_paths(remap);
        }
      }
    }
    std::map<std::string, std::vector<uint8_t>> remapped_assets;
    for (const auto& asset : assets_) {
      const auto mapped = remap.find(asset.first);
      const std::string& name = mapped == remap.end() ? asset.first : mapped->second;
      if (!remapped_assets.emplace(name, asset.second).second) {
        error_ = "Asset path remap creates duplicate packaged asset names";
        return false;
      }
    }
    export_data_.clear();
    if (!exportAsUSDZFrom_(remapped, remapped_assets, &export_data_, usda_root)) {
      export_data_.clear();
      return false;
    }
    if (export_data_.size() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      error_ = "USDZ output exceeds 2 GiB";
      export_data_.clear();
      return false;
    }
    return true;
  }

 private:
  bool exportAsUSDZFrom_(
      tn::Stage& stage,
      const std::map<std::string, std::vector<uint8_t>>& assets,
      std::vector<uint8_t>* output, bool usda_root) {
    tn::USDZWriteOptions usdz_options;
    usdz_options.max_file_size_bytes = usdc_max_file_size_bytes_;
    usdz_options.max_memory_bytes = usdc_max_memory_bytes_;
    tn::USDZWriteResult result;
    if (usda_root) {
      const std::string usda = tn::WriteUSDAToString(stage_);
      if (usda.empty()) {
        error_ = "USDA export failed";
        return false;
      }
      result = tn::WriteUSDZFromUSDAAndAssetsToMemory(
          *output, reinterpret_cast<const uint8_t*>(usda.data()), usda.size(),
          assets, usdz_options);
    } else {
      std::vector<uint8_t> usdc;
      tn::USDCWriteOptions usdc_options;
      usdc_options.crate_options.max_file_size_bytes = usdc_max_file_size_bytes_;
      usdc_options.crate_options.max_memory_bytes = usdc_max_memory_bytes_;
      tn::USDCWriteResult usdc_result =
          tn::WriteUSDCToMemory(usdc, stage, usdc_options);
      if (!usdc_result.success) {
        error_ = usdc_result.error.empty() ? "USDC export failed"
                                           : usdc_result.error;
        return false;
      }
      result = tn::WriteUSDZFromUSDCAndAssetsToMemory(
          *output, usdc.data(), usdc.size(), assets, usdz_options);
    }
    if (!result.success) {
      error_ = result.error.empty() ? "USDZ export failed" : result.error;
      return false;
    }
    error_.clear();
    return true;
  }

 public:

  bool exportData(uint8_t kind, uint8_t root_format = 0) {
    export_data_.clear();
    if (kind == 0 || kind == 1) {
      const std::string output = kind == 0 ? extractPhysicsSceneJSON()
                                            : exportAsUSDA();
      if (!error_.empty()) return false;
      if (output.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        error_ = "Converter output exceeds 2 GiB";
        return false;
      }
      export_data_.assign(output.begin(), output.end());
      return true;
    }
    const bool ok = kind == 2 ? exportAsUSDC(&export_data_)
        : exportAsUSDZ(&export_data_, kind == 3 && root_format == 1);
    if (!ok) {
      export_data_.clear();
      return false;
    }
    if (export_data_.size() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      error_ = "Converter output exceeds 2 GiB";
      export_data_.clear();
      return false;
    }
    return true;
  }
  const std::vector<uint8_t>& exportBytes() const { return export_data_; }

  bool rewriteRoot(const uint8_t* bytes, uint32_t size,
                   const lightusd_next_rewrite_options& options,
                   lightusd_next_rewrite_info* out) {
    error_.clear();
    warn_.clear();
    rewrite_data_.clear();
    out->format = options.format;
    out->data_size = 0;
    out->reserved = 0;
    out->token_count = 0;
    out->path_count = 0;
    out->spec_count = 0;
    std::string input;
    if (size) input.assign(reinterpret_cast<const char*>(bytes), size);

    tn::Stage stage;
    tn::LoadUSDOptions load_opts;
    load_opts.usda_options.parse_options.enable_usda_lazy_arrays =
        options.usda_lazy != 0;
    // Zero retains finite library defaults, matching the historical sentinel.
    if (options.max_memory > 0) {
      const size_t limit = static_cast<size_t>(options.max_memory);
      load_opts.limits.max_input_bytes = limit;
      load_opts.limits.max_asset_bytes = limit;
      load_opts.limits.max_resident_bytes = limit;
    }

    const bool ok = tn::LoadUSDFromMemoryOwned(
        std::move(input), &stage, load_opts, &warn_, &error_);
    if (!ok) {
      if (error_.empty()) error_ = "next-core USD load failed";
      return false;
    }

    if (options.format == 1) {
      std::string text = tn::WriteUSDAToString(stage);
      if (text.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        error_ = "USDA rewrite output exceeds 2 GiB";
        return false;
      }
      rewrite_data_.assign(text.begin(), text.end());
      out->data_size = static_cast<uint32_t>(rewrite_data_.size());
      return true;
    }

    tn::USDCWriteOptions write_options;
    write_options.crate_options.max_file_size_bytes = usdc_max_file_size_bytes_;
    write_options.crate_options.max_memory_bytes = usdc_max_memory_bytes_;
    tn::USDCWriteResult wr =
        tn::WriteUSDCToMemory(rewrite_data_, stage, write_options);
    if (!wr.success) {
      error_ = wr.error.empty() ? "next-core USDC write failed" : wr.error;
      rewrite_data_.clear();
      return false;
    }
    if (rewrite_data_.size() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      error_ = "USDC rewrite output exceeds 2 GiB";
      rewrite_data_.clear();
      return false;
    }
    out->data_size = static_cast<uint32_t>(rewrite_data_.size());
    out->token_count = static_cast<double>(wr.token_count);
    out->path_count = static_cast<double>(wr.path_count);
    out->spec_count = static_cast<double>(wr.spec_count);
    return true;
  }

  const std::vector<uint8_t>& rewriteData() const { return rewrite_data_; }

 private:
  bool setURDFMeshBuffer(const std::string& name,
                         const float* positions, uint32_t position_count,
                         const float* normals, uint32_t normal_count,
                         const float* uvs, uint32_t uv_count,
                         const uint32_t* indices, uint32_t index_count) {
    if (preflightMesh(name, position_count, normal_count, uv_count,
                      index_count) != 0) return false;
    for (uint32_t i = 0; i < index_count; ++i) {
      if (indices[i] > static_cast<uint32_t>(INT32_MAX)) {
        error_ = "Mesh index exceeds int32 range";
        return false;
      }
    }
    tr::URDFMeshBuffer buffer;
    if (position_count) buffer.positions.assign(positions, positions + position_count);
    if (normal_count) buffer.normals.assign(normals, normals + normal_count);
    if (uv_count) buffer.uvs.assign(uvs, uvs + uv_count);
    buffer.indices.reserve(index_count);
    for (uint32_t i = 0; i < index_count; ++i) {
      buffer.indices.push_back(static_cast<int32_t>(indices[i]));
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
    const auto existing = urdf_mesh_buffers_.find(name);
    const uint64_t old_size = existing == urdf_mesh_buffers_.end()
        ? 0 : meshBufferBytes_(existing->second);
    const uint64_t new_size = meshBufferBytes_(buffer);
    urdf_mesh_buffers_[name] = std::move(buffer);
    mesh_bytes_ = mesh_bytes_ - old_size + new_size;
    error_.clear();
    return true;
  }

  bool canReplaceRetainedPayload_(uint64_t old_size, uint64_t new_size) {
    const uint64_t retained = root_bytes_ + asset_bytes_ + mesh_bytes_;
    if (old_size > retained || retained - old_size > max_retained_payload_bytes_ ||
        new_size > max_retained_payload_bytes_ - (retained - old_size)) {
      error_ = "Payload exceeds configured aggregate retained byte limit";
      return false;
    }
    return true;
  }

  bool canReplaceRootBytes_(uint64_t new_size) {
    if (new_size > (uint32_t{1} << 30) ||
        !canReplaceRetainedPayload_(root_bytes_, new_size)) {
      if (error_.empty()) error_ = "Root input exceeds 1 GiB limit";
      return false;
    }
    return true;
  }

  uint64_t meshBufferBytes_(const tr::URDFMeshBuffer& buffer) const {
    return (static_cast<uint64_t>(buffer.positions.size()) +
            buffer.normals.size() + buffer.uvs.size() +
            buffer.indices.size()) * sizeof(uint32_t);
  }

  int preflightMesh(const std::string& name, uint32_t position_count,
                    uint32_t normal_count, uint32_t uv_count,
                    uint32_t index_count) {
    if (name.empty()) {
      error_ = "setVisualMesh/setCollisionMesh requires a non-empty name";
      return -1;
    }
    if (position_count < 9 || position_count % 3 != 0) {
      error_ = "Mesh positions must contain at least three xyz points";
      return -1;
    }
    if (normal_count != 0 && normal_count != position_count) {
      error_ = "Mesh normals length must match positions length";
      return -1;
    }
    if (uv_count != 0 && uv_count != (position_count / 3) * 2) {
      error_ = "Mesh UV length must equal vertex count * 2";
      return -1;
    }
    if (index_count % 3 != 0) {
      error_ = "Mesh indices must contain triangles";
      return -1;
    }
    const auto existing = urdf_mesh_buffers_.find(name);
    const uint64_t old_size = existing == urdf_mesh_buffers_.end()
        ? 0 : meshBufferBytes_(existing->second);
    const uint64_t retained = mesh_bytes_ - old_size;
    const uint64_t new_size =
        (static_cast<uint64_t>(position_count) + normal_count + uv_count +
         index_count) * sizeof(uint32_t);
    if (retained > max_mesh_bytes_ || new_size > max_mesh_bytes_ - retained) {
      error_ = "URDF mesh buffers exceed configured aggregate byte limit";
      return -1;
    }
    if (!canReplaceRetainedPayload_(old_size, new_size)) return -1;
    error_.clear();
    return 0;
  }

  std::string error_;
  std::string warn_;
  std::vector<uint8_t> rewrite_data_;
  std::vector<uint8_t> export_data_;
  uint64_t usdc_max_file_size_bytes_{0};
  uint64_t usdc_max_memory_bytes_{0};
  uint32_t max_mesh_bytes_{uint32_t{1} << 29};
  uint64_t mesh_bytes_{0};
  uint64_t root_bytes_{0};
  tn::Stage stage_;
  bool has_stage_ = false;
  std::map<std::string, tr::URDFMeshBuffer> urdf_mesh_buffers_;
  std::map<std::string, std::vector<uint8_t>> assets_;
  uint32_t max_asset_bytes_{uint32_t{1} << 29};
  size_t asset_bytes_{0};
  uint32_t max_retained_payload_bytes_{uint32_t{1} << 30};
};



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
static std::string validateFromBytes(const uint8_t* data, size_t size,
                                      const std::string& filename,
                                      const std::string& options_json) {
  tn::ValidationOptions options =
      ParseValidationOptionsJSONForWeb(options_json);
  // This module-level adapter preserves the legacy loader contract. The next
  // validator keeps its stricter authored-stage-presence rules enabled for
  // native callers; the legacy WASM validator never emitted these warnings.
  options.stage_presence_checks = false;

  lightusd::minijson::Value result;
  tn::USDValidationResult validation;
  std::string warn, err;
  const bool is_usdc = tn::IsUSDCData(data, size);
  bool loaded = false;
  if (is_usdc) {
    tn::CrateReadOptions crate_options;
    tn::CrateReader reader(crate_options);
    tn::CrateReadResult crate = reader.Read(data, size);
    for (const std::string& item : crate.warnings) {
      if (!warn.empty() && warn.back() != '\n') warn.push_back('\n');
      warn += item;
      if (warn.empty() || warn.back() != '\n') warn.push_back('\n');
    }
    if (crate.success) {
      const tn::Layer* root = crate.stage.GetRootLayer();
      if (root) {
        validation = tn::ValidateLayerAgainstAOUSDCore(*root, options);
        // A successful bounded CrateReader pass has checked the binary crate
        // structure; the semantic rules above validate its decoded root layer.
        validation.checked_groups.crate = options.core || options.crate;
        loaded = true;
      }
    }
  }
  if (!loaded) {
    warn.clear();
    err.clear();
    loaded = tn::ValidateUSDFromMemoryAgainstAOUSDCore(
        data, size, filename, options, &validation, &warn, &err);
  }
  // The legacy WASM CrateReader reports that threaded reading is unavailable
  // for every USDC load. Keep that observable loader warning in the
  // compatibility adapter; it does not imply the crate validation group ran.
  if (is_usdc) {
    if (!warn.empty() && warn.back() != '\n') warn.push_back('\n');
    warn += "Threading is disabled for this build.\n";
  }
  if (!loaded) {
    result["parse_ok"] = false;
    result["ok"] = false;
    result["error"] = err.empty() ? std::string("Validation failed") : err;
    if (!warn.empty()) result["warn"] = warn;
    return result.dump();
  }

  result = ValidationResultToJSON(validation);
  if (!warn.empty()) result["warn"] = warn;
  return result.dump();
}

// Pre-composition layer diff, retaining the JS result fields as JSON.
static std::string usddiffJson(const uint8_t* left, size_t left_size,
                               const std::string& left_name,
                               const uint8_t* right, size_t right_size,
                               const std::string& right_name,
                               const lightusd_next_diff_options& options) {
  lightusd::minijson::Value result;
  tn::DiffOptions diff_opts;
  if (options.ulps >= 0) {
    diff_opts.floatUlps = static_cast<uint32_t>(options.ulps);
    diff_opts.doubleUlps = static_cast<uint64_t>(options.ulps);
  }
  diff_opts.absEps = options.abs_eps;
  diff_opts.compareMetadata = options.compare_metadata != 0;
  diff_opts.fuzzyAssetPaths = options.fuzzy_asset_paths != 0;

  std::string warn, err;
  std::shared_ptr<tn::Layer> lhs = tn::pcp::LoadLayerFromMemory(
      left_name, left, left_size, &warn, &err);
  if (!lhs) {
    result["success"] = false;
    result["error"] = "Error loading " + left_name + ": " + err;
    return result.dump();
  }
  err.clear();
  std::shared_ptr<tn::Layer> rhs = tn::pcp::LoadLayerFromMemory(
      right_name, right, right_size, &warn, &err);
  if (!rhs) {
    result["success"] = false;
    result["error"] = "Error loading " + right_name + ": " + err;
    return result.dump();
  }

  std::unordered_map<std::string, tn::PrimSpecDiff> prim_diffs;
  std::unordered_map<std::string, tn::PropDiff> property_diffs;
  tn::LayerMetaDiff layer_meta_diff;
  tn::Diff(*lhs, *rhs, prim_diffs, property_diffs, diff_opts,
           &layer_meta_diff);
  const bool has_diffs = !prim_diffs.empty() || !property_diffs.empty() ||
                         layer_meta_diff.changed();
  result["success"] = true;
  result["hasDiffs"] = has_diffs;
  if (!warn.empty()) result["warn"] = warn;
  if (options.format == 0 || options.format == 2) {
    result["text"] = has_diffs
        ? tn::DiffToText(*lhs, *rhs, left_name, right_name, diff_opts)
        : std::string("No differences found.\n");
  }
  if (options.format == 1 || options.format == 2) {
    result["json"] = tn::DiffToJSON(*lhs, *rhs, left_name, right_name,
                                     diff_opts);
  }
  return result.dump();
}

// Direct C dispatch: one compiled call boundary, no class_/function binding
// templates or per-signature invoker registration.
namespace lightusd {
namespace web_next {
int NextConverterRewrite(void* object, const uint8_t* data, uint32_t size,
                         const lightusd_next_rewrite_options* options,
                         lightusd_next_rewrite_info* out) {
  return static_cast<NextUSDZConverterNative*>(object)->rewriteRoot(
      data, size, *options, out) ? 1 : 0;
}
int NextConverterRewriteBuffer(void* object, uint8_t* out, uint32_t cap) {
  const auto& data = static_cast<NextUSDZConverterNative*>(object)->rewriteData();
  if (data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return -1;
  if (out && cap >= data.size()) std::memcpy(out, data.data(), data.size());
  return static_cast<int>(data.size());
}
int NextConverterString(void* object, uint8_t kind, uint8_t* out,
                        uint32_t cap) {
  const auto* converter = static_cast<NextUSDZConverterNative*>(object);
  const std::string value = kind == 0 ? converter->error() : converter->warn();
  if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return -1;
  if (out && cap >= value.size()) std::memcpy(out, value.data(), value.size());
  return static_cast<int>(value.size());
}
int NextConverterSetBytes(void* object, uint8_t kind, const uint8_t* name,
                          uint32_t name_size, const uint8_t* data,
                          uint32_t size) {
  if (size > (uint32_t{1} << 30) || (name_size && !name) ||
      (size && !data)) return -1;
  auto* converter = static_cast<NextUSDZConverterNative*>(object);
  if ((kind == 0 || kind == 2) && converter->preflightRoot(size) != 0) {
    return 0;
  }
  const std::string key = name_size
      ? std::string(reinterpret_cast<const char*>(name), name_size)
      : std::string();
  if (kind == 0) return converter->loadFromBinary(data, size, key) ? 1 : 0;
  if (kind == 1) {
    return converter->setAsset(key, data, size) ? 1 : 0;
  }
  const std::string json = size
      ? std::string(reinterpret_cast<const char*>(data), size)
      : std::string();
  return converter->createURDFPhysicsScene(json) ? 1 : 0;
}
int NextConverterSetMesh(void* object, const uint8_t* name,
                         uint32_t name_size, const float* positions,
                         uint32_t position_count, const float* normals,
                         uint32_t normal_count, const float* uvs,
                         uint32_t uv_count, const uint32_t* indices,
                         uint32_t index_count) {
  const std::string key = name_size
      ? std::string(reinterpret_cast<const char*>(name), name_size)
      : std::string();
  return static_cast<NextUSDZConverterNative*>(object)->setMeshBuffer(
      key, positions, position_count, normals, normal_count, uvs, uv_count,
      indices, index_count) ? 1 : 0;
}
int NextConverterPreflightMesh(void* object, const uint8_t* name,
                               uint32_t name_size, uint32_t position_count,
                               uint32_t normal_count, uint32_t uv_count,
                               uint32_t index_count) {
  return static_cast<NextUSDZConverterNative*>(object)->preflightMesh(
      name, name_size, position_count, normal_count, uv_count, index_count) == 0
      ? 1 : 0;
}
int NextConverterControl(void* object, uint8_t kind, int32_t first,
                         int32_t second) {
  auto* converter = static_cast<NextUSDZConverterNative*>(object);
  if (kind == 0) {
    converter->clearURDFMeshBuffers();
    return 0;
  }
  if (kind == 1)
    return converter->setUSDCExportLimitMB(first, second) ? 0 : -2;
  if (kind == 2) return converter->setMaxAssetBytes(static_cast<uint32_t>(first));
  if (kind == 3) {
    if (second != 0 || (first != 0 && first != 1)) return -1;
    return first == 0 ? static_cast<int>(converter->assetBytes())
                      : static_cast<int>(converter->maxAssetBytes());
  }
  if (kind == 4) return converter->setMaxMeshBytes(static_cast<uint32_t>(first));
  if (kind == 5) {
    if (second != 0 || (first != 0 && first != 1)) return -1;
    return first == 0 ? static_cast<int>(converter->meshBytes())
                      : static_cast<int>(converter->maxMeshBytes());
  }
  if (kind == 6) return converter->setMaxRetainedPayloadBytes(
      static_cast<uint32_t>(first));
  if (kind == 7) {
    if (second != 0 || (first != 0 && first != 1)) return -1;
    return first == 0 ? static_cast<int>(converter->retainedPayloadBytes())
                      : static_cast<int>(converter->maxRetainedPayloadBytes());
  }
  return -1;
}

int NextConverterRootPreflight(void* object, uint32_t size) {
  return static_cast<NextUSDZConverterNative*>(object)->preflightRoot(size);
}

int NextConverterPreflightAsset(void* object, const uint8_t* name,
                                uint32_t name_size, uint32_t size) {
  return static_cast<NextUSDZConverterNative*>(object)->preflightAsset(
      name, name_size, size);
}
int NextConverterExport(void* object, uint8_t kind) {
  return static_cast<NextUSDZConverterNative*>(object)->exportData(kind) ? 1 : 0;
}
int NextConverterExportWithOptions(void* object, uint8_t kind,
                                   uint8_t root_format) {
  if (kind != 3 || root_format > 1) return -1;
  return static_cast<NextUSDZConverterNative*>(object)->exportData(
      kind, root_format) ? 1 : 0;
}
int NextConverterExportWithRemap(void* object, const uint8_t* data,
                                 uint32_t size, uint8_t root_format) {
  if (root_format > 1) return -1;
  return static_cast<NextUSDZConverterNative*>(object)->
      exportAsUSDZWithRemapJSON(data, size, root_format == 1) ? 1 : 0;
}
int NextConverterExportBuffer(void* object, uint8_t* out, uint32_t cap) {
  const auto& data = static_cast<NextUSDZConverterNative*>(object)->exportBytes();
  if (data.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    return -1;
  if (out && cap >= data.size()) std::memcpy(out, data.data(), data.size());
  return static_cast<int>(data.size());
}
const uint8_t* NextConverterExportData(void* object) {
  const auto& data = static_cast<NextUSDZConverterNative*>(object)->exportBytes();
  return data.empty() ? nullptr : data.data();
}

uint8_t* NextValidateJSON(const uint8_t* data, uint32_t size,
                          const uint8_t* filename, uint32_t filename_size,
                          const uint8_t* options, uint32_t options_size) {
  if (size > (uint32_t{1} << 30) || (size && !data) ||
      (filename_size && !filename) || (options_size && !options)) return nullptr;
  const std::string name = filename_size
      ? std::string(reinterpret_cast<const char*>(filename), filename_size)
      : std::string();
  const std::string config = options_size
      ? std::string(reinterpret_cast<const char*>(options), options_size)
      : std::string();
  const std::string json = validateFromBytes(data, size, name, config);
  uint8_t* result = static_cast<uint8_t*>(std::malloc(json.size() + 1));
  if (!result) return nullptr;
  std::memcpy(result, json.c_str(), json.size() + 1);
  return result;
}

uint8_t* NextDiffJSON(const lightusd_next_diff_options* options,
                      const uint8_t* left, uint32_t left_size,
                      const uint8_t* left_name, uint32_t left_name_size,
                      const uint8_t* right, uint32_t right_size,
                      const uint8_t* right_name, uint32_t right_name_size) {
  if (!options || options->struct_size < sizeof(lightusd_next_diff_options) ||
      left_size > (uint32_t{1} << 30) || right_size > (uint32_t{1} << 30) ||
      (left_size && !left) || (right_size && !right) ||
      (left_name_size && !left_name) || (right_name_size && !right_name))
    return nullptr;
  const std::string lhs_name = left_name_size
      ? std::string(reinterpret_cast<const char*>(left_name), left_name_size)
      : std::string();
  const std::string rhs_name = right_name_size
      ? std::string(reinterpret_cast<const char*>(right_name), right_name_size)
      : std::string();
  const std::string json = usddiffJson(
      left, left_size, lhs_name, right, right_size, rhs_name, *options);
  uint8_t* result = static_cast<uint8_t*>(std::malloc(json.size() + 1));
  if (!result) return nullptr;
  std::memcpy(result, json.c_str(), json.size() + 1);
  return result;
}

void* NextCreateObject(uint32_t kind) {
  switch (kind) {
    case 1: return new (std::nothrow) NextUSDZConverterNative;
    case 2: return new (std::nothrow) SubdivStreamer;
    case 3: return new (std::nothrow) NextFlattenSession;
    case 4: return new (std::nothrow) RenderStream;
    case 5: return new (std::nothrow) NextAssetStore;
    case 6: return new (std::nothrow) NextLayerDocument;
    default: return nullptr;
  }
}
void NextDestroyObject(uint32_t kind, void* object) {
  switch (kind) {
    case 1: delete static_cast<NextUSDZConverterNative*>(object); return;
    case 2: delete static_cast<SubdivStreamer*>(object); return;
    case 3: delete static_cast<NextFlattenSession*>(object); return;
    case 4: delete static_cast<RenderStream*>(object); return;
    case 5: delete static_cast<NextAssetStore*>(object); return;
    case 6: delete static_cast<NextLayerDocument*>(object); return;
    default: return;
  }
}
}  // namespace web_next
}  // namespace lightusd
