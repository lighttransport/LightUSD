// SPDX-License-Identifier: Apache-2.0
#ifndef LIGHTUSD_VALIDATION_C_H_
#define LIGHTUSD_VALIDATION_C_H_
#include "lightusd-c.h"
#ifdef __cplusplus
extern "C" {
#endif
/* AOUSD Core validation of the stage's root layer. Validation findings are
 * reported as counts; a successfully checked but invalid layer still returns
 * LIGHTUSD_OK. */
LIGHTUSD_API lightusd_status lightusd_stage_validate_core(
    const lightusd_stage* stage, size_t* errors, size_t* warnings);

typedef struct lightusd_validation_report lightusd_validation_report;
typedef enum lightusd_validation_group {
  LIGHTUSD_VALIDATE_CORE = 1u,
  LIGHTUSD_VALIDATE_GEOM = 2u,
  LIGHTUSD_VALIDATE_SHADE = 4u,
  LIGHTUSD_VALIDATE_LUX = 8u,
  LIGHTUSD_VALIDATE_PHYSICS = 16u,
  LIGHTUSD_VALIDATE_RENDER = 32u,
  LIGHTUSD_VALIDATE_ARKIT = 64u
} lightusd_validation_group;
#define LIGHTUSD_VALIDATE_SAMPLES 1u
#define LIGHTUSD_VALIDATE_REQUIRE_COMPLETE 2u
#define LIGHTUSD_VALIDATE_NO_ASSET_CHECKS 4u
typedef struct lightusd_validation_options {
  uint32_t struct_size, groups, flags, _reserved;
  uint64_t max_issues, max_report_bytes, max_samples;
} lightusd_validation_options;
typedef struct lightusd_validation_summary {
  size_t issue_count, error_count, warning_count, omitted_issues;
  uint32_t checked_groups;
  uint8_t complete, valid;
} lightusd_validation_summary;
typedef struct lightusd_validation_issue {
  uint32_t severity; /* 0 = error, 1 = warning */
  lightusd_sv rule_id, location, message, source_asset, variants;
  uint8_t has_time;
  double time;
} lightusd_validation_issue;
/* Defaults: core checks, 10,000 retained issues, 8 MiB conservative diagnostic
 * accounting and 256 sampled times. All three limits must be positive. */
LIGHTUSD_API void lightusd_validation_options_init(lightusd_validation_options*);
/* Owned structured semantic report, independent of stage lifetime. Invalid USD
 * findings return OK with summary.valid == 0; invalid arguments return errors
 * and clear *out. Container-byte checks are outside this stage-only API.
 * Link lightusd_validation_c (or the shared C ABI). */
LIGHTUSD_API lightusd_status lightusd_stage_validate(const lightusd_stage*,
    const lightusd_validation_options*, lightusd_validation_report** out);
LIGHTUSD_API void lightusd_validation_report_destroy(lightusd_validation_report*);
LIGHTUSD_API lightusd_status lightusd_validation_report_summary(
    const lightusd_validation_report*, lightusd_validation_summary* out);
/* Index order matches native report presentation. Strings borrow from report.
 * Counts describe retained issues; omitted_issues > 0 forces complete/valid off. */
LIGHTUSD_API lightusd_status lightusd_validation_report_issue(
    const lightusd_validation_report*, size_t index, lightusd_validation_issue* out);
#ifdef __cplusplus
}
#endif
#endif
