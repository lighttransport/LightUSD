// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// AssetResolver context, scoped-cache, package-resolver and new-asset tests.

#include "next/resolver/asset-resolver.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace lightusd::next;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::cerr << "  FAIL: " << msg << "\n";                               \
      ++g_fail;                                                             \
    } else {                                                                \
      std::cout << "  ok: " << msg << "\n";                                 \
    }                                                                       \
  } while (0)

namespace {

std::filesystem::path MakeTempRoot(const char* tag) {
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
                        ("lightusd-next-resolver-" + std::string(tag) + "-" +
                         std::to_string(stamp));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

std::string Normalized(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path.string(), std::ios::binary);
  out << text;
}

void TestResolveForNewAsset() {
  namespace fs = std::filesystem;
  const fs::path root = MakeTempRoot("new-asset");
  fs::create_directories(root / "look");

  ResolverConfig cfg;
  cfg.working_directory = root.string();
  AssetResolver resolver(cfg);

  const ResolvedAsset anchored = resolver.ResolveForNewAsset(
      "sub/new.usda", (root / "look" / "layer.usda").string());
  CHECK(anchored.resolved_path == Normalized(root / "look" / "sub" / "new.usda"),
        "new relative asset anchors to the referring layer directory");
  CHECK(!anchored.exists, "new asset is reported as non-existent");

  const ResolvedAsset working = resolver.ResolveForNewAsset("sub/new.usda");
  CHECK(working.resolved_path == Normalized(root / "sub" / "new.usda"),
        "new relative asset without an anchor uses the working directory");

  const ResolvedAsset absolute =
      resolver.ResolveForNewAsset((root / "abs" / "x.usda").string());
  CHECK(absolute.resolved_path == Normalized(root / "abs" / "x.usda") &&
            !absolute.exists,
        "new absolute asset is normalized without requiring existence");

  const ResolvedAsset scheme = resolver.ResolveForNewAsset("studio:new.usda");
  CHECK(scheme.resolved_path == "studio:new.usda" && !scheme.exists,
        "new scheme identifier retains scheme resolution semantics");

  const ResolvedAsset package = resolver.ResolveForNewAsset(
      "new.usda", "pkg.usdz[dir/root.usda]");
  CHECK(package.is_package &&
            package.asset_in_package == "dir/new.usda" &&
            package.resolved_path == "pkg.usdz[dir/new.usda]",
        "new asset inside a package anchor stays in the same package");

  const ResolvedAsset new_package = resolver.ResolveForNewAsset(
      "pkg.usdz[dir/new.usda]", (root / "look" / "layer.usda").string());
  CHECK(new_package.is_package &&
            new_package.resolved_path ==
                Normalized(root / "look" / "pkg.usdz") + "[dir/new.usda]",
        "new package container anchors to the referring layer directory");

  cfg.allow_parent_paths = false;
  ResolverConfig restricted = cfg;
  AssetResolver restricted_resolver(restricted);
  CHECK(restricted_resolver.ResolveForNewAsset("../escape.usda").resolved_path
            .empty(),
        "new asset parent escape is rejected when parent paths are disabled");

  fs::remove_all(root);
}

void TestContexts() {
  namespace fs = std::filesystem;
  const fs::path root = MakeTempRoot("context");
  fs::create_directories(root / "a");
  fs::create_directories(root / "b");
  WriteText(root / "a" / "tex.usda", "#usda 1.0\n");
  WriteText(root / "b" / "tex.usda", "#usda 1.0\n");

  ResolverConfig cfg;
  cfg.working_directory = root.string();
  cfg.search_paths = {(root / "a").string()};
  AssetResolver resolver(cfg);

  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "a" / "tex.usda"),
        "configured search path resolves before contexts are bound");

  const ResolverContext ctx_b =
      resolver.CreateContextFromString((root / "b").string());
  CHECK(ctx_b.search_paths.size() == 1 && !ctx_b.is_default,
        "context string parses into search paths");

  void* b_binding = nullptr;
  resolver.BindContext(ctx_b, &b_binding);
  CHECK(resolver.GetCurrentContext().context_string == (root / "b").string(),
        "bound context is returned by GetCurrentContext");
  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "b" / "tex.usda"),
        "bound context search path overrides configured search paths");

  const ResolverContext ctx_a =
      resolver.CreateContextFromString((root / "a").string());
  resolver.BindContext(ctx_a);
  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "a" / "tex.usda"),
        "nested context binding overrides the outer binding");
  resolver.UnbindContext(ctx_a);
  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "b" / "tex.usda"),
        "unbinding a nested context restores the outer binding");
  resolver.UnbindContext(ctx_b, &b_binding);
  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "a" / "tex.usda"),
        "unbinding restores the configured search paths");

  void* first = nullptr;
  void* second = nullptr;
  resolver.BindContext(ctx_a, &first);
  resolver.BindContext(ctx_b, &second);
  resolver.UnbindContext(ctx_a, &first);
  CHECK(resolver.GetCurrentContext().context_string == (root / "b").string(),
        "out-of-order unbind removes only the requested binding");
  CHECK(resolver.UnbindContext(ctx_b, &second),
        "unbinding removes the requested binding");
  CHECK(!resolver.UnbindContext(ctx_b, &second),
        "unbinding an already-removed binding reports no removal");

  // Bindings are thread-local: another thread cannot unbind them.
  {
    void* third = nullptr;
    resolver.BindContext(ctx_a, &third);
    void* binding_handle = third;
    std::thread worker([&resolver, &ctx_a, binding_handle]() {
      void* local = binding_handle;
      CHECK(!resolver.UnbindContext(ctx_a, &local),
            "a foreign thread cannot unbind another thread's binding");
    });
    worker.join();
    CHECK(resolver.GetCurrentContext().context_string == ctx_a.context_string,
          "a failed cross-thread unbind leaves the binding intact");
    resolver.UnbindContext(ctx_a, &third);
  }
  CHECK(resolver.GetCurrentContext().is_default,
        "unbinding the last context returns to the default context");

  ResolverConfig persistent = cfg;
  persistent.context = ctx_b;
  AssetResolver persistent_resolver(persistent);
  CHECK(persistent_resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "b" / "tex.usda"),
        "persistent ResolverConfig context applies without binding");

  // Scheme-specific contexts reach context-aware scheme handlers.
  {
    AssetResolver scheme_resolver;
    scheme_resolver.RegisterScheme(
        "studio", ContextResolverCallback(
                      [](const std::string& identifier,
                         const std::string&, const std::string& context) {
                        return (context.empty() ? std::string("base")
                                                : context) +
                               ":" + identifier;
                      }));
    ResolverContext scheme_ctx = scheme_resolver.CreateDefaultContext();
    scheme_ctx.SetSchemeContext("StUdIo", "scheme-ctx");
    void* scheme_binding = nullptr;
    scheme_resolver.BindContext(scheme_ctx, &scheme_binding);
    CHECK(scheme_resolver.Resolve("studio:asset.usda").resolved_path ==
              "scheme-ctx:studio:asset.usda",
          "scheme resolver receives its bound scheme context");
    scheme_resolver.UnbindContext(scheme_ctx, &scheme_binding);
    CHECK(scheme_resolver.Resolve("studio:asset.usda").resolved_path ==
              "base:studio:asset.usda",
          "scheme resolver falls back when no scheme context is bound");
  }

  // A callback may unbind its own context; the context string passed to it
  // must remain valid for the duration of the callback.
  {
    AssetResolver reentrant;
    void* reentrant_binding = nullptr;
    reentrant.RegisterScheme(
        "reentrant",
        ContextResolverCallback(
            [&](const std::string& identifier, const std::string&,
                const std::string& context) {
              reentrant.UnbindContext(ResolverContext{}, &reentrant_binding);
              return context + ":" + identifier;
            }));
    ResolverContext reentrant_ctx = reentrant.CreateDefaultContext();
    reentrant_ctx.SetSchemeContext("reentrant", "live");
    reentrant.BindContext(reentrant_ctx, &reentrant_binding);
    CHECK(reentrant_binding != nullptr,
          "reentrant scheme context is bound before resolution");
    CHECK(reentrant.Resolve("reentrant:asset.usda").resolved_path ==
              "live:reentrant:asset.usda",
          "scheme context survives re-entrant unbind during the callback");
    CHECK(reentrant_binding == nullptr,
          "callback unbind clears its binding token");
  }

  const ResolverContext default_ctx = resolver.CreateDefaultContext();
  resolver.BindContext(default_ctx);
  CHECK(resolver.Resolve("tex.usda").resolved_path ==
            Normalized(root / "a" / "tex.usda"),
        "binding a default context falls back to configured search paths");
  resolver.UnbindContext(default_ctx);

  fs::remove_all(root);
}

