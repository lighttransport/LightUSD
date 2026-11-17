// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// lmtlxinfo [--upgrade] [--lib FILE]... FILE
//
// Prints the canonical resolved-document dump of a MaterialX file: XIncludes
// expanded relative to the file, node definitions resolved against the
// document plus every --lib document, file prefixes and color spaces applied.
// The format is defined by the Lean model (materialx-conformance,
// verif/lean/MtlxSpec/Dump.lean) and compared byte for byte against it and
// against the reference MaterialX library.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "c/mtlx/mtlx-internal.h"

static bool ReadFile(const std::string &path, std::string *out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static std::string DirOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// XIncludes are resolved relative to the top-level file's directory.
static int LoadRelative(void *user, const char *href, const char * /*including*/, char **xml,
                        size_t *len) {
  std::string text;
  if (!ReadFile(*static_cast<const std::string *>(user) + "/" + href, &text)) return 0;
  char *buf = static_cast<char *>(std::malloc(text.size() + 1));
  if (!buf) return 0;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  *xml = buf;
  *len = text.size();
  return 1;
}

// `root` must be initialised.
static bool LoadExpanded(const std::string &path, lu_mtlx_elem *root) {
  std::string text;
  if (!ReadFile(path, &text)) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return false;
  }
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  std::string dir = DirOf(path);
  const bool ok = lu_mtlx_parse_xml(text.data(), text.size(), root, &err) &&
                  lu_mtlx_expand_includes(root, LoadRelative, &dir, &err);
  if (!ok) std::fprintf(stderr, "error: %s: %s\n", path.c_str(), lu_mtlx_sb_str(&err));
  lu_mtlx_sb_free(&err);
  return ok;
}

int main(int argc, char **argv) {
  std::vector<std::string> libs;
  std::string file;
  bool upgrade = false;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--upgrade") {
      upgrade = true;
    } else if (a == "--lib" && i + 1 < argc) {
      libs.push_back(argv[++i]);
    } else {
      file = a;
    }
  }
  if (file.empty()) {
    std::fprintf(stderr, "usage: lmtlxinfo [--upgrade] [--lib FILE]... FILE\n");
    return 2;
  }
  lu_mtlx_elem root;
  lu_mtlx_elem_init(&root);
  if (!LoadExpanded(file, &root)) {
    lu_mtlx_elem_free(&root);
    return 1;
  }
  if (upgrade) lu_mtlx_upgrade_version(&root);
  // The document's own definitions first, then each library in order (the
  // order the reference `Document::importLibrary` produces).
  lu_mtlx_library lib;
  lu_mtlx_library_init(&lib);
  int rc = lu_mtlx_library_add_from(&lib, &root) ? 0 : 1;
  for (const std::string &l : libs) {
    if (rc) break;
    lu_mtlx_elem e;
    lu_mtlx_elem_init(&e);
    if (!LoadExpanded(l, &e) || !lu_mtlx_library_add_from(&lib, &e)) rc = 1;
    lu_mtlx_elem_free(&e);
  }
  if (!rc) {
    lu_mtlx_sb dump;
    lu_mtlx_sb_init(&dump);
    lu_mtlx_dump_document(&lib, &root, &dump);
    std::fwrite(lu_mtlx_sb_str(&dump), 1, dump.n, stdout);
    lu_mtlx_sb_free(&dump);
  }
  lu_mtlx_library_free(&lib);
  lu_mtlx_elem_free(&root);
  return rc;
}
