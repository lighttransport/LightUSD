// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstring>
#include <iostream>

#include "external/tinyobjloader-c/tiny_obj_c.h"
#include "next/pcp/layer-registry.hh"
#include "next/reader/obj-reader.hh"
using namespace lightusd::next;
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
  std::cout << "OBJ reader, C11 skin weights and mixed primitives passed\n";
}
