// SPDX-License-Identifier: Apache-2.0
// Foreign-layer texture cache: authored aliases keep their own identifiers in
// USD output but retain one payload keyed by the canonical identity.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "tydra/asset-import/import.hh"

using namespace lightusd::tydra::import_asset;

int main() {
  int opens = 0;
  int reads = 0;
  Options options;
  options.open = [&](const std::string& name, const std::string&,
                     AssetSource* source, std::string*) {
    ++opens;
    source->identifier = name;      // what USD output and diagnostics see
    source->canonical = "tex.png";  // one retained payload for every alias
    source->read = [&](std::vector<uint8_t>* bytes, std::string*) {
      ++reads;
      bytes->assign({'P', 'N', 'G'});
      return true;
    };
    return true;
  };

  Reader reader(options);
  AssetBytes first, second, repeat;
  std::string first_id, second_id, repeat_id, error;

  assert(reader.ReadTexture("a.png", "mesh.mtl", &first, &first_id, &error));
  assert(reader.ReadTexture("b.png", "mesh.mtl", &second, &second_id, &error));
  assert(reader.ReadTexture("a.png", "mesh.mtl", &repeat, &repeat_id, &error));

  assert(first && second && repeat);
  assert(first_id == "a.png" && second_id == "b.png" &&
         repeat_id == "a.png");
  assert(first.get() == second.get() && first.get() == repeat.get());
  assert(!first->empty() && (*first)[0] == 'P');
  assert(opens == 2);  // one per distinct authored path
  assert(reads == 1);  // the canonical payload is read once

  std::cout << "ok - texture aliases share one retained payload\n";
  return 0;
}
