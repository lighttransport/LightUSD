// SPDX-License-Identifier: Apache-2.0
// MJCF <asset> meshes and heightfields.
#include "mjcf-internal.hh"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "external/stb_image.h"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
std::map<std::string, MeshAsset> CollectMujocoAssets(const pugi::xml_node& root,
                                                     const fs::path& base_dir) {
  std::map<std::string, MeshAsset> assets;
  const auto compiler = Child(root, "compiler");
  // MuJoCo <compiler meshdir> takes precedence; assetdir is the shared
  // default for meshdir/texturedir when meshdir is absent.
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);
  // <extension><plugin plugin><instance name><config key value>.
  std::map<std::string, std::pair<std::string, std::map<std::string, std::string>>> instances;
  for (const auto& extension : Children(root, "extension"))
    for (const auto& plugin : Children(extension, "plugin"))
      for (const auto& instance : Children(plugin, "instance")) {
        auto& entry = instances[Attr(instance, "name")];
        entry.first = Attr(plugin, "plugin");
        for (const auto& config : Children(instance, "config"))
          entry.second[Attr(config, "key")] = Attr(config, "value");
      }

  for (const auto& asset_node : Children(root, "asset")) {
    for (const auto& mesh_node : Children(asset_node, "mesh")) {
      const std::string file = Attr(mesh_node, "file");
      const auto plugin_node = Child(mesh_node, "plugin");
      if (file.empty() && plugin_node && active_plugin_meshes) {
        // A mesh MuJoCo's compiler generates from an SDF plugin
        // (mjCMesh::LoadSDF), by instance or by plugin with inline config.
        const std::string name = Attr(mesh_node, "name");
        std::string plugin = Attr(plugin_node, "plugin");
        std::map<std::string, std::string> config;
        const std::string instance = Attr(plugin_node, "instance");
        if (!instance.empty()) {
          const auto found = instances.find(instance);
          if (found != instances.end()) {
            plugin = found->second.first;
            config = found->second.second;
          }
        }
        for (const auto& c : Children(plugin_node, "config")) config[Attr(c, "key")] = Attr(c, "value");
        Mesh mesh;
        std::string why;
        const size_t budget = active_reader->options.max_memory -
                              std::min(active_reader->options.max_memory, active_reader->bytes);
        if (name.empty()) {
          why = "a plugin mesh needs a name";
        } else if (!IsMjcfSdfPlugin(plugin)) {
          why = "plugin `" + plugin + "` is not one of MuJoCo's analytic SDF plugins";
        } else if (GenerateMjcfSdfMesh(plugin, config, budget, &mesh, &why) &&
                   active_reader->ChargeArray(mesh.positions.size() + mesh.normals.size() +
                                                  mesh.indices.size(),
                                              4, &why)) {
          why.clear();
          MeshAsset asset;
          asset.path = kPluginMeshPrefix + name;
          if (HasAttr(mesh_node, "inertia")) asset.inertia = ToLower(Attr(mesh_node, "inertia"));
          (*active_plugin_meshes)[asset.path.string()] = std::move(mesh);
          assets[name] = asset;
          active_reader->Warn("MJCF mesh `" + name + "` generated from SDF plugin " + plugin,
                              "mjcf.sdf_plugin_mesh");
          continue;
        }
        if (!why.empty()) {
          const std::string message = "MJCF mesh `" + name + "` cannot be generated: " + why;
          if (active_reader->options.unsupported_policy == UnsupportedPolicy::Error)
            active_reader->failure = message;
          else
            active_reader->Warn(message, "mjcf.sdf_plugin_unsupported");
        }
        continue;
      }
      if (file.empty()) continue;
      std::string name = Attr(mesh_node, "name");
      if (name.empty()) name = fs::path(file).stem().string();
      MeshAsset asset;
      asset.path = ResolveMeshPath(base_dir, mesh_dir, file, index);
      asset.scale = ParseDouble3(Attr(mesh_node, "scale"), {{1.0, 1.0, 1.0}});
      if (HasAttr(mesh_node, "refpos")) {
        asset.refpos = ParseDouble3(Attr(mesh_node, "refpos"), {{0, 0, 0}});
        asset.has_refpos = true;
      }
      asset.smoothnormal = Attr(mesh_node, "smoothnormal") == "true";
      if (HasAttr(mesh_node, "maxhullvert"))
        asset.maxhullvert = ToInt32(ParseDoubleAttr(mesh_node, "maxhullvert", -1.0), "maxhullvert");
      if (HasAttr(mesh_node, "inertia")) asset.inertia = ToLower(Attr(mesh_node, "inertia"));
      const std::vector<double> rq = ParseDoubles(Attr(mesh_node, "refquat"));
      if (rq.size() >= 4) {
        asset.refquat = {{rq[0], rq[1], rq[2], rq[3]}};
        asset.has_refquat = true;
      }
      assets[name] = asset;
    }
  }
  return assets;
}

