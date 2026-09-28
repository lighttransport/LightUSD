// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Memory-rooted layer loading + flatten tests for src/next:
// pcp::LoadLayerFromMemory[Owned] content dispatch (USDA / USDC / USDZ),
// AssetResolver::ReadAsset + pipeline::MakeResolverLayerLoader (no
// filesystem), and pipeline::FlattenUSDMemoryToUSDCOwned for USDA roots with
// USDA dependency layers.

#include "next/pcp/layer-registry.hh"
#include "next/pipeline/flatten.hh"
#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace lightusd::next;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << "\n"; ++g_fail; }      \
    else { std::cout << "  ok: " << msg << "\n"; }                          \
  } while (0)

static void test_shared_asset_payload_budget() {
  auto budget = std::make_shared<AssetPayloadBudget>(5);
  auto lease = budget->Reserve(3);
  CHECK(lease && budget->UsedBytes() == 3,
        "shared payload budget tracks retained bytes");
  CHECK(!budget->Reserve(3), "shared payload budget rejects over-cap reserve");
  CHECK(!budget->SetLimit(2), "shared payload cap cannot shrink below use");
  lease.reset();
  CHECK(budget->UsedBytes() == 0 && budget->SetLimit(2),
        "shared payload lease releases bytes for a smaller cap");
}

static const char* kBaseUSDA = R"(#usda 1.0

def Xform "Base"
{
    int imported = 42

    def Mesh "Geo"
    {
        int n = 3
    }
}
)";

static void test_usda_progress_and_cancellation() {
  LoadOptions options;
  std::vector<std::string> phases;
  options.parse_options.progress_callback =
      [&phases](const char* phase, size_t current, size_t total) {
        phases.emplace_back(phase);
        return current <= total;
      };
  const LoadResult loaded = LoadUSDAFromString(kBaseUSDA, options);
  CHECK(loaded.success, "USDA parse with progress callback succeeds");
  CHECK(phases.size() >= 4 && phases.front() == "bootstrap" &&
            phases.back() == "complete",
        "USDA progress reports bootstrap, completed prims, and completion");

  options.parse_options.progress_callback =
      [](const char* phase, size_t, size_t) {
        return std::string(phase) != "prims";
      };
  const LoadResult cancelled = LoadUSDAFromString(kBaseUSDA, options);
  CHECK(!cancelled.success &&
            cancelled.error_summary.find("USDA parse cancelled during prims") !=
                std::string::npos,
        "USDA progress callback can cancel before publishing the Stage");
}

static void test_crate_and_usdz_file_size_limits() {
  LoadResult parsed = LoadUSDAFromString(kBaseUSDA);
  CHECK(parsed.success, "file-limit fixture stage parses");
  if (!parsed.success) return;

  USDCWriteOptions crate_options;
  crate_options.crate_options.max_file_size_bytes = 128;
  std::vector<uint8_t> crate;
  const USDCWriteResult rejected =
      WriteUSDCToMemory(crate, parsed.stage, crate_options);
  CHECK(!rejected.success &&
            rejected.error.find("configured file-size limit") != std::string::npos,
        "Crate writer rejects output beyond the configured byte cap");
  CHECK(crate.empty(), "rejected Crate output is not published");

  crate_options.crate_options.max_file_size_bytes = 0;
  crate_options.crate_options.max_memory_bytes = 1;
  const USDCWriteResult memory_rejected =
      WriteUSDCToMemory(crate, parsed.stage, crate_options);
  CHECK(!memory_rejected.success &&
            memory_rejected.error.find("estimated working set") != std::string::npos,
        "Crate writer rejects an estimated working set above its memory policy");
  CHECK(crate.empty(), "memory-rejected Crate output is not published");
  crate_options.crate_options.max_memory_bytes = 1u << 20;

  crate_options.crate_options.max_file_size_bytes = 1u << 20;
  const USDCWriteResult written =
      WriteUSDCToMemory(crate, parsed.stage, crate_options);
  CHECK(written.success && crate.size() <= (1u << 20),
        "Crate writer accepts output within the configured byte cap");
  if (!written.success) return;

  USDZWriteOptions usdz_options;
  usdz_options.max_file_size_bytes = crate.size() + 70;
  std::vector<uint8_t> package;
  const USDZWriteResult package_rejected = WriteUSDZFromUSDCToMemory(
      package, crate.data(), crate.size(), usdz_options);
  CHECK(!package_rejected.success && package.empty(),
        "USDZ writer applies the file cap to local entries and central directory");

  usdz_options.max_file_size_bytes = 0;
  usdz_options.max_memory_bytes = 1;
  const USDZWriteResult usdz_memory_rejected = WriteUSDZFromUSDCToMemory(
      package, crate.data(), crate.size(), usdz_options);
  CHECK(!usdz_memory_rejected.success &&
            usdz_memory_rejected.error.find("estimated working set") != std::string::npos &&
            package.empty(),
        "USDZ writer rejects a working set above its memory policy");

  std::map<std::string, std::vector<uint8_t>> assets;
  assets["payload.bin"].resize(2u << 20, 0x5a);
  usdz_options.max_memory_bytes = 3u << 20;
  const USDZWriteResult asset_memory_rejected =
      WriteUSDZFromUSDCAndAssetsToMemory(package, crate.data(), crate.size(),
                                          assets, usdz_options);
  CHECK(!asset_memory_rejected.success && package.empty(),
        "USDZ memory policy includes retained archive assets and output storage");
  usdz_options.max_memory_bytes = 8u << 20;
  const USDZWriteResult asset_memory_accepted =
      WriteUSDZFromUSDCAndAssetsToMemory(package, crate.data(), crate.size(),
                                          assets, usdz_options);
  CHECK(asset_memory_accepted.success && !package.empty(),
        "USDZ accepts archive payloads within a sufficient memory estimate");
}

