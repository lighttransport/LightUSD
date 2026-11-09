// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Shared implementation details for loaders and the stage session. Not installed.
#pragma once
#include "load-usd.hh"
namespace lightusd { namespace next { namespace load_detail {
USDZReadOptions EffectiveUSDZOptions(const LoadUSDOptions& options);
std::string DirOfPath(const std::string& path);
} } }
