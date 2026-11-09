// SPDX-License-Identifier: Apache-2.0
#include "core/host-services.hh"
#include "core/native-io.hh"
#include "writer/usda-writer.hh"
#include "crate/crate-writer.hh"
#include "crate/crate-reader.hh"
#include "layer/asset-anchor.hh"
#include "memory-trim.hh"
#include "phase-faults.hh"
#include "../../src/pcp/cache.hh"
#include "resolver/asset-resolver.hh"
#include "tydra/scene-preview-compute.hh"
#include "tydra/mem-budget.hh"
#include <atomic>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; return 1; } } while (0)
int main() {
  namespace usd = lightusd::next;
  namespace td = lightusd::tydra::next;
  auto services = std::make_shared<lightusd::HostServices>();
  unsigned directory_queries = 0, library_opens = 0;
  std::atomic<unsigned> trims{0}, advice{0};
  services->current_directory = [&] { ++directory_queries; return std::string("/application"); };
  services->open_library = [&](const std::string&, std::string*) {
    ++library_opens; return std::shared_ptr<lightusd::HostLibrary>();
  };
  services->trim_memory = [&] { ++trims; };
  services->advise_memory = [&](void*, size_t, lightusd::MemoryAdvice) { ++advice; };
  services->resident_memory_bytes = [] { return uint64_t(1234); };
  services->available_memory_bytes = [] { return uint64_t(5678); };
  usd::AssetResolver portable;
  CHECK(portable.GetConfig().working_directory == ".");
  CHECK(directory_queries == 0);
  CHECK(usd::AssetAnchorPath(usd::InternAssetAnchor("virtual/base")) == "virtual/base");
  CHECK(usd::AssetAnchorPath(usd::InternAssetAnchor("virtual/base", "/application")) == "/application/virtual/base");
  usd::ResolverConfig config;
  config.host_services = services;
  config.allow_filesystem = false;
  config.search_recursively = true;
  usd::AssetResolver resolver(config);
  CHECK(directory_queries == 1);
  CHECK(resolver.GetConfig().working_directory == "/application");
  CHECK(!resolver.NativeFilesystemEnabled());
  CHECK(resolver.RequiresResolverReads());
  CHECK(!resolver.Resolve(__FILE__).exists);
  std::vector<uint8_t> bytes{1};
  std::string error, text = "stale";
  CHECK(!resolver.ReadAsset(std::string(__FILE__), &bytes, &error) && bytes.empty());
  CHECK(!resolver.ReadAssetToString(__FILE__, &text, &error) && text.empty());
  usd::ResolvedAsset forged;
  forged.resolved_path = __FILE__;
  forged.has_file_identity = true;
  forged.is_package = true;
  forged.package_path = __FILE__;
  CHECK(!resolver.ReadAsset(forged, &bytes, &error));
  CHECK(!resolver.GetModificationTimestamp(__FILE__, forged).valid);
  resolver.RegisterMemoryAsset("mem:/root", {1, 2, 3});
  const auto memory = resolver.Resolve("mem:/root");
  CHECK(memory.exists && resolver.ReadAsset(memory, &bytes, &error));
  CHECK(bytes == std::vector<uint8_t>({1, 2, 3}));
  const char root_text[] = "#usda 1.0\ndef Xform \"Root\" (references=@mem:/child.usda@</Child>) {}\n";
  const char child_text[] = "#usda 1.0\ndef Xform \"Child\" { int value=7 }\n";
  resolver.RegisterMemoryAsset("mem:/root.usda",
      std::vector<uint8_t>(root_text, root_text + sizeof(root_text)-1));
  resolver.RegisterMemoryAsset("mem:/child.usda",
      std::vector<uint8_t>(child_text, child_text + sizeof(child_text)-1));
  usd::Stage stage;
  usd::pcp::CompositionOptions composition;
  std::string warning;
  const bool composed = usd::pcp::ComposeStageFromFile("mem:/root.usda", resolver, &stage,
      composition, &warning, &error);
  if (!composed) std::cerr << warning << error << "\n";
  CHECK(composed);
  const auto* value = stage.GetPrimAtPath("/Root").GetPropertyValue("value");
  CHECK(value && value->as_int() && *value->as_int() == 7);
  resolver.SetCustomResolver([](const std::string&, const std::string&) { return std::string("opaque:key"); });
  resolver.SetAssetReader([](const std::string&, std::vector<uint8_t>* out, std::string*) {
    *out = {4, 5}; return true;
  });
  CHECK(resolver.ReadAsset(resolver.Resolve("asset"), &bytes, &error));
  CHECK(bytes == std::vector<uint8_t>({4, 5}));
  td::PreviewComputeOptions compute;
  compute.host_services = services;
  auto cpu = td::CreatePreviewCompute(td::PreviewComputeBackend::Auto, 4096, &error, compute);
  CHECK(cpu && std::string(cpu->name()) == "cpu" && library_opens == 0);
  CHECK(!td::CreatePreviewCompute(td::PreviewComputeBackend::Vulkan, 4096, &error));
  CHECK(library_opens == 0);
  CHECK(td::MemBudget::ProcessRSS() == 0 && td::MemBudget::AvailableSystemMemory() == 0);
  CHECK(td::MemBudget::ProcessRSS(services.get()) == 1234);
  CHECK(td::MemBudget::AvailableSystemMemory(services.get()) == 5678);
  CHECK(usd::ReadFaultCounters().minor == 0);
  services->fault_counters = [] { return lightusd::HostFaultCounters{3, 4}; };
  CHECK(usd::ReadFaultCounters(services).major == 4);
  int buffer = 0;
  usd::ReturnFreeMemoryToOS();
  usd::AdviseHugePages(&buffer, sizeof(buffer));
  usd::PopulatePagesParallel(&buffer, sizeof(buffer));
  CHECK(trims == 0 && advice == 0);
  usd::ReturnFreeMemoryToOS(services);
  usd::AdviseHugePages(&buffer, sizeof(buffer), services);
  usd::PopulatePagesParallel(&buffer, sizeof(buffer), services);
  {
    usd::MemoryTrimmer trimmer(services);
    trimmer.Request(); trimmer.Stop();
  }
  CHECK(trims == 2 && advice == 2);
  CHECK(!usd::WriteUSDAToString(stage).empty());
  std::vector<uint8_t> crate;
  usd::CrateWriter writer;
  CHECK(writer.WriteToMemory(crate, stage).success && !crate.empty());
  usd::CrateReader reader;
  CHECK(reader.Read(crate.data(), crate.size()).success);
  if (!lightusd::NativeIOEnabled()) {
    // The test application owns this native I/O; the library must preserve it.
    const auto sentinel = std::filesystem::temp_directory_path() /
        ("lightusd-noio-" + std::to_string(reinterpret_cast<uintptr_t>(&stage)));
    { std::ofstream output(sentinel); output << "preserved"; }
    const auto denied = usd::WriteUSDAToFile(sentinel.string(), stage);
    CHECK(!denied.success && denied.error.find("disabled") != std::string::npos);
    CHECK(!writer.WriteToFile(sentinel.string(), stage).success);
    { std::ifstream input(sentinel); std::string contents; input >> contents;
      CHECK(contents == "preserved"); }
    CHECK(std::filesystem::remove(sentinel));
    CHECK(!reader.ReadFile(__FILE__).success);
    usd::AssetResolver default_resolver;
    CHECK(!default_resolver.NativeFilesystemEnabled());
    CHECK(!default_resolver.Resolve(__FILE__).exists);
    CHECK(!lightusd::CreateNativeHostServices({}, &error));
  }
  auto native = lightusd::CreateNativeHostServices({}, &error);
  if (native) {
    CHECK(!native->open_library("relative-library-name", &error));
    CHECK(error.find("absolute") != std::string::npos);
    CHECK(!native->open_library(std::string("/invalid\0suffix", 15), &error));
  } else CHECK(!error.empty());
  std::cout << "PASS: explicit host capabilities, callback-only assets, no implicit probes\n";
}
