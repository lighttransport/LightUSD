// SPDX-License-Identifier: Apache-2.0
// flexcomp and raw <flex> soft bodies, GMSH parsing.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
bool IsSupportedFlexcompType(const std::string& type) {
  for (const char* t : {"grid", "circle", "square", "disc", "direct", "box", "cylinder",
                        "ellipsoid", "mesh", "gmsh"})
    if (type == t) return true;
  return false;
}

// Directory flexcomp `file` attributes resolve against (MuJoCo meshdir, or
// assetdir, relative to the model file).
fs::path FlexMeshDir(const pugi::xml_node& root, const fs::path& base_dir) {
  return CompilerAssetDir(root, base_dir, "meshdir");
}

// mjCFlexcomp::MakeBox with MuJoCo's default open=true: points on the box
// surface projected to a box, cylinder or ellipsoid (dim 3 adds the center
// as point 0 and builds tets to it; dim 2 has no top face).
void MakeFlexBox(const std::string& type, int dim, const std::array<int, 3>& count,
                 const std::array<double, 3>& spacing, std::vector<double>* point,
                 std::vector<int>* element) {
  const bool open = true;
  auto project = [&](int ix, int iy, int iz) {
    double p[3] = {2.0 * ix / (count[0] - 1) - 1, 2.0 * iy / (count[1] - 1) - 1,
                   2.0 * iz / (count[2] - 1) - 1};
    const double size[3] = {0.5 * spacing[0] * (count[0] - 1), 0.5 * spacing[1] * (count[1] - 1),
                            0.5 * spacing[2] * (count[2] - 1)};
    auto normalize = [&](int n) {
      double l = 0;
      for (int k = 0; k < n; ++k) l += p[k] * p[k];
      if (l < 1e-6) return;  // mjuu_normvec: tiny vectors are left as is
      l = std::sqrt(l);
      if (std::abs(l - 1) > 1e-6)
        for (int k = 0; k < n; ++k) p[k] /= l;
    };
    if (type == "cylinder") {
      const double l0 = std::max(std::abs(p[0]), std::abs(p[1]));
      normalize(2);
      p[0] *= size[0] * l0;
      p[1] *= size[1] * l0;
      p[2] *= size[2];
    } else {
      if (type == "ellipsoid") normalize(3);
      for (int k = 0; k < 3; ++k) p[k] *= size[k];
    }
    point->insert(point->end(), {p[0], p[1], p[2]});
  };
  auto lin = [&](int ix, int iy, int iz) { return ix * count[1] * count[2] + iy * count[2] + iz; };
  auto box_id = [&](int ix, int iy, int iz) {
    if (iz == 0) return ix * count[1] + iy + 1;
    if (iz == count[2] - 1) return count[0] * count[1] + ix * count[1] + iy + 1;
    if (iy == 0) return 2 * count[0] * count[1] + ix * (count[2] - 2) + iz - 1 + 1;
    if (iy == count[1] - 1)
      return 2 * count[0] * count[1] + count[0] * (count[2] - 2) + ix * (count[2] - 2) + iz - 1 + 1;
    if (ix == 0)
      return 2 * count[0] * count[1] + 2 * count[0] * (count[2] - 2) + (iy - 1) * (count[2] - 2) +
             iz - 1 + 1;
    return 2 * count[0] * count[1] + 2 * count[0] * (count[2] - 2) +
           (count[1] - 2) * (count[2] - 2) + (iy - 1) * (count[2] - 2) + iz - 1 + 1;
  };
  if (dim == 3) point->insert(point->end(), {0, 0, 0});
  std::vector<int> idx(static_cast<size_t>(count[0] * count[1] * count[2]), 0);
  int n = 0;
  const bool open2 = open && dim == 2;
  for (int iz = 0; iz < count[2]; iz += count[2] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iy = 0; iy < count[1]; ++iy) {
        if (open2 && iz != 0) continue;
        project(ix, iy, iz);
        idx[size_t(lin(ix, iy, iz))] = n++;
      }
  for (int iy = 0; iy < count[1]; iy += count[1] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iz = 0; iz < count[2]; ++iz)
        if (iz > 0 && (open2 || iz < count[2] - 1)) {
          project(ix, iy, iz);
          idx[size_t(lin(ix, iy, iz))] = n++;
        }
  for (int ix = 0; ix < count[0]; ix += count[0] - 1)
    for (int iy = 0; iy < count[1]; ++iy)
      for (int iz = 0; iz < count[2]; ++iz)
        if (iz > 0 && (open2 || iz < count[2] - 1) && iy > 0 && iy < count[1] - 1) {
          project(ix, iy, iz);
          idx[size_t(lin(ix, iy, iz))] = n++;
        }
  auto id = [&](int ix, int iy, int iz) { return idx[size_t(lin(ix, iy, iz))]; };
  auto push = [&](std::initializer_list<int> v) { element->insert(element->end(), v); };
  for (int iz = 0; iz < count[2]; iz += count[2] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iy = 0; iy < count[1]; ++iy) {
        if (open2 && iz != 0) continue;
        if (ix >= count[0] - 1 || iy >= count[1] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix + 1, iy, iz), box_id(ix + 1, iy + 1, iz)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy + 1, iz), box_id(ix + 1, iy + 1, iz)});
        } else {
          const int s1 = iz == 0 ? 1 : 0, s2 = iz == 0 ? 0 : 1;
          push({id(ix, iy, iz), id(ix + 1, iy + s1, iz), id(ix + 1, iy + s2, iz)});
          push({id(ix, iy, iz), id(ix + s2, iy + 1, iz), id(ix + s1, iy + 1, iz)});
        }
      }
  for (int iy = 0; iy < count[1]; iy += count[1] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iz = 0; iz < count[2]; ++iz) {
        if (ix >= count[0] - 1 || iz >= count[2] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix + 1, iy, iz), box_id(ix + 1, iy, iz + 1)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy, iz + 1), box_id(ix + 1, iy, iz + 1)});
        } else {
          const int ix0 = iy == 0 ? ix : ix + 1, dx = iy == 0 ? 1 : -1;
          push({id(ix0, iy, iz), id(ix0 + dx, iy, iz), id(ix0 + dx, iy, iz + 1)});
          push({id(ix0, iy, iz), id(ix0 + dx, iy, iz + 1), id(ix0, iy, iz + 1)});
        }
      }
  for (int ix = 0; ix < count[0]; ix += count[0] - 1)
    for (int iy = 0; iy < count[1]; ++iy)
      for (int iz = 0; iz < count[2]; ++iz) {
        if (iy >= count[1] - 1 || iz >= count[2] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix, iy + 1, iz), box_id(ix, iy + 1, iz + 1)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy, iz + 1), box_id(ix, iy + 1, iz + 1)});
        } else {
          const int iy0 = ix != 0 ? iy : iy + 1, dy = ix != 0 ? 1 : -1;
          push({id(ix, iy0, iz), id(ix, iy0 + dy, iz), id(ix, iy0 + dy, iz + 1)});
          push({id(ix, iy0, iz), id(ix, iy0 + dy, iz + 1), id(ix, iy0, iz + 1)});
        }
      }
}

