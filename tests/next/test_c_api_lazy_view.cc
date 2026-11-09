// SPDX-License-Identifier: Apache-2.0
// Internal storage regression; the separate facade test uses installed headers.
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "c-api/c-internal.hh"
#include "crate/crate-data-source.hh"
#include "crate/lazy-array.hh"
#include "layer/layer.hh"
#include "types/value-view.hh"

namespace n = lightusd::next;
static int failures = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

struct Source final : n::LazyArraySource {
  std::vector<uint64_t> words;
  size_t offset;
  mutable unsigned decoded{0};
  explicit Source(size_t block = 8)
      : words((block + 8 + 24 + 7) / 8), offset(block) {
    const uint64_t count = 2;
    const float values[] = {1, 2, 3, 4, 5, 6};
    std::memcpy(reinterpret_cast<uint8_t*>(words.data()) + block, &count, 8);
    std::memcpy(reinterpret_cast<uint8_t*>(words.data()) + block + 8, values,
                24);
  }
  const uint8_t* base() const override {
    return reinterpret_cast<const uint8_t*>(words.data());
  }
  size_t size() const override { return words.size() * 8; }
  n::CrateVersion version() const override { return {0, 8, 0}; }
  bool is_mmapped() const override { return false; }
  bool can_borrow() const override { return true; }
  bool MaterializeArray(const n::LazyArrayRef& ref,
                        n::Value* out) const override {
    ++decoded;
    if (ref.value_type == n::TypeId::Half) {
      if (ref.element_count != 4 || ref.max_elements < 4) return false;
      uint16_t bits[4];
      std::memcpy(bits, base() + offset + 8, sizeof(bits));
      float values[4];
      for (size_t i = 0; i < 4; ++i) values[i] = lu_half_to_float(bits[i]);
      *out = n::Value::MakeArrayFromRaw(ref.value_type, values, 4);
      return !out->is_empty();
    }
    if (ref.element_count != 2 || ref.max_elements < 2) return false;
    *out = n::Value::MakeArrayFromRaw(ref.value_type, base() + offset + 8, 2);
    return !out->is_empty();
  }
};

static n::Value lazy(const std::shared_ptr<Source>& source,
                     bool compressed = false) {
  n::LazyArrayRef ref;
  ref.source = source;
  ref.block_offset = source->offset;
  ref.block_len = 32;
  ref.element_count = 2;
  ref.src_elem_stride = 12;
  ref.value_type = n::TypeId::Point3f;
  ref.crate_type = n::CrateTypeId::Vec3f;
  ref.is_compressed = compressed;
  return n::Value::MakeLazyArray(ref);
}

static void wrong_type_does_not_decode() {
  auto source = std::make_shared<Source>();
  n::Value value = lazy(source);
  CHECK(!value.as_token_array());
  CHECK(!value.as_int_array());
  n::ArrayScratch<int32_t> scratch;
  n::ArrayView<int32_t> view;
  CHECK(!n::GetIntArrayView(value, &scratch, &view));
  CHECK(source->decoded == 0);
  CHECK(value.is_lazy());
}

static void borrowed_pin_retains_source() {
  auto source = std::make_shared<Source>();
  std::weak_ptr<Source> weak = source;
  auto* value = new lightusd_value;
  value->v = lazy(source);
  lightusd_value_view view{};
  CHECK(lightusd_value_get_view(value, &view) == LIGHTUSD_OK);
  CHECK(source->decoded == 0);
  CHECK(view.data == source->base() + source->offset + 8);
  CHECK(view.count == 2 && view.components == 3 && view.nbytes == 24);
  lightusd_buffer* pin = nullptr;
  CHECK(lightusd_buffer_pin(view, &pin) == LIGHTUSD_OK);
  CHECK(source->decoded == 0);
  source.reset();
  lightusd_value_destroy(value);
  CHECK(!weak.expired());
  const lightusd_value_view held = lightusd_buffer_get_view(pin);
  const float expected[] = {1, 2, 3, 4, 5, 6};
  CHECK(held.data && held.nbytes == sizeof(expected));
  if (held.data) CHECK(std::memcmp(held.data, expected, sizeof(expected)) == 0);
  lightusd_buffer_destroy(pin);
  CHECK(weak.expired());
}

static void unsuitable_layout_materializes() {
  for (bool compressed : {false, true}) {
    auto source = std::make_shared<Source>(compressed ? 8 : 1);
    lightusd_value value;
    value.v = lazy(source, compressed);
    lightusd_value_view view{};
    CHECK(lightusd_value_get_view(&value, &view) == LIGHTUSD_OK);
    CHECK(source->decoded == 1);
    CHECK(view.data != source->base() + source->offset + 8);
    const float expected[] = {1, 2, 3, 4, 5, 6};
    CHECK(view.data && view.nbytes == sizeof(expected));
    if (view.data)
      CHECK(std::memcmp(view.data, expected, sizeof(expected)) == 0);
  }
}

