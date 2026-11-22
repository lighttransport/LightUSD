// SPDX-License-Identifier: Apache-2.0
#include "ply-reader.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>


namespace lightusd {
namespace {

enum class Fmt { Ascii, BinLE, BinBE };

struct Prop {
  std::string name;
  int size = 0;  // bytes (binary)
  int kind = 0;  // 0 int signed, 1 unsigned, 2 float
  bool list = false;
};

struct Elem {
  std::string name;
  size_t count = 0;
  std::vector<Prop> props;
};

bool PropType(const std::string& t, Prop* p) {
  struct T { const char* a; const char* b; int size, kind; };
  static const T types[] = {
      {"char", "int8", 1, 0},     {"uchar", "uint8", 1, 1},
      {"short", "int16", 2, 0},   {"ushort", "uint16", 2, 1},
      {"int", "int32", 4, 0},     {"uint", "uint32", 4, 1},
      {"float", "float32", 4, 2}, {"double", "float64", 8, 2}};
  for (const auto& e : types)
    if (t == e.a || t == e.b) {
      p->size = e.size;
      p->kind = e.kind;
      return true;
    }
  return false;
}

double ReadBin(const uint8_t* q, const Prop& p, bool swap) {
  uint8_t b[8];
  std::memcpy(b, q, size_t(p.size));
  if (swap) std::reverse(b, b + p.size);
  switch (p.kind * 16 + p.size) {
    case 0 * 16 + 1: { int8_t v; std::memcpy(&v, b, 1); return v; }
    case 1 * 16 + 1: return b[0];
    case 0 * 16 + 2: { int16_t v; std::memcpy(&v, b, 2); return v; }
    case 1 * 16 + 2: { uint16_t v; std::memcpy(&v, b, 2); return v; }
    case 0 * 16 + 4: { int32_t v; std::memcpy(&v, b, 4); return v; }
    case 1 * 16 + 4: { uint32_t v; std::memcpy(&v, b, 4); return v; }
    case 2 * 16 + 4: { float v; std::memcpy(&v, b, 4); return v; }
    case 2 * 16 + 8: { double v; std::memcpy(&v, b, 8); return v; }
  }
  return 0;
}

void SetAttr(PrimSpec* prim, const std::string& name, Value value,
             const std::string& type, bool uniform = false) {
  prim->upsert_property(name, std::move(value),
                        uniform ? PropSlot::kFlagUniform : 0);
  prim->set_property_type_name(name, type);
}

}  // namespace

std::shared_ptr<Layer> LoadPLYLayerFromMemory(const std::string& source,
                                              const uint8_t* data, size_t size,
                                              std::string* warn,
                                              std::string* err) {
  auto fail = [&](const std::string& m) -> std::shared_ptr<Layer> {
    if (err) *err = "PLY '" + source + "': " + m;
    return nullptr;
  };
  if (!data || size < 10 || std::memcmp(data, "ply", 3) != 0)
    return fail("not a PLY file");
  const char* text = reinterpret_cast<const char*>(data);
  const char* end_hdr = nullptr;
  for (size_t i = 0; i + 10 <= size && i < (size_t(1) << 20); ++i)
    if (std::memcmp(text + i, "end_header", 10) == 0) {
      size_t j = i + 10;
      if (j < size && data[j] == '\r') ++j;
      if (j < size && data[j] == '\n') ++j;
      end_hdr = text + j;
      break;
    }
  if (!end_hdr) return fail("missing end_header");

  // ---- header ----
  Fmt fmt = Fmt::Ascii;
  std::vector<Elem> elems;
  std::istringstream hs(std::string(text, size_t(end_hdr - text)));
  std::string line;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string kw;
    ls >> kw;
    if (kw == "format") {
      std::string f;
      ls >> f;
      if (f == "ascii") fmt = Fmt::Ascii;
      else if (f == "binary_little_endian") fmt = Fmt::BinLE;
      else if (f == "binary_big_endian") fmt = Fmt::BinBE;
      else return fail("unsupported format " + f);
    } else if (kw == "element") {
      Elem e;
      ls >> e.name >> e.count;
      elems.push_back(e);
    } else if (kw == "property") {
      if (elems.empty()) return fail("property before element");
      Prop p;
      std::string t;
      ls >> t;
      if (t == "list") {
        std::string ct, it;
        ls >> ct >> it >> p.name;
        p.list = true;
        Prop c;
        if (!PropType(ct, &c) || !PropType(it, &p)) return fail("bad list type");
        p.size = p.size * 256 + c.size;  // packed: item size, count size
      } else {
        ls >> p.name;
        if (!PropType(t, &p)) return fail("unknown property type " + t);
      }
      elems.back().props.push_back(p);
    }
  }
  const bool swap = fmt == Fmt::BinBE;  // hosts are little-endian

