// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Unified Header
//
// This header provides the main entry point for the "next" architecture.
// Built by default from the root CMake project (or `cmake -S src`).
//
// Key differences from the original architecture:
// - Minimal template usage for faster compilation
// - Runtime type dispatch via TypeId enum
// - Small Buffer Optimization (SBO) for Value class
// - Unified PrimSpec (no separate Prim/PrimSpec trees)
// - Time sample value deduplication
// - O(1) property lookup via interned name IDs

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "core/security-policy.hh"
#include "execution.hh"
#include "operation-status.hh"
#include "resource-limits.hh"

// Core types
#include "types/type-id.hh"
#include "types/type-info.hh"
#include "types/value.hh"
#include "types/interpolation.hh"

// Persistent PCP composition/cache APIs.
#include "pcp/cache.hh"

// Prim types
#include "prim/path.hh"

// Layer and Stage
#include "layer/property-index.hh"
#include "layer/prim-spec.hh"
#include "layer/layer.hh"
#include "stage/stage.hh"
#include "stage/change-set.hh"

// Parsers
#include "parser/lexer.hh"
#include "parser/value-parser.hh"
#include "parser/parse-options.hh"

// Readers
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "reader/usdz-reader.hh"

// Writers
#include "writer/usda-writer.hh"
#include "writer/usdc-writer.hh"

// Evaluation
#include "eval/attribute-eval.hh"
#include "eval/value-clip.hh"

// Asset resolution
#include "resolver/asset-resolver.hh"

// Composition
#include "composition/composition.hh"

// Schema APIs
#include "schema/geom-mesh.hh"
#include "schema/schema-registry.hh"
#include "schema/geom-point-instancer.hh"
#include "schema/geom-xform.hh"
#include "schema/usd-lux.hh"
#include "schema/usd-geom-camera.hh"
#include "schema/usd-shade.hh"
#include "schema/usd-skel.hh"
#include "schema/usdPhysics.hh"
#include "schema/usd-ar.hh"
#include "schema/usd-media.hh"
#include "schema/usd-mtlx.hh"
#include "schema/usd-render.hh"
#include "schema/usd-semantics.hh"
#include "schema/usd-vol.hh"
#include "load-usd.hh"

namespace lightusd {

// Version info — keep in sync with src/c-api/lightusd-c.cc (lightusd_version_string) and web/{npm,js}/package.json
constexpr int version_major = 1;
constexpr int version_minor = 1;
constexpr int version_micro = 0;
constexpr const char* version_string = "1.1.0";


}  // namespace lightusd
