// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <iostream>
#include <string>

#include "next/reader/obj-reader.hh"

namespace ia = lightusd::tydra::import_asset;
namespace n = lightusd::next;
namespace {
void Check(bool ok, const char* message) {
  if (!ok) { std::cerr << "Texture regression failed: " << message << '\n'; std::exit(1); }
}
void SharedImports() {
  const std::vector<uint8_t> image(1024 * 1024, 73);
  const std::string mtl = "newmtl a\nmap_Kd alias_a.png\nmap_Ks alias_b.png\n"
      "newmtl b\nmap_Kd alias_b.png\nnewmtl c\nmap_Kd alias_a.png\n";
  const std::string obj = "mtllib materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
      "usemtl a\nf 1 2 3\nusemtl b\nf 1 2 3\nusemtl c\nf 1 2 3\n";
  size_t texture_reads = 0, material_reads = 0;
  ia::Options options;
  options.max_memory = 4 * 1024 * 1024;
  options.open = [&](const std::string& name, const std::string&,
                     ia::AssetSource* source, std::string*) {
    if (name == "materials.mtl") {
      source->identifier = "memory:materials.mtl";
      source->read = [&](std::vector<uint8_t>* out, std::string*) {
        ++material_reads;
        out->assign(mtl.begin(), mtl.end());
        return true;
      };
    } else {
      source->identifier = "memory:shared.png";
      source->read = [&](std::vector<uint8_t>* out, std::string*) {
        ++texture_reads;
        *out = image;
        return true;
      };
    }
    return true;
  };
  std::string err, warn;
  ia::OBJ geometry;
  ia::Result result;
  Check(ia::BuildOBJScene(obj, "memory:scene.obj", options, &geometry, &result, &err), err.c_str());
  Check(texture_reads == 1 && material_reads == 1, "alias textures read once through conversion and gathering");
  Check(result.assets.size() == 1 && *result.assets.begin()->second == image, "one retained package payload");
  std::string package;
  for (const auto& material : result.payload["materials"])
    for (const auto& map : material["maps"]) {
      const auto name = map["package"].get<std::string>();
      if (package.empty()) package = name;
      Check(name == package, "all maps use one package entry");
    }
  std::weak_ptr<const std::vector<uint8_t>> retained;
  {
    auto layer = n::LoadOBJLayerFromMemory("memory:scene.obj",
        reinterpret_cast<const uint8_t*>(obj.data()), obj.size(), options, &warn, &err);
    Check(bool(layer), err.c_str());
    Check(texture_reads == 2 && material_reads == 2, "cache scope is a single import");
    auto assets = layer->meta().imported_assets;
    Check(assets && assets->by_identifier.size() == 1, "one layer-owned texture");
    retained = assets->by_identifier.begin()->second;
    auto clone = layer->Clone();
    assets.reset();
    layer.reset();
    Check(!retained.expired() && *retained.lock() == image, "cloned layer retains immutable bytes");
    clone.meta().imported_assets.reset();
  }
  Check(retained.expired(), "last layer releases its texture bytes");
}
void LegacyCallbacks() {
  ia::Options options;
  options.max_memory = 1024 * 1024 + 4096;
  size_t reads = 0;
  options.read = [&](const std::string&, const std::string&, std::vector<uint8_t>* out,
                     std::string* resolved, std::string*) {
    ++reads;
    *resolved = "memory:shared.png";
    out->assign(1024 * 1024, 37);
    return true;
  };
  ia::Reader reader(options);
  ia::AssetBytes a, b;
  std::string resolved, err;
  Check(reader.ReadTexture("a.png", "", &a, &resolved, &err), err.c_str());
  Check(reader.ReadTexture("b.png", "", &b, &resolved, &err), err.c_str());
  Check(reader.ReadTexture("a.png", "", &b, &resolved, &err), err.c_str());
  Check(a == b && reads == 2 && reader.textures.size() == 1, "combined callbacks retain one payload and cache requests");
}
void AuthoritativeOpen() {
  ia::Options options;
  size_t fallback_reads = 0;
  options.open = [](const std::string&, const std::string&, ia::AssetSource*, std::string* err) {
    *err = "unresolved";
    return false;
  };
  options.read = [&](const std::string&, const std::string&, std::vector<uint8_t>*, std::string*, std::string*) {
    ++fallback_reads;
    return true;
  };
  ia::Reader reader(options);
  ia::AssetBytes asset;
  std::vector<uint8_t> bytes;
  std::string resolved, err;
  Check(!reader.ReadTexture("missing.png", "", &asset, &resolved, &err), "failed open rejects texture");
  Check(!reader.Read("missing.obj", "", &bytes, &resolved, &err) && !fallback_reads, "failed open has no read fallback");
}
void Reports() {
  ia::Options options;
  options.report = std::make_shared<ia::ImportReport>();
  const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
  ia::OBJ geometry;
  ia::Result result;
  std::string err;
  Check(ia::BuildOBJScene(obj, "memory:ok.obj", options, &geometry, &result, &err), err.c_str());
  Check(result.report == options.report, "native result exposes caller report");
  Check(result.report->status == "success" && result.report->input_bytes == obj.size(), "native success telemetry");
  Check(!ia::BuildOBJScene("v invalid 0 0\n", "memory:bad.obj", options, &geometry, &result, &err), "invalid OBJ fails");
  Check(options.report->status == "failed" && options.report->source == "memory:bad.obj", "reused collector resets on new import");
  Check(!options.report->diagnostics.empty(), "failure retains diagnostics");
  options.report->Diagnostic("warning", "test.invalid_utf8", std::string(1, char(0xff)));
  options.report->Diagnostic("warning", "test.long_text", std::string(20000, 'a'));
  for (size_t i = 0; i < 5000; ++i) options.report->Diagnostic("warning", "test.bounded", "bounded warning");
  const auto serialized = options.report->ToJSON().dump();
  Check(!serialized.empty() && serialized.size() < 2 * 1024 * 1024, "report remains bounded and valid UTF-8 JSON");
  Check(options.report->dropped_records && options.report->truncated_fields, "report declares truncation");
}
}
int main() {
  SharedImports();
  LegacyCallbacks();
  AuthoritativeOpen();
  Reports();
  std::cout << "Shared texture reads, ownership, aliases and resolver policy passed\n";
}
