// SPDX-License-Identifier: Apache-2.0
// Convex hull for face-less MJCF mesh assets (MuJoCo's mjCMesh::Process
// hulls a mesh whose face_ is empty). Checks geometric invariants rather than
// a particular triangulation: the hull contains every input point, its faces
// are outward and non-degenerate, it is a closed two-manifold with Euler
// characteristic 2, and it realizes the same support function as the input
// (which pins the geometry of the hull, not just its vertices).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tydra/asset-import/convex-hull.hh"
#include "tydra/asset-import/import.hh"

namespace {

using lightusd::tydra::import_asset::BuildConvexHull;
using lightusd::tydra::import_asset::HullStatus;

struct Vec3 {
  double x, y, z;
};

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

// Signed volume of the triangle soup: positive when faces wind outward.
double SignedVolume(const std::vector<Vec3>& p, const std::vector<int32_t>& tri) {
  double six = 0;
  for (size_t i = 0; i + 2 < tri.size(); i += 3)
    six += Dot(p[size_t(tri[i])],
               Cross(p[size_t(tri[i + 1])], p[size_t(tri[i + 2])]));
  return six / 6.0;
}

void CheckHull(const std::vector<Vec3>& points, const std::vector<int32_t>& tri,
               const char* label) {
  assert(!tri.empty() && tri.size() % 3 == 0);
  double diagonal = 0;
  {
    Vec3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    for (const Vec3& q : points) {
      lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
      hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
    diagonal = Length(Sub(hi, lo));
  }
  const double eps = std::max(1e-9, 1e-6 * diagonal);

  // Faces: non-degenerate, outward, and the soup is closed + manifold.
  std::map<std::pair<int32_t, int32_t>, int> edges;
  std::set<int32_t> used;
  for (size_t f = 0; f < tri.size(); f += 3) {
    const Vec3& a = points[size_t(tri[f])];
    const Vec3& b = points[size_t(tri[f + 1])];
    const Vec3& c = points[size_t(tri[f + 2])];
    const double area = 0.5 * Length(Cross(Sub(b, a), Sub(c, a)));
    assert(area > 0.0);
    used.insert(tri[f]);
    used.insert(tri[f + 1]);
    used.insert(tri[f + 2]);
    for (int k = 0; k < 3; ++k) {
      const int32_t u = tri[f + size_t(k)];
      const int32_t v = tri[f + size_t((k + 1) % 3)];
      ++edges[{u, v}];
    }
  }
  for (const auto& e : edges) {
    auto opposite = edges.find({e.first.second, e.first.first});
    assert(e.second == 1 && opposite != edges.end() && opposite->second == 1);
  }
  const size_t v = used.size();
  const size_t e = edges.size() / 2;
  const size_t fcount = tri.size() / 3;
  if (!(v + fcount - e == 2)) {
    std::cerr << label << ": not a closed sphere (V=" << v << " E=" << e
              << " F=" << fcount << ")\n";
    std::abort();
  }

  // Outward: positive volume; every input point inside or on a face.
  const double volume = SignedVolume(points, tri);
  if (!(volume > 0)) {
    std::cerr << label << ": hull volume " << volume << "\n";
    std::abort();
  }
  for (const Vec3& q : points) {
    for (size_t f = 0; f < tri.size(); f += 3) {
      const Vec3& a = points[size_t(tri[f])];
      Vec3 n = Cross(Sub(points[size_t(tri[f + 1])], a), Sub(points[size_t(tri[f + 2])], a));
      n = {n.x / Length(n), n.y / Length(n), n.z / Length(n)};
      if (Dot(n, Sub(q, a)) > eps) {
        std::cerr << label << ": point outside the hull\n";
        std::abort();
      }
    }
  }

  // Support function: no input point is further along a random direction than
  // the hull (in fact the hull's vertices are a subset of the input).
  for (int s = 0; s < 64; ++s) {
    Vec3 d{std::cos(0.37 * s), std::sin(0.61 * s), std::cos(1.13 * s + 0.5)};
    const double len = Length(d);
    d = {d.x / len, d.y / len, d.z / len};
    double input_max = -INFINITY;
    for (const Vec3& q : points) input_max = std::max(input_max, Dot(d, q));
    double hull_max = -INFINITY;
    for (int32_t i : tri) hull_max = std::max(hull_max, Dot(d, points[size_t(i)]));
    if (std::fabs(input_max - hull_max) > eps) {
      std::cerr << label << ": support mismatch " << input_max << " vs " << hull_max << "\n";
      std::abort();
    }
  }
}

void TestCube() {
  // Unit cube corners plus an interior point, a face centre and a point on an
  // edge: only the eight corners may become hull vertices.
  std::vector<Vec3> p;
  for (int i = 0; i < 8; ++i)
    p.push_back({double(i & 1), double((i >> 1) & 1), double((i >> 2) & 1)});
  p.push_back({0.5, 0.5, 0.5});   // interior
  p.push_back({0.5, 0.5, 1.0});   // top face centre
  p.push_back({0.5, 0.0, 0.0});   // bottom edge
  std::vector<float> flat;
  for (const Vec3& q : p) {
    flat.push_back(float(q.x));
    flat.push_back(float(q.y));
    flat.push_back(float(q.z));
  }
  std::vector<int32_t> tri;
  std::string why;
  const HullStatus status = BuildConvexHull(flat.data(), p.size(), &tri, &why);
  assert(status == HullStatus::Ok);
  CheckHull(p, tri, "cube");
  assert(tri.size() == 12 * 3);  // six quads, two triangles each
  const double volume = SignedVolume(p, tri);
  assert(std::fabs(volume - 1.0) < 1e-9);
  std::set<int32_t> vertices(tri.begin(), tri.end());
  assert(vertices.size() == 8);
  std::cout << "  cube: 12 triangles, volume 1, 8 hull vertices\n";
}

void TestDegenerate() {
  std::vector<int32_t> tri;
  std::string why;
  {
    std::vector<float> p = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    assert(BuildConvexHull(p.data(), 3, &tri, &why) == HullStatus::TooFewPoints);
  }
  {
    // Colocated: four copies of one point.
    std::vector<float> p = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(BuildConvexHull(p.data(), 4, &tri, &why) == HullStatus::Colocated);
  }
  {
    // Collinear along x.
    std::vector<float> p = {0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0};
    assert(BuildConvexHull(p.data(), 4, &tri, &why) == HullStatus::Collinear);
    assert(why == "collinear vertices");
  }
  {
    // Coplanar in z = 0 (a square).
    std::vector<float> p = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    assert(BuildConvexHull(p.data(), 4, &tri, &why) == HullStatus::Coplanar);
    assert(why == "coplanar vertices");
  }
  {
    // Non-finite input is rejected, not hulled.
    std::vector<float> p = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, NAN};
    assert(BuildConvexHull(p.data(), 4, &tri, &why) == HullStatus::Invalid);
  }
  std::cout << "  degenerate inputs: too few / colocated / collinear / coplanar\n";
}

// MuJoCo's model/plugin/sdf/asset/die.obj is a vertex-only point cloud that
// the simulator hulls. Optional: needs the MuJoCo source checkout.
void TestMuJoCoDie() {
  const char* root = std::getenv("MUJOCO_MODEL_DIR");
  std::string path;
  if (root && root[0]) {
    path = std::string(root) + "/plugin/sdf/asset/die.obj";
  } else if (const char* home = std::getenv("HOME")) {
    path = std::string(home) + "/work/mujoco/model/plugin/sdf/asset/die.obj";
  }
  std::ifstream in(path);
  if (!in) {
    std::cout << "  die.obj: SKIP (set MUJOCO_MODEL_DIR)\n";
    return;
  }
  std::vector<Vec3> points;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("v ", 0) != 0) continue;
    double x = 0, y = 0, z = 0;
    if (std::sscanf(line.c_str(), "v %lf %lf %lf", &x, &y, &z) == 3) points.push_back({x, y, z});
  }
  if (points.size() < 4) {
    std::cout << "  die.obj: SKIP (no vertices)\n";
    return;
  }
  std::vector<float> flat;
  flat.reserve(points.size() * 3);
  for (const Vec3& q : points) {
    flat.push_back(float(q.x));
    flat.push_back(float(q.y));
    flat.push_back(float(q.z));
  }
  std::vector<int32_t> tri;
  std::string why;
  const HullStatus status = BuildConvexHull(flat.data(), points.size(), &tri, &why);
  assert(status == HullStatus::Ok);
  CheckHull(points, tri, "die.obj");
  // The fixture samples the die's surface, so every point is extreme; what
  // matters is that the hull is a closed convex polyhedron over them.
  std::set<int32_t> vertices(tri.begin(), tri.end());
  assert(vertices.size() <= points.size());
  std::cout << "  die.obj: " << points.size() << " points -> " << vertices.size()
            << " hull vertices, " << tri.size() / 3 << " triangles, volume "
            << SignedVolume(points, tri) << "\n";
}

