// SPDX-License-Identifier: Apache-2.0
// Keep fixture setup and checks active in Release builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lightusd-session-cpp.hh"
#include "preview_cache.hh"

namespace fs = std::filesystem;

int main() {
  const fs::path base = fs::temp_directory_path() / "lusdview-preview-cache-test";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);
  assert(!ec);
  const fs::path root = base / "root.usda";
  const fs::path dependency = base / "dependency.usda";
  {
    std::ofstream output(root);
    output << R"(#usda 1.0
def Cube "Bound" {
  float3[] extent = [(-1, -1, -1), (1, 1, 1)]
}
)";
  }
  {
    std::ofstream output(dependency);
    output << "#usda 1.0\ndef Scope \"Dependency\" {}\n";
  }

  lightusd::api::Stage preview;
  {
    lightusd::api::DocumentSession document;
    assert(document.create() == LIGHTUSD_OK);
    lightusd::api::DocumentSnapshot snapshot;
    assert(document.open_file(root.string().c_str(), &snapshot) == LIGHTUSD_OK);
    assert(lightusd::api::DocumentSnapshotStage(snapshot, &preview) == LIGHTUSD_OK);
    assert(preview.is_read_only());
  }

  lusdview::PreviewCacheOptions options;
  options.mode = lusdview::PreviewCacheMode::Auto;
  options.directory = (base / "cache").string();
  options.maxBytes = size_t(16) << 20;
  const std::string fingerprint = "payload=all;time=default";
  const std::vector<std::string> dependencies = {
      root.string(), dependency.string()};
  std::string reason;
  assert(lusdview::StorePreviewCache(options, root.string(), fingerprint,
                                     preview, dependencies, &reason));

  lusdview::PreviewCacheLookup hit = lusdview::LoadPreviewCache(
      options, root.string(), fingerprint);
  assert(hit.hit);
  assert(hit.stage && hit.stage.prim("/Bound"));
  lightusd_value_view extent{};
  assert(lightusd_attr_get(lightusd_stage_prim_at_path(hit.stage.get(), "/Bound"),
                            "extent", &extent) == LIGHTUSD_OK);
  const float expected_extent[] = {-1,-1,-1,1,1,1};
  assert(extent.nbytes == sizeof(expected_extent));
  assert(std::memcmp(extent.data, expected_extent, sizeof(expected_extent)) == 0);

  const std::string key = lusdview::PreviewCacheFingerprint(root.string(), fingerprint);
  const fs::path cached_stage = fs::path(options.directory) / (key + ".usdc");
  // A valid manifest must not make a corrupt or wrong-format stage a cache hit.
  for (const char* invalid : {"PXR-USDC", "#usda 1.0\ndef Scope \"WrongFormat\" {}\n"}) {
    {
      std::ofstream output(cached_stage, std::ios::binary | std::ios::trunc);
      output << invalid;
    }
    const auto corrupt = lusdview::LoadPreviewCache(options, root.string(), fingerprint);
    assert(!corrupt.hit && !corrupt.stage);
    assert(corrupt.reason.find("preview USDC invalid:") == 0);
    assert(lusdview::StorePreviewCache(options, root.string(), fingerprint,
                                       preview, dependencies, &reason));
  }
  // Previously loaded public handles keep their data after the cache is replaced.
  assert(hit.stage.prim("/Bound"));
  lightusd::api::Stage invalid_stage;
  assert(!lusdview::StorePreviewCache(options, root.string(), "invalid", invalid_stage,
                                      dependencies, &reason));
  assert(reason.find("preview write failed:") == 0);
  assert(!fs::exists(fs::path(options.directory) /
      (lusdview::PreviewCacheFingerprint(root.string(), "invalid") + ".json")));

  lusdview::PreviewCacheLookup different = lusdview::LoadPreviewCache(
      options, root.string(), fingerprint + ";variant=high");
  assert(!different.hit);

  options.mode = lusdview::PreviewCacheMode::Refresh;
  lusdview::PreviewCacheLookup refresh = lusdview::LoadPreviewCache(
      options, root.string(), fingerprint);
  assert(!refresh.hit && refresh.reason == "refresh requested");
  options.mode = lusdview::PreviewCacheMode::Auto;

  // Force metadata invalidation without relying on filesystem timestamp
  // resolution or changing the fixture's semantic shape.
  const auto old_time = fs::last_write_time(dependency, ec);
  assert(!ec);
  fs::last_write_time(dependency, old_time + std::chrono::seconds(2), ec);
  assert(!ec);
  lusdview::PreviewCacheLookup stale = lusdview::LoadPreviewCache(
      options, root.string(), fingerprint);
  assert(!stale.hit);
  assert(stale.reason.find("dependency changed") != std::string::npos);

  {
    std::ofstream output(fs::path(options.directory) / (key + ".json"),
                         std::ios::trunc);
    output << "{";
  }
  lusdview::PreviewCacheLookup malformed = lusdview::LoadPreviewCache(
      options, root.string(), fingerprint);
  assert(!malformed.hit && malformed.reason == "manifest malformed");

  fs::remove_all(base, ec);
  std::cout << "PASS: preview cache hit and invalidation\n";
  return 0;
}
