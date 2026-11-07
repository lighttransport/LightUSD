#include "loader.hh"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ld3d12 {
namespace {

// Phases that report real progress, in order: open, compose, convert, textures.
float OverallProgress(int phase, float fraction) {
  if (phase >= LMV_PHASE_DONE) return 1.0f;
  if (phase < LMV_PHASE_OPEN) return 0.0f;
  const float index = static_cast<float>(phase - LMV_PHASE_OPEN);
  return std::min(1.0f, (index + std::min(1.0f, std::max(0.0f, fraction))) / 4.0f);
}

}  // namespace

AsyncLoader::~AsyncLoader() {
  pending_.clear();
  pending_variants_.clear();
  if (state_) state_->cancel = true;
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    stop_ = true;
  }
  job_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (state_ && state_->result.scene) lmv_scene_free(state_->result.scene);
}

void AsyncLoader::Run(State* state, Hook* hook, const std::string& path,
                      const VariantOverrides& variants, int delay_ms) {
  // Test delay in small steps so cancellation stays responsive.
  for (int waited = 0; waited < delay_ms && !state->cancel; waited += 20) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->progress.phase = LMV_PHASE_OPEN;
      state->progress.fraction =
          delay_ms > 0 ? static_cast<float>(waited) / static_cast<float>(delay_ms)
                       : 0.0f;
      state->progress.message = "waiting (test delay)";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  lmv_load_options options;
  lmv_load_options_init(&options);
  std::vector<const char*> variant_sets, variant_names;  // borrowed for lmv_load
  for (const auto& v : variants) {
    variant_sets.push_back(v.first.c_str());
    variant_names.push_back(v.second.c_str());
  }
  options.extra_variant_sets = variant_sets.data();
  options.extra_variant_names = variant_names.data();
  options.extra_variant_count = static_cast<uint32_t>(variants.size());
  options.progress_user = hook;
  options.progress = [](void* user, const lmv_progress* p) -> int {
    auto* h = static_cast<Hook*>(user);
    // Inert once the load is over (the scene calls back during set_time).
    if (!h->active) return 1;
    State* st = h->state;
    {
      std::lock_guard<std::mutex> lock(st->mutex);
      st->progress.phase = p->phase;
      st->progress.fraction = p->fraction;
      st->progress.message = p->message ? p->message : "";
    }
    return st->cancel ? 0 : 1;  // returning 0 cancels the load
  };

  LoadResult result;
  result.path = path;
  result.variants = variants;
  if (state->cancel) {
    result.status = LMV_ERR_CANCELLED;
  } else {
    char err[512] = {};
    result.status = lmv_load(path.c_str(), &options, &result.scene, err, sizeof(err));
    result.error = err;
    if (result.status != LMV_OK && result.scene) {
      lmv_scene_free(result.scene);
      result.scene = nullptr;
    }
  }
  hook->active = false;  // no further callbacks may touch `state`
  result.cancelled = state->cancel || result.status == LMV_ERR_CANCELLED;
  if (result.cancelled && result.scene) {
    lmv_scene_free(result.scene);
    result.scene = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->result = std::move(result);
  }
  state->done = true;
}

void AsyncLoader::Launch(const std::string& path,
                         const VariantOverrides& variants) {
  state_ = std::make_unique<State>();
  state_->start = std::chrono::steady_clock::now();
  state_->progress.active = true;
  state_->progress.path = path;
  state_->progress.phase = LMV_PHASE_OPEN;
  hooks_.push_back(std::make_unique<Hook>());
  hooks_.back()->state = state_.get();
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    job_ = Job{state_.get(), hooks_.back().get(), path, variants, delay_ms_};
    has_job_ = true;
  }
  job_cv_.notify_one();
  if (!thread_.joinable()) thread_ = std::thread(&AsyncLoader::Worker, this);
}

void AsyncLoader::Worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(job_mutex_);
      job_cv_.wait(lock, [this] { return stop_ || has_job_; });
      if (!has_job_) return;  // stop requested and nothing left to run
      job = job_;
      has_job_ = false;
    }
    Run(job.state, job.hook, job.path, job.variants, job.delay_ms);
  }
}

void AsyncLoader::Request(const std::string& path,
                          const VariantOverrides& variants) {
  if (path.empty()) return;
  if (state_) {  // supersede the running load
    state_->cancel = true;
    pending_ = path;
    pending_variants_ = variants;
    return;
  }
  Launch(path, variants);
}

void AsyncLoader::Cancel() {
  pending_.clear();
  pending_variants_.clear();
  if (state_) state_->cancel = true;
}

bool AsyncLoader::Poll(LoadResult* out) {
  if (!state_ || !state_->done) return false;
  *out = std::move(state_->result);
  state_.reset();
  if (!pending_.empty()) {
    const std::string next = std::move(pending_);
    const VariantOverrides next_variants = std::move(pending_variants_);
    pending_.clear();
    pending_variants_.clear();
    Launch(next, next_variants);
  }
  return true;
}

LoadProgress AsyncLoader::Progress() const {
  LoadProgress p;
  if (!state_) return p;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    p = state_->progress;
  }
  p.active = true;
  p.cancelling = state_->cancel;
  p.overall = OverallProgress(p.phase, p.fraction);
  p.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            state_->start)
                  .count();
  return p;
}

}  // namespace ld3d12
