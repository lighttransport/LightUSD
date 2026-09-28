/// USDZ write tests for the next library.
/// Writes a Stage to USDZ, reads it back with the USDZ reader,
/// then reads the embedded USDC with the USDC reader.

#include "next/writer/usdz-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/reader/usdz-reader.hh"
#include "next/load-usd.hh"
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>

using namespace lightusd::next;

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { printf("  %s ... ", name); test_count++; } while(0)
#define PASS() do { pass_count++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void AppendU16LE(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
}

static void AppendU32LE(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

static void AppendStoredEntry(std::vector<uint8_t>* out,
                              const std::string& name,
                              const std::string& bytes) {
  AppendU32LE(out, 0x04034b50u);
  AppendU16LE(out, 20);
  AppendU16LE(out, 0);  // flags
  AppendU16LE(out, 0);  // stored
  AppendU16LE(out, 0);  // time
  AppendU16LE(out, 0);  // date
  AppendU32LE(out, 0);  // CRC is not consumed by USDZReader
  AppendU32LE(out, static_cast<uint32_t>(bytes.size()));
  AppendU32LE(out, static_cast<uint32_t>(bytes.size()));
  AppendU16LE(out, static_cast<uint16_t>(name.size()));
  AppendU16LE(out, 0);  // extra field
  out->insert(out->end(), name.begin(), name.end());
  out->insert(out->end(), bytes.begin(), bytes.end());
}

static Stage MakeTestStage() {
  StageBuilder sb;
  sb.SetDefaultPrim("World");
  sb.SetUpAxis("Y");

  auto& layer = sb.GetLayerBuilder();

  layer.begin_prim("World", "Xform");
  layer.end_prim();

  layer.begin_prim("Cube", "Mesh");
  layer.add_property("visibility", Value::MakeToken("inherited"));
  layer.add_property("points", Value::MakeFloat3Array(
      {-1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
       -1,-1,1, 1,-1,1, 1,1,1, -1,1,1}));
  layer.add_property("faceVertexCounts", Value::MakeIntArray({4,4,4,4,4,4}));
  layer.end_prim();

  layer.finalize();
  return sb.Build();
}

void test_write_usdz_to_memory() {
  TEST("WriteUSDZToMemory");
  auto stage = MakeTestStage();

  std::vector<uint8_t> buf;
  auto result = WriteUSDZToMemory(buf, stage);
  if (!result.success) { FAIL(result.error.c_str()); return; }

  // Verify ZIP signature
  if (buf.size() < 4) { FAIL("buffer too small"); return; }
  if (buf[0] != 0x50 || buf[1] != 0x4b || buf[2] != 0x03 || buf[3] != 0x04) {
    FAIL("missing ZIP local header signature"); return;
  }

  // Verify EOCD signature
  if (buf.size() < 22) { FAIL("buffer too small for EOCD"); return; }
  if (buf[buf.size()-22] != 0x50 || buf[buf.size()-21] != 0x4b ||
      buf[buf.size()-20] != 0x05 || buf[buf.size()-19] != 0x06) {
    FAIL("missing EOCD signature"); return;
  }

  PASS();
}

void test_write_usdz_to_file() {
  TEST("WriteUSDZToFile");
  auto stage = MakeTestStage();

  auto result = WriteUSDZToFile("/tmp/test_usdz_next.usdz", stage);
  if (!result.success) { FAIL(result.error.c_str()); return; }

  // Verify file exists
  FILE* f = fopen("/tmp/test_usdz_next.usdz", "rb");
  if (!f) { FAIL("file not created"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fclose(f);

  if (size < 100) { FAIL("file too small"); return; }

  PASS();
}

void test_read_usdz_with_reader() {
  TEST("Read USDZ with USDZReader");

  USDZReader reader;
  if (!reader.OpenFile("/tmp/test_usdz_next.usdz")) {
    FAIL("failed to open USDZ file"); return;
  }

  // Should have at least 1 entry (root.usdc)
  if (reader.NumEntries() < 1) { FAIL("no entries found"); return; }

  // Find USDC entry
  int idx = reader.FindRootLayer();
  if (idx < 0) { FAIL("no .usdc entry found"); return; }

  // Verify entry name
  const std::string& entry_name = reader.EntryName(idx);
  if (entry_name.find(".usdc") == std::string::npos) {
    FAIL("entry is not a .usdc file"); return;
  }

  // Verify entry has data
  const uint8_t* data = reader.EntryData(idx);
  size_t data_size = reader.EntrySize(idx);
  if (!data || data_size == 0) { FAIL("entry has no data"); return; }

  PASS();
}

void test_read_usdz_stage() {
  TEST("Load and read USDZ through Stage");

  Stage loaded_stage;
  std::string warn, err;
  bool ok = LoadUSD("/tmp/test_usdz_next.usdz", &loaded_stage, &warn, &err);
  if (!ok) { FAIL(err.c_str()); return; }

  // Verify prims
  auto world = loaded_stage.GetPrimAtPath("/World");
  if (!world.IsValid()) { FAIL("World not found"); return; }

  auto cube = loaded_stage.GetPrimAtPath("/Cube");
  if (!cube.IsValid()) { FAIL("Cube not found"); return; }

  PASS();
}

void test_usdz_root_is_first_entry() {
  TEST("USDZ root is first entry");
  const std::string usda = "#usda 1.0\n\ndef Xform \"World\"\n{\n}\n";

  std::vector<uint8_t> invalid;
  AppendStoredEntry(&invalid, "textures/first.bin", "not usd");
  AppendStoredEntry(&invalid, "late.usda", usda);
  USDZReader reader;
  if (!reader.Open(invalid.data(), invalid.size())) {
    FAIL("failed to parse stored-entry fixture"); return;
  }
  if (reader.FindRootLayer() >= 0) {
    FAIL("later USDA entry was selected as package root"); return;
  }
  if (reader.FindUSDAFile() != 1) {
    FAIL("legacy non-root USDA search changed unexpectedly"); return;
  }

  std::vector<uint8_t> valid;
  AppendStoredEntry(&valid, "ROOT.USD", usda);
  AppendStoredEntry(&valid, "a.png", "image");
  if (!reader.Open(valid.data(), valid.size()) || reader.FindRootLayer() != 0) {
    FAIL("content-sniffed uppercase .USD root was rejected"); return;
  }

  USDZReadOptions entry_limit;
  entry_limit.max_entries = 1;
  if (reader.Open(valid.data(), valid.size(), entry_limit) ||
      reader.Error().find("too many entries") == std::string::npos) {
    FAIL("archive entry-count limit was ignored"); return;
  }

  std::vector<uint8_t> unsafe;
  AppendStoredEntry(&unsafe, "../root.usda", usda);
  if (reader.Open(unsafe.data(), unsafe.size()) ||
      reader.Error().find("unsafe USDZ entry path") == std::string::npos) {
    FAIL("parent-traversal entry name was accepted"); return;
  }

  std::vector<uint8_t> duplicate;
  AppendStoredEntry(&duplicate, "root.usda", usda);
  AppendStoredEntry(&duplicate, "root.usda", usda);
  if (reader.Open(duplicate.data(), duplicate.size()) ||
      reader.Error().find("duplicate USDZ entry path") == std::string::npos) {
    FAIL("duplicate entry name was accepted"); return;
  }

  std::vector<uint8_t> compressed_first;
  AppendStoredEntry(&compressed_first, "first.bin", "compressed placeholder");
  // Local-header compression method field (8 = deflate). The payload itself is
  // irrelevant: USDZ must reject the method before considering a later root.
  compressed_first[8] = 8;
  compressed_first[9] = 0;
  AppendStoredEntry(&compressed_first, "late.usda", usda);
  if (reader.Open(compressed_first.data(), compressed_first.size()) ||
      reader.Error().find("compressed ZIP entry") == std::string::npos) {
    FAIL("compressed first entry was skipped in favor of a later USD layer");
    return;
  }

  std::vector<uint8_t> truncated_later;
  AppendStoredEntry(&truncated_later, "root.usda", usda);
  truncated_later.push_back(0x50);
  truncated_later.push_back(0x4b);
  truncated_later.push_back(0x03);
  if (reader.Open(truncated_later.data(), truncated_later.size()) ||
      reader.Error().find("invalid or truncated data") == std::string::npos) {
    FAIL("truncated later local header was hidden by the valid root entry");
    return;
  }

  if (!reader.Open(valid.data(), valid.size())) {
    FAIL("failed to reopen valid fixture before state-reset check"); return;
  }
  if (reader.OpenFile("/tmp/lightusd-next-definitely-missing.usdz") ||
      reader.NumEntries() != 0 || reader.EntryData(0) != nullptr) {
    FAIL("failed OpenFile retained entries from the previous archive"); return;
  }

  Stage stage;
  std::string warn, err;
  if (!LoadUSDFromMemory(valid.data(), valid.size(), &stage, LoadUSDOptions{},
                         &warn, &err)) {
    FAIL(err.empty() ? "failed to load neutral .USD package root" : err.c_str());
    return;
  }
  if (!stage.GetPrimAtPath("/World").IsValid()) {
    FAIL("neutral .USD root did not produce its stage"); return;
  }
  PASS();
}

void test_usdz_memory_caps() {
  TEST("USDZ memory caps");

  USDZReadOptions read_opts;
  read_opts.max_archive_size = 1;
  USDZReader reader;
  if (reader.OpenFile("/tmp/test_usdz_next.usdz", read_opts)) {
    FAIL("USDZReader ignored archive cap"); return;
  }

  read_opts.max_archive_size = 0;
  read_opts.max_entry_size = 1;
  if (reader.OpenFile("/tmp/test_usdz_next.usdz", read_opts)) {
    FAIL("USDZReader ignored entry cap"); return;
  }

  LoadUSDOptions load_opts;
  load_opts.limits.max_input_bytes = 1;
  Stage loaded_stage;
  std::string warn, err;
  if (LoadUSD("/tmp/test_usdz_next.usdz", &loaded_stage, load_opts, &warn, &err)) {
    FAIL("LoadUSD ignored USDZ memory cap"); return;
  }

  PASS();
}

void test_usdc_from_usdz() {
  TEST("Extract USDC from USDZ and read");

  USDZReader reader;
  if (!reader.OpenFile("/tmp/test_usdz_next.usdz")) {
    FAIL("failed to open USDZ file"); return;
  }

  int idx = reader.FindUSDCFile();
  if (idx < 0) { FAIL("no .usdc entry"); return; }

  const uint8_t* usdc_data = reader.EntryData(idx);
  size_t usdc_size = reader.EntrySize(idx);

  // Write USDC to temp file and read with USDC reader
  std::string tmpfile = "/tmp/test_usdz_extracted.usdc";
  FILE* f = fopen(tmpfile.c_str(), "wb");
  if (!f) { FAIL("can't create temp file"); return; }
  fwrite(usdc_data, 1, usdc_size, f);
  fclose(f);

  Stage stage2;
  std::string warn2, err2;
  bool ok = LoadUSDC(tmpfile, &stage2, &warn2, &err2);
  if (!ok) { FAIL(err2.c_str()); return; }

  auto world = stage2.GetPrimAtPath("/World");
  if (!world.IsValid()) { FAIL("World not found after extraction"); return; }

  PASS();
}

void test_write_from_usdc() {
  TEST("Write USDZ directly from USDC data");

  // First create a USDC in memory
  auto stage = MakeTestStage();
  std::vector<uint8_t> usdc_buf;
  auto usdc_result = WriteUSDCToMemory(usdc_buf, stage);
  if (!usdc_result.success) { FAIL(usdc_result.error.c_str()); return; }

  // Now write as USDZ
  std::vector<uint8_t> usdz_buf;
  auto result = WriteUSDZFromUSDCToMemory(usdz_buf, usdc_buf.data(), usdc_buf.size());
  if (!result.success) { FAIL(result.error.c_str()); return; }

  // Verify the USDZ can be read
  USDZReader reader;
  if (!reader.Open(usdz_buf.data(), usdz_buf.size())) {
    FAIL("failed to open USDZ from memory"); return;
  }

  int idx = reader.FindUSDCFile();
  if (idx < 0) { FAIL("no .usdc entry"); return; }

  // Verify the extracted USDC matches
  const uint8_t* extracted = reader.EntryData(idx);
  size_t extracted_size = reader.EntrySize(idx);
  if (extracted_size != usdc_buf.size()) { FAIL("USDC size mismatch"); return; }
  if (std::memcmp(extracted, usdc_buf.data(), extracted_size) != 0) {
    FAIL("USDC data mismatch"); return;
  }

  PASS();
}

void test_usdz_stream_sink() {
  TEST("Stream USDZ archive to sink");
  const std::string root = "root-usdc-payload";
  std::map<std::string, std::vector<uint8_t>> assets;
  assets["textures/albedo.bin"] = {1, 2, 3, 4};
  std::vector<uint8_t> expected;
  const auto memory = WriteUSDZFromUSDCAndAssetsToMemory(
      expected, reinterpret_cast<const uint8_t*>(root.data()), root.size(), assets);
  if (!memory.success) { FAIL(memory.error.c_str()); return; }

  std::vector<uint8_t> streamed;
  const auto result = WriteUSDZFromUSDCAndAssetsToSink(
      reinterpret_cast<const uint8_t*>(root.data()), root.size(), assets,
      [&streamed](const uint8_t* data, size_t size) {
        streamed.insert(streamed.end(), data, data + size);
        return true;
      });
  if (!result.success || result.bytes_written != streamed.size() ||
      streamed != expected) {
    FAIL(result.error.empty() ? "sink bytes differ from memory writer" : result.error.c_str());
    return;
  }
  bool rejected = false;
  const auto failed = WriteUSDZFromUSDCAndAssetsToSink(
      reinterpret_cast<const uint8_t*>(root.data()), root.size(), assets,
      [&rejected](const uint8_t*, size_t) { rejected = true; return false; });
  if (failed.success || !rejected || failed.error.empty()) {
    FAIL("sink rejection was not propagated"); return;
  }
  PASS();
}

void test_write_from_usda_with_assets() {
  TEST("Write USDA root with packaged assets");
  const std::string usda = "#usda 1.0\n\ndef Xform \"World\" {}\n";
  std::map<std::string, std::vector<uint8_t>> assets;
  assets["textures/albedo.bin"] = {1, 2, 3};
  std::vector<uint8_t> bytes;
  const auto result = WriteUSDZFromUSDAAndAssetsToMemory(
      bytes, reinterpret_cast<const uint8_t*>(usda.data()), usda.size(), assets);
  if (!result.success) { FAIL(result.error.c_str()); return; }

  USDZReader reader;
  if (!reader.Open(bytes.data(), bytes.size())) {
    FAIL("failed to open USDA-root USDZ"); return;
  }
  const int root = reader.FindRootLayer();
  if (root < 0 || reader.EntryName(root) != "root.usda" ||
      reader.EntrySize(root) != usda.size() ||
      std::memcmp(reader.EntryData(root), usda.data(), usda.size()) != 0) {
    FAIL("USDA root layer was not written faithfully"); return;
  }
  int asset = -1;
  for (size_t i = 0; i < reader.NumEntries(); ++i) {
    if (reader.EntryName(i) == "textures/albedo.bin") {
      asset = static_cast<int>(i);
      break;
    }
  }
  if (asset < 0 || reader.EntrySize(asset) != 3 ||
      reader.EntryData(asset)[2] != 3) {
    FAIL("packaged asset was not retained"); return;
  }
  PASS();
}

void test_write_failure() {
  TEST("Report file write failure");
#if defined(__linux__)
  auto stage = MakeTestStage();
  auto result = WriteUSDZToFile("/dev/full", stage);
  if (result.success || result.error.empty() ||
      result.bytes_written > (size_t{1} << 30)) {
    FAIL("/dev/full write was not reported safely"); return;
  }
#endif
  PASS();
}

int main() {
  printf("USDZ Writer Tests\n");
  printf("=================\n\n");

  test_write_usdz_to_memory();
  test_write_usdz_to_file();
  test_read_usdz_with_reader();
  test_read_usdz_stage();
  test_usdz_root_is_first_entry();
  test_usdz_memory_caps();
  test_usdc_from_usdz();
  test_write_from_usdc();
  test_usdz_stream_sink();
  test_write_from_usda_with_assets();
  test_write_failure();

  printf("\n%d/%d tests passed\n", pass_count, test_count);
  return pass_count == test_count ? 0 : 1;
}
