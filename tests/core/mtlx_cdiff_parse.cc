// SPDX-License-Identifier: Apache 2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Differential harness: the C11 MaterialX parse/value/scope/include code of
// src/c/mtlx/ (mtlx-xml.c, mtlx-value.c, mtlx-scope.c, mtlx-include.c)
// against the original C++ implementation (src/core/mtlx-resolve.cc,
// mtlx-dom.cc, src/mtlx-xml-parser.cc).
//
// Usage: mtlx_cdiff_parse [oracle_dir]   (default /mnt/nvme01/tmp/mtlx-c11/oracle)
// The corpus is the file named on the first line ("# <path>") of every
// oracle *.txt. Exit status is non-zero on any mismatch.

#include <dirent.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "c/mtlx/mtlx-internal.h"
#include "core/mtlx-resolve.hh"
#include "mtlx-xml-parser.hh"

using namespace lightusd::mtlx;

namespace {

size_t g_checks = 0, g_fail = 0, g_hangs = 0;

void Check(bool ok, const std::string &what) {
  g_checks++;
  if (!ok) {
    g_fail++;
    if (g_fail < 60) std::fprintf(stderr, "MISMATCH: %s\n", what.c_str());
  }
}

std::string Short(const std::string &s) {
  std::string o = s.size() > 120 ? s.substr(0, 120) + "..." : s;
  for (char &c : o)
    if (c == '\n' || c == '\r') c = ' ';
  return o;
}

std::string Sb(const lu_mtlx_sb &b) { return lu_mtlx_sb_str(&b); }

// ---- copies of anonymous-namespace C++ code of mtlx-dom.cc --------------

void AppendXmlEscaped(std::ostringstream *out, const std::string &text) {
  for (char c : text) {
    switch (c) {
      case '&': *out << "&amp;"; break;
      case '<': *out << "&lt;"; break;
      case '>': *out << "&gt;"; break;
      case '"': *out << "&quot;"; break;
      case '\'': *out << "&apos;"; break;
      default: *out << c; break;
    }
  }
}

void AppendXmlNode(std::ostringstream *out, const XMLNodePtr &node) {
  if (!out || !node) return;
  *out << '<' << node->GetName();
  for (const auto &attr : node->GetAttributes()) {
    *out << ' ' << attr.first << "=\"";
    AppendXmlEscaped(out, attr.second);
    *out << '"';
  }
  const auto &children = node->GetChildren();
  if (children.empty() && node->GetText().empty()) {
    *out << "/>";
    return;
  }
  *out << '>';
  AppendXmlEscaped(out, node->GetText());
  for (const XMLNodePtr &child : children) AppendXmlNode(out, child);
  *out << "</" << node->GetName() << '>';
}

XMLNodePtr MtlxElemToXMLNode(const MtlxElem &elem) {
  auto node = std::make_shared<XMLNode>(elem.tag);
  for (const auto &attr : elem.attrs) node->SetAttribute(attr.first, attr.second);
  for (const MtlxElem &child : elem.children) node->AddChild(MtlxElemToXMLNode(child));
  return node;
}

MtlxElem XMLNodeToMtlxElem(XMLNodePtr node) {
  MtlxElem elem;
  if (!node) return elem;
  elem.tag = node->GetName();
  for (const auto &attr : node->GetAttributes()) elem.attrs.emplace_back(attr.first, attr.second);
  for (const XMLNodePtr &child : node->GetChildren())
    elem.children.push_back(XMLNodeToMtlxElem(child));
  return elem;
}

std::string GetResolvedXML(const MtlxElem &root) {
  std::ostringstream out;
  out << "<?xml version=\"1.0\"?>";
  AppendXmlNode(&out, MtlxElemToXMLNode(root));
  return out.str();
}

std::string FormatMtlxFloat(float value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
  return out.str();
}

// ---- tree conversion / comparison -----------------------------------------

MtlxElem FromC(const lu_mtlx_elem &e) {
  MtlxElem o;
  o.tag = e.tag;
  for (size_t i = 0; i < e.nattrs; i++) o.attrs.emplace_back(e.attrs[i].key, e.attrs[i].value);
  for (size_t i = 0; i < e.nchildren; i++) o.children.push_back(FromC(e.children[i]));
  return o;
}

void ToC(const MtlxElem &s, lu_mtlx_elem *d) {  // d initialised
  lu_mtlx_elem_set_tag(d, s.tag.c_str());
  for (const auto &kv : s.attrs) lu_mtlx_attr_append(d, kv.first.c_str(), kv.second.c_str());
  for (const MtlxElem &c : s.children) {
    lu_mtlx_elem ce;
    lu_mtlx_elem_init(&ce);
    ToC(c, &ce);
    lu_mtlx_child_push(d, &ce);
  }
}

void Serialize(const MtlxElem &e, std::string *o) {
  *o += "<" + e.tag;
  for (const auto &kv : e.attrs) *o += "|" + kv.first + "=" + kv.second;
  *o += ">";
  for (const MtlxElem &c : e.children) Serialize(c, o);
  *o += "</>";
}

std::string Ser(const MtlxElem &e) {
  std::string o;
  Serialize(e, &o);
  return o;
}

// ---- XML comparisons --------------------------------------------------------

void CompareParseXml(const std::string &xml, const std::string &label) {
  MtlxElem cr;
  std::string cerr_;
  const bool cok = ParseMtlxXml(xml, &cr, &cerr_);
  lu_mtlx_elem r;
  lu_mtlx_elem_init(&r);
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  const int ok = lu_mtlx_parse_xml(xml.data(), xml.size(), &r, &err);
  Check(cok == (ok != 0), "parse_xml ok " + label);
  if (cok && ok) {
    Check(Ser(cr) == Ser(FromC(r)), "parse_xml tree " + label);
    // Writer and normalisation.
    lu_mtlx_sb out;
    lu_mtlx_sb_init(&out);
    Check(lu_mtlx_write_xml(&r, &out) && Sb(out) == GetResolvedXML(cr), "write_xml " + label);
    lu_mtlx_sb_free(&out);
    const std::string want = Ser(XMLNodeToMtlxElem(MtlxElemToXMLNode(cr)));
    Check(lu_mtlx_xmlnode_normalize(&r) && Ser(FromC(r)) == want, "normalize " + label);
    lu_mtlx_sb_init(&out);
    Check(lu_mtlx_write_xml(&r, &out) && Sb(out) == GetResolvedXML(cr), "write_xml(norm) " + label);
    lu_mtlx_sb_free(&out);
  } else if (!cok && !ok) {
    Check(cerr_ == Sb(err), "parse_xml err " + label + ": '" + cerr_ + "' vs '" + Sb(err) + "'");
  }
  lu_mtlx_elem_free(&r);
  lu_mtlx_sb_free(&err);
}

// Returns a serialized outcome string of the class parser (C++ or C).
std::string ClassOutcomeCpp(const std::string &xml) {
  MaterialXParser parser;
  if (!parser.Parse(xml)) return "ERR:" + parser.GetError();
  return "OK:" + parser.GetWarning() + "\n" +
         Ser(XMLNodeToMtlxElem(parser.GetDocument().GetRoot()));
}

std::string ClassOutcomeC(const std::string &xml) {
  lu_mtlx_elem r;
  lu_mtlx_elem_init(&r);
  lu_mtlx_sb err, warn;
  lu_mtlx_sb_init(&err);
  lu_mtlx_sb_init(&warn);
  std::string res;
  if (!lu_mtlx_parse_xml_checked(xml.data(), xml.size(), &r, &err, &warn))
    res = "ERR:" + Sb(err);
  else
    res = "OK:" + Sb(warn) + "\n" + Ser(FromC(r));
  lu_mtlx_elem_free(&r);
  lu_mtlx_sb_free(&err);
  lu_mtlx_sb_free(&warn);
  return res;
}

void CompareClassDirect(const std::string &xml, const std::string &label) {
  const std::string a = ClassOutcomeCpp(xml), b = ClassOutcomeC(xml);
  Check(a == b, "class parser " + label + ": " + Short(a) + " vs " + Short(b));
}

// Runs fn in a forked child under a 0.4 s real-time timer. 0 ok, 1 differs, 2 timeout.
template <typename F>
int Forked(F fn) {
  const pid_t pid = fork();
  if (pid == 0) {
    itimerval tv = {{0, 0}, {0, 400000}};  // 0.4 s
    setitimer(ITIMER_REAL, &tv, nullptr);
    _exit(fn() ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (getenv("CDIFF_TRACE")) std::fprintf(stderr, " fork status %x\n", st);
  if (WIFEXITED(st)) return WEXITSTATUS(st) ? 1 : 0;
  return 2;
}

// The C++ class parser loops forever on some malformed tags (an attribute it
// cannot parse is retried without consuming input). The C port reports
// "Malformed attribute in <tag>" instead: where the C++ hangs, the C side must
// terminate with that error. Fuzz cases run forked under a timer.
void CompareClassForked(const std::string &xml, const std::string &label) {
  const int r = Forked([&] { return ClassOutcomeCpp(xml) == ClassOutcomeC(xml); });
  if (r == 2) {
    g_hangs++;
    const int a = Forked([&] { ClassOutcomeCpp(xml); return true; });
    const int b = Forked(
        [&] { return ClassOutcomeC(xml).rfind("ERR:Malformed attribute in <", 0) == 0; });
    Check(a == 2 && b == 0, "class parser: C++ hang -> C error " + label);
    return;
  }
  Check(r == 0, "class parser (forked) " + label + ": " + Short(xml));
}

// ---- scope -----------------------------------------------------------------

void CompareScopesRec(const MtlxElem &ce, const lu_mtlx_elem &e, std::vector<const MtlxElem *> &cchain,
                      std::vector<const lu_mtlx_elem *> &chain, const std::string &label) {
  cchain.insert(cchain.begin(), &ce);
  chain.insert(chain.begin(), &e);
  lu_mtlx_scope s;
  s.v = chain.data();
  s.n = s.cap = chain.size();
  for (const char *k : {"colorspace", "fileprefix", "namespace", "name", "nope"}) {
    std::string cv;
    const bool a = ScopedMtlxAttr(k, cchain, &cv);
    const char *v = nullptr;
    const int b = lu_mtlx_scoped_attr(k, &s, &v);
    Check(a == (b != 0) && (!a || cv == v), "scoped_attr " + label);
  }
  {
    std::string cv;
    const bool a = ActiveMtlxColorSpace(cchain, &cv);
    lu_mtlx_sb o;
    lu_mtlx_sb_init(&o);
    const int b = lu_mtlx_active_colorspace(&s, &o);
    Check(a == (b != 0) && (!a || cv == Sb(o)), "active_colorspace " + label);
    lu_mtlx_sb_clear(&o);
    const bool a2 = ResolvedMtlxFilename(cchain, &cv);
    const int b2 = lu_mtlx_resolved_filename(&s, &o);
    Check(a2 == (b2 != 0) && (!a2 || cv == Sb(o)), "resolved_filename " + label + " " + cv);
    lu_mtlx_sb_free(&o);
  }
  for (size_t i = 0; i < e.nchildren; i++)
    CompareScopesRec(ce.children[i], e.children[i], cchain, chain, label);
  cchain.erase(cchain.begin());
  chain.erase(chain.begin());
}

void CompareScopes(const std::string &xml, const std::string &label) {
  MtlxElem cr;
  if (!ParseMtlxXml(xml, &cr, nullptr)) return;
  lu_mtlx_elem r;
  lu_mtlx_elem_init(&r);
  ToC(cr, &r);
  std::vector<const MtlxElem *> cc;
  std::vector<const lu_mtlx_elem *> c;
  CompareScopesRec(cr, r, cc, c, label);
  lu_mtlx_elem_free(&r);
}

// ---- includes --------------------------------------------------------------

struct Files {
  std::map<std::string, std::string> mem;  // href -> text (in-memory)
  std::string base_dir;                    // when non-empty, read from disk
  bool Load(const std::string &href, const std::string &including, std::string *xml) const {
    auto it = mem.find(href);
    if (it != mem.end()) {
      *xml = it->second;
      return true;
    }
    if (base_dir.empty()) return false;
    std::string dir = base_dir;
    if (!including.empty()) {
      const std::string p = including.find('/') != std::string::npos
                                ? including.substr(0, including.rfind('/') + 1)
                                : "";
      dir = base_dir + p;
    }
    std::ifstream f(dir + href, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *xml = ss.str();
    return true;
  }
};

int CLoader(void *user, const char *href, const char *including, char **xml, size_t *len) {
  std::string t;
  if (!static_cast<const Files *>(user)->Load(href, including, &t)) return 0;
  *xml = static_cast<char *>(std::malloc(t.size() + 1));
  std::memcpy(*xml, t.data(), t.size());
  (*xml)[t.size()] = '\0';
  *len = t.size();
  return 1;
}

void CompareIncludes(const std::string &main, const Files &files, const std::string &label,
                     bool null_loader = false) {
  MtlxElem cr;
  if (!ParseMtlxXml(main, &cr, nullptr)) return;
  lu_mtlx_elem r;
  lu_mtlx_elem_init(&r);
  ToC(cr, &r);
  std::string cerr_;
  MtlxAnchoredIncludeLoader cl;
  if (!null_loader)
    cl = [&files](const std::string &h, const std::string &i, std::string *x) {
      return files.Load(h, i, x);
    };
  const bool a = ExpandMtlxIncludes(&cr, cl, &cerr_);
  lu_mtlx_sb err;
  lu_mtlx_sb_init(&err);
  const int b = lu_mtlx_expand_includes(&r, null_loader ? nullptr : CLoader,
                                        const_cast<Files *>(&files), &err);
  Check(a == (b != 0), "includes ok " + label);
  if (!a && !b) Check(cerr_ == Sb(err), "includes err " + label + ": " + cerr_ + " vs " + Sb(err));
  Check(Ser(cr) == Ser(FromC(r)), "includes tree " + label);
  lu_mtlx_elem_free(&r);
  lu_mtlx_sb_free(&err);
}

// ---- values ----------------------------------------------------------------

std::string SerValue(const MtlxTypedValue &v) {
  std::ostringstream o;
  o << std::hexfloat << int(v.kind) << "|" << v.i << "|" << v.b << "|" << v.num << "|";
  for (double d : v.nums) o << d << ",";
  o << "|";
  for (int64_t d : v.ints) o << d << ",";
  o << "|" << v.str << "|";
  for (const auto &s : v.strs) o << s << "\x1f";
  return o.str();
}

std::string SerValue(const lu_mtlx_typed_value &v) {
  MtlxTypedValue t;
  t.kind = MtlxValueKind(v.kind);
  t.i = v.i;
  t.b = v.b != 0;
  t.num = v.num;
  t.nums.assign(v.nums, v.nums + v.nnums);
  t.ints.assign(v.ints, v.ints + v.nints);
  t.str = v.str ? v.str : "";
  for (size_t i = 0; i < v.strs.n; i++) t.strs.push_back(v.strs.v[i]);
  return SerValue(t);
}

const char *const kTypes[] = {
    "integer", "boolean", "float", "color3", "color4", "vector2", "vector3", "vector4",
    "matrix33", "matrix44", "string", "filename", "integerarray", "floatarray", "color3array",
    "color4array", "vector2array", "vector3array", "vector4array", "stringarray", "color2",
    "surfaceshader", "", "Float", "geomname"};

void CompareValue(const std::string &type, const std::string &text) {
  MtlxTypedValue cv;
  const bool a = ParseMtlxValue(type, text, &cv);
  lu_mtlx_typed_value v;
  lu_mtlx_typed_value_init(&v);
  const int b = lu_mtlx_parse_value(type.c_str(), text.c_str(), &v);
  Check(a == (b != 0) && SerValue(cv) == SerValue(v),
        "parse_value " + type + " '" + Short(text) + "': " + SerValue(cv) + " vs " + SerValue(v));
  lu_mtlx_typed_value_free(&v);
  lu_mtlx_sb o;
  lu_mtlx_sb_init(&o);
  Check(lu_mtlx_lex_value(type.c_str(), text.c_str(), &o) && Sb(o) == LexMtlxValue(type, text),
        "lex_value " + type + " '" + Short(text) + "'");
  lu_mtlx_sb_free(&o);
}

void CompareTypeFns(const std::string &type) {
  Check(MtlxTypeArity(type) == lu_mtlx_type_arity(type.c_str()), "arity " + type);
  Check(MtlxArrayStride(type) == lu_mtlx_array_stride(type.c_str()), "stride " + type);
  Check(IsBuiltinMtlxType(type) == (lu_mtlx_is_builtin_type(type.c_str()) != 0), "builtin " + type);
  MtlxTypedValue cv;
  const bool a = DefaultMtlxValue(type, &cv);
  lu_mtlx_typed_value v;
  lu_mtlx_typed_value_init(&v);
  const int b = lu_mtlx_default_value(type.c_str(), &v);
  Check(a == (b != 0) && SerValue(cv) == SerValue(v), "default " + type);
  lu_mtlx_typed_value_free(&v);
}

void CompareName(const std::string &n) {
  Check(IsValidMtlxName(n) == (lu_mtlx_is_valid_name(n.c_str()) != 0), "valid_name " + n);
  Check(IsReferenceValidMtlxName(n) == (lu_mtlx_is_reference_valid_name(n.c_str()) != 0),
        "ref_valid_name " + n);
}

void CompareFloatFormat(float f) {
  lu_mtlx_sb o;
  lu_mtlx_sb_init(&o);
  lu_mtlx_sb_puts(&o, "x");
  Check(lu_mtlx_format_float(f, &o) && Sb(o) == "x" + FormatMtlxFloat(f),
        "format_float " + FormatMtlxFloat(f) + " vs " + Sb(o));
  lu_mtlx_sb_free(&o);
}

void ValueSuite(std::mt19937_64 &rng) {
  std::vector<std::string> texts = {
      "", " ", "1", "-7", "+12", " 3 ", "0", "1.0", "1e3", "abc", "--1", "007", "true", "false",
      "True", " true", "-0", "-0.0", ".5", "5.", ".", "-.", "+.e1", "1e", "1e+", "1e-5", "1E5",
      "1.2.3", "1,2", "1, 2, 3", "1,2,3,", ",", " , ", "1,,2", "0.1,0.2,0.3,0.4",
      "9223372036854775807", "-9223372036854775808", "9223372036854775808",
      "-9223372036854775809", "99999999999999999999", "1e308", "1e309", "-1e400", "1e-400",
      "4.9e-324", "2.4703282292062327e-324", "1e99999999999999999999999", "0x10", "inf", "nan",
      "1 2", "1\t,\n2\r", "a\\,b,c", "a\\;b\\\\,c\\", " a , b ", "\\", "1,2,3,4,5,6,7,8,9",
      "1,0,0,0,1,0,0,0,1", "1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1", "0.30000000000000004",
      "123456789012345678901234567890.5e-10", "00000.000001e0000005", "1e-0", "+-1", "1_0"};
  std::uniform_int_distribution<int> len(0, 14), ch(0, 15);
  const char alphabet[] = "0123456789.,-+e ";
  for (int i = 0; i < 4000; i++) {
    std::string s;
    const int n = len(rng);
    for (int k = 0; k < n; k++) s.push_back(alphabet[ch(rng)]);
    texts.push_back(s);
  }
#define MTLX_VALUE(id, type, text, ok, kind, payload) texts.push_back(text);
#define MTLX_DEFAULT(type, kind, payload) CompareTypeFns(type);
#include "tests/core/generated/lean-mtlx/mtlx-value.inc"
#undef MTLX_VALUE
#undef MTLX_DEFAULT
  for (const char *t : kTypes) {
    CompareTypeFns(t);
    for (const std::string &x : texts) CompareValue(t, x);
  }
#define MTLX_VALUE(id, type, text, ok, kind, payload) CompareValue(type, text);
#define MTLX_DEFAULT(type, kind, payload)
#include "tests/core/generated/lean-mtlx/mtlx-value.inc"
#undef MTLX_VALUE
#undef MTLX_DEFAULT
#define MTLX_NAME(id, text, spec, ref) CompareName(text);
#include "tests/core/generated/lean-mtlx/mtlx-name.inc"
#undef MTLX_NAME
  for (const char *n : {"", "a", "_", "1a", "a1", "a:b", ":", "a-b", "a.b", "ns:a_1", "\xc3\xa9",
                        "A_Z", "z9_"})
    CompareName(n);
  // Floats.
  std::vector<float> fl = {0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 0.5f, 3.0f, 100.0f, 1e7f, 1e8f,
                           123456789.0f, 1e-5f, 1e-4f, 1.17549435e-38f, 1.4e-45f, 3.40282347e38f,
                           16777216.0f, 16777217.0f, 0.333333343f, 2.2f,
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN(),
                           -std::numeric_limits<float>::quiet_NaN()};
  std::uniform_int_distribution<uint32_t> bits;
  for (int i = 0; i < 200000; i++) {
    const uint32_t u = bits(rng);
    float f;
    std::memcpy(&f, &u, 4);
    fl.push_back(f);
  }
  for (int i = -2000; i <= 2000; i++) fl.push_back(float(i) * 0.25f);
  for (float f : fl) CompareFloatFormat(f);
}

// ---- synthesized XML cases --------------------------------------------------

std::vector<std::string> XmlCases() {
  std::vector<std::string> v = {
      "",
      "   ",
      "<",
      "<>",
      "<a",
      "<a>",
      "<a/>",
      "<a></a>",
      "<a></b>",
      "<a></a >",
      "<a></a x>",
      "<a/><b/>",
      "<a/>trailing",
      "<a/><!-- c -->",
      "<a/><!-- c",
      "<!-- c --><a/>",
      "<!-- unterminated",
      "<?xml version=\"1.0\"?><a/>",
      "<?xml version=\"1.0\"",
      "<!DOCTYPE x><a/>",
      "\xef\xbb\xbf<a/>",
      "\xef\xbb\xbf<materialx version=\"1.38\"></materialx>",
      "<a x=\"1\" y='2'/>",
      "<a x=1/>",
      "<a x/>",
      "<a x =  \"1\"  />",
      "<a x=\"1/>",
      "<a x='1\"/>",
      "<a =\"1\"/>",
      "<a x=\"&lt;&gt;&amp;&quot;&apos;&bogus;&amp\"/>",
      "<a x=\"&#65;\"/>",
      "<a><b><c/></b></a>",
      "<a><b></a></b>",
      "<a> text <b/> more </a>",
      "<a><![CDATA[ <x> ]]></a>",
      "<a><![CDATA[ unterminated</a>",
      "<a x=\"1\" x=\"2\"/>",
      "<materialx version=\"1.38\"/>",
      "<materialx version=\"1.38\"></materialx>",
      "<materialx></materialx>",
      "<materialx version=\"\"></materialx>",
      "<materialx version=\"1.23\"></materialx>",
      "<materialx version=\"1.38\" version=\"1.23\"></materialx>",
      "<materialx version=\"1.39\"><node/></materialx>",
      "<materialx version=\"1.39\"><a><b/></a><c x='1'/></materialx>",
      "<materialx version=\"1.39\"><a></a><c/></materialx>",
      "<materialx version=\"1.39\"><a>txt</a></materialx>",
      "<materialx version=\"1.39\"><a b='1'></b></materialx>",
      "<materialx version=\"1.39\"><a><!--c--></a></materialx>",
      "<materialx version=\"1.39\"><a><?pi x?></a></materialx>",
      "<materialx version=\"1.39\"><a/></materialx><extra/>",
      "<materialx version=\"1.39\"></materialx>trailing junk",
      "<materialx version=\"1.39\"><input name=\"x\" type=\"color3\" value=\"1,2,3\" name=\"y\"/></materialx>",
      "<materialx version=\"1.39\" colorspace=\"lin_rec709\" b='&amp;' a=\"z\"><n z='1' a='2' m='3'/></materialx>",
      "<foo version=\"1.39\"></foo>",
      "<?xml version=\"1.0\"?><!-- c --><materialx version=\"1.38\">\n  <nodegraph name=\"g\">\n  </nodegraph>\n</materialx>\n",
      "<materialx version=\"1.39\">\v</materialx>",
      "<materialx version=\"1.39\"><a\tx='1'\n/></materialx>",
      "<materialx version=\"1.39\"><a:b x.y-z='1'/></materialx>",
      "<materialx version=\"1.39\"><1a/></materialx>",
      "<materialx version=\"1.39\"></ materialx>",
      "<materialx version=\"1.39\"><a/></materialx  >",
      "<materialx version='1.38'><a><b><c/></b></a></materialx>",
  };
  // Long names (tokenizer MAX_NAME_LENGTH 256).
  for (size_t n : {255, 256, 257}) {
    const std::string nm(n, 'a');
    v.push_back("<materialx version=\"1.39\"><" + nm + "/></materialx>");
    v.push_back("<materialx version=\"1.39\" " + nm + "=\"1\"></materialx>");
  }
  v.push_back("<materialx version=\"1.39\" x=\"" + std::string(70000, 'q') + "\"></materialx>");
  // Deep nesting around both depth limits.
  for (size_t d : {998, 999, 1000, 1001, 1023, 1024, 1025, 1100}) {
    std::string s = "<materialx version=\"1.39\">";
    for (size_t i = 0; i < d; i++) s += "<n>";
    for (size_t i = 0; i < d; i++) s += "</n>";
    s += "</materialx>";
    v.push_back(s);
    std::string t = "<materialx version=\"1.39\">";
    for (size_t i = 0; i < d; i++) t += "<n x='1'/>";
    t += "</materialx>";
    v.push_back(t);
  }
  return v;
}

std::string ReadFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<std::string> CorpusPaths(const std::string &oracle) {
  std::vector<std::string> out;
  DIR *d = opendir(oracle.c_str());
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() < 4 || n.substr(n.size() - 4) != ".txt") continue;
    std::ifstream f(oracle + "/" + n);
    std::string line;
    std::getline(f, line);
    if (line.size() > 2 && line[0] == '#') out.push_back(line.substr(2));
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

void IncludeSuite() {
#define MTLX_INCLUDE(id, main, ha, xa, hb, xb, ok, listing) \
  {                                                         \
    Files f;                                                \
    if (*ha) f.mem[ha] = xa;                                \
    if (*hb) f.mem[hb] = xb;                                \
    CompareIncludes(main, f, "lean include " #id);          \
  }
#define MTLX_RESOLVE_LIB(...)
#define MTLX_RESOLVE(...)
#define MTLX_DUMP(...)
#define MTLX_SCOPE(...)
#include "tests/core/generated/lean-mtlx/mtlx-doc.inc"
#undef MTLX_INCLUDE
#undef MTLX_SCOPE
#undef MTLX_RESOLVE_LIB
#undef MTLX_RESOLVE
#undef MTLX_DUMP

  const std::string H = "<materialx version=\"1.39\">";
  const std::string E = "</materialx>";
  Files f;
  f.mem["cyc_a"] = H + "<xi:include href=\"cyc_b\"/>" + E;
  f.mem["cyc_b"] = H + "<xi:include href=\"cyc_a\"/>" + E;
  f.mem["self"] = H + "<xi:include href=\"self\"/>" + E;
  f.mem["bad"] = H + "<a></b>" + E;
  f.mem["empty"] = "";
  f.mem["lib"] = "<materialx version=\"1.39\" namespace=\"ns\" fileprefix=\"p/\" colorspace=\"srgb\">"
                 "<nodedef name=\"ND_x\" node=\"x\"/><nodedef name=\"ND_y\" namespace=\"\"/>"
                 "<image name=\"i\" fileprefix=\"own/\"/><a/><b/><xi:include href=\"leaf\"/></materialx>";
  f.mem["leaf"] = "<materialx version=\"1.39\" geomprefix=\"g/\"><c name=\"ND_x\"/><d name=\"q\" namespace=\"z\" name=\"r\"/></materialx>";
  f.mem["dup"] = H + "<c name=\"local\" v=\"inc\"/>" + E;
  std::string chain;
  for (int i = 0; i < 70; i++) {
    f.mem["d" + std::to_string(i)] = H + "<xi:include href=\"d" + std::to_string(i + 1) + "\"/><n name=\"n" + std::to_string(i) + "\"/>" + E;
  }
  f.mem["d70"] = H + E;
  const std::vector<std::string> mains = {
      H + "<xi:include href=\"cyc_a\"/>" + E,
      H + "<xi:include href=\"self\"/>" + E,
      H + "<xi:include href=\"bad\"/>" + E,
      H + "<xi:include href=\"empty\"/>" + E,
      H + "<xi:include href=\"missing\"/>" + E,
      H + "<xi:include/>" + E,
      H + "<c name=\"local\"/><xi:include href=\"dup\"/><c name=\"local\" v=\"own2\"/>" + E,
      H + "<xi:include href=\"lib\"/><xi:include href=\"lib\"/>" + E,
      H + "<xi:include href=\"lib\"/><x/><y/>" + E,
      H + "<xi:include href=\"d0\"/>" + E,
      H + "<xi:include href=\"d10\"/>" + E,
      H + "<g><xi:include href=\"lib\"/></g>" + E,
  };
  for (size_t i = 0; i < mains.size(); i++) CompareIncludes(mains[i], f, "synth include " + std::to_string(i));
  CompareIncludes(mains[7], f, "null loader", true);
}

}  // namespace

int main(int argc, char **argv) {
  const std::string oracle = argc > 1 ? argv[1] : "/mnt/nvme01/tmp/mtlx-c11/oracle";
  std::mt19937_64 rng(12345);

  // Corpus.
  const std::vector<std::string> paths = CorpusPaths(oracle);
  std::vector<std::string> docs;
  for (const std::string &p : paths) {
    const std::string xml = ReadFile(p);
    docs.push_back(xml);
    CompareParseXml(xml, p);
    CompareClassDirect(xml, p);
    CompareScopes(xml, p);
    Files f;
    f.base_dir = p.substr(0, p.rfind('/') + 1);
    CompareIncludes(xml, f, "corpus " + p);
  }
  std::fprintf(stderr, "corpus documents: %zu\n", paths.size());

  // Synthesized XML.
  const std::vector<std::string> cases = XmlCases();
  for (size_t i = 0; i < cases.size(); i++) {
    const std::string label = "case " + std::to_string(i) + " " + Short(cases[i]);
    if (getenv("CDIFF_TRACE")) std::fprintf(stderr, "%s\n", label.c_str());
    CompareParseXml(cases[i], label);
    if (getenv("CDIFF_TRACE")) std::fprintf(stderr, " parsed\n");
    CompareScopes(cases[i], label);
    if (getenv("CDIFF_TRACE")) std::fprintf(stderr, " scoped\n");
    CompareClassForked(cases[i], label);
  }
#define MTLX_SCOPE(id, xml, path, hf, rf, acs) CompareScopes(xml, "lean scope " #id);
#define MTLX_INCLUDE(...)
#define MTLX_RESOLVE_LIB(...)
#define MTLX_RESOLVE(...)
#define MTLX_DUMP(...)
#include "tests/core/generated/lean-mtlx/mtlx-doc.inc"
#undef MTLX_SCOPE
#undef MTLX_INCLUDE
#undef MTLX_RESOLVE_LIB
#undef MTLX_RESOLVE
#undef MTLX_DUMP


  // Mutation fuzz of corpus documents.
  if (!docs.empty()) {
    const char bytes[] = "<>/=\"'&;!?- \nab:";
    std::uniform_int_distribution<size_t> pick(0, docs.size() - 1);
    std::uniform_int_distribution<int> op(0, 2), bi(0, int(sizeof(bytes)) - 2);
    const int nfuzz = 800;
    for (int i = 0; i < nfuzz; i++) {
      std::string s = docs[pick(rng)];
      if (s.size() > 20000) s.resize(20000);
      const int nmut = 1 + int(rng() % 4);
      for (int k = 0; k < nmut && !s.empty(); k++) {
        const size_t at = rng() % s.size();
        switch (op(rng)) {
          case 0: s[at] = bytes[bi(rng)]; break;
          case 1: s.erase(at, 1 + rng() % 8); break;
          default: s.insert(at, 1, bytes[bi(rng)]); break;
        }
      }
      const std::string label = "fuzz " + std::to_string(i);
      CompareParseXml(s, label);
      CompareClassForked(s, label);
    }
  }

  IncludeSuite();
  ValueSuite(rng);

  std::printf("mtlx_cdiff_parse: %zu checks, %zu failures (%zu inputs hang the C++ class parser; C reports an error)\n",
              g_checks, g_fail, g_hangs);
  return g_fail ? 1 : 0;
}
