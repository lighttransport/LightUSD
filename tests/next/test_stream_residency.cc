// SPDX-License-Identifier: Apache-2.0
#include "next/crate/crate-data-source.hh"
#include "next/crate/crate-writer.hh"
#include "next/stage/stage.hh"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace lightusd::next;
static int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
constexpr size_t kWindow = size_t(4) << 20;

// Observable source lifetime/advice, without relying on OS page-cache policy.
// Different wrappers have different source identities but identical payloads.
struct ObservedSource final : LazyArraySource {
  std::shared_ptr<CrateDataSource> backing;
  bool mapped;
  mutable std::atomic<size_t> pieces{0}, advice{0};
  mutable std::atomic<bool> invalid{false};
  ObservedSource(std::shared_ptr<CrateDataSource> b, bool m)
      : backing(std::move(b)), mapped(m) {}
  bool MaterializeArray(const LazyArrayRef& r, Value* out) const override {
    return backing->MaterializeArray(r, out);
  }
  const uint8_t* base() const override { return backing->base(); }
  size_t size() const override { return backing->size(); }
  CrateVersion version() const override { return backing->version(); }
  bool is_mmapped() const override { return mapped; }
  bool can_borrow() const override { return true; }
  void DiscardRange(uint64_t offset, uint64_t length) const override {
    ++advice;
    if (offset < 16 || offset > size() || length > size() - offset)
      invalid = true;
    if (length && length <= kWindow) ++pieces;
  }
};

static void Test(size_t payload_bytes) {
  const uint64_t count = payload_bytes / sizeof(float);
  std::string raw(24 + payload_bytes, '\0');
  std::memcpy(&raw[16], &count, sizeof(count));
  for (size_t i = 0; i < count; ++i) {
    const float value = float(i % 7919) * 0.125f;
    std::memcpy(&raw[24 + i * sizeof(float)], &value, sizeof(value));
  }
  auto backing = CrateDataSource::Adopt(std::move(raw), CrateVersion{0, 8, 0});
  LazyArrayRef base;
  CHECK(ProbeArrayBlock(backing, ValueRep::Make(CrateTypeId::Float, 16, true),
                         size_t(count), &base));
  if (!base.source) return;
  std::vector<uint8_t> expected;
  for (int mode : {0, 1, 2, 3, 4}) {
    const bool mapped = mode != 0;
    const bool windowed = mapped && mode < 3;
    auto a = std::make_shared<ObservedSource>(backing, mapped);
    auto b = std::make_shared<ObservedSource>(backing, mapped);
    auto owned = std::make_shared<ObservedSource>(backing, false);
    LazyArrayRef ra = base, rb = base, ro = base;
    ra.source = a; rb.source = b; ro.source = owned;
    StageBuilder builder;
    auto& layer = builder.GetLayerBuilder();
    layer.begin_prim("P", "Scope");
    layer.add_property("a", Value::MakeLazyArray(ra));
    layer.add_property("b", Value::MakeLazyArray(rb));
    layer.add_property("c", Value::MakeLazyArray(ra));
    // Mixed backing within ONE write proves that streaming XXH3 produces
    // exactly the owned source's key, rather than two internally consistent
    // but incompatible digest schemes that would emit this payload twice.
    layer.add_property("d", Value::MakeLazyArray(ro));
    layer.end_prim();
    // Force the worker/commit pipeline as well as the serial encoder.
    for (int i = 0; i < 9000; ++i) {
      layer.begin_prim("F" + std::to_string(i), "Scope");
      layer.end_prim();
    }
    layer.finalize();
    Stage stage = builder.Build();
    CrateWriteOptions options;
    options.num_threads = mode == 2 || mode == 4 ? 4 : 1;
    options.spill_columns_bytes = mode < 3 ? uint64_t(64) << 20 : 0;
    if (!mapped) {
      CHECK(CrateWriter(options).WriteToMemory(expected, stage).success);
      CHECK(a->advice == 0 && b->advice == 0);
    }
    std::vector<uint8_t> actual;
    bool wide_write = false, first_payload = true;
    size_t payload_calls = 0;
    CrateWriteSink sink = [&](const uint8_t* bytes, size_t n) {
      if (n >= size_t(1) << 20) {
        if (windowed && first_payload) CHECK(a->pieces > 0);
        first_payload = false;
        ++payload_calls;
        if (n > kWindow) wide_write = true;
      }
      actual.insert(actual.end(), bytes, bytes + n);
      return true;
    };
    CrateWritePatch patch = [&](uint64_t at, const uint8_t* bytes, size_t n) {
      if (at > actual.size() || n > actual.size() - size_t(at)) return false;
      std::memcpy(actual.data() + size_t(at), bytes, n);
      return true;
    };
    CHECK(CrateWriter(options).WriteLayerToSeekableSink(sink, patch,
            *stage.GetRootLayer()).success);
    CHECK(actual == expected);
    CHECK(owned->advice == 0);
    if (windowed) {
      CHECK(!wide_write && payload_calls >= payload_bytes / kWindow);
      // b deduplicates against a, so no b payload is written. Hashing b must
      // nevertheless release its mapped pages, not leave them resident.
      CHECK(a->pieces > 0 && b->pieces > 0);
      CHECK(!a->invalid && !b->invalid);
      auto value = stage.GetPrimAtPath("/P").GetPrimSpec()->property_value("a");
      CHECK(value && value->is_lazy());
      CHECK(value && value->materialized_copy().array_size() == count);
      size_t calls = 0;
      CrateWriteSink fail = [&](const uint8_t*, size_t n) {
        return n < (size_t(1) << 20) || ++calls < 2;
      };
      CrateWritePatch ignore = [](uint64_t, const uint8_t*, size_t) { return true; };
      CHECK(!CrateWriter(options).WriteLayerToSeekableSink(fail, ignore,
              *stage.GetRootLayer()).success);
      CHECK(calls >= 2);
    } else if (mapped) {
      CHECK(wide_write && a->pieces == 0 && b->pieces == 0);
    } else CHECK(a->advice == 0 && b->advice == 0);
  }
}
int main() {
  Test(size_t(31) << 20);  // one-shot XXH3 and its streaming equivalent
  Test(size_t(32) << 20);  // existing composite digest threshold (+count header)
  std::printf("stream residency failures: %d\n", failures);
  return failures ? 1 : 0;
}