void TestPathNormalization() {
  // The fast path must agree with the full normalization for every shape.
  struct Case {
    const char* input;
    const char* expected;
  };
  const Case cases[] = {
      {"a/b", "a/b"},
      {"/a/b", "/a/b"},
      {"a//b", "a/b"},
      {"a/./b", "a/b"},
      {"a/../b", "b"},
      {"./a", "a"},
      {"a/", "a"},
      {".", "."},
      {"..", ".."},
      {"/a/../b", "/b"},
      {"/a/b/../../c", "/c"},
      {"a/b/..", "a"},
  };
  for (const Case& c : cases) {
    CHECK(AssetResolver::NormalizePath(c.input) == c.expected,
          "normalized path keeps its meaning");
  }
}

void TestScopedCache() {
  int calls = 0;
  AssetResolver resolver;
  resolver.SetCustomResolver(
      [&calls](const std::string& asset_path, const std::string&) {
        ++calls;
        return std::string("virtual:") + asset_path;
      });

  resolver.Resolve("x.usda");
  resolver.Resolve("x.usda");
  CHECK(calls == 2, "resolution is not cached without a scope");

  {
    ResolverScopedCache cache(&resolver);
    CHECK(resolver.Resolve("x.usda").resolved_path == "virtual:x.usda",
          "scoped resolution returns the custom-resolver result");
    resolver.Resolve("x.usda");
    CHECK(calls == 3, "scoped cache reuses the first result");
  }

  resolver.Resolve("x.usda");
  CHECK(calls == 4, "cache is discarded when the scope ends");

  {
    ResolverScopedCache cache(&resolver);
    resolver.Resolve("y.usda");
    const ResolverContext ctx = resolver.CreateContextFromString("ctx");
    void* binding = nullptr;
    resolver.BindContext(ctx, &binding);
    resolver.Resolve("y.usda");
    resolver.UnbindContext(ctx, &binding);
    resolver.Resolve("y.usda");
    CHECK(calls == 6,
          "context changes participate in the scoped-cache key");
  }

  resolver.Resolve("x.usda");
  CHECK(calls == 7, "scoped cache does not leak across scopes");
}

