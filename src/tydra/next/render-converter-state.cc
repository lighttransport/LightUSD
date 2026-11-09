// SPDX-License-Identifier: Apache-2.0
// Converter operation state and diagnostics (budget tracking, warnings,
// last-error). Split from render-converter.cc for translation-unit size.
#include "render-converter-internal.hh"

namespace lightusd {
namespace tydra {
namespace next {

bool RenderSceneConverter::Impl::BudgetWouldExceed(size_t estimate,
                                             const char* phase) {
  // Callable from the parallel per-record conversion phases (e.g. mesh
  // conversion, see ConvertMeshesParallel), so budget_*_ bookkeeping and the
  // resulting warning are both taken under state_mu_. Locks warnings_
  // directly rather than through AddWarning() -- AddWarning() takes the same
  // mutex, and it is non-recursive.
  ConverterStateLock lk(state_mu_);

  if (budget_exceeded_) return true;  // latched: stay degraded for this run

  size_t operation_limit = config_.limits.max_resident_bytes;
  if (config_.execution.max_in_flight_bytes) {
    operation_limit = std::min(operation_limit,
                               config_.execution.max_in_flight_bytes);
  }
  if (estimate > operation_limit ||
      budget_accounted_bytes_ > operation_limit - estimate) {
    budget_exceeded_ = true;
    warnings_.push_back(std::string("Operation memory limit reached during ") +
                        phase + "; remaining geometry is skipped");
    return true;
  }
  budget_accounted_bytes_ = SaturatingAdd(budget_accounted_bytes_, estimate);
  return false;
}

void RenderSceneConverter::Impl::ResetOperationState() {
  warnings_.clear();
  conversion_diagnostics_.clear();
  budget_accounted_bytes_ = 0;
  budget_exceeded_ = false;
}

void RenderSceneConverter::Impl::AddWarning(std::string msg) {
  ConverterStateLock lk(state_mu_);
  warnings_.push_back(std::move(msg));
}

void RenderSceneConverter::Impl::AddConversionWarning(
    const std::string& path, std::string msg,
    ConversionDisposition disposition) {
  ConverterStateLock lk(state_mu_);
  conversion_diagnostics_.push_back({disposition, path, msg});
  warnings_.push_back(std::move(msg));
}

void RenderSceneConverter::Impl::SetLastError(std::string msg) {
  ConverterStateLock lk(state_mu_);
  last_error_ = std::move(msg);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
