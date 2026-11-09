// SPDX-License-Identifier: Apache-2.0
#include "change-set.hh"

#include <algorithm>

namespace lightusd {
namespace next {

bool AppendStageChangeSet(const StageChangeSet& incoming,
                          StageChangeSet* aggregate) {
  if (!aggregate) return false;
  bool continuous = true;
  if (aggregate->new_revision == aggregate->base_revision &&
      aggregate->prims.empty() && !aggregate->full_resync &&
      !aggregate->stage_metadata_changed) {
    aggregate->base_revision = incoming.base_revision;
  } else if (aggregate->new_revision != incoming.base_revision) {
    continuous = false;
    aggregate->full_resync = true;
  }
  aggregate->new_revision = incoming.new_revision;
  aggregate->full_resync = aggregate->full_resync || incoming.full_resync;
  aggregate->stage_metadata_changed =
      aggregate->stage_metadata_changed || incoming.stage_metadata_changed;
  for (const PrimChange& change : incoming.prims) {
    auto found = std::find_if(
        aggregate->prims.begin(), aggregate->prims.end(),
        [&](const PrimChange& existing) { return existing.path == change.path; });
    if (found == aggregate->prims.end()) {
      aggregate->prims.push_back(change);
      continue;
    }
    found->flags |= change.flags;
    for (const std::string& property : change.properties) {
      if (std::find(found->properties.begin(), found->properties.end(),
                    property) == found->properties.end()) {
        found->properties.push_back(property);
      }
    }
  }
  return continuous;
}

}  // namespace next
}  // namespace lightusd
