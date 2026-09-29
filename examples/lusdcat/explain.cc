// SPDX-License-Identifier: Apache-2.0
#include "lightusd-c.h"
#include "minijson.hh"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

int ExplainProperty(int argc, char** argv) {
  std::string input, property;
  double time = std::numeric_limits<double>::quiet_NaN();
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--explain" && i + 1 < argc) property = argv[++i];
    else if (a == "--json" || a == "-j") json = true;
    else if ((a == "--time" && i + 1 < argc) || a.rfind("--time=", 0) == 0) {
      const std::string text = a == "--time" ? argv[++i] : a.substr(7);
      if (text == "default") time = std::numeric_limits<double>::quiet_NaN();
      else {
        char* end = nullptr; errno = 0;
        const double parsed = std::strtod(text.c_str(), &end);
        if (text.empty() || errno || *end || !std::isfinite(parsed)) return 2;
        time = parsed;
      }
    } else if (!a.empty() && a[0] != '-' && input.empty()) input = a;
    else { std::cerr << "unsupported --explain argument: " << a << '\n'; return 2; }
  }
  const size_t dot = property.rfind('.');
  if (input.empty() || property.empty() || property[0] != '/' ||
      dot == std::string::npos || dot <= 1 || dot + 1 >= property.size()) {
    std::cerr << "Usage: lusdcat --explain /Prim.attribute [--time default|NUMBER] [--json] FILE\n";
    return 2;
  }
  const std::string prim_path = property.substr(0, dot);
  const std::string property_name = property.substr(dot + 1);
  lightusd_stage* stage = nullptr;
  lightusd_load_options load_options;
  lightusd_load_options_init(&load_options);
  if (lightusd_stage_load(input.c_str(), &load_options, &stage) != LIGHTUSD_OK) {
    std::cerr << lightusd_last_error() << '\n';
    return 2;
  }
  lightusd_string* json_result = nullptr;
  if (lightusd_stage_explain_property(stage, prim_path.c_str(),
                                      property_name.c_str(), time,
                                      &json_result) != LIGHTUSD_OK) {
    std::cerr << lightusd_last_error() << '\n';
    lightusd_stage_destroy(stage);
    return 2;
  }
  const lightusd_sv json_view = lightusd_string_view(json_result);
  const std::string text(json_view.data ? json_view.data : "", json_view.len);
  if (json) std::cout << text << '\n';
  else {
    lightusd::minijson::Value report;
    if (!lightusd::minijson::Parse(text, &report)) {
      lightusd_string_destroy(json_result);
      lightusd_stage_destroy(stage);
      return 2;
    }
    std::cout << property << " = " << report["resolution"]["value"].get_string()
              << " (" << report["resolution"]["source"].get_string() << ")\n";
    for (const auto& o : report["opinions"])
      std::cout << o["status"].get_string() << " " << o["layer"].get_string()
                << ":" << o["primPath"].get_string() << " via " << o["arc"].get_string()
                << " offset=" << o["offset"].get_double() << " scale=" << o["scale"].get_double() << '\n';
    if (report.contains("error") && report["error"].is_string() &&
        !report["error"].get_string().empty()) {
      std::cerr << report["error"].get_string() << '\n';
    }
  }
  lightusd_string_destroy(json_result);
  lightusd_stage_destroy(stage);
  return std::cout ? 0 : 2;
}
