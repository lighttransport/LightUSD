// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-2022 Syoyo Fujita.
// Copyright 2023-Present Light Transport Entertainment Inc.
//
// Common macros and definitions for crate-reader split files
//

#define kTag "[Crate]"

#define CHECK_MEMORY_USAGE(__nbytes) \
  MEMORY_BUDGET_CHECK((*memory_manager_), (__nbytes), kTag)

#define REDUCE_MEMORY_USAGE(__nbytes) \
  memory_manager_->Release(__nbytes)

#define VERSION_LESS_THAN_0_8_0(__version) ((_version[0] == 0) && (_version[1] < 7))
