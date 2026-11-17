// SPDX-License-Identifier: Apache-2.0
// Differential harness: C++ UpgradeMtlxVersion / InheritMtlxAttributes vs the
// C11 port (lu_mtlx_upgrade_version / lu_mtlx_inherit_attributes).
// Not registered in CMake. Usage:
//   mtlx_cdiff_upgrade <file.mtlx | @listfile>...
// Every document is checked as authored and relabelled with each version
// 1.22 .. 1.39. Exit status is non-zero on any mismatch.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/mtlx-resolve.hh"
#include "core/mtlx-upgrade.hh"
#include "c/mtlx/mtlx-internal.h"

using lightusd::mtlx::MtlxElem;
using lightusd::mtlx::MtlxScope;

namespace {

// Verbatim copy of the anonymous-namespace InheritMtlxAttributes of
// src/core/mtlx-dom.cc (not linkable).
void InheritMtlxAttributes(MtlxElem *elem, const MtlxScope &outer) {
  if (!elem) return;
  static const char *const kScopedAttributes[] = {
      "colorspace", "fileprefix", "geomprefix", "namespace"};
  for (const char *key : kScopedAttributes) {
    if (elem->Attr(key)) continue;
    std::string value;
    if (lightusd::mtlx::ScopedMtlxAttr(key, outer, &value)) elem->SetAttr(key, value);
  }
  MtlxScope child_scope;
  child_scope.reserve(outer.size() + 1);
  child_scope.push_back(elem);
  child_scope.insert(child_scope.end(), outer.begin(), outer.end());
  for (MtlxElem &child : elem->children)
    InheritMtlxAttributes(&child, child_scope);
}

void InheritMtlxAttributes(MtlxElem *root) {
  if (!root) return;
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children)
    InheritMtlxAttributes(&child, outer);
}

void ToC(const MtlxElem &src, lu_mtlx_elem *dst) {
  lu_mtlx_elem_init(dst);
  lu_mtlx_elem_set_tag(dst, src.tag.c_str());
  for (const auto &a : src.attrs) lu_mtlx_attr_append(dst, a.first.c_str(), a.second.c_str());
  for (const MtlxElem &c : src.children) {
    lu_mtlx_elem tmp;
    ToC(c, &tmp);
    lu_mtlx_child_push(dst, &tmp);
  }
}

bool Same(const MtlxElem &a, const lu_mtlx_elem &b, const std::string &path, std::string *why) {
  const std::string here = path + "/" + a.tag + "[" + a.Name() + "]";
  if (a.tag != b.tag) { *why = here + ": tag " + a.tag + " vs " + b.tag; return false; }
  if (a.attrs.size() != b.nattrs) {
    *why = here + ": attr count " + std::to_string(a.attrs.size()) + " vs " + std::to_string(b.nattrs);
    return false;
  }
  for (size_t i = 0; i < b.nattrs; i++) {
    if (a.attrs[i].first != b.attrs[i].key || a.attrs[i].second != b.attrs[i].value) {
      *why = here + ": attr " + std::to_string(i) + " " + a.attrs[i].first + "=" +
             a.attrs[i].second + " vs " + b.attrs[i].key + "=" + b.attrs[i].value;
      return false;
    }
  }
  if (a.children.size() != b.nchildren) {
    *why = here + ": child count " + std::to_string(a.children.size()) + " vs " +
           std::to_string(b.nchildren);
    return false;
  }
  for (size_t i = 0; i < b.nchildren; i++)
    if (!Same(a.children[i], b.children[i], here, why)) return false;
  return true;
}

void Relabel(MtlxElem *root, const std::string &version) {
  for (auto &a : root->attrs)
    if (a.first == "version") { a.second = version; return; }
  root->attrs.emplace_back("version", version);
}

size_t g_checks = 0, g_mismatch = 0;

void Check(const MtlxElem &input, const std::string &label, bool inherit) {
  MtlxElem cpp = input;
  lu_mtlx_elem c;
  ToC(input, &c);
  if (inherit) {
    InheritMtlxAttributes(&cpp);
    lu_mtlx_inherit_attributes(&c);
  } else {
    lightusd::mtlx::UpgradeMtlxVersion(&cpp);
    lu_mtlx_upgrade_version(&c);
  }
  std::string why;
  ++g_checks;
  if (!Same(cpp, c, "", &why)) {
    ++g_mismatch;
    std::cerr << "MISMATCH " << (inherit ? "inherit " : "upgrade ") << label << ": " << why << "\n";
  }
  lu_mtlx_elem_free(&c);
}

void AddArg(const std::string &arg, std::vector<std::string> *files) {
  if (!arg.empty() && arg[0] == '@') {
    std::ifstream in(arg.substr(1));
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) files->push_back(line);
  } else {
    files->push_back(arg);
  }
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) AddArg(argv[i], &files);
  size_t docs = 0, parse_fail = 0;
  for (const std::string &path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    MtlxElem root;
    std::string err;
    if (!lightusd::mtlx::ParseMtlxXml(ss.str(), &root, &err)) { ++parse_fail; continue; }
    ++docs;
    Check(root, path, false);
    Check(root, path, true);
    for (int minor = 22; minor <= 39; minor++) {
      MtlxElem relabelled = root;
      const std::string v = "1." + std::to_string(minor);
      Relabel(&relabelled, v);
      Check(relabelled, path + "@" + v, false);
      // Inherit on the upgraded tree, as MtlxDocument loading does.
      MtlxElem up = relabelled;
      lightusd::mtlx::UpgradeMtlxVersion(&up);
      Check(up, path + "@" + v + "+upgraded", true);
    }
  }
  std::printf("files=%zu parsed=%zu parse_fail=%zu checks=%zu match=%zu mismatch=%zu\n",
              files.size(), docs, parse_fail, g_checks, g_checks - g_mismatch, g_mismatch);
  return g_mismatch || docs == 0 ? 1 : 0;
}
