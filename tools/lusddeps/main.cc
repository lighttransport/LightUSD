// SPDX-License-Identifier: Apache-2.0
#include "next/resolver/dependency-report.hh"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
  lightusd::next::DependencyOptions options;
  lightusd::next::ResolverConfig config;
  config.enable_suffix_fallback = false;
  std::string input;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: lusddeps [--json] [--composed|--all-variants] [--no-payloads]\n"
                   "                [--search-path DIR] [--max-layers N] [--max-records N]\n"
                   "                [--max-depth N] [--max-memory-mb N] FILE\n"
                   "Inventory authored dependencies with source locations. Default: authored variant selections.\n"
                   "Exit: 0 complete, 1 incomplete/unresolved, 2 usage error.\n";
      return 0;
    } else if (arg == "--json") json = true;
    else if (arg == "--composed") options.scope = lightusd::next::DependencyScope::ComposedStage;
    else if (arg == "--all-variants") options.all_authored_variants = true;
    else if (arg == "--no-payloads") options.follow_payloads = false;
    else if (arg == "--search-path" && i + 1 < argc) config.search_paths.push_back(argv[++i]);
    else if ((arg == "--max-layers" || arg == "--max-records" || arg == "--max-depth" || arg == "--max-memory-mb") && i + 1 < argc) {
      const char* text = argv[++i]; char* end = nullptr; errno = 0;
      const auto n = std::strtoull(text, &end, 10);
      if (text[0] == '-' || !text[0] || errno || *end || !n || n > std::numeric_limits<size_t>::max()) return 2;
      if (arg == "--max-layers") options.max_layers = size_t(n);
      else if (arg == "--max-records") options.max_records = size_t(n);
      else if (arg == "--max-depth") options.max_depth = size_t(n);
      else {
        if (n > std::numeric_limits<size_t>::max() / (1024 * 1024)) return 2;
        options.max_resident_bytes = size_t(n) * 1024 * 1024;
        options.load.max_memory = options.max_resident_bytes;
      }
    } else if (!arg.empty() && arg[0] != '-' && input.empty()) input = arg;
    else { std::cerr << "lusddeps: invalid argument: " << arg << '\n'; return 2; }
  }
  if (input.empty()) { std::cerr << "lusddeps: input required\n"; return 2; }
  lightusd::next::AssetResolver resolver(config);
  const auto report = lightusd::next::CollectDependencies(input, resolver, options);
  if (json) std::cout << lightusd::next::DependencyReportToJSON(report) << '\n';
  else {
    for (const auto& r : report.records)
      std::cout << r.status << '\t' << r.kind << '\t' << r.referring_layer << ':' << r.location
                << '\t' << r.authored_path << " -> " << r.resolved_identifier << '\n';
    for (const auto& d : report.diagnostics) std::cerr << d << '\n';
    std::cout << (report.complete ? "Complete" : "Incomplete") << " dependency inventory\n";
  }
  return std::cout ? (report.complete ? 0 : 1) : 2;
}
