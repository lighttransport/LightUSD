// Tangent computation benchmark.
//
// Compares three methods:
//   1. Reference MikkTSpace (original C implementation via wrapper)
//   2. Optimized MikkTSpace (fast-mikktspace.hh)
//   3. Lengyel (existing LightUSD method)
//
// Test scenes:
//   - Synthetic UV sphere (various tessellation)
//   - Synthetic icosphere (various subdivision levels)
//   - Optional: USD file (e.g. suzanne-subd-lv5.usdc, suzanne-subd-lv6.usdc)
//
// Usage:
//   ./bench_tangent [options]
//     --usd <file>         Load USD file as additional benchmark scene
//     --sizes <list>       Comma-separated UV sphere tessellation sizes
//                          (default: 32,64,128,256,512,1024)
//     --ico-levels <list>  Comma-separated icosphere subdivision levels
//                          (default: 1,2,3,4,5,6,7)
//     --skip-ref           Skip reference MikkTSpace (slow on large meshes)
//     --skip-lengyel       Skip Lengyel method
//     --quality            Measure angular deviation from reference MikkTSpace
//     --warmup <n>         Number of warmup iterations (default: 1)
//     --repeat <n>         Number of timed iterations (default: 3)
//
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

// ---- LightUSD headers for USD loading (optional) ----
#ifdef BENCH_WITH_USD
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#endif

// ---- LightUSD types for tangent computation ----
// We include the minimal headers needed for the tangent functions.
#include "value-types.hh"

// The three tangent methods:
// 1. Reference MikkTSpace wrapper
#include "tydra/mikktspace-tangent.hh"
// 2. Optimized MikkTSpace
#include "tydra/fast-mikktspace.hh"
// 4. Tangent quantization
#include "tydra/tangent-quantize.hh"

// 3. Lengyel — we extract just what we need
// ComputeTangentsAndBinormals is a static function in render-data.cc, so we
// replicate a lightweight standalone version here for the benchmark.

using float2 = lightusd::legacy::value::float2;
using float3 = lightusd::legacy::value::float3;

