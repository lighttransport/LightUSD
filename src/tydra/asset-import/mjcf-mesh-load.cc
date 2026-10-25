// SPDX-License-Identifier: Apache-2.0
// Mesh asset loading (STL, OBJ, GMSH/MSH, VDB), preload and the mesh cache.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
float ReadFloatLE(const uint8_t* bytes, size_t offset) {
  float v = 0.0f;
  std::memcpy(&v, bytes + offset, sizeof(float));
  return v;
}

// MuJoCo removes repeated STL vertices and computes one normal per vertex:
// face normals weighted by area, and (unless smoothnormal) a face whose normal
// deviates from the vertex average by more than acos(0.8) is left out so
// sharp edges stay sharp. Reproduce that so the USD mesh is indexed (an STL
// is a triangle soup, ~6x the unique positions) and shades like MuJoCo.
void ComputeMujocoVertexNormals(Mesh* mesh, bool smoothnormal) {
  const size_t nv = mesh->positions.size() / 3;
  const size_t nf = mesh->indices.size() / 3;
  std::vector<double> acc(nv * 3, 0.0), face(nf * 3, 0.0);
  auto P = [&](int32_t i, int c) {
    return double(mesh->positions[size_t(i) * 3 + c]);
  };
  for (size_t f = 0; f < nf; ++f) {
    const int32_t a = mesh->indices[f * 3], b = mesh->indices[f * 3 + 1],
                  c = mesh->indices[f * 3 + 2];
    const double e1[3] = {P(b, 0) - P(a, 0), P(b, 1) - P(a, 1),
                          P(b, 2) - P(a, 2)};
    const double e2[3] = {P(c, 0) - P(a, 0), P(c, 1) - P(a, 1),
                          P(c, 2) - P(a, 2)};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                         e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};  // length = 2 * area
    for (int k = 0; k < 3; ++k) face[f * 3 + k] = n[k];
    for (int32_t v : {a, b, c}) {
      for (int k = 0; k < 3; ++k) acc[size_t(v) * 3 + k] += n[k];
    }
  }
  auto normalized = [](const double* v, double out[3]) {
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int k = 0; k < 3; ++k) out[k] = len > 0.0 ? v[k] / len : 0.0;
    return len > 0.0;
  };
  // Smooth normals use the accumulated sums as they are; only the sharp-edge
  // pass needs its own copy to subtract excluded faces from.
  std::vector<double> sharp;
  if (!smoothnormal) sharp = acc;
  std::vector<double>& sum = smoothnormal ? acc : sharp;
  if (!smoothnormal) {
    for (size_t f = 0; f < nf; ++f) {
      double uf[3];
      if (!normalized(&face[f * 3], uf)) continue;
      for (int j = 0; j < 3; ++j) {
        const size_t v = size_t(mesh->indices[f * 3 + j]);
        double avg[3];
        if (!normalized(&acc[v * 3], avg)) continue;
        if (avg[0] * uf[0] + avg[1] * uf[1] + avg[2] * uf[2] < 0.8) {
          for (int k = 0; k < 3; ++k) sum[v * 3 + k] -= face[f * 3 + k];
        }
      }
    }
  }
  mesh->normals.assign(nv * 3, 0.0f);
  for (size_t v = 0; v < nv; ++v) {
    double n[3];
    // Every face was excluded (a spike vertex): fall back to the full average.
    if (!normalized(&sum[v * 3], n)) normalized(&acc[v * 3], n);
    for (int k = 0; k < 3; ++k) mesh->normals[v * 3 + k] = float(n[k]);
  }
}

// ASCII STL: welded by exact float bit pattern like the binary path.
bool LoadASCIISTL(std::string_view text, const fs::path& filename, bool smoothnormal,
                  Mesh* mesh, std::string* err) {
  struct Key {
    uint32_t x, y, z;
    bool operator<(const Key& o) const {
      return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z);
    }
  };
  std::map<Key, int32_t> welded;
  mesh->positions.clear();
  mesh->indices.clear();
  size_t pos = 0, corners = 0;
  auto skip_space = [&]() {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  };
  auto word = [&]() {
    skip_space();
    const size_t begin = pos;
    while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    return text.substr(begin, pos - begin);
  };
  while (pos < text.size()) {
    const std::string_view token = word();
    if (token.empty()) break;
    if (token != "vertex") continue;
    float v[3];
    for (float& c : v) {
      skip_space();
      const char* begin = text.data() + pos;
      const auto parsed = std::from_chars(begin, text.data() + text.size(), c);
      if (parsed.ec != std::errc() || !std::isfinite(c)) {
        if (err) *err = "Invalid ASCII STL vertex: " + filename.string();
        return false;
      }
      pos += size_t(parsed.ptr - begin);
    }
    if (!active_reader->Charge(96, err)) return false;
    Key key;
    std::memcpy(&key.x, &v[0], 4);
    std::memcpy(&key.y, &v[1], 4);
    std::memcpy(&key.z, &v[2], 4);
    auto found = welded.find(key);
    if (found == welded.end()) {
      if (mesh->positions.size() / 3 >= size_t(INT32_MAX)) {
        if (err) *err = "ASCII STL has too many vertices: " + filename.string();
        return false;
      }
      found = welded.emplace(key, static_cast<int32_t>(mesh->positions.size() / 3)).first;
      mesh->positions.insert(mesh->positions.end(), v, v + 3);
    }
    mesh->indices.push_back(found->second);
    ++corners;
  }
  if (corners == 0 || corners % 3) {
    if (err) *err = "ASCII STL has no complete triangles: " + filename.string();
    return false;
  }
  ComputeMujocoVertexNormals(mesh, smoothnormal);
  return true;
}