// mjCFlexcomp::MakeMesh: the surface mesh's vertices (exact repeats merged
// in first-occurrence order, OBJ vertices in file order) and faces; dim 1
// takes edge pairs from degenerate triangles and dim 3 builds tets from the
// origin to every face that sees it with positive volume.
bool LoadFlexMesh(const fs::path& path, const std::string& text, int dim,
                  const std::array<double, 3>& origin, bool has_origin,
                  std::vector<double>* point, std::vector<int>* element, std::string* err,
                  bool* stuffed) {
  Mesh mesh;
  const std::string ext = ToLower(path.extension().string());
  if (ext == ".vdb" && dim == 3) {
    // A level set is already volumetric: stuff it directly.
    LevelSet level_set;
    if (!LoadLevelSetVDB(path, &level_set, err)) return false;
    const auto& opts = active_reader->options;
    const size_t budget = opts.max_memory - std::min(opts.max_memory, active_reader->bytes);
    StuffingOptions so;
    so.spacing = opts.flex_tet_spacing > 0 ? opts.flex_tet_spacing : 4 * level_set.voxel_size();
    so.threads = opts.threads;
    so.device = VolumetricDevice();
    so.max_tets = std::max<size_t>(1000, std::min<size_t>(budget / 256, 4000000));
    // The stuffing lattice is dense (~32 B per vertex before any tet exists).
    so.max_lattice_vertices = std::max<size_t>(100000, std::min<size_t>(budget / 64, 8000000));
    TetMeshData tm;
    if (!LevelSetToTetMesh(level_set, so, &tm, err)) return false;
    if (!active_reader->ChargeArray(tm.points.size() + tm.tets.size(), 8, err)) return false;
    point->assign(tm.points.begin(), tm.points.end());
    element->assign(tm.tets.begin(), tm.tets.end());
    if (stuffed) *stuffed = true;
    active_reader->Warn("flexcomp level-set VDB tetrahedralized by isosurface stuffing (" +
                            std::to_string(tm.tets.size() / 4) + " tets; LightUSD extension)",
                        "mjcf.flex_tetrahedralized");
    return true;
  }
  if (ext == ".vdb") {
    if (!LoadVDBMesh(path, &mesh, err)) return false;
  } else if (ext == ".obj") {
    OBJ obj;
    if (!ReadOBJ(text, path.string(), *active_reader, true, &obj, err)) return false;
    if (!obj.meshes.empty()) mesh = std::move(obj.meshes[0]);
  } else if (ext == ".stl") {
    if (!LoadBinarySTL(path, false, &mesh, err)) return false;
  } else {
    if (err) *err = "mesh flexcomp supports OBJ, STL and level-set VDB files";
    return false;
  }
  std::vector<int32_t> tri;
  if (!MeshTriangles(mesh, &tri) || tri.empty()) {
    if (err) *err = "mesh has no faces";
    return false;
  }
  const size_t corners = mesh.positions.size() / 3;
  std::vector<int> remap(corners, -1);
  if (mesh.source_vertices.size() == corners) {
    // OBJ: rebuild the file's vertex array, then merge exact repeats.
    int32_t max_source = -1;
    for (int32_t v : mesh.source_vertices) max_source = std::max(max_source, v);
    std::vector<float> raw(size_t(max_source + 1) * 3, 0.0f);
    std::vector<bool> seen(size_t(max_source + 1), false);
    for (size_t i = 0; i < corners; ++i) {
      const size_t v = size_t(mesh.source_vertices[i]);
      std::copy(&mesh.positions[3 * i], &mesh.positions[3 * i] + 3, &raw[3 * v]);
      seen[v] = true;
    }
    std::map<std::array<float, 3>, int> keys;
    std::vector<int> vertex_id(seen.size(), -1);
    for (size_t v = 0; v < seen.size(); ++v) {
      if (!seen[v]) continue;  // unreferenced: dropped later anyway
      const std::array<float, 3> key{{raw[3 * v], raw[3 * v + 1], raw[3 * v + 2]}};
      auto it = keys.emplace(key, static_cast<int>(point->size() / 3));
      if (it.second) point->insert(point->end(), key.begin(), key.end());
      vertex_id[v] = it.first->second;
    }
    for (size_t i = 0; i < corners; ++i) remap[i] = vertex_id[size_t(mesh.source_vertices[i])];
  } else {
    std::map<std::array<float, 3>, int> keys;
    for (size_t i = 0; i < corners; ++i) {
      const std::array<float, 3> key{{mesh.positions[3 * i], mesh.positions[3 * i + 1],
                                      mesh.positions[3 * i + 2]}};
      auto it = keys.emplace(key, static_cast<int>(point->size() / 3));
      if (it.second) point->insert(point->end(), key.begin(), key.end());
      remap[i] = it.first->second;
    }
  }
  if (dim == 2) {
    for (int32_t c : tri) element->push_back(remap[size_t(c)]);
  } else if (dim == 1) {
    for (size_t i = 0; i + 2 < tri.size(); i += 3)
      element->insert(element->end(), {remap[size_t(tri[i])], remap[size_t(tri[i + 1])]});
  } else if (active_reader->options.flex_tetrahedralize == "stuffing") {
    // Opt-in volumetric mesh: isosurface stuffing of the surface's level
    // set instead of MuJoCo's fan of every triangle to `origin`.
    Mesh surface;
    surface.positions.assign(point->begin(), point->end());
    for (int32_t c : tri) surface.indices.push_back(remap[size_t(c)]);
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < point->size(); i += 3)
      for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], (*point)[i + size_t(a)]);
        hi[a] = std::max(hi[a], (*point)[i + size_t(a)]);
      }
    const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    const auto& opts = active_reader->options;
    const double spacing = opts.flex_tet_spacing > 0 ? opts.flex_tet_spacing : extent / 12;
    LevelSetOptions ls;
    ls.voxel_size = spacing / 4;
    ls.threads = opts.threads;
    ls.device = VolumetricDevice();
    const size_t budget = opts.max_memory - std::min(opts.max_memory, active_reader->bytes);
    ls.max_voxels = std::max<size_t>(100000, std::min<size_t>(budget / 128, 50000000));
    LevelSet level_set;
    StuffingOptions so;
    so.spacing = spacing;
    so.threads = opts.threads;
    so.device = ls.device;
    so.max_tets = std::max<size_t>(1000, std::min<size_t>(budget / 256, 4000000));
    // The stuffing lattice is dense (~32 B per vertex before any tet exists).
    so.max_lattice_vertices = std::max<size_t>(100000, std::min<size_t>(budget / 64, 8000000));
    TetMeshData tm;
    std::string why;
    if (!(extent > 0) || !MeshToLevelSet(surface, ls, &level_set, &why) ||
        !LevelSetToTetMesh(level_set, so, &tm, &why)) {
      if (err) *err = "flexcomp tetrahedralization failed: " + (why.empty() ? std::string("no extent") : why);
      return false;
    }
    if (!active_reader->ChargeArray(tm.points.size() + tm.tets.size(), 8, err)) return false;
    point->assign(tm.points.begin(), tm.points.end());
    element->assign(tm.tets.begin(), tm.tets.end());
    if (stuffed) *stuffed = true;
    active_reader->Warn("flexcomp mesh tetrahedralized by isosurface stuffing (" +
                            std::to_string(tm.tets.size() / 4) + " tets, spacing " +
                            std::to_string(spacing) + ") instead of MuJoCo's fan to origin",
                        "mjcf.flex_tetrahedralized");
  } else {
    if (!has_origin) {
      if (err) *err = "origin must be specified for mesh flexcomps if dim=3";
      return false;
    }
    point->insert(point->begin(), origin.begin(), origin.end());
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
      const int t[3] = {remap[size_t(tri[i])] + 1, remap[size_t(tri[i + 1])] + 1,
                        remap[size_t(tri[i + 2])] + 1};
      double e[3][3];
      for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 3; ++k) e[a][k] = (*point)[3 * size_t(t[a]) + size_t(k)] - origin[size_t(k)];
      const double nx = e[0][1] * e[1][2] - e[0][2] * e[1][1];
      const double ny = e[0][2] * e[1][0] - e[0][0] * e[1][2];
      const double nz = e[0][0] * e[1][1] - e[0][1] * e[1][0];
      if (nx * e[2][0] + ny * e[2][1] + nz * e[2][2] < 1e-15) continue;  // mjMINVAL
      element->insert(element->end(), {0, t[0], t[1], t[2]});
    }
  }
  return true;
}

