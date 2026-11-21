// SPDX-License-Identifier: Apache-2.0
#include "next/prim/name-order.hh"
#include "next/layer/property-index.hh"
#include "next/layer/layer.hh"
#include "next/reader/usda-reader.hh"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace lightusd::next;

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--compare") {
    std::string line;
    while (std::getline(std::cin, line)) {
      const size_t separator = line.find('\t');
      if (separator == std::string::npos) return 2;
      const std::string_view a(line.data(), separator);
      const std::string_view b(line.data() + separator + 1, line.size() - separator - 1);
      std::cout << (a == b ? 0 : DictionaryLess(a, b) ? -1 : 1) << '\n';
    }
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "--properties") {
    LoadResult loaded = LoadUSDAFromFile(argv[2]);
    if (!loaded.success) return 1;
    UsdPrim prim = loaded.stage.GetPrimAtPath(Path(argv[3]));
    if (!prim.IsValid()) return 1;
    for (const auto& name : prim.GetPropertyNames()) {
      assert(prim.HasProperty(name));
      assert(prim.HasProperty(GetPropNameTable().intern(name)));
      assert(prim.HasAuthoredProperty(name));
      std::cout << name << '\n';
    }
    return 0;
  }
  if (argc != 1) return 2;
  assert(DictionaryLess("a0_a", "a00"));
  assert(DictionaryLess("foo2", "foo10"));
  assert(DictionaryLess("foo01a", "foo1b"));
  assert(DictionaryLess("Foo1", "foo1"));
  assert(DictionaryLess("foo1", "foo01"));
  assert(DictionaryLess("foo0", "foo_"));
  assert(DictionaryLess("foo_", "fooa"));
  assert(DictionaryLess("x" + std::string(400, '9'), "x1" + std::string(400, '0')));
  const std::vector<std::string> names{
      "", "_", "A", "a", "a0", "a00", "a01", "a1", "A1", "a1a",
      "a01b", "a2", "a10", "a_", "ns:A2", "ns:a02", "é", "中"};
  for (const auto& a : names) {
    assert(!DictionaryLess(a, a));
    for (const auto& b : names) {
      assert(!(DictionaryLess(a, b) && DictionaryLess(b, a)));
      assert((!DictionaryLess(a, b) && !DictionaryLess(b, a)) == (a == b));
      for (const auto& c : names)
        if (DictionaryLess(a, b) && DictionaryLess(b, c)) assert(DictionaryLess(a, c));
    }
  }
  const char source[] = R"USD(#usda 1.0
def "P" {
 int a10
 rel _a
 int a2
 int a1
 reorder properties = ["a10", "a2"]
}
)USD";
  LoadResult stage = LoadUSDAFromString(source, sizeof(source) - 1);
  assert(stage.success);
  UsdPrim prim = stage.stage.GetPrimAtPath(Path("/P"));
  assert((prim.GetPropertyNames() == std::vector<std::string>{"a10", "a2", "_a", "a1"}));
  assert(prim.HasProperty("_a") && prim.HasAuthoredProperty("_a"));
  assert((stage.stage.GetRootLayer()->prim_at_path("/P")->authored_property_names() ==
          std::vector<std::string>{"a10", "_a", "a2", "a1"}));
  std::cout << "name-order tests passed\n";
}