bool LoadBinarySTL(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                   std::string* err) {
  MappedAsset asset;
  std::string resolved;
  if (!active_reader->Map(filename.string(), active_reader->root_source, &asset, &resolved,
                          err))
    return false;
  const uint8_t* bytes = asset.data();
  if (asset.size() < 84) {
    if (err) *err = "STL is too small: " + filename.string();
    return false;
  }
  uint32_t tri_count = 0;
  std::memcpy(&tri_count, bytes + 80, sizeof(uint32_t));
  const uint64_t expected = 84ull + uint64_t(tri_count) * 50ull;
  if (expected != asset.size()) {
    // ASCII STL ("solid ... facet ... vertex x y z ... endsolid").
    std::string_view text = asset.text();
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && text.compare(first, 5, "solid") == 0)
      return LoadASCIISTL(text, filename, smoothnormal, mesh, err);
    if (err)
      *err = "STL is neither binary with an exact triangle count nor ASCII: " +
             filename.string();
    return false;
  }

  if (!active_reader->ChargeArray(size_t(tri_count), 3 * 96, err)) return false;
  // Weld bit-identical positions (MuJoCo's repeated-vertex removal) through
  // an open-addressed table; indices follow first occurrence.
  struct Slot {
    uint32_t x, y, z;
    int32_t index;  // -1: empty
  };
  // Sized for the typical ~tri_count/2 unique vertices and doubled when half
  // full (a soup has 3 per triangle): far less to clear and to cache than a
  // worst-case table. Indices do not depend on the table size.
  size_t capacity = 16;
  while (capacity < size_t(tri_count)) capacity <<= 1;
  std::vector<Slot> table(capacity, Slot{0, 0, 0, -1});
  size_t mask = capacity - 1;
  size_t unique = 0;
  const auto hash = [](uint32_t x, uint32_t y, uint32_t z) {
    uint64_t h = (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^
                 (uint64_t(z) * 0x165667B19E3779F9ull);
    h ^= h >> 29;
    return size_t(h);
  };
  const auto grow = [&]() {
    std::vector<Slot> old;
    old.swap(table);
    capacity <<= 1;
    mask = capacity - 1;
    table.assign(capacity, Slot{0, 0, 0, -1});
    for (const Slot& slot : old) {
      if (slot.index < 0) continue;
      size_t at = hash(slot.x, slot.y, slot.z) & mask;
      while (table[at].index >= 0) at = (at + 1) & mask;
      table[at] = slot;
    }
  };
  mesh->positions.clear();
  mesh->indices.clear();
  mesh->indices.reserve(size_t(tri_count) * 3);
  mesh->positions.reserve(size_t(tri_count) * 3 / 2 + 3);
  for (uint32_t i = 0; i < tri_count; i++) {
    const size_t base = 84ull + size_t(i) * 50ull;
    for (size_t v = 0; v < 3; v++) {
      const size_t vo = base + 12 + v * 12;
      uint32_t x, y, z;
      std::memcpy(&x, bytes + vo + 0, 4);
      std::memcpy(&y, bytes + vo + 4, 4);
      std::memcpy(&z, bytes + vo + 8, 4);
      size_t at = hash(x, y, z) & mask;
      while (table[at].index >= 0 && !(table[at].x == x && table[at].y == y && table[at].z == z))
        at = (at + 1) & mask;
      if (table[at].index < 0) {
        table[at] = Slot{x, y, z, static_cast<int32_t>(mesh->positions.size() / 3)};
        for (size_t k = 0; k < 3; ++k) {
          const float c = ReadFloatLE(bytes, vo + 4 * k);
          if (!std::isfinite(c)) {
            if (err) *err = "STL vertex is not finite";
            return false;
          }
          mesh->positions.push_back(c);
        }
        mesh->indices.push_back(table[at].index);
        if (++unique * 2 > capacity) grow();
        continue;
      }
      mesh->indices.push_back(table[at].index);
    }
  }
  ComputeMujocoVertexNormals(mesh, smoothnormal);
  return true;
}