  // ---- locate the vertex element ----
  size_t vi = elems.size();
  for (size_t i = 0; i < elems.size(); ++i)
    if (elems[i].name == "vertex") vi = i;
  if (vi == elems.size()) return fail("no vertex element");
  for (size_t i = 0; i < vi; ++i)
    if (elems[i].count && (fmt != Fmt::Ascii)) {
      // Elements before "vertex" would need skipping; 3DGS files never have
      // them, so keep the reader simple and explicit.
      return fail("elements before 'vertex' are not supported");
    }
  const Elem& v = elems[vi];
  for (const auto& p : v.props)
    if (p.list) return fail("list properties in 'vertex' are not supported");
  auto index_of = [&](const std::string& n) -> int {
    for (size_t i = 0; i < v.props.size(); ++i)
      if (v.props[i].name == n) return int(i);
    return -1;
  };
  const int ix = index_of("x"), iy = index_of("y"), iz = index_of("z");
  int is[3], ir[4], idc[3];
  for (int k = 0; k < 3; ++k) is[k] = index_of("scale_" + std::to_string(k));
  for (int k = 0; k < 4; ++k) ir[k] = index_of("rot_" + std::to_string(k));
  for (int k = 0; k < 3; ++k) idc[k] = index_of("f_dc_" + std::to_string(k));
  const int iop = index_of("opacity");
  if (ix < 0 || iy < 0 || iz < 0) return fail("missing x/y/z");
  if (is[0] < 0 || is[1] < 0 || is[2] < 0 || ir[0] < 0 || ir[1] < 0 ||
      ir[2] < 0 || ir[3] < 0 || iop < 0)
    return fail("not a Gaussian splat PLY (needs scale_0..2, rot_0..3, "
                "opacity)");
  std::vector<int> irest;
  for (int k = 0;; ++k) {
    const int i = index_of("f_rest_" + std::to_string(k));
    if (i < 0) break;
    irest.push_back(i);
  }
  const size_t rest_per_channel = irest.size() / 3;
  int degree = 0;
  while (degree < 3 && size_t((degree + 2) * (degree + 2) - 1) <= rest_per_channel)
    ++degree;
  if (size_t((degree + 1) * (degree + 1) - 1) != rest_per_channel ||
      irest.size() % 3) {
    if (warn)
      *warn += "PLY '" + source + "': " + std::to_string(irest.size()) +
               " f_rest values are not a full SH band set; using degree " +
               std::to_string(degree) + "\n";
  }
  const bool have_dc = idc[0] >= 0 && idc[1] >= 0 && idc[2] >= 0;
  if (!have_dc) degree = 0;
  const size_t ncoef = size_t((degree + 1) * (degree + 1));

  // ---- read records ----
  const size_t n = v.count;
  size_t stride = 0;
  std::vector<size_t> offset(v.props.size());
  for (size_t i = 0; i < v.props.size(); ++i) {
    offset[i] = stride;
    stride += size_t(v.props[i].size);
  }
  const uint8_t* body = reinterpret_cast<const uint8_t*>(end_hdr);
  const size_t body_size = size - size_t(end_hdr - text);
  if (fmt != Fmt::Ascii && (stride == 0 || n > body_size / stride))
    return fail("truncated vertex data");
  std::vector<float> row(v.props.size());
  std::istringstream as;
  if (fmt == Fmt::Ascii)
    as.str(std::string(reinterpret_cast<const char*>(body), body_size));