// Normalize raw elevation samples to [0,1] the way MuJoCo's compiler does:
// subtract the min, divide by the range (constant fields collapse to 0).
void NormalizeHField(std::vector<float>* data) {
  if (data->empty()) return;
  float emin = (*data)[0], emax = (*data)[0];
  for (float v : *data) {
    emin = std::min(emin, v);
    emax = std::max(emax, v);
  }
  const float range = emax - emin;
  for (float& v : *data) {
    v -= emin;
    if (range > 1e-9f) v /= range;
  }
}

// Collect <asset><hfield> declarations. File-based fields decode a (grayscale)
// PNG with rows reversed (MuJoCo stores row 0 at the -y edge); inline fields
// read nrow/ncol/elevation. All are normalized to [0,1].
std::map<std::string, HFieldAsset> CollectMujocoHFields(
    const pugi::xml_node& root, const fs::path& base_dir) {
  std::map<std::string, HFieldAsset> hfields;
  const auto compiler = Child(root, "compiler");
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);

  for (const auto& asset_node : Children(root, "asset")) {
    for (const auto& hf_node : Children(asset_node, "hfield")) {
      std::string name = Attr(hf_node, "name");
      const std::string file = Attr(hf_node, "file");
      if (name.empty()) {
        name = file.empty() ? std::string() : fs::path(file).stem().string();
      }
      if (name.empty()) continue;
      HFieldAsset hf;
      const std::vector<double> sz = ParseDoubles(Attr(hf_node, "size"));
      for (size_t i = 0; i < 4 && i < sz.size(); ++i) hf.size[i] = sz[i];

      if (!file.empty()) {
        const fs::path path = ResolveMeshPath(base_dir, mesh_dir, file, index);
        int w = 0, h = 0, comp = 0;
        std::vector<uint8_t> png;
        std::string resolved, why;
        if (!active_reader->Read(path.string(), active_reader->root_source,
                                 &png, &resolved, &why)) {
          active_reader->failure = why;
          return {};
        }
        if (png.size() > size_t(INT32_MAX) ||
            !stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w,
                                   &h, &comp) ||
            w <= 0 || h <= 0 ||
            size_t(w) > active_reader->options.max_memory / size_t(h) / 64 ||
            !active_reader->ChargeArray(size_t(w) * size_t(h), 64, &why)) {
          active_reader->failure =
              "Heightfield dimensions exceed memory budget";
          return {};
        }
        unsigned char* img = stbi_load_from_memory(
            png.data(), static_cast<int>(png.size()), &w, &h, &comp, 1);
        if (img && w > 0 && h > 0) {
          hf.ncol = w;
          hf.nrow = h;
          hf.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
          for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
              // reverse rows: data row 0 is the bottom (-y) image row
              hf.data[static_cast<size_t>(r) * w + c] =
                  static_cast<float>(img[c + (h - 1 - r) * w]);
            }
          }
        }
        if (img) stbi_image_free(img);
      } else {
        hf.nrow = ToInt32(ParseDoubleAttr(hf_node, "nrow", 0.0), "nrow");
        hf.ncol = ToInt32(ParseDoubleAttr(hf_node, "ncol", 0.0), "ncol");
        const std::vector<double> elev =
            ParseDoubles(Attr(hf_node, "elevation"));
        if (hf.nrow > 0 && hf.ncol > 0 &&
            elev.size() == static_cast<size_t>(hf.nrow) * hf.ncol) {
          hf.data.assign(elev.begin(), elev.end());
        }
      }
      if (hf.nrow > 0 && hf.ncol > 0 && !hf.data.empty()) {
        NormalizeHField(&hf.data);
        hfields[name] = std::move(hf);
      }
    }
  }
  return hfields;
}

