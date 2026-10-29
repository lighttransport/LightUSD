// SPDX-License-Identifier: Apache-2.0
#include "lightusd-validation-c.h"
#include "c-internal.hh"
#include "next/validation/usd-validation.hh"
#include "next/validation/validation-context.hh"
#include <new>
namespace n = lightusd::next;
using lightusd_internal::Fail;
struct lightusd_validation_report {
  n::USDValidationResult result;
  std::vector<size_t> order;
};
namespace {
constexpr uint32_t kAllGroups = LIGHTUSD_VALIDATE_CORE | LIGHTUSD_VALIDATE_GEOM |
    LIGHTUSD_VALIDATE_SHADE | LIGHTUSD_VALIDATE_LUX | LIGHTUSD_VALIDATE_PHYSICS |
    LIGHTUSD_VALIDATE_RENDER | LIGHTUSD_VALIDATE_ARKIT;
constexpr uint32_t kAllFlags = LIGHTUSD_VALIDATE_SAMPLES |
    LIGHTUSD_VALIDATE_REQUIRE_COMPLETE | LIGHTUSD_VALIDATE_NO_ASSET_CHECKS;
uint32_t Bit(bool on, uint32_t bit) { return on ? bit : 0u; }
uint32_t Groups(const n::ValidationOptions& o) {
  return Bit(o.core, LIGHTUSD_VALIDATE_CORE) | Bit(o.geom, LIGHTUSD_VALIDATE_GEOM) |
      Bit(o.shade, LIGHTUSD_VALIDATE_SHADE) | Bit(o.lux, LIGHTUSD_VALIDATE_LUX) |
      Bit(o.physics, LIGHTUSD_VALIDATE_PHYSICS) | Bit(o.render, LIGHTUSD_VALIDATE_RENDER) |
      Bit(o.arkit, LIGHTUSD_VALIDATE_ARKIT);
}
lightusd_sv View(const std::string& text) { return {text.data(), text.size()}; }
}  // namespace
extern "C" {
lightusd_status lightusd_stage_validate_core(const lightusd_stage* stage,
                                              size_t* errors,
                                              size_t* warnings) {
  if (!stage || !errors || !warnings) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage/errors/warnings is null");
  }
  *errors = 0;
  *warnings = 0;
  const n::Layer* root = stage->ReadStage().GetRootLayer();
  if (!root) return Fail(LIGHTUSD_ERR_NOT_FOUND, "stage has no root layer");
  const n::USDValidationResult result = n::ValidateLayerAgainstAOUSDCore(*root);
  *errors = result.error_count();
  *warnings = result.warning_count();
  return LIGHTUSD_OK;
}

void lightusd_validation_options_init(lightusd_validation_options* options) {
  if (!options) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->groups = LIGHTUSD_VALIDATE_CORE;
  options->max_issues = 10000;
  options->max_report_bytes = 8ull * 1024 * 1024;
  options->max_samples = 256;
}
lightusd_status lightusd_stage_validate(const lightusd_stage* stage,
    const lightusd_validation_options* options, lightusd_validation_report** out) {
  if (out) *out = nullptr;
  if (!stage || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Validation stage/output is null");
  lightusd_validation_options defaults;
  lightusd_validation_options_init(&defaults);
  const auto& o = options ? *options : defaults;
  if (o.struct_size < sizeof(o) || !o.groups || (o.groups & ~kAllGroups) ||
      (o.flags & ~kAllFlags) || o._reserved || !o.max_issues || !o.max_report_bytes || !o.max_samples ||
      o.max_issues > SIZE_MAX || o.max_report_bytes > SIZE_MAX || o.max_samples > SIZE_MAX)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "Invalid validation options");
  const auto& native_stage = stage->ReadStage();
  const auto* root = native_stage.GetRootLayer();
  if (!root) return Fail(LIGHTUSD_ERR_NOT_FOUND, "Stage has no root layer");
  n::ValidationOptions native;
  native.core = (o.groups & LIGHTUSD_VALIDATE_CORE) != 0;
  native.geom = (o.groups & LIGHTUSD_VALIDATE_GEOM) != 0;
  native.shade = (o.groups & LIGHTUSD_VALIDATE_SHADE) != 0;
  native.lux = (o.groups & LIGHTUSD_VALIDATE_LUX) != 0;
  native.physics = (o.groups & LIGHTUSD_VALIDATE_PHYSICS) != 0;
  native.render = (o.groups & LIGHTUSD_VALIDATE_RENDER) != 0;
  native.arkit = (o.groups & LIGHTUSD_VALIDATE_ARKIT) != 0;
  native.require_complete = (o.flags & LIGHTUSD_VALIDATE_REQUIRE_COMPLETE) != 0;
  native.asset_checks = (o.flags & LIGHTUSD_VALIDATE_NO_ASSET_CHECKS) == 0;
  native.max_issues = static_cast<size_t>(o.max_issues);
  native.max_report_bytes = static_cast<size_t>(o.max_report_bytes);
  auto result = n::ValidateLayerAgainstAOUSDCore(*root, native);
  if (o.flags & LIGHTUSD_VALIDATE_SAMPLES) {
    n::ValidationContext context;
    context.stage = &native_stage;
    context.layer = root;
    context.options = native;
    context.limits.max_value_clip_samples = static_cast<size_t>(o.max_samples);
    n::MergeValidationResults(&result, n::ValidateStageSamples(native_stage, context));
  }
  auto* report = new (std::nothrow) lightusd_validation_report;
  if (!report) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate validation report");
  report->result = std::move(result);
  for (const auto* issue : n::GetOrderedValidationIssues(report->result))
    report->order.push_back(static_cast<size_t>(issue - report->result.issues.data()));
  *out = report;
  return LIGHTUSD_OK;
}
void lightusd_validation_report_destroy(lightusd_validation_report* report) { delete report; }
lightusd_status lightusd_validation_report_summary(const lightusd_validation_report* report,
    lightusd_validation_summary* out) {
  if (out) *out = {};
  if (!report || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Validation report/summary is null");
  const auto& r = report->result;
  *out = {r.issues.size(), r.error_count(), r.warning_count(), r.omitted_issues,
          Groups(r.checked_groups), uint8_t(r.complete), uint8_t(r.ok())};
  return LIGHTUSD_OK;
}
lightusd_status lightusd_validation_report_issue(const lightusd_validation_report* report,
    size_t index, lightusd_validation_issue* out) {
  if (out) *out = {};
  if (!report || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Validation report/issue is null");
  if (index >= report->order.size()) return Fail(LIGHTUSD_ERR_NOT_FOUND, "Validation issue index out of range");
  const auto& i = report->result.issues[report->order[index]];
  *out = {i.severity == n::USDValidationSeverity::Error ? 0u : 1u, View(i.rule_id),
          View(i.location), View(i.message), View(i.source_asset), View(i.variants),
          uint8_t(i.has_time), i.time};
  return LIGHTUSD_OK;
}

}