void TestPackageResolvers() {
  namespace fs = std::filesystem;
  const fs::path root = MakeTempRoot("package");

  AssetResolver resolver;
  int calls = 0;
  std::string last_package;
  resolver.RegisterPackageResolver(
      ".PKG", [&](const std::string& package_path, const std::string& asset_path,
                  std::string* resolved) {
        ++calls;
        last_package = package_path;
        *resolved = "virtual:" + package_path + "|" + asset_path;
        return true;
      });
  CHECK(resolver.HasUserCallbacks(),
        "registered package resolver counts as a user callback");

  const ResolvedAsset virtual_package =
      resolver.Resolve("bundle.pkg[dir/entry.usda]");
  CHECK(virtual_package.is_package && virtual_package.exists,
        "virtual package resolver resolves a missing package file");
  CHECK(virtual_package.resolved_path ==
            "virtual:bundle.pkg|dir/entry.usda",
        "package callback receives authored package path and normalized entry");
  CHECK(virtual_package.package_path == "bundle.pkg" &&
            virtual_package.asset_in_package == "dir/entry.usda",
        "package fields retain the authored container and entry");
  CHECK(calls == 1 && last_package == "bundle.pkg",
        "package callback is invoked once");

  const ResolvedAsset new_virtual =
      resolver.ResolveForNewAsset("bundle.pkg[dir/new.usda]");
  CHECK(new_virtual.is_package && !new_virtual.exists &&
            new_virtual.resolved_path ==
                "virtual:bundle.pkg|dir/new.usda",
        "new package asset consults the package resolver");
  const ResolvedAsset new_anchor =
      resolver.ResolveForNewAsset("new.usda", "bundle.pkg[dir/root.usda]");
  CHECK(new_anchor.is_package && !new_anchor.exists &&
            new_anchor.resolved_path ==
                "virtual:bundle.pkg|dir/new.usda",
        "new package-anchored asset consults the package resolver");

  resolver.SetAssetReader(
      [&virtual_package](const std::string& identifier,
                         std::vector<uint8_t>* out, std::string*) {
        if (identifier != virtual_package.resolved_path) return false;
        out->assign({'u', 's', 'd'});
        return true;
      });
  std::vector<uint8_t> bytes;
  std::string err;
  CHECK(resolver.ReadAsset(virtual_package.resolved_path, &bytes, &err) &&
            bytes == std::vector<uint8_t>({'u', 's', 'd'}),
        "asset reader reads a package-resolver key");

  CHECK(resolver.UnregisterPackageResolver(".pkg") &&
            !resolver.UnregisterPackageResolver("pkg"),
        "package resolver unregister is case-insensitive and reports presence");
  CHECK(!resolver.Resolve("bundle.pkg[dir/entry.usda]").exists,
        "package resolution falls back after unregister");

  // Existing package files are resolved first; the callback then sees the
  // resolved container path.
  WriteText(root / "bundle2.pkg", "container");
  ResolverConfig cfg;
  cfg.working_directory = root.string();
  AssetResolver file_resolver(cfg);
  int file_calls = 0;
  file_resolver.RegisterPackageResolver(
      "pkg", [&file_calls](const std::string& package_path,
                           const std::string& asset_path, std::string* resolved) {
        ++file_calls;
        if (!std::filesystem::path(package_path).is_absolute()) return false;
        *resolved = "resolved-container:" + asset_path;
        return true;
      });
  const ResolvedAsset file_package =
      file_resolver.Resolve("bundle2.pkg[entry.usda]");
  CHECK(file_package.exists &&
            file_package.resolved_path == "resolved-container:entry.usda" &&
            file_package.package_path == Normalized(root / "bundle2.pkg"),
        "package callback receives the resolved container path");
  CHECK(file_calls == 2,
        "package callback is retried with the resolved container path");

  AssetResolver copied = file_resolver;
  CHECK(copied.Resolve("bundle2.pkg[entry.usda]").resolved_path ==
            "resolved-container:entry.usda",
        "copy preserves registered package resolvers");
  AssetResolver moved = std::move(copied);
  CHECK(moved.Resolve("bundle2.pkg[entry.usda]").resolved_path ==
            "resolved-container:entry.usda",
        "move preserves registered package resolvers");

  fs::remove_all(root);
}

