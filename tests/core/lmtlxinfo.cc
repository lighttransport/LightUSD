// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// lmtlxinfo [--lib FILE]... FILE
//
// Prints the canonical resolved-document dump of a MaterialX file: XIncludes
// expanded relative to the file, node definitions resolved against the
// document plus every --lib document, file prefixes and color spaces applied.
// The format is defined by the Lean model (materialx-conformance,
// verif/lean/MtlxSpec/Dump.lean) and compared byte for byte against it and
// against the reference MaterialX library.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/mtlx-resolve.hh"

using namespace lightusd::mtlx;

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

static bool LoadExpanded(const std::string &path, MtlxElem *root) {
  std::string text, err;
  if (!ReadFile(path, &text)) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return false;
  }
  if (!ParseMtlxXml(text, root, &err)) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), err.c_str());
    return false;
  }
  const std::string dir = DirOf(path);
  if (!ExpandMtlxIncludes(
          root,
          [&](const std::string &href, std::string *xml) {
            return ReadFile(dir + "/" + href, xml);
          },
          &err)) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), err.c_str());
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  std::vector<std::string> libs;
  std::string file;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--lib" && i + 1 < argc) {
      libs.push_back(argv[++i]);
    } else {
      file = a;
    }
  }
  if (file.empty()) {
    std::fprintf(stderr, "usage: lmtlxinfo [--lib FILE]... FILE\n");
    return 2;
  }
  MtlxElem root;
  if (!LoadExpanded(file, &root)) return 1;
  // The document's own definitions first, then each library in order (the
  // order the reference `Document::importLibrary` produces).
  MtlxLibrary lib;
  lib.AddFrom(root);
  for (const std::string &l : libs) {
    MtlxElem e;
    if (!LoadExpanded(l, &e)) return 1;
    lib.AddFrom(e);
  }
  const std::string dump = DumpMtlxDocument(lib, root);
  std::fwrite(dump.data(), 1, dump.size(), stdout);
  return 0;
}
