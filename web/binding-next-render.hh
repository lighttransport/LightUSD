// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include "binding-next-common.hh"
namespace lightusd {
namespace web_next {
class RenderStream {
 public:
  RenderStream();
  ~RenderStream();

  void setMaterialDedup(bool enabled) { material_dedup_ = enabled; }
  void setMeshMerge(bool enabled) { mesh_merge_ = enabled; }
  void setMeshMergeBakeTransform(bool enabled) {
    mesh_merge_bake_transform_ = enabled;
  }
  void setFlattenRenderTree(bool enabled) { flatten_render_tree_ = enabled; }
  void setMeshOnly(bool enabled) { mesh_only_ = enabled; }
  void setComputeTangents(bool enabled) { compute_tangents_ = enabled; }
  void setRenderSettingsPath(const std::string& path) {
    render_settings_path_ = path;
  }
  void setBuildVertexIndices(bool enabled) {
    build_vertex_indices_ = enabled;
    build_vertex_indices_set_ = true;
  }

  void provideAsset(const std::string& name, const emscripten::val& bytes);
  void clearAssets() { clip_assets_.clear(); }

  // Strongest variant selection. `key` is a variant-set name (applies to
  // every prim carrying that set) or the prim-scoped form
  // "<primPath>{<set>}" which wins over the bare-set key. Takes effect on
  // the next begin()/beginOwned().
  void setVariantOverride(const std::string &key,
                          const std::string &selection) {
    variant_overrides_[key] = selection;
  }
  void clearVariantOverrides() { variant_overrides_.clear(); }

  // Authored variant sets of the most recently loaded root layer (recorded
  // before composition consumes them): [{primPath, setName, selected,
  // variants: [names...]}].
  emscripten::val listVariants() const;
  void setTangentMethod(const std::string &method);

  // Adopt the root bytes by move. USDC lazy arrays and USDA lazy slices retain
  // this buffer directly instead of copying it again inside the loader.
  emscripten::val beginOwned(std::string &&crate);

  // Begin from a JS Uint8Array (one copy into the WASM heap, then adopted).
  emscripten::val begin(emscripten::val bytes);

  int meshCount() const {
    if (!loaded_ && outputs_.empty()) return 0;
    const size_t authored = mesh_merge_ ? outputs_.size() : meshes_.size();
    return static_cast<int>(authored + analytic_outputs_.size());
  }

  int nodeCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.nodes.size()) : 0;
  }