static void test_writer_memory_estimate_counts_string_payloads() {
  std::vector<std::string> string_items;
  string_items.emplace_back(400u * 1024u, 's');
  const Value string_array = Value::MakeStringLikeArray(
      std::move(string_items), TypeId::String);
  CHECK(string_array.dynamic_string_memory_usage() >= 400u * 1024u,
        "dynamic value estimate includes string-array storage");
  Dict nested;
  nested.set("payload", Value(std::string(400u * 1024u, 'd')));
  const Value dictionary = Value::MakeDictionary(std::move(nested));
  CHECK(dictionary.dynamic_string_memory_usage() >= 400u * 1024u,
        "dynamic value estimate includes recursive dictionary strings");

  const std::string payload(1100u * 1024u, 'x');
  const std::string source = "#usda 1.0\ndef Xform \"Root\" { custom string payload = \"" +
      payload + "\" }\n";
  LoadResult parsed = LoadUSDAFromString(source);
  CHECK(parsed.success, "large string memory-estimate fixture parses");
  if (!parsed.success) return;

  USDCWriteOptions options;
  options.crate_options.max_memory_bytes = 1u << 20;
  std::vector<uint8_t> crate;
  const USDCWriteResult rejected = WriteUSDCToMemory(crate, parsed.stage, options);
  CHECK(!rejected.success &&
            rejected.error.find("estimated working set") != std::string::npos,
        "writer memory estimate includes retained scalar string capacity");
  CHECK(crate.empty(), "string-heavy memory-rejected output is not published");

  options.crate_options.max_memory_bytes = 4u << 20;
  const USDCWriteResult accepted = WriteUSDCToMemory(crate, parsed.stage, options);
  CHECK(accepted.success && !crate.empty(),
        "writer accepts string payload under a sufficient estimate budget");
}

static const char* kRootUSDA = R"(#usda 1.0
(
    defaultPrim = "Root"
)

def Xform "Root" (
    references = @dep.usda@</Base>
)
{
    double localOnly = 1.0
}
)";

static std::vector<uint8_t> MakeCrateBytes(const char* usda_text) {
  LoadResult r = LoadUSDAFromString(usda_text, std::strlen(usda_text));
  if (!r.success) return {};
  std::vector<uint8_t> buf;
  USDCWriteResult wr = WriteUSDCToMemory(buf, r.stage);
  if (!wr.success) return {};
  return buf;
}

