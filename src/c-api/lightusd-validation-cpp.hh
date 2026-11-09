// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lightusd-cpp.hh"
#include "lightusd-validation-c.h"
namespace lightusd { namespace api {
using ValidationReport = Owner<lightusd_validation_report, lightusd_validation_report_destroy>;
inline lightusd_status validate(const Stage& stage, ValidationReport* out,
                                const lightusd_validation_options* options = nullptr) {
  return out ? lightusd_stage_validate(stage.get(), options, out->put()) : LIGHTUSD_ERR_INVALID_ARG;
}
inline lightusd_status validate_core(const Stage& stage, size_t* errors, size_t* warnings) {
  return lightusd_stage_validate_core(stage.get(), errors, warnings);
}
} }