void TestAliases() {
  AssetResolver resolver;

  const std::vector<uint8_t> payload = {'t', 'e', 'x'};
  resolver.RegisterMemoryAsset("tex.png", payload);
  CHECK(resolver.SetAlias("a.png", "tex.png"), "alias registration succeeds");
  CHECK(resolver.SetAlias("b.png", "a.png"), "alias chains are accepted");
  CHECK(!resolver.SetAlias("tex.png", "a.png"), "alias cycle is rejected");
  CHECK(!resolver.SetAlias("a.png", "a.png"), "self alias is rejected");
  CHECK(!resolver.SetAlias("", "tex.png"), "empty alias is rejected");

  const ResolvedAsset a = resolver.Resolve("a.png");
  CHECK(a.exists && a.resolved_path == "a.png" && a.canonical_path == "tex.png",
        "alias keeps its authored identity and reports the canonical target");

  const ResolvedAsset b = resolver.Resolve("b.png");
  CHECK(b.exists && b.canonical_path == "tex.png",
        "alias chains canonicalize to the final target");

  const ResolvedAsset plain = resolver.Resolve("tex.png");
  CHECK(plain.canonical_path == "tex.png",
        "non-alias resolution reports itself as canonical");

  std::vector<uint8_t> bytes;
  std::string error;
  CHECK(resolver.ReadAsset("a.png", &bytes, &error) && bytes == payload,
        "ReadAsset follows an alias identifier");
  CHECK(resolver.ReadAsset(a, &bytes, &error) && bytes == payload,
        "ReadAsset(ResolvedAsset) uses the canonical path");
  CHECK(resolver.GetMemoryAssetView("b.png") != nullptr,
        "memory views follow alias chains");

  // Copying the resolver preserves the alias table.
  AssetResolver copy(resolver);
  const ResolvedAsset copied = copy.Resolve("b.png");
  CHECK(copied.exists && copied.canonical_path == "tex.png",
        "copy preserves aliases");

  CHECK(resolver.UnsetAlias("a.png"), "alias removal succeeds");
  CHECK(!resolver.UnsetAlias("a.png"), "removing a missing alias fails");
  const ResolvedAsset dangling = resolver.Resolve("b.png");
  CHECK(!dangling.exists, "a dangling alias no longer resolves");
  resolver.ClearAliases();
  CHECK(resolver.Resolve("tex.png").canonical_path == "tex.png",
        "clearing aliases keeps direct identifiers intact");
}

