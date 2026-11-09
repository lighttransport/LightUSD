// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include "lightusd-cpp.hh"
#include "vchar_control_map.hh"
#include "lightusd-session-cpp.hh"
#include <chrono>
#include <filesystem>
#include <fstream>

using lightusd::api::Stage;

static Stage Load(const char* source) {
  lightusd_stage* handle = nullptr;
  assert(lightusd_stage_load_from_memory(reinterpret_cast<const uint8_t*>(source),
                                         std::strlen(source), nullptr, &handle) == LIGHTUSD_OK);
  return Stage(handle);
}

int main() {
  auto stage = Load(R"(#usda 1.0
def Xform "Root" {
  def Xform "Face" (
    customData = {
      dictionary vchar = {
        string[] controlNames = ["smile", "blink"]
        token[] controlMappings = ["mouthSmile"]
        float2[] controlRanges = [(2, -1)]
        float[] controlDefaults = [0.25]
      }
    }
  ) {}
}
def Xform "Later" (
  customData = { dictionary vchar = { token[] controlNames = ["ignored"] } }
) {}
)");
  auto controls = lusdview::ReadVcharControls(stage.get());
  assert(lightusd_prim_is_valid(lightusd_stage_prim_at_path(stage.get(), "/Root/Face")));
  assert(controls.size() == 2);
  assert(controls[0].name == "smile" && controls[0].blendshape == "mouthSmile");
  assert(controls[0].minimum == -1 && controls[0].maximum == 2 && controls[0].defaultValue == 0.25f);
  assert(controls[1].name == "blink" && controls[1].blendshape == "blink");
  assert(controls[1].minimum == -1 && controls[1].maximum == 1 && controls[1].defaultValue == 0);
  lightusd_dict_ref data{}, group{};
  assert(lightusd_prim_custom_data(lightusd_stage_prim_at_path(stage.get(), "/Root/Face"), &data) == LIGHTUSD_OK);
  assert(lightusd_dict_find(data, "vchar", nullptr, nullptr, &group) == LIGHTUSD_OK);
  lightusd::api::StringList names;
  assert(lightusd::api::DictionaryView(group).token_array("controlNames", &names) == LIGHTUSD_OK);
  lightusd_strlist* failed = nullptr;
  assert(lightusd_dict_get_token_array(group, "absent", &failed) == LIGHTUSD_ERR_NOT_FOUND);
  assert(!failed);
  assert(lightusd_dict_get_token_array(group, "controlDefaults", &failed) == LIGHTUSD_ERR_TYPE_MISMATCH);
  assert(!failed);
  assert(lightusd_dict_get_token_array({}, "controlNames", &failed) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_dict_get_token_array(group, nullptr, &failed) == LIGHTUSD_ERR_INVALID_ARG);
  assert(lightusd_dict_get_token_array(group, "controlNames", nullptr) == LIGHTUSD_ERR_INVALID_ARG);
  stage = Stage();
  assert(lightusd::api::StringListSize(names) == 2);
  const auto name = lightusd::api::StringListGet(names, 1);
  assert(name.len == 5 && std::memcmp(name.data, "blink", 5) == 0);

  // A scalar float array must not be reinterpreted as pairs and read past its end.
  auto malformed = Load(R"(#usda 1.0
def Xform "Face" (
  customData = { dictionary vchar = {
    token[] controlNames = ["a", "b"]
    float[] controlRanges = [99]
    double[] controlDefaults = [0.75]
  } }
) {}
)");
  controls = lusdview::ReadVcharControls(malformed.get());
  assert(controls.size() == 2);
  for (const auto& control : controls)
    assert(control.minimum == -1 && control.maximum == 1 && control.defaultValue == 0);
  assert(lusdview::ReadVcharControls(static_cast<const lightusd_stage*>(nullptr)).empty());
  auto empty = Load("#usda 1.0\ndef Xform \"Empty\" {}\n");
  assert(lusdview::ReadVcharControls(empty.get()).empty());

  // Public document views retain read-only data after the session is destroyed.
  const auto directory = std::filesystem::temp_directory_path() /
      ("lusdview-vchar-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto file = directory / "root.usda";
  {
    std::ofstream output(file);
    output << R"(#usda 1.0
def Xform "Face" (
  customData = { dictionary vchar = { token[] controlNames = ["retained"] } }
) {}
)";
  }
  Stage retained;
  {
    lightusd::api::DocumentSession document;
    assert(document.create() == LIGHTUSD_OK);
    lightusd::api::DocumentSnapshot snapshot;
    assert(document.open_file(file.string().c_str(), &snapshot) == LIGHTUSD_OK);
    assert(lightusd::api::DocumentSnapshotStage(snapshot, &retained) == LIGHTUSD_OK);
    assert(lightusd_stage_is_read_only(retained.get()));
  }
  controls = lusdview::ReadVcharControls(retained.get());
  assert(controls.size() == 1 && controls[0].name == "retained");
  std::filesystem::remove_all(directory);
}