// One polygon as MuJoCo's OBJ decoder sees it: tinyobjloader 2945a96 (the
// version MuJoCo 3.16 pins), float as its real_t. Quads split along the
// shorter diagonal; larger polygons are ear-clipped in the plane picked from
// the first non-degenerate corner. A fan differs for concave polygons, and legacy
// inertia sums |volume| per triangle, so the masses would too. Like
// tinyobjloader, a polygon the clipper cannot finish drops its remainder.
// Appends triangles as corner numbers 0..n-1 of the polygon. `work` bounds
// the clipper's O(n^3) worst case: when it runs out the polygon is a fan and
// false is returned through *exhausted.
bool ClipPolygon(const std::vector<float>& pos, const int32_t* poly, size_t n,
                 std::vector<int32_t>* tri, size_t* work, bool* exhausted) {
  for (size_t k = 0; k < n; ++k)
    if (poly[k] < 0 || size_t(poly[k]) * 3 + 2 >= pos.size()) return false;
  auto at = [&](int32_t vi, size_t axis) { return pos[size_t(vi) * 3 + axis]; };
  if (n == 4) {
    float d02 = 0, d13 = 0;
    for (size_t a = 0; a < 3; ++a) {
      const float e02 = at(poly[2], a) - at(poly[0], a), e13 = at(poly[3], a) - at(poly[1], a);
      d02 += e02 * e02;
      d13 += e13 * e13;
    }
    if (d02 < d13) tri->insert(tri->end(), {0, 1, 2, 0, 2, 3});
    else tri->insert(tri->end(), {0, 1, 3, 1, 2, 3});
    return true;
  }
  size_t axes[2] = {1, 2};
  for (size_t k = 0; k < n; ++k) {
    const int32_t i0 = poly[k % n], i1 = poly[(k + 1) % n], i2 = poly[(k + 2) % n];
    const float e0x = at(i1, 0) - at(i0, 0), e0y = at(i1, 1) - at(i0, 1), e0z = at(i1, 2) - at(i0, 2);
    const float e1x = at(i2, 0) - at(i1, 0), e1y = at(i2, 1) - at(i1, 1), e1z = at(i2, 2) - at(i1, 2);
    const float cx = std::fabs(e0y * e1z - e0z * e1y), cy = std::fabs(e0z * e1x - e0x * e1z),
                cz = std::fabs(e0x * e1y - e0y * e1x);
    const float epsilon = std::numeric_limits<float>::epsilon();
    if (cx > epsilon || cy > epsilon || cz > epsilon) {
      if (!(cx > cy && cx > cz)) {
        axes[0] = 0;
        if (cz > cx && cz > cy) axes[1] = 1;
      }
      break;
    }
  }
  std::vector<int32_t> rest(n);  // remaining corners
  for (size_t k = 0; k < n; ++k) rest[k] = int32_t(k);
  size_t guess = 0, iterations = n, previous = n;
  const size_t first = tri->size();
  while (rest.size() > 3 && iterations > 0) {
    const size_t m = rest.size();
    if (work) {
      if (*work < m) {
        tri->resize(first);
        for (size_t k = 1; k + 1 < n; ++k) tri->insert(tri->end(), {0, int32_t(k), int32_t(k + 1)});
        if (exhausted) *exhausted = true;
        return true;
      }
      *work -= m;
    }
    if (guess >= m) guess -= m;
    if (previous != m) {
      previous = m;
      iterations = m;
    } else {
      --iterations;
    }
    int32_t ind[3];
    float vx[3], vy[3];
    for (size_t k = 0; k < 3; ++k) {
      ind[k] = rest[(guess + k) % m];
      vx[k] = at(poly[ind[k]], axes[0]);
      vy[k] = at(poly[ind[k]], axes[1]);
    }
    const float cross = (vx[1] - vx[0]) * (vy[2] - vy[1]) - (vy[1] - vy[0]) * (vx[2] - vx[1]);
    // (tinyobjloader's per-candidate "area" term, not the polygon's.)
    const float area = (vx[0] * vy[1] - vy[0] * vx[1]) * 0.5f;
    if (cross * area < 0.0f) {
      ++guess;
      continue;
    }
    bool overlap = false;
    for (size_t other = 3; other < m && !overlap; ++other) {
      const int32_t o = poly[rest[(guess + other) % m]];
      const float tx = at(o, axes[0]), ty = at(o, axes[1]);
      bool inside = false;
      for (size_t i = 0, j = 2; i < 3; j = i++)
        if (((vy[i] > ty) != (vy[j] > ty)) &&
            (tx < (vx[j] - vx[i]) * (ty - vy[i]) / (vy[j] - vy[i]) + vx[i]))
          inside = !inside;
      overlap = inside;
    }
    if (overlap) {
      ++guess;
      continue;
    }
    tri->insert(tri->end(), ind, ind + 3);
    rest.erase(rest.begin() + std::ptrdiff_t((guess + 1) % m));
  }
  if (rest.size() == 3) tri->insert(tri->end(), rest.begin(), rest.end());
  return true;
}

