// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment, Inc.
//
// Render-conversion benchmark: Stage -> raster/RT renderable mesh conversion.
//
// Measures the legacy tydra::RenderSceneConverter pipeline and/or the
// tydra::next converter over a deterministic synthetic multi-mesh USDA scene
// or an explicit scene file.
//
// Pipeline selection at compile time:
//   - PERFRC_ENABLE_LEGACY (=1 default): legacy tydra conversion.
//   - PERFRC_ENABLE_NEXT: tydra-next conversion (requires tydra_next +
//     lightusd_next libs; the standalone src/next tree cannot build the
//     legacy pipeline, so it sets PERFRC_ENABLE_LEGACY=0).
// When both are compiled into one binary they are measured back to back on
// the same input.
//
// Prints a stable FNV-1a scene checksum per pipeline so A/B builds (e.g.
// serial vs parallel conversion) can be verified byte-identical: identical
// input + config must produce an identical hash.
//
// Not a pass/fail timing gate: exits 0 unless conversion itself fails.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef PERFRC_ENABLE_LEGACY
#define PERFRC_ENABLE_LEGACY 1
#endif

#if defined(PERFRC_ENABLE_NEXT)
#define PERFRC_HAS_NEXT 1
#else
#define PERFRC_HAS_NEXT 0
#endif

#if PERFRC_ENABLE_LEGACY
#include "lightusd.hh"
#include "tydra/render-data.hh"
#endif

#if !PERFRC_ENABLE_LEGACY || PERFRC_HAS_NEXT
// The next pipeline headers live under src/next; consumers add src/ and
// src/next to their include path.
#include "next/lightusd-next.hh"
#include "tydra/next/render-converter.hh"
#endif

namespace {

// ----------------------------- options ---------------------------------

struct Options {
  int prims = 256;    // synthetic meshes when generating
  int iters = 3;
  int threads = 0;    // next-converter workers (0 = auto)
  int legacy_threads = -2;  // legacy converter num_threads (-2 = default)
  bool json = false;
  bool legacy_only = false;
  bool next_only = false;
  std::string scene;  // explicit scene file (optional)
};

bool ParseOptions(int argc, char **argv, Options *opts) {
  for (int i = 1; i < argc; i++) {
    const std::string arg(argv[i]);
    auto need_value = [&](void) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", arg.c_str());
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--prims") {
      const char *v = need_value();
      if (!v) return false;
      opts->prims = std::atoi(v);
    } else if (arg == "--iters") {
      const char *v = need_value();
      if (!v) return false;
      opts->iters = std::atoi(v);
    } else if (arg == "--threads") {
      const char *v = need_value();
      if (!v) return false;
      opts->threads = std::atoi(v);
    } else if (arg == "--legacy-threads") {
      const char *v = need_value();
      if (!v) return false;
      opts->legacy_threads = std::atoi(v);
    } else if (arg == "--scene") {
      const char *v = need_value();
      if (!v) return false;
      opts->scene = v;
    } else if (arg == "--json") {
      opts->json = true;
    } else if (arg == "--legacy-only") {
      opts->legacy_only = true;
    } else if (arg == "--next-only") {
      opts->next_only = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "Usage: %s [--prims N] [--iters N] [--threads N] [--json] "
          "[--legacy-only] [--next-only] [--scene file.usda]\n",
          argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return false;
    }
  }
  opts->prims = std::max(1, std::min(opts->prims, 100000));
  opts->iters = std::max(1, std::min(opts->iters, 64));
  return true;
}

// ----------------------- synthetic scene generator ----------------------

