// SPDX-License-Identifier: Apache-2.0
#include "layer/int64-listop.hh"
#include "crate/crate-reader.hh"
#include "crate/listop-wire.hh"
#include "crate/crate-writer.hh"
#include "pcp/batched-compose.hh"
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "schema/geom-point-instancer.hh"
#include "writer/usda-writer.hh"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <chrono>
#include <filesystem>

using namespace lightusd;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static const DecodedExtensionField* Field(const Stage& stage) {
  const auto prim = stage.GetPrimAtPath("/P");
  if (!prim.IsValid()) return nullptr;
  for (const auto& field : prim.GetPrimSpec()->meta().unknownFields())
    if (field.name == "inactiveIds") return &field;
  return nullptr;
}
static std::vector<int64_t> Ids(const Value& value) {
  auto a = value.as_int64_array();
  return a ? std::vector<int64_t>(a->begin(), a->end()) : std::vector<int64_t>();
}
static std::vector<uint8_t> Encode(const Stage& stage, int threads = 1) {
  CrateWriteOptions options;
  options.num_threads = threads;
  std::vector<uint8_t> data;
  const auto r = CrateWriter(options).WriteToMemory(data, stage);
  CHECK(r.success);
  return data;
}
static Stage Decode(const std::vector<uint8_t>& data) {
  auto r = LoadUSDCFromMemory(data.data(), data.size());
  CHECK(r.success);
  CHECK(r.warnings.empty());
  return std::move(r.stage);
}
static void Put64(std::vector<uint8_t>* data, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i) data->push_back(uint8_t(n >> (8 * i)));
}
static size_t Payload(const std::vector<uint8_t>& data) {
  CrateReader reader;
  CHECK(reader.Read(data.data(), data.size()).success);
  for (const auto& field : reader.fields())
    if (field.value_rep.type_id() == CrateTypeId::Int64ListOp)
      return size_t(field.value_rep.payload());
  CHECK(false);
  return 0;
}
static void Save(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  CHECK(f.good());
}
static void SaveText(const std::string& path, const std::string& text) {
  std::ofstream f(path);
  f << text;
  CHECK(f.good());
}
static void CheckOperationModel() {
  uint64_t seed = 12345;
  auto random = [&]() { seed = seed * UINT64_C(6364136223846793005) + 1; return seed >> 32; };
  const int64_t ids[] = {INT64_MIN, -3, -1, 0, 1, 2, INT64_MAX};
  for (int trial = 0; trial < 300; ++trial) {
    Int64ListOpStack stack;
    for (int layer = 0; layer < 3; ++layer) {
      Int64ListOp op;
      op.bits = random() % 5 == 0 ? 3 : 0x7c;
      for (size_t r = 0; r < 6; ++r) {
        std::vector<int64_t> values;
        const size_t n = random() % 8;
        for (size_t i = 0; i < n; ++i) values.push_back(ids[random() % 7]);
        op.items[r] = Value::MakeInt64Array(values);
      }
      stack.ops.push_back(std::move(op));
    }
    // Independent existing string-list-op model, using exact decimal IDs.
    std::vector<std::string> expected;
    for (auto op = stack.ops.rbegin(); op != stack.ops.rend(); ++op) {
      StringListOps text;
      text.authored = true;
      text.is_explicit = op->is_explicit();
      std::vector<std::string>* lists[] = {&text.explicit_items, &text.added,
          &text.prepended, &text.appended, &text.deleted, &text.ordered};
      for (size_t r = 0; r < 6; ++r)
        for (int64_t id : Ids(op->items[r])) lists[r]->push_back(std::to_string(id));
      expected = ApplyStringListOp(text, expected);
    }
    const auto actual = Ids(ApplyInt64ListOps(stack));
    CHECK(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size() && i < expected.size(); ++i)
      CHECK(std::to_string(actual[i]) == expected[i]);
  }
}

