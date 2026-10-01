// SPDX-License-Identifier: Apache-2.0
#include "binding-next-udim.hh"

#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "udim-layer.hh"
namespace lightusd::web_next {
namespace {
namespace tn = lightusd::next;
class Access final : public udim::LayerAccess {
 public:
  explicit Access(tn::Layer& l) : l_(l) {}
  struct Address {
    tn::Layer* layer;
    std::string path;
    tn::VariantData* option;
  };
  mutable std::map<std::string, Address> addresses_;
  mutable std::set<std::string> local_geometry_;
  Address address(const std::string& p) const {
    const auto found = addresses_.find(p);
    return found == addresses_.end() ? Address{&l_, p, nullptr} : found->second;
  }
  tn::PrimSpec* prim(const std::string& p) const {
    const auto a = address(p);
    return a.layer->prim_at_path_mutable(a.path);
  }
  std::string fallback(const std::string& p) const {
    const auto brace = p.rfind('{');
    return !p.empty() && p.back() == '}' && brace != std::string::npos
               ? p.substr(0, brace)
               : "";
  }
  const tn::Value* value(
      const std::string& p, const std::string& prop,
      double t = std::numeric_limits<double>::infinity()) const {
    const auto* spec = prim(p);
    if (!spec) return nullptr;
    const auto id = tn::GetPropNameTable().find(prop);
    const auto* samples = spec->time_samples(id);
    if (std::isfinite(t) && samples)
      for (const auto& sample : *samples)
        if (sample.first == t) return spec->time_sample_value(sample.second);
    const auto* v = spec->property_value(prop);
    if (v && !v->is_empty()) return v;
    return samples && !samples->empty()
               ? spec->time_sample_value(samples->front().second)
               : nullptr;
  }
  std::vector<std::string> paths() const override {
    std::vector<std::string> out;
    addresses_.clear();
    local_geometry_.clear();
    std::function<void(tn::Layer&, const std::string&, tn::VariantData*,
                       size_t)>
        walk;
    walk = [&](tn::Layer& layer, const std::string& prefix,
               tn::VariantData* option, size_t depth) {
      if (depth > 64) {
        accessFail("UDIM bake: variant depth exceeds limit");
        return;
      }
      for (const auto& spec : layer.prims()) {
        if (out.size() >= memoryBudget() / 512) {
          accessFail("UDIM bake: layer traversal exceeds working-memory limit");
          return;
        }
        if (!layer.prim_at_path(spec.path())) continue;
        const auto actual = spec.path().str();
        if (!prefix.empty() && actual != "/__self__" &&
            actual.find("/__self__/") != 0) {
          accessFail("UDIM bake: invalid variant content root");
          return;
        }
        const auto path =
            prefix.empty()
                ? actual
                : prefix + actual.substr(std::string("/__self__").size());
        out.push_back(path);
        addresses_.emplace(
            path,
            Address{&layer, actual, actual == "/__self__" ? option : nullptr});
        auto* p = layer.prim_at_path_mutable(actual);
        for (const auto& prop :
             {"points", "faceVertexCounts", "faceVertexIndices"})
          if (p->property_value(prop) ||
              p->has_time_samples(tn::GetPropNameTable().find(prop)))
            local_geometry_.insert(path);
        for (auto& set : p->meta().variantSets())
          for (auto& variant : set.variants) {
            if (!variant.content && !variant.properties.empty()) {
              variant.content = std::make_shared<tn::Layer>();
              variant.content->define_prim_at_path("/__self__", "");
              auto* self = variant.content->prim_at_path_mutable("/__self__");
              for (const auto& prop : variant.properties)
                self->upsert_property(prop.name, prop.value, prop.flags);
            }
            if (variant.content)
              walk(*variant.content,
                   path + "{" + set.name + "=" + variant.name + "}", &variant,
                   depth + 1);
          }
      }
    };
    walk(l_, "", nullptr, 0);
    return out;
  }
  std::string bindingStrength(const std::string& p) const override {
    const auto* spec = prim(p);
    const auto* meta = spec ? spec->property_meta("material:binding") : nullptr;
    return meta ? meta->bindMaterialAs : "";
  }
  std::vector<std::string> referenceRoots(const std::string& p) const override {
    std::vector<std::string> out;
    if (const auto* spec = prim(p))
      for (const auto& ref : spec->meta().references)
        if (ref.size() > 3 && ref[0] == '<' && ref[1] == '/' &&
            ref.back() == '>') {
          const auto child = local_geometry_.lower_bound(p + "/");
          if (local_geometry_.count(p) ||
              (child != local_geometry_.end() && child->find(p + "/") == 0)) {
            accessFail(
                "UDIM bake: instance geometry overrides require composition "
                "before dense baking");
            return {};
          }
          out.push_back(ref.substr(1, ref.size() - 2));
        }
    return out;
  }
  std::string type(const std::string& p) const override {
    const auto* s = prim(p);
    return s && !s->type_name().empty() ? s->type_name()
           : !fallback(p).empty()       ? type(fallback(p))
                                        : "";
  }
  std::string text(const std::string& p, const std::string& prop,
                   double time) const override {
    const auto* spec = prim(p);
    const auto* v = std::isfinite(time) ? value(p, prop, time)
                    : spec              ? spec->property_value(prop)
                                        : nullptr;
    if (!v)
      return !fallback(p).empty() &&
                     (prop == "info:id" || prop == "inputs:sourceColorSpace")
                 ? text(fallback(p), prop, time)
                 : "";
    if (const auto* s = v->as_token()) return *s;
    if (const auto* s = v->as_asset_path()) return *s;
    if (const auto* s = v->as_string()) return *s;
    return "";
  }
  std::vector<std::string> targets(const std::string& p,
                                   const std::string& n) const override {
    std::vector<std::string> out;
    const auto* s = prim(p);
    if (!s) return out;
    if (const auto* t = s->connection(n))
      for (const auto& v : *t) out.push_back(v.str());
    if (const auto* t = s->relationship(n))
      for (const auto& v : *t) out.push_back(v.str());
    return out;
  }
  bool dynamic(const std::string& p, const std::string& n) const override {
    const auto* s = prim(p);
    return s && s->has_time_samples(tn::GetPropNameTable().find(n));
  }
  std::string child(const std::string& parent, const std::string& base) {
    std::string path = parent + "/" + base;
    size_t i = 0;
    while (l_.prim_at_path(path))
      path = parent + "/" + base + std::to_string(++i);
    const auto a = address(parent);
    const auto actual = a.path + "/" + path.substr(parent.size() + 1);
    a.layer->define_prim_at_path(actual, "Shader");
    addresses_[path] = Address{a.layer, actual, nullptr};
    return path;
  }
  void set(const std::string& p, const std::string& n, tn::Value v,
           const std::string& type = "") {
    auto* s = prim(p);
    s->upsert_property(n, std::move(v));
    if (!type.empty()) s->set_property_type_name(n, type);
  }
  bool writeAsset(const std::string& p, const std::string& asset,
                  double time) override {
    auto* spec = prim(p);
    if (!spec) return false;
    if (std::isfinite(time))
      spec->add_time_sample(tn::GetPropNameTable().intern("inputs:file"), time,
                            tn::Value::MakeAssetPath(asset));
    else {
      spec->upsert_property("inputs:file", tn::Value::MakeAssetPath(asset));
      if (auto* option = address(p).option)
        for (auto& prop : option->properties)
          if (prop.name == "inputs:file")
            prop.value = tn::Value::MakeAssetPath(asset);
    }
    return true;
  }
  bool grid(const udim::Plan& p, std::string*) override {
    tn::Value input = tn::Value::MakeFloat2(0, 0);
    if (const auto* v = value(p.site.path, "inputs:st")) input = *v;
    const auto links = targets(p.site.path, "inputs:st");
    // Copy authored time samples before define_prim grows the flat prim vector.
    std::vector<std::pair<double, tn::Value>> samples;
    for (double t : times(p.site.path, "inputs:st"))
      samples.push_back({t, *value(p.site.path, "inputs:st", t)});
    const auto path = child(p.site.path, "_udimAtlas");
    if (!prim(path)) return false;
    const auto& l = p.layout;
    set(path, "info:id", tn::Value::MakeToken("UsdTransform2d"), "token");
    set(path, "inputs:in", std::move(input), "float2");
    std::vector<tn::Path> targets;
    for (const auto& link : links) targets.push_back(tn::Path::Parse(link));
    if (!targets.empty())
      prim(path)->set_connection_targets("inputs:in", std::move(targets));
    for (const auto& s : samples)
      prim(path)->add_time_sample(tn::GetPropNameTable().intern("inputs:in"),
                                  s.first, s.second);
    set(path, "inputs:scale",
        tn::Value::MakeFloat2(1.0f / float(l.cols), 1.0f / float(l.rows)),
        "float2");
    set(path, "inputs:translation",
        tn::Value::MakeFloat2(-float(l.min_u) / float(l.cols),
                              -float(l.min_v) / float(l.rows)),
        "float2");
    set(path, "outputs:result", tn::Value(), "float2");
    finish(p, path);
    return true;
  }
  void finish(const udim::Plan& p, const std::string& source) {
    std::string logical;
    bool variant = false;
    for (char c : source) {
      if (c == '{')
        variant = true;
      else if (c == '}')
        variant = false;
      else if (!variant)
        logical += c;
    }
    prim(p.site.path)
        ->set_connection_targets(
            "inputs:st", {tn::Path::Parse(logical + ".outputs:result")});
    set(p.site.path, "inputs:wrapS", tn::Value::MakeToken("black"), "token");
    set(p.site.path, "inputs:wrapT", tn::Value::MakeToken("black"), "token");
  }
  bool reader(const udim::Plan& p, const std::string& uv,
              std::string*) override {
    const auto path = child(p.site.path, "_udimReader");
    if (!prim(path)) return false;
    set(path, "info:id", tn::Value::MakeToken("UsdPrimvarReader_float2"),
        "token");
    set(path, "inputs:varname", tn::Value::MakeToken(uv), "token");
    set(path, "outputs:result", tn::Value(), "float2");
    finish(p, path);
    return true;
  }
  std::vector<std::string> properties(const std::string& p) const override {
    std::vector<std::string> out;
    if (const auto* s = prim(p)) {
      for (const auto& slot : s->properties().slots())
        out.emplace_back(tn::GetPropNameTable().get(slot.name_id));
      for (const auto& rel : s->relationship_names()) out.push_back(rel);
    }
    return out;
  }
  std::vector<double> times(const std::string& p,
                            const std::string& n) const override {
    std::vector<double> out;
    if (const auto* s = prim(p))
      if (const auto* samples = s->time_samples(tn::GetPropNameTable().find(n)))
        for (const auto& t : *samples) out.push_back(t.first);
    return out;
  }
  bool numeric(const std::string& p, const std::string& n, udim::Numeric* out,
               double t) const override {
    const auto* v = value(p, n, t);
    if (!v) return false;
    if (v->is_array() && (v->type_id() == tn::TypeId::Token ||
                          v->type_id() == tn::TypeId::String)) {
      if (v->array_size() > memoryBudget() / 128) return false;
      if (const auto* values = v->as_token_array())
        return encodeDiscrete(
            p + "." + n, values->size(),
            [&](size_t i) -> const std::string& { return (*values)[i]; }, out);
    }
    out->components = tn::GetComponentCount(v->type_id());
    if (!out->components) return false;
    if (v->is_array()) {
      if (v->array_size() >
          memoryBudget() / (out->components * sizeof(double) * 4)) {
        accessFail("UDIM bake: numeric attribute exceeds working-memory limit");
        return false;
      }
      if (const auto* a = v->as_token_array())
        return encodeDiscrete(
            p + "." + n, a->size(),
            [&](size_t i) -> const std::string& { return (*a)[i]; }, out);
      if (const auto* a = v->as_float_array()) {
        out->values.assign(a->begin(), a->end());
        return true;
      }
      if (const auto* a = v->as_double_array()) {
        out->values = *a;
        return true;
      }
      if (const auto* a = v->as_int_array()) {
        out->values.assign(a->begin(), a->end());
        out->integral = true;
        return true;
      }
      return false;
    }
    const float* f = nullptr;
    const double* d = nullptr;
    if (out->components == 1) {
      f = v->as_float();
      d = v->as_double();
    } else if (out->components == 2) {
      f = v->as_float2();
      d = v->as_double2();
    } else if (out->components == 3) {
      f = v->as_float3();
      d = v->as_double3();
    } else if (out->components == 4) {
      f = v->as_float4();
      d = v->as_double4();
    }
    if (f) {
      out->values.assign(f, f + out->components);
      return true;
    }
    if (d) {
      out->values.assign(d, d + out->components);
      return true;
    }
    return false;
  }
  bool writeNumeric(const std::string& p, const std::string& n,
                    const udim::Numeric& src, double t) override {
    auto* spec = prim(p);
    if (!spec || !src.components || src.values.size() % src.components)
      return false;
    const auto* original = value(p, n, t);
    const auto type = original              ? original->type_id()
                      : src.integral        ? tn::TypeId::Int
                      : src.components == 2 ? tn::TypeId::Texcoord2f
                      : src.components == 3 ? tn::TypeId::Float3
                                            : tn::TypeId::Float;
    tn::Value v;
    if (original && original->as_token_array()) {
      std::vector<std::string> values;
      if (!decodeDiscrete(p + "." + n, src, &values)) return false;
      v = tn::Value::MakeStringLikeArray(std::move(values), type);
    } else if (original && original->as_double_array()) {
      std::vector<double> values = src.values;
      v = tn::Value::MakeDoubleCompArray(std::move(values), type,
                                         src.components);
    } else if (src.integral) {
      std::vector<int32_t> values;
      for (double x : src.values) {
        if (x < INT32_MIN || x > INT32_MAX || !std::isfinite(x)) return false;
        values.push_back(int32_t(x));
      }
      v = tn::Value::MakeIntCompArray(std::move(values), type, src.components);
    } else {
      std::vector<float> values;
      values.assign(src.values.begin(), src.values.end());
      v = tn::Value::MakeFloatCompArray(std::move(values), type,
                                        src.components);
    }
    if (std::isfinite(t))
      spec->add_time_sample(tn::GetPropNameTable().intern(n), t, std::move(v));
    else
      spec->upsert_property(n, std::move(v));
    if (!original)
      spec->set_property_type_name(n, src.integral          ? "int[]"
                                      : src.components == 2 ? "texCoord2f[]"
                                      : src.components == 3 ? "float3[]"
                                                            : "float[]");
    return true;
  }
  std::string interpolation(const std::string& p,
                            const std::string& n) const override {
    const auto* s = prim(p);
    const auto* m = s ? s->property_meta(n) : nullptr;
    return m ? m->interpolation : "";
  }
  void setInterpolation(const std::string& p, const std::string& n,
                        const std::string& i) override {
    auto& m = prim(p)->ensure_property_meta(n);
    m.interpolation = i;
    m.authored |= tn::PropMeta::kInterpolation;
  }
  void erase(const std::string& p, const std::string& n) override {
    if (auto* s = prim(p)) s->remove_property(n);
  }
  size_t elementSize(const std::string& p,
                     const std::string& n) const override {
    const auto* s = prim(p);
    const auto* m = s ? s->property_meta(n) : nullptr;
    return m ? size_t(m->elementSize) : 1;
  }
  void setElementSize(const std::string& p, const std::string& n,
                      size_t k) override {
    auto& m = prim(p)->ensure_property_meta(n);
    m.elementSize = int32_t(k);
    m.authored |= tn::PropMeta::kElementSize;
  }
  void setToken(const std::string& p, const std::string& n,
                const std::string& token) override {
    set(p, n, tn::Value::MakeToken(token), "token");
  }
  std::string cloneBlendShape(const std::string& mesh,
                              const std::string& source) override {
    const auto* original = prim(source);
    if (!original || original->child_count()) return "";
    tn::PrimSpec copy = original->Clone();
    const auto path = child(mesh, "_udimBlendShape");
    if (!prim(path)) return "";
    const auto name = prim(path)->name();
    *prim(path) = std::move(copy);
    prim(path)->set_path(tn::Path::Parse(path));
    prim(path)->set_name(name);
    return path;
  }
  void setTargets(const std::string& p, const std::string& n,
                  const std::vector<std::string>& targets) override {
    std::vector<tn::Path> t;
    for (const auto& v : targets) t.push_back(tn::Path::Parse(v));
    prim(p)->set_relationship_targets(n, std::move(t));
  }

 private:
  tn::Layer& l_;
};
}  // namespace
std::string UDIMLayerJSON(next::Layer& layer, bool apply,
                          const std::string& input) {
  Access a(layer);
  return apply ? udim::ApplyJSON(a, input) : udim::DescribeJSON(a);
}
}  // namespace lightusd::web_next