bool TriangulateLikeMuJoCo(Mesh* mesh) {
  if (mesh->counts.empty()) return true;
  const bool corner_normals = mesh->normal_indices.size() == mesh->indices.size();
  const bool corner_uvs = mesh->uv_indices.size() == mesh->indices.size();
  const bool face_materials = mesh->materials.size() == mesh->counts.size();
  std::vector<int32_t> indices, normal_indices, uv_indices, materials, corners;
  size_t work = size_t(1) << 28, dropped = 0;
  bool exhausted = false;
  size_t offset = 0;
  for (size_t face = 0; face < mesh->counts.size(); ++face) {
    const int32_t count = mesh->counts[face];
    if (count < 3 || offset + size_t(count) > mesh->indices.size()) return false;
    corners.clear();
    if (count > kMuJoCoMaxObjPolygon) {
      ++dropped;
      offset += size_t(count);
      continue;
    }
    if (count == 3) corners = {0, 1, 2};
    else if (!ClipPolygon(mesh->positions, mesh->indices.data() + offset, size_t(count), &corners,
                          &work, &exhausted))
      return false;
    for (int32_t c : corners) {
      indices.push_back(mesh->indices[offset + size_t(c)]);
      if (corner_normals) normal_indices.push_back(mesh->normal_indices[offset + size_t(c)]);
      if (corner_uvs) uv_indices.push_back(mesh->uv_indices[offset + size_t(c)]);
    }
    if (face_materials) materials.insert(materials.end(), corners.size() / 3, mesh->materials[face]);
    offset += size_t(count);
  }
  if (offset != mesh->indices.size()) return false;
  if (exhausted)
    active_reader->Warn("OBJ polygons exceed the triangulation work limit; the rest are fans",
                        "mjcf.obj_polygon_fan");
  if (dropped)
    active_reader->Warn(std::to_string(dropped) + " OBJ polygon(s) of more than " +
                            std::to_string(kMuJoCoMaxObjPolygon) +
                            " vertices dropped, as MuJoCo's OBJ decoder drops them",
                        "mjcf.obj_polygon_dropped");
  mesh->counts.assign(indices.size() / 3, 3);
  mesh->indices = std::move(indices);
  if (corner_normals) mesh->normal_indices = std::move(normal_indices);
  if (corner_uvs) mesh->uv_indices = std::move(uv_indices);
  if (face_materials) mesh->materials = std::move(materials);
  return true;
}

bool LoadOBJ(const fs::path& filename, bool smoothnormal, Mesh* mesh, std::string* err) {
  MappedAsset asset;
  std::string resolved;
  if (!active_reader->Map(filename.string(), active_reader->root_source, &asset, &resolved,
                          err))
    return false;
  const std::string_view text = asset.text();
  OBJ obj;
  const bool dae = ToLower(filename.extension().string()) == ".dae";
  // Non-legacy profiles triangulate OBJ polygons as MuJoCo does, so the
  // collider and the inferred mass properties match the compiled model.
  const bool mujoco_faces = !dae && active_reader->options.profile != "legacy";
  if (!(dae ? ReadDAE(text, filename.string(), *active_reader, &obj, err)
            : ReadOBJ(text, filename.string(), *active_reader, !mujoco_faces, &obj, err,
                      /* collect_vertices = */ mujoco_faces)))
    return false;
  if (mujoco_faces) {
    // Untriangulated reads also return line/point primitives.
    const size_t before = obj.meshes.size();
    obj.meshes.erase(std::remove_if(obj.meshes.begin(), obj.meshes.end(),
                                    [](const Mesh& m) { return m.type != "Mesh"; }),
                     obj.meshes.end());
    if (obj.meshes.size() != before)
      active_reader->Warn(
          "OBJ line, point or free-form primitives are not converted to "
          "polygon meshes");
    if (obj.meshes.empty() && !obj.vertices.empty()) {
      // MuJoCo hulls a face-less mesh asset: mjCMesh::Process builds the
      // convex hull when face_ is empty and copies its faces, so a point-cloud
      // OBJ is a valid mesh with the hull's volume and inertia. Match it (and
      // its diagnostics for degenerate point sets).
      std::vector<int32_t> hull;
      std::string why;
      const HullStatus status = BuildConvexHull(
          obj.vertices.data(), obj.vertices.size() / 3, &hull, &why);
      if (status != HullStatus::Ok) {
        if (err) *err = "OBJ has no faces and its vertices are " + why;
        return false;
      }
      Mesh mesh;
      mesh.name = filename.stem().string();
      mesh.type = "Mesh";
      std::vector<uint8_t> used(obj.vertices.size() / 3, 0);
      for (int32_t index : hull)
        if (index >= 0 && size_t(index) < used.size()) used[size_t(index)] = 1;
      std::vector<int32_t> remap(used.size(), -1);
      for (size_t v = 0; v < used.size(); ++v) {
        if (!used[v]) continue;
        remap[v] = static_cast<int32_t>(mesh.positions.size() / 3);
        mesh.positions.insert(mesh.positions.end(), &obj.vertices[3 * v],
                              &obj.vertices[3 * v] + 3);
      }
      mesh.indices.reserve(hull.size());
      for (int32_t index : hull) mesh.indices.push_back(remap[size_t(index)]);
      mesh.counts.assign(hull.size() / 3, 3);
      mesh.materials.assign(hull.size() / 3, -1);
      // No authored normals: MuJoCo computes them from the hull faces
      // (MakeNormal), honouring <mesh smoothnormal>.
      ComputeMujocoVertexNormals(&mesh, smoothnormal);
      active_reader->Warn("MJCF mesh " + filename.filename().string() +
                              " has no faces; converted to the convex hull of its " +
                              std::to_string(mesh.positions.size() / 3) +
                              " vertices, as MuJoCo does",
                          "mjcf.mesh_convex_hull", filename.string());
      obj.meshes.push_back(std::move(mesh));
    }
    if (obj.meshes.empty()) {
      if (err) *err = "OBJ contains no polygon faces";
      return false;
    }
    for (auto& part : obj.meshes)
      if (!TriangulateLikeMuJoCo(&part)) {
        if (err) *err = "Invalid OBJ polygon in " + filename.string();
        return false;
      }
  }
  const int32_t material_base = static_cast<int32_t>(active_materials->size());
  for (auto& mat : obj.materials) {
    mat["name"] = "obj_" + std::to_string(active_materials->size()) + "_" +
                  mat["name"].get<std::string>();
    active_materials->push_back(mat);
  }
  // MuJoCo's OBJ decoder converts only the first object of a mesh asset
  // (plugin/obj_decoder, GetShapes()[0]); non-legacy profiles match it so
  // geometry, colliders and inferred inertia agree with the simulator.
  if (active_reader->options.profile != "legacy" && obj.meshes.size() > 1) {
    active_reader->Warn("MJCF mesh " + filename.filename().string() + " has " +
                            std::to_string(obj.meshes.size()) +
                            " OBJ objects; MuJoCo uses only the first",
                        "mjcf.obj_first_object", filename.string());
    obj.meshes.resize(1);
  }
  for (auto& part : obj.meshes) {
    const int32_t base = static_cast<int32_t>(mesh->positions.size() / 3);
    mesh->positions.insert(mesh->positions.end(), part.positions.begin(),
                           part.positions.end());
    mesh->normals.insert(mesh->normals.end(), part.normals.begin(),
                         part.normals.end());
    mesh->uvs.insert(mesh->uvs.end(), part.uvs.begin(), part.uvs.end());
    for (int32_t i : part.indices) mesh->indices.push_back(base + i);
    mesh->counts.insert(mesh->counts.end(), part.counts.begin(),
                        part.counts.end());
    for (int32_t i : part.materials)
      mesh->materials.push_back(i < 0 ? -1 : material_base + i);
  }
  if (mesh->uvs.size() != mesh->positions.size() / 3 * 2) mesh->uvs.clear();
  return true;
}

