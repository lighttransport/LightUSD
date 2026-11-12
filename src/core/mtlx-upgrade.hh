// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "core/mtlx-resolve.hh"

namespace lightusd {
namespace mtlx {

// Upgrade a supported legacy document root to the current MaterialX version.
// The canonical dump tools use this explicitly; MtlxDocument applies it during
// parsing.
void UpgradeMtlxVersion(MtlxElem *root);

}  // namespace mtlx
}  // namespace lightusd
