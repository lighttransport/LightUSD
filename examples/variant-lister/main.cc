#include <iostream>
#include <iomanip>
#include <map>
#include <string>

#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/variant-converter.hh"
#include "tydra/variant-support.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

void print_help() {
  std::cout << "Usage: variant-lister [options] input.usda/usdc/usdz\n\n";
  std::cout << "List all variant sets and options in a USD file.\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help              Show this help message\n";
  std::cout << "  -v, --verbose           Show detailed variant information\n";
  std::cout << "  -s, --summary           Show summary statistics\n";
  std::cout << "  --json                  Output in JSON format\n";
}

void print_variant_tree(const VariantGroup& group, int indent = 0) {
  std::string prefix(indent, ' ');
  std::cout << prefix << "├─ Prim: " << group.prim_path << "\n";

  for (size_t i = 0; i < group.variant_sets.size(); ++i) {
    const auto& vs = group.variant_sets[i];
    bool is_last_vs = (i == group.variant_sets.size() - 1);
    std::string vs_prefix = is_last_vs ? "└─" : "├─";
    std::string vs_indent = is_last_vs ? "   " : "│  ";

    std::cout << prefix << vs_prefix << " VariantSet: \"" << vs.name << "\"\n";
    std::cout << prefix << vs_indent << "├─ Default: "
              << (vs.default_option_index >= 0 &&
                  vs.default_option_index < static_cast<int32_t>(vs.options.size()) ?
                  vs.options[vs.default_option_index].name : "none") << "\n";

    for (size_t j = 0; j < vs.options.size(); ++j) {
      const auto& opt = vs.options[j];
      bool is_last = (j == vs.options.size() - 1);
      std::string opt_prefix = is_last ? "└─" : "├─";
      std::string opt_indent = is_last ? "   " : "│  ";

      std::cout << prefix << vs_indent << opt_prefix << " \"" << opt.name << "\"";

      if (!opt.nested_variant_sets.empty()) {
        std::cout << " [+nested: " << opt.nested_variant_sets.size() << "]";
      }

      if (!opt.description.empty()) {
        std::cout << " - " << opt.description;
      }
      std::cout << "\n";

      // Show nested variants (max 3 for readability)
      size_t nested_count = std::min(opt.nested_variant_sets.size(), size_t(3));
      for (size_t k = 0; k < nested_count; ++k) {
        const auto nested_vs_ptr = opt.nested_variant_sets[k];
        bool is_last_nested = (k == nested_count - 1);
        std::string nested_prefix = is_last_nested ? "└─" : "├─";
        std::string nested_indent = is_last_nested ? "   " : "│  ";

        std::cout << prefix << vs_indent << opt_indent << nested_prefix
                  << " VariantSet: \"" << nested_vs_ptr->name << "\" ("
                  << nested_vs_ptr->options.size() << " options)\n";
      }

      if (opt.nested_variant_sets.size() > 3) {
        std::cout << prefix << vs_indent << opt_indent << "└─ ... and "
                  << (opt.nested_variant_sets.size() - 3) << " more nested variant sets\n";
      }
    }
  }
}

void print_summary(const std::vector<VariantGroup>& groups) {
  std::cout << "\n=== Summary ===\n";
  std::cout << "Total variant groups: " << groups.size() << "\n";

  size_t total_sets = 0;
  size_t total_options = 0;
  uint32_t max_depth = 0;

  for (const auto& group : groups) {
    total_sets += group.variant_sets.size();

    for (const auto& vs : group.variant_sets) {
      total_options += vs.options.size();

      // Calculate depth (simplified - just check if nested exists)
      for (const auto& opt : vs.options) {
        if (!opt.nested_variant_sets.empty()) {
          max_depth = std::max(max_depth, 2u);
        }
      }
    }
  }

  std::cout << "Total variant sets: " << total_sets << "\n";
  std::cout << "Total variant options: " << total_options << "\n";
  std::cout << "Max nesting depth: " << max_depth << "\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 1;
  }

  std::string filepath;
  bool verbose = false;
  bool show_summary = false;
  bool json_output = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_help();
      return 0;
    } else if (arg == "-v" || arg == "--verbose") {
      verbose = true;
    } else if (arg == "-s" || arg == "--summary") {
      show_summary = true;
    } else if (arg == "--json") {
      json_output = true;
    } else {
      filepath = arg;
    }
  }

  if (filepath.empty()) {
    std::cerr << "Error: No input file specified\n";
    print_help();
    return 1;
  }

  // Load USD file
  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(filepath, &stage, &warn, &err);
  if (!warn.empty()) {
    std::cerr << "WARN: " << warn << "\n";
  }
  if (!ret) {
    std::cerr << "Error: Failed to load USD file\n";
    std::cerr << err << "\n";
    return 1;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Error: Failed to extract variants\n";
    std::cerr << err << "\n";
    return 1;
  }

  if (scene.variant_groups.empty()) {
    std::cout << "No variants found in file: " << filepath << "\n";
    return 0;
  }

  // Create variant manager for statistics
  DefaultVariantManager manager;
  auto& mutable_groups = manager.GetMutableVariantGroups();
  mutable_groups = scene.variant_groups;

  // Output
  if (json_output) {
    std::cout << "{\n";
    std::cout << "  \"file\": \"" << filepath << "\",\n";
    std::cout << "  \"variant_groups\": [\n";

    for (size_t i = 0; i < scene.variant_groups.size(); ++i) {
      const auto& group = scene.variant_groups[i];
      std::cout << "    {\n";
      std::cout << "      \"prim_path\": \"" << group.prim_path << "\",\n";
      std::cout << "      \"variant_sets\": [\n";

      for (size_t j = 0; j < group.variant_sets.size(); ++j) {
        const auto& vs = group.variant_sets[j];
        std::cout << "        {\n";
        std::cout << "          \"name\": \"" << vs.name << "\",\n";
        std::cout << "          \"options\": [";

        for (size_t k = 0; k < vs.options.size(); ++k) {
          if (k > 0) std::cout << ", ";
          std::cout << "\"" << vs.options[k].name << "\"";
        }

        std::cout << "]\n";
        std::cout << "        }";
        if (j < group.variant_sets.size() - 1) std::cout << ",";
        std::cout << "\n";
      }

      std::cout << "      ]\n";
      std::cout << "    }";
      if (i < scene.variant_groups.size() - 1) std::cout << ",";
      std::cout << "\n";
    }

    std::cout << "  ]\n";
    std::cout << "}\n";
  } else {
    std::cout << "=== Variants in " << filepath << " ===\n\n";

    for (const auto& group : scene.variant_groups) {
      print_variant_tree(group);
      std::cout << "\n";
    }

    if (show_summary) {
      print_summary(scene.variant_groups);
    }
  }

  if (verbose && !json_output) {
    auto stats = manager.GetStatistics();
    std::cout << "\n=== Statistics ===\n";
    std::cout << "Variant groups: " << stats.num_variant_groups << "\n";
    std::cout << "Variant sets: " << stats.num_variant_sets << "\n";
    std::cout << "Variant options: " << stats.num_variant_options << "\n";
    std::cout << "Max nesting depth: " << stats.max_nesting_depth << "\n";
  }

  return 0;
}
