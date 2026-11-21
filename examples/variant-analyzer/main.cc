#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "load-usd.hh"
#include "writer/usda-writer.hh"

using namespace lightusd;

struct VariantOption {
  uint32_t nested_depth = 0;
};
struct VariantSet {
  std::string name;
  std::vector<VariantOption> options;
};
struct VariantGroup {
  std::string prim_path;
  std::vector<VariantSet> variant_sets;
};

// Count authored nesting, including child prims introduced by an option.
static uint32_t variant_depth(const std::vector<PrimVariantSetData>& sets) {
  uint32_t depth = sets.empty() ? 0u : 1u;
  for (const auto& set : sets) {
    for (const auto& option : set.variants) {
      depth = std::max(depth, 1u + variant_depth(option.variantSets));
      if (option.content) {
        for (const auto& prim : option.content->prims())
          depth =
              std::max(depth, 1u + variant_depth(prim.meta().variantSets()));
      }
    }
  }
  return depth;
}

static std::vector<VariantGroup> collect_variants(const Stage& stage) {
  std::vector<VariantGroup> groups;
  for (const auto& prim : stage.GetRootLayer()->prims()) {
    if (prim.meta().variantSets().empty()) continue;
    VariantGroup group;
    group.prim_path = prim.path().str();
    for (const auto& set : prim.meta().variantSets()) {
      VariantSet result;
      result.name = set.name;
      for (const auto& option : set.variants) {
        uint32_t depth = variant_depth(option.variantSets);
        if (option.content) {
          for (const auto& child : option.content->prims())
            depth = std::max(depth, variant_depth(child.meta().variantSets()));
        }
        result.options.push_back({depth});
      }
      group.variant_sets.push_back(std::move(result));
    }
    groups.push_back(std::move(group));
  }
  return groups;
}

static uint32_t capped_product(uint32_t a, uint32_t b) {
  return b && a > 1000000u / b ? UINT32_MAX : a * b;
}

