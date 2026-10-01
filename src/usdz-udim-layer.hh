// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/prim-spec.hh"
#include "layer.hh"
#include "udim-layer.hh"
namespace lightusd::usdz {
class LegacyUDIMLayer final : public udim::LayerAccess {
 public:
  explicit LegacyUDIMLayer(Layer& layer) : layer_(layer) {}
  std::vector<std::string> paths() const override;
  std::string type(const std::string&) const override;
  std::string bindingStrength(const std::string&) const override;
  std::string text(const std::string&, const std::string&,
                   double) const override;
  std::vector<std::string> targets(const std::string&,
                                   const std::string&) const override;
  bool dynamic(const std::string&, const std::string&) const override;
  bool writeAsset(const std::string&, const std::string&, double) override;
  bool grid(const udim::Plan&, std::string*) override;
  bool reader(const udim::Plan&, const std::string&, std::string*) override;
  std::vector<std::string> properties(const std::string&) const override;
  std::vector<double> times(const std::string&,
                            const std::string&) const override;
  bool numeric(const std::string&, const std::string&, udim::Numeric*,
               double) const override;
  bool writeNumeric(const std::string&, const std::string&,
                    const udim::Numeric&, double) override;
  std::string interpolation(const std::string&,
                            const std::string&) const override;
  void setInterpolation(const std::string&, const std::string&,
                        const std::string&) override;
  void erase(const std::string&, const std::string&) override;
  size_t elementSize(const std::string&, const std::string&) const override;
  void setElementSize(const std::string&, const std::string&, size_t) override;
  void setToken(const std::string&, const std::string&,
                const std::string&) override;
  std::string cloneBlendShape(const std::string&, const std::string&) override;
  void setTargets(const std::string&, const std::string&,
                  const std::vector<std::string>&) override;

 private:
  PrimSpec* prim(const std::string&) const;
  Attribute* attr(const std::string&, const std::string&) const;
  std::string child(const std::string&, PrimSpec);
  Layer& layer_;
};
}  // namespace lightusd::usdz