// Binary GMSH 4.1 and 2.2, as MuJoCo 3.16's GMSH decoder (plugin/
// gmsh_decoder) reads them. 4.1: size_t numEntityBlocks, numNodes,
// minNodeTag, maxNodeTag; int entityDim, entityTag, parametric; size_t
// numNodesInBlock; size_t tags[n]; double xyz[3n]; elements alike with
// size_t (tag, nodes...) records. 2.2: an ASCII count line, then int tag +
// double xyz[3] per node; elements in blocks of int (type, count, ntags)
// followed by int (number, tags[ntags], nodes...) records, which covers both
// gmsh's one-block-per-element files and fTetWild's single tagless block.
bool ParseGMSHBinary(const std::string& text, long version, int* dim, std::vector<double>* point,
                     std::vector<int>* element, std::string* err) {
  const size_t nb = text.find("$Nodes"), ne = text.find("$EndNodes");
  const size_t eb = text.find("$Elements"), ee = text.find("$EndElements");
  if (nb == std::string::npos || ne == std::string::npos || eb == std::string::npos ||
      ee == std::string::npos || ne < nb + 7 || ee < eb + 10) {
    *err = "GMSH file is missing its $Nodes or $Elements section";
    return false;
  }
  const char* bytes = text.data();
  const size_t node_begin = nb + 7, node_end = ne, elem_begin = eb + 10, elem_end = ee;
  auto read = [&](size_t at, size_t end, size_t n, void* out) {
    if (at > end || n > end - at) return false;
    std::memcpy(out, bytes + at, n);
    return true;
  };
  const size_t kMax = 50000000;
  if (version == 410) {
    uint64_t blocks = 0, num = 0, min_tag = 0, max_tag = 0, in_block = 0;
    int32_t edim = 0, etag = 0, param = 0;
    size_t at = node_begin;
    if (!read(at, node_end, 8, &blocks) || !read(at + 8, node_end, 8, &num) ||
        !read(at + 16, node_end, 8, &min_tag) || !read(at + 24, node_end, 8, &max_tag) ||
        !read(at + 32, node_end, 4, &edim) || !read(at + 36, node_end, 4, &etag) ||
        !read(at + 40, node_end, 4, &param) || !read(at + 44, node_end, 8, &in_block)) {
      *err = "invalid GMSH nodes header";
      return false;
    }
    if (blocks != 1 || num != in_block || num > kMax || edim < 1 || edim > 3) {
      *err = "GMSH nodes must be one block of 1D, 2D or 3D entities";
      return false;
    }
    at += 52;
    if (node_end - std::min(node_end, at) < 32 * num) {
      *err = "insufficient byte size of GMSH nodes";
      return false;
    }
    *dim = edim;
    for (uint64_t i = 0; i < num; ++i) {
      uint64_t tag = 0;
      read(at + 8 * i, node_end, 8, &tag);
      if (tag != i + min_tag) {
        *err = "GMSH node tags must be sequential";
        return false;
      }
    }
    at += 8 * num;
    point->resize(3 * num);
    if (num) std::memcpy(point->data(), bytes + at, 24 * num);
    uint64_t enumb = 0, emin = 0, emax = 0, ein = 0;
    int32_t type = 0;
    at = elem_begin;
    if (!read(at, elem_end, 8, &blocks) || !read(at + 8, elem_end, 8, &enumb) ||
        !read(at + 16, elem_end, 8, &emin) || !read(at + 24, elem_end, 8, &emax) ||
        !read(at + 32, elem_end, 4, &edim) || !read(at + 36, elem_end, 4, &etag) ||
        !read(at + 40, elem_end, 4, &type) || !read(at + 44, elem_end, 8, &ein)) {
      *err = "invalid GMSH elements header";
      return false;
    }
    if (blocks != 1 || enumb != ein || edim != *dim || enumb > kMax ||
        type != (edim == 1 ? 1 : edim == 2 ? 2 : 4)) {
      *err = "GMSH elements must be one block matching the node dimension";
      return false;
    }
    at += 52;
    const size_t record = 8 * size_t(edim + 2);
    if (elem_end - std::min(elem_end, at) < record * enumb) {
      *err = "insufficient byte size of GMSH elements";
      return false;
    }
    element->reserve(size_t(edim + 1) * enumb);
    for (uint64_t i = 0; i < enumb; ++i)
      for (int k = 0; k <= edim; ++k) {
        uint64_t node = 0;
        read(at + record * i + 8 * size_t(k + 1), elem_end, 8, &node);
        if (node < min_tag || node - min_tag >= num) {
          *err = "invalid node index in GMSH element";
          return false;
        }
        element->push_back(static_cast<int>(node - min_tag));
      }
    return true;
  }
  // 2.2: "<count>\n" then binary records.
  auto count_line = [&](size_t begin, size_t end, size_t* count, size_t* data) {
    size_t at = begin, value = 0;
    while (at < end && bytes[at] >= '0' && bytes[at] <= '9' && value <= kMax)
      value = value * 10 + size_t(bytes[at++] - '0');
    if (at == begin || at >= end || bytes[at] != '\n' || value > kMax) return false;
    *count = value;
    *data = at + 1;
    return true;
  };
  size_t num = 0, at = 0;
  if (!count_line(node_begin, node_end, &num, &at)) {
    *err = "invalid GMSH node count";
    return false;
  }
  if (node_end - std::min(node_end, at) < 28 * num) {
    *err = "insufficient byte size of GMSH nodes";
    return false;
  }
  point->resize(3 * num);
  for (size_t i = 0; i < num; ++i) std::memcpy(&(*point)[3 * i], bytes + at + 28 * i + 4, 24);
  size_t count = 0;
  if (!count_line(elem_begin, elem_end, &count, &at)) {
    *err = "invalid GMSH element count";
    return false;
  }
  int nodes_per = 0;
  for (size_t done = 0; done < count;) {
    int32_t header[3];
    if (!read(at, elem_end, 12, header) || header[1] < 1 || header[2] < 0 || header[2] > 64 ||
        size_t(header[1]) > count - done) {
      *err = "invalid GMSH element block";
      return false;
    }
    const int per = header[0] == 2 ? 3 : header[0] == 4 ? 4 : 0;
    if (!per || (nodes_per && per != nodes_per)) {
      *err = "GMSH 2.2 elements must all be triangles or tetrahedra";
      return false;
    }
    nodes_per = per;
    at += 12;
    const size_t record = 4 * (1 + size_t(header[2]) + size_t(per));
    for (int32_t e = 0; e < header[1]; ++e, at += record) {
      for (int k = 0; k < per; ++k) {
        int32_t node = 0;
        if (!read(at + 4 * (1 + size_t(header[2]) + size_t(k)), elem_end, 4, &node) || node < 1 ||
            size_t(node) > num) {
          *err = "invalid GMSH node tag";
          return false;
        }
        element->push_back(node - 1);
      }
    }
    done += size_t(header[1]);
  }
  *dim = nodes_per == 3 ? 2 : 3;
  return true;
}

