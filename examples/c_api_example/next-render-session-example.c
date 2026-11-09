/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal consumer of the persistent render-session C ABI. */
#include <stdio.h>

#include "lightusd-c.h"
#include "lightusd-render-c.h"

struct EventCounts {
  unsigned begin;
  unsigned upsert;
  unsigned remove;
  unsigned end;
  unsigned abort;
};

static int on_event(void* userdata, const lightusd_render_event* event) {
  struct EventCounts* counts = (struct EventCounts*)userdata;
  if (!counts || !event) return 0;
  switch (event->type) {
    case LIGHTUSD_RENDER_EVENT_BEGIN: ++counts->begin; break;
    case LIGHTUSD_RENDER_EVENT_UPSERT: ++counts->upsert; break;
    case LIGHTUSD_RENDER_EVENT_REMOVE: ++counts->remove; break;
    case LIGHTUSD_RENDER_EVENT_END: ++counts->end; break;
    case LIGHTUSD_RENDER_EVENT_ABORT: ++counts->abort; break;
    default: return 0;
  }
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: next_render_session_example FILE.usda\n");
    return 2;
  }

  lightusd_stage* stage = NULL;
  lightusd_status status = lightusd_stage_load(argv[1], NULL, &stage);
  if (status != LIGHTUSD_OK) {
    fprintf(stderr, "Load failed: %s\n", lightusd_last_error());
    return 1;
  }

  struct EventCounts events = {0, 0, 0, 0, 0};
  lightusd_render_event_sink sink;
  lightusd_render_event_sink_init(&sink);
  sink.callback = on_event;
  sink.userdata = &events;

  lightusd_render_config config;
  lightusd_render_config_init(&config);
  lightusd_render_session* session = NULL;
  status = lightusd_render_session_create(stage, &config, &session);
  if (status != LIGHTUSD_OK) {
    fprintf(stderr, "Render session failed: %s\n", lightusd_last_error());
    lightusd_stage_destroy(stage);
    return 1;
  }
  status = lightusd_render_session_set_event_sink(session, &sink);
  if (status != LIGHTUSD_OK) {
    fprintf(stderr, "Event sink failed: %s\n", lightusd_last_error());
    lightusd_render_session_destroy(session);
    lightusd_stage_destroy(stage);
    return 1;
  }

  lightusd_render_scene* scene = NULL;
  lightusd_render_update_info update;
  lightusd_render_update_info_init(&update);
  status = lightusd_render_session_update(session, stage, &scene, &update);
  if (status != LIGHTUSD_OK) {
    fprintf(stderr, "Render update failed: %s\n", lightusd_last_error());
    lightusd_render_session_destroy(session);
    lightusd_stage_destroy(stage);
    return 1;
  }

  printf("revision=%llu full_resync=%u resources=%llu nodes=%zu meshes=%zu "
         "events=%u/%u/%u/%u/%u\n",
         (unsigned long long)update.revision,
         (unsigned)update.full_resync,
         (unsigned long long)update.converted_resource_count,
         lightusd_render_count(scene, LIGHTUSD_RENDER_NODE),
         lightusd_render_count(scene, LIGHTUSD_RENDER_MESH), events.begin,
         events.upsert, events.remove, events.end, events.abort);

  lightusd_render_scene_destroy(scene);
  lightusd_render_session_destroy(session);
  lightusd_stage_destroy(stage);
  return 0;
}
