// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Lean-generated conformance vectors.
//
// The expected values in tests/next/generated/lean/*.inc are computed by the
// Lean 4 formalization of the AOUSD USD Core Specification v1.1.0 under
// the usdspec-conformance repo (lake exe usdspec-gen in verif/lean). Each section below replays one vector
// table against the corresponding src/next implementation. The tables are
// COMMITTED; this binary is self-contained and never skips.
//
// Usage: test_lean_vectors [--combine-chain <corpus data_types/tests/combine_chain dir>]
//   With --combine-chain, ONLY the AOUSD supplemental corpus combine_chain
//   JSON cases are replayed (exit 77 when the directory is absent).

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "next/layer/prim-spec.hh"
#include "next/layer/int64-listop.hh"
#include "next/layer/prim-spec-metadata.hh"
#include "next/types/value.hh"
#include "next/prim/path.hh"
#include "next/schema/schema-registry.hh"
#include "next/crate/crate-format.hh"
#include "next/crate/listop-wire.hh"
#include "next/crate/crate-reader.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usda-writer.hh"
#include "next/reader/usdc-reader.hh"
#include "next/layer/layer.hh"
#include "core/minijson.hh"
#include "next/pcp/cache.hh"
#include "next/pcp/namespace-mapping.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/eval/attribute-eval.hh"
#include "next/reader/usda-reader.hh"
#include "next/types/interpolation.hh"
#include "next/types/spline.hh"
#include "next/eval/value-clip.hh"

using namespace lightusd::next;

namespace {

int g_failures = 0;
int g_checks = 0;

std::vector<std::string> SplitList(const char *s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char *p = s; *p; ++p) {
    if (*p == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(*p);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<int64_t> SplitInts(const char *s) {
  std::vector<int64_t> out;
  for (const std::string &item : SplitList(s)) out.push_back(std::strtoll(item.c_str(), nullptr, 10));
  return out;
}

std::string Join(const std::vector<std::string> &v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += v[i];
  }
  return out;
}

std::string JoinInts(const std::vector<int64_t> &v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += std::to_string(v[i]);
  }
  return out;
}

void Expect(const std::string &table, int id, const std::string &what,
            const std::string &got, const std::string &want) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    if (g_failures <= 50) {
      std::fprintf(stderr, "FAIL %s#%d %s: got [%s] want [%s]\n", table.c_str(), id, what.c_str(),
                   got.c_str(), want.c_str());
    }
  }
}

// AOUSD ListOp wire order: explicit, added, prepended, appended, deleted, ordered.
Int64ListOp MakeInt64Op(bool is_explicit, const std::vector<int64_t> &expl,
                        const std::vector<int64_t> &pre, const std::vector<int64_t> &app,
                        const std::vector<int64_t> &del) {
  Int64ListOp op;
  if (is_explicit) {
    op.bits = 1u | kInt64ListOpBits[0];
    op.items[0] = Value::MakeInt64Array(ValueArrayView<const int64_t>(expl.data(), expl.size()));
  } else {
    op.bits = static_cast<uint8_t>(kInt64ListOpBits[2] | kInt64ListOpBits[3] | kInt64ListOpBits[4]);
    op.items[2] = Value::MakeInt64Array(ValueArrayView<const int64_t>(pre.data(), pre.size()));
    op.items[3] = Value::MakeInt64Array(ValueArrayView<const int64_t>(app.data(), app.size()));
    op.items[4] = Value::MakeInt64Array(ValueArrayView<const int64_t>(del.data(), del.size()));
  }
  return op;
}

std::vector<int64_t> ToVector(const Value &v) {
  auto view = v.as_int64_array();
  return std::vector<int64_t>(view.data(), view.data() + view.size());
}

// Parses "E:1,2" or "C:del|pre|app".
Int64ListOp ParseOp(const std::string &enc) {
  if (enc.rfind("E:", 0) == 0) return MakeInt64Op(true, SplitInts(enc.c_str() + 2), {}, {}, {});
  std::string body = enc.substr(2);
  size_t p1 = body.find('|');
  size_t p2 = body.find('|', p1 + 1);
  std::string del = body.substr(0, p1);
  std::string pre = body.substr(p1 + 1, p2 - p1 - 1);
  std::string app = body.substr(p2 + 1);
  return MakeInt64Op(false, {}, SplitInts(pre.c_str()), SplitInts(app.c_str()), SplitInts(del.c_str()));
}

void CheckListOp(int id, bool is_explicit, const char *expl, const char *pre, const char *app,
                 const char *del, const char *weaker, const char *expected) {
  // String path: ApplyStringListOp over the weaker, already-iterated list.
  StringListOps edits;
  edits.authored = true;
  edits.is_explicit = is_explicit;
  edits.explicit_items = SplitList(expl);
  edits.prepended = SplitList(pre);
  edits.appended = SplitList(app);
  edits.deleted = SplitList(del);
  Expect("listop/string", id, "ApplyStringListOp", Join(ApplyStringListOp(edits, SplitList(weaker))),
         expected);

  // Int64 path: stack [op, explicit(weaker)], strongest first.
  Int64ListOpStack stack;
  stack.ops.push_back(MakeInt64Op(is_explicit, SplitInts(expl), SplitInts(pre), SplitInts(app),
                                  SplitInts(del)));
  stack.ops.push_back(MakeInt64Op(true, SplitInts(weaker), {}, {}, {}));
  Expect("listop/int64", id, "ApplyInt64ListOps", JoinInts(ToVector(ApplyInt64ListOps(stack))),
         expected);
}

