#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_set>

#include "core/prim.hh"
#include "../layer.hh"
#include "../stage.hh"
#include "../tiny-hashmap.hh"
#include "diff-and-compare.hh"

namespace lightusd {

namespace tydra {

// Forward declare JS engine state (defined in js-script.hh)
struct JSEngineState;

namespace mcp {

struct Image
{
  std::string name; // optional
  std::string mimeType; // 'image/jpeg' or 'image/png' for now
  std::string data; // based64 encoded image
};

struct AssetSelection
{
  std::string asset_name;

  // Instance and transform parameters
  int instance_id = 0; // Instance ID for the asset
  std::array<float, 3> position = {0.0f, 0.0f, 0.0f}; // x, y, z
  std::array<float, 3> scale = {1.0f, 1.0f, 1.0f}; // x, y, z
  std::array<float, 3> rotation = {0.0f, 0.0f, 0.0f}; // x, y, z Euler angles in degrees

};

// Generic Asset(USD, textures, etc.)
struct MCPAsset
{
  std::string name;
  std::string data; // base64 encoded asset data
  std::string description; // optional
  Image preview; // preview image of the asset(optional)
  std::string uuid;


  // Geometry and bounding box parameters
  std::array<float, 3> pivot_position = {0.0f, 0.0f, 0.0f}; // pivot point for rotation and scaling
  std::array<float, 3> bmin = {-1.0f, -1.0f, -1.0f}; // bounding box minimum
  std::array<float, 3> bmax = {1.0f, 1.0f, 1.0f}; // bounding box maximum
};

struct USDLayer
{
  std::string uri;
  std::string name;
  std::string description; // optional
  Layer layer;
};

struct Screenshot
{
  std::string uuid;
  std::string mimeType;
  std::string data; // base64 encoded image data.
};

// A computed diff between two layers, cached in the session so the LLM can
// query narrow slices (summary / paths / one prim) instead of re-diffing or
// ingesting the whole diff. `left`/`right` are retained (possibly flattened)
// so per-prim detail and JS drill-down can re-render values.
struct DiffSession
{
  std::string left_name{"left"};
  std::string right_name{"right"};
  Layer left;
  Layer right;
  tydra::DiffOptions opts;
  lightusd::HashMap<std::string, tydra::PrimSpecDiff> psDiffs;
  lightusd::HashMap<std::string, tydra::PropDiff> propDiffs;
  tydra::LayerMetaDiff layerMetaDiff;
};

struct Context
{
  // ---- Composed Stage (for scene graph queries) ----
  std::unique_ptr<Stage> stage;
  bool stage_loaded{false};

  // ---- Cached layer diff (for the diff_* tools + lightusd.diff.* in JS) ----
  std::unique_ptr<DiffSession> diff;

  // ---- Uncomposed Layers (for editing) ----
  // key = UUID
  lightusd::HashMap<std::string, USDLayer> layers;

  // ---- Viewer workflow assets ----
  // key = name
  lightusd::HashMap<std::string, MCPAsset> assets;

  std::vector<AssetSelection> selected_assets;

  // key = name
  lightusd::HashMap<std::string, Screenshot> screenshots;

  // ---- QuickJS engine (persistent per session) ----
  std::unique_ptr<JSEngineState> js_engine;

  // ---- Utility ----
  std::string session_id;
};

} // namespace mcp
} // namespace tydra
} // namespace lightusd