  std::vector<float> pos(n * 3), scl(n * 3), rot(n * 4), opa(n),
      sh(n * ncoef * 3, 0.f);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t r = 0; r < n; ++r) {
    if (fmt == Fmt::Ascii) {
      for (auto& f : row)
        if (!(as >> f)) return fail("truncated ASCII vertex data");
    } else {
      const uint8_t* rec = body + r * stride;
      for (size_t i = 0; i < row.size(); ++i)
        row[i] = float(ReadBin(rec + offset[i], v.props[i], swap));
    }
    for (int k = 0; k < 3; ++k) {
      pos[r * 3 + size_t(k)] = row[size_t(k == 0 ? ix : k == 1 ? iy : iz)];
      scl[r * 3 + size_t(k)] = std::exp(row[size_t(is[k])]);
    }
    float q[4], qn = 0.f;
    for (int k = 0; k < 4; ++k) {
      q[k] = row[size_t(ir[k])];
      qn += q[k] * q[k];
    }
    qn = qn > 0.f ? 1.f / std::sqrt(qn) : 0.f;
    if (qn == 0.f) { q[0] = 1.f; qn = 1.f; }
    for (int k = 0; k < 4; ++k) rot[r * 4 + size_t(k)] = q[k] * qn;
    opa[r] = 1.f / (1.f + std::exp(-row[size_t(iop)]));
    if (have_dc) {
      float* c = &sh[r * ncoef * 3];
      for (int k = 0; k < 3; ++k) c[k] = row[size_t(idc[k])];
      // f_rest is channel-major: [R1..Rm, G1..Gm, B1..Bm].
      for (size_t j = 1; j < ncoef; ++j)
        for (size_t ch = 0; ch < 3; ++ch)
          c[j * 3 + ch] = row[size_t(irest[ch * rest_per_channel + (j - 1)])];
    } else {
      // No radiance: neutral grey (DC 0 -> 0.5 after the +0.5 offset).
    }
    const float ext = 3.f * std::max(scl[r * 3], std::max(scl[r * 3 + 1],
                                                          scl[r * 3 + 2]));
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], pos[r * 3 + size_t(k)] - ext);
      hi[k] = std::max(hi[k], pos[r * 3 + size_t(k)] + ext);
    }
  }

  // ---- author the layer ----
  auto layer = std::make_shared<Layer>();
  layer->meta().defaultPrim = "Splats";
  const uint32_t id =
      layer->define_prim_at_path("/Splats", "ParticleField3DGaussianSplat");
  if (id == UINT32_MAX) return fail("cannot define /Splats");
  PrimSpec* prim = layer->prim_mutable(id);
  SetAttr(prim, "positions",
          Value::MakeArrayFromRaw(TypeId::Point3f, pos.data(), n), "point3f[]");
  SetAttr(prim, "orientations",
          Value::MakeArrayFromRaw(TypeId::Quatf, rot.data(), n), "quatf[]");
  SetAttr(prim, "scales",
          Value::MakeArrayFromRaw(TypeId::Float3, scl.data(), n), "float3[]");
  SetAttr(prim, "opacities", Value::MakeFloatArray(opa), "float[]");
  SetAttr(prim, "radiance:sphericalHarmonicsDegree", Value(int32_t(degree)),
          "int", /*uniform=*/true);
  SetAttr(prim, "radiance:sphericalHarmonicsCoefficients",
          Value::MakeArrayFromRaw(TypeId::Float3, sh.data(), n * ncoef),
          "float3[]");
  if (n) {
    const float extent[6] = {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
    SetAttr(prim, "extent", Value::MakeArrayFromRaw(TypeId::Float3, extent, 2),
            "float3[]");
  }
  return layer;
}

std::shared_ptr<Layer> LoadPLYLayerFromFile(const std::string& path,
                                            std::string* warn,
                                            std::string* err) {
  // Plain read: the core pipeline library has no mmap dependency. The decoded
  // arrays dominate memory anyway (the input is released before returning).
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (err) *err = "PLY '" + path + "': cannot open";
    return nullptr;
  }
  const std::streamoff size = f.tellg();
  if (size <= 0) {
    if (err) *err = "PLY '" + path + "': empty file";
    return nullptr;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) {
    if (err) *err = "PLY '" + path + "': read failed";
    return nullptr;
  }
  return LoadPLYLayerFromMemory(path, bytes.data(), bytes.size(), warn, err);
}

}  // namespace lightusd
