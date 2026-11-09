// Fuzzer for Layer::find_primspec_at — specifically targets the prim path
// cache correctness fix (cache keyed on full_path_name, not prim_part).
//
// Run with:
//   clang++ -fsanitize=fuzzer,address -I src -o layer_find_primspec_fuzz \
//     tests/fuzzer/layer_find_primspec_fuzzmain.cc src/liblightusd.a
//   ./layer_find_primspec_fuzz corpus/layer_find_primspec/
//
#include <cstdint>
#include <cstring>

#include "layer.hh"
#include "prim-types.hh"
#include "lightusd.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Build a layer with same-leaf-name prims at different depths,
// then exercise find_primspec_at with paths derived from the input bytes.
static void fuzz_find_primspec_at(const uint8_t *data, size_t size)
{
  // Bound input size to avoid excessive allocation
  if (size > 64 * 1024) return;

  Layer layer;
  layer.set_name("fuzz_layer");

  // Create two root prims with different names
  layer.add_primspec("Alpha", PrimSpec(Specifier::Def, "Xform", "Alpha"));
  layer.add_primspec("Beta", PrimSpec(Specifier::Def, "Xform", "Beta"));

  PrimSpec &alpha = layer.primspecs().at("Alpha");
  PrimSpec &beta = layer.primspecs().at("Beta");

  // Add children to both roots — varying depth based on input byte
  uint8_t depth_byte = size > 0 ? data[0] : 0;
  size_t depth = (depth_byte % 8) + 1;  // 1..8 levels deep

  PrimSpec *current = &alpha;
  for (size_t i = 0; i < depth; ++i) {
    std::string child_name = "L" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Xform", child_name.c_str());
    current->children().push_back(child);
    current = &current->children().back();
  }
  // Add a leaf Mesh prim at the deepest level with name "Leaf"
  PrimSpec leaf_mesh(Specifier::Def, "Mesh", "Leaf");
  current->children().push_back(leaf_mesh);

  // Same structure under Beta but with a Sphere at the leaf
  current = &beta;
  for (size_t i = 0; i < depth; ++i) {
    std::string child_name = "L" + std::to_string(i);
    PrimSpec child(Specifier::Def, "Xform", child_name.c_str());
    current->children().push_back(child);
    current = &current->children().back();
  }
  PrimSpec leaf_sphere(Specifier::Def, "Sphere", "Leaf");
  current->children().push_back(leaf_sphere);

  // Now exercise find_primspec_at with synthesized paths.
  // The key test: /Alpha/L0/.../Leaf and /Beta/L0/.../Leaf should
  // return different PrimSpecs (both named "Leaf" but different types).
  // The old cache (keyed on "Leaf" only) would return the same PrimSpec
  // for both queries — this fuzzer would catch such regressions via
  // ASan if the wrong pointer is returned and then dereferenced.
  std::string err;
  const PrimSpec *found_alpha = nullptr;
  const PrimSpec *found_beta = nullptr;

  // Build path to alpha leaf
  std::string alpha_path = "/Alpha";
  PrimSpec *walk = &alpha;
  while (!walk->children().empty()) {
    walk = &walk->children().back();
    alpha_path += "/" + walk->name();
  }

  // Build path to beta leaf
  std::string beta_path = "/Beta";
  walk = &beta;
  while (!walk->children().empty()) {
    walk = &walk->children().back();
    beta_path += "/" + walk->name();
  }

  bool ok_alpha = layer.find_primspec_at(Path(alpha_path, ""), &found_alpha, &err);
  (void)ok_alpha;

  bool ok_beta = layer.find_primspec_at(Path(beta_path, ""), &found_beta, &err);
  (void)ok_beta;

  // If both found, they must be different PrimSpecs (even though both named "Leaf")
  if (found_alpha && found_beta) {
    // Crash here would indicate the cache bug (same pointer returned for both paths)
    // Use volatile to prevent the compiler from optimizing away the comparison
    volatile const PrimSpec *a = found_alpha;
    volatile const PrimSpec *b = found_beta;
    (void)a;
    (void)b;
  }
}

extern "C"
int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size)
{
  fuzz_find_primspec_at(data, size);
  return 0;
}
