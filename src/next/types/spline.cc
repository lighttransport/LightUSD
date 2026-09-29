// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - typed AOUSD spline support. See spline.hh.

#include "spline.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "../../spline-eval.hh"  // shared header-only evaluator (std-only)
#include "../crate/crate-format.hh"  // FloatToHalf/HalfToFloat (round-to-even)
#include "../writer/dtoa.hh"

namespace lightusd {
namespace next {

namespace {

// Use the canonical half converters so spline-encoded halves are byte-identical
// to pxr (round-to-nearest-even) and to next's own half evaluation cast; a
// truncating local copy silently diverged from both.
using lightusd::next::FloatToHalf;
using lightusd::next::HalfToFloat;

template <typename T>
void Wr(std::vector<uint8_t>* buf, const T& v) {
  const size_t o = buf->size();
  buf->resize(o + sizeof(T));
  std::memcpy(buf->data() + o, &v, sizeof(T));
}

template <typename T>
bool Rd(const uint8_t** p, size_t* remain, T* out) {
  if (*remain < sizeof(T)) return false;
  std::memcpy(out, *p, sizeof(T));
  *p += sizeof(T);
  *remain -= sizeof(T);
  return true;
}

// Write a scalar of the descriptor's type from a double.
void WrTyped(std::vector<uint8_t>* buf, int desc, double v) {
  if (desc == 2) {
    Wr<float>(buf, static_cast<float>(v));
  } else if (desc == 3) {
    Wr<uint16_t>(buf, FloatToHalf(static_cast<float>(v)));
  } else {
    Wr<double>(buf, v);  // 0 (unspecified) and 1 (double)
  }
}

// Read a scalar of the descriptor's type as a double.
bool RdTyped(const uint8_t** p, size_t* remain, int desc, double* out) {
  if (desc == 2) {
    float f;
    if (!Rd<float>(p, remain, &f)) return false;
    *out = double(f);
  } else if (desc == 3) {
    uint16_t h;
    if (!Rd<uint16_t>(p, remain, &h)) return false;
    *out = double(HalfToFloat(h));
  } else {
    double d;
    if (!Rd<double>(p, remain, &d)) return false;
    *out = d;
  }
  return true;
}

// --- Minimal char scanner over the captured spline text -------------------

struct Scanner {
  const char* p;
  const char* end;