void PreloadJobs(const std::vector<std::pair<PreloadKey, PreloadJob>>& jobs) {
#if (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)) || defined(__wasi__)
  (void)jobs;
#else
  const auto& keys = jobs;
  Reader& parent = *active_reader;
  if (!active_mesh_preload || parent.options.open || parent.options.read ||
      !parent.failure.empty() || parent.options.threads == 1 || keys.size() < 2)
    return;
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const size_t workers = std::min<size_t>(
      keys.size(), parent.options.threads > 1 ? size_t(parent.options.threads)
                                              : std::min(hardware, 8u));
  if (workers < 2) return;
  const size_t remaining = parent.options.max_memory -
                           std::min(parent.bytes, parent.options.max_memory);
  Options child_options = parent.options;
  child_options.report = nullptr;
  child_options.progress = nullptr;
  child_options.threads = 1;
  // Workers parse concurrently, so each gets a share of what is left: the
  // transient peaks together stay within the import budget. A mesh that does
  // not fit its share is parsed again serially at first use.
  child_options.max_memory = remaining / workers;
  std::vector<PreloadedMesh> results(keys.size());
  std::vector<char> kept(keys.size(), 0);
  std::atomic<size_t> next{0}, committed{0};
  std::atomic<bool> stop{false};
  const std::thread::id caller = std::this_thread::get_id();
  auto work = [&]() {
    for (size_t i = next++; i < keys.size(); i = next++) {
      if (stop.load() || committed.load() >= remaining) return;
      // Cancellation: only the calling thread may run the progress callback.
      if (parent.options.progress && std::this_thread::get_id() == caller &&
          !parent.options.progress("import", parent.bytes, parent.options.max_memory)) {
        stop = true;
        return;
      }
      Reader child(child_options);
      child.packages = parent.packages;
      child.extra_roots = parent.extra_roots;
      child.root_source = parent.root_source;
      child.allow_unanchored = parent.allow_unanchored;
      child.record_accesses = true;
      Json materials = Json::array();
      active_reader = &child;
      active_materials = &materials;
      std::string err;
      PreloadedMesh& out = results[i];
      if (!jobs[i].second(&out, &err) ||
          !child.failure.empty() || !child.textures.empty() || !child.texture_requests.empty() || !child.meshes.empty() ||
          child.preserved.size() || committed.fetch_add(child.bytes) + child.bytes > remaining) {
        out = PreloadedMesh();
        continue;
      }
      out.materials = std::move(materials);
      out.accesses = std::move(child.accesses);
      out.warnings = std::move(child.recorded_warnings);
      out.bytes = child.bytes;
      out.peak = child.peak_bytes;
      kept[i] = 1;
    }
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < workers; ++t) pool.emplace_back(work);
  {
    // The calling thread works too; restore its import state afterwards.
    Reader* reader = active_reader;
    Json* materials = active_materials;
    work();
    active_reader = reader;
    active_materials = materials;
  }
  for (auto& thread : pool) thread.join();
  if (stop.load()) {
    parent.failure = "Import cancelled";
    return;
  }
  for (size_t i = 0; i < keys.size(); ++i)
    if (kept[i]) active_mesh_preload->emplace(keys[i].first, std::move(results[i]));
#endif
}

void PreloadMeshFiles(const std::vector<PreloadKey>& keys) {
  std::vector<std::pair<PreloadKey, PreloadJob>> jobs;
  for (const auto& key : keys)
    jobs.emplace_back(key, [key](PreloadedMesh* out, std::string* err) {
      return LoadMeshFileUncached(fs::path(key.first), key.second, &out->mesh, err);
    });
  PreloadJobs(jobs);
}

