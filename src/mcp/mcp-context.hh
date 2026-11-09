// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Per-session state of the next MCP server. Stages and loaded layers are
// public C API stage handles (each owned exclusively by the session); diff
// sessions keep next layers directly.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp-json.hh"
#include "lightusd-c.h"
#include "next/diff/layer-diff.hh"
#include "next/layer/layer.hh"

namespace lightusd {
namespace mcp {

// Owning C stage reference.
struct StageRef {
  StageRef() = default;
  explicit StageRef(lightusd_stage* s) : stage(s) {}
  StageRef(const StageRef&) = delete;
  StageRef& operator=(const StageRef&) = delete;
  StageRef(StageRef&& o) noexcept : stage(o.stage) { o.stage = nullptr; }
  StageRef& operator=(StageRef&& o) noexcept {
    if (this != &o) {
      reset();
      stage = o.stage;
      o.stage = nullptr;
    }
    return *this;
  }
  ~StageRef() { reset(); }
  void reset(lightusd_stage* s = nullptr) {
    if (stage) lightusd_stage_destroy(stage);
    stage = s;
  }
  explicit operator bool() const { return stage != nullptr; }
  lightusd_stage* stage = nullptr;
};

struct Image {
  std::string name;
  std::string mimeType;
  std::string data;  // base64
};

struct AssetSelection {
  std::string asset_name;
  int instance_id = 0;
  std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
  std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
  std::array<float, 3> rotation = {0.0f, 0.0f, 0.0f};  // Euler degrees
};

struct Asset {
  std::string name;
  std::string data;  // base64
  std::string description;
  Image preview;
  std::string uuid;
  std::array<float, 3> pivot_position = {0.0f, 0.0f, 0.0f};
  std::array<float, 3> bmin = {-1.0f, -1.0f, -1.0f};
  std::array<float, 3> bmax = {1.0f, 1.0f, 1.0f};
};

struct LayerEntry {
  std::string uri;
  std::string name;
  std::string description;
  StageRef stage;  // uncomposed root layer
};

struct Screenshot {
  std::string uuid;
  std::string mimeType;
  std::string data;  // base64
};

struct DiffSession {
  std::string left_name{"left"};
  std::string right_name{"right"};
  next::Layer left;
  next::Layer right;
  next::DiffOptions opts;
  std::unordered_map<std::string, next::PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, next::PropDiff> propDiffs;
  next::LayerMetaDiff layerMetaDiff;
};

struct Context {
  StageRef stage;
  std::unique_ptr<DiffSession> diff;
  std::map<std::string, LayerEntry> layers;  // key: uuid
  std::map<std::string, Asset> assets;       // key: name
  std::vector<AssetSelection> selected_assets;
  std::map<std::string, Screenshot> screenshots;  // key: name
  std::string session_id;
};

using ToolFn = bool (*)(Context& ctx, const json& args, json& result,
                        std::string& err);

}  // namespace mcp
}  // namespace lightusd
