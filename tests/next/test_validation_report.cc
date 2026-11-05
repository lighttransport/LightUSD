// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "c-api/lightusd-validation-cpp.hh"
#include "next/validation/usd-validation.hh"

namespace n = lightusd::next;
namespace api = lightusd::api;
void Check(bool ok, const char* why) {
  if (!ok) { std::cerr << why << ": " << lightusd_last_error() << '\n'; std::exit(1); }
}
bool Text(lightusd_sv text, const char* expected) {
  return text.len == std::strlen(expected) && std::memcmp(text.data, expected, text.len) == 0;
}
void NativeLimits() {
  n::USDValidationResult source;
  source.checked_groups.max_issues = 3;
  source.checked_groups.max_report_bytes = 4096;
  for (size_t i = 0; i < 100000; ++i)
    source.AddIssue({n::USDValidationSeverity::Error, "physics.mass", "/Body", "Invalid mass"});
  Check(source.issues.size() == 3 && source.omitted_issues == 99997 && !source.ok(), "native streaming report limit");
  Check(source.report_bytes <= 4096 && !source.complete, "native byte accounting");
  n::USDValidationResult merged;
  merged.checked_groups.max_issues = 2;
  n::MergeValidationResults(&merged, source);
  Check(merged.issues.size() == 2 && merged.omitted_issues == 99998 && !merged.complete, "merge respects destination limits and omissions");
  Check(n::FormatValidationResult(merged).find("Omitted 99998") != std::string::npos, "text reports declare omissions");
  n::USDValidationResult tiny;
  tiny.checked_groups.max_report_bytes = 1;
  tiny.AddIssue({n::USDValidationSeverity::Error, "physics.mass", "/Body", "Invalid mass"});
  Check(tiny.issues.empty() && tiny.omitted_issues == 1 && !tiny.ok(), "no false success when all errors are omitted");
}
void OwnedReport() {
  const char* source = R"(#usda 1.0
def Xform "Body" (prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]) {
  float physics:mass = -1
}
def PhysicsRevoluteJoint "Joint" { rel physics:body1 = </Missing>; }
)";
  api::Stage stage;
  lightusd_stage* raw = nullptr;
  Check(lightusd_stage_load_from_memory(reinterpret_cast<const uint8_t*>(source),
      std::strlen(source), nullptr, &raw) == LIGHTUSD_OK, "load input");
  stage = api::Stage(raw);
  lightusd_validation_options options;
  lightusd_validation_options_init(&options);
  options.groups = LIGHTUSD_VALIDATE_PHYSICS;
  api::ValidationReport report;
  Check(api::validate(stage, &report, &options) == LIGHTUSD_OK, "structured C++ facade");
  stage = api::Stage();
  lightusd_validation_summary summary{};
  Check(lightusd_validation_report_summary(report.get(), &summary) == LIGHTUSD_OK, "owned summary after stage destruction");
  Check(summary.complete && !summary.valid && summary.error_count && summary.checked_groups == LIGHTUSD_VALIDATE_PHYSICS, "physics scope and findings");
  bool mass = false, target = false;
  for (size_t i = 0; i < summary.issue_count; ++i) {
    lightusd_validation_issue issue{};
    Check(lightusd_validation_report_issue(report.get(), i, &issue) == LIGHTUSD_OK, "issue iterator");
    mass |= Text(issue.rule_id, "physics.mass") && Text(issue.location, "/Body");
    target |= Text(issue.rule_id, "physics.joint.bodyTarget");
    Check(!issue.has_time, "default checks have no numeric time annotation");
  }
  Check(mass && target, "native rule identities retained");
  lightusd_validation_issue issue{};
  Check(lightusd_validation_report_issue(report.get(), SIZE_MAX, &issue) == LIGHTUSD_ERR_NOT_FOUND &&
      !issue.location.data, "invalid issue clears output");
  Check(lightusd_validation_report_summary(nullptr, &summary) == LIGHTUSD_ERR_INVALID_ARG &&
      !summary.issue_count, "null report clears summary");
  lightusd_validation_report* invalid = reinterpret_cast<lightusd_validation_report*>(uintptr_t(1));
  Check(lightusd_stage_validate(nullptr, nullptr, &invalid) == LIGHTUSD_ERR_INVALID_ARG && !invalid, "failed call clears report");
  Check(stage.create() == LIGHTUSD_OK, "empty stage");
  options.max_issues = 0;
  Check(lightusd_stage_validate(stage.get(), &options, &invalid) == LIGHTUSD_ERR_INVALID_ARG && !invalid, "zero limit rejected");
  lightusd_validation_options_init(&options);
  options.groups = 1u << 31;
  Check(lightusd_stage_validate(stage.get(), &options, &invalid) == LIGHTUSD_ERR_INVALID_ARG && !invalid, "unknown group rejected");
  options.groups = LIGHTUSD_VALIDATE_PHYSICS;
  const char* fallbacks[] = {
      "quatf physics:principalAxes = (1, 0, 0, 0)",
      "float3 physics:diagonalInertia = (1, 2, 3)",
      "float3 physics:diagonalInertia = (0, 0, 0)\nquatf physics:principalAxes = (1, 0, 0, 0)",
      "float3 physics:diagonalInertia = (1, 2, 3)\nquatf physics:principalAxes = (0, 0, 0, 0)"};
  for (const char* attributes : fallbacks) {
    const std::string text = std::string("#usda 1.0\ndef Xform \"Body\" (prepend apiSchemas = [\"PhysicsMassAPI\", \"PhysicsRigidBodyAPI\"]) {\n") + attributes + "\n}";
    Check(stage.load_from_memory(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == LIGHTUSD_OK, "inertia fallback fixture");
    Check(api::validate(stage, &report, &options) == LIGHTUSD_OK, "inertia fallback report");
    Check(lightusd_validation_report_summary(report.get(), &summary) == LIGHTUSD_OK && summary.valid,
          "independent inertia and axes fallbacks are valid");
  }
}
int main() {
  NativeLimits();
  OwnedReport();
  std::cout << "Owned validation reports, native limits, lifetime and C/C++ parity passed\n";
}