static std::string json_string(const std::string& value) {
  std::ostringstream out;
  out << '\"';
  for (unsigned char c : value) {
    if (c == '\"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << unsigned(c) << std::dec;
    else
      out << c;
  }
  out << '\"';
  return out.str();
}

void print_help() {
  std::cout << "Usage: variant-analyzer [options] input.usda/usdc/usdz\n\n";
  std::cout << "Analyze variant complexity and statistics in a USD file.\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help              Show this help message\n";
  std::cout << "  --detailed              Show detailed analysis\n";
  std::cout << "  --json                  Output in JSON format\n";
}

struct ComplexityMetrics {
  uint32_t num_prims_with_variants = 0;
  uint32_t total_variant_sets = 0;
  uint32_t total_variant_options = 0;
  uint32_t total_variant_combinations = 1;
  uint32_t max_nesting_depth = 0;
  uint32_t max_variants_per_prim = 0;
  uint32_t max_options_per_set = 0;
  float avg_options_per_set = 0.0f;
  float avg_variants_per_prim = 0.0f;
};

ComplexityMetrics analyze_variants(const std::vector<VariantGroup>& groups) {
  ComplexityMetrics metrics;

  metrics.num_prims_with_variants = groups.size();

  for (const auto& group : groups) {
    metrics.total_variant_sets += group.variant_sets.size();
    metrics.max_variants_per_prim =
        std::max(metrics.max_variants_per_prim,
                 static_cast<uint32_t>(group.variant_sets.size()));

    uint32_t prim_combinations = 1;

    for (const auto& vs : group.variant_sets) {
      uint32_t option_count = vs.options.size();
      metrics.total_variant_options += option_count;
      metrics.max_options_per_set =
          std::max(metrics.max_options_per_set, option_count);

      prim_combinations = capped_product(prim_combinations, option_count);

      // Check if has nested variants (simplified depth calculation)
      for (const auto& opt : vs.options) {
        metrics.max_nesting_depth =
            std::max(metrics.max_nesting_depth, 1u + opt.nested_depth);
      }
    }

    // Total combinations is product of all variant options
    // (capped to avoid overflow)
    metrics.total_variant_combinations =
        capped_product(metrics.total_variant_combinations, prim_combinations);
  }

  if (metrics.total_variant_sets > 0) {
    metrics.avg_options_per_set =
        static_cast<float>(metrics.total_variant_options) /
        metrics.total_variant_sets;
  }

  if (metrics.num_prims_with_variants > 0) {
    metrics.avg_variants_per_prim =
        static_cast<float>(metrics.total_variant_sets) /
        metrics.num_prims_with_variants;
  }

  return metrics;
}

void print_text_report(const std::string& filepath,
                       const ComplexityMetrics& metrics,
                       const std::vector<VariantGroup>& groups, bool detailed) {
  std::cout << "=== Variant Complexity Analysis ===\n";
  std::cout << "File: " << filepath << "\n\n";

  std::cout << "=== Summary ===\n";
  std::cout << "Prims with variants:     " << metrics.num_prims_with_variants
            << "\n";
  std::cout << "Total variant sets:      " << metrics.total_variant_sets
            << "\n";
  std::cout << "Total variant options:   " << metrics.total_variant_options
            << "\n";

  std::cout << "\n=== Statistics ===\n";
  std::cout << "Max variants per prim:   " << metrics.max_variants_per_prim
            << "\n";
  std::cout << "Max options per set:     " << metrics.max_options_per_set
            << "\n";
  std::cout << "Avg options per set:     " << std::fixed << std::setprecision(2)
            << metrics.avg_options_per_set << "\n";
  std::cout << "Avg variants per prim:   " << metrics.avg_variants_per_prim
            << "\n";
  std::cout << "Max nesting depth:       " << metrics.max_nesting_depth << "\n";

  if (metrics.total_variant_combinations == UINT32_MAX) {
    std::cout << "Total combinations:      > 1,000,000 (capped)\n";
  } else {
    std::cout << "Total combinations:      "
              << metrics.total_variant_combinations << "\n";
  }

  if (detailed) {
    std::cout << "\n=== Detailed Breakdown ===\n";
    for (size_t i = 0; i < groups.size(); ++i) {
      const auto& group = groups[i];
      std::cout << "\nPrim " << (i + 1) << ": " << group.prim_path << "\n";
      std::cout << "  Variant sets: " << group.variant_sets.size() << "\n";

      uint32_t prim_combinations = 1;
      for (const auto& vs : group.variant_sets) {
        uint32_t option_count = vs.options.size();
        prim_combinations = capped_product(prim_combinations, option_count);

        std::cout << "    - \"" << vs.name << "\": " << option_count
                  << " options";

        // Check for nested variants
        bool has_nested = false;
        for (const auto& opt : vs.options) {
          if (opt.nested_depth) {
            has_nested = true;
            break;
          }
        }

        if (has_nested) {
          std::cout << " [with nested variants]";
        }

        std::cout << "\n";
      }

      std::cout << "  Possible combinations: " << prim_combinations << "\n";
    }
  }

  // Complexity assessment
  std::cout << "\n=== Complexity Assessment ===\n";
  if (metrics.max_nesting_depth > 5) {
    std::cout << "⚠  High nesting depth (" << metrics.max_nesting_depth
              << ")\n";
  } else if (metrics.max_nesting_depth > 3) {
    std::cout << "⚡ Moderate nesting depth (" << metrics.max_nesting_depth
              << ")\n";
  } else {
    std::cout << "✓  Low nesting depth (" << metrics.max_nesting_depth << ")\n";
  }

  if (metrics.total_variant_combinations > 1000000) {
    std::cout << "⚠  Very high variant combinations (>1M)\n";
  } else if (metrics.total_variant_combinations > 100000) {
    std::cout << "⚡ High variant combinations ("
              << metrics.total_variant_combinations << ")\n";
  } else {
    std::cout << "✓  Manageable variant combinations ("
              << metrics.total_variant_combinations << ")\n";
  }

  if (metrics.max_options_per_set > 20) {
    std::cout << "⚠  Large variant set (" << metrics.max_options_per_set
              << " options)\n";
  } else if (metrics.max_options_per_set > 10) {
    std::cout << "⚡ Medium-sized variant set (" << metrics.max_options_per_set
              << " options)\n";
  } else {
    std::cout << "✓  Small variant set (" << metrics.max_options_per_set
              << " options)\n";
  }
}

void print_json_report(const std::string& filepath,
                       const ComplexityMetrics& metrics) {
  std::cout << "{\n";
  std::cout << "  \"file\": " << json_string(filepath) << ",\n";
  std::cout << "  \"summary\": {\n";
  std::cout << "    \"prims_with_variants\": "
            << metrics.num_prims_with_variants << ",\n";
  std::cout << "    \"total_variant_sets\": " << metrics.total_variant_sets
            << ",\n";
  std::cout << "    \"total_variant_options\": "
            << metrics.total_variant_options << "\n";
  std::cout << "  },\n";
  std::cout << "  \"statistics\": {\n";
  std::cout << "    \"max_variants_per_prim\": "
            << metrics.max_variants_per_prim << ",\n";
  std::cout << "    \"max_options_per_set\": " << metrics.max_options_per_set
            << ",\n";
  std::cout << "    \"avg_options_per_set\": " << std::fixed
            << std::setprecision(2) << metrics.avg_options_per_set << ",\n";
  std::cout << "    \"avg_variants_per_prim\": "
            << metrics.avg_variants_per_prim << ",\n";
  std::cout << "    \"max_nesting_depth\": " << metrics.max_nesting_depth
            << ",\n";
  std::cout << "    \"total_combinations\": ";

  if (metrics.total_variant_combinations == UINT32_MAX) {
    std::cout << "null";
  } else {
    std::cout << metrics.total_variant_combinations;
  }

  std::cout << "\n";
  std::cout << "  }\n";
  std::cout << "}\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 1;
  }

  std::string filepath;
  bool detailed = false;
  bool json_output = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_help();
      return 0;
    } else if (arg == "--detailed") {
      detailed = true;
    } else if (arg == "--json") {
      json_output = true;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      return 1;
    } else if (!filepath.empty()) {
      std::cerr << "Only one input file is supported\n";
      return 1;
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

  bool ret = LoadUSD(filepath, &stage, &warn, &err);
  if (!warn.empty()) {
    std::cerr << "WARN: " << warn << "\n";
  }
  if (!ret) {
    std::cerr << "Error: Failed to load USD file\n";
    std::cerr << err << "\n";
    return 1;
  }

  const auto groups = collect_variants(stage);

  // Analyze
  auto metrics = analyze_variants(groups);

  // Output
  if (json_output) {
    print_json_report(filepath, metrics);
  } else {
    print_text_report(filepath, metrics, groups, detailed);
  }

  return 0;
}
