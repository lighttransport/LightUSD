// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Replays minimized fuzzer-found crash inputs through the next USDC reader.
// Each must load-or-reject cleanly (no crash/UB). Run under the ASAN/UBSAN
// build (build/next-asan or -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined) to
// catch the original out-of-bounds / null-memcpy / shift-UB regressions; in a
// plain build the heap-overflow case still aborts on its own.
//
// Inputs live in tests/core/crash_regressions/usdc_*.bin and are stored WITHOUT
// the 8-byte "PXR-USDC" magic (which the fuzz harness prepends); we re-prepend
// it here.

#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>

#include "reader/usdc-reader.hh"
#include "reader/usda-reader.hh"
#include "../../src/pcp/layer-registry.hh"
#include "layer/layer.hh"
#include <memory>

#ifndef CRASH_REGRESSION_DIR
#define CRASH_REGRESSION_DIR "."
#endif

using namespace lightusd;

static bool read_file(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  out.resize(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  return static_cast<bool>(f.read(&out[0], static_cast<std::streamsize>(out.size())));
}

int main() {
  const std::string dir = CRASH_REGRESSION_DIR;
  DIR *d = opendir(dir.c_str());
  if (!d) {
    std::printf("crash_regressions dir not found: %s (skipping)\n", dir.c_str());
    return 0;
  }

  int replayed = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;

    std::string body;
    if (!read_file(dir + "/" + name, body)) continue;

    std::string buf("PXR-USDC");
    buf += body;

    USDCLoadOptions opts;
    opts.crate_options.max_array_elements = 1u << 20;
    opts.crate_options.max_tokens = 1u << 18;
    opts.crate_options.max_strings = 1u << 18;
    opts.crate_options.max_fields = 1u << 18;
    opts.crate_options.max_specs = 1u << 18;
    opts.crate_options.max_paths = 1u << 18;

    // Must return (success or graceful failure), never crash / trip a sanitizer.
    USDCLoadResult r = LoadUSDCFromMemory(
        reinterpret_cast<const uint8_t *>(buf.data()), buf.size(), opts);
    (void)r;
    std::printf("  replayed %s\n", name.c_str());
    ++replayed;
  }
  closedir(d);

  std::printf("crash regressions: %d input(s) replayed, no crash.\n", replayed);

  // Regression: a DIRECTORY opens successfully with std::ifstream on POSIX and
  // reports tellg() == LLONG_MAX. Every "read the whole file" path fed that
  // straight into std::string(n, '\0') or vector::resize(n), which throws an
  // uncaught std::length_error -- terminating the process. A USD file
  // referencing `@./somedir.mtlx@` was enough; fuzz_core_compose found it.
  //
  // Every cap that should have caught it was conditional (`if (max_* > 0)`),
  // so the default configuration was unguarded. Each reader must now reject a
  // directory cleanly instead of dying.
  {
    const std::string dirpath = dir;  // a real directory
    int checked = 0;

    {
      USDCLoadOptions o;
      USDCLoadResult r = LoadUSDCFromFile(dirpath, o);
      if (r.success) { std::printf("FAIL: usdc accepted a directory\n"); return 1; }
      ++checked;
    }
    {
      LoadOptions o;
      LoadResult r = LoadUSDAFromFile(dirpath, o);
      if (r.success) { std::printf("FAIL: usda accepted a directory\n"); return 1; }
      ++checked;
    }
    {
      // The exact shape the fuzzer produced: an asset path that RESOLVES TO A
      // DIRECTORY and carries a layer extension, routed through the pcp layer
      // registry. The extension matters -- the registry dispatches on it -- so
      // the directory has to be named e.g. "foo.mtlx".
      const std::string base =
          "/tmp/lightusd_dir_asset_" + std::to_string(getpid());
      const char *exts[] = {".mtlx", ".usda", ".usdc", ".usdz", ".usd"};
      pcp::LayerLoadOptions o;
      for (const char *ext : exts) {
        const std::string dpath = base + ext;
        if (mkdir(dpath.c_str(), 0700) != 0) continue;  // best effort
        std::string warn2, err2;
        std::shared_ptr<Layer> l2 =
            pcp::LoadLayerFromFile(dpath, &warn2, &err2, o);
        rmdir(dpath.c_str());
        if (l2) {
          std::printf("FAIL: layer registry accepted a directory (%s)\n", ext);
          return 1;
        }
        ++checked;
      }
    }
    std::printf("directory-as-asset: %d reader path(s) rejected cleanly.\n",
                checked);
  }

  return 0;
}