// LoadLayerFromMemory dispatches USDA text / crate magic / ZIP header.
static void test_load_layer_from_memory_dispatch() {
  std::cout << "[LoadLayerFromMemory dispatch]\n";
  std::string warn, err;

  // USDA text
  {
    std::shared_ptr<Layer> l = pcp::LoadLayerFromMemory(
        "mem.usda", reinterpret_cast<const uint8_t*>(kBaseUSDA),
        std::strlen(kBaseUSDA), &warn, &err);
    CHECK(l != nullptr, "USDA text parses");
    if (l) {
      l->build_path_index();
      CHECK(l->prim_at_path("/Base/Geo") != nullptr, "USDA layer has prims");
    }
  }

  // Crate bytes
  {
    std::vector<uint8_t> crate = MakeCrateBytes(kBaseUSDA);
    CHECK(!crate.empty(), "crate fixture built");
    err.clear();
    std::shared_ptr<Layer> l = pcp::LoadLayerFromMemory(
        "mem.usdc", crate.data(), crate.size(), &warn, &err);
    CHECK(l != nullptr, "crate bytes parse");
    if (l) {
      l->build_path_index();
      CHECK(l->prim_at_path("/Base") != nullptr, "crate layer has prims");
    }
  }

  // USDZ bytes (crate entry inside zip)
  {
    std::vector<uint8_t> crate = MakeCrateBytes(kBaseUSDA);
    std::vector<uint8_t> zip;
    USDZWriteResult zr =
        WriteUSDZFromUSDCToMemory(zip, crate.data(), crate.size());
    CHECK(zr.success, "usdz fixture built");
    err.clear();
    std::shared_ptr<Layer> l = pcp::LoadLayerFromMemory(
        "mem.usdz", zip.data(), zip.size(), &warn, &err);
    CHECK(l != nullptr, "usdz bytes parse");
    if (l) {
      l->build_path_index();
      CHECK(l->prim_at_path("/Base") != nullptr, "usdz layer has prims");
    }
  }

  // Owned variant (USDA)
  {
    err.clear();
    std::shared_ptr<Layer> l = pcp::LoadLayerFromMemoryOwned(
        "mem.usda", std::string(kBaseUSDA), &warn, &err);
    CHECK(l != nullptr, "owned USDA parses");
  }

  // Empty buffer fails cleanly
  {
    err.clear();
    std::shared_ptr<Layer> l =
        pcp::LoadLayerFromMemory("empty", nullptr, 0, &warn, &err);
    CHECK(l == nullptr && !err.empty(), "empty buffer rejected with error");
  }
}

// A resolver whose bytes come from an in-memory map (no filesystem), driving
// a flatten of a USDA root with a USDA dependency — the wasm/HTTP shape.
static void test_resolver_layer_loader_usda_dep() {
  std::cout << "[MakeResolverLayerLoader + USDA dependency]\n";

  std::map<std::string, std::string> assets;
  assets["dep.usda"] = kBaseUSDA;

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string& asset_path, const std::string& anchor) {
        (void)anchor;
        return asset_path;  // identity: keys are asset paths
      });
  resolver.SetAssetReader([&assets](const std::string& resolved_path,
                                    std::vector<uint8_t>* out,
                                    std::string* err) {
    auto it = assets.find(resolved_path);
    if (it == assets.end()) {
      if (err) *err += "no such asset: " + resolved_path + "\n";
      return false;
    }
    out->assign(it->second.begin(), it->second.end());
    return true;
  });

  pipeline::FlattenOptions opts;
  opts.resolver = &resolver;
  opts.layer_loader = pipeline::MakeResolverLayerLoader(&resolver);
  opts.fail_on_composition_error = true;

  std::vector<uint8_t> out;
  pipeline::FlattenStats stats;
  std::string err;
  bool ok = pipeline::FlattenUSDMemoryToUSDCOwned(
      "root.usda", std::string(kRootUSDA), out, opts, &stats, &err);
  CHECK(ok, ("flatten with USDA dep succeeds: " + err).c_str());
  CHECK(!out.empty(), "flatten produced crate bytes");
  CHECK(stats.composition_errors.empty(), "no composition errors");

  if (ok && !out.empty()) {
    USDCLoadResult rr = LoadUSDCFromMemory(out.data(), out.size());
    CHECK(rr.success, "flattened crate parses back");
    if (rr.success) {
      std::shared_ptr<Layer> l = rr.stage.ReleaseRootLayer();
      l->build_path_index();
      const PrimSpec* root = l->prim_at_path("/Root");
      CHECK(root != nullptr, "composed /Root exists");
      CHECK(root && root->property_value("imported") != nullptr,
            "referenced USDA opinion composed onto /Root");
      CHECK(root && root->property_value("localOnly") != nullptr,
            "local opinion preserved");
      CHECK(l->prim_at_path("/Root/Geo") != nullptr,
            "referenced USDA subtree grafted");
    }
  }
}