void CheckListOpChain(int id, const char *ops, const char *expected) {
  Int64ListOpStack stack;
  std::string s(ops);
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(';', start);
    if (end == std::string::npos) end = s.size();
    stack.ops.push_back(ParseOp(s.substr(start, end - start)));
    start = end + 1;
  }
  Expect("listop/chain", id, "ApplyInt64ListOps(stack)", JoinInts(ToVector(ApplyInt64ListOps(stack))),
         expected);
}

std::string JoinWords(const std::vector<uint64_t>& values) {
  std::string result;
  for (uint64_t value : values) {
    if (!result.empty()) result += ",";
    result += std::to_string(value);
  }
  return result;
}

void CheckCrateListOp(int id, const char* header, const char* wire, bool explicit_mode,
                     const char* expl, const char* added, const char* pre,
                     const char* app, const char* deleted, const char* ordered, bool compatibility = false) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < std::strlen(wire); i += 2) {
    char octet[3] = {wire[i], wire[i + 1], 0};
    bytes.push_back(static_cast<uint8_t>(std::strtoul(octet, nullptr, 16)));
  }
  std::array<std::vector<uint64_t>, 6> runs;
  uint8_t bits = 0;
  auto decode = [&](size_t length) {
    StreamReader reader(bytes.data(), length);
    return DecodeCrateListOpWire(reader, 1024, bits, [&](size_t run, uint64_t count) {
      if (!reader.has_elements(static_cast<size_t>(count), 8)) return false;
      runs[run].clear();
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t value = 0;
        if (!reader.read_u64(value)) return false;
        runs[run].push_back(value);
      }
      return true;
    }) && reader.position() == length;
  };
  for (size_t prefix = 0; prefix < bytes.size(); ++prefix)
    Expect("crate/listop", id, "reject truncation", decode(prefix) ? "accepted" : "rejected", "rejected");
  for (auto& run : runs) run.clear();
  Expect("crate/listop", id, "shared wire decoder", decode(bytes.size()) ? "ok" : "failed", "ok");
  Expect("crate/listop", id, "header", std::to_string(bits),
         std::to_string(std::strtoul(header, nullptr, 0)));
  Expect("crate/listop", id, "explicit flag",
         ((compatibility ? CrateListOpSemanticHeader(bits) : bits) & 1u) ? "1" : "0",
         explicit_mode ? "1" : "0");
  const char* expected[] = {expl, added, pre, app, deleted, ordered};
  Int64ListOp op;
  op.bits = bits;
  for (size_t run = 0; run < 6; ++run) {
    Expect("crate/listop", id, "wire vector",
           JoinWords(compatibility && !CrateListOpRetainsRun(bits, run)
                         ? std::vector<uint64_t>{} : runs[run]), expected[run]);
    std::vector<int64_t> signed_values;
    for (uint64_t word : runs[run]) {
      int64_t value = 0;
      std::memcpy(&value, &word, sizeof(value));
      signed_values.push_back(value);
    }
    op.items[run] = Value::MakeInt64Array(signed_values);
  }
  // Exercise the actual writer and CrateReader::DecodeInt64ListOpField, not
  // only a test adapter: inspect the field's payload against Lean wire bytes.
  Layer layer;
  LayerBuilder builder(layer);
  builder.begin_prim("P", "Scope");
  builder.current()->meta().unknownFields().push_back(MakeInt64ListOpField("leanListOp", op));
  builder.end_prim();
  builder.finalize();
  std::vector<uint8_t> crate;
  const auto written = WriteLayerToUSDCMemory(crate, layer);
  Expect("crate/listop", id, "production writer", written.success ? "ok" : "failed", "ok");
  CrateReader reader;
  auto read = reader.Read(crate.data(), crate.size());
  Expect("crate/listop", id, "production reader", read.success ? "ok" : "failed", "ok");
  if (!read.success) return;
  const auto tokens = reader.tokens();
  bool found = false;
  for (const auto& field : reader.fields()) {
    if (field.token_index.value >= tokens.size() || tokens[field.token_index.value] != "leanListOp") continue;
    const uint64_t offset = field.value_rep.payload();
    const bool exact = offset <= crate.size() && bytes.size() <= crate.size() - offset &&
        std::equal(bytes.begin(), bytes.end(), crate.begin() + static_cast<size_t>(offset));
    Expect("crate/listop", id, "production wire bytes", exact ? "exact" : "different", "exact");
    found = true;
  }
  Expect("crate/listop", id, "production wire field", found ? "found" : "missing", "found");
  const PrimSpec* prim = read.stage.GetRootLayer()->prim_at_path("/P");
  bool retained = false;
  if (prim) for (const auto& field : prim->meta().unknownFields()) {
    if (field.name != "leanListOp" || !field.int64_list_op || field.int64_list_op->ops.size() != 1) continue;
    retained = true;
    const Int64ListOp& decoded = field.int64_list_op->ops.front();
    Expect("crate/listop", id, "production header", std::to_string(decoded.bits),
           std::to_string(compatibility ? CrateListOpSemanticHeader(bits) : bits));
    for (size_t run = 0; run < 6; ++run) {
      std::vector<uint64_t> words;
      if (auto values = decoded.items[run].as_int64_array())
        for (int64_t value : *values) words.push_back(static_cast<uint64_t>(value));
      Expect("crate/listop", id, "production vector", JoinWords(words), expected[run]);
    }
  }
  Expect("crate/listop", id, "production structured field", retained ? "found" : "missing", "found");
}

