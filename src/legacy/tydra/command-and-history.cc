#include <string>

#include "command-and-history.hh"

#include "json-util.hh"
#include "core/minijson.hh"


namespace lightusd { namespace legacy {
namespace tydra {

bool HistoryQueue::push(EditHistory &&hist) {
  if (history_queues.size() > kMaxHistories) {
    return false;
  }

  history_queues.emplace_back(std::move(hist));

  return true;
}

bool HistoryQueue::undo() {
  if (history_queues.empty()) {
    return false;
  }

  // reuse maxHistories for undo buffer.
  if (undo_queues.size() > kMaxHistories) {
    return false;
  }

  undo_queues.emplace_back(history_queues.back());
  history_queues.pop_back();

  return true;
}

bool HistoryQueue::redo() {
  if (undo_queues.empty()) {
    return false;
  }

  history_queues.emplace_back(undo_queues.back());
  undo_queues.pop_back();

  return true;
}

std::string HistoryQueue::to_json_string(bool dump_layer) const {
  json j;
  j["history"] = json::array();

  for (const auto &hist : history_queues) {
    json hist_json;
    hist_json["cmd"] = static_cast<int>(hist.cmd);
    hist_json["op"] = static_cast<int>(hist.op);
    hist_json["arg"] = hist.arg;
    hist_json["id"] = hist.id;

    if (dump_layer) {
      // TODO: Serialize layer if needed
      // hist_json["layer"] = ...; // Implement layer serialization if necessary
    }

    j["history"].push_back(hist_json);
  }

  return j.dump();
}

} // namespace tydra
}  // namespace legacy
} // namespace lightusd
