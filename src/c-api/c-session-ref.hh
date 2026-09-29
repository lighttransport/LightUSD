// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

#include "next/stage/change-set.hh"

struct lightusd_document_session;
struct lightusd_document_snapshot;

namespace lightusd_internal {
bool DocumentSessionIsOpen(const lightusd_document_session* session);
std::string_view DocumentSessionSourceDir(
    const lightusd_document_session* session);
std::string_view DocumentSnapshotSourceDir(
    const lightusd_document_snapshot* snapshot);
const lightusd::next::StageSnapshot& DocumentSnapshotStage(
    const lightusd_document_snapshot* snapshot);
const lightusd::next::StageChangeSet& DocumentSnapshotChanges(
    const lightusd_document_snapshot* snapshot);
}  // namespace lightusd_internal
