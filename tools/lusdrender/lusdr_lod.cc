// SPDX-License-Identifier: Apache-2.0
// lusdrender -- view-dependent district LOD pre-pass (-lodStream).
//
// Whole-island districtLod=full does not fit a single host: every backend
// eagerly composes all districts into one in-memory triangle soup before the
// GPU is touched (see doc/large-scene.md). This pre-pass makes LOD
// view-dependent instead: it composes the scene cheaply in proxy LOD, ranks the
// districts by camera distance, and promotes the NEAREST districts to the
// `full` districtLod variant -- via a generated wrapper layer (the same trick
// that works by hand) -- until a host RSS / GPU VRAM budget is reached. Far
// districts stay proxy. The normal render flow then loads the wrapper.
//
// This is the load-time form of streamed LOD: only the selected full payloads
// get composed/resident; it is not yet per-frame incremental streaming.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

#include "next/pcp/prim-index.hh"  // pcp::CompositionOptions
#include "tydra/next/resource-budget.hh"
#include "lusdr_context.hh"
#include "io-util.hh"

#ifdef HAVE_VULKAN
extern "C" {
#include "lightrt_c_vk.h"
}
#endif

namespace lusdr {

size_t QueryDeviceLocalVRAMBytes() {
#ifdef HAVE_VULKAN
  return static_cast<size_t>(lrt_vk_device_local_bytes(/*prefer_discrete=*/1));
#else
  return 0;
#endif
}

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// Per-district accumulation from the proxy pass.
struct District {
  std::string path;   // full prim path of the district (under the container)
  std::string name;   // leaf component (e.g. "map_capital")
  double verts = 0.0; // proxy vertex count (relative complexity signal)
  Vec3 bbmin{1e30f, 1e30f, 1e30f};
  Vec3 bbmax{-1e30f, -1e30f, -1e30f};
  bool has_bounds = false;
  // filled later
  float dist = 0.0f;   // camera distance to the district centroid
  float align = 1.0f;  // dot(dir-to-district, camera forward): 1=ahead, <0=behind
  double score = 0.0;  // ranking: projected coverage weighted by view-alignment
  bool full = false;
};

// Expand `d`'s world AABB by the 8 corners of a local AABB transformed by world.
void ExpandByLocalAABB(District *d, const Vec3 &lmin, const Vec3 &lmax,
                       const matrix4d &world) {
  for (int c = 0; c < 8; ++c) {
    Vec3 corner{(c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y,
                (c & 4) ? lmax.z : lmin.z};
    Vec3 w = TransformPoint(world, corner);
    d->bbmin.x = std::min(d->bbmin.x, w.x);
    d->bbmin.y = std::min(d->bbmin.y, w.y);
    d->bbmin.z = std::min(d->bbmin.z, w.z);
    d->bbmax.x = std::max(d->bbmax.x, w.x);
    d->bbmax.y = std::max(d->bbmax.y, w.y);
    d->bbmax.z = std::max(d->bbmax.z, w.z);
  }
  d->has_bounds = true;
}

// District = the component immediately following `/<container>/` in a prim path.
// Returns false if `path` is not under the container. Fills the district's full
// prim path and leaf name.
bool DistrictOf(const std::string &path, const std::string &container,
                std::string *district_path, std::string *name) {
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

std::string AbsolutePath(const std::string &p) {
#ifdef _WIN32
  char buf[_MAX_PATH];
  if (_fullpath(buf, p.c_str(), _MAX_PATH)) return std::string(buf);
#else
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return std::string(buf);
#endif
  return p;
}

}  // namespace

bool PrepareLodStream(Options *opt, std::string *generated_wrapper) {
  const double time = opt->timecode;

  // 1) Compose the scene in proxy LOD (cheap: ~1.7 GiB for Caldera).
  lightusd::next::Stage stage;
  std::string warn, err;
  lightusd::next::pcp::CompositionOptions copts;  // authored (proxy) selections
  if (!lightusd::next::LoadUSDComposed(opt->input, &stage, &warn, &err, &copts)) {
    std::cerr << "[lodStream] proxy compose failed: " << err << "\n";
    return false;
  }
  if (!warn.empty()) std::cerr << "[lodStream] WARN: " << warn << "\n";

  // 2) Reference point: the framed USD camera, else the scene-bbox centre.
  CameraFrame cam;
  float aspect = 1.0f;
  bool have_cam = FindNextCameraFrame(stage, opt->camera, time, &cam, &aspect);
  Vec3 cam_ref = have_cam ? cam.origin : Vec3{0, 0, 0};

  // 3) Collect proxy meshes and aggregate per district.
  std::vector<MeshJobNext> jobs;
  for (const auto &root : stage.GetRootPrims()) {
    CollectRTPreviewMeshesNext(stage, root, matrix4d::identity(),
                               lightusd::Purpose::Default, time, opt->mask,
                               &jobs);
  }

  std::map<std::string, District> districts;
  Vec3 scene_min{1e30f, 1e30f, 1e30f}, scene_max{-1e30f, -1e30f, -1e30f};
  for (MeshJobNext &job : jobs) {
    const std::string path = job.prim.GetPath().str();
    std::string dpath, dname;
    if (!DistrictOf(path, opt->lod_container, &dpath, &dname)) continue;

    const lightusd::next::Value *val = job.prim.GetPropertyValue("points");
    if (!val) continue;
    const auto pts = val->as_float_array();
    if (!pts || pts->empty()) continue;
    const size_t nv = pts->size() / 3;

    Vec3 lmin{1e30f, 1e30f, 1e30f}, lmax{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nv; ++i) {
      Vec3 p{(*pts)[i * 3 + 0], (*pts)[i * 3 + 1], (*pts)[i * 3 + 2]};
      lmin.x = std::min(lmin.x, p.x);
      lmin.y = std::min(lmin.y, p.y);
      lmin.z = std::min(lmin.z, p.z);
      lmax.x = std::max(lmax.x, p.x);
      lmax.y = std::max(lmax.y, p.y);
      lmax.z = std::max(lmax.z, p.z);
    }

    District &d = districts[dpath];
    if (d.path.empty()) {
      d.path = dpath;
      d.name = dname;
    }
    d.verts += double(nv);
    ExpandByLocalAABB(&d, lmin, lmax, job.world);
    scene_min.x = std::min(scene_min.x, d.bbmin.x);
    scene_min.y = std::min(scene_min.y, d.bbmin.y);
    scene_min.z = std::min(scene_min.z, d.bbmin.z);
    scene_max.x = std::max(scene_max.x, d.bbmax.x);
    scene_max.y = std::max(scene_max.y, d.bbmax.y);
    scene_max.z = std::max(scene_max.z, d.bbmax.z);
  }

  if (districts.empty()) {
    std::cerr << "[lodStream] no districts found under '" << opt->lod_container
              << "' -- rendering scene as authored.\n";
    return false;
  }
  if (!have_cam) {
    cam_ref = Mul(Add(scene_min, scene_max), 0.5f);
    std::cerr << "[lodStream] camera '" << opt->camera
              << "' not resolved; ranking districts by distance to scene"
                 " centre.\n";
  }

  // 4) Rank by a screen-space-importance score: projected coverage
  //    (proxyVerts / distance^2) weighted by how centred the district is in the
  //    view (dot of the to-district direction with the camera forward, squared).
  //    Pure distance fails for "overview" cameras -- it rewards small gameplay
  //    overlays that happen to sit near the eye over the dense district the shot
  //    actually frames; coverage adds geometric weight, alignment adds "what the
  //    camera looks at". Skip trivially-small children (trigger/volume/bounds
  //    prims that author no `full` geometry and would only waste budget).
  std::vector<District> ranked;
  ranked.reserve(districts.size());
  int n_skipped = 0;
  for (auto &kv : districts) {
    District d = kv.second;
    if (d.verts < opt->lod_min_verts ||
        (opt->lod_max_verts > 0.0 && d.verts > opt->lod_max_verts)) {
      ++n_skipped;
      continue;
    }
    Vec3 centre = d.has_bounds ? Mul(Add(d.bbmin, d.bbmax), 0.5f) : Vec3{0, 0, 0};
    Vec3 to = Sub(centre, cam_ref);
    d.dist = Length(to);
    // View alignment: 1 dead-ahead, 0 at 90 deg, <0 behind. No camera -> treat
    // all as ahead (fall back to pure coverage ranking around the scene centre).
    d.align = (have_cam && d.dist > 1e-3f)
                  ? Dot(Mul(to, 1.0f / d.dist), cam.forward)
                  : 1.0f;
    const double dist2 = double(d.dist) * double(d.dist) + 1.0;  // +1: avoid /0
    const double coverage = d.verts / dist2;
    const double a = d.align > 0.0f ? double(d.align) : 0.0;
    d.score = coverage * a * a;  // behind-camera districts score ~0
    ranked.push_back(d);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const District &a, const District &b) { return a.score > b.score; });
  if (ranked.empty()) {
    std::cerr << "[lodStream] no districts above " << opt->lod_min_verts
              << " proxy verts -- rendering scene as authored.\n";
    return false;
  }

  // 5) Budgets. Host: -maxMem else 50% of MemAvailable. VRAM (GPU backends
  //    only): -maxVram else 50% of the device-local heap. Cost model: a flat
  //    per-district charge (proxy geometry does not predict full cost, so we do
  //    not model per-district variation -- conservative and predictable).
  const bool gpu = opt->vulkan || opt->vulkan_rt || opt->use_d3d || opt->hip;
  const double host_avail = double(MemBudget::AvailableSystemMemory());
  const uint64_t host_capacity = host_avail > 0.0
                                     ? std::min<uint64_t>(
                                           static_cast<uint64_t>(host_avail),
                                           lightusd::tydra::next::GiB(32))
                                     : lightusd::tydra::next::GiB(32);
  const double vram_total = gpu ? double(QueryDeviceLocalVRAMBytes()) : 0.0;
  const lightusd::tydra::next::ResourceBudget automatic =
      lightusd::tydra::next::ComputeResourceBudget(
          host_capacity, static_cast<uint64_t>(vram_total));
  const double host_budget = opt->max_mem_gib > 0.0
                                 ? opt->max_mem_gib * kGiB
                                 : double(automatic.host_limit);
  const double vram_budget =
      !gpu ? 1e30
           : (opt->max_vram_gib > 0.0 ? opt->max_vram_gib * kGiB
                                      : double(automatic.vram_limit));
  const double cost_host = opt->lod_district_mem_gib * kGiB;
  const double cost_vram = opt->lod_district_vram_gib * kGiB;
  const double base_host = double(MemBudget::ProcessRSS());  // proxy structure

  // Give the hard memory-abort cap headroom ABOVE the selection budget: the flat
  // per-district charge can undershoot the heaviest district pair, and with the
  // auto cap equal to the selection budget a modest underestimate aborts the
  // render instead of completing. When -maxMem was set, that value is the user's
  // explicit hard cap and is respected; otherwise raise the cap to 90% of avail
  // (still an OOM safety net, well under physical RAM).
  if (opt->max_mem_gib <= 0.0 && host_avail > 0.0) {
    MemBudget::Get().Init(0.9 * host_avail / kGiB);
  }

  // 6) Greedy promotion, nearest first, under both budgets. Always promote the
  //    nearest district so we render something even on a tiny budget.
  double host_used = base_host, vram_used = 0.0;
  int n_full = 0;
  for (size_t i = 0; i < ranked.size(); ++i) {
    const bool first = (i == 0);
    if (first || (host_used + cost_host <= host_budget &&
                  vram_used + cost_vram <= vram_budget)) {
      ranked[i].full = true;
      host_used += cost_host;
      vram_used += cost_vram;
      ++n_full;
    }
  }

  // 7) Report the decision.
  std::cerr << "[lodStream] budgets: host "
            << (host_budget / kGiB) << " GiB (avail " << (host_avail / kGiB)
            << ", proxy " << (base_host / kGiB) << ")";
  if (gpu)
    std::cerr << ", vram " << (vram_budget / kGiB) << " GiB (device "
              << (vram_total / kGiB) << ")";
  std::cerr << "\n[lodStream] " << ranked.size() << " districts (" << n_skipped
            << " sub-threshold skipped), promoting " << n_full
            << " to full @ " << opt->lod_district_mem_gib << " GiB host";
  if (gpu) std::cerr << "/" << opt->lod_district_vram_gib << " GiB vram each";
  std::cerr << " (cam ref " << cam_ref.x << "," << cam_ref.y << ","
            << cam_ref.z << ")\n";
  std::cerr << "[lodStream]   est host " << (host_used / kGiB) << " GiB";
  if (gpu) std::cerr << ", vram " << (vram_used / kGiB) << " GiB";
  std::cerr << "\n";
  for (const District &d : ranked) {
    std::cerr << "[lodStream]   " << (d.full ? "FULL  " : "proxy ")
              << d.name << "  score=" << d.score << "  dist=" << d.dist
              << "  align=" << d.align << "  proxyVerts=" << uint64_t(d.verts)
              << "\n";
  }

  // 8) Generate the wrapper layer: subLayer the original (absolute) and promote
  // each chosen district to full via a nested `over` mirroring its path.
  std::vector<const District *> full;
  for (const District &d : ranked)
    if (d.full) full.push_back(&d);
  if (full.empty()) return false;

  // Path prefix shared by all districts: components up to and including the
  // container. e.g. /world/mp_wz_island/mp_wz_island_paths/mp_wz_island_geo
  std::vector<std::string> prefix;
  {
    const std::string &p = full[0]->path;
    size_t pos = 0;
    while (pos < p.size()) {
      if (p[pos] == '/') {
        size_t e = p.find('/', pos + 1);
        if (e == std::string::npos) e = p.size();
        std::string comp = p.substr(pos + 1, e - pos - 1);
        if (comp == full[0]->name) break;  // stop before the district leaf
        if (!comp.empty()) prefix.push_back(comp);
        pos = e;
      } else {
        ++pos;
      }
    }
  }

  const std::string abs_input = AbsolutePath(opt->input);
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  const std::string wrapper =
      lightusd::io::GetTempDir(opt->temp_directory) + "/lusdr_lod_" + std::to_string(pid) + ".usda";
  std::ofstream ofs(wrapper);
  if (!ofs) {
    std::cerr << "[lodStream] cannot write wrapper " << wrapper << "\n";
    return false;
  }
  ofs << "#usda 1.0\n(\n    subLayers = [\n        @" << abs_input
      << "@\n    ]\n    upAxis = \"" << stage.GetUpAxis() << "\"\n)\n\n";
  std::string indent;
  for (const std::string &comp : prefix) {
    ofs << indent << "over \"" << comp << "\"\n" << indent << "{\n";
    indent += "    ";
  }
  for (const District *d : full) {
    ofs << indent << "over \"" << d->name
        << "\" (\n" << indent << "    variants = {\n" << indent
        << "        string districtLod = \"full\"\n" << indent << "    }\n"
        << indent << ")\n" << indent << "{\n" << indent << "}\n";
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    indent.resize(indent.size() - 4);
    ofs << indent << "}\n";
  }
  ofs.close();

  std::cerr << "[lodStream] wrote wrapper " << wrapper << " ("
            << full.size() << " full districts); rendering it.\n";
  *generated_wrapper = wrapper;
  opt->input = wrapper;
  return true;
}

}  // namespace lusdr
