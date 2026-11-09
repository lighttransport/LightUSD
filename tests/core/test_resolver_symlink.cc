// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Regression test: the next-module AssetResolver must follow SYMLINKED assets.
//
// Bug: AssetResolver::FileExistsImpl (src/next/resolver/asset-resolver.cc) used
// lstat()+S_ISREG to probe candidate paths. For a symlink, lstat reports the
// link itself (S_ISLNK, not S_ISREG), so a symlinked asset was reported
// "missing". The resolver then fell through to returning the bare, *unanchored*
// relative path (e.g. "render_high/mesh/foo.usd"), which fails to open -- so any
// payload/reference whose target file is a symlink was silently dropped.
//
// This regularly happens in production scenes: Animal Logic ALab merges
// techvar_assets / baked_procedurals / trailer_cameras into the tree as
// symlinks, so every symlinked variant-gated payload and surfacing binding
// vanished from the composed stage (entity composed to 0 meshes via --next).
//
// Fix: probe with stat() (follows the link to its target's type). A symlink to a
// regular file now yields S_ISREG and resolves against the anchor dir; a
// dangling symlink still fails stat (correctly "not found").
//
// Coverage:
//   A. AssetResolver unit: Exists/ResolvePath follow a symlink-to-file; a
//      dangling symlink and a symlink-to-directory are correctly NOT regular
//      files (guards against over-correcting).
//   B. End-to-end pcp compose of the checked-in fixture
//      tests/usda/feat-symlink-payload/ (references -> payload whose target file
//      is a symlink); /root/SymlinkedMesh must survive composition.
//   C. End-to-end pcp compose of an equivalent fixture built in a temp dir with
//      an explicit symlink() at runtime -- so the regression is caught even if a
//      git checkout did not preserve the on-disk symlink.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "../../src/pcp/cache.hh"
#include "resolver/asset-resolver.hh"
#include "stage/stage.hh"

using namespace lightusd;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL: " << (msg) << "  (line " << __LINE__ << ")\n";  \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// Unique-ish temp dir without relying on mkdtemp's template churn; pid keeps
// parallel ctest runs from colliding.
std::string MakeTempDir(const std::string &tag) {
  std::string base = "/tmp/lightusd_symlink_" + tag + "_" + std::to_string(getpid());
  // Best-effort clean + create.
  std::string rm = "rm -rf '" + base + "'";
  if (std::system(rm.c_str()) != 0) { /* ignore */ }
  if (::mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) {
    std::cerr << "FAIL: could not create temp dir " << base << "\n";
    ++g_failures;
  }
  return base;
}

void WriteFile(const std::string &path, const std::string &contents) {
  std::ofstream f(path);
  f << contents;
}

bool IsSymlink(const std::string &path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// ----------------------------------------------------------------------------
// A. AssetResolver unit: symlink probing.
// ----------------------------------------------------------------------------
void TestResolverFollowsSymlink() {
  const std::string dir = MakeTempDir("unit");
  const std::string real = dir + "/mesh_real.usda";
  const std::string link = dir + "/mesh_link.usda";       // -> mesh_real.usda
  const std::string dangling = dir + "/dangling.usda";    // -> nonexistent
  const std::string dirlink = dir + "/dir_link";          // -> a directory

  WriteFile(real, "#usda 1.0\n");
  CHECK(::symlink("mesh_real.usda", link.c_str()) == 0, "create file symlink");
  CHECK(::symlink("does_not_exist.usda", dangling.c_str()) == 0,
        "create dangling symlink");
  CHECK(::symlink(dir.c_str(), dirlink.c_str()) == 0, "create dir symlink");

  AssetResolver resolver;
  // Anchor is a FILE path in `dir`; the resolver takes its directory.
  const std::string anchor = dir + "/anchor.usda";

  // The symlink-to-file must resolve (this is the regression).
  CHECK(resolver.Exists("mesh_link.usda", anchor),
        "Exists() must follow a symlink to a regular file");
  CHECK(resolver.ResolvePath("mesh_link.usda", anchor) == link,
        "ResolvePath() must anchor the symlinked asset to its directory");

  // A plain regular file still resolves (no behavioral change).
  CHECK(resolver.Exists("mesh_real.usda", anchor),
        "Exists() must resolve a plain regular file");

  // Guards against over-correcting: a dangling symlink is NOT found, and a
  // symlink to a directory is not a regular-file asset.
  CHECK(!resolver.Exists("dangling.usda", anchor),
        "dangling symlink must NOT be reported as existing");
  CHECK(!resolver.Exists("dir_link", anchor),
        "symlink to a directory must NOT be reported as a regular-file asset");

  std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) { /* ignore */ }
}

