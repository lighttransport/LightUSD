// SPDX-License-Identifier: Apache-2.0
//
// lusdquicklook — the loading worker.
//
// Runs ::lightusd::StageSession + tydra streaming conversion on a background
// thread and publishes results through a bounded, byte-accounted queue. Nothing
// here touches the UI, the window or any GPU state.
//
// Cancellation is the backbone: selecting a different file sets `cancel` and the
// UI thread carries on immediately. The worker observes the flag from every
// progress/preview callback and at each geometry boundary, so an abandoned load
// stops within one mesh rather than at the end of the file.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "options.hh"
#include "ql_scene.hh"

namespace lusdql {

// ---------------------------------------------------------------------------
// Cross-thread control token
// ---------------------------------------------------------------------------

enum class LoadPhase : int {
  Idle = 0,
  Open,      // reading the root layer
  Compose,   // resolving composition arcs
  Extract,   // tydra -> QlScene
  Textures,  // decoding + downsampling
  Done,
  Failed,
  Cancelled,
};

struct LoadControl {
  std::atomic<bool> cancel{false};
  std::atomic<LoadPhase> phase{LoadPhase::Idle};
  std::atomic<int> phase_permille{0};
  std::atomic<uint32_t> meshes_done{0};
  std::atomic<uint32_t> meshes_total{0};
  std::atomic<uint64_t> triangles_done{0};

  // Advisory ceilings enforced inside the worker.
  uint64_t max_stage_bytes = 0;
  uint64_t max_geometry_bytes = 0;
  uint64_t max_texture_bytes = 0;
  uint64_t max_triangles = 20ull << 20;
  uint32_t max_textures = 16;

  void Reset() {
    cancel.store(false);
    phase.store(LoadPhase::Idle);
    phase_permille.store(0);
    meshes_done.store(0);
    meshes_total.store(0);
    triangles_done.store(0);
  }
};

const char* LoadPhaseName(LoadPhase phase);

// ---------------------------------------------------------------------------
// Worker -> UI events
// ---------------------------------------------------------------------------

struct LoadEvent {
  enum class Kind : uint8_t {
    Progress,
    Bounds,     // early camera framing, before any geometry arrives
    Resources,  // materials + textures + lights + cameras
    Mesh,
    Complete,
    Failed,
  };

  Kind kind = Kind::Progress;

  // Progress
  LoadPhase phase = LoadPhase::Idle;
  int permille = 0;
  std::string message;

  // Bounds
  QlAabb bounds;
  bool y_up = true;

  // Resources
  std::vector<QlMaterial> materials;
  std::vector<QlTexture> textures;
  std::vector<QlLight> lights;
  std::vector<QlCameraDesc> cameras;
  // Environment: index into `textures` plus its prefiltered chain, or -1.
  int env_texture = -1;
  int env_prefiltered[QlScene::kEnvPrefilterLevels] = {-1, -1, -1, -1};
  float env_rotation = 0.0f;
  float env_intensity = 1.0f;

  // Mesh
  QlMesh mesh;

  // Complete / Failed
  QlSceneStats stats;
  QlDegradation degraded;
  std::string error;

  LoadEvent() = default;
  LoadEvent(LoadEvent&&) = default;
  LoadEvent& operator=(LoadEvent&&) = default;
  LoadEvent(const LoadEvent&) = delete;
  LoadEvent& operator=(const LoadEvent&) = delete;

  uint64_t byte_size() const;
};

// Bounded producer/consumer queue. The byte bound is what keeps a huge scene
// from piling up in the queue faster than the UI thread drains it: the worker
// blocks in Push until there is room, which throttles conversion to the
// consumer's rate for free.
class LoadStream {
 public:
  explicit LoadStream(uint64_t max_bytes) : max_bytes_(max_bytes) {}

  // Blocks while the queue is full. Returns false once cancelled.
  bool Push(LoadEvent&& ev);
  // Never blocks. Returns false when the queue is empty.
  bool TryPop(LoadEvent* out);

  void Cancel();
  bool cancelled() const { return cancelled_.load(); }
  uint64_t queued_bytes() const;

 private:
  mutable std::mutex mu_;
  std::condition_variable ready_;
  std::condition_variable space_;
  std::deque<LoadEvent> q_;
  uint64_t queued_bytes_ = 0;
  uint64_t max_bytes_;
  std::atomic<bool> cancelled_{false};
};

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

class Loader {
 public:
  Loader() = default;
  ~Loader();

  Loader(const Loader&) = delete;
  Loader& operator=(const Loader&) = delete;

  // Cancels any in-flight load (without joining on the caller's thread) and
  // starts a new one. Safe to call from the UI thread every keystroke.
  void Start(const std::string& path, const Options& opts,
             const PreviewBudget& budget);

  // Signals cancellation. The worker winds down on its own; the thread is
  // joined by the next Start() or by the destructor.
  void Cancel();

  bool running() const { return running_.load(); }
  bool control_valid() const { return control_ != nullptr; }
  LoadControl& control() { return *control_; }
  LoadStream* stream() { return stream_.get(); }

  const std::string& path() const { return path_; }

 private:
  void Join();

  std::thread thread_;
  std::shared_ptr<LoadControl> control_;
  std::shared_ptr<LoadStream> stream_;
  std::atomic<bool> running_{false};
  std::string path_;
};

// The worker body. Exposed so the headless path can run it synchronously on the
// calling thread without spawning anything.
void RunLoad(const std::string& path, const Options& opts,
             const PreviewBudget& budget, std::shared_ptr<LoadControl> ctrl,
             std::shared_ptr<LoadStream> stream);

}  // namespace lusdql