// Crate roots keep the lazy passthrough path.
static void test_memory_flatten_crate_root() {
  std::cout << "[FlattenUSDMemoryToUSDCOwned crate root]\n";
  std::vector<uint8_t> crate = MakeCrateBytes(kBaseUSDA);
  CHECK(!crate.empty(), "crate fixture built");

  std::vector<uint8_t> out;
  std::string err;
  bool ok = pipeline::FlattenUSDMemoryToUSDCOwned(
      "root.usdc",
      std::string(reinterpret_cast<const char*>(crate.data()), crate.size()),
      out, {}, nullptr, &err);
  CHECK(ok, ("crate root flatten succeeds: " + err).c_str());
  if (ok) {
    USDCLoadResult rr = LoadUSDCFromMemory(out.data(), out.size());
    CHECK(rr.success, "crate root output parses back");
    if (rr.success) {
      std::shared_ptr<Layer> l = rr.stage.ReleaseRootLayer();
      l->build_path_index();
      CHECK(l->prim_at_path("/Base/Geo") != nullptr,
            "crate root prims survive");
    }
  }
}

// Missing dependency must fail (not silently emit partial output) when
// fail_on_composition_error is set — the resumable/need-layer contract.
static void test_missing_dep_fails() {
  std::cout << "[missing dependency fails hard]\n";

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string& asset_path, const std::string&) {
        return asset_path;
      });
  resolver.SetAssetReader([](const std::string& resolved_path,
                             std::vector<uint8_t>*, std::string* err) {
    if (err) *err += "NEED_LAYER:" + resolved_path + "\n";
    return false;
  });

  pipeline::FlattenOptions opts;
  opts.resolver = &resolver;
  opts.layer_loader = pipeline::MakeResolverLayerLoader(&resolver);
  opts.fail_on_composition_error = true;

  std::vector<uint8_t> out;
  std::string err;
  bool ok = pipeline::FlattenUSDMemoryToUSDCOwned(
      "root.usda", std::string(kRootUSDA), out, opts, nullptr, &err);
  CHECK(!ok, "flatten fails when dependency bytes are unavailable");
  CHECK(err.find("dep.usda") != std::string::npos ||
            err.find("NEED_LAYER") != std::string::npos,
        "error names the missing layer");
}


// Suffix fallback: an asset authored with another machine's absolute prefix
// resolves by retrying progressively shorter path suffixes against the
// search paths (and stays a miss when the fallback is disabled).
static void test_suffix_fallback_resolution() {
  std::cout << "[AssetResolver suffix fallback]\n";

  namespace fs = std::filesystem;
  fs::path root = fs::path("suffix-fallback-assets");
  fs::create_directories(root / "textures");
  {
    std::ofstream f((root / "textures" / "wood.png").string(),
                    std::ios::binary);
    f << "png";
  }

  AssetResolver resolver;
  resolver.AddSearchPath(root.string());

  ResolvedAsset hit =
      resolver.Resolve("F:/projects/other-machine/textures/wood.png", "");
  CHECK(hit.exists, "foreign absolute path rehomes via suffix fallback");
  CHECK(hit.resolved_path.find("textures/wood.png") != std::string::npos,
        "resolved path points at the rehomed file");
  CHECK(hit.original_path == "F:/projects/other-machine/textures/wood.png",
        "original path preserved");

  ResolvedAsset strict_miss = resolver.Resolve(
      "F:/projects/other-machine/textures/wood.png", "",
      /*allow_suffix_fallback=*/false);
  CHECK(!strict_miss.exists,
        "per-call strict resolution disables suffix fallback");

  ResolverConfig cfg = resolver.GetConfig();
  cfg.enable_suffix_fallback = false;
  resolver.SetConfig(cfg);
  ResolvedAsset miss =
      resolver.Resolve("F:/projects/other-machine/textures/wood.png", "");
  CHECK(!miss.exists, "fallback disabled -> foreign path stays unresolved");

  fs::remove_all(root);
}

