// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include "next/eval/property-trace.hh"
#include "minijson.hh"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>

int ExplainProperty(int argc, char** argv) {
  namespace n = lightusd::next;
  std::string input, property;
  n::EvalOptions options;
  options.time = n::TimeQuery::Default();
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--explain" && i + 1 < argc) property = argv[++i];
    else if (a == "--json" || a == "-j") json = true;
    else if ((a == "--time" && i + 1 < argc) || a.rfind("--time=", 0) == 0) {
      const std::string text = a == "--time" ? argv[++i] : a.substr(7);
      if (text == "default") options.time = n::TimeQuery::Default();
      else {
        char* end = nullptr; errno = 0;
        const double time = std::strtod(text.c_str(), &end);
        if (text.empty() || errno || *end || !std::isfinite(time)) return 2;
        options.time = n::TimeQuery::Numeric(time);
      }
    } else if (!a.empty() && a[0] != '-' && input.empty()) input = a;
    else { std::cerr << "unsupported --explain argument: " << a << '\n'; return 2; }
  }
  const n::Path path(property);
  if (input.empty() || property.empty() || path.property_name().empty() || property[0] != '/') {
    std::cerr << "Usage: lusdcat --explain /Prim.attribute [--time default|NUMBER] [--json] FILE\n";
    return 2;
  }
  n::ResolverConfig config;
  config.enable_suffix_fallback = false;
  n::AssetResolver resolver(config);
  n::pcp::LayerRegistry registry;
  std::string warn, error;
  const auto resolved = resolver.Resolve(input);
  auto layer = registry.GetOrLoad(resolver, input, "", &warn, &error);
  if (!layer) { std::cerr << error << '\n'; return 2; }
  auto cache = n::pcp::Cache::Open(resolver, layer, resolved.resolved_path);
  if (!cache) { std::cerr << cache.error() << '\n'; return 2; }
  options.clip_stage_loader = [&](const std::string& asset, n::Stage* stage, std::string* w, std::string* e) {
    const auto clip = resolver.Resolve(asset, resolved.resolved_path);
    return clip.exists && n::LoadUSDComposed(clip.resolved_path, stage, w, e);
  };
  const auto trace = n::TraceProperty(*cache, path.prim_path(), path.property_name(), options);
  const std::string text = n::PropertyResolutionTraceToJSON(trace);
  if (json) std::cout << text << '\n';
  else {
    lightusd::minijson::Value report;
    if (!lightusd::minijson::Parse(text, &report)) return 2;
    std::cout << property << " = " << report["resolution"]["value"].get_string()
              << " (" << report["resolution"]["source"].get_string() << ")\n";
    for (const auto& o : report["opinions"])
      std::cout << o["status"].get_string() << " " << o["layer"].get_string()
                << ":" << o["primPath"].get_string() << " via " << o["arc"].get_string()
                << " offset=" << o["offset"].get_double() << " scale=" << o["scale"].get_double() << '\n';
    if (!trace.error.empty()) std::cerr << trace.error << '\n';
    if (!trace.resolved.error.empty()) std::cerr << trace.resolved.error << '\n';
  }
  return std::cout ? (trace.complete && trace.resolved.success ? 0 : 1) : 2;
}
