// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "next/crate/crate-writer.hh"
#include "next/layer/prim-spec.hh"
#include "scene-preview-internal.hh"
#include "tinyvdb_io.h"
#include "tinyvdb_sparse_tree.h"

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
tvdb_grid_t Template(double size, const double* origin,
                     tvdb_value_type_t type) {
  tvdb_grid_t grid{};
  grid.transform.type = TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE;
  for (int k = 0; k < 3; ++k) {
    grid.transform.voxel_size[k] = size;
    grid.transform.scale_values[k] = size;
    grid.transform.translation[k] = origin[k];
    grid.transform.matrix[k][k] = size;
    grid.transform.matrix[k][3] = origin[k];
  }
  grid.transform.matrix[3][3] = 1;
  grid.tree.layout.num_levels = 4;
  for (int k = 0; k < 4; ++k) {
    grid.tree.layout.levels[k].node_type = k == 0   ? TVDB_NODE_ROOT
                                           : k == 3 ? TVDB_NODE_LEAF
                                                    : TVDB_NODE_INTERNAL;
    grid.tree.layout.levels[k].value_type = type;
    grid.tree.layout.levels[k].log2dim = k == 1   ? 5
                                         : k == 2 ? 4
                                         : k == 3 ? 3
                                                  : 0;
  }
  return grid;
}
}  // namespace
static bool TryWriteVolume(Build& build, usd::Layer& root,
                           const std::vector<Sample>& samples) {
  // Small independent bricks keep dense GPU upload bounded; empty world space
  // never becomes a scene-wide dense volume. Geometry sampling and output share
  // one budget, including the three overlapping device/upload copies.
  constexpr int side = 32, halo = 3, dim = side + halo * 2;
  constexpr uint64_t brick_cost = uint64_t(dim) * dim * dim * 16 * 3;
  const uint64_t reserve =
      std::min(build.opts.gpu_budget_bytes / 2, uint64_t(512) << 20);
  const size_t limit =
      static_cast<size_t>((build.opts.gpu_budget_bytes - reserve) / brick_cost);
  if (!limit) {
    build.result.error = "volume preview budget cannot hold one brick";
    return false;
  }
  double cell = build.cell_size;
  std::map<std::array<int64_t, 3>, std::vector<size_t>> bricks;
  for (;;) {
    bricks.clear();
    for (size_t i = 0; i < samples.size(); ++i) {
      std::array<int64_t, 3> key{};
      for (int k = 0; k < 3; ++k)
        key[k] =
            static_cast<int64_t>(std::floor(samples[i].p[k] / (cell * side)));
      bricks[key].push_back(i);
    }
    if (bricks.size() <= limit) break;
    if (build.opts.resolution) {
      build.result.error =
          "volume preview resolution exceeds the GPU brick budget";
      return false;
    }
    cell *= 2;
  }
  usd::LayerBuilder builder(root);
  builder.begin_prim("ScenePreview", "Xform");
  // Existing renderers shade level sets as surfaces. Cache the same bounded
  // per-brick surface with baked albedo so GPU consumers need no original
  // geometry, dense scene-wide grid, or per-frame VDB reconstruction.
  usd::Layer renderRoot;
  renderRoot.meta() = root.meta();
  usd::LayerBuilder renderBuilder(renderRoot);
  renderBuilder.begin_prim("ScenePreview", "Xform");
  uint64_t surface_bytes = 0;
  size_t index = 0;
  for (const auto& brick : bricks) {
    if (!build.check("volume", index)) return false;
    double origin[3];
    for (int k = 0; k < 3; ++k)
      origin[k] = (brick.first[k] * side - halo) * cell;
    const size_t count = size_t(dim) * dim * dim;
    std::vector<float> sdf(count, static_cast<float>(3 * cell));
    std::vector<float> albedo(count * 3, 0.5f);
    // Rasterize oriented surflet discs, then use their finite-thickness shell
    // distance. This remains meaningful for open terrain and thin walls.
    for (size_t si : brick.second) {
      const Sample& s = samples[si];
      // Deterministic coverage preserves fractional opacity in opaque shells.
      const double coverage =
          double(Hash(std::to_string(si)) >> 11) / 9007199254740992.0;
      if (coverage >= std::max(0.f, std::min(1.f, s.opacity))) continue;
      double center[3];
      for (int k = 0; k < 3; ++k) center[k] = (s.p[k] - origin[k]) / cell;
      const double radius =
          std::max(cell, std::min(double(s.radius), cell * 4));
      const int extent = static_cast<int>(std::ceil(radius / cell)) + halo;
      const int cx = static_cast<int>(std::floor(center[0])),
                cy = static_cast<int>(std::floor(center[1])),
                cz = static_cast<int>(std::floor(center[2]));
      for (int z = std::max(0, cz - extent); z < std::min(dim, cz + extent + 1);
           ++z)
        for (int y = std::max(0, cy - extent);
             y < std::min(dim, cy + extent + 1); ++y)
          for (int x = std::max(0, cx - extent);
               x < std::min(dim, cx + extent + 1); ++x) {
            const double d[] = {x - center[0], y - center[1], z - center[2]};
            const double plane = d[0] * s.n[0] + d[1] * s.n[1] + d[2] * s.n[2];
            const double radial = std::sqrt(std::max(
                0.0, d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - plane * plane));
            const double q0 = radial - radius / cell,
                         q1 = std::abs(plane) - .55;
            const double distance =
                (std::min(std::max(q0, q1), 0.0) +
                 std::hypot(std::max(q0, 0.0), std::max(q1, 0.0))) *
                cell;
            const size_t off =
                (size_t(z) * dim + static_cast<size_t>(y)) * dim +
                static_cast<size_t>(x);
            if (distance < sdf[off]) {
              sdf[off] = static_cast<float>(distance);
              for (int c = 0; c < 3; ++c)
                albedo[off * 3 + static_cast<size_t>(c)] = s.color[c];
            }
          }
    }
    std::vector<tvdb_vec3i> coords;
    std::vector<float> values, colors;
    coords.reserve(count);
    values.reserve(count);
    colors.reserve(count * 3);
    for (int z = 0; z < dim; ++z)
      for (int y = 0; y < dim; ++y)
        for (int x = 0; x < dim; ++x) {
          const size_t o = (size_t(z) * dim + static_cast<size_t>(y)) * dim +
                           static_cast<size_t>(x);
          if (sdf[o] >= static_cast<float>(3 * cell)) continue;
          coords.push_back({x, y, z});
          values.push_back(sdf[o]);
          colors.insert(colors.end(),
                        albedo.begin() + static_cast<ptrdiff_t>(o * 3),
                        albedo.begin() + static_cast<ptrdiff_t>(o * 3 + 3));
        }
    if (coords.empty()) continue;
    auto scalar = Template(cell, origin, TVDB_VALUE_FLOAT),
         color = Template(cell, origin, TVDB_VALUE_VEC3F);
    tvdb_grid_t grids[2]{};
    const float background = static_cast<float>(3 * cell),
                rgb[3] = {.5f, .5f, .5f};
    if (!tvdb_grid_from_sparse_typed_using_template(
            &scalar, coords.data(), values.data(), coords.size(),
            TVDB_VALUE_FLOAT, &background, "surface", &grids[0]) ||
        !tvdb_grid_from_sparse_vec3_using_template(&color, coords.data(),
                                                   colors.data(), coords.size(),
                                                   "albedo", rgb, &grids[1])) {
      for (auto& g : grids) tvdb_grid_destroy_owned(&g);
      build.result.error = "cannot construct sparse preview volume";
      return false;
    }
    tvdb_error_t error{};
    tvdb_grid_set_metadata_string(&grids[0], "class", "level set", &error);
    const std::string file = "volume_" + std::to_string(index) + ".vdb",
                      name = "Tile_" + std::to_string(index);
    tvdb_file_t vdb;
    tvdb_file_init_for_write(&vdb, nullptr);
    vdb.grids = grids;
    vdb.num_grids = 2;
    const auto status = tvdb_file_save(
        &vdb, (build.directory / file).string().c_str(),
        TVDB_COMPRESS_ZIP | TVDB_COMPRESS_ACTIVE_MASK, 1, 0, &error);
    for (auto& g : grids) tvdb_grid_destroy_owned(&g);
    if (status != TVDB_OK) {
      build.result.error = "cannot serialize preview volume";
      return false;
    }
    {
      ::lightusd::tydra::vol::FieldCache fields({}, uint64_t(128) << 20);
      ::lightusd::tydra::vol::FieldRequest request;
      request.prim_type = "OpenVDBAsset";
      request.file_path = (build.directory / file).string();
      request.field_name = "surface";
      request.field_class = "levelSet";
      ::lightusd::tydra::vol::ResolvedField field;
      std::vector<float> positions, colors;
      std::vector<int32_t> indices;
      if (!fields.Resolve(request, &field, &build.result.error) ||
          !fields.MeshLevelSet(field, 1000000, &positions, &indices,
                               &build.result.error))
        return false;
      request.field_name = "albedo";
      request.field_class.clear();
      request.field_data_type = "float3";
      if (!fields.Resolve(request, &field, &build.result.error)) return false;
      std::vector<double> points(positions.begin(), positions.end());
      int components = 0;
      if (!fields.SampleWorld(field, points.data(), points.size() / 3, true,
                              &colors, &components, &build.result.error) ||
          components != 3) {
        build.result.error = "preview albedo field sampling failed";
        return false;
      }
      surface_bytes += positions.size() * sizeof(float) * 12 +
                       indices.size() * sizeof(int32_t) * 4;
      if (surface_bytes > build.opts.gpu_budget_bytes - reserve) {
        build.result.error =
            "volume surface carrier exceeds preview budget; reduce resolution";
        return false;
      }
      usd::Layer mesh;
      usd::LayerBuilder mb(mesh);
      mb.begin_prim(name, "Mesh");
      mb.add_property("points", usd::Value::MakeArrayFromRaw(
                                    usd::TypeId::Point3f, positions.data(),
                                    positions.size() / 3));
      mb.add_property("faceVertexIndices", usd::Value::MakeIntArray(indices));
      const std::vector<int32_t> counts(indices.size() / 3, 3);
      mb.add_property("faceVertexCounts", usd::Value::MakeIntArray(counts));
      mb.add_property(
          "primvars:displayColor",
          usd::Value::MakeArrayFromRaw(usd::TypeId::Color3f, colors.data(),
                                       colors.size() / 3));
      mb.current()
          ->ensure_property_meta("primvars:displayColor")
          .interpolation = "vertex";
      mb.add_property("subdivisionScheme", usd::Value::MakeToken("none"));
      mb.add_property("doubleSided", usd::Value(true));
      mb.end_prim();
      mb.finalize();
      const std::string meshfile = "surface_" + std::to_string(index) + ".usdc";
      usd::CrateWriter writer;
      const auto written = writer.WriteLayerToFile(
          (build.directory / meshfile).string().c_str(), mesh);
      if (!written.success) {
        build.result.error = written.error;
        return false;
      }
      renderBuilder.begin_prim(name, "");
      renderBuilder.current()->meta().references.push_back("@" + meshfile +
                                                           "@</" + name + ">");
      renderBuilder.end_prim();
    }
    builder.begin_prim(name, "Volume");
    builder.add_relationship("field:density",
                             usd::Path("/ScenePreview/" + name + "/Surface"));
    builder.add_relationship("field:albedo",
                             usd::Path("/ScenePreview/" + name + "/Albedo"));
    for (int field = 0; field < 2; ++field) {
      builder.begin_prim(field ? "Albedo" : "Surface", "OpenVDBAsset");
      builder.add_property("filePath", usd::Value::MakeAssetPath(file));
      builder.add_property("fieldName",
                           usd::Value::MakeToken(field ? "albedo" : "surface"));
      builder.add_property("fieldDataType",
                           usd::Value::MakeToken(field ? "float3" : "float"));
      if (!field)
        builder.add_property("fieldClass", usd::Value::MakeToken("levelSet"));
      builder.end_prim();
    }
    builder.end_prim();
    ++index;
  }
  builder.end_prim();
  builder.finalize();
  renderBuilder.end_prim();
  renderBuilder.finalize();
  AppendPreviewExtras(build, renderRoot);
  usd::CrateWriter writer;
  const auto written = writer.WriteLayerToFile(
      (build.directory / "render.usdc").string().c_str(), renderRoot);
  if (!written.success) {
    build.result.error = written.error;
    return false;
  }
  build.result.voxel_size = cell;
  build.result.estimated_gpu_bytes =
      reserve + std::max(surface_bytes, index * brick_cost);
  return true;
}
bool WriteVolume(Build& build, usd::Layer& root,
                 const std::vector<Sample>& samples) {
  const auto metadata = root.meta();
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (TryWriteVolume(build, root, samples)) return true;
    if (build.opts.resolution ||
        build.result.error !=
            "volume surface carrier exceeds preview budget; reduce resolution")
      return false;
    // The generated surface can cost more than the brick estimate. Retry the
    // sampled data at a coarser voxel size without composing the source again.
    std::error_code ec;
    for (fs::directory_iterator it(build.directory, ec), end; !ec && it != end;
         it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.rfind("volume_", 0) == 0 || name.rfind("surface_", 0) == 0 ||
          name == "render.usdc")
        fs::remove(it->path(), ec);
    }
    if (ec) {
      build.result.error = "cannot clean volume refinement attempt";
      return false;
    }
    root = usd::Layer();
    root.meta() = metadata;
    build.cell_size *= 2;
    build.result.error.clear();
    if (!build.check("volume-refine", static_cast<uint64_t>(attempt + 1)))
      return false;
  }
  build.result.error = "volume refinement limit exceeded";
  return false;
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
