/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "lightusd-c.h"

namespace {
struct VariantSet {
  std::string name;
  std::string selection;
  std::vector<std::string> options;
};
struct VariantGroup {
  std::string path;
  std::vector<VariantSet> sets;
};

std::string copy(lightusd_sv value) {
  return value.data ? std::string(value.data, value.len) : std::string();
}

void collect(const lightusd_stage* stage, lightusd_prim prim,
             std::vector<VariantGroup>* groups) {
  VariantGroup group;
  group.path = copy(lightusd_prim_path(prim));
  // Query the stage view by path: root-prim traversal returns authored layer
  // handles, while variant metadata belongs to the composed prim view.
  const lightusd_prim composed = lightusd_stage_prim_at_path(stage, group.path.c_str());
  const size_t set_count = lightusd_prim_variant_set_count(composed);
  for (size_t i = 0; i < set_count; ++i) {
    VariantSet set;
    set.name = copy(lightusd_prim_variant_set_name(composed, i));
    set.selection = copy(lightusd_variant_selection(composed, set.name.c_str()));
    const size_t option_count = lightusd_variant_count(composed, set.name.c_str());
    for (size_t j = 0; j < option_count; ++j)
      set.options.push_back(copy(lightusd_variant_name(composed, set.name.c_str(), j)));
    group.sets.push_back(std::move(set));
  }
  if (!group.sets.empty()) groups->push_back(std::move(group));
  for (size_t i = 0, n = lightusd_prim_child_count(prim); i < n; ++i)
    collect(stage, lightusd_prim_child(prim, i), groups);
}

void json_string(const std::string& value) {
  std::cout << '"';
  for (unsigned char c : value) {
    switch (c) {
      case '"': std::cout << "\\\""; break;
      case '\\': std::cout << "\\\\"; break;
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      case '\t': std::cout << "\\t"; break;
      default:
        if (c < 0x20) std::cout << "?";
        else std::cout << static_cast<char>(c);
    }
  }
  std::cout << '"';
}
}

int main(int argc, char** argv) {
  bool json = false, summary = false, verbose = false;
  std::string filename;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: variant-lister [--json] [--summary] [--verbose] input.usda/usdc/usdz\n";
      return 0;
    } else if (arg == "--json") json = true;
    else if (arg == "-s" || arg == "--summary") summary = true;
    else if (arg == "-v" || arg == "--verbose") verbose = true;
    else filename = arg;
  }
  if (filename.empty()) {
    std::cerr << "Error: No input file specified\n";
    return 1;
  }

  lightusd_stage* stage = nullptr;
  lightusd_load_options load_options;
  lightusd_load_options_init(&load_options);
  // Variant inspection describes the authored root layer. Composition can
  // flatten away that layer-level authoring metadata, so keep this load raw.
  load_options.composed = 0;
  const lightusd_status status =
      lightusd_stage_load(filename.c_str(), &load_options, &stage);
  if (status != LIGHTUSD_OK) {
    std::cerr << "Error: Failed to load USD file: " << lightusd_last_error() << '\n';
    return 1;
  }
  std::vector<VariantGroup> groups;
  for (size_t i = 0, n = lightusd_stage_root_prim_count(stage); i < n; ++i)
    collect(stage, lightusd_stage_root_prim(stage, i), &groups);

  size_t sets = 0, options = 0;
  if (json) {
    std::cout << "{\n  \"file\": "; json_string(filename);
    std::cout << ",\n  \"variant_groups\": [";
    for (size_t i = 0; i < groups.size(); ++i) {
      const auto& group = groups[i];
      std::cout << (i ? "," : "") << "\n    {\n      \"prim_path\": "; json_string(group.path);
      std::cout << ",\n      \"variant_sets\": [";
      for (size_t j = 0; j < group.sets.size(); ++j) {
        const auto& set = group.sets[j];
        std::cout << (j ? "," : "") << "{\"name\":"; json_string(set.name);
        std::cout << ",\"selection\":"; json_string(set.selection);
        std::cout << ",\"options\":[";
        for (size_t k = 0; k < set.options.size(); ++k) {
          if (k) std::cout << ',';
          json_string(set.options[k]);
        }
        std::cout << "]}";
      }
      std::cout << "]\n    }";
    }
    std::cout << "\n  ]\n}\n";
  } else {
    std::cout << "=== Variants in " << filename << " ===\n\n";
    for (const auto& group : groups) {
      std::cout << "Prim: " << group.path << '\n';
      for (const auto& set : group.sets) {
        ++sets; options += set.options.size();
        std::cout << "  VariantSet: \"" << set.name << "\" (selected: "
                  << (set.selection.empty() ? "none" : set.selection) << ")\n";
        for (const auto& option : set.options) std::cout << "    \"" << option << "\"\n";
      }
      std::cout << '\n';
    }
    if (groups.empty()) std::cout << "No variants found in file: " << filename << '\n';
    if (summary || verbose) {
      if (sets == 0) for (const auto& g : groups) for (const auto& s : g.sets) {
        ++sets; options += s.options.size();
      }
      std::cout << "=== Summary ===\nTotal variant groups: " << groups.size()
                << "\nTotal variant sets: " << sets
                << "\nTotal variant options: " << options << "\n";
    }
  }
  lightusd_stage_destroy(stage);
  return 0;
}
