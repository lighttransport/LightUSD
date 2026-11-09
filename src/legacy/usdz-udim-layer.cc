// SPDX-License-Identifier: Apache-2.0
#if defined(__clang__)
// Exact equality is required for authored time keys and discrete opinions.
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#include "usdz-udim-layer.hh"

#include <cmath>
#include <cstring>

#include "core/attribute.hh"
#include "timesamples.hh"
namespace lightusd::usdz {
namespace {
bool Paths(const PrimSpec& prim, const std::string& path,
           std::vector<std::string>* out, const udim::LayerAccess& access,
           size_t depth = 0) {
  if (depth > 512 || out->size() >= access.memoryBudget() / 512) {
    access.accessFail(
        "UDIM bake: layer traversal exceeds memory or depth limit");
    return false;
  }
  out->push_back(path);
  for (const auto& c : prim.children())
    if (!Paths(c, path + "/" + c.name(), out, access, depth + 1)) return false;
  for (const auto& set : prim.variantSets())
    for (const auto& variant : set.second.variantSet)
      if (!Paths(variant.second,
                 path + "{" + set.first + "=" + variant.first + "}", out,
                 access, depth + 1))
        return false;
  return true;
}
std::string WithoutVariants(const std::string& path) {
  std::string result;
  bool variant = false;
  for (char c : path) {
    if (c == '{')
      variant = true;
    else if (c == '}')
      variant = false;
    else if (!variant)
      result += c;
  }
  return result;
}
template <class T, class S, size_t C>
bool Read(const value::Value& v, udim::Numeric* n,
          const udim::LayerAccess& access) {
  const auto view = v.as_view<T>(false);
  if (!view.data() && !v.as<std::vector<T>>(false)) return false;
  if (view.size() > access.memoryBudget() / (C * sizeof(double) * 4)) {
    access.accessFail(
        "UDIM bake: numeric attribute exceeds working-memory limit");
    return false;
  }
  n->components = C;
  n->integral = std::is_integral<S>::value;
  n->values.resize(view.size() * C);
  for (size_t i = 0; i < view.size(); ++i) {
    S lanes[C];
    std::memcpy(lanes, &view[i], sizeof(lanes));
    for (size_t j = 0; j < C; ++j) n->values[i * C + j] = double(lanes[j]);
  }
  return true;
}
template <class T>
bool SetSample(Attribute* attr, const T& value, double time, size_t budget) {
  if (!std::isfinite(time)) {
    attr->set_value(value);
    return true;
  }
  const auto& source = attr->get_var().ts_raw();
  size_t count = 0;
  for (size_t i = 0; i < source.size(); ++i) {
    const size_t n = source.get_array_count(i);
    if (count > budget / 128 || n > budget / 128 - count) return false;
    count += n;
  }
  value::TimeSamples replacement;
  bool inserted = false;
  for (const auto& sample : attr->get_var().ts_raw().get_samples()) {
    if (sample.t == time) {
      value::TimeSamples::Sample next = sample;
      next.value = value::Value(value);
      next.blocked = false;
      next.value_group = 0;
      if (!replacement.add_sample(next)) return false;
      inserted = true;
    } else if (!replacement.add_sample(sample))
      return false;
  }
  if (!inserted && !replacement.add_sample(time, value::Value(value)))
    return false;
  attr->get_var().set_timesamples(std::move(replacement));
  return true;
}
template <class T, class S, size_t C>
bool Write(Attribute* attr, const udim::Numeric& n, double time,
           const value::Value& original, size_t budget) {
  if (!original.as_view<T>(true).data() && !original.as<std::vector<T>>(true))
    return false;
  std::vector<T> out(n.values.size() / C);
  for (size_t i = 0; i < out.size(); ++i) {
    S lanes[C];
    for (size_t j = 0; j < C; ++j) lanes[j] = S(n.values[i * C + j]);
    std::memcpy(&out[i], lanes, sizeof(lanes));
  }
  return SetSample(attr, out, time, budget);
}
}  // namespace
PrimSpec* LegacyUDIMLayer::prim(const std::string& path) const {
  if (path.empty() || path[0] != '/') return nullptr;
  PrimSpec* current = nullptr;
  size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto segment = path.substr(start, end - start);
    const auto brace = segment.find('{');
    const auto name = segment.substr(0, brace);
    if (!current) {
      auto i = layer_.primspecs().find(name);
      if (i == layer_.primspecs().end()) return nullptr;
      current = &i->second;
    } else {
      PrimSpec* child = nullptr;
      for (auto& c : current->children())
        if (c.name() == name) {
          child = &c;
          break;
        }
      current = child;
      if (!current) return nullptr;
    }
    size_t variant = brace;
    while (variant != std::string::npos) {
      const auto equal = segment.find('=', variant + 1);
      const auto close = segment.find('}', variant + 1);
      if (equal == std::string::npos || close == std::string::npos ||
          equal > close)
        return nullptr;
      auto set = current->variantSets().find(
          segment.substr(variant + 1, equal - variant - 1));
      if (set == current->variantSets().end()) return nullptr;
      auto option = set->second.variantSet.find(
          segment.substr(equal + 1, close - equal - 1));
      if (option == set->second.variantSet.end()) return nullptr;
      current = &option->second;
      variant = segment.find('{', close + 1);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return current;
}
Attribute* LegacyUDIMLayer::attr(const std::string& path,
                                 const std::string& name) const {
  auto* p = prim(path);
  if (!p) return nullptr;
  auto i = p->props().find(name);
  return i == p->props().end() ? nullptr : i->second.get_attribute_or_null();
}
std::vector<std::string> LegacyUDIMLayer::paths() const {
  std::vector<std::string> out;
  for (const auto& p : layer_.primspecs())
    if (!Paths(p.second, "/" + p.second.name(), &out, *this)) break;
  return out;
}
std::string LegacyUDIMLayer::bindingStrength(const std::string& path) const {
  const auto* p = prim(path);
  if (!p) return "";
  auto found = p->props().find("material:binding");
  if (found == p->props().end()) return "";
  const auto* rel = found->second.get_relationship_or_null();
  return rel ? rel->metas().get_bindMaterialAs().str() : "";
}
std::string LegacyUDIMLayer::type(const std::string& p) const {
  const auto* v = prim(p);
  return v ? v->typeName() : "";
}
std::string LegacyUDIMLayer::text(const std::string& p, const std::string& name,
                                  double time) const {
  const auto* a = attr(p, name);
  if (!a) {
    const auto fallback = WithoutVariants(p);
    return fallback != p &&
                   (name == "info:id" || name == "inputs:sourceColorSpace")
               ? text(fallback, name, time)
               : "";
  }
  const value::Value* v = &a->get_var().value_raw();
  if (std::isfinite(time)) {
    v = nullptr;
    for (const auto& sample : a->get_var().ts_raw().get_samples())
      if (sample.t == time) {
        v = &sample.value;
        break;
      }
  }
  if (!v) return "";
  if (auto t = v->get_value<value::token>()) return t->str();
  if (auto f = v->get_value<value::AssetPath>()) return f->GetAssetPath();
  if (auto str = v->get_value<std::string>()) return *str;
  return "";
}
bool LegacyUDIMLayer::writeAsset(const std::string& p, const std::string& asset,
                                 double time) {
  auto* a = attr(p, "inputs:file");
  return a && SetSample(a, value::AssetPath(asset), time, memoryBudget());
}
std::vector<std::string> LegacyUDIMLayer::targets(
    const std::string& path, const std::string& name) const {
  std::vector<std::string> out;
  const auto* p = prim(path);
  if (!p) return out;
  auto i = p->props().find(name);
  if (i == p->props().end()) return out;
  if (const auto* a = i->second.get_attribute_or_null())
    for (const auto& t : a->connections()) out.push_back(t.full_path_name());
  if (const auto* r = i->second.get_relationship_or_null()) {
    if (r->is_path())
      out.push_back(r->targetPath.full_path_name());
    else if (r->is_pathvector())
      for (const auto& t : r->targetPathVector)
        out.push_back(t.full_path_name());
  }
  return out;
}
bool LegacyUDIMLayer::dynamic(const std::string& p,
                              const std::string& n) const {
  const auto* a = attr(p, n);
  return a && a->has_timesamples();
}
std::string LegacyUDIMLayer::child(const std::string& path, PrimSpec new_prim) {
  auto* p = prim(path);
  const std::string base = new_prim.name();
  size_t i = 0;
  for (;;) {
    bool used = false;
    for (const auto& c : p->children())
      if (c.name() == new_prim.name()) used = true;
    if (!used) break;
    new_prim.name() = base + std::to_string(++i);
  }
  const auto result = path + "/" + new_prim.name();
  p->children().push_back(std::move(new_prim));
  return result;
}
bool LegacyUDIMLayer::grid(const udim::Plan& plan, std::string*) {
  Attribute input(value::float2{0, 0});
  if (auto* a = attr(plan.site.path, "inputs:st")) input = *a;
  input.set_type_name("float2");
  PrimSpec transform(Specifier::Def, "Shader", "_udimAtlas");
  const auto& l = plan.layout;
  transform.props()["info:id"] =
      Attribute::Uniform(value::token("UsdTransform2d"));
  transform.props()["inputs:in"] = std::move(input);
  transform.props()["inputs:scale"] =
      Attribute(value::float2{1.0f / float(l.cols), 1.0f / float(l.rows)});
  transform.props()["inputs:translation"] = Attribute(value::float2{
      -float(l.min_u) / float(l.cols), -float(l.min_v) / float(l.rows)});
  Attribute output;
  output.set_type_name("float2");
  transform.props()["outputs:result"] = output;
  const auto generated = child(plan.site.path, std::move(transform));
  auto* p = prim(plan.site.path);
  Attribute st(Path(WithoutVariants(generated), "outputs:result"));
  st.set_type_name("float2");
  p->props()["inputs:st"] = st;
  p->props()["inputs:wrapS"] = Attribute(value::token("black"));
  p->props()["inputs:wrapT"] = Attribute(value::token("black"));
  return true;
}
bool LegacyUDIMLayer::reader(const udim::Plan& plan, const std::string& uv,
                             std::string*) {
  PrimSpec r(Specifier::Def, "Shader", "_udimReader");
  r.props()["info:id"] =
      Attribute::Uniform(value::token("UsdPrimvarReader_float2"));
  r.props()["inputs:varname"] = Attribute(value::token(uv));
  Attribute out;
  out.set_type_name("float2");
  r.props()["outputs:result"] = out;
  const auto path = child(plan.site.path, std::move(r));
  auto* p = prim(plan.site.path);
  Attribute st(Path(path, "outputs:result"));
  st.set_type_name("float2");
  p->props()["inputs:st"] = st;
  p->props()["inputs:wrapS"] = Attribute(value::token("black"));
  p->props()["inputs:wrapT"] = Attribute(value::token("black"));
  return true;
}
std::vector<std::string> LegacyUDIMLayer::properties(
    const std::string& path) const {
  std::vector<std::string> out;
  if (const auto* p = prim(path))
    for (const auto& v : p->props()) out.push_back(v.first);
  return out;
}
std::vector<double> LegacyUDIMLayer::times(const std::string& path,
                                           const std::string& name) const {
  std::vector<double> out;
  if (const auto* a = attr(path, name))
    for (size_t i = 0; i < a->get_var().ts_raw().size(); ++i)
      if (const auto t = a->get_var().ts_raw().get_time(i)) out.push_back(*t);
  return out;
}
static const value::Value* NumericValue(const Attribute& a, double time,
                                        value::TimeSamples::Sample* scratch,
                                        const udim::LayerAccess& access) {
  const auto& samples = a.get_var().ts_raw();
  size_t index = samples.size();
  if (std::isfinite(time))
    for (size_t i = 0; i < samples.size(); ++i)
      if (samples.get_time(i).value_or(
              std::numeric_limits<double>::infinity()) == time) {
        index = i;
        break;
      }
  if (index == samples.size() && !a.get_var().value_raw().is_empty())
    return &a.get_var().value_raw();
  if (index == samples.size()) index = 0;
  if (samples.get_array_count(index) > access.memoryBudget() / 128) {
    access.accessFail("UDIM bake: time sample exceeds working-memory limit");
    return nullptr;
  }
  return samples.get_sample_at(index, scratch) && !scratch->blocked
             ? &scratch->value
             : nullptr;
}
bool LegacyUDIMLayer::numeric(const std::string& path, const std::string& name,
                              udim::Numeric* n, double time) const {
  const auto* a = attr(path, name);
  if (!a) return false;
  value::TimeSamples::Sample scratch;
  const auto* v = NumericValue(*a, time, &scratch, *this);
  if (!v) return false;
#define READ(T, S, C) \
  if (Read<T, S, C>(*v, n, *this)) return true
  READ(float, float, 1);
  READ(double, double, 1);
  READ(int32_t, int32_t, 1);
  READ(value::float2, float, 2);
  READ(value::float3, float, 3);
  READ(value::float4, float, 4);
  READ(value::double2, double, 2);
  READ(value::double3, double, 3);
  READ(value::double4, double, 4);
#undef READ
  if (auto values = v->as<std::vector<std::string>>())
    return encodeDiscrete(
        path + "." + name, values->size(),
        [&](size_t i) -> const std::string& { return (*values)[i]; }, n);
  if (auto values = v->as<std::vector<value::token>>())
    return encodeDiscrete(
        path + "." + name, values->size(),
        [&](size_t i) -> const std::string& { return (*values)[i].str(); }, n);
  if (auto f = v->get_value<float>()) {
    n->values = {double(*f)};
    return true;
  }
  if (auto f = v->get_value<value::float2>()) {
    n->values = {double((*f)[0]), double((*f)[1])};
    n->components = 2;
    return true;
  }
  return false;
}
bool LegacyUDIMLayer::writeNumeric(const std::string& path,
                                   const std::string& name,
                                   const udim::Numeric& n, double time) {
  auto* p = prim(path);
  if (!p || !n.components || n.values.size() % n.components) return false;
  auto* a = attr(path, name);
  if (!a) {
    Attribute generated;
    generated.set_type_name(n.components == 2 ? "texCoord2f[]" : "float[]");
    p->props()[name] = std::move(generated);
    a = attr(path, name);
  }
  value::TimeSamples::Sample scratch;
  const auto* v = NumericValue(*a, time, &scratch, *this);
  value::Value original = v ? *v : value::Value();
  if (v && (v->as<std::vector<std::string>>() ||
            v->as<std::vector<value::token>>())) {
    std::vector<std::string> values;
    if (!decodeDiscrete(path + "." + name, n, &values)) return false;
    if (v->as<std::vector<std::string>>())
      return SetSample(a, values, time, memoryBudget());
    std::vector<value::token> tokens;
    for (const auto& text : values) tokens.emplace_back(text);
    return SetSample(a, tokens, time, memoryBudget());
  }
#define WRITE(T, S, C) \
  if (Write<T, S, C>(a, n, time, original, memoryBudget())) return true
  WRITE(value::point3f, float, 3);
  WRITE(value::vector3f, float, 3);
  WRITE(value::normal3f, float, 3);
  WRITE(value::color3f, float, 3);
  WRITE(value::color4f, float, 4);
  WRITE(value::texcoord2f, float, 2);
  WRITE(value::texcoord3f, float, 3);
  WRITE(value::point3d, double, 3);
  WRITE(value::vector3d, double, 3);
  WRITE(value::normal3d, double, 3);
  WRITE(value::color3d, double, 3);
  WRITE(value::color4d, double, 4);
  WRITE(value::texcoord2d, double, 2);
  WRITE(value::texcoord3d, double, 3);
  WRITE(float, float, 1);
  WRITE(double, double, 1);
  WRITE(int32_t, int32_t, 1);
  WRITE(value::float2, float, 2);
  WRITE(value::float3, float, 3);
  WRITE(value::float4, float, 4);
  WRITE(value::double2, double, 2);
  WRITE(value::double3, double, 3);
  WRITE(value::double4, double, 4);
#undef WRITE
  if (!v) {
    if (n.components == 1) {
      if (n.integral) {
        std::vector<int32_t> out;
        for (double x : n.values) out.push_back(int32_t(x));
        a->set_type_name("int[]");
        if (!SetSample(a, out, time, memoryBudget())) return false;
      } else {
        std::vector<float> out(n.values.begin(), n.values.end());
        a->set_type_name("float[]");
        if (!SetSample(a, out, time, memoryBudget())) return false;
      }
      return true;
    }
    if (n.components == 2) {
      std::vector<value::float2> out;
      for (size_t i = 0; i < n.values.size(); i += 2)
        out.push_back(
            value::float2{float(n.values[i]), float(n.values[i + 1])});
      a->set_type_name("texCoord2f[]");
      if (!SetSample(a, out, time, memoryBudget())) return false;
      return true;
    }
    if (n.components == 3) {
      std::vector<value::float3> out;
      for (size_t i = 0; i < n.values.size(); i += 3)
        out.push_back(value::float3{float(n.values[i]), float(n.values[i + 1]),
                                    float(n.values[i + 2])});
      a->set_type_name("float3[]");
      if (!SetSample(a, out, time, memoryBudget())) return false;
      return true;
    }
  }
  return false;
}
std::string LegacyUDIMLayer::interpolation(const std::string& p,
                                           const std::string& n) const {
  const auto* a = attr(p, n);
  return a && a->metas().has_interpolation()
             ? a->metas().get_interpolation().str()
             : "";
}
void LegacyUDIMLayer::setInterpolation(const std::string& p,
                                       const std::string& n,
                                       const std::string& i) {
  if (auto* a = attr(p, n)) a->metas().set_interpolation(value::token(i));
}
void LegacyUDIMLayer::erase(const std::string& p, const std::string& n) {
  if (auto* v = prim(p)) v->props().erase(n);
}
size_t LegacyUDIMLayer::elementSize(const std::string& p,
                                    const std::string& n) const {
  const auto* a = attr(p, n);
  return a && a->metas().has_elementSize()
             ? size_t(a->metas().get_elementSize())
             : 1;
}
void LegacyUDIMLayer::setElementSize(const std::string& p, const std::string& n,
                                     size_t k) {
  if (auto* a = attr(p, n)) a->metas().set_elementSize(uint32_t(k));
}
void LegacyUDIMLayer::setToken(const std::string& p, const std::string& n,
                               const std::string& token) {
  if (auto* spec = prim(p)) spec->props()[n] = Attribute(value::token(token));
}
std::string LegacyUDIMLayer::cloneBlendShape(const std::string& mesh,
                                             const std::string& source) {
  const auto* p = prim(source);
  if (!p || !p->children().empty()) return "";
  PrimSpec copy = *p;
  copy.name() = "_udimBlendShape";
  return child(mesh, std::move(copy));
}
void LegacyUDIMLayer::setTargets(const std::string& p, const std::string& n,
                                 const std::vector<std::string>& t) {
  std::vector<Path> paths;
  for (const auto& v : t) paths.emplace_back(v, "");
  if (auto* spec = prim(p)) {
    spec->props()[n].relationship().set(std::move(paths));
  }
}
}  // namespace lightusd::usdz