// Replay a preloaded load's reads, warnings, charges and materials into the
// import; false leaves it to a serial load (for example when the budget has
// since been used up).
// `keep` leaves the entry for later uses that a serial import would also
// read and parse again (URDF meshes are not cached).
bool TakePreloaded(const PreloadKey& key, PreloadedMesh* out, std::string* err,
                   bool keep) {
  if (!active_mesh_preload) return false;
  auto found = active_mesh_preload->find(key);
  if (found == active_mesh_preload->end()) return false;
  PreloadedMesh preloaded;
  if (keep) {
    preloaded = found->second;
  } else {
    preloaded = std::move(found->second);
    active_mesh_preload->erase(found);
  }
  Reader& reader = *active_reader;
  // A serial parse fails once its transient peak exceeds the budget left.
  if (!reader.failure.empty() ||
      std::max(preloaded.bytes, preloaded.peak) >
          reader.options.max_memory - std::min(reader.bytes, reader.options.max_memory))
    return false;
  for (size_t k = 0; k < preloaded.materials.size(); ++k) {
    const Json& name = preloaded.materials[k]["name"];
    if (!name.is_string() || name.get<std::string>().rfind("obj_" + std::to_string(k) + "_", 0) != 0)
      return false;
  }
  const size_t base = reader.bytes;
  size_t read = 0;
  for (const auto& access : preloaded.accesses) {
    if (access.read && access.success && !access.cached && !access.texture) {
      if (!reader.Account(access.name, access.anchor, access.identifier, access.bytes, err))
        return false;
      read += access.bytes;
    } else if (reader.options.report) {
      // A failed read (for example a missing MTL) charges nothing.
      reader.options.report->Dependency(access.name, access.anchor, access.identifier,
                                        access.bytes, access.read, access.cached,
                                        access.success, access.texture);
    }
  }
  for (const auto& warning : preloaded.warnings)
    reader.Warn(warning.message, warning.code.c_str(), warning.source, warning.line);
  if (reader.options.report) reader.options.report->ObserveMemory(base + preloaded.peak);
  reader.peak_bytes = std::max(reader.peak_bytes, base + preloaded.peak);
  if (preloaded.bytes > read && !reader.Charge(preloaded.bytes - read, err)) return false;
  // LoadOBJ names materials obj_<import index>_<name>; renumber from the
  // import's current count as a serial load would have.
  const int32_t offset = static_cast<int32_t>(active_materials->size());
  for (size_t k = 0; k < preloaded.materials.size(); ++k) {
    Json mat = std::move(preloaded.materials[k]);
    const std::string name = mat["name"].get<std::string>();
    const std::string prefix = "obj_" + std::to_string(k) + "_";
    mat["name"] = "obj_" + std::to_string(offset + int32_t(k)) + "_" + name.substr(prefix.size());
    active_materials->push_back(std::move(mat));
  }
  if (!preloaded.materials.empty())
    for (int32_t& index : preloaded.mesh.materials)
      if (index >= 0) index += offset;
  *out = std::move(preloaded);
  return true;
}

bool TakePreloadedMesh(const PreloadKey& key, Mesh* mesh, std::string* err) {
  PreloadedMesh preloaded;
  if (!TakePreloaded(key, &preloaded, err)) return false;
  *mesh = std::move(preloaded.mesh);
  return true;
}

// MJCF sdf colliders: a narrow-band level set of the collider mesh in its own
// frame, as a generated .vdb (sdf/<mesh>.vdb), shared by every geom using the
// mesh. Fills `out` with the asset path and the grid parameters; returns false
// (with a reason) when no level set could be built.

// The volumetric device for this import: an unavailable CUDA/HIP device
// falls back to the CPU once, with a warning.
const std::string& VolumetricDevice() {
  std::string& device = active_reader->volumetric_device;
  if (!device.empty()) return device;
  std::string why;
  if (!VolumetricDeviceAvailable(active_reader->options.volumetric_device, &device, &why))
    active_reader->Warn(why + "; using the CPU", "asset.volumetric_device_unavailable");
  return device;
}

