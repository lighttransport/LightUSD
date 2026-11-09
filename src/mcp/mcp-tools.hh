// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Tool handlers of the next MCP server. Each fills `result` (the tools/call
// result object) or returns false with `err`.
#pragma once

#include <string>

#include "mcp-context.hh"

namespace lightusd {
namespace mcp {

#define LIGHTUSD_MCP_TOOL(fn) \
  bool fn(Context& ctx, const json& args, json& result, std::string& err)

// Stage, prims and attributes (mcp-tools-stage.cc)
LIGHTUSD_MCP_TOOL(GetVersion);
LIGHTUSD_MCP_TOOL(StageNew);
LIGHTUSD_MCP_TOOL(StageLoad);
LIGHTUSD_MCP_TOOL(StageLoadData);
LIGHTUSD_MCP_TOOL(StageExport);
LIGHTUSD_MCP_TOOL(StageToString);
LIGHTUSD_MCP_TOOL(StageInfo);
LIGHTUSD_MCP_TOOL(PrimList);
LIGHTUSD_MCP_TOOL(PrimGet);
LIGHTUSD_MCP_TOOL(PrimCreate);
LIGHTUSD_MCP_TOOL(PrimRemove);
LIGHTUSD_MCP_TOOL(PrimRename);
LIGHTUSD_MCP_TOOL(PrimGetMetadata);
LIGHTUSD_MCP_TOOL(AttrList);
LIGHTUSD_MCP_TOOL(AttrGet);
LIGHTUSD_MCP_TOOL(AttrSet);
LIGHTUSD_MCP_TOOL(AttrBlock);
LIGHTUSD_MCP_TOOL(AttrConnections);

// Composition arcs and variants (mcp-tools-composition.cc)
LIGHTUSD_MCP_TOOL(ReferenceAdd);
LIGHTUSD_MCP_TOOL(ReferenceList);
LIGHTUSD_MCP_TOOL(ReferenceClear);
LIGHTUSD_MCP_TOOL(PayloadAdd);
LIGHTUSD_MCP_TOOL(PayloadList);
LIGHTUSD_MCP_TOOL(InheritAdd);
LIGHTUSD_MCP_TOOL(SpecializeAdd);
LIGHTUSD_MCP_TOOL(VariantListSets);
LIGHTUSD_MCP_TOOL(VariantGetSelection);
LIGHTUSD_MCP_TOOL(VariantSetSelection);
LIGHTUSD_MCP_TOOL(VariantDefine);

// Query, schemas and validation (mcp-tools-query.cc)
LIGHTUSD_MCP_TOOL(QueryPrimsByType);
LIGHTUSD_MCP_TOOL(SchemaListTypes);
LIGHTUSD_MCP_TOOL(SchemaGetType);
LIGHTUSD_MCP_TOOL(Search);
LIGHTUSD_MCP_TOOL(UsdValidate);

// USDZ packaging (mcp-tools-usdz.cc)
LIGHTUSD_MCP_TOOL(USDZConvert);
LIGHTUSD_MCP_TOOL(USDZPack);

// Diff (mcp-tools-diff.cc)
LIGHTUSD_MCP_TOOL(DiffOpen);
LIGHTUSD_MCP_TOOL(DiffSummary);
LIGHTUSD_MCP_TOOL(DiffPaths);
LIGHTUSD_MCP_TOOL(DiffPrim);
LIGHTUSD_MCP_TOOL(DiffTree);
LIGHTUSD_MCP_TOOL(DiffText);
LIGHTUSD_MCP_TOOL(DiffJson);

// Layers, assets, screenshots and selection (mcp-tools-assets.cc)
LIGHTUSD_MCP_TOOL(GetAllUSDDescriptions);
LIGHTUSD_MCP_TOOL(GetUSDDescription);
LIGHTUSD_MCP_TOOL(LoadUSDLayerFromFile);
LIGHTUSD_MCP_TOOL(LoadUSDLayerFromData);
LIGHTUSD_MCP_TOOL(ToUSDA);
LIGHTUSD_MCP_TOOL(ListPrimSpecs);
LIGHTUSD_MCP_TOOL(DebugPrimSpecDump);
LIGHTUSD_MCP_TOOL(StoreAsset);
LIGHTUSD_MCP_TOOL(ReadAsset);
LIGHTUSD_MCP_TOOL(ReadAssetPreview);
LIGHTUSD_MCP_TOOL(GetAllAssetDescriptions);
LIGHTUSD_MCP_TOOL(GetAssetDescription);
LIGHTUSD_MCP_TOOL(SelectAssets);
LIGHTUSD_MCP_TOOL(GetSelectedAssets);
LIGHTUSD_MCP_TOOL(SaveScreenshot);
LIGHTUSD_MCP_TOOL(ListScreenshots);
LIGHTUSD_MCP_TOOL(ReadScreenshot);

#undef LIGHTUSD_MCP_TOOL

}  // namespace mcp
}  // namespace lightusd
