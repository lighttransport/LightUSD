// SPDX-License-Identifier: Apache-2.0
// Generated in-memory OBJ regressions and optional multi-gigabyte stress runs.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "external/tinyobjloader-c/tiny_obj_c.h"
#include "next/reader/obj-reader.hh"
namespace ia = lightusd::tydra::import_asset;
namespace n = lightusd::next;
namespace {
void Check(bool ok, const char* what) {
  if (!ok) { std::cerr << "OBJ regression failed: " << what << '\n'; std::exit(1); }
}
const std::string triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
struct Allocations {
  size_t used = 0, peak = 0, calls = 0, fail_at = 0;
  size_t limit = 512 * 1024;
  static void* Alloc(void* ud, size_t size, size_t) {
    auto& a = *static_cast<Allocations*>(ud);
    if (++a.calls == a.fail_at || size > a.limit - a.used) return nullptr;
    void* p = std::malloc(size);
    if (p) { a.used += size; a.peak = std::max(a.peak, a.used); }
    return p;
  }
  static void Free(void* ud, void* ptr, size_t size) {
    auto& a = *static_cast<Allocations*>(ud);
    Check(a.used >= size, "allocator accounting");
    a.used -= size;
    std::free(ptr);
  }
  tobj_load_config Config() {
    auto c = tobj_default_config();
    c.num_threads = 1;
    c.allocator = {};
    c.allocator.alloc = Alloc; c.allocator.free = Free;
    c.allocator.user_data = this;
    return c;
  }
};
void Reject(const std::string& text, const tobj_load_config* config = nullptr) {
  tobj_scene_f f{}; tobj_scene_d d{};
  Check(tobj_load_obj_from_memory_f(&f, reinterpret_cast<const uint8_t*>(text.data()), text.size(), config, nullptr) != TOBJ_OK, text.c_str());
  Check(!f.arena && !f.attrib.vertices.ptr && !f.shapes, "failed float output cleared");
  Check(tobj_load_obj_from_memory_d(&d, reinterpret_cast<const uint8_t*>(text.data()), text.size(), config, nullptr) != TOBJ_OK, text.c_str());
  Check(!d.arena && !d.attrib.vertices.ptr && !d.shapes, "failed double output cleared");
}
void GeneratedNormals() {
  ia::Options options;
  for (const char* scale : {"e-40", "e-20", "e0", "e30"}) {
    const std::string one = std::string("1") + scale;
    const std::string two = std::string("2") + scale;
    const std::string vertices = "v 0 0 0\nv " + two + " 0 0\nv 0 " + one +
        " 0\nv 0 0 " + two + "\n";
    for (bool smoothing : {false, true}) {
      // The authored face contributes to the geometric smoothing of its
      // neighbor, even across an object boundary. Its own normals stay intact.
      const auto text = vertices + "vn 0 0 -3\n" + (smoothing ? "s 1\n" : "s off\n") +
          "o authored\nf 1//1 2//1 3//1\no generated\nf 1 4 2";
      std::string warn, err;
      auto layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(text.data()), text.size(), options, &warn, &err);
      Check(bool(layer), err.c_str());
      const auto authored = layer->prim_at_path("/World/Links/Geometry/authored")->property_value("normals")->as_float_array();
      Check(authored == std::vector<float>({0, 0, -3, 0, 0, -3, 0, 0, -3}), "authored normals retained without normalization");
      const auto generated = layer->prim_at_path("/World/Links/Geometry/generated")->property_value("normals")->as_float_array();
      // Account for source coordinates rounded to float subnormal values.
      const double first = std::strtof(one.c_str(), nullptr);
      const double second = std::strtof(two.c_str(), nullptr);
      for (size_t i = 0; i < 3; ++i) {
        const float y = smoothing && i != 1 ? float(second / std::hypot(first, second)) : 1;
        const float z = smoothing && i != 1 ? float(first / std::hypot(first, second)) : 0;
        Check(std::abs(generated[i * 3]) < 1e-6f &&
              std::abs(generated[i * 3 + 1] - y) < 1e-6f &&
              std::abs(generated[i * 3 + 2] - z) < 1e-6f, "scale-independent area-weighted normals");
      }
    }
  }
  // Smoothing groups do not need a geometric-normal map when every corner
  // already has an authored normal. Use disconnected faces to expose growth.
  std::string text = "vn 0 0 -3\ns 0\n";
  for (size_t i = 0; i < 5000; ++i) {
    const auto base = i * 3 + 1;
    text += triangle + "f " + std::to_string(base) + "//1 " +
        std::to_string(base + 1) + "//1 " + std::to_string(base + 2) + "//1\n";
  }
  ia::OBJ obj;
  ia::Result flat, smooth;
  std::string err;
  Check(ia::BuildOBJScene(text, "", options, &obj, &flat, &err), err.c_str());
  text[text.find("s 0") + 2] = '1';
  Check(ia::BuildOBJScene(text, "", options, &obj, &smooth, &err), err.c_str());
  Check(flat.memory_estimate == smooth.memory_estimate, "authored normals avoid smoothing-map growth");
  const auto cancelled = triangle + "s 1\nf 1 2 3\nf 3 2 1";
  std::string warn;
  auto layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(cancelled.data()), cancelled.size(), options, &warn, &err);
  Check(bool(layer), err.c_str());
  const auto normals = layer->prim_at_path("/World/Links/Geometry/mesh")->property_value("normals")->as_float_array();
  Check(normals == std::vector<float>(9, 0), "cancelled normals remain finite zero vectors");
}
void Hardening() {
  for (const char* record : {"f 0 2 3", "f 4 2 3", "f 1x 2 3", "f 1/ 2 3", "f 1/0 2 3", "f 1//4 2 3",
      "f 2147483648 2 3", "f -2147483648 2 3", "f -9999999999999999999999999 2 3",
      "vw 2147483648 0 1", "vw 0 2147483648 1", "vw 0 999999999999999999999 1",
      "v 0 0", "v 0 0 nan", "v 0 0 inf", "vn 0 0", "vt 1e999", "vt 0junk", "v 0 1 2e",
      "l 1x 2", "p 9999999999999999999999"}) Reject(triangle + record + '\n');
  auto c = tobj_default_config();
  c.max_indices = 3;
  Reject(triangle + "o a\nf 1 2 3\no b\nf 1 2 3\n", &c);
  Reject(triangle + "l 1 2\np 3 1\n", &c);
  Reject(triangle + "o a\np 1 2\no b\np 1 2\n", &c);
  c = tobj_default_config(); c.max_faces = 1;
  Reject(triangle + "o a\nf 1 2 3\no b\nf 1 2 3\n", &c);
  Reject(triangle + "v 1 1 0\nf 1 2 4 3\n", &c); // Two emitted triangles.
  c = tobj_default_config(); c.max_line_bytes = 16;
  Reject(triangle + "#" + std::string(16, 'x'), &c);
  c = tobj_default_config(); c.max_vertices = 2;
  Reject("vn 0 0 1\nvn 0 0 1\nvn 0 0 1\n", &c);
  Reject("vt 0 0\nvt 0 0\nvt 0 0\n", &c);

  // All allocation failures must unwind both scratch vectors and arena blocks.
  const auto weighted = triangle + "vw 0 0 .25 1 .75\nf 1 2 3\n";
  for (size_t fail = 1; fail < 80; ++fail) {
    Allocations a; a.fail_at = fail; c = a.Config();
    tobj_scene_f scene{};
    auto r = tobj_load_obj_from_memory_f(&scene, reinterpret_cast<const uint8_t*>(weighted.data()), weighted.size(), &c, nullptr);
    if (r == TOBJ_OK) tobj_scene_free_f(&scene);
    else Check(!scene.arena && !scene.attrib.skin_weights, "OOM output cleared");
    Check(!a.used, "OOM allocations released");
    if (r == TOBJ_OK) break;
    Check(fail < 79, "allocation failure sweep terminates");
  }
  // A million blank lines must not require a multi-megabyte line table.
  const std::string blank = std::string(1000000, '\n') + triangle + "f 1 2 3\n";
  Allocations a; c = a.Config();
  tobj_scene_f scene{};
  Check(tobj_load_obj_from_memory_f(&scene, reinterpret_cast<const uint8_t*>(blank.data()), blank.size(), &c, nullptr) == TOBJ_OK, "bounded blank-line memory");
  tobj_scene_free_f(&scene); Check(!a.used, "blank-line cleanup");
  tobj_callbacks_f callbacks{};
  Check(tobj_load_obj_with_callbacks_f(reinterpret_cast<const uint8_t*>(blank.data()), blank.size(), &callbacks, &c, nullptr) == TOBJ_OK, "bounded callback line memory");
  Check(!a.used, "callback cleanup");
  const auto mtl = std::string(1000000, '\n') + "newmtl test\nKd 1 0 0\n";
  tobj_material_list_f materials{};
  Check(tobj_parse_mtl_from_memory_f(&materials, reinterpret_cast<const uint8_t*>(mtl.data()), mtl.size(), &c, nullptr) == TOBJ_OK, "bounded MTL line memory");
  tobj_material_list_free_f(&materials); Check(!a.used, "MTL cleanup");

  // Leading zeroes must not exhaust the significant-digit accumulator.
  const auto tiny = std::string("v 0.00000000000000000001 0 0\nv 000000000000000000001 0 0\nv 1e-320 0 0\nv 0e99999999999999999999999 0 0\n");
  tobj_scene_d precise{};
  Check(tobj_load_obj_from_memory_d(&precise, reinterpret_cast<const uint8_t*>(tiny.data()), tiny.size(), nullptr, nullptr) == TOBJ_OK, "small decimal parsing");
  Check(std::abs(precise.attrib.vertices.ptr[0] / 1e-20 - 1) < 1e-12, "leading fractional zeros");
  Check(precise.attrib.vertices.ptr[3] == 1 && precise.attrib.vertices.ptr[6] > 0 && precise.attrib.vertices.ptr[9] == 0, "integer zeros and subnormal values");
  tobj_scene_free_d(&precise);
  ia::Options options; std::string warn, err;
  ia::Result payload;
  Check(ia::BuildOBJPayload(weighted, "", options, &payload, &err), "legacy JSON bridge");
  n::Layer compatibility;
  Check(n::ConvertOBJPayloadToLayer(payload.payload, &compatibility, &err), "JSON-to-layer compatibility");
  Check(compatibility.prim_at_path("/World/Links/Geometry/mesh")->property_value("primvars:skel:jointWeights")->array_size() == 6, "JSON skin arrays");
  std::string dense = triangle;
  for (size_t i = 0; i < 2000; ++i) dense += "f 1 2 3\n";
  options.max_memory = 2 * 1024 * 1024;
  ia::OBJ compact;
  Check(ia::BuildOBJScene(dense, "", options, &compact, &payload, &err), "compact geometry fits budget");
  Check(!ia::BuildOBJPayload(dense, "", options, &payload, &err) && err.find("budget") != std::string::npos, "JSON expansion fits budget only when allowed");
  options = ia::Options{};
  // Finite float coordinates can overflow float intermediate normal products.
  const auto huge = std::string("v 1e38 0 0\nv 0 1e38 0\nv 0 0 1e38\nf 1 2 3");
  auto layer = n::LoadOBJLayerFromMemory("memory:large-coordinates.obj", reinterpret_cast<const uint8_t*>(huge.data()), huge.size(), options, &warn, &err);
  Check(bool(layer), err.c_str());
  const auto normals = layer->prim_at_path("/World/Links/Geometry/mesh")->property_value("normals")->as_float_array();
  for (float value : normals) Check(std::isfinite(value) && value > .5f, "finite computed normals");
  options.max_input = SIZE_MAX; options.max_memory = 1024;
  // Reject an oversized length before touching the small supplied buffer.
  layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>("x"), size_t(1) << (sizeof(size_t) == 8 ? 33 : 30), options, &warn, &err);
  Check(!layer && err.find("limit") != std::string::npos, "preallocation size rejection");
  ia::Reader reader(options);
  Check(!reader.ChargeArray(SIZE_MAX, 8, &err), "checked array multiplication");
  reader.Warn(std::string(2 * 1024 * 1024, 'w'));
  Check(reader.warnings.size() <= 1024 * 1024, "bounded warning memory");
  for (bool input_limit : {false, true}) {
    ia::Options limits;
    limits.max_input = input_limit ? 4 : 16;
    limits.max_memory = input_limit ? 16 : 4;
    size_t reads = 0;
    limits.read = [&](const std::string& name, const std::string&,
                      std::vector<uint8_t>* data, std::string* resolved, std::string*) {
      ++reads;
      data->assign(8, 0);
      *resolved = name;
      return true;
    };
    ia::Reader limited(limits);
    std::vector<uint8_t> bytes;
    std::string resolved;
    Check(!limited.Read("oversized", "", &bytes, &resolved, &err) &&
          !limited.failure.empty(), "dependency limit is fatal");
    Check(!limited.Read("untouched", "", &bytes, &resolved, &err) && reads == 1,
          "dependency limit stops subsequent reads");
  }
  std::string groups = triangle;
  for (size_t i = 0; i < 2048; ++i) groups += "o repeat\nf 1 2 3\n";
  options = ia::Options{};
  layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(groups.data()), groups.size(), options, &warn, &err);
  Check(bool(layer) && layer->prim_at_path("/World/Links/Geometry/repeat_2047"), "many duplicate object names");
  const auto collision = triangle + "o part\nf 1 2 3\no part_1\nf 1 2 3\no part\nf 1 2 3\n";
  layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(collision.data()), collision.size(), options, &warn, &err);
  Check(bool(layer) && layer->prim_at_path("/World/Links/Geometry/part_2"), "explicit name collides with generated suffix");


