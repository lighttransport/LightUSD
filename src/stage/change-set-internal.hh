// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Stage-to-stage change classification. Private; not installed.
#pragma once
#include "change-set.hh"
namespace lightusd { namespace next {
class Stage;
namespace change_set_detail {
StageChangeSet BuildStageChangeSet(const Stage* previous, const Stage& next,
                                 uint64_t base_revision, uint64_t new_revision);
}
} }