bool MjcfSdfLevelSet(const std::string& mesh_name, const Mesh& mesh, Json* out,
                     std::string* why) {
  auto& cache = active_reader->generated_cache;
  const std::string key = "sdf:" + mesh_name;
  const auto hit = cache.find(key);
  if (hit != cache.end()) {
    *out = hit->second;
    return true;
  }
  const auto& opts = active_reader->options;
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], double(mesh.positions[i + a]));
      hi[a] = std::max(hi[a], double(mesh.positions[i + a]));
    }
  const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  if (!(extent > 0)) {
    *why = "the mesh has no extent";
    return false;
  }
  LevelSetOptions ls;
  ls.voxel_size = extent / double(opts.sdf_resolution);
  ls.exterior_width = ls.interior_width = opts.sdf_band * ls.voxel_size;
  ls.threads = opts.threads;
  ls.name = "sdf";
  ls.device = VolumetricDevice();
  // ~128 bytes per candidate voxel while building.
  const size_t budget = opts.max_memory - std::min(opts.max_memory, active_reader->bytes);
  ls.max_voxels = std::max<size_t>(100000, std::min<size_t>(budget / 128, 50000000));
  LevelSet level_set;
  std::vector<uint8_t> bytes;
  if (!MeshToLevelSet(mesh, ls, &level_set, why) || !WriteLevelSetVDB(level_set, &bytes, why))
    return false;
  if (!active_reader->Charge(bytes.size(), why)) return false;
  std::string file;
  for (char c : mesh_name)
    file += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
  std::string path = "sdf/" + (file.empty() ? std::string("mesh") : file) + ".vdb";
  for (int n = 2; active_reader->generated.count(path); ++n)
    path = "sdf/" + file + "_" + std::to_string(n) + ".vdb";
  const size_t size = bytes.size();
  active_reader->generated[path] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
  // Newton's sdfMaxResolution: voxels across the largest extent, a multiple of 8.
  const int resolution = (opts.sdf_resolution + 7) / 8 * 8;
  *out = {{"asset", path},
          {"voxelSize", ls.voxel_size},
          {"exteriorWidth", ls.exterior_width},
          {"interiorWidth", ls.interior_width},
          {"maxResolution", resolution},
          {"activeVoxels", uint64_t(level_set.active_voxels())},
          // The narrow band's reach in mesh space: the Volume's extent.
          {"extent", Json::array({lo[0] - ls.exterior_width, lo[1] - ls.exterior_width,
                                  lo[2] - ls.exterior_width, hi[0] + ls.exterior_width,
                                  hi[1] + ls.exterior_width, hi[2] + ls.exterior_width})}};
  cache[key] = *out;
  if (opts.report)
    opts.report->Diagnostic("info", "mjcf.sdf_level_set",
                            "MJCF mesh `" + mesh_name + "`: level set " + path + " (" +
                                std::to_string(size) + " bytes)",
                            active_reader->root_source);
  return true;
}

bool LoadMeshFile(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                  std::string* err) {
  if (!active_mesh_cache) return LoadMeshFileUncached(filename, smoothnormal, mesh, err);
  const auto key = std::make_pair(filename.string(), smoothnormal);
  auto found = active_mesh_cache->find(key);
  if (found == active_mesh_cache->end()) {
    Mesh loaded;
    if (!TakePreloadedMesh(key, &loaded, err)) {
      if (!active_reader->failure.empty()) return false;
      if (!LoadMeshFileUncached(filename, smoothnormal, &loaded, err)) return false;
    }
    found = active_mesh_cache->emplace(key, std::move(loaded)).first;
  }
  // The importer charged the first load; a copy is an ordinary allocation.
  *mesh = found->second;
  return true;
}

bool LoadGMSHMesh(const fs::path& filename, Mesh* mesh, std::string* err) {
  std::string text;
  if (!ReadFile(filename, &text, err)) return false;
  int dim = 0;
  std::vector<double> point;
  std::vector<int> element;
  if (!ParseGMSH(text, &dim, &point, &element, err)) {
    if (err) *err = filename.string() + ": " + *err;
    return false;
  }
  if (!AllFinite(point)) {
    if (err) *err = filename.string() + ": GMSH node is not finite";
    return false;
  }
  if (dim == 1) {
    if (err) *err = filename.string() + ": 1D GMSH meshes are not supported";
    return false;
  }
  const size_t nnode = point.size() / 3;
  for (int e : element)
    if (e < 0 || size_t(e) >= nnode) {
      if (err) *err = filename.string() + ": GMSH element index out of range";
      return false;
    }
  mesh->positions.clear();
  mesh->normals.clear();
  mesh->indices.clear();
  if (dim == 2) {
    for (double v : point) mesh->positions.push_back(float(v));
    mesh->indices.assign(element.begin(), element.end());
    return true;
  }
  struct Candidate {
    int tri[3];
    bool boundary;
  };
  std::vector<Candidate> candidates;
  std::map<std::array<int, 3>, size_t> faces;
  for (size_t t = 0; t + 3 < element.size(); t += 4) {
    int v0 = element[t], v1 = element[t + 1];
    const int v2 = element[t + 2], v3 = element[t + 3];
    const double* p0 = &point[3 * size_t(v0)];
    const double* p1 = &point[3 * size_t(v1)];
    const double* p2 = &point[3 * size_t(v2)];
    const double* p3 = &point[3 * size_t(v3)];
    const double d1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const double d2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    const double d3[3] = {p3[0] - p0[0], p3[1] - p0[1], p3[2] - p0[2]};
    const double cross[3] = {d1[1] * d2[2] - d1[2] * d2[1], d1[2] * d2[0] - d1[0] * d2[2],
                             d1[0] * d2[1] - d1[1] * d2[0]};
    if (cross[0] * d3[0] + cross[1] * d3[1] + cross[2] * d3[2] < 0) std::swap(v0, v1);
    const int tet_faces[4][3] = {{v1, v2, v3}, {v0, v3, v2}, {v0, v1, v3}, {v0, v2, v1}};
    for (const auto& f : tet_faces) {
      std::array<int, 3> key{{f[0], f[1], f[2]}};
      std::sort(key.begin(), key.end());
      const auto found = faces.find(key);
      if (found == faces.end()) {
        faces.emplace(key, candidates.size());
        candidates.push_back({{f[0], f[1], f[2]}, true});
      } else {
        candidates[found->second].boundary = false;
      }
    }
  }
  std::vector<int> remap(nnode, -1);
  for (const auto& c : candidates)
    if (c.boundary)
      for (int v : c.tri) remap[size_t(v)] = 0;
  int next = 0;
  for (size_t i = 0; i < nnode; ++i) {
    if (remap[i] < 0) continue;
    remap[i] = next++;
    for (size_t k = 0; k < 3; ++k) mesh->positions.push_back(float(point[3 * i + k]));
  }
  for (const auto& c : candidates)
    if (c.boundary)
      for (int v : c.tri) mesh->indices.push_back(remap[size_t(v)]);
  return true;
}