static int WriteHeaderCases(const char* directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return 1;
  std::printf("[");
  bool first = true;
  for (unsigned empty = 0; empty < 2; ++empty) {
    for (unsigned header = 0; header < 128; ++header) {
      auto source = LoadUSDAFromString("#usda 1.0\ndef PointInstancer \"P\" {}\n");
      CHECK(source.success);
      Int64ListOp op;
      op.bits = static_cast<uint8_t>(header);
      for (size_t run = 0; run < 6; ++run) {
        std::vector<int64_t> items;
        if (!empty && (header & kInt64ListOpBits[run])) items.push_back(int64_t(run + 10));
        op.items[run] = Value::MakeInt64Array(items);
      }
      source.stage.GetRootLayer()->prim_at_path_mutable("/P")->meta().unknownFields().push_back(
          MakeInt64ListOpField("inactiveIds", op));
      const auto bytes = Encode(source.stage);
      const size_t offset = Payload(bytes);
      CHECK(offset < bytes.size() && bytes[offset] == header);
      if (header == 0x22 && !empty) {
        auto invalid = bytes;
        for (size_t i = 0; i < 8; ++i) invalid[offset + 1 + i] = 0xff;
        CHECK(!CrateReader().Read(invalid.data(), invalid.size()).success);
      }
      const std::string name = std::to_string(empty) + "-" + std::to_string(header) + ".usdc";
      Save(std::string(directory) + "/" + name, bytes);
      Stage decoded = Decode(bytes);
      const auto* field = Field(decoded);
      CHECK(field && field->int64_list_op && field->int64_list_op->ops.size() == 1);
      if (!field || !field->int64_list_op || field->int64_list_op->ops.size() != 1) continue;
      const auto& actual = field->int64_list_op->ops[0];
      if (!first) std::printf(",");
      first = false;
      std::printf("{\"file\":\"%s\",\"header\":%u,\"explicit\":%s,\"runs\":[",
                  name.c_str(), header, actual.is_explicit() ? "true" : "false");
      auto print_items = [](const std::vector<int64_t>& items) {
        std::printf("[");
        for (size_t i = 0; i < items.size(); ++i)
          std::printf("%s%lld", i ? "," : "", static_cast<long long>(items[i]));
        std::printf("]");
      };
      for (size_t run = 0; run < 6; ++run) {
        if (run) std::printf(",");
        print_items(Ids(actual.items[run]));
      }
      std::printf("],\"effective\":");
      print_items(Ids(field->value));
      std::printf("}");
    }
  }
  struct Family { const char* name; const char* authored; const char* field; CrateTypeId type; size_t width; const char* spec = "/P"; };
  const Family families[] = {
      {"token", "apiSchemas", "apiSchemas", CrateTypeId::TokenListOp, 4},
      {"string", "variantSets", "variantSetNames", CrateTypeId::StringListOp, 4},
      {"path", "inherits", "inheritPaths", CrateTypeId::PathListOp, 4},
      {"reference", "references", "references", CrateTypeId::ReferenceListOp, 32},
      {"payload", "payload", "payload", CrateTypeId::PayloadListOp, 24},
      {"relationship", "rel r", "targetPaths", CrateTypeId::PathListOp, 4, "/P.r"},
      {"connection", "float a.connect", "connectionPaths", CrateTypeId::PathListOp, 4, "/P.a"}};
  const char* qualifiers[] = {"add", "prepend", "append", "delete", "reorder"};
  for (const auto& family : families) {
    const bool property = std::string(family.spec) != "/P";
    std::string text = "#usda 1.0\ndef Scope \"P\" ";
    text += property ? "{\n" : "(\n";
    for (size_t run = 0; run < 5; ++run) {
      text += std::string(qualifiers[run]) + " " + family.authored + " = [";
      for (size_t item = 0; item < 2; ++item) {
        if (item) text += ", ";
        const std::string name = "Item_" + std::to_string(run) + "_" + std::to_string(item);
        if (family.width > 4) text += "@dep.usda@</" + name + ">";
        else if (family.type == CrateTypeId::PathListOp)
          text += "</" + name + (std::string(family.name) == "connection" ? ".a>" : ">");
        else text += "\"" + name + "\"";
      }
      text += "]\n";
    }
    text += property ? "}\n" : ") {}\n";
    auto source = LoadUSDAFromString(text.data(), text.size());
    CHECK(source.success);
    if (!source.success) continue;
    const auto baseline = Encode(source.stage);
    CrateReader layout;
    CHECK(layout.Read(baseline.data(), baseline.size()).success);
    size_t offset = 0;
    for (const auto& field : layout.fields())
      if (field.value_rep.type_id() == family.type) offset = size_t(field.value_rep.payload());
    CHECK(offset && offset < baseline.size() && baseline[offset] == 0x7c);
    if (!offset || offset >= baseline.size() || baseline[offset] != 0x7c) continue;
    StreamReader wire(baseline);
    CHECK(wire.seek(offset + 1));
    std::array<std::vector<uint8_t>, 6> items;
    for (size_t run = 1; run < 6; ++run) {
      uint64_t count = 0;
      CHECK(wire.read_u64(count) && count == 2);
      const size_t at = wire.position();
      CHECK(wire.has_elements(2, family.width));
      items[run].assign(baseline.begin() + at, baseline.begin() + at + family.width);
      if (run == 1) items[0].assign(baseline.begin() + at + family.width,
                                  baseline.begin() + at + 2 * family.width);
      CHECK(wire.seek(at + 2 * family.width));
    }
    const size_t capacity = wire.position() - offset;
    for (unsigned empty = 0; empty < 2; ++empty) for (unsigned header = 0; header < 128; ++header) {
      std::vector<uint8_t> payload{static_cast<uint8_t>(header)};
      for (size_t run = 0; run < 6; ++run) if (header & kInt64ListOpBits[run]) {
        Put64(&payload, empty ? 0 : 1);
        if (!empty) payload.insert(payload.end(), items[run].begin(), items[run].end());
      }
      CHECK(payload.size() <= capacity);
      if (payload.size() > capacity) continue;
      auto original = baseline;
      std::copy(payload.begin(), payload.end(), original.begin() + offset);
      if (header == 0x22 && !empty) {
        // Discarding the explicit vector must not bypass its count or index validation.
        auto invalid = original;
        for (size_t i = 0; i < 4; ++i) invalid[offset + 9 + i] = 0xff;
        const auto compatible = CrateReader().Read(invalid.data(), invalid.size());
        CHECK(compatible.success && !compatible.warnings.empty());
        CrateReadOptions strict;
        strict.strict_aousd_conformance = true;
        CHECK(!CrateReader(strict).Read(invalid.data(), invalid.size()).success);
        invalid = original;
        for (size_t i = 0; i < 8; ++i) invalid[offset + 1 + i] = 0xff;
        CHECK(!CrateReader().Read(invalid.data(), invalid.size()).success);
      }
      const std::string name = std::string(family.name) + "-" + std::to_string(empty) + "-" +
                               std::to_string(header) + ".usdc";
      Save(std::string(directory) + "/" + name, original);
      const std::string rewritten = "rewritten-" + name;
      Save(std::string(directory) + "/" + rewritten, Encode(Decode(original)));
      std::printf(",{\"kind\":\"roundtrip\",\"file\":\"%s\",\"rewritten\":\"%s\",\"field\":\"%s\",\"spec\":\"%s\"}",
                  name.c_str(), rewritten.c_str(), family.field, family.spec);
    }
  }
  std::printf("]\n");
  return failures ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--header-cases") return WriteHeaderCases(argv[2]);
  CheckOperationModel();
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) return 1;
  std::string dir;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto path = temporary / ("lightusd-int64-listop-" + std::to_string(stamp) +
                              "-" + std::to_string(attempt));
    if (std::filesystem::create_directory(path, error)) { dir = path.string(); break; }
  }
  if (dir.empty()) return 1;
  const int64_t big = INT64_C(9007199254740993);
  const std::vector<int64_t> extremes = {INT64_MIN, -1, 0, big, INT64_MAX};

  auto base = LoadUSDAFromString("#usda 1.0\ndef PointInstancer \"P\" {}\n");
  CHECK(base.success);
  Int64ListOp explicit_op;
  explicit_op.bits = 3;
  explicit_op.items[0] = Value::MakeInt64Array(extremes);
  base.stage.GetRootLayer()->prim_at_path_mutable("/P")->meta().unknownFields().push_back(
      MakeInt64ListOpField("inactiveIds", explicit_op));
  auto bytes = Encode(base.stage);
  const size_t payload = Payload(bytes);
  // Independently spell the AOUSD payload: header + LE u64 count + raw i64s.
  std::vector<uint8_t> expected{3};
  Put64(&expected, extremes.size());
  for (int64_t id : extremes) Put64(&expected, uint64_t(id));
  CHECK(payload + expected.size() <= bytes.size());
  CHECK(std::equal(expected.begin(), expected.end(), bytes.begin() + payload));
  auto decoded = Decode(bytes);
  CHECK(Field(decoded) && Ids(Field(decoded)->value) == extremes);
  USDCLoadOptions strict;
  strict.crate_options.strict_aousd_conformance = true;
  CHECK(LoadUSDCFromMemory(bytes.data(), bytes.size(), strict).success);
  CHECK(Encode(decoded) == bytes);
  auto explicit_text = LoadUSDAFromString(WriteUSDAToString(decoded));
  CHECK(explicit_text.success && Field(explicit_text.stage) &&
        Ids(Field(explicit_text.stage)->value) == extremes);
  CHECK(Encode(explicit_text.stage) == bytes);
  CHECK(Encode(base.stage, 4) == bytes);
  if (argc > 1) Save(argv[1], bytes);

  auto metadata = LoadUSDAFromString("#usda 1.0\ndef Scope \"P\" { float a = 1 }\n");
  CHECK(metadata.success);
  auto* meta_prim = metadata.stage.GetRootLayer()->prim_at_path_mutable("/P");
  meta_prim->meta().unknownFields().push_back(
      MakeInt64ListOpField("inactiveIds", explicit_op));
  DecodedExtensionField other;
  other.name = "other";
  other.value = Value("hello");
  other.unregistered = true;
  other.unregistered_source = "\"hello\"";
  meta_prim->meta().unknownFields().push_back(std::move(other));
  metadata.stage.GetRootLayer()->meta().unknownFields.push_back(
      MakeInt64ListOpField("testList", explicit_op));
  auto& property = meta_prim->ensure_property_meta("a");
  property.authored |= PropMeta::kUnknownMeta;
  property.unknownFields.push_back(MakeInt64ListOpField("testList", explicit_op));
  auto metadata_bytes = Encode(metadata.stage);
  CHECK(Encode(Decode(metadata_bytes)) == metadata_bytes);
  const auto old_memory = metadata.stage.GetRootLayer()->memory_usage();
  Int64ListOp large_op;
  large_op.bits = 3;
  std::vector<int64_t> large_ids(4096, 1);
  for (size_t i = 0; i < large_ids.size(); ++i) large_ids[i] = int64_t(i);
  large_op.items[0] = Value::MakeInt64Array(large_ids);
  meta_prim->meta().unknownFields()[0] = MakeInt64ListOpField("inactiveIds", large_op);
  CHECK(metadata.stage.GetRootLayer()->memory_usage() >= old_memory + 2 * 4000 * sizeof(int64_t));

  // Force the writer's chunk pipeline; small fixtures take its serial path.
  std::string padded_source = "#usda 1.0\ndef Scope \"P\" {}\n";
  for (int i = 0; i < 9000; ++i)
    padded_source += "def Scope \"F" + std::to_string(i) + "\" {}\n";
  auto padded = LoadUSDAFromString(padded_source);
  CHECK(padded.success);
  padded.stage.GetRootLayer()->prim_at_path_mutable("/P")->meta().unknownFields().push_back(
      MakeInt64ListOpField("inactiveIds", explicit_op));
  CHECK(Encode(padded.stage, 1) == Encode(padded.stage, 4));
  std::vector<std::vector<uint8_t>> streamed;
  for (int threads : {1, 4}) {
    CrateWriteOptions options;
    options.num_threads = threads;
    const std::string file = dir + "/stream.usdc";
    CHECK(CrateWriter(options).WriteLayerToFile(file.c_str(), *padded.stage.GetRootLayer()).success);
    std::ifstream input(file, std::ios::binary);
    streamed.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    std::remove(file.c_str());
  }
  CHECK(streamed[0] == streamed[1]);

  const std::string authored =
      "#usda 1.0\ndef PointInstancer \"P\" (\n"
      "add inactiveIds = [7]\nprepend inactiveIds = [5]\n"
      "append inactiveIds = [9]\ndelete inactiveIds = [3]\n"
      "reorder inactiveIds = [9, 5]\n) {}\n";
  auto all = LoadUSDAFromString(authored);
  CHECK(all.success);
  const auto* field = Field(all.stage);
  CHECK(field && field->int64_list_op && field->int64_list_op->ops[0].bits == 0x7c);
  CHECK(field && Ids(field->value) == std::vector<int64_t>({9, 5, 7}));
  const auto all_bytes = Encode(all.stage);
  CrateReadOptions per_run_limit;
  per_run_limit.max_array_elements = 2;
  CHECK(CrateReader(per_run_limit).Read(all_bytes.data(), all_bytes.size()).success);
  auto all_decoded = Decode(all_bytes);
  CHECK(Encode(all_decoded) == all_bytes);
  auto text_roundtrip = LoadUSDAFromString(WriteUSDAToString(all_decoded));
  CHECK(text_roundtrip.success && Encode(text_roundtrip.stage) == all_bytes);

  for (uint8_t bits : {uint8_t(0), uint8_t(1), uint8_t(3), uint8_t(32)}) {
    Int64ListOp empty;
    empty.bits = bits;
    auto& fields = base.stage.GetRootLayer()->prim_at_path_mutable("/P")->meta().unknownFields();
    fields[0] = MakeInt64ListOpField("inactiveIds", empty);
    auto b = Encode(base.stage);
    CHECK(Encode(Decode(b)) == b);
  }

  auto bad = bytes;
  bad[payload] |= 0x80;
  CHECK(!CrateReader().Read(bad.data(), bad.size()).success);
  bad = bytes;
  for (size_t i = 0; i < 8; ++i) bad[payload + 1 + i] = 0xff;
  CHECK(!CrateReader().Read(bad.data(), bad.size()).success);
  CrateReadOptions limited;
  limited.max_array_elements = 2;
  CHECK(!CrateReader(limited).Read(bytes.data(), bytes.size()).success);

  const std::string mask_source =
      "#usda 1.0\ndef PointInstancer \"P\" (inactiveIds = [-9223372036854775808, 9007199254740993]) {\n"
      "int[] protoIndices = [0, 0, 0, 0]\n"
      "int64[] ids = [-9223372036854775808, 9007199254740993, 7, 9223372036854775807]\n"
      "int64[] invisibleIds = [7]\n}\n";
  auto mask = LoadUSDAFromString(mask_source);
  CHECK(mask.success);
  LoadOptions text_limit;
  text_limit.parse_options.max_usda_lazy_array_elements = 1;
  // This existing option limits lazy retention, not eager parsing.
  CHECK(LoadUSDAFromString(mask_source, text_limit).success);
  CHECK(!LoadUSDAFromString("#usda 1.0\ndef Scope \"P\" (inactiveIds = [9223372036854775808]) {}\n").success);
  CHECK(!LoadUSDAFromString("#usda 1.0\ndef Scope \"P\" (inactiveIds = [-9223372036854775809]) {}\n").success);
  UsdGeomPointInstancer pi(mask.stage.GetPrimAtPath("/P"));
  CHECK(pi.GetIds() == std::vector<int64_t>({INT64_MIN, big, 7, INT64_MAX}));
  CHECK(pi.GetInactiveIds() == std::vector<int64_t>({INT64_MIN, big}));
  CHECK(pi.ComputeMaskAtTime(0) == std::vector<bool>({false, false, false, true}));
  auto mask_dc = Decode(Encode(mask.stage));
  CHECK(UsdGeomPointInstancer(mask_dc.GetPrimAtPath("/P")).ComputeMaskAtTime(0) ==
        std::vector<bool>({false, false, false, true}));

  SaveText(dir + "/weak.usda", "#usda 1.0\ndef PointInstancer \"P\" (inactiveIds = [1, 2, 3, 9007199254740993]) {}\n");
  SaveText(dir + "/middle.usda", "#usda 1.0\nover \"P\" (delete inactiveIds = [3]\nprepend inactiveIds = [4]) {}\n");
  SaveText(dir + "/root.usda", "#usda 1.0\n(subLayers = [@middle.usda@, @weak.usda@])\nover \"P\" (delete inactiveIds = [1]\nappend inactiveIds = [2]\nreorder inactiveIds = [2, 4]) {}\n");
  AssetResolver resolver;
  pcp::CompositionOptions options;
  options.num_threads = 4;
  Stage composed;
  std::string warn, err;
  CHECK(pcp::ComposeStageFromFile(dir + "/root.usda", resolver, &composed, options, &warn, &err));
  CHECK(Field(composed) && Ids(Field(composed)->value) == std::vector<int64_t>({2, 4, big}));
  auto composed_dc = Decode(Encode(composed));
  CHECK(Field(composed_dc) && Ids(Field(composed_dc)->value) == std::vector<int64_t>({2, 4, big}));
  Stage batched;
  pcp::BatchedComposeOptions batch;
  batch.target_batch_prims = 1;
  batch.max_frontier = 1;
  CHECK(pcp::ComposeStageBatched(dir + "/root.usda", resolver, &batched, options, batch, &warn, &err));
  CHECK(WriteUSDAToString(batched) == WriteUSDAToString(composed));
  SaveText(dir + "/clear.usda", "#usda 1.0\n(subLayers = [@weak.usda@])\nover \"P\" (inactiveIds = None) {}\n");
  Stage cleared;
  CHECK(pcp::ComposeStageFromFile(dir + "/clear.usda", resolver, &cleared, options, &warn, &err));
  CHECK(Field(cleared) && Ids(Field(cleared)->value).empty());
  for (const char* name : {"weak.usda", "middle.usda", "root.usda", "clear.usda"})
    std::remove((dir + "/" + name).c_str());
  std::filesystem::remove(dir, error);
  std::printf("Int64 list-op failures: %d\n", failures);
  return failures ? 1 : 0;
}