// Tessellate a heightfield's top surface into a triangle mesh in the field's
// local frame: x spans [-rx,rx] across ncol columns, y spans [-ry,ry] across
// nrow rows, z = normalized_elevation * elevation_z. Per-vertex normals come
// from the local height gradient (smooth shading).
Mesh TessellateHField(const HFieldAsset& hf) {
  Mesh mesh;
  const int nr = hf.nrow, nc = hf.ncol;
  if (nr < 2 || nc < 2) return mesh;
  // Vertex ids are int32 (r * nc + c); reject grids whose ids would overflow.
  if (uint64_t(nr) * uint64_t(nc) > uint64_t(INT32_MAX) ||
      uint64_t(nr) * uint64_t(nc) != hf.data.size()) {
    if (active_reader)
      active_reader->Warn("Heightfield grid exceeds int32 vertex ids or data size",
                          "mjcf.hfield_size");
    return mesh;
  }
  // Positions and normals (6 floats) per vertex, two triangles per cell;
  // every geom using the heightfield tessellates it again.
  std::string why;
  if (active_reader &&
      !active_reader->ChargeArray(size_t(nr) * size_t(nc), 6 * sizeof(float) + 6 * sizeof(int32_t), &why))
    return mesh;
  const double rx = hf.size[0], ry = hf.size[1], ez = hf.size[2];
  const double dx = (2.0 * rx) / (nc - 1);
  const double dy = (2.0 * ry) / (nr - 1);
  auto H = [&](int r, int c) -> double {
    r = std::max(0, std::min(nr - 1, r));
    c = std::max(0, std::min(nc - 1, c));
    return static_cast<double>(hf.data[static_cast<size_t>(r) * nc + c]) * ez;
  };
  mesh.positions.reserve(static_cast<size_t>(nr) * nc * 3);
  mesh.normals.reserve(static_cast<size_t>(nr) * nc * 3);
  for (int r = 0; r < nr; ++r) {
    for (int c = 0; c < nc; ++c) {
      mesh.positions.push_back(static_cast<float>(-rx + c * dx));
      mesh.positions.push_back(static_cast<float>(-ry + r * dy));
      mesh.positions.push_back(static_cast<float>(H(r, c)));
      // Normal from the height-field gradient. Use the ACTUAL neighbor span so
      // the border ring is a correct one-sided difference (interior: 2 cells;
      // edges: 1 cell) rather than a half-magnitude central difference.
      const int cl = std::max(0, c - 1), cr = std::min(nc - 1, c + 1);
      const int rb = std::max(0, r - 1), rt = std::min(nr - 1, r + 1);
      const double hx = (H(r, cr) - H(r, cl)) / ((cr - cl) * dx);
      const double hy = (H(rt, c) - H(rb, c)) / ((rt - rb) * dy);
      double nx = -hx, ny = -hy, nz = 1.0;
      const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-12) {
        nx /= len;
        ny /= len;
        nz /= len;
      }
      mesh.normals.push_back(static_cast<float>(nx));
      mesh.normals.push_back(static_cast<float>(ny));
      mesh.normals.push_back(static_cast<float>(nz));
    }
  }
  mesh.indices.reserve(static_cast<size_t>(nr - 1) * (nc - 1) * 6);
  for (int r = 0; r < nr - 1; ++r) {
    for (int c = 0; c < nc - 1; ++c) {
      const int32_t v00 = r * nc + c;
      const int32_t v01 = r * nc + (c + 1);
      const int32_t v10 = (r + 1) * nc + c;
      const int32_t v11 = (r + 1) * nc + (c + 1);
      // two CCW triangles (viewed from +z)
      mesh.indices.push_back(v00);
      mesh.indices.push_back(v01);
      mesh.indices.push_back(v11);
      mesh.indices.push_back(v00);
      mesh.indices.push_back(v11);
      mesh.indices.push_back(v10);
    }
  }
  return mesh;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
