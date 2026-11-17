// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "external/tinyobjloader-c/tiny_obj_c.h"
#include "../../src/pcp/layer-registry.hh"
#include "reader/obj-reader.hh"
using namespace lightusd;
// Always-on check (assert is compiled out in Release builds).
#define REQUIRE(c)                                                   \
  do {                                                               \
    if (!(c)) {                                                      \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " #c "\n";       \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)
int main() {
  const char* obj =
      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvw 0 1 .25 2 -.5\nvw 1 1 2\nf 1 2 3\nl 1 "
      "2\np 3\n";
  tobj_scene_f f{};
  tobj_scene_d d{};
  assert(tobj_load_obj_from_memory_f(&f, reinterpret_cast<const uint8_t*>(obj),
                                     std::strlen(obj), nullptr,
                                     nullptr) == TOBJ_OK);
  assert(tobj_load_obj_from_memory_d(&d, reinterpret_cast<const uint8_t*>(obj),
                                     std::strlen(obj), nullptr,
                                     nullptr) == TOBJ_OK);
  assert(f.attrib.num_skin_weights == 2 && d.attrib.num_skin_weights == 2);
  assert(f.attrib.skin_weights[0].weights[1].weight == -.5f);
  assert(d.attrib.skin_weights[0].vertex_id == 0 &&
         d.attrib.skin_weights[0].weights[0].joint_id == 1);
  tobj_scene_free_f(&f);
  tobj_scene_free_d(&d);
  for (const char* bad :
       {"vw -1 0 1\n", "vw 3 0 1\n", "vw 0 0 nan\n", "vw 0 0 inf\n",
        "vw 0 0 1e999\n", "vw 0 1\n", "vw 0 1 1\nvw 0 0 1\n"}) {
    auto text = std::string("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n") + bad;
    assert(tobj_load_obj_from_memory_f(
               &f, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
               nullptr, nullptr) != TOBJ_OK);
    assert(!f.arena && !f.attrib.skin_weights);
    assert(tobj_load_obj_from_memory_d(
               &d, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
               nullptr, nullptr) != TOBJ_OK);
    assert(!d.arena && !d.attrib.skin_weights);
  }
  lightusd::tydra::import_asset::Options options;
  std::string warning, error;
  auto layer =
      LoadOBJLayerFromMemory("", reinterpret_cast<const uint8_t*>(obj),
                             std::strlen(obj), options, &warning, &error);
  assert(layer);
  auto* mesh = layer->prim_at_path("/World/Links/Geometry/mesh");
  assert(mesh &&
         mesh->property_meta("primvars:skel:jointWeights")->elementSize == 2);
  assert(mesh->meta().apiSchemasAuthored());
  assert(layer->prim_at_path("/World/Links/Geometry/mesh_curves"));
  assert(layer->prim_at_path("/World/Links/Geometry/mesh_points"));
  assert(!warning.empty());
  // Resolver-backed composition loader uses the source identifier extension.
  pcp::LayerLoadOptions load;
  auto memory = pcp::LoadLayerFromMemory(
      "memory:model.obj", reinterpret_cast<const uint8_t*>(obj),
      std::strlen(obj), &warning, &error, load);
  assert(memory && memory->prim_at_path("/World/Links/Geometry/mesh"));
  {
    // MaterialX-in-MTL: info:id is the stdlib nodedef selected by the node's
    // output and input types, not ND_<category>_<output type>.
    const std::string mtl =
        "newmtl m\nmtlx_surface sh open_pbr_surface\n"
        "mtlx_node tex image vector3\nmtlx_node nm normalmap vector3\n"
        "mtlx_node mul multiply color3\nmtlx_node c constant color3\n"
        "mtlx_node mix mix color3\nmtlx_node g foo color3\n"
        "mtlx_input tex.file filename n.png\nmtlx_input nm.in vector3 @tex\n"
        "mtlx_input c.value color3 1 0 0\nmtlx_input mul.in1 color3 @c\n"
        "mtlx_input mul.in2 float 0.5\nmtlx_input mix.fg color3 @mul\n"
        "mtlx_input mix.mix float 0.5\nmtlx_input sh.base_color color3 @mix\n"
        "mtlx_input sh.geometry_normal vector3 @nm\n"
        "mtlx_input sh.specular_color color3 @g\n";
    const std::string src = "mtllib a.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "usemtl m\nf 1 2 3\n";
    lightusd::tydra::import_asset::Options mopt;
    mopt.open = [&](const std::string& name, const std::string&,
                    lightusd::tydra::import_asset::AssetSource* source,
                    std::string*) {
      if (name != "a.mtl") return false;
      source->identifier = "memory:a.mtl";
      source->read = [&](std::vector<uint8_t>* out, std::string*) {
        out->assign(mtl.begin(), mtl.end());
        return true;
      };
      return true;
    };
    auto ml = LoadOBJLayerFromMemory(
        "memory:a.obj", reinterpret_cast<const uint8_t*>(src.data()),
        src.size(), mopt, &warning, &error);
    REQUIRE(ml);
    auto id = [&](const char* path) {
      auto* p = ml->prim_at_path(path);
      const Value* v = p ? p->property_value("info:id") : nullptr;
      return v && v->as_token() ? std::string(*v->as_token()) : std::string();
    };
    const std::string g = "/World/Materials/m/NodeGraphs/";
    REQUIRE(id((g + "tex").c_str()) == "ND_image_vector3");
    REQUIRE(id((g + "nm").c_str()) == "ND_normalmap_float");
    REQUIRE(id((g + "mul").c_str()) == "ND_multiply_color3FA");
    REQUIRE(id((g + "c").c_str()) == "ND_constant_color3");
    REQUIRE(id((g + "mix").c_str()) == "ND_mix_color3");
    REQUIRE(id((g + "g").c_str()) == "ND_foo_color3");  // not in stdlib
    REQUIRE(id("/World/Materials/m/MtlxSurface") ==
           "ND_open_pbr_surface_surfaceshader");
  }
  std::cout << "OBJ reader, C11 skin weights and mixed primitives passed\n";
}
