// SPDX-License-Identifier: Apache-2.0
// Generated MTL inputs exercise both precision families without asset files.
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "external/tinyobjloader-c/tiny_obj_c.h"

namespace {
void Check(bool ok, const char* message) {
  if (!ok) { std::cerr << "MTL regression failed: " << message << '\n'; std::exit(1); }
}
struct Source {
  std::string text;
  size_t calls = 0;
  static tobj_result Resolve(void* ud, const char*, tobj_mtl_source* out) {
    auto& source = *static_cast<Source*>(ud);
    ++source.calls;
    *out = {};
    out->data = reinterpret_cast<const uint8_t*>(source.text.data());
    out->len = source.text.size();
    return TOBJ_OK;
  }
};
void Reject(const char* record) {
  Source source{"newmtl test\n" + std::string(record) + '\n'};
  const auto* data = reinterpret_cast<const uint8_t*>(source.text.data());
  tobj_material_list_f f{};
  tobj_material_list_d d{};
  Check(tobj_parse_mtl_from_memory_f(&f, data, source.text.size(), nullptr, nullptr) == TOBJ_ERR_PARSE, record);
  Check(!f.strpool && !f.items && !f.count, "failed float MTL output cleared");
  Check(tobj_parse_mtl_from_memory_d(&d, data, source.text.size(), nullptr, nullptr) == TOBJ_ERR_PARSE, record);
  Check(!d.strpool && !d.items && !d.count, "failed double MTL output cleared");
  const std::string obj = "mtllib test.mtl untouched.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl test\nf 1 2 3\n";
  data = reinterpret_cast<const uint8_t*>(obj.data());
  auto config = tobj_default_config();
  config.num_threads = 1;
  config.mtl_resolver = Source::Resolve;
  config.mtl_resolver_user_data = &source;
  tobj_scene_f sf{};
  tobj_scene_d sd{};
  const auto stopped = [&] {
    Check(source.calls == 1, "malformed MTL stops subsequent dependency reads");
    source.calls = 0;
  };
  Check(tobj_load_obj_from_memory_f(&sf, data, obj.size(), &config, nullptr) == TOBJ_ERR_PARSE, "OBJ rejects malformed float MTL");
  stopped();
  Check(!sf.arena && !sf.materials, "failed float scene cleared");
  Check(tobj_load_obj_from_memory_d(&sd, data, obj.size(), &config, nullptr) == TOBJ_ERR_PARSE, "OBJ rejects malformed double MTL");
  stopped();
  Check(!sd.arena && !sd.materials, "failed double scene cleared");
  tobj_callbacks_f cf{};
  tobj_callbacks_d cd{};
  Check(tobj_load_obj_with_callbacks_f(data, obj.size(), &cf, &config, nullptr) == TOBJ_ERR_PARSE, "callback rejects malformed float MTL");
  stopped();
  Check(tobj_load_obj_with_callbacks_d(data, obj.size(), &cd, &config, nullptr) == TOBJ_ERR_PARSE, "callback rejects malformed double MTL");
  stopped();
}
void ResolverFailures() {
  struct Failure {
    tobj_result status;
    size_t calls = 0;
    static tobj_result Resolve(void* ud, const char*, tobj_mtl_source* out) {
      auto& f = *static_cast<Failure*>(ud);
      ++f.calls;
      *out = {};
      return f.status;
    }
  };
  const std::string text = "mtllib first.mtl second.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  const auto* data = reinterpret_cast<const uint8_t*>(text.data());
  for (const auto status : {TOBJ_ERR_NOT_FOUND, TOBJ_ERR_ALLOC, TOBJ_ERR_LIMIT_EXCEEDED, TOBJ_ERR_IO}) {
    Failure failure{status};
    auto config = tobj_default_config();
    config.num_threads = 1;
    config.mtl_resolver = Failure::Resolve;
    config.mtl_resolver_user_data = &failure;
    const auto expected = status == TOBJ_ERR_NOT_FOUND ? TOBJ_OK : status;
    const auto check = [&](tobj_result result) {
      Check(result == expected, "MTL resolver status propagated");
      Check(failure.calls == (status == TOBJ_ERR_NOT_FOUND ? 2u : 1u), "fatal resolver status stops dependency reads");
      failure.calls = 0;
    };
    tobj_scene_f f{};
    tobj_scene_d d{};
    check(tobj_load_obj_from_memory_f(&f, data, text.size(), &config, nullptr));
    tobj_scene_free_f(&f);
    check(tobj_load_obj_from_memory_d(&d, data, text.size(), &config, nullptr));
    tobj_scene_free_d(&d);
    tobj_callbacks_f cf{};
    tobj_callbacks_d cd{};
    check(tobj_load_obj_with_callbacks_f(data, text.size(), &cf, &config, nullptr));
    check(tobj_load_obj_with_callbacks_d(data, text.size(), &cd, &config, nullptr));
  }
}
void Numeric() {
  for (const char* record : {"Kd nan 0 0", "Ks 1e999 0 0", "Ke .1junk 0 0", "Ka 0 0 -infinity",
      "Ni .", "Ni 1e", "Pr", "Pm .5 extra", "illum 2147483648", "illum 1junk",
      "map_Kd -s nan texture.png", "map_Kd -s 1e999 texture.png", "map_Kd -s 1 1e999 texture.png",
      "map_Kd -o 0 -inf texture.png", "map_Kd -mm 1 nan texture.png", "map_Kd -bm 2junk texture.png",
      "map_Kd -texres 2147483648 texture.png", "map_Kd -texres 1junk texture.png",
      "map_Kd -clamp maybe texture.png", "map_Kd -s", "map_Kd -s 1"}) Reject(record);
  const std::string valid = "newmtl valid\nKd .25 # retain omitted-component defaults\nNs 100\n"
      "map_Kd -s 2 3 -o .1 .2 -clamp on 123.png\nmap_Ks -s 1 2 123.png\nillum 2\n";
  const auto* data = reinterpret_cast<const uint8_t*>(valid.data());
  tobj_material_list_f f{};
  tobj_material_list_d d{};
  Check(tobj_parse_mtl_from_memory_f(&f, data, valid.size(), nullptr, nullptr) == TOBJ_OK, "valid float MTL");
  Check(tobj_parse_mtl_from_memory_d(&d, data, valid.size(), nullptr, nullptr) == TOBJ_OK, "valid double MTL");
  auto verify = [](const auto& list) {
    const auto& m = list.items[0];
    Check(list.count == 1 && m.diffuse[0] == .25 && m.diffuse[1] == 0, "partial color compatibility");
    Check(m.diffuse_texopt.scale[0] == 2 && m.diffuse_texopt.scale[1] == 3 && m.diffuse_texopt.scale[2] == 1, "optional texture components");
    Check(m.diffuse_texopt.clamp && m.illum == 2, "boolean and integer fields");
    Check(std::strcmp(m.diffuse_texname, "123.png") == 0 && std::strcmp(m.specular_texname, "123.png") == 0, "numeric texture filename preserved");
  };
  verify(f); verify(d);
  tobj_material_list_free_f(&f); tobj_material_list_free_d(&d);
  const std::string wide = "newmtl wide\nKd 1e100 0 0\n";
  data = reinterpret_cast<const uint8_t*>(wide.data());
  Check(tobj_parse_mtl_from_memory_f(&f, data, wide.size(), nullptr, nullptr) == TOBJ_ERR_PARSE, "float material overflow rejected");
  Check(tobj_parse_mtl_from_memory_d(&d, data, wide.size(), nullptr, nullptr) == TOBJ_OK, "double material range retained");
  Check(d.items[0].diffuse[0] > 1e99 && d.items[0].diffuse[0] < 1e101, "double material value");
  tobj_material_list_free_d(&d);
}
void Unknowns() {
  const size_t materials = 2000, fields = 30000;
  std::string text;
  for (size_t i = 0; i < materials; ++i) {
    text += "newmtl material_" + std::to_string(i) + "\ncustom_z last\ncustom_a first\n";
  }
  text += "newmtl reverse\n";
  for (size_t i = fields; i > 0; --i) {
    const auto key = std::to_string(i);
    text += "key_" + std::string(6 - key.size(), '0') + key + " value_" + key + '\n';
  }
  text += "duplicate first\nduplicate second\nduplicate third\n";
  const auto* data = reinterpret_cast<const uint8_t*>(text.data());
  const auto verify = [&](const auto& list) {
    Check(list.count == materials + 1, "generated material count");
    for (size_t i = 0; i < materials; ++i) {
      const auto& m = list.items[i];
      Check(m.num_unknown_params == 2 && std::strcmp(m.unknown_params[0].key, "custom_a") == 0 &&
            std::strcmp(m.unknown_params[1].key, "custom_z") == 0, "custom fields grouped by material");
    }
    const auto& m = list.items[materials];
    Check(m.num_unknown_params == fields + 3, "all custom fields retained");
    for (size_t i = 1; i < m.num_unknown_params; ++i)
      Check(std::strcmp(m.unknown_params[i - 1].key, m.unknown_params[i].key) <= 0, "custom keys sorted");
    for (size_t i = 0; i < 3; ++i)
      Check(std::strcmp(m.unknown_params[i].value, i == 0 ? "first" : i == 1 ? "second" : "third") == 0, "duplicate-key order retained");
  };
  tobj_material_list_f f{};
  Check(tobj_parse_mtl_from_memory_f(&f, data, text.size(), nullptr, nullptr) == TOBJ_OK, "generated float custom fields");
  verify(f);
  Check(std::strcmp(tobj_material_get_param_f(&f.items[materials], "key_000001"), "value_1") == 0, "float custom lookup");
  tobj_material_list_free_f(&f);
  tobj_material_list_d d{};
  Check(tobj_parse_mtl_from_memory_d(&d, data, text.size(), nullptr, nullptr) == TOBJ_OK, "generated double custom fields");
  verify(d);
  Check(std::strcmp(tobj_material_get_param_d(&d.items[materials], "key_030000"), "value_30000") == 0, "double custom lookup");
  tobj_material_list_free_d(&d);
}
void Lookup(size_t count) {
  const size_t width = std::to_string(count - 1).size();
  const auto name_for = [width](size_t key) {
    const auto number = std::to_string(key);
    return "material_" + std::string(width - number.size(), '0') + number;
  };
  struct Library {
    std::string text;
    size_t loaded = 0, missing = 0;
    static tobj_result Resolve(void* ud, const char* name, tobj_mtl_source* out) {
      auto& library = *static_cast<Library*>(ud);
      *out = {};
      if (std::strcmp(name, "library.mtl") != 0) {
        ++library.missing;
        return TOBJ_ERR_NOT_FOUND;
      }
      ++library.loaded;
      out->data = reinterpret_cast<const uint8_t*>(library.text.data());
      out->len = library.text.size();
      return TOBJ_OK;
    }
  };
  struct Visits {
    const std::vector<int>* ids;
    size_t offset = 0;
    static void Use(void* ud, const char*, int id) {
      auto& visits = *static_cast<Visits*>(ud);
      Check(visits.offset < visits.ids->size() && id == (*visits.ids)[visits.offset++], "callback material lookup and duplicate precedence");
    }
  };
  for (int order = 0; order < 3; ++order) {
    Library library{"newmtl shared\nKd 1 0 0\n"};
    std::vector<int> ids(count);
    std::string obj = "mtllib library.mtl library.mtl\nmtllib library.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n";
    for (size_t i = 0; i < count; ++i) {
      const size_t key = order == 0 ? i : order == 1 ? count - i - 1
                                                   : (i % 2 ? count - 1 - i / 2 : i / 2);
      library.text += "newmtl " + name_for(key) + "\nKd .2 .3 .4\n";
      ids[key] = static_cast<int>(i + 1);
      const auto name = "missing_" + name_for(key) + ".mtl";
      obj += "mtllib " + name + " " + name + '\n';
    }
    library.text += "newmtl shared\nKd 0 1 0\nnewmtl " + name_for(0) + "\nKd 0 0 1\nnewmtl material_\nKd .5 .5 .5\n";
    for (size_t key = 0; key < count; ++key)
      obj += "usemtl " + name_for(key) + "\nf 1 2 3\n";
    obj += "usemtl shared\nf 1 2 3\nusemtl material_\nf 1 2 3\nusemtl absent\nf 1 2 3\n";
    ids.push_back(0);
    ids.push_back(static_cast<int>(count + 3));
    ids.push_back(-1);
    auto config = tobj_default_config();
    config.num_threads = 1;
    config.mtl_resolver = Library::Resolve;
    config.mtl_resolver_user_data = &library;
    const auto check_reads = [&] {
      Check(library.loaded == 1 && library.missing == count, "each distinct library resolved once per load");
      library.loaded = library.missing = 0;
    };
    const auto verify = [&](const auto& scene) {
      Check(scene.num_materials == count + 4 && std::strcmp(scene.materials[0].name, "shared") == 0, "material definition order retained");
      Check(scene.num_shapes == 1 && scene.shapes[0].mesh.num_faces == ids.size(), "generated material face count");
      for (size_t i = 0; i < ids.size(); ++i)
        Check(scene.shapes[0].mesh.material_ids[i] == ids[i], "material lookup and duplicate precedence");
      check_reads();
    };
    const auto* data = reinterpret_cast<const uint8_t*>(obj.data());
    tobj_scene_f f{};
    Check(tobj_load_obj_from_memory_f(&f, data, obj.size(), &config, nullptr) == TOBJ_OK, "generated float material lookup");
    verify(f);
    tobj_scene_free_f(&f);
    tobj_scene_d d{};
    Check(tobj_load_obj_from_memory_d(&d, data, obj.size(), &config, nullptr) == TOBJ_OK, "generated double material lookup");
    verify(d);
    tobj_scene_free_d(&d);
    Visits visits{&ids};
    tobj_callbacks_f cf{};
    cf.user_data = &visits;
    cf.usemtl_cb = Visits::Use;
    Check(tobj_load_obj_with_callbacks_f(data, obj.size(), &cf, &config, nullptr) == TOBJ_OK, "float callback material lookup");
    Check(visits.offset == ids.size(), "all float material visits");
    check_reads();
    visits.offset = 0;
    tobj_callbacks_d cd{};
    cd.user_data = &visits;
    cd.usemtl_cb = Visits::Use;
    Check(tobj_load_obj_with_callbacks_d(data, obj.size(), &cd, &config, nullptr) == TOBJ_OK, "double callback material lookup");
    Check(visits.offset == ids.size(), "all double material visits");
    check_reads();
  }
}
struct Allocations {
  size_t calls = 0, fail_at = 0, used = 0;
  static void* Alloc(void* ud, size_t size, size_t) {
    auto& a = *static_cast<Allocations*>(ud);
    if (++a.calls == a.fail_at) return nullptr;
    void* p = std::malloc(size);
    if (p) a.used += size;
    return p;
  }
  static void Free(void* ud, void* p, size_t size) {
    auto& a = *static_cast<Allocations*>(ud);
    Check(a.used >= size, "MTL allocation accounting");
    a.used -= size;
    std::free(p);
  }
};
void LibraryAllocationFailures() {
  Source source{"newmtl shared\nKd .2 .3 .4\ncustom_z last\ncustom_a first\n"};
  std::string obj;
  for (size_t i = 0; i < 32; ++i) {
    const auto name = "library_" + std::to_string(i) + ".mtl";
    obj += "mtllib " + name + " " + name + '\n';
  }
  obj += "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl shared\nf 1 2 3\n";
  const auto* data = reinterpret_cast<const uint8_t*>(obj.data());
  for (int mode = 0; mode < 4; ++mode) {
    bool finished = false;
    for (size_t fail = 1; fail <= 256; ++fail) {
      Allocations a;
      a.fail_at = fail;
      auto config = tobj_default_config();
      config.num_threads = 1;
      config.allocator = {};
      config.allocator.alloc = Allocations::Alloc;
      config.allocator.free = Allocations::Free;
      config.allocator.user_data = &a;
      config.mtl_resolver = Source::Resolve;
      config.mtl_resolver_user_data = &source;
      if (mode == 0) {
        tobj_scene_f scene{};
        const auto result = tobj_load_obj_from_memory_f(&scene, data, obj.size(), &config, nullptr);
        if (result == TOBJ_OK) Check(scene.num_materials == 32 && scene.shapes[0].mesh.material_ids[0] == 0, "float scene survives allocation sweep");
        else Check(!scene.arena && !scene.materials && !scene.shapes, "failed float scene cleared");
        tobj_scene_free_f(&scene);
      } else if (mode == 1) {
        tobj_scene_d scene{};
        const auto result = tobj_load_obj_from_memory_d(&scene, data, obj.size(), &config, nullptr);
        if (result == TOBJ_OK) Check(scene.num_materials == 32 && scene.shapes[0].mesh.material_ids[0] == 0, "double scene survives allocation sweep");
        else Check(!scene.arena && !scene.materials && !scene.shapes, "failed double scene cleared");
        tobj_scene_free_d(&scene);
      } else if (mode == 2) {
        tobj_callbacks_f cb{};
        tobj_load_obj_with_callbacks_f(data, obj.size(), &cb, &config, nullptr);
      } else {
        tobj_callbacks_d cb{};
        tobj_load_obj_with_callbacks_d(data, obj.size(), &cb, &config, nullptr);
      }
      Check(!a.used, "library/name index allocation failures release all storage");
      if (a.calls < fail) { finished = true; break; }
    }
    Check(finished, "library allocation sweep terminates");
  }
}
template <class List, class Parse, class Free>
void AllocationFailures(const std::string& text, Parse parse, Free free_list) {
  for (size_t fail = 1; fail <= 256; ++fail) {
    Allocations a;
    a.fail_at = fail;
    auto config = tobj_default_config();
    config.allocator = {};
    config.allocator.alloc = Allocations::Alloc;
    config.allocator.free = Allocations::Free;
    config.allocator.user_data = &a;
    List list{};
    const auto result = parse(&list, reinterpret_cast<const uint8_t*>(text.data()), text.size(), &config, nullptr);
    if (result == TOBJ_OK) {
      Check(list.count == 96, "complete MTL after allocation sweep");
      for (size_t i = 0; i < list.count; ++i)
        Check(list.items[i].name && list.items[i].num_unknown_params == 2, "MTL data retained after allocation sweep");
      free_list(&list);
    } else Check(!list.items && !list.strpool && !list.count, "MTL allocation failure output cleared");
    Check(!a.used, "MTL allocations released");
    if (a.calls < fail) return;
  }
  Check(false, "MTL allocation sweep terminates");
}
}  // namespace
int main(int argc, char** argv) {
  size_t materials = 4096;
  if (argc != 1) {
    Check(argc == 3 && std::strcmp(argv[1], "--lookup-materials") == 0, "expected --lookup-materials COUNT");
    const char* end = argv[2] + std::strlen(argv[2]);
    const auto result = std::from_chars(argv[2], end, materials);
    Check(result.ec == std::errc{} && result.ptr == end && materials && materials < size_t(INT32_MAX) - 3, "material count fits ids");
  }
  Numeric();
  ResolverFailures();
  Unknowns();
  Lookup(materials);
  LibraryAllocationFailures();
  std::string text;
  for (size_t i = 0; i < 96; ++i)
    text += "newmtl material_" + std::to_string(i) + "\nKd .2 .3 .4\ncustom_z last\ncustom_a first\n";
  AllocationFailures<tobj_material_list_f>(text, tobj_parse_mtl_from_memory_f, tobj_material_list_free_f);
  AllocationFailures<tobj_material_list_d>(text, tobj_parse_mtl_from_memory_d, tobj_material_list_free_d);
  std::cout << "MTL numeric, lookup, repeated-library and custom-property regressions passed; lookup materials=" << materials << '\n';
}
