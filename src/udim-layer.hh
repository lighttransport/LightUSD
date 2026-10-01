// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <limits>
#include <map>

#include "minijson.hh"
#include "udim-mesh.hh"

namespace lightusd::udim {
struct Site {
  std::string path, pattern;
  bool srgb{true};
  double time{std::numeric_limits<double>::infinity()};
};
struct Plan {
  Site site;
  Layout layout;
  std::string asset;
};
struct Numeric {
  std::vector<double> values;
  size_t components{1};
  bool integral{false};
};
// Both layer implementations edit their authored specs directly. Values and
// metadata outside the generated UVs retain their original storage and type.
class LayerAccess {
 public:
  virtual ~LayerAccess();
  void setMemoryBudget(size_t bytes) { memory_budget_ = bytes; }
  size_t memoryBudget() const { return memory_budget_; }
  const std::string& accessError() const { return access_error_; }
  void accessFail(const std::string& error) const { access_error_ = error; }
  // String/token primvars use stable integer codes only while generating
  // stencils. Discrete contributors must agree; writers restore exact strings.
  template <class Get>
  bool encodeDiscrete(const std::string& key, size_t count, Get get,
                      Numeric* out) const {
    if (count > memory_budget_ / 128) return false;
    auto& table = discrete_[key];
    out->components = 1;
    out->integral = true;
    out->values.resize(count);
    for (size_t i = 0; i < count; ++i) {
      const std::string& value = get(i);
      auto found = table.ids.find(value);
      if (found == table.ids.end()) {
        if (value.size() > memory_budget_ / 2 ||
            discrete_bytes_ > memory_budget_ ||
            value.size() * 2 + 96 > memory_budget_ - discrete_bytes_) {
          accessFail(
              "UDIM bake: discrete primvars exceed working-memory limit");
          return false;
        }
        discrete_bytes_ += value.size() * 2 + 96;
        found = table.ids.emplace(value, table.values.size()).first;
        table.values.push_back(value);
      }
      out->values[i] = double(found->second);
    }
    return true;
  }
  bool decodeDiscrete(const std::string&, const Numeric&,
                      std::vector<std::string>*) const;
  virtual std::vector<std::string> paths() const = 0;
  virtual std::string bindingStrength(const std::string&) const { return ""; }
  virtual std::vector<std::string> referenceRoots(const std::string&) const {
    return {};
  }
  virtual std::string type(const std::string&) const = 0;
  virtual std::string text(
      const std::string&, const std::string&,
      double time = std::numeric_limits<double>::infinity()) const = 0;
  virtual std::vector<std::string> targets(const std::string&,
                                           const std::string&) const = 0;
  virtual bool dynamic(const std::string&, const std::string&) const = 0;
  virtual bool writeAsset(const std::string&, const std::string&, double) = 0;
  virtual bool grid(const Plan&, std::string*) = 0;
  virtual bool reader(const Plan&, const std::string&, std::string*) = 0;
  virtual std::vector<std::string> properties(const std::string&) const = 0;
  virtual std::vector<double> times(const std::string&,
                                    const std::string&) const = 0;
  virtual bool numeric(
      const std::string&, const std::string&, Numeric*,
      double time = std::numeric_limits<double>::infinity()) const = 0;
  virtual bool writeNumeric(
      const std::string&, const std::string&, const Numeric&,
      double time = std::numeric_limits<double>::infinity()) = 0;
  virtual std::string interpolation(const std::string&,
                                    const std::string&) const = 0;
  virtual void setInterpolation(const std::string&, const std::string&,
                                const std::string&) = 0;
  virtual void erase(const std::string&, const std::string&) = 0;
  virtual size_t elementSize(const std::string&, const std::string&) const = 0;
  virtual void setElementSize(const std::string&, const std::string&,
                              size_t) = 0;
  virtual void setToken(const std::string&, const std::string&,
                        const std::string&) = 0;
  virtual std::string cloneBlendShape(const std::string& mesh,
                                      const std::string& source) = 0;
  virtual void setTargets(const std::string&, const std::string&,
                          const std::vector<std::string>&) = 0;

 private:
  size_t memory_budget_{size_t(512) << 20};
  mutable std::string access_error_;
  struct Discrete {
    std::map<std::string, size_t> ids;
    std::vector<std::string> values;
  };
  mutable std::map<std::string, Discrete> discrete_;
  mutable size_t discrete_bytes_{0};
};
void NormalizeNormals(const std::string&, Numeric*);
bool DescribeLayer(const LayerAccess&, std::vector<Site>*, std::string*);
bool ApplyPlans(LayerAccess&, const std::vector<Plan>&, const Options&,
                std::string*);
std::string DescribeJSON(const LayerAccess&);
// JSON is a bounded transport for small edit plans, never for scene data.
std::string ApplyJSON(LayerAccess&, const std::string&);
}  // namespace lightusd::udim