// Compose `main_usda` and assert the symlinked-payload mesh survived.
void ComposeAndCheckMesh(const std::string &main_usda, const char *label) {
  AssetResolver resolver;
  Stage stage;
  std::string warn, err;
  bool ok = pcp::ComposeStageFromFile(main_usda, resolver, &stage, {}, &warn,
                                      &err);
  CHECK(ok, std::string(label) + ": ComposeStageFromFile must succeed");

  UsdPrim mesh = stage.GetPrimAtPath(std::string("/root/SymlinkedMesh"));
  CHECK(mesh.IsValid(),
        std::string(label) +
            ": /root/SymlinkedMesh (from the symlinked payload) must compose in");
  if (mesh.IsValid()) {
    CHECK(mesh.GetTypeName() == "Mesh",
          std::string(label) + ": composed prim must be a Mesh");
  }
  // The bug surfaced as a "Failed to open file: <bare relative path>" warning;
  // make sure the symlinked target did not degrade to an unanchored bare path.
  CHECK(warn.find("Failed to open file") == std::string::npos,
        std::string(label) +
            ": no 'Failed to open file' warning (symlink resolved)");
}

// ----------------------------------------------------------------------------
// B. End-to-end: the checked-in fixture (real on-disk symlink).
// ----------------------------------------------------------------------------
void TestCheckedInFixture() {
#ifdef SYMLINK_FIXTURE_DIR
  const std::string fix = SYMLINK_FIXTURE_DIR;
  const std::string link = fix + "/geo/mesh_link.usda";
  // A git checkout without symlink support would materialize mesh_link.usda as a
  // text file containing "mesh_real.usda". Restore the symlink so the test still
  // exercises the symlink path (the whole point of this fixture).
  if (!IsSymlink(link)) {
    std::string rm = "rm -f '" + link + "'";
    if (std::system(rm.c_str()) != 0) { /* ignore */ }
    if (::symlink("mesh_real.usda", link.c_str()) != 0) {
      std::cerr << "WARN: could not restore fixture symlink " << link
                << "; skipping checked-in-fixture case\n";
      return;
    }
  }
  ComposeAndCheckMesh(fix + "/main.usda", "checked-in fixture");
#else
  std::cerr << "WARN: SYMLINK_FIXTURE_DIR not defined; "
               "skipping checked-in-fixture case\n";
#endif
}

// ----------------------------------------------------------------------------
// C. End-to-end: self-contained fixture built at runtime with a real symlink.
//    Mirrors tests/usda/feat-symlink-payload/ so the regression is caught even
//    if the checked-in symlink was not preserved on this platform/checkout.
// ----------------------------------------------------------------------------
void TestRuntimeFixture() {
  const std::string dir = MakeTempDir("e2e");
  if (::mkdir((dir + "/geo").c_str(), 0755) != 0 && errno != EEXIST) {
    std::cerr << "FAIL: could not create geo subdir\n";
    ++g_failures;
    return;
  }

  WriteFile(dir + "/main.usda",
            "#usda 1.0\n(\n    defaultPrim = \"root\"\n)\n\n"
            "def Xform \"root\" (\n"
            "    prepend references = @geo/frag.usda@\n)\n{\n}\n");
  WriteFile(dir + "/geo/frag.usda",
            "#usda 1.0\n(\n    defaultPrim = \"root\"\n)\n\n"
            "def Xform \"root\" (\n"
            "    prepend payload = @mesh_link.usda@\n)\n{\n}\n");
  WriteFile(dir + "/geo/mesh_real.usda",
            "#usda 1.0\n(\n    defaultPrim = \"root\"\n)\n\n"
            "def Xform \"root\"\n{\n"
            "    def Mesh \"SymlinkedMesh\"\n    {\n"
            "        int[] faceVertexCounts = [4]\n"
            "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
            "        point3f[] points = [(-1, 0, -1), (1, 0, -1), "
            "(1, 0, 1), (-1, 0, 1)]\n    }\n}\n");
  // The payload target is a SYMLINK in the geo/ subdir (anchored there, so the
  // resolver's working-directory fallback -- main.usda's dir -- cannot find it;
  // resolution must succeed through the symlink itself).
  CHECK(::symlink("mesh_real.usda", (dir + "/geo/mesh_link.usda").c_str()) == 0,
        "create runtime fixture symlink");

  ComposeAndCheckMesh(dir + "/main.usda", "runtime fixture");

  std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) { /* ignore */ }
}

}  // namespace

int main() {
  TestResolverFollowsSymlink();
  TestCheckedInFixture();
  TestRuntimeFixture();

  if (g_failures == 0) {
    std::cout << "test-resolver-symlink: ALL PASS\n";
    return 0;
  }
  std::cerr << "test-resolver-symlink: " << g_failures << " failure(s)\n";
  return 1;
}
