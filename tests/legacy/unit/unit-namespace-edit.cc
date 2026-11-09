// SPDX-License-Identifier: Apache 2.0
// Unit tests for Stage namespace editing (RenamePrim / RemovePrim /
// ReparentPrim) -- mechanical prim-tree edits.

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-namespace-edit.h"
#include "lightusd.hh"

#include <string>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

static bool LoadUSDA(const std::string &usda, Stage *stage) {
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      stage, &warn, &err);
  if (!ok) TEST_MSG("USDA parse failed: %s", err.c_str());
  return ok;
}

static bool HasPrim(const Stage &stage, const char *path) {
  return bool(stage.GetPrimAtPath(Path(path, "")));
}

static const char *kHierarchy =
    "#usda 1.0\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Scope \"A\"\n"
    "    {\n"
    "        def Xform \"child\"\n"
    "        {\n"
    "        }\n"
    "    }\n"
    "    def Scope \"B\"\n"
    "    {\n"
    "    }\n"
    "}\n";

void namespace_edit_rename_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kHierarchy, &stage));
  TEST_CHECK(HasPrim(stage, "/World/A"));

  std::string err;
  TEST_CHECK_(stage.RenamePrim(Path("/World/A", ""), "Renamed", &err),
              "rename failed: %s", err.c_str());

  // Old path gone, new path present, and the subtree moved with it.
  TEST_CHECK(!HasPrim(stage, "/World/A"));
  TEST_CHECK(HasPrim(stage, "/World/Renamed"));
  TEST_CHECK(HasPrim(stage, "/World/Renamed/child"));
  TEST_CHECK(!HasPrim(stage, "/World/A/child"));
  {
    auto child = stage.GetPrimAtPath(Path("/World/Renamed/child", ""));
    TEST_CHECK(child && child.value());
    if (child && child.value()) {
      TEST_CHECK(child.value()->absolute_path().full_path_name() ==
                 "/World/Renamed/child");
    }
  }

  // Export still references the renamed prim.
  std::string usda = stage.ExportToString();
  TEST_CHECK(usda.find("Scope \"Renamed\"") != std::string::npos);
  TEST_CHECK(usda.find("Scope \"A\"") == std::string::npos);
}

void namespace_edit_remove_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kHierarchy, &stage));
  TEST_CHECK(HasPrim(stage, "/World/A/child"));

  std::string err;
  TEST_CHECK_(stage.RemovePrim(Path("/World/A", ""), &err), "remove failed: %s",
              err.c_str());

  // The prim and its whole subtree are gone; siblings remain.
  TEST_CHECK(!HasPrim(stage, "/World/A"));
  TEST_CHECK(!HasPrim(stage, "/World/A/child"));
  TEST_CHECK(HasPrim(stage, "/World/B"));
  TEST_CHECK(HasPrim(stage, "/World"));

  std::string usda = stage.ExportToString();
  TEST_CHECK(usda.find("Scope \"A\"") == std::string::npos);
  TEST_CHECK(usda.find("Scope \"B\"") != std::string::npos);
}

void namespace_edit_reparent_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kHierarchy, &stage));

  // Move /World/A under /World/B.
  std::string err;
  TEST_CHECK_(stage.ReparentPrim(Path("/World/A", ""), Path("/World/B", ""), "",
                                 &err),
              "reparent failed: %s", err.c_str());

  TEST_CHECK(!HasPrim(stage, "/World/A"));
  TEST_CHECK(HasPrim(stage, "/World/B/A"));
  TEST_CHECK(HasPrim(stage, "/World/B/A/child"));
  {
    auto child = stage.GetPrimAtPath(Path("/World/B/A/child", ""));
    TEST_CHECK(child && child.value());
    if (child && child.value()) {
      TEST_CHECK(child.value()->absolute_path().full_path_name() ==
                 "/World/B/A/child");
    }
  }

  // Reparent to root with a rename.
  std::string err2;
  TEST_CHECK_(stage.ReparentPrim(Path("/World/B/A", ""), Path("/", ""), "Moved",
                                 &err2),
              "reparent-to-root failed: %s", err2.c_str());
  TEST_CHECK(!HasPrim(stage, "/World/B/A"));
  TEST_CHECK(HasPrim(stage, "/Moved"));
  TEST_CHECK(HasPrim(stage, "/Moved/child"));
  {
    auto child = stage.GetPrimAtPath(Path("/Moved/child", ""));
    TEST_CHECK(child && child.value());
    if (child && child.value()) {
      TEST_CHECK(child.value()->absolute_path().full_path_name() ==
                 "/Moved/child");
    }
  }
}

void namespace_edit_errors_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kHierarchy, &stage));
  std::string err;

  // Renaming the root path is invalid.
  TEST_CHECK(!stage.RenamePrim(Path("/", ""), "x", &err));
  // Non-existent prim.
  TEST_CHECK(!stage.RenamePrim(Path("/World/Nope", ""), "x", &err));
  // Name clash with an existing sibling.
  TEST_CHECK(!stage.RenamePrim(Path("/World/A", ""), "B", &err));
  // Reparent under itself / a descendant is rejected.
  TEST_CHECK(!stage.ReparentPrim(Path("/World/A", ""), Path("/World/A/child", ""),
                                 "", &err));
  // Removing a non-existent prim fails.
  TEST_CHECK(!stage.RemovePrim(Path("/World/Nope", ""), &err));

  // The stage is unchanged after the failed edits.
  TEST_CHECK(HasPrim(stage, "/World/A"));
  TEST_CHECK(HasPrim(stage, "/World/A/child"));
  TEST_CHECK(HasPrim(stage, "/World/B"));
}
