// SPDX-License-Identifier: Apache-2.0
#include "compare_panel.hh"
#include "renderer.hh"
#include "image-loader.hh"
#include "imgui.h"
#include "external/jsonhpp/nlohmann/json.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

namespace lusdview {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Images { std::array<light3d::Image,4> data; std::string error; };
Images ReadImages(const fs::path& directory) {
  Images result;
  const char* names[] = {"reference.png", "candidate.png", "diff.png", "heatmap.png"};
  for (size_t slot=0; slot<4; ++slot) {
    auto path=directory/names[slot];
    std::error_code ec;
    auto size=fs::file_size(path,ec);
    if (ec || size>64*1024*1024) { result.error="Missing or oversized comparison image"; return result; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream input(path,std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) {
      result.error="Cannot read comparison image"; return result;
    }
    constexpr size_t budget=256*1024*1024;
    auto info=lightusd::image::GetImageInfoFromMemoryBounded(bytes.data(),bytes.size(),path.string(),budget);
    if (!info || !info->width || !info->height || info->width>4096 || info->height>4096) {
      result.error="Preview requires images up to 4096 x 4096; use the saved report for larger captures"; return result;
    }
    auto loaded=lightusd::image::LoadImageFromMemoryBounded(bytes.data(),bytes.size(),path.string(),budget);
    if (!loaded) { result.error=loaded.error(); return result; }
    const auto& source=loaded->image;
    if (source.bpp!=8 || source.format!=lightusd::Image::PixelFormat::UInt || source.channels<1 || source.channels>4 ||
        source.width!=static_cast<int>(info->width) || source.height!=static_cast<int>(info->height) ||
        source.data.size()!=static_cast<size_t>(source.width)*source.height*source.channels) {
      result.error="Unsupported comparison pixel format"; return result;
    }
    auto& image=result.data[slot];
    image.width=source.width; image.height=source.height; image.channels=4;
    image.data.resize(static_cast<size_t>(image.width)*image.height*4);
    for (size_t i=0;i<image.data.size()/4;++i) {
      const auto* p=source.data.data()+i*static_cast<size_t>(source.channels);
      auto* q=image.data.data()+i*4;
      q[0]=p[0]; q[1]=source.channels<3?p[0]:p[1]; q[2]=source.channels<3?p[0]:p[2];
      q[3]=(source.channels==2)?p[1]:(source.channels==4?p[3]:255);
    }
  }
  return result;
}
Json ReadJson(const fs::path& path) {
  std::error_code ec;
  if (fs::file_size(path,ec)>1024*1024 || ec) return Json();
  std::ifstream input(path);
  return Json::parse(input,nullptr,false);
}
}
struct ComparePanel::State {
  bool visible{false};
  fs::path root;
  Json status;
  std::string capture, error, pending;
  std::future<Images> loading;
  Images images;
  unsigned long long generation{~0ull}, sequence{0};
  bool upload{false}, previewReady{false};
  int mode{0}; float mix{.5f};
  std::array<char,16384> edits{};
  std::string response;
  std::chrono::steady_clock::time_point last{};
  void command(const char* tool, Json arguments=Json::object()) {
    if (root.empty() || !pending.empty()) return;
    auto tick=std::chrono::steady_clock::now().time_since_epoch().count();
    pending=std::to_string(tick)+"-"+std::to_string(++sequence);
    auto base=root/"control-requests"/pending;
    std::ofstream out(base.string()+".pending.json");
    out << Json{{"tool",tool},{"arguments",arguments}}.dump(); out.close();
    std::error_code ec;
    fs::rename(base.string()+".pending.json",base.string()+".request.json",ec);
    if (ec || !out) { error="Cannot submit controller command"; pending.clear(); }
  }
};
ComparePanel::ComparePanel():state_(new State) {
  const char* root=std::getenv("LUSDVIEW_CONTROL_DIR");
  if (root && *root) state_->root=root;
}
ComparePanel::~ComparePanel()=default;
void ComparePanel::open(int mode) { state_->visible=true; if (mode>=0 && mode<5) state_->mode=mode; }
void ComparePanel::draw(Renderer* renderer, unsigned long long generation, bool threaded, const char* focusedPath) {
  auto& s=*state_;
  if (!s.visible) return;
  ImGui::SetNextWindowSize(ImVec2(880,620),ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Compare viewers",&s.visible)) { ImGui::End(); return; }
  if (s.root.empty()) {
    ImGui::TextWrapped("Launch both viewers with tools/view_control/control.py launch to enable linked comparison.");
    ImGui::End(); return;
  }
  auto now=std::chrono::steady_clock::now();
  if (now-s.last>std::chrono::milliseconds(500)) {
    s.last=now;
    auto status=ReadJson(s.root/"control-state.json");
    if (status.is_object()) s.status=std::move(status);
    if (!s.pending.empty()) {
      auto response=ReadJson(s.root/"control-requests"/(s.pending+".response.json"));
      if (response.is_object()) {
        s.error=response.value("ok",false)?"":response.value("error",std::string("Controller error"));
        s.response=response.dump(2);
        std::error_code ec;
        fs::remove(s.root/"control-requests"/(s.pending+".response.json"),ec);
        s.pending.clear();
      }
    }
    if (s.status.is_object() && s.status.contains("capture") && s.status["capture"].is_object()) {
      auto id=s.status["capture"].value("directory",std::string());
      // Only controller-owned immediate capture directories are accepted.
      if (!id.empty() && fs::path(id).filename()==fs::path(id) && id!=s.capture && !s.loading.valid()) {
        s.capture=id; s.error.clear(); s.previewReady=false;
        s.loading=std::async(std::launch::async,ReadImages,s.root/id);
      }
    }
  }
  if (s.loading.valid() && s.loading.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
    s.images=s.loading.get(); s.error=s.images.error; s.upload=s.error.empty(); s.previewReady=false;
    if (!s.error.empty()) s.images.data={};
  }
  if (!s.pending.empty()) ImGui::TextUnformatted("Controller operation running...");
  ImGui::BeginDisabled(!s.pending.empty());
  if (ImGui::Button("Capture pair")) s.command("capture");
  ImGui::SameLine(); if (ImGui::Button("Sync from lusdview")) s.command("sync",{{"enabled",true},{"source","lusdview"}});
  ImGui::SameLine(); if (ImGui::Button("Sync from usdview")) s.command("sync",{{"enabled",true},{"source","usdview"}});
  if (ImGui::Button("Play")) s.command("playback",{{"action","play"}});
  ImGui::SameLine(); if (ImGui::Button("Pause")) s.command("playback",{{"action","pause"}});
  for (const char* action : {"undo","redo","clear"}) {
    ImGui::SameLine(); if (ImGui::Button(action)) s.command("overlay",{{"action",action}});
  }
  ImGui::SameLine(); if (ImGui::Button("Export overlay")) s.command("overlay",{{"action","export"}});
  if (ImGui::CollapsingHeader("Picked prim and debug edits")) {
    ImGui::TextWrapped("Picked: %s",focusedPath && *focusedPath?focusedPath:"(none)");
    if (ImGui::Button("Inspect picked in both") && focusedPath && *focusedPath)
      s.command("inspect_pair",{{"path",focusedPath},{"limit",64}});
    ImGui::InputTextMultiline("Edit batch JSON",s.edits.data(),s.edits.size(),ImVec2(-1,100));
    if (ImGui::Button("Apply shared overlay")) {
      auto edits=Json::parse(s.edits.data(),nullptr,false);
      if (!edits.is_array() || edits.empty() || edits.size()>128) s.error="Enter a JSON array of 1..128 edits; see tools/view_control/README.md";
      else s.command("overlay",{{"action","edit"},{"edits",edits}});
    }
  }
  ImGui::EndDisabled();
  if (s.status.is_object()) {
    if (s.status.contains("error") && s.status["error"].is_string()) ImGui::TextWrapped("%s",s.status["error"].get<std::string>().c_str());
    if (s.status.contains("control_url") && s.status["control_url"].is_string()) ImGui::TextWrapped("Dashboard: %s",s.status["control_url"].get<std::string>().c_str());
  }
  if (!s.error.empty()) ImGui::TextWrapped("%s",s.error.c_str());
  if (!s.response.empty() && ImGui::CollapsingHeader("Last controller result")) {
    ImGui::BeginChild("Controller result",ImVec2(0,160),ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(s.response.c_str()); ImGui::EndChild();
  }
  if (threaded) { ImGui::TextWrapped("Comparison previews require the controller's single-threaded launch profile."); ImGui::End(); return; }
  if ((s.upload || s.generation!=generation) && s.images.data[0].isValid()) {
    bool ready=true;
    for (int slot=0;slot<4;++slot) ready=renderer->comparisonImage(slot,&s.images.data[static_cast<size_t>(slot)])!=0 && ready;
    s.upload=false; s.previewReady=ready; s.generation=generation;
    if (!ready) s.error="Comparison texture upload failed";
  }
  ImGui::Combo("Mode",&s.mode,"Side by side\0Overlay\0Wipe\0Difference\0Heatmap\0");
  if (s.mode==1 || s.mode==2) ImGui::SliderFloat("Blend / wipe",&s.mix,0,1);
  if (s.images.data[0].isValid() && s.generation==generation && s.previewReady) {
    const auto& image=s.images.data[0];
    float width=ImGui::GetContentRegionAvail().x;
    if (s.mode==0) width=(width-ImGui::GetStyle().ItemSpacing.x)*.5f;
    float height=width*static_cast<float>(image.height)/static_cast<float>(image.width);
    const float availableHeight=std::max(32.0f,ImGui::GetContentRegionAvail().y-32.0f);
    if (height>availableHeight) { width*=availableHeight/height; height=availableHeight; }
    auto texture=[&](int slot) { return static_cast<ImTextureID>(renderer->comparisonImage(slot)); };
    if (s.mode==0) {
      ImGui::Image(texture(0),ImVec2(width,height)); ImGui::SameLine(); ImGui::Image(texture(1),ImVec2(width,height));
    } else if (s.mode>=3) ImGui::Image(texture(s.mode-1),ImVec2(width,height));
    else {
      ImVec2 p=ImGui::GetCursorScreenPos(), end(p.x+width,p.y+height);
      auto* draw=ImGui::GetWindowDrawList();
      draw->AddImage(texture(0),p,end);
      if (s.mode==2) draw->PushClipRect(p,ImVec2(p.x+width*s.mix,end.y),true);
      draw->AddImage(texture(1),p,end,ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,s.mode==1?static_cast<int>(s.mix*255):255));
      if (s.mode==2) draw->PopClipRect();
      ImGui::Dummy(ImVec2(width,height));
    }
    ImGui::TextUnformatted("Left/base: usdview. Right/top: lusdview. Preview displays saved capture pixels.");
  }
  ImGui::End();
}
}
