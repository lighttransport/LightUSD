// SPDX-License-Identifier: Apache-2.0
#include "lightusd-render-cpp.hh"
#include "minijson.hh"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  bool strict = false;
  std::string input, output, report;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--strict") strict = true;
    else if (arg == "--report" && i + 1 < argc) report = argv[++i];
    else if (!arg.empty() && arg[0] != '-' && input.empty()) input = arg;
    else if (!arg.empty() && arg[0] != '-' && output.empty()) output = arg;
    else { std::cerr << "Usage: usd_to_gltf INPUT OUTPUT.glb [--strict] [--report losses.json]\n"; return 2; }
  }
  if (input.empty() || output.empty() || output == input || report == input || report == output) {
    std::cerr << "Supply distinct input, output and optional report paths\n"; return 2;
  }

  lightusd_load_options load_options;
  lightusd::api::InitLoadOptions(&load_options);
  load_options.max_resident_bytes = uint64_t(1) << 30;
  lightusd::api::Stage stage;
  if (stage.load(input.c_str(), &load_options) != LIGHTUSD_OK) {
    std::cerr << lightusd_last_error() << '\n';
    return 1;
  }

  lightusd_render_config render_config;
  lightusd::api::InitRenderConfig(&render_config);
  render_config.load_textures = 0;
  lightusd::api::RenderScene scene;
  if (lightusd::api::Convert(stage, &scene, &render_config) != LIGHTUSD_OK) {
    std::cerr << lightusd_last_error() << '\n';
    return 1;
  }
  lightusd::api::String glb;
  lightusd::api::StringList losses;
  const lightusd_status export_status = lightusd::api::ExportGLB(
      scene, input.c_str(), strict, uint64_t(1) << 30, &glb, &losses);
  const std::string error = export_status == LIGHTUSD_OK
                                ? std::string() : lightusd_last_error();
  std::vector<std::string> all_losses;
  const auto append = [&all_losses](const lightusd::api::StringList& list) {
    for (size_t i = 0; i < lightusd_strlist_size(list.get()); ++i) {
      const lightusd_sv item = lightusd_strlist_get(list.get(), i);
      all_losses.emplace_back(item.data, item.len);
    }
  };
  append(losses);
  const bool success = export_status == LIGHTUSD_OK;
  for (const std::string& loss : all_losses) std::cerr << "loss: " << loss << '\n';
  if (!report.empty()) {
    auto json_losses = lightusd::minijson::Value::array();
    for (const std::string& loss : all_losses) json_losses.push_back(loss);
    lightusd::minijson::Value json{{"schemaVersion", 1}, {"success", success},
        {"error", success ? "" : (error.empty() ? "strict export refuses conversion losses" : error)},
        {"losses", json_losses}};
    std::ofstream out(report); out << json.dump(2) << '\n'; out.close();
    if (!out) { std::cerr << "Cannot write loss report\n"; return 1; }
  }
  if (!success) {
    std::cerr << (error.empty() ? "strict export refuses conversion losses" : error) << '\n';
    return 1;
  }
  const lightusd_sv bytes = lightusd_string_view(glb.get());
  std::ofstream out(output, std::ios::binary);
  out.write(bytes.data, std::streamsize(bytes.len));
  out.close();
  if (!out) { std::cerr << "Cannot write GLB\n"; return 1; }
  return 0;
}