// Deterministic splitmix64 so every run/build produces the same scene.
uint64_t RngNext(uint64_t *s) {
  uint64_t x = (*s += 0x9e3779b97f4a7c15ull);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

std::string GenSceneUSDA(int num_prims) {
  uint64_t rng = 0x12345678ull;
  std::string s;
  s.reserve(size_t(num_prims) * 4096 + 256);
  s += "#usda 1.0\n(\n    upAxis = \"Y\"\n    metersPerUnit = 1\n)\n\n";
  s += "def Xform \"root\"\n{\n";
  for (int p = 0; p < num_prims; p++) {
    // Mixed sizes: small quads to mid-size grids, plus occasional big meshes.
    int seg = 1 + int(RngNext(&rng) % 24u);
    if ((p % 16) == 0) seg += 40;
    const int verts_per_row = seg + 1;
    const size_t nfaces = size_t(seg) * size_t(seg);
    const float step = 0.5f;

    char name[32];
    std::snprintf(name, sizeof(name), "mesh_%05d", p);

    s += "    def Mesh \"";
    s += name;
    s += "\"\n    {\n";

    s += "        int[] faceVertexCounts = [";
    for (size_t i = 0; i < nfaces; i++) {
      s += "4";
      if (i + 1 < nfaces) s += ", ";
    }
    s += "]\n";

    // Quad grid indices.
    s += "        int[] faceVertexIndices = [";
    for (int y = 0; y < seg; y++) {
      for (int x = 0; x < seg; x++) {
        const int quad[4] = {y * verts_per_row + x,
                             y * verts_per_row + x + 1,
                             (y + 1) * verts_per_row + x + 1,
                             (y + 1) * verts_per_row + x};
        for (int k = 0; k < 4; k++) {
          s += std::to_string(quad[k]);
          if (!(y == seg - 1 && x == seg - 1 && k == 3)) s += ", ";
        }
      }
    }
    s += "]\n";

    // Points: slightly wavy plane.
    s += "        point3f[] points = [";
    for (int y = 0; y <= seg; y++) {
      for (int x = 0; x <= seg; x++) {
        const float fz =
            0.25f * std::sin(0.7f * float(x) + 0.3f * float(y) + float(p % 7));
        char pt[96];
        std::snprintf(pt, sizeof(pt), "(%.3f, %.3f, %.3f)", x * step, y * step,
                      fz);
        s += pt;
        if (!(y == seg && x == seg)) s += ", ";
      }
    }
    s += "]\n";

    // Every other mesh gets vertex-interpolated UVs.
    if ((p % 2) == 0) {
      s += "        texCoord2f[] primvars:st = [";
      for (int y = 0; y <= seg; y++) {
        for (int x = 0; x <= seg; x++) {
          char uv[64];
          std::snprintf(uv, sizeof(uv), "(%.4f, %.4f)", float(x) / float(seg),
                        float(y) / float(seg));
          s += uv;
          if (!(y == seg && x == seg)) s += ", ";
        }
      }
      s += "] (\n            interpolation = \"vertex\"\n        )\n";
    }

    s += "        color3f[] primvars:displayColor = [(0.4, 0.5, 0.6)]\n";
    s += "        uniform token subdivisionScheme = \"none\"\n";
    s += "    }\n";
  }
  s += "}\n";
  return s;
}

bool WriteFileText(const std::string &path, const std::string &data,
                   std::string *err) {
  FILE *fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    *err = "failed to open for write: " + path;
    return false;
  }
  const size_t n = data.size();
  const size_t w = (n == 0) ? 0 : std::fwrite(data.data(), 1, n, fp);
  std::fclose(fp);
  if (w != n) {
    *err = "short write: " + path;
    return false;
  }
  return true;
}

// ----------------------------- hashing ----------------------------------

// FNV-1a 64-bit over the raw bytes, with fixed-width integer mixers so the
// checksum is independent of struct padding and of native counter widths.
class Hasher {
 public:
  void Bytes(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) {
      h_ ^= b[i];
      h_ *= 1099511628211ull;
    }
  }
  template <typename T>
  void Pod(const T &v) {
    Bytes(&v, sizeof(T));
  }
  void U64(uint64_t v) {
    for (int i = 0; i < 8; i++) {
      h_ ^= uint8_t((v >> (8 * i)) & 0xffu);
      h_ *= 1099511628211ull;
    }
  }
  void Str(const std::string &s) {
    U64(uint64_t(s.size()));
    Bytes(s.data(), s.size());
  }
  uint64_t digest() const { return h_; }

 private:
  uint64_t h_ = 14695981039346656037ull;
};