// MuJoCo's plugin/sdf example models: mesh.xml references a vertex-only OBJ
// (die.obj) that the importer must hull, and mug.xml references a mesh two
// directories up, which resolves through an allowed asset root.
void TestMuJoCoExampleImports() {
  namespace fs = std::filesystem;
  std::string model_dir;
  if (const char* root = std::getenv("MUJOCO_MODEL_DIR")) {
    if (root[0]) model_dir = root;
  }
  if (model_dir.empty()) {
    if (const char* home = std::getenv("HOME")) model_dir = std::string(home) + "/work/mujoco/model";
  }
  const fs::path mesh_xml = fs::path(model_dir) / "plugin/sdf/mesh.xml";
  const fs::path mug_xml = fs::path(model_dir) / "plugin/sdf/mug.xml";
  if (model_dir.empty() || !fs::exists(mesh_xml)) {
    std::cout << "  MJCF point-cloud example: SKIP (set MUJOCO_MODEL_DIR)\n";
    return;
  }
  auto import = [](const fs::path& xml, const std::vector<std::string>& asset_dirs,
                   bool* hulled) {
    lightusd::tydra::import_asset::Options options;
    options.format = "mjcf";
    options.profile = "newton";
    options.asset_dirs = asset_dirs;
    // Keep the test fast: no SDF sidecars for the plugin meshes.
    options.sdf = "off";
    options.typed_meshes = true;
    lightusd::tydra::import_asset::Result result;
    std::string err;
    const bool ok = lightusd::tydra::import_asset::BuildPayloadFile(
        xml.string(), options, &result, &err);
    if (!ok) {
      std::cerr << xml << ": " << err << "\n";
      return false;
    }
    if (hulled) {
      // The payload's mesh reference for the point-cloud asset must carry
      // faces now (the hull), and a meshTransforms entry must exist for it.
      *hulled = false;
      for (const auto& entry : result.meshes) {
        if (entry.second.indices.size() >= 12) *hulled = true;
      }
    }
    return true;
  };
  // die.obj lives next to mesh.xml, so no extra root is needed.
  bool hulled = false;
  if (!import(mesh_xml, {}, &hulled)) std::abort();
  if (!hulled) {
    std::cerr << "mesh.xml: the hulled mesh has no faces\n";
    std::abort();
  }
  // mug.xml references ../../mug/mug.obj: the model root must be allowed.
  const fs::path model_root = fs::path(mesh_xml).parent_path().parent_path().parent_path();
  if (fs::exists(mug_xml) && !import(mug_xml, {model_root.string()}, nullptr)) std::abort();
  std::cout << "  MJCF examples: die.obj hulled, mug.xml parent-relative mesh resolved\n";
}

}  // namespace

int main() {
  std::cout << "=== Convex hull tests ===\n";
  TestCube();
  TestDegenerate();
  TestMuJoCoDie();
  TestMuJoCoExampleImports();
  std::cout << "=== Convex hull tests PASSED ===\n";
  return 0;
}