static void decode_failure_is_not_an_empty_array() {
  auto source = std::make_shared<Source>();
  n::LazyArrayRef ref = *lazy(source).lazy_ref();
  ref.max_elements = 1;  // Two declared elements cannot pass the retained cap.
  lightusd_value value;
  value.v = n::Value::MakeLazyArray(ref);
  lightusd_value_view view{};
  CHECK(lightusd_value_get_view(&value, &view) == LIGHTUSD_ERR_PARSE);
  CHECK(view.data == nullptr && view.nbytes == 0 && view.count == 0);
  CHECK(source->decoded == 1);

  // A real in-bounds compressed block can still contain an invalid codec
  // stream. Probing its shape must not turn a later decode failure into an
  // apparently successful, nonzero-count view with no bytes.
  std::string bytes(25, '\0');
  bytes[8] = 2;
  bytes[16] = 1;
  bytes[24] = static_cast<char>(0xff);
  auto malformed = n::CrateDataSource::Adopt(std::move(bytes), {0, 8, 0});
  CHECK(n::ProbeArrayBlock(
      malformed, n::ValueRep::Make(n::CrateTypeId::Int, 8, true, false, true),
      16, &ref));
  CHECK(ref.block_len == 17);
  value.v = n::Value::MakeLazyArray(ref);
  CHECK(lightusd_value_get_view(&value, &view) == LIGHTUSD_ERR_PARSE);
  CHECK(view.data == nullptr && view.nbytes == 0 && view.count == 0);
}

static void half_values_are_widened() {
  const float values[] = {1, 2, -2, 0};
  for (const auto type :
       {n::TypeId::Half, n::TypeId::Half2, n::TypeId::Half4}) {
    const size_t components = n::GetComponentCount(type);
    n::Value value = n::Value::MakeArrayFromRaw(type, values, 4 / components);
    n::ArrayScratch<float> scratch;
    n::ArrayView<float> view;
    CHECK(n::GetFloatArrayView(value, &scratch, &view));
    CHECK(view.size == 4);
    const float expected[] = {1, 2, -2, 0};
    if (view.data && view.size == 4)
      for (size_t i = 0; i < 4; ++i) CHECK(view[i] == expected[i]);
    CHECK(value.type_id() == type);
  }
}

static void wire_half_does_not_alias_widened_storage() {
  auto source = std::make_shared<Source>();
  const uint16_t bits[] = {0x3c00, 0x4000, 0xc000, 0};
  const uint64_t count = 4;
  std::memcpy(reinterpret_cast<uint8_t*>(source->words.data()) + source->offset,
              &count, 8);
  std::memcpy(
      reinterpret_cast<uint8_t*>(source->words.data()) + source->offset + 8,
      bits, sizeof(bits));
  n::LazyArrayRef ref;
  ref.source = source;
  ref.block_offset = source->offset;
  ref.block_len = 16;
  ref.element_count = 4;
  ref.src_elem_stride = 2;
  ref.value_type = n::TypeId::Half;
  ref.crate_type = n::CrateTypeId::Half;
  lightusd_value value;
  value.v = n::Value::MakeLazyArray(ref);
  lightusd_value_view view{};
  CHECK(lightusd_value_get_view(&value, &view) == LIGHTUSD_OK);
  CHECK(source->decoded == 1);
  CHECK(view.storage == LIGHTUSD_COMP_FLOAT32 && view.nbytes == 16);
  const float expected[] = {1, 2, -2, 0};
  if (view.data) CHECK(std::memcmp(view.data, expected, sizeof(expected)) == 0);
}

static void bool_bytes_remain_canonical() {
  std::string bytes(18, '\0');
  bytes[8] = 2;   // little-endian count at the block offset
  bytes[17] = 2;  // tolerated noncanonical true byte
  auto source = n::CrateDataSource::Adopt(std::move(bytes), {0, 8, 0});
  n::LazyArrayRef ref;
  CHECK(n::ProbeArrayBlock(
      source, n::ValueRep::Make(n::CrateTypeId::Bool, 8, true), 16, &ref));
  lightusd_value value;
  value.v = n::Value::MakeLazyArray(ref);
  lightusd_value_view view{};
  CHECK(lightusd_value_get_view(&value, &view) == LIGHTUSD_OK);
  CHECK(view.storage == LIGHTUSD_COMP_UINT8 && view.nbytes == 2);
  const uint8_t expected[] = {0, 1};
  if (view.data) CHECK(std::memcmp(view.data, expected, sizeof(expected)) == 0);
}

static void export_preserves_borrowed_source() {
  auto source = std::make_shared<Source>();
  std::weak_ptr<Source> weak = source;
  lightusd_stage* stage = nullptr;
  CHECK(lightusd_stage_create(&stage) == LIGHTUSD_OK);
  lightusd_prim prim{};
  CHECK(lightusd_stage_define_prim(stage, "/World", "Mesh", 0, &prim) ==
        LIGHTUSD_OK);
  auto* spec = stage->stage.GetRootLayer()->prim_at_path_mutable("/World");
  CHECK(spec != nullptr);
  spec->add_property("points", lazy(source));
  lightusd_value_view borrowed{};
  CHECK(lightusd_attr_get(prim, "points", &borrowed) == LIGHTUSD_OK);
  source.reset();
  lightusd_string* text = nullptr;
  CHECK(lightusd_stage_export_usda(stage, &text) == LIGHTUSD_OK);
  CHECK(!weak.expired());
  if (!weak.expired()) {
    CHECK(weak.lock()->decoded == 0);
    const float expected[] = {1, 2, 3, 4, 5, 6};
    CHECK(std::memcmp(borrowed.data, expected, sizeof(expected)) == 0);
  }
  lightusd_string_destroy(text);
  lightusd_stage_destroy(stage);
  CHECK(weak.expired());
}

int main() {
  export_preserves_borrowed_source();
  wrong_type_does_not_decode();
  borrowed_pin_retains_source();
  unsuitable_layout_materializes();
  decode_failure_is_not_an_empty_array();
  half_values_are_widened();
  wire_half_does_not_alias_widened_storage();
  bool_bytes_remain_canonical();
  return failures ? 1 : 0;
}
