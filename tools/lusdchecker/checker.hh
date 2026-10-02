// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "next/validation/validation-context.hh"
namespace lusdchecker {
// Copy the caller's registry into a run-local snapshot, load CLI manifests,
// then validate. The caller may reuse its registry after this returns.
int RunChecker(int argc, char** argv,
               const lightusd::next::ValidationRegistry& registry);
}
