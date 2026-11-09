// SPDX-License-Identifier: Apache 2.0
// Unit tests for VtArrayEdit (crate ValueRep IsArrayEdit bit, crate >= 0.14.0).
//
// The local OpenUSD build only supports crate <= 0.9.0, so cross-tool fixtures
// for 0.14.0 cannot be generated here; these tests build a lightusd::legacy::value::ArrayEdit
// programmatically and exercise the Crate write + read round-trip.

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-array-edit.h"
#include "lightusd.hh"
#include "usdc-writer.hh"
#include "array-edit.hh"
#include "crate-format.hh"
#include "core/property.hh"
#include "usdGeom.hh"  // Xform

#include <string>
#include <vector>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void array_edit_crate_roundtrip_test(void) {
  // Build: int[] edited = edit [ write 123 to [1]; append 234; erase [0] ]
  lightusd::legacy::value::ArrayEdit ae;
  ae.element_type_id =
      static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INT);
  ae.literals = lightusd::legacy::value::Value(std::vector<int32_t>{123, 234});
  // write literal[0] (=123) to [1]
  ae.ops.push_back(
      lightusd::legacy::value::ArrayEditPackOpWord(lightusd::legacy::value::ArrayEditOp::WriteLiteral, 1));
  ae.ops.push_back(0);
  ae.ops.push_back(1);
  // append literal[1] (=234)  -- insert at EndIndex
  ae.ops.push_back(
      lightusd::legacy::value::ArrayEditPackOpWord(lightusd::legacy::value::ArrayEditOp::InsertLiteral, 1));
  ae.ops.push_back(1);
  ae.ops.push_back(lightusd::legacy::value::kArrayEditEndIndex);
  // erase [0]
  ae.ops.push_back(lightusd::legacy::value::ArrayEditPackOpWord(lightusd::legacy::value::ArrayEditOp::EraseRef, 1));
  ae.ops.push_back(0);

  Attribute attr;
  attr.set_type_name("int[]");
  attr.set_value(ae);

  Xform xform;
  xform.name = "Foo";
  xform.props["edited"] = Property(attr, /*custom=*/true);

  Stage stage;
  TEST_CHECK(stage.add_root_prim(Prim(xform)));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;

  // A VtArrayEdit value must bump the emitted crate version to >= 0.14.0.
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 14, "crate version minor = %d, expected >= 14",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc", &stage2,
                                 &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  TEST_CHECK(stage2.root_prims().size() == 1);
  if (stage2.root_prims().empty()) return;
  const Xform* xf = stage2.root_prims()[0].as<Xform>();
  TEST_CHECK(xf != nullptr);
  if (!xf) return;

  auto it = xf->props.find("edited");
  TEST_CHECK_(it != xf->props.end(), "`edited` property missing after reload");
  if (it == xf->props.end()) return;

  auto got = it->second.get_attribute().get_value<lightusd::legacy::value::ArrayEdit>();
  TEST_CHECK_(bool(got), "reloaded attr value is not a lightusd::legacy::value::ArrayEdit");
  if (!got) return;

  TEST_CHECK(got.value().element_type_id == ae.element_type_id);
  TEST_CHECK_(got.value().ops == ae.ops, "op stream mismatch after roundtrip");

  auto lits = got.value().literals.as<std::vector<int32_t>>();
  TEST_CHECK_(lits != nullptr, "literals not int[] after roundtrip");
  if (lits) {
    TEST_CHECK(lits->size() == 2);
    if (lits->size() == 2) {
      TEST_CHECK((*lits)[0] == 123 && (*lits)[1] == 234);
    }
  }
}

void array_edit_identity_test(void) {
  // An identity edit (no ops) is inlined: no out-of-line data, payload 0.
  lightusd::legacy::value::ArrayEdit ae;
  ae.element_type_id =
      static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_FLOAT);
  // ops intentionally empty
  TEST_CHECK(ae.is_identity());

  Attribute attr;
  attr.set_type_name("float[]");
  attr.set_value(ae);

  Xform xform;
  xform.name = "Bar";
  xform.props["e"] = Property(attr, /*custom=*/true);

  Stage stage;
  TEST_CHECK(stage.add_root_prim(Prim(xform)));

  std::vector<uint8_t> usdc;
  std::string w, e;
  TEST_CHECK(usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e));
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 14, "crate version minor = %d, expected >= 14",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc", &stage2,
                                 &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  if (stage2.root_prims().empty()) {
    TEST_CHECK(false);
    return;
  }
  const Xform* xf = stage2.root_prims()[0].as<Xform>();
  TEST_CHECK(xf != nullptr);
  if (!xf) return;
  auto it = xf->props.find("e");
  TEST_CHECK(it != xf->props.end());
  if (it == xf->props.end()) return;

  auto got = it->second.get_attribute().get_value<lightusd::legacy::value::ArrayEdit>();
  TEST_CHECK_(bool(got), "reloaded identity attr is not a lightusd::legacy::value::ArrayEdit");
  if (!got) return;
  TEST_CHECK(got.value().is_identity());
  TEST_CHECK(got.value().element_type_id == ae.element_type_id);
}