static void test_recursive_resolution_and_identifiers() {
  std::cout << "[AssetResolver recursive search + identifiers]\n";

  namespace fs = std::filesystem;
  const fs::path root = fs::path("recursive-resolver-assets");
  fs::create_directories(root / "a" / "nested");
  {
    std::ofstream f((root / "a" / "nested" / "material.usda").string());
    f << "#usda 1.0\n";
  }

  ResolverConfig cfg;
  cfg.working_directory = root.string();
  cfg.search_paths = {root.string()};
  cfg.search_recursively = true;
  cfg.enable_suffix_fallback = false;
  AssetResolver resolver(cfg);
  const ResolvedAsset hit = resolver.Resolve("material.usda");
  CHECK(hit.exists, "recursive search finds a nested asset");
  CHECK(hit.resolved_path.find("a/nested/material.usda") != std::string::npos,
        "recursive search returns the nested path");

  cfg.search_recursively = false;
  resolver.SetConfig(cfg);
  CHECK(!resolver.Resolve("material.usda").exists,
        "recursive search obeys its configuration switch");

  const std::string id_a = resolver.CreateIdentifier(
      "future.usda", (root / "layerA" / "root.usda").string());
  const std::string id_b = resolver.CreateIdentifier(
      "future.usda", (root / "layerB" / "root.usda").string());
  CHECK(id_a != id_b,
        "unresolved relative identifiers retain their anchor context");
  CHECK(!AssetResolver::IsPackagePath("pkg.usdz[item.usda]trailing"),
        "malformed package identifiers are rejected");

  fs::remove_all(root);
}

static void test_scheme_and_anonymous_assets() {
  std::cout << "[AssetResolver schemes + anonymous assets]\n";

  AssetResolver resolver;
  const std::string dep(kBaseUSDA);
  resolver.RegisterScheme(
      "StUdIo",
      [](const std::string& identifier, const std::string&) {
        return identifier;
      },
      [&dep](const std::string& identifier, std::vector<uint8_t>* out,
             std::string*) {
        if (identifier != "studio:dep.usda") return false;
        out->assign(dep.begin(), dep.end());
        return true;
      });
  CHECK(resolver.HasScheme("studio"),
        "scheme registration is ASCII case-insensitive");
  const ResolvedAsset scheme_hit = resolver.Resolve("studio:dep.usda");
  CHECK(scheme_hit.exists && scheme_hit.resolved_path == "studio:dep.usda",
        "registered scheme resolves without filesystem normalization");
  CHECK(resolver.CreateIdentifier("missing:asset", "/tmp/root.usda") ==
            "missing:asset",
        "unregistered scheme identifier does not become a filesystem path");

  std::vector<uint8_t> bytes;
  std::string err;
  CHECK(resolver.ReadAsset(scheme_hit.resolved_path, &bytes, &err) &&
            std::string(bytes.begin(), bytes.end()) == dep,
        "registered scheme reader returns bytes");

  const std::string anonymous = resolver.RegisterMemoryAsset(
      "", std::vector<uint8_t>(dep.begin(), dep.end()));
  CHECK(AssetResolver::GetIdentifierScheme(anonymous) == "usd-anon" &&
            resolver.Resolve(anonymous).exists,
        "empty memory identifier creates a resolvable usd-anon asset");
  bytes.clear();
  CHECK(resolver.ReadAsset(anonymous, &bytes, &err) &&
            std::string(bytes.begin(), bytes.end()) == dep,
        "anonymous asset bytes are readable");
  AssetResolver copied_resolver = resolver;
  AssetResolver moved_resolver = std::move(copied_resolver);
  bytes.clear();
  CHECK(moved_resolver.ReadAsset(anonymous, &bytes, &err) &&
            std::string(bytes.begin(), bytes.end()) == dep &&
            moved_resolver.HasScheme("studio"),
        "resolver copy/move preserves registered schemes and memory assets");

  const std::string owned_view_id = resolver.RegisterMemoryAsset(
      "studio:owned-view", std::vector<uint8_t>{4, 5, 6});
  const auto owned_view = resolver.GetMemoryAssetView(owned_view_id);
  CHECK(owned_view && *owned_view == std::vector<uint8_t>({4, 5, 6}),
        "memory asset views share the registered payload without copying");
  CHECK(resolver.UnregisterMemoryAsset(owned_view_id) && owned_view &&
            *owned_view == std::vector<uint8_t>({4, 5, 6}),
        "owning resolver views remain valid after unregister");
  const auto shared_payload = std::make_shared<const std::vector<uint8_t>>(
      std::vector<uint8_t>{7, 8, 9});
  AssetResolver imported_resolver;
  const std::string imported_id = imported_resolver.RegisterMemoryAssetView(
      "studio:shared-view", shared_payload);
  CHECK(imported_id == "studio:shared-view" &&
            imported_resolver.GetMemoryAssetView(imported_id) == shared_payload,
        "resolver imports an immutable payload view without copying");
  imported_resolver.ClearMemoryAssets();
  CHECK(!imported_resolver.Resolve(imported_id).exists &&
            *shared_payload == std::vector<uint8_t>({7, 8, 9}),
        "clearing imported resolver keys preserves the external owner");

  std::string root =
      "#usda 1.0\ndef Xform \"Root\" (references = @" + anonymous +
      "@</Base>) {}\n";
  pipeline::FlattenOptions opts;
  opts.resolver = &resolver;
  opts.layer_loader = pipeline::MakeResolverLayerLoader(&resolver);
  opts.fail_on_composition_error = true;
  std::vector<uint8_t> flattened;
  err.clear();
  CHECK(pipeline::FlattenUSDMemoryToUSDCOwned(
            "root.usda", std::move(root), flattened, opts, nullptr, &err),
        ("anonymous referenced layer composes: " + err).c_str());

  CHECK(resolver.UnregisterMemoryAsset(anonymous) &&
            !resolver.Resolve(anonymous).exists,
        "unregistered anonymous asset no longer resolves");
  CHECK(resolver.UnregisterScheme("STUDIO") && !resolver.HasScheme("studio"),
        "scheme handler can be removed");
}