void TestAssetInfoAndTimestamp() {
  namespace fs = std::filesystem;
  const fs::path root = MakeTempRoot("info");
  WriteText(root / "tex.usda", "#usda 1.0\n");
  WriteText(root / "bundle.usdz", "package");
  WriteText(root / "payload.bin", "hello");

  ResolverConfig cfg;
  cfg.working_directory = root.string();
  AssetResolver resolver(cfg);

  const ResolvedAsset resolved = resolver.Resolve("tex.usda");
  const AssetInfo info = resolver.GetAssetInfo("tex.usda", resolved);
  CHECK(info.asset_name == "tex.usda", "asset info reports the basename");
  CHECK(info.repository_path == Normalized(root),
        "asset info reports the containing directory");
  CHECK(info.exists && info.resolved_path == Normalized(root / "tex.usda"),
        "asset info carries resolution state");

  std::vector<uint8_t> bytes;
  std::string read_error;
  CHECK(resolver.ReadAsset(Normalized(root / "payload.bin"), &bytes,
                           &read_error) &&
            bytes == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}),
        "ReadAsset opens and reads a regular file from its descriptor");
  CHECK(!resolver.ReadAsset(Normalized(root), &bytes, &read_error),
        "ReadAsset rejects a directory descriptor");

  const ResolvedAsset payload = resolver.Resolve("payload.bin");
  CHECK(payload.has_file_identity,
        "resolution captures the file device/inode identity");
  CHECK(resolver.ReadAsset(payload, &bytes, &read_error) &&
            bytes == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}),
        "identity-checked ReadAsset reads the resolved file");
  CHECK(resolver.FileIdentityUnchanged(payload),
        "resolved file identity is stable while untouched");
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  fs::rename(root / "payload.bin", root / "payload.old");
  WriteText(root / "payload.bin", "replaced");
  CHECK(!resolver.FileIdentityUnchanged(payload),
        "replacing a resolved path is detected by device/inode");
  CHECK(!resolver.ReadAsset(payload, &bytes, &read_error),
        "identity-checked ReadAsset rejects a replaced path");
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  const ResolvedAsset package_resolved =
      resolver.Resolve("bundle.usdz[dir/entry.usda]");
  CHECK(package_resolved.is_package && package_resolved.has_file_identity,
        "package resolution captures the package file identity");
  CHECK(resolver.ReadAsset(package_resolved, &bytes, &read_error) &&
            bytes == std::vector<uint8_t>({'p', 'a', 'c', 'k', 'a', 'g', 'e'}),
        "ReadAsset on a package returns the package file bytes");
  fs::rename(root / "bundle.usdz", root / "bundle.old");
  WriteText(root / "bundle.usdz", "replaced package");
  CHECK(!resolver.FileIdentityUnchanged(package_resolved),
        "replacing a package file is detected by device/inode");
  CHECK(!resolver.ReadAsset(package_resolved, &bytes, &read_error),
        "identity-checked package read rejects a replaced package");
