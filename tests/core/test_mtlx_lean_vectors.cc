// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Lean-generated MaterialX conformance vectors.
//
// The expected values in tests/core/generated/lean-mtlx/*.inc are computed by
// the Lean 4 formalization of MaterialX 1.39 in the materialx-conformance
// repository (`lake exe mtlxspec-gen` in verif/lean). Each section replays one
// vector table against src/core/mtlx-resolve. The tables are COMMITTED; this
// binary is self-contained and never skips.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/mtlx-resolve.hh"

using namespace lightusd::mtlx;

static int g_fail = 0;
static int g_checks = 0;

static void Fail(const char *table, int id, const std::string &msg) {
  std::fprintf(stderr, "FAIL %s[%d]: %s\n", table, id, msg.c_str());
  g_fail++;
}

static std::vector<std::string> Split(const std::string &s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

// "num/den" -> double.
static double Rat(const std::string &s) {
  const size_t slash = s.find('/');
  const double n = std::strtod(s.substr(0, slash).c_str(), nullptr);
  const double d = slash == std::string::npos
                       ? 1.0
                       : std::strtod(s.substr(slash + 1).c_str(), nullptr);
  return n / d;
}

static bool Close(double a, double b) {
  if (a == b) return true;
  const double scale = std::fmax(std::fabs(a), std::fabs(b));
  return std::fabs(a - b) <= scale * 1e-15;
}

static const char *KindName(MtlxValueKind k) {
  switch (k) {
    case MtlxValueKind::None: return "none";
    case MtlxValueKind::Int: return "int";
    case MtlxValueKind::Bool: return "bool";
    case MtlxValueKind::Num: return "num";
    case MtlxValueKind::Agg: return "agg";
    case MtlxValueKind::Str: return "str";
    case MtlxValueKind::Ints: return "ints";
    case MtlxValueKind::Nums: return "nums";
    case MtlxValueKind::Strs: return "strs";
  }
  return "?";
}

static bool PayloadMatches(const MtlxTypedValue &v, const std::string &payload) {
  switch (v.kind) {
    case MtlxValueKind::None: return payload.empty();
    case MtlxValueKind::Int: return std::to_string(v.i) == payload;
    case MtlxValueKind::Bool: return payload == (v.b ? "true" : "false");
    case MtlxValueKind::Num: return Close(v.num, Rat(payload));
    case MtlxValueKind::Str: return v.str == payload;
    case MtlxValueKind::Agg:
    case MtlxValueKind::Nums: {
      const std::vector<std::string> parts = Split(payload, ',');
      if (parts.size() != v.nums.size()) return false;
      for (size_t i = 0; i < parts.size(); i++) {
        if (!Close(v.nums[i], Rat(parts[i]))) return false;
      }
      return true;
    }
    case MtlxValueKind::Ints: {
      const std::vector<std::string> parts = Split(payload, ',');
      if (parts.size() != v.ints.size()) return false;
      for (size_t i = 0; i < parts.size(); i++) {
        if (std::to_string(v.ints[i]) != parts[i]) return false;
      }
      return true;
    }
    case MtlxValueKind::Strs: {
      // The model's empty array and its one-empty-member array both print as
      // "", so compare through the same join.
      std::string joined;
      for (size_t i = 0; i < v.strs.size(); i++) {
        if (i) joined.push_back('\037');
        joined += v.strs[i];
      }
      return joined == payload;
    }
  }
  return false;
}

static void CheckName(int id, const char *text, int spec, int ref) {
  g_checks++;
  if (IsValidMtlxName(text) != (spec != 0)) Fail("MTLX_NAME", id, "strict validity");
  if (IsReferenceValidMtlxName(text) != (ref != 0)) {
    Fail("MTLX_NAME", id, "reference validity");
  }
}

static void CheckValue(int id, const char *type, const char *text, int ok,
                       const char *kind, const char *payload) {
  g_checks++;
  MtlxTypedValue v;
  const bool got = ParseMtlxValue(type, text, &v);
  if (got != (ok != 0)) {
    Fail("MTLX_VALUE", id, std::string("acceptance of [") + text + "] as " + type);
    return;
  }
  if (!got) return;
  if (std::string(KindName(v.kind)) != kind) {
    Fail("MTLX_VALUE", id, std::string("kind ") + KindName(v.kind) + " != " + kind);
  } else if (!PayloadMatches(v, payload)) {
    Fail("MTLX_VALUE", id, std::string("payload of [") + text + "]");
  }
}

static void CheckDefault(const char *type, const char *kind, const char *payload) {
  g_checks++;
  MtlxTypedValue v;
  if (!DefaultMtlxValue(type, &v)) {
    Fail("MTLX_DEFAULT", 0, std::string("no default for ") + type);
    return;
  }
  if (std::string(KindName(v.kind)) != kind || !PayloadMatches(v, payload)) {
    Fail("MTLX_DEFAULT", 0, std::string("default of ") + type);
  }
}

static void CheckScope(int id, const char *xml, const char *path, int has_fn,
                       const char *fn, const char *cs) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_SCOPE", id, "parse: " + err);
    return;
  }
  MtlxScope chain = {&root};
  for (const std::string &name : Split(path, '/')) {
    const MtlxElem *c = chain.front()->Child(name);
    if (!c) {
      Fail("MTLX_SCOPE", id, "path " + name);
      return;
    }
    chain.insert(chain.begin(), c);
  }
  std::string got_fn;
  const bool got = ResolvedMtlxFilename(chain, &got_fn);
  if (got != (has_fn != 0) || (got && got_fn != fn)) {
    Fail("MTLX_SCOPE", id, "filename [" + got_fn + "] != [" + fn + "]");
  }
  std::string got_cs;
  (void)ActiveMtlxColorSpace(chain, &got_cs);
  if (got_cs != cs) Fail("MTLX_SCOPE", id, "colorspace [" + got_cs + "] != [" + cs + "]");
}

