// SPDX-License-Identifier: Apache 2.0

///
/// @file prim-types.hh
/// @brief DEPRECATED - This header is no longer maintained.
///
/// Use specific headers instead:
///   core/prim.hh       - Prim class
///   core/prim-spec.hh  - PrimSpec class, prim:: typedefs
///   core/prim-node.hh  - PrimNode (deprecated)
///   core/prim-enums.hh - Specifier, Permission, Variability, etc.
///   core/path.hh       - Path class
///   core/extent.hh     - Extent struct
///   core/list-op.hh    - ListOp<T> templates
///   core/relationship.hh        - Relationship class
///   core/collection-api.hh      - Collection, CollectionInstance
///   core/composition-types.hh   - Reference, Payload, LayerOffset
///   core/model-scope.hh         - Model, Scope
///   xform.hh                    - Identity(), matrix ops
///
#pragma once

#ifdef _MSC_VER
#pragma message("prim-types.hh is deprecated. Include specific core/ headers instead (see comment above).")
#else
#warning "prim-types.hh is deprecated. Include specific core/ headers instead (see comment above)."
#endif

// Preserve backward compatibility for any external consumers
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "core/prim-node.hh"
#include "core/model-scope.hh"
#include "xform.hh"