static const char* kArrayEditUSDA =
    "#usda 1.0\n"
    "\n"
    "def Xform \"W\"\n"
    "{\n"
    "    custom int[] iattr = edit [\n"
    "        write 123 to [0]\n"
    "        append 234\n"
    "        prepend 99\n"
    "        erase [1]\n"
    "        resize 8 fill 7\n"
    "    ]\n"
    "}\n";

static bool LoadAE(const std::string& usda, Stage* stage) {
  std::string warn, err;
  bool ok = LoadUSDFromMemory(reinterpret_cast<const uint8_t*>(usda.data()),
                              usda.size(), "mem.usda", stage, &warn, &err);
  if (!ok) TEST_MSG("USDA parse failed: %s", err.c_str());
  return ok;
}

// Verify the parsed op stream for kArrayEditUSDA: write/append/prepend/erase/
// resize-fill, with literals [123, 234, 99, 7].
static void CheckParsedEdit(const Stage& stage) {
  const Xform* xf = stage.root_prims().empty()
                        ? nullptr
                        : stage.root_prims()[0].as<Xform>();
  TEST_CHECK(xf != nullptr);
  if (!xf) return;
  auto it = xf->props.find("iattr");
  TEST_CHECK(it != xf->props.end());
  if (it == xf->props.end()) return;
  auto got = it->second.get_attribute().get_value<lightusd::legacy::value::ArrayEdit>();
  TEST_CHECK_(bool(got), "iattr is not a lightusd::legacy::value::ArrayEdit");
  if (!got) return;

  // Literals collected in encounter order: 123, 234, 99, 7.
  auto lits = got.value().literals.as<std::vector<int32_t>>();
  TEST_CHECK_(lits != nullptr, "literals not int[]");
  if (lits) {
    TEST_CHECK(lits->size() == 4);
    if (lits->size() == 4) {
      TEST_CHECK((*lits)[0] == 123 && (*lits)[1] == 234 && (*lits)[2] == 99 &&
                 (*lits)[3] == 7);
    }
  }

  // Decode the op stream and check the op sequence.
  const auto& ins = got.value().ops;
  std::vector<lightusd::legacy::value::ArrayEditOp> ops_seq;
  size_t i = 0;
  while (i < ins.size()) {
    lightusd::legacy::value::ArrayEditOp op = lightusd::legacy::value::ArrayEditWordOp(ins[i]);
    int64_t cnt = lightusd::legacy::value::ArrayEditWordCount(ins[i]);
    ++i;
    int arity = lightusd::legacy::value::ArrayEditOpArity(op);
    for (int64_t c = 0; c < cnt; c++) {
      ops_seq.push_back(op);
      i += static_cast<size_t>(arity);
    }
  }
  TEST_CHECK(ops_seq.size() == 5);
  if (ops_seq.size() == 5) {
    TEST_CHECK(ops_seq[0] == lightusd::legacy::value::ArrayEditOp::WriteLiteral);
    TEST_CHECK(ops_seq[1] == lightusd::legacy::value::ArrayEditOp::InsertLiteral);  // append
    TEST_CHECK(ops_seq[2] == lightusd::legacy::value::ArrayEditOp::InsertLiteral);  // prepend
    TEST_CHECK(ops_seq[3] == lightusd::legacy::value::ArrayEditOp::EraseRef);
    TEST_CHECK(ops_seq[4] == lightusd::legacy::value::ArrayEditOp::SetSizeFill);
  }
}

void array_edit_usda_roundtrip_test(void) {
  // Parse -> export USDA -> re-parse; the edit (and its op stream) must survive.
  Stage stage;
  TEST_CHECK(LoadAE(kArrayEditUSDA, &stage));
  CheckParsedEdit(stage);

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("edit [") != std::string::npos);
  TEST_CHECK(exported.find("write 123 to [0]") != std::string::npos);
  TEST_CHECK(exported.find("append 234") != std::string::npos);
  TEST_CHECK(exported.find("prepend 99") != std::string::npos);
  TEST_CHECK(exported.find("erase [1]") != std::string::npos);
  TEST_CHECK(exported.find("resize 8 fill 7") != std::string::npos);

  Stage stage2;
  TEST_CHECK(LoadAE(exported, &stage2));
  CheckParsedEdit(stage2);
}

void array_edit_usda_crate_roundtrip_test(void) {
  // Parse USDA -> save USDC (0.14.0) -> reload -> re-export; edit survives.
  Stage stage;
  TEST_CHECK(LoadAE(kArrayEditUSDA, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 14, "crate version minor = %d, expected >= 14",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc", &stage2,
                                 &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;
  CheckParsedEdit(stage2);
}