  void skip_ws() {
    while (p < end) {
      if (*p == '#') {  // comment to end of line
        while (p < end && *p != '\n') p++;
      } else if (*p == '/' && p + 1 < end && p[1] == '*') {
        // Block comments count as whitespace (AOUSD splines.usda).
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
        p = (p + 1 < end) ? p + 2 : end;
      } else if (*p == '/' && p + 1 < end && p[1] == '/') {
        while (p < end && *p != '\n') p++;
      } else if (std::isspace(static_cast<unsigned char>(*p))) {
        p++;
      } else {
        break;
      }
    }
  }
  bool eof() { return p >= end; }
  char peek() { return p < end ? *p : '\0'; }
  bool expect(char c) {
    skip_ws();
    if (p < end && *p == c) {
      p++;
      return true;
    }
    return false;
  }
  bool ident(std::string* out) {
    skip_ws();
    const char* s = p;
    while (p < end && (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_'))
      p++;
    if (p == s) return false;
    out->assign(s, static_cast<size_t>(p - s));
    return true;
  }
  bool number(double* out) {
    skip_ws();
    char* np = nullptr;
    // The scanner runs over std::string storage, which is NUL-terminated, so
    // strtod cannot run past `end` into unowned memory.
    double v = std::strtod(p, &np);
    if (np == p || np > end) return false;
    *out = v;
    p = np;
    return true;
  }
};

// none|held|linear|sloped(x)|loop [repeat|reset|oscillate]
bool ParseExtrap(Scanner& sc, int* mode, double* slope, std::string* err) {
  std::string kw;
  if (!sc.ident(&kw)) {
    *err = "Expected extrapolation keyword";
    return false;
  }
  *slope = 0.0;
  if (kw == "none") {
    *mode = 0;
  } else if (kw == "held") {
    *mode = 1;
  } else if (kw == "linear") {
    *mode = 2;
  } else if (kw == "sloped") {
    *mode = 3;
    if (!sc.expect('(') || !sc.number(slope) || !sc.expect(')')) {
      *err = "Malformed sloped(<slope>) extrapolation";
      return false;
    }
  } else if (kw == "loop") {
    *mode = 4;  // repeat
    sc.skip_ws();
    if (std::isalpha(static_cast<unsigned char>(sc.peek()))) {
      std::string sub;
      if (!sc.ident(&sub)) return false;
      if (sub == "repeat") *mode = 4;
      else if (sub == "reset") *mode = 5;
      else if (sub == "oscillate") *mode = 6;
      else {
        *err = "Unknown loop extrapolation mode: " + sub;
        return false;
      }
    }
  } else {
    *err = "Unknown spline extrapolation mode: " + kw;
    return false;
  }
  return true;
}

bool ParseTangentAlgo(Scanner& sc, int* algo, std::string* err) {
  std::string kw;
  if (!sc.ident(&kw)) {
    *err = "Expected tangent algorithm keyword";
    return false;
  }
  if (kw == "none") *algo = 0;
  else if (kw == "custom") *algo = 1;
  else if (kw == "autoEase") *algo = 2;
  else {
    *err = "Unknown spline tangent algorithm: " + kw;
    return false;
  }
  return true;
}

// `( <width>, <slope> [, <algo>] )` (bezier) or `( <slope> [, <algo>] )`
// (hermite, width implied).
bool ParseTangent(Scanner& sc, double* width, double* slope, int* algo,
                  std::string* err) {
  *algo = 0;
  if (!sc.expect('(')) {
    *err = "Expected `(` for spline tangent";
    return false;
  }
  double a;
  if (!sc.number(&a)) {
    *err = "Expected number in spline tangent";
    return false;
  }
  sc.skip_ws();
  if (sc.peek() == ',') {
    sc.p++;
    sc.skip_ws();
    if (std::isalpha(static_cast<unsigned char>(sc.peek())) || sc.peek() == '_') {
      // hermite with algorithm: `( <slope>, <algo> )`
      *width = 1.0;
      *slope = a;
      if (!ParseTangentAlgo(sc, algo, err)) return false;
    } else {
      double b;
      if (!sc.number(&b)) {
        *err = "Expected slope in spline tangent";
        return false;
      }
      *width = a;
      *slope = b;
      sc.skip_ws();
      if (sc.peek() == ',') {
        sc.p++;
        if (!ParseTangentAlgo(sc, algo, err)) return false;
      }
    }
  } else {
    *width = 1.0;  // hermite: width implied
    *slope = a;
  }
  if (!sc.expect(')')) {
    *err = "Expected `)` for spline tangent";
    return false;
  }
  return true;
}

}  // namespace

bool ParseSplineText(const std::string& text, SplineData* out,
                     std::string* err) {
  if (!out) return false;
  *out = SplineData();
  std::string local_err;
  std::string* e = err ? err : &local_err;

  Scanner sc{text.data(), text.data() + text.size()};
  if (!sc.expect('{')) {
    *e = "Expected `{` for spline value";
    return false;
  }

  while (true) {
    sc.skip_ws();
    if (sc.eof()) {
      *e = "Unexpected end of spline value";
      return false;
    }
    char c = sc.peek();
    if (c == '}') {
      sc.p++;
      break;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::string kw;
      if (!sc.ident(&kw)) return false;
      if (kw == "bezier") {
        out->curve_type = 0;
      } else if (kw == "hermite") {
        out->curve_type = 1;
      } else if (kw == "pre" || kw == "post") {
        if (!sc.expect(':')) {
          *e = "Expected `:` after spline `" + kw + "`";
          return false;
        }
        int mode = 1;
        double slope = 0.0;
        if (!ParseExtrap(sc, &mode, &slope, e)) return false;
        if (kw == "pre") {
          out->pre_extrap = mode;
          out->pre_slope = slope;
        } else {
          out->post_extrap = mode;
          out->post_slope = slope;
        }
      } else if (kw == "loop") {
        // loop: (protoStart, protoEnd, numPreLoops, numPostLoops, valueOffset)
        if (!sc.expect(':') || !sc.expect('(')) {
          *e = "Malformed spline loop params";
          return false;
        }
        double vals[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < 5; i++) {
          if (!sc.number(&vals[i])) {
            *e = "Expected number in spline loop params";
            return false;
          }
          if (i < 4 && !sc.expect(',')) {
            *e = "Expected `,` in spline loop params";
            return false;
          }
        }
        if (!sc.expect(')')) {
          *e = "Expected `)` for spline loop params";
          return false;
        }
        out->has_loop = true;
        out->loop_start = vals[0];
        out->loop_end = vals[1];
        out->loop_pre = static_cast<int>(vals[2]);
        out->loop_post = static_cast<int>(vals[3]);
        out->loop_offset = vals[4];
      } else {
        *e = "Unknown spline keyword: " + kw;
        return false;
      }
    } else {
      // Knot: <time> : <value> [& <postValue>] [; <specs>] [{ customData }]
      SplineKnot k;
      if (!sc.number(&k.time)) {
        *e = "Expected knot time";
        return false;
      }
      if (!sc.expect(':')) {
        *e = "Expected `:` in spline knot";
        return false;
      }
      double v0;
      if (!sc.number(&v0)) {
        *e = "Expected knot value";
        return false;
      }
      sc.skip_ws();
      if (sc.peek() == '&') {
        // Dual-valued knot: `<preValue> & <value>`.
        sc.p++;
        double v1;
        if (!sc.number(&v1)) {
          *e = "Expected post value in dual-valued knot";
          return false;
        }
        k.pre_value = v0;
        k.value = v1;
        k.dual = true;
      } else {
        k.value = v0;
        k.pre_value = v0;
      }

      // Optional `;`-separated tangent / interpolation specs.
      while (true) {
        sc.skip_ws();
        if (sc.peek() != ';') break;
        sc.p++;
        sc.skip_ws();
        if (sc.peek() == '{') {
          const char* custom_begin = sc.p;
          // Per-knot customData dictionary. Brace
          // counting must ignore braces inside string literals and comments,
          // else a value like `"}"` desyncs the depth and truncates early.
          int depth = 0;
          do {
            if (sc.eof()) {
              *e = "Unterminated spline knot customData";
              return false;
            }
            char cc = *sc.p++;
            if (cc == '#') {  // comment to end of line
              while (!sc.eof() && *sc.p != '\n') sc.p++;
            } else if (cc == '"' || cc == '\'') {
              const char quote = cc;
              // Triple-quoted string (`"""..."""` / `'''...'''`): only a run of
              // three quotes closes it, so a lone quote inside (and any braces
              // around it) must not end the scan.
              const bool triple = (sc.p + 1) < sc.end && sc.p[0] == quote &&
                                  sc.p[1] == quote;
              if (triple) {
                sc.p += 2;  // consume the opening triple's other two quotes
                while (sc.p + 2 < sc.end &&
                       !(sc.p[0] == quote && sc.p[1] == quote &&
                         sc.p[2] == quote)) {
                  sc.p++;
                }
                sc.p = (sc.p + 3 <= sc.end) ? sc.p + 3 : sc.end;  // closing """
              } else {
                while (!sc.eof() && *sc.p != quote) {
                  if (*sc.p == '\\' && (sc.p + 1) < sc.end) sc.p++;  // skip escape
                  sc.p++;
                }
                if (!sc.eof()) sc.p++;  // closing quote
              }
            } else if (cc == '{') {
              depth++;
            } else if (cc == '}') {
              depth--;
            }
          } while (depth > 0);
          k.custom_data_source.assign(custom_begin,
                                      static_cast<size_t>(sc.p - custom_begin));
          continue;
        }
        std::string spec;
        if (!sc.ident(&spec)) {
          *e = "Expected spline knot spec after `;`";
          return false;
        }
        if (spec == "pre") {
          if (!ParseTangent(sc, &k.pre_tan_width, &k.pre_tan_slope,
                            &k.pre_algo, e)) {
            return false;
          }
        } else if (spec == "post") {
          std::string interp;
          if (!sc.ident(&interp)) {
            *e = "Expected interpolation after `post`";
            return false;
          }
          if (interp == "none") k.interp = 0;
          else if (interp == "held") k.interp = 1;
          else if (interp == "linear") k.interp = 2;
          else if (interp == "curve") {
            k.interp = 3;
            if (!ParseTangent(sc, &k.post_tan_width, &k.post_tan_slope,
                              &k.post_algo, e)) {
              return false;
            }
          } else {
            *e = "Unknown spline interpolation: " + interp;
            return false;
          }
        } else {
          *e = "Unknown spline knot spec: " + spec;
          return false;
        }
      }
      out->knots.push_back(k);
    }

    sc.skip_ws();
    if (sc.peek() == ',') sc.p++;
  }

  return true;
}

std::string FormatSplineText(const SplineData& sd, const std::string& indent) {
  const std::string ki = indent + "    ";
  const bool hermite = (sd.curve_type == 1);

  auto extrap_str = [](int mode, double slope) -> std::string {
    switch (mode) {
      case 0: return "none";
      case 1: return "held";
      case 2: return "linear";
      case 3: return "sloped(" + dtos(slope) + ")";
      case 4: return "loop repeat";
      case 5: return "loop reset";
      case 6: return "loop oscillate";
      default: return "held";
    }
  };
  auto algo_suffix = [](int algo) -> std::string {
    switch (algo) {
      case 1: return ", custom";
      case 2: return ", autoEase";
      default: return "";
    }
  };
  auto tangent = [&](double width, double slope, int algo) -> std::string {
    if (hermite) return "(" + dtos(slope) + algo_suffix(algo) + ")";
    return "(" + dtos(width) + ", " + dtos(slope) + algo_suffix(algo) + ")";
  };

  std::string s = "{\n";
  s += ki + (hermite ? "hermite" : "bezier") + ",\n";
  // Only emit extrapolation when it differs from the default (held).
  if (sd.pre_extrap != 1) {
    s += ki + "pre: " + extrap_str(sd.pre_extrap, sd.pre_slope) + ",\n";
  }
  if (sd.post_extrap != 1) {
    s += ki + "post: " + extrap_str(sd.post_extrap, sd.post_slope) + ",\n";
  }
  if (sd.has_loop) {
    s += ki + "loop: (" + dtos(sd.loop_start) + ", " + dtos(sd.loop_end) +
         ", " + std::to_string(sd.loop_pre) + ", " +
         std::to_string(sd.loop_post) + ", " + dtos(sd.loop_offset) + "),\n";
  }
  for (const SplineKnot& k : sd.knots) {
    s += ki + dtos(k.time) + ": ";
    if (k.dual) {
      s += dtos(k.pre_value) + " & " + dtos(k.value);
    } else {
      s += dtos(k.value);
    }
    if (k.pre_tan_width != 0.0 || k.pre_tan_slope != 0.0 || k.pre_algo != 0) {
      s += "; pre " + tangent(k.pre_tan_width, k.pre_tan_slope, k.pre_algo);
    }
    switch (k.interp) {
      case 0: s += "; post none"; break;
      case 1: s += "; post held"; break;
      case 2: s += "; post linear"; break;
      case 3:
        s += "; post curve " +
             tangent(k.post_tan_width, k.post_tan_slope, k.post_algo);
        break;
      default: break;
    }
    if (!k.custom_data_source.empty()) {
      s += "; " + k.custom_data_source;
    }
    s += ",\n";
  }
  s += indent + "}";
  return s;
}

uint8_t SplineBinaryVersion(const SplineData& sd) {
  for (const SplineKnot& k : sd.knots) {
    if (k.pre_algo != 0 || k.post_algo != 0) return 2;
  }
  return 1;
}

bool EncodeSplineBinary(const SplineData& sd, std::vector<uint8_t>* out,
                        std::string* err) {
  if (!out) return false;
  out->clear();

  const int desc = (sd.value_desc >= 1 && sd.value_desc <= 3) ? sd.value_desc : 1;
  const bool hermite = (sd.curve_type == 1);
  const uint8_t version = SplineBinaryVersion(sd);

  // headerByte1: [0-3]=version [4-5]=typeDescriptor [6]=timeValued [7]=curveType
  uint8_t h1 = static_cast<uint8_t>((version & 0x0f) | ((desc & 0x03) << 4) |
                                    (0 << 6) | ((sd.curve_type & 0x01) << 7));
  Wr<uint8_t>(out, h1);

  // headerByte2: [0-2]=preExtrap [3-5]=postExtrap [6]=hasLoops
  uint8_t h2 = static_cast<uint8_t>((sd.pre_extrap & 0x07) |
                                    ((sd.post_extrap & 0x07) << 3) |
                                    ((sd.has_loop ? 1 : 0) << 6));
  Wr<uint8_t>(out, h2);

  if (sd.pre_extrap == 3) Wr<double>(out, sd.pre_slope);
  if (sd.post_extrap == 3) Wr<double>(out, sd.post_slope);

  if (sd.has_loop) {
    Wr<double>(out, sd.loop_start);
    Wr<double>(out, sd.loop_end);
    Wr<int32_t>(out, static_cast<int32_t>(sd.loop_pre));
    Wr<int32_t>(out, static_cast<int32_t>(sd.loop_post));
    Wr<double>(out, sd.loop_offset);
  }

  if (sd.knots.size() > size_t((std::numeric_limits<uint32_t>::max)())) {
    if (err) *err = "Too many spline knots to encode";
    return false;
  }
  Wr<uint32_t>(out, static_cast<uint32_t>(sd.knots.size()));

  for (const SplineKnot& k : sd.knots) {
    uint8_t flag = static_cast<uint8_t>((k.dual ? 1 : 0) |
                                        ((k.interp & 0x03) << 1) |
                                        ((sd.curve_type & 0x01) << 3));
    Wr<uint8_t>(out, flag);
    Wr<double>(out, k.time);
    WrTyped(out, desc, k.value);
    if (k.dual) WrTyped(out, desc, k.pre_value);
    if (!hermite) {
      Wr<double>(out, k.pre_tan_width);
      Wr<double>(out, k.post_tan_width);
    }
    WrTyped(out, desc, k.pre_tan_slope);
    WrTyped(out, desc, k.post_tan_slope);
    if (version > 1) {
      // algorithmByte: low nibble = pre, high nibble = post.
      uint8_t algo = static_cast<uint8_t>((k.pre_algo & 0x0f) |
                                          ((k.post_algo & 0x0f) << 4));
      Wr<uint8_t>(out, algo);
    }
  }
  return true;
}

bool DecodeSplineBinary(const uint8_t* data, size_t size, SplineData* out,
                        std::string* err) {
  if (!out) return false;
  *out = SplineData();

  const uint8_t* p = data;
  size_t remain = size;

  uint8_t h1 = 0;
  if (!Rd<uint8_t>(&p, &remain, &h1)) {
    if (err) *err = "Unexpected end of spline data (header byte 1)";
    return false;
  }
  const uint8_t version = h1 & 0x0f;
  const int desc = (h1 & 0x30) >> 4;
  const bool hermite = ((h1 & 0x80) >> 7) != 0;
  out->curve_type = hermite ? 1 : 0;
  out->value_desc = (desc == 0) ? 1 : desc;

  uint8_t h2 = 0;
  if (!Rd<uint8_t>(&p, &remain, &h2)) {
    if (err) *err = "Unexpected end of spline data (header byte 2)";
    return false;
  }
  out->pre_extrap = h2 & 0x07;
  out->post_extrap = (h2 & 0x38) >> 3;
  const bool has_loops = (h2 & 0x40) != 0;

  if (out->pre_extrap == 3 && !Rd<double>(&p, &remain, &out->pre_slope))
    return false;
  if (out->post_extrap == 3 && !Rd<double>(&p, &remain, &out->post_slope))
    return false;

  if (has_loops) {
    int32_t npre = 0, npost = 0;
    if (!Rd<double>(&p, &remain, &out->loop_start)) return false;
    if (!Rd<double>(&p, &remain, &out->loop_end)) return false;
    if (!Rd<int32_t>(&p, &remain, &npre)) return false;
    if (!Rd<int32_t>(&p, &remain, &npost)) return false;
    if (!Rd<double>(&p, &remain, &out->loop_offset)) return false;
    out->has_loop = true;
    out->loop_pre = npre;
    out->loop_post = npost;
  }

  uint32_t knot_count = 0;
  if (!Rd<uint32_t>(&p, &remain, &knot_count)) {
    if (err) *err = "Unexpected end of spline data (knot count)";
    return false;
  }

  const size_t value_size = (desc == 2) ? 4u : (desc == 3) ? 2u : 8u;
  size_t min_knot = 1 + 8 + value_size * 3;  // flag+time+value+2 slopes
  if (!hermite) min_knot += 16;              // 2 tangent widths
  if (version > 1) min_knot += 1;            // algorithm byte
  if (static_cast<uint64_t>(knot_count) >
      static_cast<uint64_t>(remain / min_knot)) {
    if (err) *err = "Spline knot count exceeds remaining data";
    return false;
  }

  out->knots.reserve(knot_count);
  for (uint32_t i = 0; i < knot_count; i++) {
    SplineKnot k;
    uint8_t flag = 0;
    if (!Rd<uint8_t>(&p, &remain, &flag)) return false;
    k.dual = (flag & 0x01) != 0;
    k.interp = (flag & 0x06) >> 1;

    if (!Rd<double>(&p, &remain, &k.time)) return false;
    if (!RdTyped(&p, &remain, desc, &k.value)) return false;
    if (k.dual) {
      if (!RdTyped(&p, &remain, desc, &k.pre_value)) return false;
    } else {
      k.pre_value = k.value;
    }
    if (!hermite) {
      if (!Rd<double>(&p, &remain, &k.pre_tan_width)) return false;
      if (!Rd<double>(&p, &remain, &k.post_tan_width)) return false;
    }
    if (!RdTyped(&p, &remain, desc, &k.pre_tan_slope)) return false;
    if (!RdTyped(&p, &remain, desc, &k.post_tan_slope)) return false;
    if (version > 1) {
      uint8_t algo = 0;
      if (!Rd<uint8_t>(&p, &remain, &algo)) return false;
      k.pre_algo = algo & 0x0f;
      k.post_algo = (algo >> 4) & 0x0f;
    }
    out->knots.push_back(k);
  }
  return true;
}

namespace {

// Recompute tangents for knots whose tangent algorithm is AutoEase (2). pxr
// treats the authored slope/width on such knots as placeholders and derives
// the real tangents from the neighbors (ts/knotData.cpp _UpdateTangentAutoEase):
// slope 0 at any discontinuity/extremum, otherwise an ease-blend of the
// neighbor slopes; width = 1/3 the distance to the adjacent knot. Done only for
// evaluation — the crate blob keeps the authored placeholders + algorithm byte,
// exactly as pxr stores them, so pxr recomputes on read too.
void ComputeAutoEaseTangents(SplineData& sd) {
  const std::vector<SplineKnot> in = sd.knots;  // recompute from authored values
  const size_t n = in.size();
  auto blend_slope = [&](size_t i) -> double {
    const double prev_t = in[i - 1].time, next_t = in[i + 1].time, t = in[i].time;
    const double prev_v = in[i - 1].value;
    const double knot_v = in[i].value;
    const double next_v = in[i + 1].dual ? in[i + 1].pre_value : in[i + 1].value;
    const double prev_slope = (knot_v - prev_v) / (t - prev_t);
    const double next_slope = (next_v - knot_v) / (next_t - t);
    if (prev_slope * next_slope <= 0.0) return 0.0;  // extremum -> flat
    const double f = (t - prev_t) / (next_t - prev_t);
    const double u = f - 0.5;
    const double g = 0.5 + u * (0.5 + 2.0 * u * u);
    double slope = prev_slope + g * (next_slope - prev_slope);
    if (next_slope > 0.0)
      slope = std::min({slope, 3.0 * next_slope, 3.0 * prev_slope});
    else
      slope = std::max({slope, 3.0 * next_slope, 3.0 * prev_slope});
    return slope;
  };
  for (size_t i = 0; i < n; ++i) {
    const bool want_pre = (in[i].pre_algo == 2);
    const bool want_post = (in[i].post_algo == 2);
    if (!want_pre && !want_post) continue;
    const bool has_prev = (i > 0);
    const bool has_next = (i + 1 < n);
    // Discontinuity: an endpoint, a dual-valued knot, or an adjacent value
    // block (interp 0 == none) forces a flat tangent.
    const bool disc = !has_prev || !has_next || in[i].dual ||
                      in[i].interp == 0 || (has_prev && in[i - 1].interp == 0);
    const double slope = disc ? 0.0 : blend_slope(i);
    if (want_pre && has_prev) {
      sd.knots[i].pre_tan_slope = slope;
      sd.knots[i].pre_tan_width = (in[i].time - in[i - 1].time) / 3.0;
    }
    if (want_post && has_next) {
      sd.knots[i].post_tan_slope = slope;
      sd.knots[i].post_tan_width = (in[i + 1].time - in[i].time) / 3.0;
    }
  }
}

}  // namespace

bool EvaluateSplineData(const SplineData& sd, double time, double* out) {
  if (!out) return false;

  // AutoEase knots carry placeholder tangents; derive the real ones first.
  bool has_auto = false;
  for (const SplineKnot& k : sd.knots) {
    if (k.pre_algo == 2 || k.post_algo == 2) {
      has_auto = true;
      break;
    }
  }
  SplineData work;
  const SplineData* src = &sd;
  if (has_auto) {
    work = sd;
    ComputeAutoEaseTangents(work);
    src = &work;
  }

  // Bridge to the shared evaluator.
  lightusd::Spline<double> sp;
  sp.curveType = (src->curve_type == 1) ? lightusd::SplineCurveType::Hermite
                                      : lightusd::SplineCurveType::Bezier;
  auto extrap = [](int m) {
    switch (m) {
      case 0: return lightusd::SplineExtrapolationMode::None;
      case 2: return lightusd::SplineExtrapolationMode::Linear;
      case 3: return lightusd::SplineExtrapolationMode::Sloped;
      case 4: return lightusd::SplineExtrapolationMode::LoopRepeat;
      case 5: return lightusd::SplineExtrapolationMode::LoopReset;
      case 6: return lightusd::SplineExtrapolationMode::LoopOscillate;
      default: return lightusd::SplineExtrapolationMode::Held;
    }
  };
  sp.preExtrapolation = extrap(src->pre_extrap);
  sp.postExtrapolation = extrap(src->post_extrap);
  sp.preExtrapolationSlope = src->pre_slope;
  sp.postExtrapolationSlope = src->post_slope;
  sp.loopParams.protoStart = src->loop_start;
  sp.loopParams.protoEnd = src->loop_end;
  sp.loopParams.numPreLoops = src->loop_pre;
  sp.loopParams.numPostLoops = src->loop_post;
  sp.loopParams.valueOffset = src->loop_offset;

  sp.knots.reserve(src->knots.size());
  for (const SplineKnot& k : src->knots) {
    lightusd::SplineKnot<double> sk;
    sk.time = k.time;
    sk.value = k.value;
    sk.preValue = k.pre_value;
    sk.hasDualValue = k.dual;
    sk.preTangentSlope = k.pre_tan_slope;
    sk.preTangentWidth = k.pre_tan_width;
    sk.postTangentSlope = k.post_tan_slope;
    sk.postTangentWidth = k.post_tan_width;
    switch (k.interp) {
      case 0: sk.nextInterpolationMode = lightusd::SplineInterpolationMode::None; break;
      case 1: sk.nextInterpolationMode = lightusd::SplineInterpolationMode::Held; break;
      case 2: sk.nextInterpolationMode = lightusd::SplineInterpolationMode::Linear; break;
      default: sk.nextInterpolationMode = lightusd::SplineInterpolationMode::Curve; break;
    }
    sp.knots.push_back(sk);
  }

  return lightusd::EvaluateSpline(sp, time, out);
}

}  // namespace next
}  // namespace lightusd
