// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "usd-validation.hh"
#include "../eval/attribute-eval.hh"
#include "../resource-limits.hh"
#include "../expected.hh"
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <string_view>

namespace lightusd { namespace next {

struct ValidationPropertyDefinition {
  std::string type;
  bool relationship{false};
  std::string variability;
  std::vector<std::string> allowed_tokens;
};
struct ValidationSchemaDefinition {
  std::string name, parent, kind, property_namespace, source;
  std::map<std::string, ValidationPropertyDefinition> properties;
};
struct ValidationShaderDefinition {
  std::string identifier, source_type, source;
  std::map<std::string, std::string> inputs, outputs;
};

// Borrowed views: built-ins reference the immutable C arena; custom definitions
// reference their owning registry. Any registry mutation invalidates its views.
struct ValidationPropertyView {
  std::string_view type, variability;
  bool relationship{false};
};
struct ValidationSchemaView {
  std::string_view name, parent, kind, property_namespace, source;
  size_t index{0};
  const ValidationSchemaDefinition* custom{nullptr};
  std::optional<ValidationPropertyView> property(const std::string& name) const;
};
struct ValidationShaderView {
  std::string_view identifier, source_type, source;
  size_t index{0};
  const ValidationShaderDefinition* custom{nullptr};
  size_t port_count(bool output) const;
  std::pair<std::string_view, std::string_view> port(bool output, size_t index) const;
  std::optional<std::string_view> input_type(std::string_view name) const;
};

enum class ValidationScope { Layer, Stage, Prim };
struct ValidationRuleMetadata {
  std::string category;  // normative, schema, recommendation, profile, coverage
  std::string specification;
  std::vector<std::string> reference_errors;
  // Fully-qualified Lean 4 declaration (verif/lean/UsdSpec) that formalizes
  // the rule; empty when the rule has no formal counterpart.
  std::string lean_ref;
};
ValidationRuleMetadata GetValidationRuleMetadata(const std::string& rule);
// The Lean declaration reflected by `rule` ("" when none); see
// verif/lean/REFLECTION.md.
std::string GetValidationRuleLeanRef(const std::string& rule);

// All pointers are borrowed for the synchronous callback. A context never
// grants mutable access to the input. An absent stage means authored-layer
// validation; a prim callback additionally receives its prim argument.
struct ValidationContext {
  const Layer* layer{nullptr};
  const Stage* stage{nullptr};
  std::string source_asset;
  std::string variants;
  TimeQuery time{TimeQuery::Default()};
  ValidationOptions options;
  ResourceLimits limits;
};
struct ValidatorDefinition {
  std::string id, description;
  std::vector<std::string> keywords, schema_types;
  ValidationScope scope{ValidationScope::Prim};
  ValidationRuleMetadata metadata;
  std::function<expected<USDValidationResult, std::string>(
      const ValidationContext&, const PrimSpec*)> callback;
};

// Caller-owned snapshot. Load/register before sharing as const with a runner.
// Loading is transactional: a malformed manifest never partially registers.
class ValidationRegistry {
 public:
  ValidationRegistry();
  expected<bool, std::string> LoadSchemaDefinitions(
      const std::string& json);
  expected<bool, std::string> LoadShaderDefinitions(
      const std::string& json);
  expected<bool, std::string> RegisterValidator(ValidatorDefinition rule);
  const std::vector<ValidatorDefinition>& validators() const { return validators_; }
  std::optional<ValidationSchemaView> FindSchema(const std::string& name) const;
  bool InheritsFrom(const std::string& name, const std::string& ancestor) const;
  std::optional<ValidationPropertyView> FindProperty(
      const PrimSpec& prim, const std::string& name) const;
  std::optional<ValidationShaderView> FindShader(
      const std::string& id, const std::string& source_type = "") const;
  void RunCallbacks(ValidationScope scope, const ValidationContext& context,
                    const PrimSpec* prim, USDValidationResult* result) const;
 private:
  std::map<std::string, ValidationSchemaDefinition> schemas_;
  std::map<std::pair<std::string, std::string>, ValidationShaderDefinition> shaders_;
  std::vector<ValidatorDefinition> validators_;
};
const ValidationRegistry& GetBuiltinValidationRegistry();
void ValidateRegisteredProperties(const Layer& layer,
                                 const ValidationOptions& options,
                                 USDValidationResult* result);
// Additional scene-wide checks (subset families, bindings, shader registry).
void ValidateRegisteredPhysics(const Layer& layer, const ValidationOptions& options,
                               USDValidationResult* result);
void ValidateRegisteredStage(const Layer& layer, const ValidationOptions& options,
                             USDValidationResult* result);
// Evaluate at numeric samples with one coherent time across related properties.
// Existing DefaultTime checks are performed by ValidateLayerAgainstAOUSDCore.
USDValidationResult ValidateStageSamples(const Stage& stage,
    const ValidationContext& context, const EvalOptions& eval_options = {});

} }  // namespace lightusd::next
