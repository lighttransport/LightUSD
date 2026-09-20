// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Immutable Stage publication and revision-to-revision change records.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stage.hh"

namespace lightusd {
namespace next {

struct StageSnapshot {
  uint64_t revision = 0;
  std::shared_ptr<const Stage> stage;

  explicit operator bool() const { return static_cast<bool>(stage); }
  const Stage* operator->() const { return stage.get(); }
  const Stage& operator*() const { return *stage; }
};

enum class StageChangeFlag : uint32_t {
  None = 0,
  Resync = 1u << 0,
  Topology = 1u << 1,
  Transform = 1u << 2,
  Primvar = 1u << 3,
  Material = 1u << 4,
  Texture = 1u << 5,
  Light = 1u << 6,
  Camera = 1u << 7,
  Animation = 1u << 8,
  Visibility = 1u << 9,
  Metadata = 1u << 10,
};

inline StageChangeFlag operator|(StageChangeFlag a, StageChangeFlag b) {
  return static_cast<StageChangeFlag>(static_cast<uint32_t>(a) |
                                      static_cast<uint32_t>(b));
}
inline StageChangeFlag& operator|=(StageChangeFlag& a, StageChangeFlag b) {
  a = a | b;
  return a;
}
inline bool HasStageChange(StageChangeFlag value, StageChangeFlag test) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(test)) != 0;
}

struct PrimChange {
  Path path;
  StageChangeFlag flags = StageChangeFlag::None;
  std::vector<std::string> properties;
};

struct StageChangeSet {
  uint64_t base_revision = 0;
  uint64_t new_revision = 0;
  bool full_resync = false;
  bool stage_metadata_changed = false;
  std::vector<PrimChange> prims;

  bool empty() const {
    return !full_resync && !stage_metadata_changed && prims.empty();
  }
};

/// Append a consecutive change record into a revision-range aggregate. Prim
/// flags are ORed and property names are deduplicated. A revision gap marks the
/// aggregate as a full resync and returns false.
bool AppendStageChangeSet(const StageChangeSet& incoming,
                          StageChangeSet* aggregate);

}  // namespace next
}  // namespace lightusd