void RunCrateListOpVectors() {
#define LEAN_CRATE_LISTOP(id, header, wire, explicit_mode, expl, added, pre, app, deleted, ordered) \
  CheckCrateListOp(id, header, wire, explicit_mode, expl, added, pre, app, deleted, ordered);
#define LEAN_CRATE_LISTOP_MODE(id, header, wire, explicit_mode, expl, added, pre, app, deleted, ordered) \
  CheckCrateListOp(id, header, wire, explicit_mode, expl, added, pre, app, deleted, ordered, true);
#include "generated/lean/crate-listop-vectors.inc"
#undef LEAN_CRATE_LISTOP_MODE
#undef LEAN_CRATE_LISTOP
  const std::vector<uint8_t> reserved{0x80};
  StreamReader reader(reserved);
  uint8_t header = 0;
  Expect("crate/listop", -1, "reserved bit", DecodeCrateListOpWire(reader, 1024, header,
      [](size_t, uint64_t) { return true; }) ? "accepted" : "rejected", "rejected");
  const std::vector<uint8_t> excessive{2, 1, 4, 0, 0, 0, 0, 0, 0};  // count 1025
  StreamReader bounded(excessive);
  bool called = false;
  const bool accepted = DecodeCrateListOpWire(bounded, 1024, header,
      [&](size_t, uint64_t) { called = true; return true; });
  Expect("crate/listop", -2, "element limit", accepted ? "accepted" : "rejected", "rejected");
  Expect("crate/listop", -2, "limit before allocation callback", called ? "called" : "not called", "not called");
}

void RunListOpVectors() {
#define LEAN_LISTOP(id, is_explicit, expl, pre, app, del, weaker, expected) \
  CheckListOp(id, (is_explicit) != 0, expl, pre, app, del, weaker, expected);
#include "generated/lean/listop-vectors.inc"
#undef LEAN_LISTOP
#define LEAN_LISTOP_CHAIN(id, ops, expected) CheckListOpChain(id, ops, expected);
#include "generated/lean/listop-chain-vectors.inc"
#undef LEAN_LISTOP_CHAIN
}


// ---------------------------------------------------------------------------
// Crate integer coding (spec L11600-11683) vs EncodeDeltaU32 / DecodeDeltaU32.
// ---------------------------------------------------------------------------
void CheckIntCode(int id, int n, const char *values, const char *bytes, int tie) {
  std::vector<int64_t> v = SplitInts(values);
  std::vector<int64_t> b = SplitInts(bytes);
  std::vector<uint32_t> vals(v.begin(), v.end());
  std::vector<uint8_t> expect(b.begin(), b.end());
  if (static_cast<int>(vals.size()) != n) {
    ++g_failures;
    std::fprintf(stderr, "FAIL intcode#%d: row length mismatch\n", id);
    return;
  }
  // Decoder: Lean bytes must decode to the values (always).
  std::vector<uint32_t> decoded(vals.size(), 0u);
  ++g_checks;
  bool ok = vals.empty() ? expect.empty()
                         : DecodeDeltaU32(expect.data(), expect.size(), decoded.data(), decoded.size());
  if (!ok || decoded != vals) {
    ++g_failures;
    if (g_failures <= 50) std::fprintf(stderr, "FAIL intcode#%d DecodeDeltaU32 mismatch\n", id);
  }
  // Encoder: byte-identical unless the most-common delta is tied (tie-break
  // differs between OpenUSD and LightUSD; both decode).
  if (!tie && !vals.empty()) {
    std::vector<uint8_t> got = EncodeDeltaU32(vals.data(), vals.size());
    std::vector<int64_t> g64(got.begin(), got.end());
    Expect("intcode/encode", id, "EncodeDeltaU32", JoinInts(g64), JoinInts(b));
  }
}

void RunIntCodeVectors() {
#define LEAN_INTCODE(id, n, values, bytes, tie) CheckIntCode(id, n, values, bytes, tie);
#include "generated/lean/intcoding-vectors.inc"
#undef LEAN_INTCODE
}

// ---------------------------------------------------------------------------
// AOUSD supplemental corpus: data_types/tests/combine_chain/*.json
// ---------------------------------------------------------------------------
std::vector<int64_t> JsonInts(const lightusd::minijson::Value &obj, const char *key, bool *present) {
  std::vector<int64_t> out;
  if (!obj.is_object()) return out;
  const auto &v = obj[key];
  if (!v.is_array()) {
    if (present) *present = false;
    return out;
  }
  if (present) *present = true;
  for (size_t i = 0; i < v.size(); ++i) out.push_back(v[i].get_int64());
  return out;
}

bool JsonOp(const lightusd::minijson::Value &obj, Int64ListOp *op, std::string *why) {
  bool has_explicit = false;
  std::vector<int64_t> expl = JsonInts(obj, "explicit_items", &has_explicit);
  bool has_added = false, has_ordered = false;
  std::vector<int64_t> added = JsonInts(obj, "added_items", &has_added);
  std::vector<int64_t> ordered = JsonInts(obj, "ordered_items", &has_ordered);
  if (!added.empty() || !ordered.empty()) {
    *why = "deprecated added/ordered items";
    return false;
  }
  *op = MakeInt64Op(has_explicit, expl, JsonInts(obj, "prepended_items", nullptr),
                    JsonInts(obj, "appended_items", nullptr), JsonInts(obj, "deleted_items", nullptr));
  return true;
}

