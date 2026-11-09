// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "lightusd-session-c.h"
#include "next/stage/stage-session.hh"

struct lightusd_document_snapshot {
  lightusd::next::StageSnapshot snapshot;
  lightusd::next::StageChangeSet changes;
  std::string source_dir;
  std::string source_filename;
  std::string warnings;
};

struct lightusd_document_session {
  lightusd::next::StageSession session;
  lightusd::next::StageSessionOptions options;
  std::string source_dir;
  lightusd_document_preview_fn preview_callback = nullptr;
  void* preview_userdata = nullptr;
  lightusd_status preview_status = LIGHTUSD_OK;
  lightusd_document_payload_policy_fn payload_policy = nullptr;
  lightusd_document_payload_selected_fn payload_selected = nullptr;
  void* payload_userdata = nullptr;
};