// ============================================================================
// Standalone Lengyel tangent computation (extracted from render-data.cc)
// ============================================================================
namespace lengyel {

static float3 f3sub(float3 a, float3 b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
static float3 f3add(float3 a, float3 b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
static float3 f3scale(float s, float3 v) { return {s*v[0], s*v[1], s*v[2]}; }
static float f3dot(float3 a, float3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static float3 f3cross(float3 a, float3 b) {
  return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
static float f3len(float3 v) { return std::sqrt(f3dot(v,v)); }
static float3 f3normalize(float3 v) {
  float l = f3len(v);
  return l > 1e-20f ? f3scale(1.0f/l, v) : float3{0,0,1};
}

static bool ComputeTangentsLengyel(
    const std::vector<float3> &positions,
    const std::vector<float3> &normals,
    const std::vector<float2> &texcoords,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices,
    bool is_facevarying,
    std::vector<float3> *out_tangents,
    std::vector<float3> *out_binormals,
    std::vector<uint32_t> *out_indices,
    std::string *err) {

  if (positions.empty() || normals.empty() || texcoords.empty()) {
    if (err) *err = "Empty input.";
    return false;
  }

  size_t nVerts = positions.size();
  size_t nFV = 0;
  for (auto c : faceVertexCounts) nFV += c;
  (void)is_facevarying; // used in indexing below

  // Per-vertex accumulation
  std::vector<float3> tan1(nVerts, {0,0,0});
  std::vector<float3> tan2(nVerts, {0,0,0});

  size_t fvOff = 0;
  for (size_t fi = 0; fi < faceVertexCounts.size(); fi++) {
    uint32_t nv = faceVertexCounts[fi];
    // Fan-triangulate
    for (uint32_t t = 0; t < nv - 2; t++) {
      size_t vi0, vi1, vi2;
      if (is_facevarying) {
        vi0 = fvOff;
        vi1 = fvOff + t + 1;
        vi2 = fvOff + t + 2;
      } else {
        vi0 = faceVertexIndices[fvOff];
        vi1 = faceVertexIndices[fvOff + t + 1];
        vi2 = faceVertexIndices[fvOff + t + 2];
      }

      float3 p0 = positions[vi0], p1 = positions[vi1], p2 = positions[vi2];
      float2 uv0 = texcoords[vi0], uv1 = texcoords[vi1], uv2 = texcoords[vi2];

      float3 d1 = f3sub(p1, p0), d2 = f3sub(p2, p0);
      float s1 = uv1[0] - uv0[0], t1_v = uv1[1] - uv0[1];
      float s2 = uv2[0] - uv0[0], t2_v = uv2[1] - uv0[1];

      float det = s1 * t2_v - s2 * t1_v;
      if (std::fabs(det) < 1e-20f) continue;
      float r = 1.0f / det;

      float3 sdir = f3scale(r, f3sub(f3scale(t2_v, d1), f3scale(t1_v, d2)));
      float3 tdir = f3scale(r, f3sub(f3scale(s1, d2), f3scale(s2, d1)));

      tan1[vi0] = f3add(tan1[vi0], sdir);
      tan1[vi1] = f3add(tan1[vi1], sdir);
      tan1[vi2] = f3add(tan1[vi2], sdir);
      tan2[vi0] = f3add(tan2[vi0], tdir);
      tan2[vi1] = f3add(tan2[vi1], tdir);
      tan2[vi2] = f3add(tan2[vi2], tdir);
    }
    fvOff += nv;
  }

  // Gram-Schmidt orthogonalize
  out_tangents->resize(nVerts);
  out_binormals->resize(nVerts);
  out_indices->resize(nFV);
  std::iota(out_indices->begin(), out_indices->end(), 0);

  for (size_t i = 0; i < nVerts; i++) {
    float3 n = normals[i];
    float3 t = tan1[i];
    // Gram-Schmidt: t' = normalize(t - n * dot(n, t))
    float3 tangent = f3normalize(f3sub(t, f3scale(f3dot(n, t), n)));
    // Handedness
    float w = f3dot(f3cross(n, t), tan2[i]) < 0.0f ? -1.0f : 1.0f;
    float3 binormal = f3scale(w, f3cross(n, tangent));

    (*out_tangents)[i] = tangent;
    (*out_binormals)[i] = binormal;
  }

  return true;
}

}  // namespace lengyel

// ============================================================================
// Mesh generators
// ============================================================================

struct FVMesh {
  std::string name;
  std::vector<float3> positions;   // facevarying
  std::vector<float3> normals;     // facevarying
  std::vector<float2> texcoords;   // facevarying
  std::vector<uint32_t> faceVertexCounts;

  size_t numFaceVerts() const {
    size_t n = 0;
    for (auto c : faceVertexCounts) n += c;
    return n;
  }
  size_t numTriangles() const {
    size_t n = 0;
    for (auto c : faceVertexCounts) n += (c - 2);
    return n;
  }
};

// Generate a UV sphere as facevarying triangle mesh.
// rings = latitude subdivisions, sectors = longitude subdivisions.
static FVMesh generateUVSphere(int rings, int sectors) {
  FVMesh mesh;
  char buf[128];
  snprintf(buf, sizeof(buf), "UVSphere(%dx%d)", rings, sectors);
  mesh.name = buf;

  // Generate vertex positions + normals + UVs on the sphere
  std::vector<float3> pts, nms;
  std::vector<float2> uvs;
  pts.reserve(size_t((rings+1) * (sectors+1)));
  nms.reserve(pts.capacity());
  uvs.reserve(pts.capacity());

  for (int r = 0; r <= rings; r++) {
    float phi = float(M_PI) * float(r) / float(rings);
    float sinP = std::sin(phi), cosP = std::cos(phi);
    for (int s = 0; s <= sectors; s++) {
      float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
      float sinT = std::sin(theta), cosT = std::cos(theta);
      float x = sinP * cosT, y = cosP, z = sinP * sinT;
      pts.push_back({x, y, z});
      nms.push_back({x, y, z}); // unit sphere: normal = position
      uvs.push_back({float(s) / float(sectors), float(r) / float(rings)});
    }
  }

  // Generate triangles (facevarying)
  int cols = sectors + 1;
  for (int r = 0; r < rings; r++) {
    for (int s = 0; s < sectors; s++) {
      int i00 = r * cols + s;
      int i10 = (r+1) * cols + s;
      int i01 = r * cols + (s+1);
      int i11 = (r+1) * cols + (s+1);

      // Two triangles per quad
      // Triangle 1: i00, i10, i11
      mesh.positions.push_back(pts[i00]); mesh.normals.push_back(nms[i00]); mesh.texcoords.push_back(uvs[i00]);
      mesh.positions.push_back(pts[i10]); mesh.normals.push_back(nms[i10]); mesh.texcoords.push_back(uvs[i10]);
      mesh.positions.push_back(pts[i11]); mesh.normals.push_back(nms[i11]); mesh.texcoords.push_back(uvs[i11]);
      mesh.faceVertexCounts.push_back(3);

      // Triangle 2: i00, i11, i01
      mesh.positions.push_back(pts[i00]); mesh.normals.push_back(nms[i00]); mesh.texcoords.push_back(uvs[i00]);
      mesh.positions.push_back(pts[i11]); mesh.normals.push_back(nms[i11]); mesh.texcoords.push_back(uvs[i11]);
      mesh.positions.push_back(pts[i01]); mesh.normals.push_back(nms[i01]); mesh.texcoords.push_back(uvs[i01]);
      mesh.faceVertexCounts.push_back(3);
    }
  }

  return mesh;
}

// Generate an icosphere by recursive subdivision of an icosahedron.
static FVMesh generateIcosphere(int subdivisions) {
  FVMesh mesh;
  char buf[128];
  snprintf(buf, sizeof(buf), "Icosphere(lv%d)", subdivisions);
  mesh.name = buf;

  // Start with icosahedron vertices
  const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
  std::vector<float3> verts = {
    {-1, t, 0}, { 1, t, 0}, {-1,-t, 0}, { 1,-t, 0},
    { 0,-1, t}, { 0, 1, t}, { 0,-1,-t}, { 0, 1,-t},
    { t, 0,-1}, { t, 0, 1}, {-t, 0,-1}, {-t, 0, 1},
  };
  // Normalize to unit sphere
  for (auto &v : verts) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;
  }

  // Icosahedron faces (20 triangles)
  struct Tri { int a, b, c; };
  std::vector<Tri> tris = {
    {0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
    {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
    {3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
    {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1},
  };

  // Subdivide
  for (int sub = 0; sub < subdivisions; sub++) {
    std::vector<Tri> newTris;
    newTris.reserve(tris.size() * 4);

    // Edge midpoint cache: key = min(a,b)*N + max(a,b)
    std::unordered_map<uint64_t, int> midCache;
    auto getMid = [&](int a, int b) -> int {
      int lo = std::min(a, b), hi = std::max(a, b);
      uint64_t key = uint64_t(lo) * uint64_t(verts.size() + tris.size() * 3) + uint64_t(hi);
      auto it = midCache.find(key);
      if (it != midCache.end()) return it->second;
      float3 m = {
        (verts[a][0] + verts[b][0]) * 0.5f,
        (verts[a][1] + verts[b][1]) * 0.5f,
        (verts[a][2] + verts[b][2]) * 0.5f
      };
      // Project onto unit sphere
      float l = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
      m[0] /= l; m[1] /= l; m[2] /= l;
      int idx = int(verts.size());
      verts.push_back(m);
      midCache[key] = idx;
      return idx;
    };

    for (auto &tri : tris) {
      int m01 = getMid(tri.a, tri.b);
      int m12 = getMid(tri.b, tri.c);
      int m20 = getMid(tri.c, tri.a);
      newTris.push_back({tri.a, m01, m20});
      newTris.push_back({m01, tri.b, m12});
      newTris.push_back({m20, m12, tri.c});
      newTris.push_back({m01, m12, m20});
    }
    tris = std::move(newTris);
  }

  // Convert to facevarying with spherical UVs
  for (auto &tri : tris) {
    for (int vi : {tri.a, tri.b, tri.c}) {
      float3 p = verts[vi];
      mesh.positions.push_back(p);
      mesh.normals.push_back(p); // unit sphere
      // Spherical UV
      float u = 0.5f + std::atan2(p[2], p[0]) / (2.0f * float(M_PI));
      float v = 0.5f - std::asin(std::max(-1.0f, std::min(1.0f, p[1]))) / float(M_PI);
      mesh.texcoords.push_back({u, v});
    }
    mesh.faceVertexCounts.push_back(3);
  }

  return mesh;
}

#ifdef BENCH_WITH_USD
// Load a USD file and extract the first mesh as facevarying data
static bool loadUSDMesh(const std::string &filepath, FVMesh *out, std::string *err) {
  lightusd::legacy::Stage stage;
  std::string warn;
  if (!lightusd::legacy::LoadUSDFromFile(filepath, &stage, &warn, err)) {
    return false;
  }

  lightusd::legacy::tydra::RenderSceneConverterEnv env(stage);
  env.mesh_config.triangulate = true;
  env.mesh_config.compute_tangents_and_binormals = false; // we compute separately
  env.mesh_config.compute_normals = true;

  lightusd::legacy::tydra::RenderScene scene;
  lightusd::legacy::tydra::RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &scene)) {
    if (err) *err = converter.GetError();
    return false;
  }

  if (scene.meshes.empty()) {
    if (err) *err = "No meshes in USD file.";
    return false;
  }

  // Take the first mesh
  const auto &rmesh = scene.meshes[0];
  out->name = "USD:" + filepath;

  // Build facevarying arrays from RenderMesh
  const auto &fvi = rmesh.triangulatedFaceVertexIndices.size()
                      ? rmesh.triangulatedFaceVertexIndices
                      : rmesh.usdFaceVertexIndices;
  const auto &fvc = rmesh.triangulatedFaceVertexCounts.size()
                      ? rmesh.triangulatedFaceVertexCounts
                      : rmesh.usdFaceVertexCounts;

  size_t totalFV = fvi.size();
  out->positions.resize(totalFV);
  out->normals.resize(totalFV);
  out->texcoords.resize(totalFV);
  out->faceVertexCounts.assign(fvc.begin(), fvc.end());

  // Determine variability of normals/texcoords
  bool normsFV = (rmesh.normals.vertex_count() == totalFV);
  bool uvsFV = false;
  if (rmesh.texcoords.count(0))
    uvsFV = (rmesh.texcoords.at(0).vertex_count() == totalFV);

  const float3 *norm_ptr = reinterpret_cast<const float3 *>(rmesh.normals.buffer());
  size_t norm_count = rmesh.normals.vertex_count();
  const float2 *uv_ptr = nullptr;
  size_t uv_count = 0;
  if (rmesh.texcoords.count(0)) {
    uv_ptr = reinterpret_cast<const float2 *>(rmesh.texcoords.at(0).buffer());
    uv_count = rmesh.texcoords.at(0).vertex_count();
  }

  for (size_t i = 0; i < totalFV; i++) {
    uint32_t vi_val = fvi[i];
    // Position: always vertex-varying, expand to facevarying
    if (vi_val < rmesh.points.size())
      out->positions[i] = rmesh.points[vi_val];

    // Normals
    if (normsFV) {
      if (i < norm_count) out->normals[i] = norm_ptr[i];
    } else {
      if (vi_val < norm_count) out->normals[i] = norm_ptr[vi_val];
    }

    // UVs
    if (uv_ptr) {
      if (uvsFV) {
        if (i < uv_count) out->texcoords[i] = uv_ptr[i];
      } else {
        if (vi_val < uv_count) out->texcoords[i] = uv_ptr[vi_val];
      }
    } else {
      out->texcoords[i] = {0.0f, 0.0f};
    }
  }

  return true;
}
#endif

// ============================================================================
// Timing helper
// ============================================================================
struct BenchResult {
  std::string method;
  double time_ms;    // average of repeated runs
  double min_ms;
  double max_ms;
  size_t numTriangles;
  size_t numFaceVerts;
  double trisPerSec;  // millions of triangles per second
  size_t memoryBytes; // estimated working memory
};

// Estimate working memory for each method (excludes input/output)
static size_t estimateMemoryRefMikkTSpace(size_t nFV, size_t nTris) {
  // Original MikkTSpace internal allocations (from mikktspace.c):
  // piTriListIn: nTris*3 ints
  // pTriInfos: nTris * ~72 bytes (STriInfo struct)
  // pTmpVert: nFV * 3 floats (welding scratch)
  // piHashTable/piHashOffsets: ~nFV ints
  // pEdge: nTris*3 * 16 bytes (for sorting)
  // pGroup: ~nTris * ~16 bytes
  // output tspace: nFV * ~32 bytes
  return nTris * 3 * 4          // piTriListIn
       + nTris * 72             // pTriInfos (STriInfo)
       + nFV * 12               // pTmpVert
       + nFV * 4 * 2            // hash tables
       + nTris * 3 * 16         // edges
       + nTris * 16             // groups (estimate)
       + nFV * 32;              // tspace output
}

static size_t estimateMemoryFastMikkTSpace(size_t nFV, size_t nTris) {
  // triList + origTriList: 2 * nTris*3 * 4
  // triInfos: nTris * 48 (compact TriInfo)
  // hash table for welding: ~2*nFV * 20 (Slot: hash+fvi+weldId)
  // edges for sorting: nTris*3 * 16
  // groups + groupTriBuf: ~nTris * 20
  // tspaces: nFV * 32
  // faceOffsets: numFaces * 4 (≈ nTris for all-tri mesh)
  return nTris * 3 * 4 * 2      // triList + origTriList
       + nTris * 48              // triInfos
       + nFV * 2 * 20            // hash table (load factor 0.5)
       + nTris * 3 * 16          // sorted edges
       + nTris * 20              // groups + groupTriBuf
       + nFV * 32                // tspaces
       + nTris * 4;              // faceOffsets
}

static size_t estimateMemoryLengyel(size_t nFV) {
  // tan1, tan2: 2 * nFV * 12 (float3)
  // output tangents, binormals: 2 * nFV * 12
  // indices: nFV * 4
  return nFV * 12 * 2            // tan1 + tan2
       + nFV * 12 * 2            // output
       + nFV * 4;                // indices
}

static const char *formatBytes(size_t bytes, char *buf, size_t bufSize) {
  if (bytes >= 1024ULL * 1024 * 1024)
    snprintf(buf, bufSize, "%.1f GB", double(bytes) / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024 * 1024)
    snprintf(buf, bufSize, "%.1f MB", double(bytes) / (1024.0 * 1024));
  else if (bytes >= 1024)
    snprintf(buf, bufSize, "%.1f KB", double(bytes) / 1024.0);
  else
    snprintf(buf, bufSize, "%zu B", bytes);
  return buf;
}

template <typename Fn>
static BenchResult benchmarkMethod(const std::string &method, const FVMesh &mesh,
                                    int warmup, int repeat, Fn fn) {
  // Warmup
  for (int i = 0; i < warmup; i++) fn();

  // Timed runs
  std::vector<double> times;
  for (int i = 0; i < repeat; i++) {
    auto t0 = std::chrono::high_resolution_clock::now();
    fn();
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    times.push_back(ms);
  }

  BenchResult r;
  r.method = method;
  r.numTriangles = mesh.numTriangles();
  r.numFaceVerts = mesh.numFaceVerts();
  r.min_ms = *std::min_element(times.begin(), times.end());
  r.max_ms = *std::max_element(times.begin(), times.end());
  r.time_ms = std::accumulate(times.begin(), times.end(), 0.0) / double(times.size());
  r.trisPerSec = (r.time_ms > 0) ? (double(r.numTriangles) / r.time_ms / 1000.0) : 0;
  return r;
}

// ============================================================================
// Quality comparison
// ============================================================================
struct QualityResult {
  double maxAngleDeg;
  double avgAngleDeg;
  double rmsAngleDeg;
  size_t count;
};

static QualityResult compareQuality(const std::vector<float3> &ref,
                                     const std::vector<float3> &test,
                                     bool printHistogram = false) {
  QualityResult q = {0, 0, 0, 0};
  size_t n = std::min(ref.size(), test.size());
  double sumAngle = 0, sumAngle2 = 0;

  // Histogram buckets: 0-0.1, 0.1-1, 1-5, 5-10, 10-45, 45-90, 90-180
  size_t hist[7] = {};

  for (size_t i = 0; i < n; i++) {
    // Normalize both vectors before comparing directions (robust against sub-unit outputs)
    float lenR = std::sqrt(ref[i][0]*ref[i][0] + ref[i][1]*ref[i][1] + ref[i][2]*ref[i][2]);
    float lenT = std::sqrt(test[i][0]*test[i][0] + test[i][1]*test[i][1] + test[i][2]*test[i][2]);
    float dot;
    if (lenR < 1e-10f || lenT < 1e-10f) {
      dot = 1.0f; // skip zero-length vectors
    } else {
      dot = (ref[i][0]*test[i][0] + ref[i][1]*test[i][1] + ref[i][2]*test[i][2]) / (lenR * lenT);
    }
    dot = std::max(-1.0f, std::min(1.0f, dot));
    double angle = std::acos(double(dot)) * (180.0 / M_PI);
    if (angle > q.maxAngleDeg) q.maxAngleDeg = angle;
    sumAngle += angle;
    sumAngle2 += angle * angle;

    if (angle < 0.1) hist[0]++;
    else if (angle < 1.0) hist[1]++;
    else if (angle < 5.0) hist[2]++;
    else if (angle < 10.0) hist[3]++;
    else if (angle < 45.0) hist[4]++;
    else if (angle < 90.0) hist[5]++;
    else hist[6]++;
  }
  q.count = n;
  q.avgAngleDeg = n > 0 ? sumAngle / double(n) : 0;
  q.rmsAngleDeg = n > 0 ? std::sqrt(sumAngle2 / double(n)) : 0;

  if (printHistogram && n > 0) {
    printf("  -> Histogram: <0.1°:%zu  0.1-1°:%zu  1-5°:%zu  5-10°:%zu  10-45°:%zu  45-90°:%zu  >90°:%zu\n",
           hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
  }
  return q;
}

// ============================================================================
// Parse comma-separated integers
// ============================================================================
static std::vector<int> parseIntList(const char *s) {
  std::vector<int> result;
  const char *p = s;
  while (*p) {
    result.push_back(atoi(p));
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
  }
  return result;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char **argv) {
  // Defaults
  std::vector<int> sphereSizes = {32, 64, 128, 256, 512, 1024};
  std::vector<int> icoLevels = {1, 2, 3, 4, 5, 6, 7};
  std::string usdFile;
  bool skipRef = false;
  bool skipLengyel = false;
  bool measureQuality = false;
  int warmup = 1;
  int repeat = 3;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--usd") == 0 && i+1 < argc) {
      usdFile = argv[++i];
    } else if (strcmp(argv[i], "--sizes") == 0 && i+1 < argc) {
      sphereSizes = parseIntList(argv[++i]);
    } else if (strcmp(argv[i], "--ico-levels") == 0 && i+1 < argc) {
      icoLevels = parseIntList(argv[++i]);
    } else if (strcmp(argv[i], "--skip-ref") == 0) {
      skipRef = true;
    } else if (strcmp(argv[i], "--skip-lengyel") == 0) {
      skipLengyel = true;
    } else if (strcmp(argv[i], "--quality") == 0) {
      measureQuality = true;
    } else if (strcmp(argv[i], "--warmup") == 0 && i+1 < argc) {
      warmup = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--repeat") == 0 && i+1 < argc) {
      repeat = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: %s [--usd file] [--sizes n,n,...] [--ico-levels n,n,...]\n"
             "         [--skip-ref] [--skip-lengyel] [--quality]\n"
             "         [--warmup n] [--repeat n]\n", argv[0]);
      return 0;
    }
  }

  // Build test meshes
  std::vector<FVMesh> meshes;

  for (int s : sphereSizes) {
    meshes.push_back(generateUVSphere(s, s * 2));
  }

  for (int lv : icoLevels) {
    meshes.push_back(generateIcosphere(lv));
  }

#ifdef BENCH_WITH_USD
  if (!usdFile.empty()) {
    FVMesh usdMesh;
    std::string err;
    printf("Loading USD: %s ...\n", usdFile.c_str());
    if (loadUSDMesh(usdFile, &usdMesh, &err)) {
      printf("  Loaded: %zu tris, %zu face-verts\n", usdMesh.numTriangles(), usdMesh.numFaceVerts());
      meshes.push_back(std::move(usdMesh));
    } else {
      fprintf(stderr, "Failed to load USD: %s\n", err.c_str());
    }
  }
#else
  if (!usdFile.empty()) {
    fprintf(stderr, "Warning: --usd requires building with BENCH_WITH_USD. Ignoring.\n");
  }
#endif

  // (diagnostic section removed — hybrid quality verified)

  // Print header
  printf("\n");
  printf("%-35s  %10s  %10s  %-19s  %10s  %10s  %10s  %12s  %10s\n",
         "Mesh", "Tris", "FaceVerts", "Method", "Avg(ms)", "Min(ms)", "Max(ms)", "MTri/s", "WorkMem");
  printf("%-35s  %10s  %10s  %-19s  %10s  %10s  %10s  %12s  %10s\n",
         "---", "---", "---", "---", "---", "---", "---", "---", "---");

  for (auto &mesh : meshes) {
    std::vector<float3> refTangents, refBinormals;
    bool hasRef = false;

    size_t nTris = mesh.numTriangles();
    size_t nFV = mesh.numFaceVerts();
    char memBuf[32];

    // --- Reference MikkTSpace ---
    if (!skipRef) {
      std::vector<float3> tangents, binormals;
      std::string err;
      size_t mem = estimateMemoryRefMikkTSpace(nFV, nTris);
      auto res = benchmarkMethod("Ref-MikkTSpace", mesh, warmup, repeat, [&]() {
        tangents.clear(); binormals.clear();
        lightusd::tydra::ComputeTangentsMikkTSpace(
            mesh.positions, mesh.normals, mesh.texcoords,
            mesh.faceVertexCounts, &tangents, &binormals, &err);
      });
      res.memoryBytes = mem;
      printf("%-35s  %10zu  %10zu  %-19s  %10.2f  %10.2f  %10.2f  %12.3f  %10s\n",
             mesh.name.c_str(), res.numTriangles, res.numFaceVerts,
             res.method.c_str(), res.time_ms, res.min_ms, res.max_ms, res.trisPerSec,
             formatBytes(mem, memBuf, sizeof(memBuf)));
      refTangents = tangents;
      refBinormals = binormals;
      hasRef = true;
    }

    // --- Optimized MikkTSpace ---
    {
      std::vector<float3> tangents, binormals;
      std::string err;
      size_t mem = estimateMemoryFastMikkTSpace(nFV, nTris);
      auto res = benchmarkMethod("Fast-MikkTSpace", mesh, warmup, repeat, [&]() {
        tangents.clear(); binormals.clear();
        lightusd::tydra::fast_mikkt::ComputeTangentsFastMikkTSpace(
            mesh.positions, mesh.normals, mesh.texcoords,
            mesh.faceVertexCounts, &tangents, &binormals, &err);
      });
      res.memoryBytes = mem;
      printf("%-35s  %10zu  %10zu  %-19s  %10.2f  %10.2f  %10.2f  %12.3f  %10s\n",
             mesh.name.c_str(), res.numTriangles, res.numFaceVerts,
             res.method.c_str(), res.time_ms, res.min_ms, res.max_ms, res.trisPerSec,
             formatBytes(mem, memBuf, sizeof(memBuf)));

      if (measureQuality && hasRef) {
        auto q = compareQuality(refTangents, tangents, true);
        printf("  -> Quality vs Ref: max=%.4f° avg=%.4f° rms=%.4f° (%zu verts)\n",
               q.maxAngleDeg, q.avgAngleDeg, q.rmsAngleDeg, q.count);
      }
    }

    // --- Hybrid Lengyel+MikkTSpace ---
    {
      std::vector<float3> tangents, binormals;
      std::string err;
      lightusd::tydra::fast_mikkt::HybridStats hstats = {};
      auto res = benchmarkMethod("Hybrid", mesh, warmup, repeat, [&]() {
        tangents.clear(); binormals.clear();
        lightusd::tydra::fast_mikkt::ComputeTangentsHybrid(
            mesh.positions, mesh.normals, mesh.texcoords,
            mesh.faceVertexCounts, &tangents, &binormals, &hstats, &err);
      });
      res.memoryBytes = hstats.working_memory_bytes;
      printf("%-35s  %10zu  %10zu  %-19s  %10.2f  %10.2f  %10.2f  %12.3f  %10s\n",
             mesh.name.c_str(), res.numTriangles, res.numFaceVerts,
             res.method.c_str(), res.time_ms, res.min_ms, res.max_ms, res.trisPerSec,
             formatBytes(hstats.working_memory_bytes, memBuf, sizeof(memBuf)));
      printf("  -> Weld groups: %zu / %zu verts (%.1f%% reduction)\n",
             hstats.num_weld_groups, hstats.total_vertices,
             hstats.total_vertices > 0 ? 100.0 * (1.0 - double(hstats.num_weld_groups) / double(hstats.total_vertices)) : 0.0);

      if (measureQuality && hasRef) {
        auto q = compareQuality(refTangents, tangents, true);
        printf("  -> Quality vs Ref: max=%.4f° avg=%.4f° rms=%.4f° (%zu verts)\n",
               q.maxAngleDeg, q.avgAngleDeg, q.rmsAngleDeg, q.count);
      }
    }

    // --- Lengyel ---
    if (!skipLengyel) {
      std::vector<float3> tangents, binormals;
      std::vector<uint32_t> indices;
      std::string err;
      size_t mem = estimateMemoryLengyel(nFV);
      // For Lengyel, we also need faceVertexIndices (identity for facevarying)
      std::vector<uint32_t> fvi(mesh.numFaceVerts());
      std::iota(fvi.begin(), fvi.end(), 0);

      auto res = benchmarkMethod("Lengyel", mesh, warmup, repeat, [&]() {
        tangents.clear(); binormals.clear(); indices.clear();
        lengyel::ComputeTangentsLengyel(
            mesh.positions, mesh.normals, mesh.texcoords,
            mesh.faceVertexCounts, fvi, true,
            &tangents, &binormals, &indices, &err);
      });
      res.memoryBytes = mem;
      printf("%-35s  %10zu  %10zu  %-19s  %10.2f  %10.2f  %10.2f  %12.3f  %10s\n",
             mesh.name.c_str(), res.numTriangles, res.numFaceVerts,
             res.method.c_str(), res.time_ms, res.min_ms, res.max_ms, res.trisPerSec,
             formatBytes(mem, memBuf, sizeof(memBuf)));

      if (measureQuality && hasRef) {
        auto q = compareQuality(refTangents, tangents, true);
        printf("  -> Quality vs Ref: max=%.4f° avg=%.4f° rms=%.4f° (%zu verts)\n",
               q.maxAngleDeg, q.avgAngleDeg, q.rmsAngleDeg, q.count);
      }
    }

    // --- Quantization quality/size report (uses Ref tangents) ---
    if (hasRef && measureQuality) {
      using namespace lightusd::tydra::tangent_quantize;

      printf("  ---- Tangent Quantization ----\n");
      printf("  %-14s  %5s  %8s  %10s  %10s  %6s\n",
             "Format", "Bytes", "Total", "Max-err", "Avg-err", "Signs");

      // 1010102
      {
        auto q = MeasureQuantizeError<PackedTangent1010102>(
            refTangents, refBinormals, mesh.normals,
            pack_tangent_1010102, unpack_tangent_1010102);
        size_t total = nFV * 4;
        printf("  %-14s  %5zu  %10s  %8.4f°  %8.4f°  %6zu\n",
               "10_10_10_2", size_t(4),
               formatBytes(total, memBuf, sizeof(memBuf)),
               q.max_angle_deg, q.avg_angle_deg, q.sign_mismatches);
      }
      // SNORM8x4
      {
        auto q = MeasureQuantizeError<PackedTangentSNorm8x4>(
            refTangents, refBinormals, mesh.normals,
            pack_tangent_snorm8, unpack_tangent_snorm8);
        size_t total = nFV * 4;
        printf("  %-14s  %5zu  %10s  %8.4f°  %8.4f°  %6zu\n",
               "SNorm8x4", size_t(4),
               formatBytes(total, memBuf, sizeof(memBuf)),
               q.max_angle_deg, q.avg_angle_deg, q.sign_mismatches);
      }
      // FP16x4
      {
        auto q = MeasureQuantizeError<PackedTangentFp16x4>(
            refTangents, refBinormals, mesh.normals,
            pack_tangent_fp16, unpack_tangent_fp16);
        size_t total = nFV * 8;
        printf("  %-14s  %5zu  %10s  %8.4f°  %8.4f°  %6zu\n",
               "Fp16x4", size_t(8),
               formatBytes(total, memBuf, sizeof(memBuf)),
               q.max_angle_deg, q.avg_angle_deg, q.sign_mismatches);
      }
      // Reference (unquantized)
      {
        size_t total = nFV * 24; // float3 tangent + float3 binormal
        printf("  %-14s  %5zu  %10s  %8s  %8s  %6s\n",
               "Float3+Float3", size_t(24),
               formatBytes(total, memBuf, sizeof(memBuf)),
               "0", "0", "0");
      }

      // Memory savings summary
      printf("\n  ---- Tangent Storage Memory ----\n");
      printf("  %-20s  %10s  %8s  %s\n", "Format", "Size", "Savings", "Use Case");
      size_t baseline = nFV * 24;
      auto printRow = [&](const char *name, size_t bytesPerVert, const char *useCase) {
        size_t total = nFV * bytesPerVert;
        double pct = 100.0 * (1.0 - double(total) / double(baseline));
        printf("  %-20s  %10s  %6.1f%%  %s\n", name,
               formatBytes(total, memBuf, sizeof(memBuf)), pct, useCase);
      };
      printRow("Float3+Float3", 24, "baseline (current)");
      printRow("Fp16x4", 8, "default (native)");
      printRow("10_10_10_2", 4, "default (WASM/WebGL2)");
      printRow("SNorm8x4", 4, "fallback (WebGL1)");
    }

    printf("\n");
  }

  return 0;
}
