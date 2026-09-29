// SPDX-License-Identifier: Apache-2.0
// lusdview — view-dependent district LOD pre-pass (see lod_stream.hh). Mirrors
// tools/lusdrender/lusdr_lod.cc: compose the proxy scene, walk it at prim
// granularity to aggregate per-district world bounds + proxy vert counts (the
// viewer's --next DrawScene merges meshes, losing district granularity, so we
// walk the composed Stage through C/POD prim handles), rank by view importance,
// promote the nearest
// under host/VRAM budgets, and emit a wrapper layer the viewer then loads.
#include "lod_stream.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>  // _getpid
#include <stdlib.h>   // _fullpath, _MAX_PATH
#else
#include <climits>   // PATH_MAX
#include <unistd.h>  // realpath, getpid
#endif

#include "hipew.h"                     // HIP VRAM query
#include "c-api/lightusd-cpp.hh"

namespace lusdview {

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

struct District {
  std::string path, name;
  double verts = 0.0;
  float bbmin[3] = {1e30f, 1e30f, 1e30f};
  float bbmax[3] = {-1e30f, -1e30f, -1e30f};
  bool has_bounds = false;
  float dist = 0.0f;
  float align = 1.0f;
  double score = 0.0;
  bool full = false;
};

// Row-vector convention: translation occupies the fourth matrix row.
void XformPoint(const double m[16], float x, float y, float z, float o[3]) {
  for (int c = 0; c < 3; ++c) {
    o[c] = float(double(x) * m[c] + double(y) * m[4 + c] +
                 double(z) * m[8 + c] + m[12 + c]);
  }
}

bool Equals(lightusd_sv value, const char* literal) {
  const size_t n = std::strlen(literal);
  return value.len == n && value.data && std::memcmp(value.data, literal, n) == 0;
}

std::string Copy(lightusd_sv value) {
  return value.data ? std::string(value.data, value.len) : std::string();
}

struct CameraPose {
  float eye[3]{};
  float forward[3]{};
};

bool FindCamera(const lightusd_stage* stage, lightusd_prim prim,
                const std::string& name, double time, CameraPose* out) {
  if (!lightusd_prim_is_valid(prim) || !lightusd_prim_is_active(prim)) return false;
  if (Equals(lightusd_prim_type_name(prim), "Camera")) {
    const std::string path = Copy(lightusd_prim_path(prim));
    const std::string pname = Copy(lightusd_prim_name(prim));
    const bool match = name.empty() || pname == name || path == name ||
        (path.size() > name.size() &&
         path.compare(path.size() - name.size(), name.size(), name) == 0 &&
         path[path.size() - name.size() - 1] == '/');
    if (match) {
      double m[16];
      if (lightusd_prim_world_transform(stage, prim, time, m) == LIGHTUSD_OK) {
        for (int c = 0; c < 3; ++c) {
          out->eye[c] = float(m[12 + c]);
          out->forward[c] = -float(m[8 + c]);
        }
        const float length = std::sqrt(out->forward[0] * out->forward[0] +
                                       out->forward[1] * out->forward[1] +
                                       out->forward[2] * out->forward[2]);
        if (length > 1e-12f) {
          for (float& component : out->forward) component /= length;
        }
        return true;
      }
    }
  }
  for (size_t i = 0; i < lightusd_prim_child_count(prim); ++i) {
    if (FindCamera(stage, lightusd_prim_child(prim, i), name, time, out)) return true;
  }
  return false;
}

bool DistrictOf(const std::string& path, const std::string& container,
                std::string* district_path, std::string* name) {
  const std::string needle = "/" + container + "/";
  size_t c = path.find(needle);
  if (c == std::string::npos) return false;
  size_t start = c + needle.size();
  size_t end = path.find('/', start);
  if (end == std::string::npos) end = path.size();
  if (end <= start) return false;
  *name = path.substr(start, end - start);
  *district_path = path.substr(0, end);
  return true;
}

// Recursively walk the composed stage, accumulating world transforms, and add
// each Mesh's world-AABB + vert count to its district.
void WalkDistricts(const lightusd_stage* stage, lightusd_prim prim, double time,
                   const std::string& container,
                   std::map<std::string, District>* districts) {
  if (!lightusd_prim_is_valid(prim) || !lightusd_prim_is_active(prim)) return;
  if (Equals(lightusd_prim_type_name(prim), "Mesh")) {
    std::string dpath, dname;
    if (DistrictOf(Copy(lightusd_prim_path(prim)), container, &dpath, &dname)) {
      lightusd_value_view points{};
      double world[16];
      if (lightusd_attr_get(prim, "points", &points) == LIGHTUSD_OK &&
          points.storage == LIGHTUSD_COMP_FLOAT32 && points.components == 3 &&
          points.data && points.count && points.nbytes >= 3 * sizeof(float) &&
          lightusd_prim_world_transform(stage, prim, time, world) == LIGHTUSD_OK) {
        const size_t nv = std::min(points.count, points.nbytes / (3 * sizeof(float)));
        const auto* pts = static_cast<const float*>(points.data);
        float lmin[3] = {1e30f, 1e30f, 1e30f}, lmax[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nv; ++i)
          for (int k = 0; k < 3; ++k) {
            float v = pts[i * 3 + k];
            lmin[k] = std::min(lmin[k], v);
            lmax[k] = std::max(lmax[k], v);
          }
        District& d = (*districts)[dpath];
        if (d.path.empty()) { d.path = dpath; d.name = dname; }
        d.verts += double(nv);
        for (int c = 0; c < 8; ++c) {
          float w[3];
          XformPoint(world, (c & 1) ? lmax[0] : lmin[0],
                     (c & 2) ? lmax[1] : lmin[1], (c & 4) ? lmax[2] : lmin[2], w);
          for (int k = 0; k < 3; ++k) {
            d.bbmin[k] = std::min(d.bbmin[k], w[k]);
            d.bbmax[k] = std::max(d.bbmax[k], w[k]);
          }
        }
        d.has_bounds = true;
      }
    }
  }
  // Mirror lusdrender: do not descend into a PointInstancer (its prototype
  // geometry is placed separately; counting it here would misplace it).
  if (Equals(lightusd_prim_type_name(prim), "PointInstancer")) return;
  for (size_t i = 0; i < lightusd_prim_child_count(prim); ++i)
    WalkDistricts(stage, lightusd_prim_child(prim, i), time, container, districts);
}

std::string AbsolutePath(const std::string& p) {
#ifdef _WIN32
  char buf[_MAX_PATH];
  if (_fullpath(buf, p.c_str(), _MAX_PATH)) return std::string(buf);
#else
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return std::string(buf);
#endif
  return p;
}
std::string TempDir() {
#ifdef _WIN32
  const char* t = std::getenv("TEMP");
  if (!t || !*t) t = std::getenv("TMP");
  if (t && *t) return std::string(t);
  return ".";
#else
  const char* t = std::getenv("TMPDIR");
  if (t && *t) return std::string(t);
  return "/tmp";
#endif
}
size_t ReadMeminfoBytes(const char* path, const char* key) {
  std::ifstream f(path);
  std::string tok;
  while (f >> tok) {
    if (tok == key) {
      size_t kb = 0;
      f >> kb;
      return kb * 1024;
    }
    std::getline(f, tok);
  }
  return 0;
}
size_t HipVramBytes() {
  if (hipewInit(HIPEW_INIT_HIP) != HIPEW_SUCCESS) return 0;
  if (!hipInit || hipInit(0) != hipSuccess) return 0;
  if (!hipSetDevice || hipSetDevice(0) != hipSuccess) return 0;
  size_t freeB = 0, totalB = 0;
  if (!hipMemGetInfo || hipMemGetInfo(&freeB, &totalB) != hipSuccess) return 0;
  return totalB;
}

}  // namespace