#if defined(__unix__) || defined(__APPLE__)
  // A bounded memory input need not have a terminator or readable next byte.
  const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* buffer = static_cast<uint8_t*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(buffer != MAP_FAILED, "guard allocation");
  Check(!mprotect(buffer + page, page, PROT_NONE), "guard protection");
  const std::string bounded = triangle + "f 1 2 3";
  std::memcpy(buffer + page - bounded.size(), bounded.data(), bounded.size());
  options = ia::Options{};
  layer = n::LoadOBJLayerFromMemory("", buffer + page - bounded.size(), bounded.size(), options, &warn, &err);
  Check(bool(layer), "non-terminated guarded input");
  munmap(buffer, page * 2);
#endif
}
size_t Number(const char* text) {
  size_t n = 0;
  const auto end = text + std::strlen(text);
  auto r = std::from_chars(text, end, n);
  Check(r.ec == std::errc{} && r.ptr == end, "integer argument");
  return n;
}
std::string Generate(size_t triangles, size_t comment_mib, bool smoothing) {
  const size_t mib = 1024 * 1024;
  Check(triangles && triangles <= size_t(INT32_MAX) / 3, "triangle count fits USD indices");
  Check(comment_mib <= (SIZE_MAX - triangles * 320) / mib, "generated input size");
  std::string obj;
  obj.reserve(comment_mib * mib + triangles * 320);
  obj.resize(comment_mib * mib, ' ');
  for (size_t pos = 0; pos < obj.size(); pos += mib) {
    obj[pos] = '#'; obj[pos + mib - 1] = '\n';
  }
  obj += "o generated\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n";
  if (smoothing) obj += "s 1\n";
  for (size_t i = 0; i < triangles; ++i) {
    const auto x = std::to_string(i);
    obj += "v " + x + " 0 0 1 0 0\nv " + x + " 1 0 0 1 0\nv " + x + " 0 1 0 0 1\n";
    for (size_t k = 0; k < 3; ++k)
      obj += "vw " + std::to_string(i * 3 + k) + " 0 .25 1 .75\n";
  }
  for (size_t i = 0; i + 1 < triangles; ++i) {
    const auto base = i * 3 + 1;
    obj += "f " + std::to_string(base) + "/1/1 " + std::to_string(base + 1) + "/2/1 " + std::to_string(base + 2) + "/3/1\n";
  }
  obj += "f -3/1/1 -2/2/1 -1/3/1 # last face, no newline";
  return obj;
}
void Large(size_t triangles, size_t comment_mib, bool parser_only, bool smoothing) {
  const auto start = std::chrono::steady_clock::now();
  auto obj = Generate(triangles, comment_mib, smoothing);
  tobj_load_config cfg = tobj_default_config(); cfg.num_threads = 1;
  cfg.max_input_bytes = obj.size();
  tobj_scene_f scene{};
  Check(tobj_load_obj_from_memory_f(&scene, reinterpret_cast<const uint8_t*>(obj.data()), obj.size(), &cfg, nullptr) == TOBJ_OK, "large C11 parse");
  Check(scene.attrib.vertices.count == triangles * 9 && scene.num_shapes == 1, "large position/shape count");
  Check(scene.shapes[0].mesh.num_faces == triangles && scene.shapes[0].mesh.num_indices == triangles * 3, "large face/index count");
  Check(scene.shapes[0].mesh.indices[triangles * 3 - 1].vertex_index == int(triangles * 3 - 1), "large final relative index");
  Check(scene.attrib.num_skin_weights == triangles * 3, "large skin count");
  tobj_scene_free_f(&scene);
  if (!parser_only) {
    ia::Options options;
    options.max_input = obj.size();
    options.max_memory = obj.size() + triangles * 3 * 800 + 64 * 1024 * 1024;
    std::string warn, err;
    auto layer = n::LoadOBJLayerFromMemory("memory:generated.obj", reinterpret_cast<const uint8_t*>(obj.data()), obj.size(), options, &warn, &err);
    Check(bool(layer), err.c_str());
    const auto* mesh = layer->prim_at_path("/World/Links/Geometry/generated");
    Check(mesh && mesh->property_value("points")->array_size() == triangles * 3, "large USD point count");
    auto indices = mesh->property_value("faceVertexIndices")->as_int_array();
    Check(indices.size() == triangles * 3 && indices.back() == int(triangles * 3 - 1), "large USD indices");
    Check(mesh->property_value("faceVertexCounts")->array_size() == triangles, "large USD polygons");
    Check(mesh->property_value("primvars:st")->array_size() == triangles * 3, "large USD UVs");
    Check(mesh->property_value("primvars:displayColor")->array_size() == triangles * 3, "large USD colors");
    const auto normals = mesh->property_value("normals")->as_float_array();
    Check(normals.size() == triangles * 9 && normals[2] == 1 && normals.back() == 1, "large authored normals");
    auto weights = mesh->property_value("primvars:skel:jointWeights")->as_float_array();
    Check(weights.size() == triangles * 6 && weights.front() == .25f && weights.back() == .75f, "large USD skin weights");
    Check(warn.empty(), "large input has no warnings");
    layer.reset();
    options.max_memory = obj.size() + 1024;
    layer = n::LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(obj.data()), obj.size(), options, &warn, &err);
    Check(!layer && (err.find("budget") != std::string::npos || err.find("limit") != std::string::npos), "large input budget rejection");
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "Generated in memory: bytes=" << obj.size() << " triangles=" << triangles
            << " vertices=" << triangles * 3 << " seconds=" << elapsed << '\n';
}
// A high-arity star polygon makes nearly every corner reflex-blocked, the
// adversarial case for ear clipping. The tessellator's linear work budget must
// keep it bounded and still emit n - 2 triangles.
void HighArityTriangulation() {
  const size_t n = 65536;
  std::string obj;
  obj.reserve(n * 40);
  char buffer[96];
  for (size_t i = 0; i < n; ++i) {
    const double a = 6.283185307179586 * double(i) / double(n);
    const double r = (i & 1) ? 1.0 : 0.25;
    obj.append(buffer, size_t(std::snprintf(buffer, sizeof(buffer), "v %.9f %.9f 0\n",
                                            r * std::cos(a), r * std::sin(a))));
  }
  obj += "f";
  for (size_t i = 1; i <= n; ++i) {
    obj += ' ';
    obj += std::to_string(i);
  }
  obj += '\n';
  auto config = tobj_default_config();
  config.triangulate = true;
  config.max_face_arity = n;
  tobj_scene_f scene{};
  const auto start = std::chrono::steady_clock::now();
  Check(tobj_load_obj_from_memory_f(&scene, reinterpret_cast<const uint8_t*>(obj.data()),
                                    obj.size(), &config, nullptr) == TOBJ_OK,
        "high-arity polygon loads");
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  size_t triangles = 0;
  for (size_t s = 0; s < scene.num_shapes; ++s) triangles += scene.shapes[s].mesh.num_faces;
  tobj_scene_free_f(&scene);
  Check(triangles == n - 2, "high-arity polygon emits n - 2 triangles");
  // Unbudgeted clipping takes ~3 s at -O2 (far longer unoptimized); the
  // budgeted path takes ~0.1 s.
  Check(seconds < 5.0, "high-arity triangulation is bounded");
}
}  // namespace
int main(int argc, char** argv) {
  size_t triangles = 10000, comment_mib = 1;
  bool parser_only = false, smoothing = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--parser-only") parser_only = true;
    else if (arg == "--smooth") smoothing = true;
    else {
      Check(i + 1 < argc, "option requires value");
      const size_t value = Number(argv[++i]);
      if (arg == "--triangles") triangles = value;
      else if (arg == "--comment-mib") comment_mib = value;
      else Check(false, "unknown option");
    }
  }
  Hardening();
  HighArityTriangulation();
  GeneratedNormals();
  Large(triangles, comment_mib, parser_only, smoothing);
  std::cout << "OBJ memory, limits, malformed-input and allocation regressions passed\n";
}