#endif

  // A memory asset registered under a path that also exists on disk must win:
  // the resolved identity check must not capture the file behind it.
  {
    const std::string dual_path = Normalized(root / "dual.usda");
    WriteText(root / "dual.usda", "#usda 1.0\n# file bytes\n");
    const std::string memory_bytes = "#usda 1.0\n# memory bytes\n";
    resolver.RegisterMemoryAsset(
        dual_path,
        std::vector<uint8_t>(memory_bytes.begin(), memory_bytes.end()));
    const ResolvedAsset dual = resolver.Resolve(dual_path);
    CHECK(dual.exists, "memory asset shadows a same-named file");
    std::vector<uint8_t> dual_bytes;
    CHECK(resolver.ReadAsset(dual, &dual_bytes, &read_error) &&
              std::string(dual_bytes.begin(), dual_bytes.end()) == memory_bytes,
          "identity-checked read returns the memory bytes, not the file");
  }

  {
    // An unresolved bare name has no known repository.
    const ResolvedAsset missing = resolver.Resolve("missing.usda");
    const AssetInfo missing_info =
        resolver.GetAssetInfo("missing.usda", missing);
    CHECK(missing_info.asset_name == "missing.usda" && !missing_info.exists,
          "unresolved asset info reports the authored name");
    CHECK(missing_info.repository_path.empty(),
          "unresolved asset info does not claim the current directory");
    // An existing relative asset reports its resolved directory.
    const ResolvedAsset relative = resolver.Resolve("tex.usda");
    const AssetInfo relative_info =
        resolver.GetAssetInfo("tex.usda", relative);
    CHECK(relative_info.exists &&
              relative_info.repository_path == Normalized(root),
          "existing relative asset reports its resolved directory");
  }

  const AssetTimestamp timestamp =
      resolver.GetModificationTimestamp("tex.usda", resolved);
  CHECK(timestamp.valid && timestamp.seconds > 0,
        "file modification timestamp is reported");
  CHECK(timestamp.nanoseconds < 1000000000ull,
        "file timestamp nanoseconds are in range");

  ResolvedAsset package;
  package.is_package = true;
  package.exists = true;
  package.package_path = Normalized(root / "bundle.usdz");
  package.asset_in_package = "dir/entry.usda";
  package.resolved_path = package.package_path + "[dir/entry.usda]";
  const AssetInfo package_info = resolver.GetAssetInfo(
      "bundle.usdz[dir/entry.usda]", package);
  CHECK(package_info.asset_name == "entry.usda",
        "package asset info reports the entry basename");
  CHECK(package_info.repository_path == Normalized(root),
        "package asset info reports the package directory");

  const AssetTimestamp package_timestamp =
      resolver.GetModificationTimestamp("bundle.usdz[dir/entry.usda]", package);
  CHECK(package_timestamp.valid &&
            package_timestamp.nanoseconds < 1000000000ull,
        "package timestamp uses the package file");

  ResolvedAsset package_without_fields;
  package_without_fields.is_package = true;
  package_without_fields.exists = true;
  package_without_fields.resolved_path = Normalized(root / "bundle.usdz") +
                                         "[dir/entry.usda]";
  const AssetInfo fallback_info = resolver.GetAssetInfo(
      package_without_fields.resolved_path, package_without_fields);
  CHECK(!fallback_info.asset_name.empty() &&
            fallback_info.asset_name == "entry.usda",
        "package asset info parses the identifier when fields are omitted");

  ResolvedAsset forced_package;
  forced_package.is_package = true;
  forced_package.exists = true;
  forced_package.resolved_path = "dir/forced.usda";
  const AssetInfo forced_info =
      resolver.GetAssetInfo("dir/forced.usda", forced_package);
  CHECK(forced_info.asset_name == "forced.usda" &&
            forced_info.repository_path == "dir",
        "forced package info falls back to a real repository directory");

  ResolvedAsset bare_package;
  bare_package.is_package = true;
  bare_package.exists = true;
  bare_package.resolved_path = "forced.usda";
  const AssetInfo bare_info =
      resolver.GetAssetInfo("forced.usda", bare_package);
  CHECK(bare_info.repository_path.empty(),
        "forced package without a package file does not invent '.' as a repo");

  ResolvedAsset identifier_only;
  identifier_only.exists = true;
  identifier_only.resolved_path = Normalized(root / "bundle.usdz") +
                                  "[dir/entry.usda]";
  const AssetTimestamp identifier_timestamp = resolver.GetModificationTimestamp(
      identifier_only.resolved_path, identifier_only);
  CHECK(identifier_timestamp.valid,
        "timestamp parses a package identifier without package fields");

  fs::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "[AssetResolver new-asset resolution]\n";
  TestResolveForNewAsset();
  std::cout << "[AssetResolver contexts]\n";
  TestContexts();
  std::cout << "[AssetResolver scoped cache]\n";
  TestPathNormalization();
  TestScopedCache();
  std::cout << "[AssetResolver package resolvers]\n";
  TestPackageResolvers();
  std::cout << "[AssetResolver asset info and timestamps]\n";
  TestAssetInfoAndTimestamp();
  std::cout << "[AssetResolver aliases]\n";
  TestAliases();

  if (g_fail) {
    std::cerr << "\n" << g_fail << " resolver check(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll resolver checks passed.\n";
  return 0;
}