  int lightCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.lights.size()) : 0;
  }

  int pointsCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.points.size()) : 0;
  }

  int curvesCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.curves.size()) : 0;
  }

  int cameraCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.cameras.size()) : 0;
  }

  int pointInstancerCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.point_instancers.size())
                              : 0;
  }

  int pointInstanceDrawCount() const {
    return render_scene_valid_
               ? static_cast<int>(render_scene_.point_instance_draws.size())
               : 0;
  }

  int skeletonCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.skeletons.size()) : 0;
  }

  int animationCount() const {
    return render_scene_valid_ ? static_cast<int>(render_scene_.animations.size()) : 0;
  }

  int unsupportedRenderableCount() const {
    return render_scene_valid_
               ? static_cast<int>(render_scene_.unsupported_renderables.size())
               : 0;
  }

  std::string error() const { return error_; }

  emscripten::val getNode(int32_t node_id) const;

  emscripten::val getLight(int32_t light_id) const;

  emscripten::val getPoints(int32_t points_id);

  emscripten::val getCurves(int32_t curves_id);

  emscripten::val getCamera(int32_t camera_id) const;

  emscripten::val getPointInstancer(int32_t instancer_id) const;

  emscripten::val getPointInstanceDraw(int32_t draw_id) const;

  emscripten::val getSkeleton(int32_t skeleton_id) const;

  emscripten::val getAnimation(int32_t anim_id) const;

  // Adapter-oriented animation getter. Large aggregate skeletal arrays are
  // exposed as transient WASM heap descriptors instead of being pushed into
  // JavaScript arrays (and duplicated in both samplers and tracks). Consumers
  // must copy descriptor-backed data before the next heap-growing native call
  // or end(). getAnimation() remains the compatibility getter for direct API
  // users.
  emscripten::val getAnimationView(int32_t anim_id) const;

  emscripten::val getAllAnimations() const;

  emscripten::val getAnimationInfo(int32_t anim_id) const;

  emscripten::val getAllAnimationInfos() const;

  emscripten::val getUnsupportedRenderables() const;

  emscripten::val getStats() const;

  emscripten::val getSceneMetadata() const;

  // Materialize mesh i's geometry into the scratch and return zero-copy
  // descriptors {points,indices,normals,uv0} + resolved material. Valid until the
  // next getMesh()/end(); the JS caller must upload before calling getMesh again.
  emscripten::val getMesh(int i);

  // Free the stage, mesh list and scratch (returns the heap to the allocator).
  void end();

  void buildRenderScene_();

 private:
  struct TextureMeta {
    std::string path;
    std::string source_color_space = "auto";
    std::string wrap_s = "useMetadata";
    std::string wrap_t = "useMetadata";
    bool is_udim = false;
    bool color_transform_valid = false;
    bool color_transform_bypass = true;
    bool source_color_is_data = false;
    float source_gamma = 1.0f;
    float source_linear_bias = 0.0f;
    std::array<float, 9> source_to_display_linear = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f};
  };

  struct MaterialRecord {
    int32_t id = -1;
    std::string key;
    std::string prim_path;
    float base_color[3] = {0.8f, 0.8f, 0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;
    float occlusion = 1.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float opacity_threshold = -1.0f;
    bool has_hair = false;
    float hair_tint_r[3] = {0.42f, 0.12f, 0.035f};
    float hair_tint_tt[3] = {0.32f, 0.075f, 0.018f};
    float hair_tint_trt[3] = {0.16f, 0.035f, 0.008f};
    float hair_roughness_r[2] = {0.22f, 0.35f};
    float hair_roughness_tt[2] = {0.32f, 0.45f};
    float hair_roughness_trt[2] = {0.42f, 0.55f};
    float hair_absorption[3] = {0.35f, 0.8f, 1.4f};
    float hair_ior = 1.55f;
    float hair_cuticle_angle = 3.0f;
    std::string base_color_texture;
    std::string normal_texture;
    std::string roughness_texture;
    std::string metallic_texture;
    std::string occlusion_texture;
    std::string emissive_texture;
    std::string opacity_texture;
    TextureMeta base_color_meta;
    TextureMeta normal_meta;
    TextureMeta roughness_meta;
    TextureMeta metallic_meta;
    TextureMeta occlusion_meta;
    TextureMeta emissive_meta;
    TextureMeta opacity_meta;
  };

  struct OutputMesh {
    bool merged = false;
    int source_index = -1;
    std::string name;
    std::string prim_path;
    std::vector<float> points;
    std::vector<float> normals;
    std::vector<float> uv;
    std::vector<uint32_t> indices;
    bool soup = false;
    int32_t material_id = -1;
    bool double_sided = false;
    std::array<double, 16> local_matrix;
    std::array<double, 16> world_matrix;
  };

  template <typename Chunked>
  static void copyChunked_(const Chunked& src, std::vector<float>* dst) {
    if (!dst) return;
    dst->resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) (*dst)[i] = src[i];
  }

  void buildAnalyticOutputs_();

  struct Stats {
    size_t source_mesh_count = 0;
    size_t source_material_count = 0;
    size_t source_texture_count = 0;
    size_t merged_mesh_count = 0;
    size_t merge_group_count = 0;
    size_t skipped_merge_count = 0;
    size_t input_bytes = 0;
    double input_copy_ms = 0.0;
    double stage_load_ms = 0.0;
    double composition_ms = 0.0;
    double mesh_discovery_ms = 0.0;
    double optimize_ms = 0.0;
    double material_ms = 0.0;
    double material_identity_ms = 0.0;
    double material_conversion_ms = 0.0;
    double geometry_build_ms = 0.0;
    double merge_append_ms = 0.0;
    size_t material_identity_hits = 0;
    size_t material_identity_misses = 0;
    size_t material_graph_cache_hits = 0;
    size_t material_graph_cache_misses = 0;
    size_t geometry_borrowed_bytes = 0;
    size_t geometry_materialized_bytes = 0;
  };

  emscripten::val outputSourceMesh_(int i);

  emscripten::val outputMergedMesh_(const OutputMesh &record) const;

  template <typename T>
  static void freeVec_(std::vector<T> &v) { std::vector<T>().swap(v); }

  tr::MeshConfig::TangentComputationMethod tangentMethod_() const;

  bool computeScratchTangents_();

  bool readFloatArray_(const lightusd::next::UsdPrim &prim, const char *name,
                       tr::ValueArrayRead<float> *out);
  bool readIntArray_(const lightusd::next::UsdPrim &prim, const char *name,
                     tr::ValueArrayRead<int32_t> *out);
  static bool matBool_(const lightusd::next::UsdPrim &prim, const char *name,
                       bool fallback);

  static bool pointsArePlanar_(const std::vector<float> &points);

  bool effectiveDoubleSided_(const lightusd::next::UsdPrim &prim,
                             int32_t material_id,
                             const std::vector<float> &points) const;

  // Build render geometry for one mesh into the scratch (s_points_/s_normals_/
  // s_uv_/s_indices_). Returns true if the result is a NON-INDEXED triangle soup
  // (drawn with drawArrays), false if INDEXED.
  //   - all primvars per-vertex  -> keep the indexed form directly (compact);
  //   - indexed UVs / per-vertex UV with face-varying normals -> de-index AND
  //     WELD inline (one vertex per distinct pos/uv/normal tuple), recovering
  //     vertex sharing while keeping correct attributes at seams;
  //   - PURE face-varying UVs (no st indices) -> emit the non-indexed soup, the
  //     minimal form when corners are mostly unique (welding would only add
  //     index + hash-map overhead).
  // The full soup is never materialized in the welded path; at most one mesh is
  // resident at a time either way.
  bool buildRenderMesh_(const lightusd::next::UsdPrim &prim, bool *soup_out,
                        std::string *err);

  static std::string fmtFloat_(float v) {
    std::ostringstream ss;
    ss << std::setprecision(9) << v;
    return ss.str();
  }

  static std::string normTexKey_(const std::string &path);

  static bool isUdimPath_(const std::string &path) {
    return path.find("<UDIM>") != std::string::npos ||
           path.find("%04d") != std::string::npos ||
           path.find("%(UDIM)d") != std::string::npos;
  }

  TextureMeta texMeta_(const std::string &connPath);

  static void addTextureKey_(const std::string &role,
                             const std::string &path,
                             std::set<std::string> *keys) {
    if (!keys || path.empty()) return;
    keys->insert(role + ":" + normTexKey_(path));
  }

  static void appendMaterialKey_(const MaterialRecord &m,
                                 std::ostringstream *ss);

  static bool populateHairMaterial_(const lightusd::next::UsdPrim &prim,
                                    MaterialRecord *rec);

  bool ensureRenderMaterial_(const lightusd::next::UsdPrim &mat);

  bool requiresRenderMaterial_(const lightusd::next::UsdPrim &mat) const;

  std::string materialSourceIdentity_(
      const lightusd::next::UsdPrim &mat) const;

  MaterialRecord materialRecordForPrim_(
      const lightusd::next::UsdPrim &mat);

  int32_t registerMaterial_(const lightusd::next::UsdPrim &mat);

  int32_t materialIdForBoundPrim_(const lightusd::next::UsdPrim &prim) {
    lightusd::next::UsdPrim mat = lightusd::next::GetBoundMaterial(stage_, prim);
    return registerMaterial_(mat);
  }

  static bool hasGeomSubset_(const lightusd::next::UsdPrim &prim);

  static bool sameMatrix_(const std::array<double, 16> &a,
                          const std::array<double, 16> &b);

  static std::string matrixKey_(const std::array<double, 16> &m);

  static void transformPoint_(const std::array<double, 16> &m,
                              float *x, float *y, float *z);

  static void transformNormal_(const std::array<double, 16> &m,
                               float *x, float *y, float *z);

  struct MergeAccumulator {
    OutputMesh mesh;
    size_t source_count = 0;
    int first_source_index = -1;
  };

  static size_t triangleIndexCount_(const std::vector<uint32_t> &indices,
                                    const std::vector<float> &points) {
    return indices.empty() ? points.size() / 3 : indices.size();
  }

  void flushAccumulator_(MergeAccumulator *acc);

  bool appendToAccumulator_(const lightusd::next::UsdPrim &prim,
                            int source_index,
                            int32_t material_id,
                            bool double_sided,
                            bool soup,
                            MergeAccumulator *acc);

  void buildOptimizedOutputs_();

  // Resolve a UsdUVTexture connection path ("/.../Tex.outputs:rgb") to its
  // inputs:file asset path, which the JS caller maps to an archive texture entry.
  std::string texFile_(const std::string &connPath);

  // Triangulate faceVertexIndices grouped by faceVertexCounts. Quads use the
  // shorter diagonal, matching the full render converter; larger polygons
  // retain the bounded fan fallback used by the mesh-only fast path.
  template <typename FloatArray, typename IndexArray, typename CountArray>
  static void triangulate_(const FloatArray &points,
                           const IndexArray &fvi,
                           const CountArray &fvc,
                           std::vector<uint32_t> &out) {
    out.clear();
    if (fvi.empty()) return;
    if (fvc.empty()) {  // assume an already-triangulated index list
      out.reserve(fvi.size());
      for (int32_t v : fvi) {
        if (v >= 0) out.push_back(static_cast<uint32_t>(v));
      }
      return;
    }
    size_t base = 0;
    auto faceSpanAvailable = [](size_t base, int32_t n, size_t total) {
      if (n < 3) return false;
      const size_t count = static_cast<size_t>(n);
      return base <= total && count <= total - base;
    };
    auto advanceFaceBase = [](size_t base, int32_t n) {
      if (n <= 0) return base;
      const size_t add = static_cast<size_t>(n);
      if (base > (std::numeric_limits<size_t>::max)() - add) {
        return (std::numeric_limits<size_t>::max)();
      }
      return base + add;
    };
    auto quadUsesDiagonal13 = [&](size_t base) {
      if (base > fvi.size() || 4 > fvi.size() - base) return false;
      const int32_t ids[4] = {fvi[base], fvi[base + 1],
                              fvi[base + 2], fvi[base + 3]};
      const size_t point_count = points.size() / 3;
      for (int32_t id : ids) {
        if (id < 0 || static_cast<size_t>(id) >= point_count) return false;
      }
      auto distSq = [&](int32_t a, int32_t b) {
        const size_t ia = static_cast<size_t>(a) * 3;
        const size_t ib = static_cast<size_t>(b) * 3;
        const float dx = points[ia] - points[ib];
        const float dy = points[ia + 1] - points[ib + 1];
        const float dz = points[ia + 2] - points[ib + 2];
        return dx * dx + dy * dy + dz * dz;
      };
      return distSq(ids[1], ids[3]) < distSq(ids[0], ids[2]);
    };
    for (int32_t n : fvc) {
      if (!faceSpanAvailable(base, n, fvi.size())) {
        base = advanceFaceBase(base, n);
        continue;
      }
      if (n == 4 && quadUsesDiagonal13(base)) {
        const size_t corners[6] = {base, base + 1, base + 3,
                                   base + 1, base + 2, base + 3};
        for (size_t corner : corners) {
          out.push_back(static_cast<uint32_t>(fvi[corner]));
        }
      } else for (int32_t k = 2; k < n; ++k) {
        const int32_t a = fvi[base];
        const int32_t b = fvi[base + static_cast<size_t>(k) - 1];
        const int32_t c = fvi[base + static_cast<size_t>(k)];
        if (a < 0 || b < 0 || c < 0) continue;
        out.push_back(static_cast<uint32_t>(a));
        out.push_back(static_cast<uint32_t>(b));
        out.push_back(static_cast<uint32_t>(c));
      }
      base = advanceFaceBase(base, n);
    }
  }

  static std::vector<uint32_t> faceTriangleStarts_(
      const std::vector<int32_t> &fvc);

  static std::vector<int32_t> matIntStatic_(
      const lightusd::next::UsdPrim &prim, const char *name);

  // Area-weighted vertex normals from the triangulated indices.
  static void computeNormals_(const std::vector<float> &pos,
                              const std::vector<uint32_t> &idx,
                              std::vector<float> &out);

  emscripten::val heapF_(const std::vector<float> &v, int comps) const;
  emscripten::val heapU32_(const std::vector<uint32_t> &v) const;
  emscripten::val heapU16_(const std::vector<uint16_t> &v) const;
  static emscripten::val arr3_(const float *c);
  static emscripten::val matArray_(const std::array<double, 16> &m) {
    emscripten::val a = emscripten::val::array();
    for (double v : m) a.call<void>("push", v);
    return a;
  }
  static std::array<double, 16> identityMatrix_();
  static std::array<double, 16> multiplyMatrix_(
      const std::array<double, 16> &a, const std::array<double, 16> &b);
  void buildMeshTransformCaches_();
  std::array<double, 16> localMatrix_(
      const lightusd::next::UsdPrim &prim) const;

  std::array<double, 16> worldMatrix_(
      const lightusd::next::UsdPrim &prim) const;

  // World transform for a prim, preferring the RenderScene node table: its
  // hierarchy traversal handles native instances correctly, while the plain
  // GetParent() chain in worldMatrix_ drops the instance root's own xform.
  std::array<double, 16> worldMatrixForPrim_(
      const lightusd::next::UsdPrim &prim) const;

  emscripten::val materialObjectForPrim_(
      const lightusd::next::UsdPrim &mat);

  emscripten::val materialObject_(int32_t material_id) const;

  // Resolve the prim's bound material to UsdPreviewSurface values + texture
  // asset paths (resolved to GPU textures by the JS caller from the archive).
  emscripten::val resolveMaterial_(const lightusd::next::UsdPrim &prim) {
    lightusd::next::UsdPrim mat = lightusd::next::GetBoundMaterial(stage_, prim);
    return materialObjectForPrim_(mat);
  }

  void addGeomSubsetMaterials_(const lightusd::next::UsdPrim &prim,
                               emscripten::val &out);

  lightusd::next::Stage stage_;
  tr::RenderScene render_scene_;
  bool render_scene_valid_ = false;
  std::vector<std::string> render_scene_warnings_;
  std::vector<lightusd::next::UsdGeomMesh> meshes_;
  std::vector<OutputMesh> outputs_;
  std::vector<OutputMesh> analytic_outputs_;
  std::vector<MaterialRecord> materials_;
  std::unordered_map<std::string, int32_t> material_key_to_id_;
  std::unordered_map<std::string, int32_t> material_path_to_id_;
  std::unordered_map<std::string, int32_t> material_identity_to_id_;
  mutable std::unordered_map<std::string, std::array<double, 16>>
      local_matrix_cache_;
  mutable std::unordered_map<std::string, std::array<double, 16>>
      world_matrix_cache_;
  std::set<std::string> source_material_keys_;
  std::set<std::string> source_texture_keys_;
  std::set<std::string> texture_keys_;
  std::map<std::string, std::string> clip_assets_;
  struct VariantSetInfo {
    std::string prim_path;
    std::string set_name;
    std::string selected;
    std::vector<std::string> variant_names;
  };

  void collectVariantSets_();

  std::vector<VariantSetInfo> variant_sets_;
  std::map<std::string, std::string> variant_overrides_;

  Stats stats_;
  double pending_input_copy_ms_ = 0.0;
  size_t pending_input_bytes_ = 0;
  bool loaded_ = false;
  bool material_dedup_ = false;
  bool mesh_merge_ = false;
  bool mesh_merge_bake_transform_ = false;
  bool flatten_render_tree_ = false;
  bool mesh_only_ = false;
  bool compute_tangents_ = false;
  bool build_vertex_indices_ = true;
  bool build_vertex_indices_set_ = false;
  std::string tangent_method_ = "hybrid";
  std::string render_settings_path_;
  std::string error_;
  std::vector<float> s_points_, s_normals_, s_uv_, s_tangents_;
  std::vector<float> s_points_cloud_points_, s_points_cloud_widths_;
  std::vector<float> s_points_cloud_colors_;
  std::vector<float> s_curve_points_, s_curve_widths_, s_curve_colors_;
  std::vector<float> s_curve_tessellated_points_;
  std::vector<float> s_curve_tessellated_widths_;
  std::vector<float> s_curve_tessellated_colors_;
  std::vector<uint32_t> s_indices_;
  std::vector<uint32_t> s_point_source_indices_;
  std::vector<uint16_t> s_joint_indices_;
  std::vector<float> s_joint_weights_;
};
}  // namespace web_next
}  // namespace lightusd