static std::string Listing(const MtlxElem &root) {
  static const char *const kKeys[] = {"fileprefix", "geomprefix", "colorspace",
                                      "namespace"};
  std::string out;
  for (size_t i = 0; i < root.children.size(); i++) {
    const MtlxElem &c = root.children[i];
    if (i) out.push_back(',');
    out += c.tag + ":" + c.Name() + "[";
    bool first = true;
    for (const char *k : kKeys) {
      if (const std::string *v = c.Attr(k)) {
        if (!first) out.push_back(';');
        out += std::string(k) + "=" + *v;
        first = false;
      }
    }
    out += "]";
  }
  return out;
}

static void CheckInclude(int id, const char *main_xml, const char *href_a,
                         const char *xml_a, const char *href_b,
                         const char *xml_b, int ok, const char *listing) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(main_xml, &root, &err)) {
    Fail("MTLX_INCLUDE", id, "parse: " + err);
    return;
  }
  const std::string ha = href_a, hb = href_b, xa = xml_a, xb = xml_b;
  const bool got = ExpandMtlxIncludes(
      &root,
      [&](const std::string &href, std::string *xml) {
        if (!ha.empty() && href == ha) {
          *xml = xa;
          return true;
        }
        if (!hb.empty() && href == hb) {
          *xml = xb;
          return true;
        }
        return false;
      },
      &err);
  if (got != (ok != 0)) {
    Fail("MTLX_INCLUDE", id, "acceptance (" + err + ")");
    return;
  }
  if (got && Listing(root) != listing) {
    Fail("MTLX_INCLUDE", id, "[" + Listing(root) + "] != [" + listing + "]");
  }
}

static MtlxLibrary g_resolve_lib;

static void SetResolveLib(const char *xml) {
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_RESOLVE_LIB", 0, "parse: " + err);
    return;
  }
  g_resolve_lib.AddFrom(root);
}

static void CheckResolve(int id, const char *node_xml, const char *strict,
                         const char *ref, const char *rough) {
  g_checks++;
  MtlxElem node;
  std::string err;
  if (!ParseMtlxXml(node_xml, &node, &err)) {
    Fail("MTLX_RESOLVE", id, "parse: " + err);
    return;
  }
  const MtlxNodeDefInfo *s = g_resolve_lib.ResolveStrict(node);
  const MtlxNodeDefInfo *r = g_resolve_lib.ResolveReference(node);
  if ((s ? s->name : std::string()) != strict) {
    Fail("MTLX_RESOLVE", id, "strict [" + (s ? s->name : std::string()) + "]");
  }
  if ((r ? r->name : std::string()) != ref) {
    Fail("MTLX_RESOLVE", id, "reference [" + (r ? r->name : std::string()) + "]");
  }
  const MtlxNodeDefInfo *g = g_resolve_lib.ResolveRough(node);
  if ((g ? g->name : std::string()) != rough) {
    Fail("MTLX_RESOLVE", id, "rough [" + (g ? g->name : std::string()) + "]");
  }
}

static void CheckDump(int id, const char *xml, const char *dump) {
  g_checks++;
  MtlxElem root;
  std::string err;
  if (!ParseMtlxXml(xml, &root, &err)) {
    Fail("MTLX_DUMP", id, "parse: " + err);
    return;
  }
  MtlxLibrary lib;
  lib.AddFrom(root);
  const std::string got = DumpMtlxDocument(lib, root);
  if (got != dump) {
    Fail("MTLX_DUMP", id, "dump differs:\n--- got ---\n" + got + "--- want ---\n" + dump);
  }
}

int main() {
#define MTLX_NAME(id, text, spec, ref) CheckName(id, text, spec, ref);
#include "generated/lean-mtlx/mtlx-name.inc"
#undef MTLX_NAME

#define MTLX_VALUE(id, type, text, ok, kind, payload) \
  CheckValue(id, type, text, ok, kind, payload);
#define MTLX_DEFAULT(type, kind, payload) CheckDefault(type, kind, payload);
#include "generated/lean-mtlx/mtlx-value.inc"
#undef MTLX_VALUE
#undef MTLX_DEFAULT

#define MTLX_SCOPE(id, xml, path, has_fn, fn, cs) \
  CheckScope(id, xml, path, has_fn, fn, cs);
#define MTLX_INCLUDE(id, main_xml, ha, xa, hb, xb, ok, listing) \
  CheckInclude(id, main_xml, ha, xa, hb, xb, ok, listing);
#define MTLX_RESOLVE_LIB(xml) SetResolveLib(xml);
#define MTLX_RESOLVE(id, node_xml, strict, ref, rough) \
  CheckResolve(id, node_xml, strict, ref, rough);
#define MTLX_DUMP(id, xml, dump) CheckDump(id, xml, dump);
#include "generated/lean-mtlx/mtlx-doc.inc"
#undef MTLX_SCOPE
#undef MTLX_INCLUDE
#undef MTLX_RESOLVE_LIB
#undef MTLX_RESOLVE
#undef MTLX_DUMP

  std::printf("test_mtlx_lean_vectors: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