// ASCII GMSH 4.1 (single node and element block) and 2.2, as
// mjCFlexcomp::LoadGMSH reads them; binary files by ParseGMSHBinary.
bool ParseGMSH(const std::string& text, int* dim, std::vector<double>* point,
               std::vector<int>* element, std::string* err) {
  if (text.compare(0, 11, "$MeshFormat") != 0) {
    *err = "GMSH file must begin with $MeshFormat";
    return false;
  }
  std::istringstream header(text.substr(11, 64));
  double version = 0;
  int binary = 0;
  if (!(header >> version >> binary)) {
    *err = "could not read the GMSH header";
    return false;
  }
  const long v = std::lround(100 * version);
  if (v != 220 && v != 410) {
    *err = "only GMSH file format versions 4.1 and 2.2 are supported";
    return false;
  }
  if (binary) return ParseGMSHBinary(text, v, dim, point, element, err);
  const size_t nb = text.find("$Nodes"), ne = text.find("$EndNodes");
  const size_t eb = text.find("$Elements"), ee = text.find("$EndElements");
  if (nb == std::string::npos || ne == std::string::npos || eb == std::string::npos ||
      ee == std::string::npos || ne < nb || ee < eb) {
    *err = "GMSH file is missing its $Nodes or $Elements section";
    return false;
  }
  std::istringstream nodes(text.substr(nb + 6, ne - nb - 6));
  std::istringstream elems(text.substr(eb + 9, ee - eb - 9));
  const size_t kMax = 50000000;
  if (v == 410) {
    size_t blocks = 0, num = 0, min_tag = 0, max_tag = 0, in_block = 0;
    int edim = 0, etag = 0, param = 0;
    if (!(nodes >> blocks >> num >> min_tag >> max_tag >> edim >> etag >> param >> in_block) ||
        blocks != 1 || num != in_block || max_tag != in_block || num > kMax || edim < 1 || edim > 3) {
      *err = "GMSH nodes must be one sequential block of 1D, 2D or 3D entities";
      return false;
    }
    *dim = edim;
    for (size_t i = 0; i < num; ++i) {
      size_t tag = 0;
      if (!(nodes >> tag) || tag != i + min_tag) {
        *err = "GMSH node tags must be sequential";
        return false;
      }
    }
    for (size_t i = 0; i < 3 * num; ++i) {
      double x = 0;
      if (!(nodes >> x)) {
        *err = "error reading GMSH nodes";
        return false;
      }
      point->push_back(x);
    }
    size_t eblocks = 0, enumb = 0, emin = 0, emax = 0, ein = 0;
    int type = 0;
    if (!(elems >> eblocks >> enumb >> emin >> emax >> edim >> etag >> type >> ein) ||
        eblocks != 1 || enumb != ein || edim != *dim || enumb > kMax ||
        type != (edim == 1 ? 1 : edim == 2 ? 2 : 4)) {
      *err = "GMSH elements must be one block matching the node dimension";
      return false;
    }
    for (size_t i = 0; i < enumb; ++i) {
      size_t tag = 0, node = 0;
      elems >> tag;
      for (int k = 0; k <= edim; ++k) {
        if (!(elems >> node) || node < min_tag || node - min_tag >= num) {
          *err = "error reading GMSH elements";
          return false;
        }
        element->push_back(static_cast<int>(node - min_tag));
      }
    }
    return true;
  }
  size_t num = 0;
  if (!(nodes >> num) || num > kMax) {
    *err = "error reading the GMSH node count";
    return false;
  }
  for (size_t i = 0; i < num; ++i) {
    size_t tag = 0;
    double x[3];
    if (!(nodes >> tag >> x[0] >> x[1] >> x[2])) {
      *err = "error reading GMSH nodes";
      return false;
    }
    point->insert(point->end(), {x[0], x[1], x[2]});
  }
  size_t count = 0;
  if (!(elems >> count) || count > kMax) {
    *err = "error reading the GMSH element count";
    return false;
  }
  int nodes_per = 0;
  for (size_t i = 0; i < count; ++i) {
    int tag = 0, type = 0, ntags = 0;
    if (!(elems >> tag >> type >> ntags) || ntags < 0 || ntags > 64) {
      *err = "error reading GMSH elements";
      return false;
    }
    const int per = type == 2 ? 3 : type == 4 ? 4 : 0;
    if (!per || (nodes_per && per != nodes_per)) {
      *err = "GMSH 2.2 elements must all be triangles or tetrahedra";
      return false;
    }
    nodes_per = per;
    for (int k = 0; k < ntags; ++k) elems >> tag;
    for (int k = 0; k < per; ++k) {
      long node = 0;
      if (!(elems >> node) || node < 1 || size_t(node) > num) {
        *err = "invalid GMSH node tag";
        return false;
      }
      element->push_back(static_cast<int>(node - 1));
    }
  }
  *dim = nodes_per == 3 ? 2 : 3;
  return true;
}

// Shared <flex>/<flexcomp> appearance, elasticity, edge and contact values.
void ReadFlexProperties(const pugi::xml_node& f, Json* out) {
  Json& entry = *out;
  const auto rgba = ParseDoubles(Attr(f, "rgba"));
  if (rgba.size() >= 4) entry["rgba"] = Array(rgba);
  if (const auto el = Child(f, "elasticity")) {
    for (const char* key : {"young", "poisson", "damping", "thickness"})
      if (HasAttr(el, key)) entry[key] = ParseDoubleAttr(el, key, 0.0);
    if (HasAttr(el, "elastic2d")) entry["elastic2d"] = ToLower(Attr(el, "elastic2d"));
  }
  if (const auto edge = Child(f, "edge")) {
    if (HasAttr(edge, "stiffness")) entry["edgeStiffness"] = ParseDoubleAttr(edge, "stiffness", 0.0);
    if (HasAttr(edge, "damping")) entry["edgeDamping"] = ParseDoubleAttr(edge, "damping", 0.0);
    if (HasAttr(edge, "equality")) entry["edgeEquality"] = ToLower(Attr(edge, "equality"));
  }
  int contype = 1, conaffinity = 1;
  if (const auto contact = Child(f, "contact")) {
    contype = ToInt32(ParseDoubleAttr(contact, "contype", 1.0), "contype");
    conaffinity = ToInt32(ParseDoubleAttr(contact, "conaffinity", 1.0), "conaffinity");
    const auto friction = ParseDoubles(Attr(contact, "friction"));
    if (!friction.empty()) entry["friction"] = friction[0];
    if (HasAttr(contact, "selfcollide")) entry["selfcollide"] = ToLower(Attr(contact, "selfcollide"));
  }
  entry["collides"] = contype != 0 || conaffinity != 0;
}

