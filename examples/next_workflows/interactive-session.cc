// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include "tydra/next/render-session.hh"
#include <iostream>

namespace render = lightusd::tydra::next;
// A headless consumer: real renderers stage resource uploads until EndUpdate.
class LoggingSink final : public render::SceneUpdateSink {
 public:
  bool BeginUpdate(uint64_t base, uint64_t next, bool full) override {
    pending = next;
    std::cout << "begin " << base << " -> " << next << " full=" << full << '\n';
    return true;
  }
  bool UpsertMesh(render::RenderId id, const render::RenderMesh&) override {
    std::cout << "mesh " << id << '\n'; return true;
  }
  bool Remove(const render::RemovedRenderResource& r) override {
    std::cout << "remove " << r.id << '\n'; return true;
  }
  bool EndUpdate() override { revision = pending; return true; }
  void AbortUpdate() override { pending = revision; }
  uint64_t revision = 0, pending = 0;
};
int main(int argc, char** argv) {
  namespace usd = lightusd::next;
  if (argc != 2) { std::cerr << "Usage: interactive_session ROOT.usda\n"; return 2; }
  bool cancel = false;
  usd::StageSessionOptions options;
  options.composition.load_payloads = false;
  options.progress_callback = [&](const usd::ProgressEvent&) { return !cancel; };
  usd::StageSession session;
  const auto opened = session.OpenFile(argv[1], options);
  if (!opened) { std::cerr << opened.error << '\n'; return 1; }
  const auto retained = session.GetSnapshot();
  render::RenderSession renderer;
  LoggingSink sink;
  if (!renderer.Initialize(retained, &sink)) return 1;
  auto apply = [&](const usd::StageEditResult& edit) {
    if (!edit) { std::cerr << edit.error << '\n'; return false; }
    const auto result = renderer.Apply(edit.snapshot, edit.changes, &sink);
    std::cout << "converted=" << result.converted_resource_count << " upserts=" << result.upsert_count << '\n';
    return bool(result);
  };
  if (!apply(session.SetVariantSelection(usd::Path("/World"), "display", "high"))) return 1;
  const auto deferred = session.GetDeferredPayloadPaths();
  for (const auto& path : deferred) {
    if (!apply(session.LoadPayload(path)) || !apply(session.UnloadPayload(path))) return 1;
  }
  // Reload reads the source again; it does not write or mutate the input file.
  if (!apply(session.ReloadLayer(session.GetRootIdentifier()))) return 1;
  const auto before_cancel = session.GetSnapshot();
  cancel = true;
  const auto cancelled = session.Rebuild();
  if (cancelled || session.GetSnapshot().revision != before_cancel.revision) return 1;
  if (!retained->GetPrimAtPath("/World")) return 1;
  std::cout << "retained revision=" << retained.revision << "; cancellation preserved current revision\n";
  return 0;
}
