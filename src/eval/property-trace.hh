// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "attribute-eval.hh"
#include "../pcp/cache.hh"

namespace lightusd { namespace next {
struct PropertyResolutionTrace {
  Path prim_path;
  std::string property;
  TimeQuery time;
  EvalResult resolved;
  std::vector<pcp::PropertyOpinion> opinions;
  // Composed connection targets in traversal order. The first element is the
  // target authored on prim_path.property; the final element owns the value.
  std::vector<std::string> connection_chain;
  bool complete = true;
  std::string warning, error;
};
PropertyResolutionTrace TraceProperty(pcp::Cache& cache, const Path& prim_path,
    const std::string& property, const EvalOptions& options = {},
    size_t max_opinions = 4096);
std::string PropertyResolutionTraceToJSON(const PropertyResolutionTrace& trace);
} }