// MJCF <flexcomp> (grid, circle, square, disc, direct) expanded the way
// MuJoCo's mjCFlexcomp::Make does, but kept as one soft body instead of a
// point-mass body per vertex: world-space points, elements, the pinned
// vertices (attached to the owning body) and the elasticity/contact values.
bool CollectBodyFlexcompsJson(const pugi::xml_node& body_node,
                              const std::vector<double>& body_world,
                              const Context& ctx, bool is_world,
                              const fs::path& mesh_dir, Json* out,
                              std::string* err) {
  for (const auto& f : Children(body_node, "flexcomp")) {
    const std::string name = Attr(f, "name");
    const std::string type = ToLower(Attr(f, "type", "grid"));
    auto fail = [&](const std::string& message, const char* code) {
      if (UnsupportedXML(f, "flexcomp `" + name + "`: " + message, code, "mjcf.flexcomp",
                         active_reader->root_source, false))
        return true;  // skipped
      if (err) *err = active_reader->failure;
      return false;
    };
    // Other types were already reported as unsupported while reading the XML.
    if (!IsSupportedFlexcompType(type)) continue;
    int dim = ToInt32(ParseDoubleAttr(f, "dim", 2.0), "dim");
    if (type == "circle") dim = 1;
    if (type == "square" || type == "disc") dim = 2;
    const bool box_type = type == "box" || type == "cylinder" || type == "ellipsoid";
    if (dim < 1 || dim > 3) {
      if (err) *err = "flexcomp `" + name + "`: dim must be between 1 and 3";
      return false;
    }
    const std::string dof = ToLower(Attr(f, "dof", "full"));
    if (dof != "full" &&
        !UnsupportedXML(f, "flexcomp `" + name + "` dof=`" + dof +
                               "` is approximated by free vertices (dof=full)",
                        "mjcf.flexcomp_dof_approximated", "mjcf.flexcomp",
                        active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    if (Child(f, "plugin") &&
        !UnsupportedXML(f, "flexcomp `" + name + "` plugin is ignored",
                        "mjcf.flexcomp_plugin", "mjcf.flexcomp", active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    std::array<int, 3> count{{10, 10, 10}};
    {
      const auto c = ParseDoubles(Attr(f, "count"));
      for (size_t i = 0; i < 3 && i < c.size(); ++i) count[i] = ToInt32(c[i], "count");
    }
    const auto spacing = ParseDouble3(Attr(f, "spacing"), {{0.02, 0.02, 0.02}});
    const auto scale = ParseDouble3(Attr(f, "scale"), {{1, 1, 1}});
    for (int c : count)
      if (c < 1 || c > 100000) {
        if (err) *err = "flexcomp `" + name + "`: invalid count";
        return false;
      }
    if (double(count[0]) * count[1] * count[2] > 1e7) {
      if (err) *err = "flexcomp `" + name + "`: more than 10 million grid points";
      return false;
    }
    std::vector<double> point;
    std::vector<int> element;
    bool stuffed = false;  // tetrahedralized here: source vertex ids are gone
    const bool direct = type == "direct" || type == "mesh" || type == "gmsh";
    // Generated lattices: charge the point and element vectors before
    // building them (three coordinates, up to 6 tets of 4 ids per cell).
    if (!direct &&
        !active_reader->ChargeArray(size_t(count[0]) * size_t(count[1]) * size_t(count[2]),
                                    3 * sizeof(double) + 24 * sizeof(int), err))
      return false;
    if (type == "direct") {
      point = ParseDoubles(Attr(f, "point"));
      for (double v : ParseDoubles(Attr(f, "element"))) {
        if (!std::isfinite(v) || std::trunc(v) != v || v < INT32_MIN || v > INT32_MAX) {
          if (err) *err = "flexcomp `" + name + "`: element must contain int32 values";
          return false;
        }
        element.push_back(static_cast<int>(v));
      }
    } else if (type == "mesh" || type == "gmsh") {
      const std::string file = Attr(f, "file");
      if (file.empty()) {
        if (err) *err = "flexcomp `" + name + "`: file is required";
        return false;
      }
      const fs::path path = ResolveMeshPath(mesh_dir, mesh_dir, file);
      std::string text;
      if (!ReadFile(path, &text, err)) return false;
      if (type == "gmsh") {
        if (!ParseGMSH(text, &dim, &point, &element, err)) {
          if (err) *err = "flexcomp `" + name + "`: " + *err;
          return false;
        }
        if (!AllFinite(point)) {
          if (err) *err = "flexcomp `" + name + "`: GMSH node is not finite";
          return false;
        }
      } else if (!LoadFlexMesh(path, text, dim, ParseDouble3(Attr(f, "origin"), {{0, 0, 0}}),
                               HasAttr(f, "origin"), &point, &element, err, &stuffed)) {
        if (err) *err = "flexcomp `" + name + "`: " + *err;
        return false;
      }
    } else if (box_type) {
      if (count[0] < 2 || count[1] < 2 || count[2] < 2) {
        if (err) *err = "flexcomp `" + name + "`: box, cylinder and ellipsoid need count >= 2";
        return false;
      }
      MakeFlexBox(type, dim, count, spacing, &point, &element);
    } else if (dim == 1) {
      for (int ix = 0; ix < count[0]; ++ix) {
        if (type == "circle") {
          if (ix >= count[0] - 1) continue;
          const double theta = 2 * kPi / (count[0] - 1);
          const double radius = spacing[0] / std::sin(theta / 2) / 2;
          point.insert(point.end(), {radius * std::cos(theta * ix), radius * std::sin(theta * ix), 0});
          element.insert(element.end(), {ix, ix == count[0] - 2 ? 0 : ix + 1});
        } else {
          point.insert(point.end(), {spacing[0] * (ix - 0.5 * (count[0] - 1)), 0, 0});
          if (ix < count[0] - 1) element.insert(element.end(), {ix, ix + 1});
        }
      }
    } else if (dim == 2) {
      for (int ix = 0; ix < count[0]; ++ix)
        for (int iy = 0; iy < count[1]; ++iy) {
          double pos[2] = {spacing[0] * (ix - 0.5 * (count[0] - 1)),
                           spacing[1] * (iy - 0.5 * (count[1] - 1))};
          int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
          if (type == "disc" && ((pos[0] < -1e-6 && pos[1] > -1e-6) || (pos[0] > -1e-6 && pos[1] < -1e-6))) {
            tri[0][2] = 3;
            tri[1][0] = 1;
          }
          point.insert(point.end(), {pos[0], pos[1], 0});
          if (ix < count[0] - 1 && iy < count[1] - 1) {
            const int v[4] = {count[2] * count[1] * ix + count[2] * iy,
                              count[2] * count[1] * (ix + 1) + count[2] * iy,
                              count[2] * count[1] * (ix + 1) + count[2] * (iy + 1),
                              count[2] * count[1] * ix + count[2] * (iy + 1)};
            for (auto& t : tri) element.insert(element.end(), {v[t[0]], v[t[1]], v[t[2]]});
          }
        }
      if (type == "disc") {
        const double size[2] = {0.5 * spacing[0] * (count[0] - 1), 0.5 * spacing[1] * (count[1] - 1)};
        for (size_t i = 0; i + 2 < point.size(); i += 3) {
          double* p = &point[i];
          const double l0 = std::max(std::abs(p[0]), std::abs(p[1]));
          const double n = std::sqrt(p[0] * p[0] + p[1] * p[1]);
          if (n >= 1e-6 && std::abs(n - 1) > 1e-6) { p[0] /= n; p[1] /= n; }
          p[0] *= size[0] * l0;
          p[1] *= size[1] * l0;
        }
      }
    } else {
      static const int kCube2Tets[6][4] = {{0, 3, 1, 7}, {0, 1, 4, 7}, {1, 3, 2, 7},
                                           {1, 2, 6, 7}, {1, 5, 4, 7}, {1, 6, 5, 7}};
      for (int ix = 0; ix < count[0]; ++ix)
        for (int iy = 0; iy < count[1]; ++iy)
          for (int iz = 0; iz < count[2]; ++iz) {
            point.insert(point.end(), {spacing[0] * (ix - 0.5 * (count[0] - 1)),
                                       spacing[1] * (iy - 0.5 * (count[1] - 1)),
                                       spacing[2] * (iz - 0.5 * (count[2] - 1))});
            if (ix < count[0] - 1 && iy < count[1] - 1 && iz < count[2] - 1) {
              const int cz = count[2], cyz = count[2] * count[1];
              const int v[8] = {cyz * ix + cz * iy + iz,           cyz * (ix + 1) + cz * iy + iz,
                                cyz * (ix + 1) + cz * (iy + 1) + iz, cyz * ix + cz * (iy + 1) + iz,
                                cyz * ix + cz * iy + iz + 1,       cyz * (ix + 1) + cz * iy + iz + 1,
                                cyz * (ix + 1) + cz * (iy + 1) + iz + 1,
                                cyz * ix + cz * (iy + 1) + iz + 1};
              for (const auto& t : kCube2Tets)
                element.insert(element.end(), {v[t[0]], v[t[1]], v[t[2]], v[t[3]]});
            }
          }
    }
    if (direct)
      for (size_t i = 0; i < point.size(); ++i) point[i] *= scale[i % 3];
    const int npnt = static_cast<int>(point.size() / 3);
    if (point.empty() || point.size() % 3 || element.empty() ||
        element.size() % static_cast<size_t>(dim + 1)) {
      if (err) *err = "flexcomp `" + name + "`: point and element sizes are invalid";
      return false;
    }
    for (int e : element)
      if (e < 0 || e >= npnt) {
        if (err) *err = "flexcomp `" + name + "`: element references a missing point";
        return false;
      }
    // Pins (ids before compaction, as MuJoCo applies them).
    std::vector<bool> pinned(static_cast<size_t>(npnt), MjcBoolAttr(Attr(f, "rigid", "false")));
    std::vector<int> pin_ids, pin_ranges, pin_grid, pin_grid_ranges;
    for (const auto& pin : Children(f, "pin")) {
      if (stuffed) {
        active_reader->Warn("flexcomp `" + name + "`: <pin> vertex ids do not survive "
                                "tetrahedralization; the pin is ignored",
                            "mjcf.flex_pin_unmapped");
        continue;
      }
      auto append = [&](const char* key, std::vector<int>* values) {
        for (double v : ParseDoubles(Attr(pin, key))) {
          if (!std::isfinite(v) || std::trunc(v) != v || v < INT32_MIN || v > INT32_MAX) {
            if (err) *err = "flexcomp `" + name + "`: pin " + key + " must contain int32 values";
            return false;
          }
          values->push_back(static_cast<int>(v));
        }
        return true;
      };
      if (!append("id", &pin_ids) || !append("range", &pin_ranges) ||
          !append("grid", &pin_grid) || !append("gridrange", &pin_grid_ranges))
        return false;
    }
    const size_t d = static_cast<size_t>(dim);
    // MuJoCo combines each attribute across all <pin> elements before
    // validating its tuple arity, so a tuple may span multiple elements.
    if (pin_ranges.size() % 2 || pin_grid.size() % d || pin_grid_ranges.size() % (2 * d)) {
      if (err) *err = "flexcomp `" + name + "`: invalid pin tuple size";
      return false;
    }
    auto grid_id = [&](const std::vector<int>& g, size_t i) {
      return dim == 2 ? g[i] * count[1] + g[i + 1]
                      : g[i] * count[1] * count[2] + g[i + 1] * count[2] + g[i + 2];
    };
    auto set = [&](int id) {
      if (id < 0 || id >= npnt) return false;
      pinned[size_t(id)] = true;
      return true;
    };
    bool ok = true;
    for (int id : pin_ids) ok &= set(id);
    for (size_t i = 0; ok && i < pin_ranges.size(); i += 2) {
      const int lo = pin_ranges[i], hi = pin_ranges[i + 1];
      ok = lo >= 0 && lo < npnt && hi >= 0 && hi < npnt;
      for (int k = lo; ok && k <= hi; ++k) ok &= set(k);
    }
    if ((!pin_grid.empty() || !pin_grid_ranges.empty()) && (type != "grid" || dim == 1)) ok = false;
    // Validate both endpoints even for reversed (empty) ranges. Do this
    // before calculating ids or iterating so malformed ranges cannot wrap.
    const auto in_lattice = [&](const std::vector<int>& g, size_t i) {
      for (size_t a = 0; a < d; ++a)
        if (g[i + a] < 0 || g[i + a] >= count[a]) return false;
      return true;
    };
    for (size_t i = 0; ok && i < pin_grid.size(); i += d)
      ok = in_lattice(pin_grid, i) && set(grid_id(pin_grid, i));
    for (size_t i = 0; ok && i < pin_grid_ranges.size(); i += 2 * d) {
      ok = in_lattice(pin_grid_ranges, i) && in_lattice(pin_grid_ranges, i + d);
      if (!ok) break;
      std::vector<int> g(3, 0);
      if (dim == 2) {
        for (g[0] = pin_grid_ranges[i]; g[0] <= pin_grid_ranges[i + 2]; ++g[0])
          for (g[1] = pin_grid_ranges[i + 1]; g[1] <= pin_grid_ranges[i + 3]; ++g[1])
            ok &= set(grid_id(g, 0));
      } else {
        for (g[0] = pin_grid_ranges[i]; g[0] <= pin_grid_ranges[i + 3]; ++g[0])
          for (g[1] = pin_grid_ranges[i + 1]; g[1] <= pin_grid_ranges[i + 4]; ++g[1])
            for (g[2] = pin_grid_ranges[i + 2]; g[2] <= pin_grid_ranges[i + 5]; ++g[2])
              ok &= set(grid_id(g, 0));
      }
    }
    if (!ok) {
      if (err) *err = "flexcomp `" + name + "`: pin is out of range";
      return false;
    }
    // Direct, mesh and gmsh types drop unreferenced points.
    if (direct) {
      std::vector<int> remap(static_cast<size_t>(npnt), -1);
      for (int e : element) remap[size_t(e)] = 0;
      std::vector<double> kept;
      std::vector<bool> kept_pins;
      for (int i = 0; i < npnt; ++i)
        if (remap[size_t(i)] == 0) {
          remap[size_t(i)] = static_cast<int>(kept.size() / 3);
          kept.insert(kept.end(), point.begin() + 3 * i, point.begin() + 3 * i + 3);
          kept_pins.push_back(pinned[size_t(i)]);
        }
      for (int& e : element) e = remap[size_t(e)];
      point.swap(kept);
      pinned.swap(kept_pins);
    }
    // Pose in the owning body, then world.
    const std::array<double, 3> fpos = ParseDouble3(Attr(f, "pos"), {{0, 0, 0}});
    const Quat fq = OrientationQuat(Attr(f, "quat"), Attr(f, "axisangle"), Attr(f, "euler"),
                                    Attr(f, "xyaxes"), Attr(f, "zaxis"), ctx);
    const std::vector<double> local = MatrixFromPosQuat(fpos, fq);
    const std::vector<double> world = MultiplyMatrix(body_world, local);
    auto apply = [](const std::vector<double>& m, const double* p) {
      return std::array<double, 3>{{m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                                    m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                                    m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]}};
    };
    // The JSON payload holds one value per coordinate and element id.
    if (!active_reader->ChargeArray(point.size() + element.size(), sizeof(Json) + 16, err))
      return false;
    Json points = Json::array(), pins = Json::array(), pin_coords = Json::array();
    for (size_t i = 0; i < point.size() / 3; ++i) {
      const auto w = apply(world, &point[3 * i]);
      points.push_back(Json::array({w[0], w[1], w[2]}));
      if (pinned[i]) {
        const auto b = apply(local, &point[3 * i]);
        pins.push_back(static_cast<int>(i));
        pin_coords.push_back(Json::array({b[0], b[1], b[2]}));
      }
    }
    if (pins.size() == points.size()) {
      if (!fail("rigid (fully pinned) flexes are not supported", "mjcf.flexcomp_rigid")) return false;
      continue;
    }
    Json entry = {{"name", name.empty() ? "flexcomp_" + std::to_string(out->size()) : name},
                  {"type", type},
                  {"dim", dim},
                  {"points", std::move(points)},
                  {"elements", Array(std::vector<double>(element.begin(), element.end()))},
                  {"mass", ParseDoubleAttr(f, "mass", 1.0)},
                  {"radius", ParseDoubleAttr(f, "radius", 0.005)},
                  {"periodic", type == "circle"}};
    if (!pins.empty()) {
      entry["pins"] = std::move(pins);
      entry["pinCoords"] = std::move(pin_coords);
      auto owner = active_body_names.find(body_node.internal_object());
      if (!is_world && owner != active_body_names.end()) entry["pinBody"] = owner->second;
    }
    ReadFlexProperties(f, &entry);
    out->push_back(std::move(entry));
  }
  return true;
}

// Depth-first over the body tree with an explicit stack: cables nest one body
// per segment, deeper than this walker's frame allows recursively.
bool CollectMujocoFlexcompsJson(const pugi::xml_node& worldbody, const Context& ctx,
                                const fs::path& mesh_dir, Json* out, std::string* err) {
  struct Item {
    pugi::xml_node node;
    std::vector<double> world;
  };
  std::vector<Item> stack{{worldbody, IdentityMatrix()}};
  bool world = true;
  while (!stack.empty()) {
    Item item = std::move(stack.back());
    stack.pop_back();
    if (!CollectBodyFlexcompsJson(item.node, item.world, ctx, world, mesh_dir, out, err))
      return false;
    world = false;
    std::vector<pugi::xml_node> children = Children(item.node, "body");
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      stack.push_back({*it, MultiplyMatrix(item.world, PoseMatrix(*it, ctx))});
  }
  return true;
}

// Raw <deformable><flex> (newton profile): each vertex follows a body. A
// vertex body that is only a point mass (three independent slide joints, or
// a free joint, an inertial and nothing else, referenced by no other
// element) is absorbed into the soft body and dropped; `world` and static
// world children pin their vertex to the world; any other body keeps its rigid body and pins
// the vertex to it. Writes `consumed` for VisitMujocoBody.
bool CollectMujocoRawFlexesJson(const pugi::xml_node& root,
                                const std::vector<pugi::xml_node>& worldbodies,
                                const Context& ctx, Json* out, std::set<const void*>* consumed,
                                std::string* err) {
  std::vector<pugi::xml_node> flexes;
  for (const auto& deformable : Children(root, "deformable"))
    for (const auto& f : Children(deformable, "flex"))
      if (HasAttr(f, "body") && HasAttr(f, "element")) flexes.push_back(f);
  if (flexes.empty()) return true;
  // Named bodies with their world frames and parents.
  struct BodyInfo {
    pugi::xml_node node;
    std::vector<double> world;
    bool world_child;
  };
  std::map<std::string, BodyInfo> bodies;
  bodies.emplace("world", BodyInfo{pugi::xml_node(), IdentityMatrix(), true});
  {
    struct Item {
      pugi::xml_node node;
      std::vector<double> world;
      bool world_child;
    };
    std::vector<Item> stack;
    for (const auto& wb : worldbodies)
      for (const auto& b : Children(wb, "body"))
        stack.push_back({b, PoseMatrix(b, ctx), true});
    while (!stack.empty()) {
      Item item = std::move(stack.back());
      stack.pop_back();
      if (HasAttr(item.node, "name"))
        bodies.emplace(Attr(item.node, "name"), BodyInfo{item.node, item.world, item.world_child});
      for (const auto& c : Children(item.node, "body"))
        stack.push_back({c, MultiplyMatrix(item.world, PoseMatrix(c, ctx)), false});
    }
  }
  // Names other elements refer to: such bodies and joints must stay.
  std::set<std::string> referenced;
  {
    static const char* const kKeys[] = {"body", "body1", "body2", "objname", "refname", "target",
                                        "joint", "joint1", "joint2", "jointinparent"};
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
      const pugi::xml_node node = stack.back();
      stack.pop_back();
      const bool flex = std::string(node.name()) == "flex";
      for (const char* key : kKeys)
        if (!(flex && std::strcmp(key, "body") == 0) && node.attribute(key))
          referenced.insert(node.attribute(key).as_string());
      for (auto c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) stack.push_back(c);
    }
  }
  bool keyframes = false;
  for (const auto& kf : Children(root, "keyframe"))
    for (const auto& key : Children(kf, "key")) keyframes = keyframes || HasAttr(key, "qpos");
  // 1 free point, 0 static (pinned to the world), -1 keep the rigid body.
  auto classify = [&](const BodyInfo& b, double* mass) {
    const auto& n = b.node;
    if (!n) return 0;  // "world": a fixed vertex
    if (referenced.count(Attr(n, "name")) || Child(n, "body") || Child(n, "geom") ||
        Child(n, "site") || Child(n, "camera") || Child(n, "light") || Child(n, "frame") ||
        Child(n, "flexcomp") || Child(n, "composite") || Child(n, "replicate") ||
        MjcBoolAttr(Attr(n, "mocap", "false")))
      return -1;
    const auto inertial = Child(n, "inertial");
    *mass = inertial ? ParseDoubleAttr(inertial, "mass", 0.0) : 0.0;
    std::vector<std::array<double, 3>> axes;
    bool free = static_cast<bool>(Child(n, "freejoint"));
    for (const auto& j : Children(n, "joint")) {
      if (HasAttr(j, "name") && referenced.count(Attr(j, "name"))) return -1;
      const std::string type = ToLower(Attr(j, "type", "hinge"));
      if (type == "free") free = true;
      else if (type == "slide") axes.push_back(ParseDouble3(Attr(j, "axis"), {{0, 0, 1}}));
      else return -1;
    }
    // A free joint only exists on world children; three slide joints free a
    // point below any body too (MuJoCo's flexcomp output nests them there).
    if (free && axes.empty()) return *mass > 0 ? 1 : -1;
    if (free) return -1;
    if (axes.empty()) return b.world_child ? 0 : -1;
    if (axes.size() != 3 || !(*mass > 0)) return -1;
    const auto& a = axes[0];
    const auto& b2 = axes[1];
    const auto& c = axes[2];
    const double det = a[0] * (b2[1] * c[2] - b2[2] * c[1]) - a[1] * (b2[0] * c[2] - b2[2] * c[0]) +
                       a[2] * (b2[0] * c[1] - b2[1] * c[0]);
    return std::fabs(det) > 1e-9 ? 1 : -1;
  };
  for (const auto& f : flexes) {
    const std::string name = Attr(f, "name");
    auto skip = [&](const std::string& message, const char* code) {
      if (UnsupportedXML(f, "flex `" + name + "`: " + message, code, "mjcf.deformable",
                         active_reader->root_source, false))
        return true;
      if (err) *err = active_reader->failure;
      return false;
    };
    if (keyframes) {
      // Dropping the vertex bodies' joints would shift keyframe qpos.
      if (!skip("keyframes address the vertex bodies' joints", "mjcf.flex_keyframes")) return false;
      continue;
    }
    if (HasAttr(f, "node") || HasAttr(f, "dof")) {
      if (!skip("interpolated (node/dof) flexes are not supported", "mjcf.flex_interpolated"))
        return false;
      continue;
    }
    const int dim = ToInt32(ParseDoubleAttr(f, "dim", 2.0), "dim");
    std::vector<std::string> names;
    {
      std::istringstream in(Attr(f, "body"));
      for (std::string b; in >> b;) names.push_back(b);
    }
    const std::vector<double> vertex = ParseDoubles(Attr(f, "vertex"));
    std::vector<int> element;
    bool integral_elements = true;
    for (double e : ParseDoubles(Attr(f, "element"))) {
      if (!std::isfinite(e) || std::trunc(e) != e || e < INT32_MIN || e > INT32_MAX) {
        integral_elements = false;
        break;
      }
      element.push_back(static_cast<int>(e));
    }
    if (!integral_elements) {
      if (!skip("element indices must be int32 values", "mjcf.flex_invalid")) return false;
      continue;
    }
    const size_t npnt = vertex.empty() ? names.size() : vertex.size() / 3;
    if (dim < 1 || dim > 3 || names.empty() || vertex.size() % 3 ||
        (names.size() != 1 && names.size() != npnt) || element.empty() ||
        element.size() % size_t(dim + 1)) {
      if (!skip("body, vertex and element sizes are invalid", "mjcf.flex_invalid")) return false;
      continue;
    }
    bool valid = true;
    for (int e : element) valid = valid && e >= 0 && size_t(e) < npnt;
    for (const auto& b : names) valid = valid && bodies.count(b);
    if (!valid) {
      if (!skip("element or body reference is missing", "mjcf.flex_invalid")) return false;
      continue;
    }
    Json points = Json::array(), pins = Json::array(), pin_coords = Json::array(),
         pin_bodies = Json::array(), masses = Json::array();
    std::vector<const void*> absorbed;
    double damping = 0.0;
    size_t free_points = 0;
    for (size_t i = 0; i < npnt; ++i) {
      const std::string& bname = names.size() == 1 ? names[0] : names[i];
      const BodyInfo& b = bodies.at(bname);
      const double local[3] = {vertex.empty() ? 0.0 : vertex[3 * i],
                               vertex.empty() ? 0.0 : vertex[3 * i + 1],
                               vertex.empty() ? 0.0 : vertex[3 * i + 2]};
      const auto& m = b.world;
      points.push_back(Json::array({m[0] * local[0] + m[4] * local[1] + m[8] * local[2] + m[12],
                                    m[1] * local[0] + m[5] * local[1] + m[9] * local[2] + m[13],
                                    m[2] * local[0] + m[6] * local[1] + m[10] * local[2] + m[14]}));
      double mass = 0.0;
      // A single body carries every vertex rigidly: pin them all to it.
      const int kind = names.size() == 1 ? -1 : classify(b, &mass);
      masses.push_back(kind == 1 ? mass : 0.0);
      if (kind == 1) {
        ++free_points;
        absorbed.push_back(b.node.internal_object());
        for (const auto& j : Children(b.node, "joint"))
          damping = std::max(damping, ParseDoubleAttr(j, "damping", 0.0));
        continue;
      }
      if (kind == 0 && b.node) absorbed.push_back(b.node.internal_object());
      pins.push_back(static_cast<int>(i));
      pin_coords.push_back(Json::array({local[0], local[1], local[2]}));
      pin_bodies.push_back(kind == 0 || !b.node ? std::string() : bname);
    }
    if (!free_points) {
      if (!skip("no vertex body is a free point mass", "mjcf.flex_rigid")) return false;
      continue;
    }
    // Pinned vertices have no mass of their own in MuJoCo; give them the
    // mean free mass so engines deriving a density see a uniform body (as
    // the flexcomp path's equal shares).
    double free_mass = 0.0;
    for (size_t i = 0; i < masses.size(); ++i) free_mass += masses[i].get<double>();
    const double mean = free_mass / double(free_points);
    std::set<int> pinned_ids;
    for (size_t i = 0; i < pins.size(); ++i) pinned_ids.insert(pins[i].get<int>());
    for (int id : pinned_ids) masses[size_t(id)] = mean;
    for (const void* b : absorbed) consumed->insert(b);
    bool periodic = false;
    if (dim == 1 && element.size() == 2 * npnt) periodic = element.back() == 0;
    Json entry = {{"name", name.empty() ? "flex_" + std::to_string(out->size()) : name},
                  {"type", "flex"},
                  {"dim", dim},
                  {"points", std::move(points)},
                  {"elements", Array(std::vector<double>(element.begin(), element.end()))},
                  {"masses", std::move(masses)},
                  {"radius", ParseDoubleAttr(f, "radius", 0.005)},
                  {"periodic", periodic}};
    if (!pins.empty()) {
      entry["pins"] = std::move(pins);
      entry["pinCoords"] = std::move(pin_coords);
      entry["pinBodies"] = std::move(pin_bodies);
    }
    ReadFlexProperties(f, &entry);
    if (damping > 0)
      active_reader->Warn("flex `" + name + "`: vertex joint damping is dropped with the "
                              "absorbed point bodies",
                          "mjcf.flex_vertex_damping");
    out->push_back(std::move(entry));
  }
  return true;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