// MuJoCo's legacy binary MSH mesh (mjCMesh::LoadMSH): int nvertex, nnormal,
// ntexcoord, nface; float vertex[3n], normal[3n], texcoord[2n]; int face[3n].
bool LoadLegacyMSH(const fs::path& filename, Mesh* mesh, std::string* err) {
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!active_reader->Read(filename.string(), active_reader->root_source, &bytes, &resolved, err))
    return false;
  int32_t n[4] = {0, 0, 0, 0};
  if (bytes.size() >= 16) std::memcpy(n, bytes.data(), 16);
  if (bytes.size() < 16 || n[0] < 4 || n[1] < 0 || n[2] < 0 || n[3] < 0 ||
      (n[1] > 0 && n[1] != n[0]) || (n[2] > 0 && n[2] != n[0])) {
    if (err) *err = "invalid sizes in MSH file " + filename.string();
    return false;
  }
  const uint64_t words = 3ull * uint64_t(n[0]) + 3ull * uint64_t(n[1]) + 2ull * uint64_t(n[2]) +
                         3ull * uint64_t(n[3]);
  if (bytes.size() != 16 + 4 * words) {
    if (err) *err = "unexpected file size in MSH file " + filename.string();
    return false;
  }
  if (!active_reader->ChargeArray(size_t(words), 4, err)) return false;
  size_t at = 16;
  auto take = [&](auto* out, size_t count) {
    out->resize(count);
    if (count) std::memcpy(out->data(), bytes.data() + at, 4 * count);
    at += 4 * count;
  };
  take(&mesh->positions, 3 * size_t(n[0]));
  take(&mesh->normals, 3 * size_t(n[1]));
  take(&mesh->uvs, 2 * size_t(n[2]));
  take(&mesh->indices, 3 * size_t(n[3]));
  for (int32_t index : mesh->indices)
    if (index < 0 || index >= n[0]) {
      if (err) *err = "face index out of range in MSH file " + filename.string();
      return false;
    }
  if (!AllFinite(mesh->positions) || !AllFinite(mesh->normals) || !AllFinite(mesh->uvs)) {
    if (err) *err = "MSH file holds non-finite values: " + filename.string();
    return false;
  }
  return true;
}

// A level-set .vdb as a mesh asset (LightUSD extension; MuJoCo has no VDB
// meshes): the first float level-set grid, read through the reader's
// budget and meshed at its zero isovalue by sparse marching cubes.
bool LoadLevelSetVDB(const fs::path& filename, LevelSet* out, std::string* err) {
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!active_reader->Read(filename.string(), active_reader->root_source, &bytes, &resolved, err))
    return false;
  if (!ReadLevelSetVDB(bytes.data(), bytes.size(), out, err)) {
    if (err) *err = filename.string() + ": " + *err;
    return false;
  }
  return true;
}

bool LoadVDBMesh(const fs::path& filename, Mesh* mesh, std::string* err) {
  LevelSet level_set;
  if (!LoadLevelSetVDB(filename, &level_set, err)) return false;
  const auto& opts = active_reader->options;
  const size_t budget = opts.max_memory - std::min(opts.max_memory, active_reader->bytes);
  if (!LevelSetToMesh(level_set, std::max<size_t>(100000, budget / 256), mesh, err)) return false;
  if (!active_reader->ChargeArray(mesh->positions.size() + mesh->indices.size(), 4, err))
    return false;
  active_reader->Warn("level-set VDB " + filename.filename().string() +
                          " meshed at its zero isovalue (LightUSD extension; MuJoCo reads no VDB meshes)",
                      "mjcf.vdb_mesh_extension");
  return true;
}

bool LoadMeshFileUncached(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                          std::string* err) {
  if (active_plugin_meshes && filename.string().rfind(kPluginMeshPrefix, 0) == 0) {
    const auto found = active_plugin_meshes->find(filename.string());
    if (found != active_plugin_meshes->end()) {
      *mesh = found->second;
      return true;
    }
  }
  const std::string ext = ToLower(filename.extension().string());
  if (ext == ".stl") return LoadBinarySTL(filename, smoothnormal, mesh, err);
  // MuJoCo reads .msh as its legacy binary mesh, .gmsh through its GMSH decoder.
  if (ext == ".msh") return LoadLegacyMSH(filename, mesh, err);
  if (ext == ".vdb") return LoadVDBMesh(filename, mesh, err);
  if (ext == ".gmsh") return LoadGMSHMesh(filename, mesh, err);
  if (ext == ".obj" || ext == ".dae") return LoadOBJ(filename, smoothnormal, mesh, err);
  if (err) *err = "Unsupported native mesh extension: " + ext;
  return false;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
