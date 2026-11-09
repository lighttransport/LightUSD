// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
// Simple USD Layer edit commands and linked-list of USD Layer history

#include <string>
#include <memory>
#include <vector>
#include <deque>

#include "core/prim.hh"
#include "../layer.hh"

namespace lightusd {
namespace tydra {

enum class EditCommand
{
  Create,
  Modify,
  Delete,
};

enum class EditOp
{
  NewLayer,
  LoadLayer,
  SaveLayer,
  XformTranslation,
  XformRotation,
  XformScale,
  XformTransform,
  XformPivotTranslation,
  XformPivotRotation,
  XformPivotScale,
};


// Only support the move operation for the efficiency.
struct EditHistory
{
  EditCommand cmd;
  EditOp op;
  std::string arg;
  uint64_t id;

  lightusd::Layer layer;
};

// We only support queues for now(no history graph)
class HistoryQueue
{
 public:
  constexpr static uint32_t kMaxHistories = 1024ul * 1024ul;

  bool push(EditHistory &&hist);

  bool undo();
  bool redo();

  // Serialize the history queue to a JSON string
  // `dump_layer` : Include JSON dump of `Layer` in each EditHistory.
  std::string to_json_string(bool dump_layer = false) const;

 private:
  std::deque<EditHistory> history_queues;
  std::deque<EditHistory> undo_queues;

};

} // namespace tydra
} // namespace lightusd