double MedianMs(std::vector<double> ms) {
  if (ms.empty()) return 0.0;
  std::sort(ms.begin(), ms.end());
  return ms[ms.size() / 2];
}

struct BenchResult {
  bool ok = false;
  double ms = 0.0;
  size_t meshes = 0;
  size_t faces = 0;
  size_t tris = 0;
  uint64_t hash = 0;
  std::string err;
};

}  // namespace

#if PERFRC_ENABLE_LEGACY

// --------------------------- legacy pipeline ----------------------------

namespace legacy_bench {

void HashVertexAttribute(const lightusd::tydra::VertexAttribute &va,
                         Hasher *hsh) {
  hsh->Str(va.name);
  hsh->Pod(va.format);
  hsh->U64(va.elementSize);
  hsh->U64(va.stride);
  hsh->U64(va.data.size());
  hsh->Bytes(va.data.data(), va.data.size());
  hsh->U64(va.indices.size());
  hsh->Bytes(va.indices.data(), va.indices.size() * sizeof(uint32_t));
  hsh->Pod(va.variability);
}

void HashNode(const lightusd::tydra::Node &node, Hasher *hsh) {
  hsh->Str(node.abs_path);
  hsh->U64(uint64_t(node.id));
  hsh->Pod(node.local_matrix);
  hsh->Pod(node.global_matrix);
  hsh->U64(node.children.size());
  for (const auto &c : node.children) {
    HashNode(c, hsh);
  }
}

uint64_t HashScene(const lightusd::tydra::RenderScene &scene) {
  Hasher hsh;

  hsh.U64(scene.meshes.size());
  hsh.U64(scene.materials.size());
  hsh.U64(scene.images.size());
  hsh.U64(scene.textures.size());
  hsh.U64(scene.lights.size());
  hsh.U64(scene.cameras.size());
  hsh.U64(scene.animations.size());
  hsh.U64(scene.skeletons.size());
  hsh.U64(scene.instances.size());

  for (const auto &m : scene.meshes) {
    hsh.Str(m.abs_path);
    hsh.U64(m.points.size());
    hsh.Bytes(m.points.data(), m.points.size() * sizeof(lightusd::tydra::vec3));
    hsh.U64(m.faceVertexIndices().size());
    hsh.Bytes(m.faceVertexIndices().data(),
              m.faceVertexIndices().size() * sizeof(uint32_t));
    hsh.U64(m.faceVertexCounts().size());
    hsh.Bytes(m.faceVertexCounts().data(),
              m.faceVertexCounts().size() * sizeof(uint32_t));
    HashVertexAttribute(m.normals, &hsh);

    // Sort texcoord slots: unordered_map iteration order is unspecified.
    std::vector<uint32_t> slots;
    slots.reserve(m.texcoords.size());
    for (const auto &kv : m.texcoords) slots.push_back(kv.first);
    std::sort(slots.begin(), slots.end());
    for (uint32_t slot : slots) {
      hsh.U64(slot);
      HashVertexAttribute(m.texcoords.at(slot), &hsh);
    }

    HashVertexAttribute(m.vertex_colors, &hsh);
    hsh.U64(uint64_t(m.material_id));
    hsh.U64(uint64_t(m.skel_id));
  }

  for (const auto &mat : scene.materials) {
    hsh.Str(mat.name);
  }

  // Node hierarchy from the root node.
  if (scene.default_root_node < scene.nodes.size()) {
    HashNode(scene.nodes[scene.default_root_node], &hsh);
  }

  return hsh.digest();
}

BenchResult Bench(const std::string &path, int iters, int num_threads) {
  BenchResult res;

  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDFromFile(path, &stage, &warn, &err)) {
    res.err = "load failed: " + err;
    if (!warn.empty()) res.err += " warn: " + warn;
    return res;
  }