std::string PrepareLodStream(const std::string& input, const LodStreamOptions& o) {
  // 1) Compose the proxy scene (authored districtLod=proxy selections).
  lightusd::api::Stage stage;
  lightusd_load_options load_options;
  lightusd::api::InitLoadOptions(&load_options);
  load_options.preserve_native_instances = 1;
  if (stage.load(input.c_str(), &load_options) != LIGHTUSD_OK) {
    std::cerr << "[lodStream] proxy compose failed: " << lightusd_last_error() << "\n";
    return "";
  }

  // 2) Reference camera (else scene centre).
  CameraPose cam;
  bool have_cam = false;
  if (!o.camera.empty()) {
    for (size_t i = 0; i < stage.root_prim_count(); ++i) {
      if (FindCamera(stage.get(), lightusd_stage_root_prim(stage.get(), i),
                     o.camera, o.time, &cam)) {
        have_cam = true;
        break;
      }
    }
  }

  // 3) Walk the stage and aggregate per district.
  std::map<std::string, District> districts;
  for (size_t i = 0; i < stage.root_prim_count(); ++i)
    WalkDistricts(stage.get(), lightusd_stage_root_prim(stage.get(), i),
                  o.time, o.container, &districts);
  if (districts.empty()) {
    std::cerr << "[lodStream] no districts under '" << o.container
              << "' -- loading scene as authored.\n";
    return "";
  }

  float cam_ref[3] = {0, 0, 0};
  if (have_cam) {
    for (int k = 0; k < 3; ++k) cam_ref[k] = cam.eye[k];
  } else {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& kv : districts)
      for (int k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], kv.second.bbmin[k]);
        hi[k] = std::max(hi[k], kv.second.bbmax[k]);
      }
    for (int k = 0; k < 3; ++k) cam_ref[k] = 0.5f * (lo[k] + hi[k]);
    std::cerr << "[lodStream] camera '" << o.camera
              << "' not resolved; ranking by distance to scene centre.\n";
  }

  // 4) Rank by projected coverage (verts / dist^2) weighted by view alignment^2.
  std::vector<District> ranked;
  int n_skipped = 0;
  for (auto& kv : districts) {
    District d = kv.second;
    if (d.verts < o.minVerts) { ++n_skipped; continue; }
    float centre[3], to[3];
    for (int k = 0; k < 3; ++k) {
      centre[k] = 0.5f * (d.bbmin[k] + d.bbmax[k]);
      to[k] = centre[k] - cam_ref[k];
    }
    d.dist = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    d.align = (have_cam && d.dist > 1e-3f)
                  ? (to[0] * cam.forward[0] + to[1] * cam.forward[1] +
                     to[2] * cam.forward[2]) / d.dist
                  : 1.0f;
    const double dist2 = double(d.dist) * double(d.dist) + 1.0;
    const double a = d.align > 0.0f ? double(d.align) : 0.0;
    d.score = (d.verts / dist2) * a * a;
    ranked.push_back(d);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const District& a, const District& b) { return a.score > b.score; });
  if (ranked.empty()) {
    std::cerr << "[lodStream] no districts above " << o.minVerts
              << " proxy verts -- loading scene as authored.\n";
    return "";
  }

  // 5) Budgets + greedy nearest-first promotion (always promote at least one).
  const double host_avail = double(ReadMeminfoBytes("/proc/meminfo", "MemAvailable:"));
  const double host_budget =
      o.maxMemGiB > 0.0 ? o.maxMemGiB * kGiB : 0.5 * host_avail;
  const double vram_total = double(HipVramBytes());
  const double vram_budget =
      o.maxVramGiB > 0.0 ? o.maxVramGiB * kGiB
                         : (vram_total > 0.0 ? 0.5 * vram_total : 1e30);
  const double cost_host = o.districtMemGiB * kGiB;
  const double cost_vram = o.districtVramGiB * kGiB;
  double host_used = double(ReadMeminfoBytes("/proc/self/status", "VmRSS:"));
  double vram_used = 0.0;
  int n_full = 0;
  for (size_t i = 0; i < ranked.size(); ++i) {
    if (i == 0 || (host_used + cost_host <= host_budget &&
                   vram_used + cost_vram <= vram_budget)) {
      ranked[i].full = true;
      host_used += cost_host;
      vram_used += cost_vram;
      ++n_full;
    }
  }

  std::cerr << "[lodStream] budgets: host " << (host_budget / kGiB)
            << " GiB (avail " << (host_avail / kGiB) << "), vram "
            << (vram_budget / kGiB) << " GiB (device " << (vram_total / kGiB)
            << ")\n[lodStream] " << ranked.size() << " districts (" << n_skipped
            << " sub-threshold), promoting " << n_full << " to full\n";
  for (const District& d : ranked) {
    std::cerr << "[lodStream]   " << (d.full ? "FULL  " : "proxy ") << d.name
              << "  score=" << d.score << "  dist=" << d.dist
              << "  align=" << d.align << "  proxyVerts=" << uint64_t(d.verts)
              << "\n";
  }

  // 6) Write the wrapper: subLayer the original + promote each chosen district.
  std::vector<const District*> full;
  for (const District& d : ranked)
    if (d.full) full.push_back(&d);
  if (full.empty()) return "";

  std::vector<std::string> prefix;
  {
    const std::string& p = full[0]->path;
    size_t pos = 0;
    while (pos < p.size()) {
      if (p[pos] == '/') {
        size_t e = p.find('/', pos + 1);
        if (e == std::string::npos) e = p.size();
        std::string comp = p.substr(pos + 1, e - pos - 1);
        if (comp == full[0]->name) break;
        if (!comp.empty()) prefix.push_back(comp);
        pos = e;
      } else {
        ++pos;
      }
    }
  }

  const std::string abs_input = AbsolutePath(input);
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  const std::string wrapper =
      TempDir() + "/lusdview_lod_" + std::to_string(pid) + ".usda";
  std::string up_axis = "Y";
  lightusd::api::Value axis_value;
  lightusd_sv axis_text{};
  if (stage.metadata("upAxis", &axis_value) == LIGHTUSD_OK &&
      lightusd_value_get_string(axis_value.get(), &axis_text) == LIGHTUSD_OK) {
    up_axis = Copy(axis_text);
  }
  std::ofstream ofs(wrapper);
  if (!ofs) {
    std::cerr << "[lodStream] cannot write wrapper " << wrapper << "\n";
    return "";
  }
  ofs << "#usda 1.0\n(\n    subLayers = [\n        @" << abs_input
      << "@\n    ]\n    upAxis = \"" << up_axis << "\"\n)\n\n";
  std::string indent;
  for (const std::string& comp : prefix) {
    ofs << indent << "over \"" << comp << "\"\n" << indent << "{\n";
    indent += "    ";
  }
  for (const District* d : full) {
    ofs << indent << "over \"" << d->name << "\" (\n"
        << indent << "    variants = {\n"
        << indent << "        string districtLod = \"full\"\n"
        << indent << "    }\n" << indent << ")\n" << indent << "{\n"
        << indent << "}\n";
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    indent.resize(indent.size() - 4);
    ofs << indent << "}\n";
  }
  ofs.close();

  std::cerr << "[lodStream] wrote wrapper " << wrapper << " (" << full.size()
            << " full districts); loading it.\n";
  return wrapper;
}

}  // namespace lusdview
