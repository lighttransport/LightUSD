#pragma once
#include "value-to-json.hh"
namespace lightusd { namespace tydra { namespace detail {
bool NativeValueToMiniJSON(const value::Value &, uint32_t, minijson::Value *);
nonstd::optional<value::Value> NativeMiniJSONToValue(const minijson::Value &, std::string *, uint32_t);
} } }