  lightusd::tydra::RenderSceneConverterEnv env(stage);
  env.usd_filename = path;
  if (num_threads != -2) {
    // -2 keeps the config default (auto); otherwise override for A/B runs.
    env.scene_config.num_threads = num_threads;
  }

  lightusd::tydra::RenderSceneConverter converter;

  lightusd::tydra::RenderScene scene;
  std::vector<double> times;
  for (int it = 0; it < iters; it++) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!converter.ConvertToRenderScene(env, &scene)) {
      res.err = "ConvertToRenderScene failed: " + converter.GetError();
      return res;
    }
    const auto t1 = std::chrono::steady_clock::now();
    times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  res.ms = MedianMs(times);
  res.hash = HashScene(scene);
  res.meshes = scene.meshes.size();
  for (const auto &m : scene.meshes) {
    res.faces += m.faceVertexCounts().size();
    res.tris += m.faceVertexIndices().size() / 3;
  }
  res.ok = true;
  if (getenv("PERFRC_TIMING")) {
    std::fprintf(stderr, "timing:\n%s\n", converter.GetTimingInfo().c_str());
  }
  return res;
}

}  // namespace legacy_bench

#endif  // PERFRC_ENABLE_LEGACY

#if PERFRC_HAS_NEXT

// ---------------------------- next pipeline -----------------------------

namespace next_bench {

template <typename ChunkedT>
void HashChunked(const ChunkedT &arr, Hasher *hsh) {
  hsh->U64(arr.size());
  for (size_t i = 0; i < arr.size(); i++) {
    hsh->Pod(arr[i]);
  }
}

uint64_t HashScene(const lightusd::tydra::next::RenderScene &scene) {
  Hasher hsh;

  hsh.U64(scene.meshes.size());
  hsh.U64(scene.materials.size());
  hsh.U64(scene.images.size());
  hsh.U64(scene.textures.size());
  hsh.U64(scene.lights.size());
  hsh.U64(scene.cameras.size());
  hsh.U64(scene.animations.size());
  hsh.U64(scene.skeletons.size());
  hsh.U64(scene.nodes.size());

  for (const auto &m : scene.meshes) {
    hsh.Str(m.prim_path);
    HashChunked(m.face_vertex_counts, &hsh);
    HashChunked(m.face_vertex_indices, &hsh);
    HashChunked(m.points, &hsh);
    HashChunked(m.normals, &hsh);
    HashChunked(m.texcoords_0, &hsh);
    hsh.Str(m.texcoords_0_name);
    hsh.U64(uint64_t(m.material_id));
  }

  for (const auto &mat : scene.materials) {
    hsh.Str(mat.name);
  }

  for (const auto &n : scene.nodes) {
    hsh.Str(n.prim_path);
    hsh.U64(uint64_t(n.data_id));
    hsh.U64(n.children.size());
  }

  return hsh.digest();
}

BenchResult Bench(const std::string &path, int iters, int threads) {
  BenchResult res;

  lightusd::next::StageSession session;
  if (!session.OpenFile(path)) {
    res.err = "next StageSession open failed: " + session.GetError();
    return res;
  }
  const lightusd::next::StageSnapshot stage_snapshot = session.GetSnapshot();
  const lightusd::next::Stage &stage = *stage_snapshot;

  lightusd::tydra::next::ConverterConfig cfg;
  cfg.execution.max_threads = threads > 0 ? threads : 0;
  cfg.mesh.compute_normals = true;
  cfg.mesh.triangulate = true;
  cfg.mesh.build_vertex_indices = true;

  std::vector<double> times;
  lightusd::tydra::next::RenderScene ref_scene;
  for (int it = 0; it < iters; it++) {
    lightusd::tydra::next::RenderSceneConverter conv(cfg);
    const auto t0 = std::chrono::steady_clock::now();
    lightusd::tydra::next::ConvertResult result = conv.Convert(stage);
    const auto t1 = std::chrono::steady_clock::now();
    if (!result.success) {
      res.err = "next Convert failed: " + result.error;
      return res;
    }
    times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    if (it == 0) ref_scene = std::move(result.scene);
  }

  res.ms = MedianMs(times);
  res.hash = HashScene(ref_scene);
  res.meshes = ref_scene.meshes.size();
  for (const auto &m : ref_scene.meshes) {
    res.faces += m.face_vertex_counts.size();
    res.tris += m.face_vertex_indices.size() / 3;
  }
  res.ok = true;
  return res;
}

}  // namespace next_bench

