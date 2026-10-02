// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "next/validation/validation-context.hh"
#include "next/resolver/asset-resolver.hh"
#include "minijson.hh"
#include <functional>
namespace lusdchecker {
// Optional run-local I/O for embedders. Named asset reads (including dependencies and
// containers) use read; report receives the structured result without output.
// The resolver and callbacks must outlive the synchronous call.
struct Environment {
  lightusd::next::AssetResolver* resolver = nullptr;
  std::function<bool(const std::string&, size_t, std::string*, std::string*)> read;
  lightusd::minijson::Value* report = nullptr;
};
// Memory-only checker, using the same profiles and engine as the CLI. assets
// is a run-local resolver containing explicitly supplied memory assets. No
// filesystem or network reads are performed. Options are a strict JSON object.
lightusd::minijson::Value CheckMemory(const uint8_t* data, size_t size,
    const std::string& filename, const std::string& options_json,
    lightusd::next::AssetResolver* assets = nullptr);

// Copy the caller's registry into a run-local snapshot, load CLI manifests,
// then validate. The caller may reuse its registry after this returns.
int RunChecker(int argc, char** argv,
               const lightusd::next::ValidationRegistry& registry,
               const Environment* environment = nullptr);
}
