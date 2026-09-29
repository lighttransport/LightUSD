// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Focused high-level USD load and write API for the next product.
#pragma once

#include <string>

#include "stage/stage-session.hh"

namespace lightusd {
namespace next {

/// Load a USD file (auto-detects format: USDA, USDC).
bool LoadUSD(const std::string& filename, Stage* stage,
             std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSD(const std::string& filename, Stage* stage,
             const LoadUSDOptions& options, std::string* warn = nullptr,
             std::string* err = nullptr);

/// Load a USD file and resolve composition arcs into the returned stage.
/// External dependencies resolve relative to `filename`; variants from
/// `comp_opts` override authored selections.
bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     std::string* warn = nullptr, std::string* err = nullptr,
                     const pcp::CompositionOptions* comp_opts = nullptr);

bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     const LoadUSDOptions& options,
                     std::string* warn = nullptr, std::string* err = nullptr,
                     const pcp::CompositionOptions* comp_opts = nullptr);

/// True when a plain single-layer load leaves composition arcs unresolved.
bool StageNeedsComposition(const Stage& stage);

/// Compose an already-loaded stage through the PCP engine.
bool ComposeLoadedStage(Stage* stage, AssetResolver& resolver,
                        const std::string& anchor_label,
                        const LoadUSDOptions& load_options,
                        std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts = nullptr,
                        pcp::CompositionReport* report = nullptr);

/// Compose a memory-rooted stage with callback-only asset resolution.
bool ComposeLoadedStage(Stage* stage, std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts = nullptr,
                        const std::string& anchor_label = "");

/// Load one USDA, USDC, or USDZ layer from memory without composition.
bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       const LoadUSDOptions& options,
                       std::string* warn = nullptr, std::string* err = nullptr);

/// Load from a buffer whose storage can be adopted by move.
bool LoadUSDFromMemoryOwned(std::string&& data, Stage* stage,
                            const LoadUSDOptions& options = {},
                            std::string* warn = nullptr,
                            std::string* err = nullptr);

bool LoadUSDA(const std::string& filename, Stage* stage,
              std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDA(const std::string& filename, Stage* stage,
              const LoadOptions& options, std::string* warn = nullptr,
              std::string* err = nullptr);

bool LoadUSDC(const std::string& filename, Stage* stage,
              std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDC(const std::string& filename, Stage* stage,
              const USDCLoadOptions& options, std::string* warn = nullptr,
              std::string* err = nullptr);

bool WriteUSDA(const Stage& stage, const std::string& filename,
               std::string* err = nullptr);

bool WriteUSDC(const Stage& stage, const std::string& filename,
               std::string* err = nullptr);

}  // namespace next
}  // namespace lightusd