// External MaterialX: a reference to @mat.mtlx@</MaterialX/...> composes a
// skeletal /MaterialX layer synthesized from the XML document.
static void test_mtlx_reference_composition() {
  std::cout << "[.mtlx reference composition]\n";

  static const char* kMtlxDoc = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <nodegraph name="NG_wood">
    <image name="albedo" type="color3">
      <input name="file" type="filename" value="textures/wood.jpg"
             colorspace="srgb_texture" />
    </image>
    <output name="base_color" type="color3" nodename="albedo" />
  </nodegraph>
  <standard_surface name="SR_wood" type="surfaceshader">
    <input name="base_color" type="color3"
           nodegraph="NG_wood" output="base_color" />
    <input name="specular_roughness" type="float" value="0.35" />
  </standard_surface>
  <surfacematerial name="M_wood" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_wood" />
  </surfacematerial>
</materialx>
)";

  static const char* kRootMtlxUSDA = R"(#usda 1.0
(
    defaultPrim = "Scene"
)

def Xform "Scene"
{
    def "WoodMat" (
        references = @mat.mtlx@</MaterialX/Materials/M_wood>
    )
    {
    }

    def "WoodShader" (
        references = @mat.mtlx@</MaterialX/Shaders/SR_wood>
    )
    {
    }
}
)";

  std::map<std::string, std::string> assets;
  assets["mat.mtlx"] = kMtlxDoc;

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string& asset_path, const std::string& anchor) {
        (void)anchor;
        return asset_path;
      });
  resolver.SetAssetReader([&assets](const std::string& resolved_path,
                                    std::vector<uint8_t>* out,
                                    std::string* err) {
    auto it = assets.find(resolved_path);
    if (it == assets.end()) {
      if (err) *err += "no such asset: " + resolved_path + "\n";
      return false;
    }
    out->assign(it->second.begin(), it->second.end());
    return true;
  });

  pipeline::FlattenOptions opts;
  opts.resolver = &resolver;
  opts.layer_loader = pipeline::MakeResolverLayerLoader(&resolver);
  opts.fail_on_composition_error = true;

  std::vector<uint8_t> out;
  pipeline::FlattenStats stats;
  std::string err;
  bool ok = pipeline::FlattenUSDMemoryToUSDCOwned(
      "root.usda", std::string(kRootMtlxUSDA), out, opts, &stats, &err);
  CHECK(ok, ("flatten with .mtlx reference succeeds: " + err).c_str());
  CHECK(stats.composition_errors.empty(), "no composition errors");

  if (ok && !out.empty()) {
    USDCLoadResult rr = LoadUSDCFromMemory(out.data(), out.size());
    CHECK(rr.success, "flattened crate parses back");
    if (rr.success) {
      std::shared_ptr<Layer> l = rr.stage.ReleaseRootLayer();
      l->build_path_index();
      const PrimSpec* mat = l->prim_at_path("/Scene/WoodMat");
      CHECK(mat != nullptr, "mtlx material prim composed");
      if (mat) {
        CHECK(mat->type_name() == "Material", "composed prim is a Material");
        const Value* version = mat->property_value("config:mtlx:version");
        CHECK(version && version->as_string() &&
                  *version->as_string() == "1.38",
              "config:mtlx:version composed from the document");
        const std::vector<Path>* surface =
            mat->relationship("mtlx:surface:source");
        CHECK(surface && surface->size() == 1 &&
                  (*surface)[0].str() == "/Scene/WoodMat/SR_wood",
              "material selects its localized terminal shader");
      }
      const PrimSpec* local_shader =
          l->prim_at_path("/Scene/WoodMat/SR_wood");
      CHECK(local_shader != nullptr, "localized terminal shader composed");
      if (local_shader) {
        const std::vector<Path>* base =
            local_shader->connection("inputs:base_color");
        CHECK(base && base->size() == 1 &&
                  (*base)[0].str() ==
                      "/Scene/WoodMat/NG_wood.outputs:base_color",
              "localized shader input targets its NodeGraph output");
      }
      const PrimSpec* graph = l->prim_at_path("/Scene/WoodMat/NG_wood");
      CHECK(graph != nullptr, "localized NodeGraph composed");
      if (graph) {
        const std::vector<Path>* base =
            graph->connection("outputs:base_color");
        CHECK(base && base->size() == 1 &&
                  (*base)[0].str() ==
                      "/Scene/WoodMat/NG_wood/albedo.outputs:out",
              "NodeGraph output targets its localized image node");
      }
      const PrimSpec* image =
          l->prim_at_path("/Scene/WoodMat/NG_wood/albedo");
      CHECK(image != nullptr, "localized MaterialX image node composed");
      if (image) {
        const Value* file = image->property_value("inputs:file");
        const std::string* path = file ? file->as_asset_path() : nullptr;
        CHECK(path && *path == "textures/wood.jpg",
              "MaterialX image filename remains layer-relative");
      }
      const PrimSpec* shader = l->prim_at_path("/Scene/WoodShader");
      CHECK(shader != nullptr, "mtlx shader prim composed");
      if (shader) {
        const Value* id = shader->property_value("info:id");
        CHECK(id && id->as_token() &&
                  *id->as_token() == "MtlxAutodeskStandardSurface",
              "standard_surface info:id mapped");
        const Value* rough =
            shader->property_value("inputs:specular_roughness");
        const float* rf = rough ? rough->as_float() : nullptr;
        CHECK(rf && std::fabs(*rf - 0.35f) < 0.001f,
              "scalar shader input composed");
      }
    }
  }
}

int main() {
  test_shared_asset_payload_budget();
  test_usda_progress_and_cancellation();
  test_crate_and_usdz_file_size_limits();
  test_writer_memory_estimate_counts_string_payloads();
  test_load_layer_from_memory_dispatch();
  test_suffix_fallback_resolution();
  test_recursive_resolution_and_identifiers();
  test_scheme_and_anonymous_assets();
  test_mtlx_reference_composition();
  test_resolver_layer_loader_usda_dep();
  test_memory_flatten_crate_root();
  test_missing_dep_fails();

  if (g_fail) {
    std::cerr << "\n" << g_fail << " memory-flatten check(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll memory-flatten checks passed.\n";
  return 0;
}
