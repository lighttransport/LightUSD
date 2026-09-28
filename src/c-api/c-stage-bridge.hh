// SPDX-License-Identifier: Apache-2.0
// Private migration adapter; not part of the installed C/C++ API.
#pragma once
#include "lightusd-c.h"
namespace lightusd { namespace next { class Stage; } }
namespace lightusd { namespace next { class Layer; } }
namespace lightusd_internal {
// Temporary read-only borrow for native converters not yet migrated. The C
// owner must remain alive and unmodified for the duration of the borrow.
LIGHTUSD_API const lightusd::next::Stage* BorrowNativeStage(const lightusd_stage* stage);
// Install a parsed root layer into a newly-created C stage handle. The caller
// transfers ownership and must not use the layer afterward.
LIGHTUSD_API bool SetNativeRootLayer(lightusd_stage* stage,
                                     lightusd::next::Layer&& layer);
}
