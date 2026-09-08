// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Tydra Next - Mesh conversion orchestration
#include "render-converter.hh"
#include "safe-arithmetic.hh"
#include <cstring>
#include "next/schema/usd-skel.hh"
#include "tsd/tinysubdiv.hh"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
namespace lightusd { namespace tydra { namespace next {
namespace {
size_t SaturatingMul(size_t a, size_t b) { return a && b > std::numeric_limits<size_t>::max() / a ? std::numeric_limits<size_t>::max() : a * b; }
bool GetToken(const UsdPrim& prim, const char* name, std::string* out) { const Value* v=prim.GetPropertyValue(name); if (!v || !out) return false; if (const std::string* x=v->as_token()) {*out=*x; return true;} if (const std::string* x=v->as_string()) {*out=*x; return true;} return false; }
void ComputePointBounds(const FloatChunked& p, Float3* lo, Float3* hi, bool* has) { if (!lo || !hi || !has || p.size()<3) return; *lo=Float3(1e30f,1e30f,1e30f); *hi=Float3(-1e30f,-1e30f,-1e30f); for(size_t i=0;i<p.size()/3;++i){ const float x=p[i*3],y=p[i*3+1],z=p[i*3+2]; lo->x=std::min(lo->x,x);lo->y=std::min(lo->y,y);lo->z=std::min(lo->z,z);hi->x=std::max(hi->x,x);hi->y=std::max(hi->y,y);hi->z=std::max(hi->z,z);} *has=true; }
}  // namespace

bool RenderSceneConverter::ConvertMesh(const Stage& stage, const UsdPrim& prim, RenderMesh* out) {
  if (!out || !IsMesh(prim)) {
    SetLastError("Invalid mesh prim");
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();

  // Extract topology
  if (!ExtractMeshTopology(prim, out)) {
    return false;
  }

  // Extract geometry
  if (!ExtractMeshGeometry(prim, out)) {
    return false;
  }

  // Sanitize topology BEFORE any consumer walks it: negative / out-of-range
  // faceVertexIndices previously flowed into normal generation and the output
  // buffers (a negative index casts to ~4 billion -> segfault; an OOB index
  // hands the renderer an out-of-bounds read). Drop offending faces with a
  // warning; also truncate a counts list that overruns the index buffer.
  SanitizeMeshTopology(out);

  // Extract render vertex attributes only when the caller retains geometry.
  // Metadata-only consumers source these arrays elsewhere; decoding/copying
  // large face-varying normals and UVs here would be pure transient overhead.
  if (config_.mesh.retain_geometry) {
    ExtractMeshPrimvars(prim, out);
  }

  // Uniformly pre-tessellate authored subdivision surfaces before building
  // skinning/blend-shape payloads and before triangulation. The next viewer
  // path used to ignore the same subdivision options honored by legacy Tydra,
  // leaving Catmull-Clark assets visibly faceted in every RT backend.
  int subdivision_level = config_.mesh.subdivision_level;
  const auto level_it =
      config_.mesh.subdivision_prim_levels.find(out->prim_path);
  if (level_it != config_.mesh.subdivision_prim_levels.end()) {
    subdivision_level = level_it->second;
  }
  std::string subdivision_scheme;
  const bool has_authored_subdivision =
      GetToken(prim, "subdivisionScheme", &subdivision_scheme);
  SkinBindingInfo subdivision_skin;
  const bool subdivision_deformed =
      GetSkinBinding(prim, &subdivision_skin) || !GetBlendShapes(prim).empty();
  if (subdivision_level > 0 && has_authored_subdivision &&
      subdivision_scheme != "none" &&
      config_.mesh.retain_geometry && !subdivision_deformed) {
    ::lightusd::tsd::Options subdiv_options;
    subdiv_options.level = std::min(subdivision_level,
                                    ::lightusd::tsd::kMaxLevel);
    if (subdivision_scheme == "loop") {
      subdiv_options.scheme = ::lightusd::tsd::Scheme::Loop;
    } else if (subdivision_scheme == "bilinear") {
      subdiv_options.scheme = ::lightusd::tsd::Scheme::Bilinear;
    } else {
      subdiv_options.scheme = ::lightusd::tsd::Scheme::CatmullClark;
    }

    const std::vector<float> points = out->points.flatten();
    const std::vector<uint32_t> counts = out->face_vertex_counts.flatten();
    const std::vector<uint32_t> indices = out->face_vertex_indices.flatten();
    ::lightusd::tsd::MeshView mesh_view;
    mesh_view.points = points.data();
    mesh_view.num_points = static_cast<uint32_t>(points.size() / 3);
    mesh_view.face_vertex_counts = counts.data();
    mesh_view.num_faces = static_cast<uint32_t>(counts.size());
    mesh_view.face_vertex_indices = indices.data();
    mesh_view.num_face_vertex_indices = static_cast<uint32_t>(indices.size());

    struct RefinedChannel {
      FloatChunked* data;
      Interpolation* interpolation;
      uint32_t stride;
      bool face_varying;
    };
    std::vector<RefinedChannel> channels;
    auto add_channel = [&](FloatChunked* data, Interpolation* interpolation,
                           uint32_t stride) {
      if (!data->empty() && (*interpolation == Interpolation::FaceVarying ||
                             *interpolation == Interpolation::Vertex ||
                             *interpolation == Interpolation::Varying)) {
        channels.push_back(
            RefinedChannel{data, interpolation, stride,
                           *interpolation == Interpolation::FaceVarying});
      }
    };
    add_channel(&out->texcoords_0, &out->texcoords_0_interp, 2);
    add_channel(&out->texcoords_1, &out->texcoords_1_interp, 2);
    const uint32_t color_stride =
        out->colors.size() == out->point_count() * 4 ||
                out->colors.size() == indices.size() * 4
            ? 4u
            : 3u;
    add_channel(&out->colors, &out->colors_interp, color_stride);
    add_channel(&out->opacities, &out->opacities_interp, 1);

    std::vector<std::vector<float>> channel_values(channels.size());
    std::vector<::lightusd::tsd::FVarChannelView> fvar;
    std::vector<::lightusd::tsd::VertexPrimvarView> vertex;
    std::vector<size_t> fvar_channel;
    std::vector<size_t> vertex_channel;
    for (size_t i = 0; i < channels.size(); ++i) {
      channel_values[i] = channels[i].data->flatten();
      if (channels[i].face_varying) {
        ::lightusd::tsd::FVarChannelView view;
        view.values = channel_values[i].data();
        view.num_values = static_cast<uint32_t>(channel_values[i].size() /
                                                channels[i].stride);
        view.stride = channels[i].stride;
        fvar.push_back(view);
        fvar_channel.push_back(i);
      } else {
        ::lightusd::tsd::VertexPrimvarView view;
        view.values = channel_values[i].data();
        view.stride = channels[i].stride;
        view.varying = *channels[i].interpolation == Interpolation::Varying;
        vertex.push_back(view);
        vertex_channel.push_back(i);
      }
    }

    ::lightusd::tsd::RefinedMesh refined;
    std::string subdivision_error;
    const ::lightusd::tsd::Result result = ::lightusd::tsd::Refine(
        mesh_view, fvar.empty() ? nullptr : fvar.data(),
        static_cast<uint32_t>(fvar.size()),
        vertex.empty() ? nullptr : vertex.data(),
        static_cast<uint32_t>(vertex.size()), subdiv_options, &refined,
        &subdivision_error);
    if (result == ::lightusd::tsd::Result::Success) {
      out->subdivision_face_source = std::move(refined.face_source);
      // Uniform primvars are one value per authored face. They are not vertex
      // or face-varying subdivision channels, but every refined child face
      // must inherit its level-0 face's value. Leaving the original short
      // array in place made consumers index it with refined face ids; misses
      // read as zero (notably turning indexed displayOpacity=1 into invisible
      // patches on subdivided character meshes).
      auto refine_uniform = [&](FloatChunked* data, Interpolation interp,
                                uint32_t stride) {
        if (!data || data->empty() || interp != Interpolation::Uniform ||
            stride == 0 || data->size() != counts.size() * stride) {
          return;
        }
        const std::vector<float> authored = data->flatten();
        std::vector<float> expanded;
        expanded.reserve(out->subdivision_face_source.size() * stride);
        for (uint32_t source_face : out->subdivision_face_source) {
          if (source_face >= counts.size()) return;
          const size_t begin = static_cast<size_t>(source_face) * stride;
          expanded.insert(expanded.end(), authored.begin() + begin,
                          authored.begin() + begin + stride);
        }
        data->clear();
        data->append(expanded.data(), expanded.size());
      };
      refine_uniform(&out->texcoords_0, out->texcoords_0_interp, 2);
      refine_uniform(&out->texcoords_1, out->texcoords_1_interp, 2);
      refine_uniform(&out->colors, out->colors_interp, color_stride);
      refine_uniform(&out->opacities, out->opacities_interp, 1);
      out->points.clear();
      out->points.append(refined.points.data(), refined.points.size());
      out->face_vertex_counts.clear();
      out->face_vertex_counts.append(refined.face_vertex_counts.data(),
                                     refined.face_vertex_counts.size());
      out->face_vertex_indices.clear();
      out->face_vertex_indices.append(refined.face_vertex_indices.data(),
                                      refined.face_vertex_indices.size());
      for (size_t i = 0; i < refined.fvar.size(); ++i) {
        RefinedChannel& channel = channels[fvar_channel[i]];
        channel.data->clear();
        channel.data->append(refined.fvar[i].data(), refined.fvar[i].size());
        *channel.interpolation = Interpolation::FaceVarying;
      }
      for (size_t i = 0; i < refined.vertex_primvars.size(); ++i) {
        RefinedChannel& channel = channels[vertex_channel[i]];
        channel.data->clear();
        channel.data->append(refined.vertex_primvars[i].data(),
                             refined.vertex_primvars[i].size());
      }
      out->normals.clear();
      out->tangents.clear();
      out->triangulated_indices.clear();
      out->triangulated_face_vertex_indices.clear();
      out->face_triangle_offsets.clear();
      out->is_triangulated = false;
      ComputePointBounds(out->points, &out->bbox_min, &out->bbox_max,
                         &out->has_bbox);
    } else {
      AddWarning("Subdivision failed for '" + out->prim_path + "': " +
                 subdivision_error);
    }
  } else if (subdivision_level > 0 && has_authored_subdivision &&
             subdivision_scheme != "none" &&
             subdivision_deformed) {
    AddWarning("Subdivision skipped for deformed mesh '" + out->prim_path +
               "' on the next path; use --no-next for refined skin and "
               "blend-shape primvars");
  }

  // Skinning binding (skel:skeleton + skel:jointIndices/Weights primvars).
  {
    SkinBindingInfo sb;
    const bool has_skin_binding = GetSkinBinding(prim, &sb);
    if (has_skin_binding && !sb.joint_indices.empty() &&
        sb.joint_indices.size() != sb.joint_weights.size()) {
      // e.g. one indexed skin primvar expanded while its pair stayed
      // authored (malformed :indices). Skipping silently leaves the mesh in
      // bind pose with no hint why.
      AddWarning("Mismatched skin jointIndices/jointWeights sizes on " +
                          prim.GetPath().str() + "; mesh renders unskinned");
    }
    if (has_skin_binding && !sb.joint_indices.empty() &&
        sb.joint_indices.size() == sb.joint_weights.size()) {
      // UsdSkel binding inheritance: `skel:skeleton` may be authored on an
      // ancestor (typically the enclosing SkelRoot) rather than on the mesh
      // itself, in which case every descendant skinnable prim inherits it.
      // GetSkinBinding only reads the mesh prim, so walk up to the nearest
      // ancestor binding when the mesh doesn't author one directly. Without
      // this, meshes that bind their skeleton at the SkelRoot (e.g. the
      // MetaHuman standalone face/body exports) resolve no skeleton_id and
      // render unskinned in bind pose.
      if (sb.skeleton_path.empty()) {
        UsdPrim anc = GetParent(stage, prim);
        UsdPrim skel_root;
        while (anc.IsValid()) {
          std::string inherited = GetBoundSkeleton(anc);
          if (!inherited.empty()) {
            sb.skeleton_path = inherited;
            break;
          }
          // Binding inheritance is scoped to the SkelRoot subtree.
          if (::lightusd::tydra::next::IsSkelRoot(anc)) {
            skel_root = anc;
            break;
          }
          anc = GetParent(stage, anc);
        }
        // No authored binding anywhere: fall back to a Skeleton contained
        // in the enclosing SkelRoot (common Blender / older-exporter shape;
        // legacy tydra binds this way too). First Skeleton in the subtree
        // wins, matching legacy.
        if (sb.skeleton_path.empty() && skel_root.IsValid()) {
          for (const UsdPrim& desc : GetDescendants(skel_root)) {
            if (::lightusd::tydra::next::IsSkeleton(desc)) {
              sb.skeleton_path = desc.GetPath().str();
              break;
            }
          }
        }
      }
      const size_t point_count = out->point_count();
      size_t influences = sb.influences_per_vertex > 0
                              ? static_cast<size_t>(sb.influences_per_vertex)
                              : 0;
      if (influences == 0 && point_count > 0 &&
          (sb.joint_indices.size() % point_count) == 0) {
        influences = sb.joint_indices.size() / point_count;
      }
      size_t expected_influence_count = 0;
      const bool influence_count_valid =
          safe::mul(point_count, influences, &expected_influence_count);
      if (influences > 0 && point_count > 1 &&
          influence_count_valid &&
          sb.joint_indices.size() == influences) {
        // Move the authored tuple out while constructing the expanded form.
        // Copying it first kept three full influence buffers resident (the
        // authored arrays, their copies, and the expanded result).
        std::vector<int32_t> indices = std::move(sb.joint_indices);
        std::vector<float> weights = std::move(sb.joint_weights);
        sb.joint_indices.reserve(expected_influence_count);
        sb.joint_weights.reserve(expected_influence_count);
        for (size_t point = 0; point < point_count; ++point) {
          sb.joint_indices.insert(sb.joint_indices.end(), indices.begin(),
                                  indices.end());
          sb.joint_weights.insert(sb.joint_weights.end(), weights.begin(),
                                  weights.end());
        }
      }
      if (influences == 0 || point_count == 0 ||
          !influence_count_valid ||
          sb.joint_indices.size() != expected_influence_count) {
        AddWarning("Ignoring malformed skin influences on " +
                            prim.GetPath().str());
      } else {
        size_t output_influences = influences;
        std::vector<int32_t> reduced_indices;
        std::vector<float> reduced_weights;
        size_t reduced_count = 0;
        size_t reduced_bytes = 0;
        const bool reduction_size_valid =
            safe::mul(point_count,
                      static_cast<size_t>(config_.mesh.target_bone_count),
                      &reduced_count) &&
            safe::mul(reduced_count, size_t{8}, &reduced_bytes);
        if (config_.mesh.enable_bone_reduction &&
            config_.mesh.target_bone_count > 0 &&
            config_.mesh.target_bone_count < influences &&
            // ~8B per point-influence pair of temporaries; on a nearly-full
            // heap keep the authored influences instead of abort()ing.
            reduction_size_valid && ProbeAlloc(reduced_bytes)) {
          output_influences = config_.mesh.target_bone_count;
          reduced_indices.resize(reduced_count, 0);
          reduced_weights.resize(reduced_count, 0.0f);
          std::vector<std::pair<float, int32_t>> ranked(influences);
          for (size_t point = 0; point < point_count; ++point) {
            const size_t source = point * influences;
            for (size_t i = 0; i < influences; ++i) {
              ranked[i] = {sb.joint_weights[source + i],
                           sb.joint_indices[source + i]};
            }
            std::stable_sort(
                ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
            float sum = 0.0f;
            for (size_t i = 0; i < output_influences; ++i) {
              sum += std::max(0.0f, ranked[i].first);
            }
            for (size_t i = 0; i < output_influences; ++i) {
              const size_t destination = point * output_influences + i;
              reduced_indices[destination] = ranked[i].second;
              reduced_weights[destination] =
                  sum > 0.0f ? std::max(0.0f, ranked[i].first) / sum
                             : (i == 0 ? 1.0f : 0.0f);
            }
          }
          sb.joint_indices = std::move(reduced_indices);
          sb.joint_weights = std::move(reduced_weights);
        }

      // Cumulative budget guard: ProbeAlloc above only bounds the transient
      // bone-reduction scratch for THIS mesh; a scene of many meshes each
      // authoring dense-but-individually-small skin influences can still
      // sum the tracked joint_indices/joint_weights buffers past the cap
      // without any single mesh tripping a per-call probe.
      if (BudgetWouldExceed(
              SaturatingMul(sb.joint_indices.size(), sizeof(int32_t) +
                                                          sizeof(float)),
              "skin binding decode")) {
        AddWarning("Memory budget reached; skipping skin binding for " +
                            prim.GetPath().str());
      } else {
      out->skin = std::make_unique<RenderMesh::SkinBinding>();
      out->skin->joint_indices.reserve(sb.joint_indices.size());
      for (int32_t ji : sb.joint_indices) {
        out->skin->joint_indices.push_back(
            ji < 0 ? uint16_t(0)
                   : static_cast<uint16_t>(std::min<int32_t>(ji, 65535)));
      }
      out->skin->joint_weights.append(sb.joint_weights.data(),
                                      sb.joint_weights.size());
      out->skin->influences_per_vertex =
          static_cast<uint32_t>(output_influences);
      out->skin->mesh_joint_order = std::move(sb.joint_order);
      std::memcpy(out->skin->geom_bind_transform.m, sb.geom_bind_transform,
                  sizeof(sb.geom_bind_transform));
      // skeleton_id is resolved by the caller once skeletons are converted
      // (stored in skin->skeleton_id via the path recorded here).
      out->skin->skeleton_path = sb.skeleton_path;
      }
      }
    }
  }

  // Blend shapes (skel:blendShapes names + skel:blendShapeTargets prims).
  for (const BlendShapeInfo& bs : GetBlendShapes(prim)) {
    UsdPrim bs_prim = stage.GetPrimAtPath(bs.path);
    if (!bs_prim.IsValid()) continue;
    ::lightusd::next::BlendShapeData bd;
    if (!::lightusd::next::GetBlendShapeData(stage, bs_prim, &bd)) continue;
    if (bd.offsets.empty()) continue;
    // Cumulative budget guard: a scene with many blend-shape targets (or
    // dense per-vertex offsets on a single target) can sum well past the cap
    // one target at a time, each individually too small to trip a per-call
    // probe. Estimate offsets + normalOffsets + every in-between's offsets.
    {
      size_t bs_floats = bd.offsets.size() + bd.normalOffsets.size();
      for (const auto& ib : bd.inbetweens) bs_floats += ib.offsets.size();
      if (BudgetWouldExceed(SaturatingMul(bs_floats, sizeof(float)),
                            "blend shape decode")) {
        AddWarning("Memory budget reached; skipping blend shape '" +
                            bs_prim.GetPath().str() + "'");
        continue;
      }
    }
    RenderMesh::BlendShape shape;
    shape.name = bs.name.empty() ? bs_prim.GetName() : bs.name;

    // Sparse targets: offsets[k] (and normalOffsets[k] / in-between
    // offsets[k]) apply to point pointIndices[k]. An out-of-range index must
    // drop the WHOLE parallel entry — dropping it from point_indices alone
    // (the previous behavior) misaligned every remaining offset.
    std::vector<size_t> kept;  // kept entry positions (sparse targets only)
    const size_t authored_entries = bd.offsets.size() / 3;
    if (bd.hasPointIndices) {
      const size_t npts = out->point_count();
      const size_t nentries =
          std::min(bd.pointIndices.size(), authored_entries);
      kept.reserve(nentries);
      for (size_t k = 0; k < nentries; ++k) {
        const int32_t pi = bd.pointIndices[k];
        if (pi >= 0 && static_cast<size_t>(pi) < npts) {
          kept.push_back(k);
        }
      }
      if (kept.size() != bd.pointIndices.size()) {
        AddWarning("BlendShape '" + bs_prim.GetPath().str() +
                            "': dropped out-of-range pointIndices entries "
                            "(with their parallel offsets)");
      }
    }

    // Copy 3-float entries at the kept positions of a parallel array.
    auto append_kept = [&kept](const std::vector<float>& src,
                               FloatChunked* dst) {
      for (size_t k : kept) {
        dst->push_back(src[k * 3 + 0]);
        dst->push_back(src[k * 3 + 1]);
        dst->push_back(src[k * 3 + 2]);
      }
    };

    if (bd.hasPointIndices) {
      append_kept(bd.offsets, &shape.point_offsets);
      if (bd.hasNormalOffsets &&
          bd.normalOffsets.size() == bd.offsets.size()) {
        append_kept(bd.normalOffsets, &shape.normal_offsets);
      }
      shape.point_indices.reserve(kept.size());
      for (size_t k : kept) {
        shape.point_indices.push_back(
            static_cast<uint32_t>(bd.pointIndices[k]));
      }
    } else {
      shape.point_offsets.append(bd.offsets.data(), bd.offsets.size());
      if (bd.hasNormalOffsets && !bd.normalOffsets.empty()) {
        shape.normal_offsets.append(bd.normalOffsets.data(),
                                    bd.normalOffsets.size());
      }
    }
    for (const ::lightusd::next::BlendShapeData::Inbetween& source :
         bd.inbetweens) {
      if (source.offsets.size() != bd.offsets.size()) {
        AddWarning("Ignoring malformed in-between '" + source.name +
                            "' on " + bs_prim.GetPath().str());
        continue;
      }
      if (!source.has_weight) {
        // A weightless in-between would sit at 0.0 and collide with the base
        // shape; legacy tydra skips these too.
        AddWarning("In-between '" + source.name + "' on " +
                            bs_prim.GetPath().str() +
                            " has no authored weight; skipped");
        continue;
      }
      RenderMesh::BlendShape::Inbetween inbetween;
      inbetween.name = source.name;
      inbetween.weight = source.weight;
      if (bd.hasPointIndices) {
        append_kept(source.offsets, &inbetween.point_offsets);
      } else {
        inbetween.point_offsets.append(source.offsets.data(),
                                       source.offsets.size());
      }
      shape.inbetweens.push_back(std::move(inbetween));
    }
    out->blend_shapes.push_back(std::move(shape));
  }

  // Triangulate if requested. A mesh whose faces were all sanitized away is
  // still a valid (empty) render mesh; only meshes with real topology that
  // cannot be triangulated (e.g. over the temp-allocation budget) are dropped.
  if ((config_.mesh.retain_geometry || config_.mesh.retain_triangulation) &&
      config_.mesh.triangulate &&
      !out->is_triangulated) {
    if (!TriangulateMesh(out) && !out->face_vertex_counts.empty()) {
      AddWarning("Failed to triangulate mesh '" + out->prim_path +
                          "'; skipping it to avoid conversion abort");
      return false;
    }
  }

  // Compute normals if needed
  if (config_.mesh.retain_geometry && config_.mesh.compute_normals &&
      out->normals.empty()) {
    ComputeVertexNormals(out);
  }

  // Compute tangents if requested (needs triangles, per-vertex normals and
  // per-vertex UVs).
  if (config_.mesh.retain_geometry && config_.mesh.compute_tangents &&
      out->tangents.empty()) {
    ComputeVertexTangents(out);
  }

  // A chunk allocation may have failed anywhere above (nothrow growth): the
  // mesh data is truncated, so report and drop the prim instead of rendering
  // partial geometry (or aborting the module, as a throwing new would under
  // -fno-exceptions).
  if (out->has_alloc_failure()) {
    AddWarning("Out of memory converting mesh '" + out->prim_path +
                        "'; the prim was skipped");
    return false;
  }

  return true;
}

// Tangent frame from triangulated topology. Lengyel keeps the compact
// per-vertex path. MikkTSpace-style methods expand to face corners first so
// UV seams and mirrored islands are not averaged through shared point indices.

} } }  // namespace lightusd::tydra::next
