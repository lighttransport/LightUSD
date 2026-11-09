// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Namespace bridge for the legacy implementation.
//
// Legacy code lives in `lightusd::legacy`. The core implementation owns the
// plain `lightusd::` names, and shared helpers (image/io/tydra asset import,
// tsd, mtlx, ...) live in `lightusd::<ns>`. This header is force-included into
// every legacy C++ translation unit (see the legacy section of the root
// CMakeLists.txt and the legacy WASM target).
//
// For every namespace `N` that both legacy and shared code (src/, outside
// src/legacy) define under `lightusd` (tydra, value, mcp, mtlx, tsd), legacy
// code in `lightusd::legacy::N` sees the shared `lightusd::N` members (both
// unqualified and as `N::X`). Shared namespaces that legacy does not define
// (io, image, ...) need no bridge: legacy reaches them by ordinary
// enclosing-scope lookup. The bridge is
// one-way on purpose: nothing in `lightusd` scope refers back to `legacy`, so
// shared and core code never see legacy names and cannot become ambiguous.
// Legacy consumers name legacy entities as `lightusd::legacy::X` (or add
// `using namespace lightusd::legacy;`).
#ifndef LIGHTUSD_LEGACY_NAMESPACE_HH_
#define LIGHTUSD_LEGACY_NAMESPACE_HH_

#ifdef __cplusplus

#define LIGHTUSD_LEGACY_BRIDGE_NS(N)                                   \
  namespace N {}                                                       \
  namespace legacy {                                                   \
  namespace N {                                                        \
  using namespace ::lightusd::N; /* NOLINT(google-build-using-namespace) */ \
  }                                                                    \
  }

namespace lightusd {
namespace legacy {}

// Shared types that legacy headers only forward-declare. Declared here, in
// their real namespaces, so a legacy forward declaration can never introduce
// a second (incomplete) `lightusd::legacy::...` type of the same name.
namespace io { struct MMapFileHandle; }
namespace minijson { class Value; }
namespace mtlx {
class MtlxNodeGraph;
class MtlxNode;
class MtlxInput;
class MtlxOutput;
}  // namespace mtlx
namespace tydra { namespace import_asset { struct Options; } }

LIGHTUSD_LEGACY_BRIDGE_NS(mcp)
LIGHTUSD_LEGACY_BRIDGE_NS(mtlx)
LIGHTUSD_LEGACY_BRIDGE_NS(tsd)
LIGHTUSD_LEGACY_BRIDGE_NS(tydra)
LIGHTUSD_LEGACY_BRIDGE_NS(value)
}  // namespace lightusd

#undef LIGHTUSD_LEGACY_BRIDGE_NS

#endif  // __cplusplus

#endif  // LIGHTUSD_LEGACY_NAMESPACE_HH_