#endif  // PERFRC_HAS_NEXT

// -------------------------------- main ----------------------------------

int main(int argc, char **argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) {
    return 1;
  }

  constexpr bool have_legacy = (PERFRC_ENABLE_LEGACY != 0);
  constexpr bool have_next = (PERFRC_HAS_NEXT != 0);

  std::string scene_path = opts.scene;
  std::string cleanup_path;
  if (scene_path.empty()) {
    std::error_code temp_error;
    std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(temp_error);
    if (temp_error) {
      temp_dir = std::filesystem::current_path(temp_error);
    }
    if (temp_error) {
      std::fprintf(stderr, "failed to determine temporary directory: %s\n",
                   temp_error.message().c_str());
      return 1;
    }
    scene_path =
        (temp_dir / ("perfrc_scene_" + std::to_string(opts.prims) + ".usda"))
            .string();
    cleanup_path = scene_path;
    std::string err;
    if (!WriteFileText(scene_path, GenSceneUSDA(opts.prims), &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
  }

  const bool want_legacy = have_legacy && !opts.next_only;
  const bool want_next = have_next && !opts.legacy_only;

  if (opts.next_only && !have_next) {
    std::fprintf(stderr,
                 "next converter not compiled in (build with "
                 "PERFRC_ENABLE_NEXT)\n");
    return 1;
  }
  if (opts.legacy_only && !have_legacy) {
    std::fprintf(stderr,
                 "legacy converter not compiled in (built with "
                 "PERFRC_ENABLE_LEGACY=0)\n");
    return 1;
  }

  bool all_ok = true;

#if PERFRC_ENABLE_LEGACY
  if (want_legacy) {
    BenchResult r = legacy_bench::Bench(scene_path, opts.iters, opts.legacy_threads);
    if (!r.ok) {
      std::fprintf(stderr, "legacy: %s\n", r.err.c_str());
      all_ok = false;
    } else if (opts.json) {
      std::printf(
          "{\"pipeline\":\"legacy\",\"ms\":%.3f,\"threads\":%d,"
          "\"meshes\":%zu,\"faces\":%zu,\"tris\":%zu,\"hash\":%016llx}\n",
          r.ms, opts.legacy_threads, r.meshes, r.faces, r.tris,
          static_cast<unsigned long long>(r.hash));
    } else {
      std::printf("[legacy] median %.3f ms | meshes %zu faces %zu tris %zu "
                  "| hash %016llx\n",
                  r.ms, r.meshes, r.faces, r.tris,
                  static_cast<unsigned long long>(r.hash));
    }
  }
#endif

#if PERFRC_HAS_NEXT
  if (want_next) {
    BenchResult r = next_bench::Bench(scene_path, opts.iters, opts.threads);
    if (!r.ok) {
      std::fprintf(stderr, "next: %s\n", r.err.c_str());
      all_ok = false;
    } else if (opts.json) {
      std::printf(
          "{\"pipeline\":\"next\",\"ms\":%.3f,\"threads\":%d,\"meshes\":%zu,"
          "\"faces\":%zu,\"tris\":%zu,\"hash\":%016llx}\n",
          r.ms, opts.threads, r.meshes, r.faces, r.tris,
          static_cast<unsigned long long>(r.hash));
    } else {
      std::printf("[next]   median %.3f ms (threads=%d) | meshes %zu "
                  "faces %zu tris %zu | hash %016llx\n",
                  r.ms, opts.threads, r.meshes, r.faces, r.tris,
                  static_cast<unsigned long long>(r.hash));
    }
  }
#endif

  if (!cleanup_path.empty()) {
    std::remove(cleanup_path.c_str());
  }

  return all_ok ? 0 : 1;
}