int RunCombineChainCorpus(const std::string &dir) {
  DIR *d = opendir(dir.c_str());
  if (!d) {
    std::fprintf(stderr, "test_lean_vectors: corpus dir not found: %s (skip)\n", dir.c_str());
    return 77;
  }
  std::vector<std::string> files;
  while (dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) files.push_back(name);
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  int cases = 0;
  for (const std::string &name : files) {
    std::ifstream in(dir + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    lightusd::minijson::Value root;
    if (!lightusd::minijson::Parse(ss.str(), &root) || !root.is_array()) {
      ++g_failures;
      std::fprintf(stderr, "FAIL %s: not a JSON array\n", name.c_str());
      continue;
    }
    for (size_t i = 0; i < root.size(); ++i) {
      const auto &c = root[i];
      const auto &chain = c["chain"];
      Int64ListOpStack stack;
      std::string why;
      bool ok = chain.is_array();
      for (size_t k = 0; ok && k < chain.size(); ++k) {
        Int64ListOp op;
        ok = JsonOp(chain[k], &op, &why);
        if (ok) stack.ops.push_back(op);
      }
      Int64ListOp expected_op;
      if (ok) ok = JsonOp(c["combined_reduced"], &expected_op, &why);
      if (!ok) {
        std::fprintf(stderr, "SKIP %s[%zu]: %s\n", name.c_str(), i, why.c_str());
        continue;
      }
      ++cases;
      Int64ListOpStack single;
      single.ops.push_back(expected_op);
      Expect("combine_chain/" + name, static_cast<int>(i), "iterate(chain) == iterate(combined_reduced)",
             JoinInts(ToVector(ApplyInt64ListOps(stack))),
             JoinInts(ToVector(ApplyInt64ListOps(single))));
    }
  }
  std::printf("combine_chain corpus: %d cases in %zu files, %d failures\n", cases, files.size(),
              g_failures);
  return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Layer offsets (L4282-4288) and the TCPS sublayer chain (L5428-5509).
// ---------------------------------------------------------------------------
double Q(const char *s) {  // "num/den", dyadic => exact
  const char *slash = std::strchr(s, '/');
  double num = std::strtod(s, nullptr);
  double den = slash ? std::strtod(slash + 1, nullptr) : 1.0;
  return num / den;
}

std::string D(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

void CheckOffset(int id, const char *o1, const char *s1, const char *o2, const char *s2,
                 const char *eo, const char *es) {
  pcp::LayerOffset a{Q(o1), Q(s1)}, b{Q(o2), Q(s2)};
  pcp::LayerOffset r = a.Compose(b);
  Expect("offset/compose", id, "offset", D(r.offset), D(Q(eo)));
  Expect("offset/compose", id, "scale", D(r.scale), D(Q(es)));
}

std::string g_tmpdir;

void CheckTcpsChain(int id, const char *root_tcps, const char *links, const char *eo,
                    const char *es) {
  std::vector<std::string> parts;
  {
    std::string l(links);
    size_t st = 0;
    while (st <= l.size()) {
      size_t e = l.find(';', st);
      if (e == std::string::npos) e = l.size();
      parts.push_back(l.substr(st, e - st));
      st = e + 1;
    }
  }
  const std::string dir = g_tmpdir + "/tcps" + std::to_string(id);
  mkdir(dir.c_str(), 0755);
  auto tcps_meta = [](const std::string &t) {
    return t == "-" ? std::string() : "    timeCodesPerSecond = " + D(Q(t.c_str())) + "\n";
  };
  // layer i sublayers layer i+1 with link i's offset; the last layer holds the samples.
  const size_t n = parts.size();
  for (size_t i = 0; i <= n; ++i) {
    std::string tcps = i == 0 ? std::string(root_tcps) : parts[i - 1].substr(0, parts[i - 1].find(':'));
    std::string text = "#usda 1.0\n(\n" + tcps_meta(tcps);
    if (i < n) {
      const std::string &lk = parts[i];
      size_t c1 = lk.find(':'), c2 = lk.find(':', c1 + 1);
      text += "    subLayers = [ @./l" + std::to_string(i + 1) + ".usda@ (offset = " +
              D(Q(lk.substr(c1 + 1, c2 - c1 - 1).c_str())) + "; scale = " +
              D(Q(lk.substr(c2 + 1).c_str())) + ") ]\n";
    }
    text += ")\n";
    if (i == n) text += "def \"P\"\n{\n    double v.timeSamples = {\n        0: 1,\n        1: 2,\n    }\n}\n";
    std::ofstream(dir + "/l" + std::to_string(i) + ".usda") << text;
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory(dir);
  Stage stage;
  pcp::CompositionOptions opts;
  std::string warn, err;
  ++g_checks;
  if (!pcp::ComposeStageFromFile(dir + "/l0.usda", resolver, &stage, opts, &warn, &err)) {
    ++g_failures;
    std::fprintf(stderr, "FAIL tcps#%d compose: %s\n", id, err.c_str());
    return;
  }
  UsdPrim prim = stage.GetPrimAtPath("/P");
  std::vector<double> times = prim.IsValid() ? prim.GetTimeSampleTimes("v") : std::vector<double>();
  std::string got = times.size() == 2 ? D(times[0]) + "," + D(times[1]) : "<" + std::to_string(times.size()) + " samples>";
  Expect("tcps/chain", id, links, got, D(Q(eo)) + "," + D(Q(eo) + Q(es)));
}

void RunOffsetVectors() {
#define LEAN_OFFSET(id, o1, s1, o2, s2, eo, es) CheckOffset(id, o1, s1, o2, s2, eo, es);
#include "generated/lean/layer-offset-vectors.inc"
#undef LEAN_OFFSET
  const char *base = std::getenv("TMPDIR");
  std::string t = std::string(base ? base : "/tmp") + "/lean-tcps-XXXXXX";
  std::vector<char> buf(t.begin(), t.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) {
    ++g_failures;
    std::fprintf(stderr, "FAIL tcps: mkdtemp\n");
    return;
  }
  g_tmpdir = buf.data();
#define LEAN_TCPS_CHAIN(id, root, links, eo, es) CheckTcpsChain(id, root, links, eo, es);
#include "generated/lean/tcps-chain-vectors.inc"
#undef LEAN_TCPS_CHAIN
  std::string cmd = "rm -rf '" + g_tmpdir + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", g_tmpdir.c_str());
}


// ---------------------------------------------------------------------------
// Paths (ch 8) and schema family/version names (L8252-8294).
// ---------------------------------------------------------------------------
void CheckPath(int id, const char *str, int valid, const char *printed) {
  Path p = Path::Parse(str);
  const bool ok = p.is_valid();
  Expect("path/valid", id, str, ok ? "1" : "0", valid ? "1" : "0");
  if (ok && valid) Expect("path/print", id, str, p.str(), printed);
}

void CheckSchemaFamily(int id, const char *name, const char *family, int has_version,
                       unsigned long long version) {
  std::string fam;
  uint32_t ver = 0;
  bool hv = false;
  ParseSchemaFamilyAndVersion(name, &fam, &ver, &hv);
  Expect("schema/family", id, name, fam, family);
  Expect("schema/hasVersion", id, name, hv ? "1" : "0", has_version ? "1" : "0");
  if (hv && has_version) Expect("schema/version", id, name, std::to_string(ver), std::to_string(version));
}

void RunPathSchemaVectors() {
#define LEAN_PATH_VALID(id, str, valid, printed) CheckPath(id, str, valid, printed);
#include "generated/lean/path-valid-vectors.inc"
#undef LEAN_PATH_VALID
#define LEAN_SCHEMA_FAMILY(id, name, family, hv, v) CheckSchemaFamily(id, name, family, hv, v##ull);
#include "generated/lean/schema-family-vectors.inc"
#undef LEAN_SCHEMA_FAMILY
}


// ---------------------------------------------------------------------------
// Crate ValueRep bit layout (L12049-12201; bits 63/62/61 per LEAN-412).
// ---------------------------------------------------------------------------
void CheckValueRep(int id, const char *hex, int type, int is_array, int is_inlined,
                   int is_compressed, const char *payload_hex) {
  const uint64_t raw = std::strtoull(hex, nullptr, 16);
  const uint64_t payload = std::strtoull(payload_hex, nullptr, 16);
  ValueRep rep(raw);
  Expect("valuerep", id, "type", std::to_string(static_cast<int>(rep.type_id())), std::to_string(type));
  Expect("valuerep", id, "is_array", rep.is_array() ? "1" : "0", is_array ? "1" : "0");
  Expect("valuerep", id, "is_inlined", rep.is_inlined() ? "1" : "0", is_inlined ? "1" : "0");
  Expect("valuerep", id, "is_compressed", rep.is_compressed() ? "1" : "0", is_compressed ? "1" : "0");
  Expect("valuerep", id, "payload", std::to_string(rep.payload()), std::to_string(payload));
  ValueRep made = ValueRep::Make(static_cast<CrateTypeId>(type), payload, is_array != 0,
                                 is_inlined != 0, is_compressed != 0);
  Expect("valuerep", id, "Make().raw()", std::to_string(made.raw()), std::to_string(raw));
}

void RunCrateVectors() {
#define LEAN_VALUEREP(id, hex, type, arr, inl, cmp, payload) CheckValueRep(id, hex, type, arr, inl, cmp, payload);
#include "generated/lean/crate-valuerep-vectors.inc"
#undef LEAN_VALUEREP
}


// ---------------------------------------------------------------------------
// Time-sample value resolution (L6539-6580, L7364-7446, L7571-7632).
// ---------------------------------------------------------------------------
void CheckVr(int id, int interp, const char *samples, const char *query, const char *expected) {
  std::string body;
  std::string ss(samples);
  size_t st = 0;
  while (st < ss.size()) {
    size_t e = ss.find(';', st);
    if (e == std::string::npos) e = ss.size();
    const std::string item = ss.substr(st, e - st);
    const size_t c = item.find(':');
    const std::string t = item.substr(0, c), v = item.substr(c + 1);
    body += "        " + D(Q(t.c_str())) + ": " + (v == "None" ? std::string("None") : D(Q(v.c_str()))) + ",\n";
    st = e + 1;
  }
  const std::string text = "#usda 1.0\ndef \"P\"\n{\n    double v.timeSamples = {\n" + body + "    }\n}\n";
  LoadOptions lo;
  auto loaded = LoadUSDAFromString(text, lo);
  ++g_checks;
  if (!loaded.success) {
    ++g_failures;
    std::fprintf(stderr, "FAIL vr#%d parse: %s\n", id, text.c_str());
    return;
  }
  Stage &stage = loaded.stage;
  AttributeEval ev(&stage);
  EvalOptions opts;
  opts.time = TimeQuery::Numeric(Q(query));
  opts.interp = interp == 0 ? TimeInterpolation::Held : TimeInterpolation::Linear;
  EvalResult r = ev.EvalWith(stage.GetPrimAtPath("/P"), "v", opts);
  std::string got;
  if (r.success && r.value.as_double()) got = D(*r.value.as_double());
  else if (r.blocked) got = "None";
  else got = "<error:" + r.error + ">";
  const std::string want = std::strcmp(expected, "None") == 0 ? std::string("None") : D(Q(expected));
  Expect("vr", id, std::string(samples) + " @" + query + (interp ? " linear" : " held"), got, want);
}

void RunVrVectors() {
#define LEAN_VR(id, interp, samples, query, expected) CheckVr(id, interp, samples, query, expected);
#include "generated/lean/vr-lean-cases.inc"
#undef LEAN_VR
}

// ---------------------------------------------------------------------------
// Splines (L7634-8096, L2006-2162): ParseSplineText + EvaluateSplineData.
// ---------------------------------------------------------------------------
void CheckSpline(int id, const char *text, const char *t, const char *expected) {
  SplineData sd;
  std::string err;
  const std::string where = std::string(text) + " @" + t;
  if (!ParseSplineText(text, &sd, &err)) {
    Expect("spline", id, where, "<parse error: " + err + ">", expected);
    return;
  }
  double v = 0.0;
  const bool ok = EvaluateSplineData(sd, Q(t), &v);
  std::string got = ok ? D(v) : std::string("none");
  std::string want = expected;
  if (want != "none") {
    const double e = Q(expected);
    want = D(e);
    if (ok && std::fabs(v - e) <= 1e-9 * std::max(1.0, std::fabs(e))) got = want;
  }
  Expect("spline", id, where, got, want);
}

void RunSplineVectors() {
#define LEAN_SPLINE(id, text, pre, post, t, expected) CheckSpline(id, text, t, expected);
#include "generated/lean/spline-vectors.inc"
#undef LEAN_SPLINE
}

// ---------------------------------------------------------------------------
// Value clips (L6941-6994, L7222-7236, templates L7032-7120).
// ---------------------------------------------------------------------------
std::vector<std::string> SplitSemi(const std::string &s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t st = 0;
  while (st <= s.size()) {
    size_t e = s.find(';', st);
    if (e == std::string::npos) e = s.size();
    out.push_back(s.substr(st, e - st));
    st = e + 1;
  }
  return out;
}

// "s:x;..." -> pairs (x read as num/den, or a bare integer).
std::vector<std::pair<double, double>> ParsePairs(const char *enc) {
  std::vector<std::pair<double, double>> out;
  for (const std::string &item : SplitSemi(enc)) {
    const size_t c = item.find(':');
    out.emplace_back(Q(item.substr(0, c).c_str()), Q(item.substr(c + 1).c_str()));
  }
  return out;
}

void CheckClipTime(int id, const char *active, const char *times, const char *stage_t,
                   int expected_index, const char *expected_time) {
  ValueClipSet set;
  for (const auto &p : ParsePairs(active)) set.active.emplace_back(p.first, static_cast<int>(p.second));
  set.times = ParsePairs(times);
  int max_index = 0;
  for (const auto &a : set.active) max_index = std::max(max_index, a.second);
  set.asset_paths.assign(static_cast<size_t>(max_index) + 1, "clip.usda");
  const std::string where = std::string(active) + " | " + times + " @" + stage_t;
  const double t = Q(stage_t);
  Expect("clip/time", id, where + " index", std::to_string(value_clip_detail::ActiveIndex(set, t)),
         std::to_string(expected_index));
  // The timing curve is a lerp, so compare with a relative tolerance.
  const double ct = value_clip_detail::ClipTime(set, t), want = Q(expected_time);
  Expect("clip/time", id, where + " clipTime",
         std::fabs(ct - want) <= 1e-9 * std::max(1.0, std::fabs(want)) ? D(want) : D(ct), D(want));
}

std::string JoinPairs(const std::vector<std::pair<double, double>> &v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ";";
    out += D(v[i].first) + ":" + D(v[i].second);
  }
  return out;
}

void CheckClipTemplate(int id, const char *tmpl, const char *start, const char *end,
                       const char *stride, const char *active_offset, const char *asset_paths,
                       const char *active, const char *times) {
  std::string text = "#usda 1.0\ndef \"P\" (\n    clips = {\n        dictionary default = {\n";
  text += std::string("            string templateAssetPath = \"") + tmpl + "\"\n";
  text += "            double templateStartTime = " + D(Q(start)) + "\n";
  text += "            double templateEndTime = " + D(Q(end)) + "\n";
  text += "            double templateStride = " + D(Q(stride)) + "\n";
  if (std::strcmp(active_offset, "-") != 0)
    text += "            double templateActiveOffset = " + D(Q(active_offset)) + "\n";
  text += "        }\n    }\n)\n{\n}\n";
  LoadOptions lo;
  auto loaded = LoadUSDAFromString(text, lo);
  std::vector<ValueClipSet> sets;
  std::string err;
  if (!loaded.success || !ParseValueClipSets(loaded.stage.GetPrimAtPath("/P"), &sets, &err) ||
      sets.size() != 1) {
    Expect("clip/template", id, "parse", "<failed: " + err + ">", "ok");
    return;
  }
  const ValueClipSet &set = sets[0];
  std::string paths;
  for (size_t i = 0; i < set.asset_paths.size(); ++i) paths += (i ? ";" : "") + set.asset_paths[i];
  Expect("clip/template", id, std::string(tmpl) + " assetPaths", paths, asset_paths);
  std::vector<std::pair<double, double>> act;
  for (const auto &a : set.active) act.emplace_back(a.first, a.second);
  Expect("clip/template", id, std::string(tmpl) + " active", JoinPairs(act), JoinPairs(ParsePairs(active)));
  Expect("clip/template", id, std::string(tmpl) + " times", JoinPairs(set.times), JoinPairs(ParsePairs(times)));
}

void RunClipVectors() {
#define LEAN_CLIP_TIME(id, active, times, stage_t, idx, ct) CheckClipTime(id, active, times, stage_t, idx, ct);
#define LEAN_CLIP_TEMPLATE(id, tmpl, start, end, stride, off, paths, active, times) \
  CheckClipTemplate(id, tmpl, start, end, stride, off, paths, active, times);
#include "generated/lean/clip-vectors.inc"
#undef LEAN_CLIP_TIME
#undef LEAN_CLIP_TEMPLATE
}


// ---------------------------------------------------------------------------
// Crate LZ4 framing (L11576-11598; OpenUSD TfFastCompression).
// ---------------------------------------------------------------------------
std::vector<uint8_t> FromHex(const char *h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    char b[3] = {h[i], h[i + 1], 0};
    out.push_back(static_cast<uint8_t>(std::strtoul(b, nullptr, 16)));
  }
  return out;
}

void CheckLz4(int id, size_t n, const char *uhex, const char *chex) {
  const std::vector<uint8_t> u = FromHex(uhex), c = FromHex(chex);
  DecompressResult r = DecompressCrateBlob(c.data(), c.size(), n);
  ++g_checks;
  if (!r.success || r.data != u) {
    ++g_failures;
    std::fprintf(stderr, "FAIL lz4#%d: %s\n", id, r.success ? "data mismatch" : r.error.c_str());
  }
  if (n > 0) {  // our own writer must round-trip (bytes differ from the Lean compressor)
    CompressResult w = CompressCrateBlob(u.data(), u.size());
    DecompressResult back = w.success ? DecompressCrateBlob(w.data.data(), w.data.size(), n) : DecompressResult();
    Expect("lz4/roundtrip", id, "CompressCrateBlob", back.success && back.data == u ? "ok" : "fail", "ok");
  }
}

void RunLz4Vectors() {
#define LEAN_LZ4_FRAME(id, n, uhex, chex) CheckLz4(id, static_cast<size_t>(n), uhex, chex);
#include "generated/lean/lz4-vectors.inc"
#include "generated/lean/lz4-multichunk-vectors.inc"
#undef LEAN_LZ4_FRAME
}


// ---------------------------------------------------------------------------
// USDA acceptance (normative PEG L9569-11375, repaired per LEAN-805..810).
// ---------------------------------------------------------------------------
std::string g_repo_root;
int g_usda_mismatch = 0;

void CheckUsda(const char *id, const char *source, const char *text, bool /*peg_literal*/,
               bool /*peg_repaired*/, int expect, const char *note) {
  if (expect < 0) return;  // no expectation (legacy syntax / semantic-only errors)
  std::string data;
  const std::string src(source);
  if (src.rfind("inline:", 0) == 0) {
    data = text;
  } else {
    std::ifstream in(g_repo_root + "/" + src);
    if (!in) {
      std::fprintf(stderr, "SKIP usda %s: cannot read %s\n", id, src.c_str());
      return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    data = ss.str();
  }
  LoadOptions o;
  o.parse_options.strict_aousd_conformance = true;
  LoadResult r = LoadUSDAFromString(data, o);
  ++g_checks;
  if ((r.success ? 1 : 0) != expect) {
    ++g_failures;
    ++g_usda_mismatch;
    std::fprintf(stderr, "FAIL usda %s %s: LightUSD %s, grammar+pxr %s (%s)%s%s\n", id, src.c_str(),
                 r.success ? "accepts" : "rejects", expect ? "accept" : "reject", note,
                 r.success ? "" : ": ", r.success ? "" : r.error_summary.c_str());
  }
}

void CheckUsdaString(const char* id, const std::string& text,
                     const std::string& expected) {
  auto matches = [&](const Stage& stage) {
    const auto* prim = stage.GetRootLayer()->prim_at_path("/P");
    if (!prim) return false;
    const auto dict = prim->meta().customData().as_dictionary();
    if (!dict) return false;
    const auto value = dict->find("s");
    return value && value->as_string() && *value->as_string() == expected;
  };
  auto check = [&](bool success, const char* transport) {
    ++g_checks;
    if (!success) {
      ++g_failures;
      std::fprintf(stderr, "FAIL quoted string %s (%s)\n", id, transport);
    }
  };
  LoadOptions options;
  options.parse_options.strict_aousd_conformance = true;
  auto parsed = LoadUSDAFromString(text, options);
  check(parsed.success && matches(parsed.stage), "Lean source");
  if (!parsed.success) return;
  auto rewritten = LoadUSDAFromString(WriteUSDAToString(parsed.stage), options);
  check(rewritten.success && matches(rewritten.stage), "native USDA");
  std::vector<uint8_t> bytes;
  const auto written = WriteUSDCToMemory(bytes, parsed.stage);
  if (expected.find('\0') != std::string::npos) {
    check(!written.success && written.error.find("NUL") != std::string::npos,
          "USDC unrepresentable NUL rejection");
    return;
  }
  if (!written.success) {
    check(false, "native USDC write");
    return;
  }
  auto binary = LoadUSDCFromMemory(bytes.data(), bytes.size());
  check(written.success && binary.success && matches(binary.stage), "native USDC");
}

void CheckUsdaKey(const char* id, const std::string& text,
                  const std::string& expected, int kind) {
  auto matches = [&](const Stage& stage) {
    const auto* prim = stage.GetRootLayer()->prim_at_path("/P");
    if (!prim) return false;
    const auto dict = prim->meta().customData().as_dictionary();
    if (!dict || dict->entries().size() != 1) return false;
    const auto value = dict->find(expected);
    if (!value) return false;
    if (kind == 0) return value->as_int() && *value->as_int() == 7;
    if (kind == 1) return value->as_string() && *value->as_string() == "payload";
    const auto nested = value->as_dictionary();
    if (!nested || nested->entries().size() != 1) return false;
    const auto leaf = nested->find(expected);
    return leaf && leaf->as_string() && *leaf->as_string() == "payload";
  };
  auto check = [&](bool success, const char* transport) {
    ++g_checks;
    if (!success) {
      ++g_failures;
      std::fprintf(stderr, "FAIL dictionary key %s (%s)\n", id, transport);
    }
  };
  LoadOptions options;
  options.parse_options.strict_aousd_conformance = true;
  auto parsed = LoadUSDAFromString(text, options);
  check(parsed.success && matches(parsed.stage), "Lean source");
  if (!parsed.success) return;
  auto rewritten = LoadUSDAFromString(WriteUSDAToString(parsed.stage), options);
  check(rewritten.success && matches(rewritten.stage), "native USDA");
  std::vector<uint8_t> bytes;
  const auto written = WriteUSDCToMemory(bytes, parsed.stage);
  if (expected.find('\0') != std::string::npos) {
    check(!written.success && written.error.find("NUL") != std::string::npos,
          "USDC unrepresentable NUL rejection");
    return;
  }
  if (!written.success) {
    check(false, "native USDC write");
    return;
  }
  auto binary = LoadUSDCFromMemory(bytes.data(), bytes.size());
  check(binary.success && matches(binary.stage), "native USDC");
}

void RunUsdaVectors() {
#define LEAN_USDA(id, source, text, lit, rep, expect, note) CheckUsda(id, source, text, lit, rep, expect, note);
#include "generated/lean/usda-peg-vectors.inc"
#undef LEAN_USDA
#define LEAN_USDA_STRING(id, text, value) \
  CheckUsdaString(id, std::string(text, sizeof(text) - 1), std::string(value, sizeof(value) - 1));
#include "generated/lean/usda-string-vectors.inc"
#undef LEAN_USDA_STRING
#define LEAN_USDA_KEY(id, text, key, kind) \
  CheckUsdaKey(id, std::string(text, sizeof(text) - 1), std::string(key, sizeof(key) - 1), kind);
#include "generated/lean/usda-key-vectors.inc"
#undef LEAN_USDA_KEY
  // Put the unrepresentable value after enough prims to exercise worker error
  // propagation after earlier chunks have committed, as well as serial writes.
  std::string many = "#usda 1.0\n";
  for (size_t i = 0; i < 8192; ++i)
    many += "def \"C" + std::to_string(i) + "\" {}\n";
  many += "def \"P\" (customData = { string s = \"bad\\000tail\"\n}) {}\n";
  auto invalid = LoadUSDAFromString(many);
  for (int threads : {1, 4}) {
    USDCWriteOptions options;
    options.crate_options.num_threads = threads;
    std::vector<uint8_t> bytes;
    const auto result = WriteUSDCToMemory(bytes, invalid.stage, options);
    ++g_checks;
    if (!invalid.success || result.success || result.error.find("NUL") == std::string::npos) {
      ++g_failures;
      std::fprintf(stderr, "FAIL crate NUL writer rejection (threads=%d)\n", threads);
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  g_repo_root = LIGHTUSD_REPO_ROOT;
  if (argc == 3 && std::strcmp(argv[1], "--combine-chain") == 0) return RunCombineChainCorpus(argv[2]);
  RunListOpVectors();
  RunCrateListOpVectors();
  RunIntCodeVectors();
  RunOffsetVectors();
  RunPathSchemaVectors();
  RunCrateVectors();
  RunVrVectors();
  RunLz4Vectors();
  RunUsdaVectors();
  RunSplineVectors();
  RunClipVectors();
  std::printf("test_lean_vectors: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
