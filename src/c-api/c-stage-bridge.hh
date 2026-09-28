// SPDX-License-Identifier: Apache-2.0
// Private migration adapter; not part of the installed C/C++ API.
#pragma once
#include "lightusd-c.h"
namespace lightusd { namespace next { class Stage; } }
namespace lightusd { namespace next { class Layer; class Value; } }
#include <string>
namespace lightusd_internal {
// Temporary read-only borrow for native converters not yet migrated. The C
// owner must remain alive and unmodified for the duration of the borrow.
LIGHTUSD_API const lightusd::next::Stage* BorrowNativeStage(const lightusd_stage* stage);
// Install a parsed root layer into a newly-created C stage handle. The caller
// transfers ownership and must not use the layer afterward.
LIGHTUSD_API bool SetNativeRootLayer(lightusd_stage* stage,
                                     lightusd::next::Layer&& layer);
// Exclusive-owner editing access to the root layer for edits the C API does
// not model (arc list clearing, variant definitions). Bumps the stage
// generation; the caller must hold the only reference to the stage.
LIGHTUSD_API lightusd::next::Layer* MutableNativeRootLayer(lightusd_stage* stage);
// Author an already-parsed value on the root layer (the C typed-buffer
// setters cover POD and string arrays; this carries any next Value, e.g. one
// parsed from USDA literal text). `type_name` is the declared USD type
// ("float3[]", "token", ...).
LIGHTUSD_API bool SetNativeAttribute(lightusd_stage* stage, const char* prim_path,
                                     const std::string& name,
                                     lightusd::next::Value&& value,
                                     const std::string& type_name, bool uniform,
                                     bool custom, std::string* err);
}
