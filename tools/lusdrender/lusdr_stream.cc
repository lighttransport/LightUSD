// SPDX-License-Identifier: Apache-2.0
// lusdrender — WebSocket browser streaming server. Keeps the scene + BVH
// resident, renders the camera to memory on demand (reusing RenderImage),
// encodes via the shared image writer and pushes frames to browsers over a
// WebSocket. The viewer has header/left/right control panes around a canvas;
// the mouse orbits/pans/dollies the camera and the browser can open a
// server-side path or upload a USD file. Adaptive quality: a small low-quality
// JPEG is rendered while the view moves, then one full-resolution lossless frame
// (PNG/QOI) once it goes stable. Compiles to nothing unless LUSDRENDER_WITH_STREAM.
#ifdef LUSDRENDER_WITH_STREAM

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "external/civetweb/civetweb.h"
#include "external/jsonhpp/nlohmann/json.hpp"
#include "image-writer.hh"
#include "lusdr_context.hh"

namespace lusdr {

namespace {
using json = nlohmann::json;
constexpr double kPi = 3.14159265358979323846;

// Browser viewer: header bar + left (Scene) / right (View) HTML control panes
// around a center canvas. The canvas draws streamed frames preserving aspect and
// orbits/pans/dollies the camera; the panes open files and pick the idle codec.
const char *kViewerHtml = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<title>lusdrender stream</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:#15171c;color:#cdd3df;font:13px system-ui,sans-serif}
body{display:grid;grid-template-rows:42px 1fr;grid-template-columns:210px 1fr 210px;
 grid-template-areas:"head head head" "left center right";height:100vh}
#head{grid-area:head;display:flex;align-items:center;gap:10px;padding:0 12px;background:#1d2026;border-bottom:1px solid #2c313b}
#head b{color:#7fb0ff;font-size:14px}
#head input{flex:0 1 360px;background:#0f1115;border:1px solid #2c313b;color:#cdd3df;padding:4px 6px;border-radius:4px}
#stat{margin-left:auto;color:#7d8595;font:12px monospace}
.pane{background:#1a1d23;overflow:auto;padding:10px}
#left{grid-area:left;border-right:1px solid #2c313b}
#right{grid-area:right;border-left:1px solid #2c313b}
.pane h3{margin:4px 0 8px;font-size:11px;text-transform:uppercase;letter-spacing:.5px;color:#7d8595}
button,select{width:100%;margin:3px 0;background:#262b34;border:1px solid #353c48;color:#cdd3df;padding:6px;border-radius:4px;cursor:pointer}
button:hover{background:#2f3540}
.row{display:flex;gap:6px}.row button{width:auto;flex:1}
#drop{border:1px dashed #3a4150;border-radius:6px;padding:14px;text-align:center;color:#7d8595;margin:6px 0}
#drop.hot{border-color:#7fb0ff;color:#7fb0ff}
#center{grid-area:center;position:relative;background:#0c0d10;overflow:hidden}
#c{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;cursor:grab;background:#0c0d10}
small{color:#6b7280}
</style></head><body>
<div id="head"><b>lusdrender</b>
 <input id="path" placeholder="server-side USD path, then Enter">
 <span id="stat">connecting…</span></div>

<div id="left" class="pane">
 <h3>Scene</h3>
 <button id="openbtn">Open path</button>
 <div id="drop">Drop .usd/.usdz here<br><small>or click to upload</small>
  <input id="file" type="file" accept=".usd,.usda,.usdc,.usdz" style="display:none"></div>
 <h3>Render</h3>
 <label><small>Idle refinement</small></label>
 <select id="codec"><option value="png">PNG (lossless)</option>
  <option value="qoi">QOI (lossless)</option></select>
 <small>Moving: fast low-res JPEG. Stable: one crisp lossless frame.</small>
</div>

<div id="center"><canvas id="c"></canvas></div>

<div id="right" class="pane">
 <h3>View</h3>
 <button data-k="f">Reset / fit (F)</button>
 <h3>Presets</h3>
 <div class="row"><button data-k="1">Front</button><button data-k="3">Right</button></div>
 <div class="row"><button data-k="7">Top</button><button data-k="0">Iso</button></div>
 <h3>Help</h3>
 <small>Left-drag orbit · Shift+drag or middle-drag pan · right-drag / wheel dolly.</small>
</div>

<script>
const cv=document.getElementById('c'),cx=cv.getContext('2d'),stat=document.getElementById('s')||document.getElementById('stat');
const ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/stream');
ws.binaryType='blob';
let frames=0,last=performance.now();
function sendSize(){const r=document.getElementById('center').getBoundingClientRect();
 send({t:'resize',w:Math.round(r.width),h:Math.round(r.height)});}
ws.onopen=()=>{stat.textContent='connected';sendSize();};
ws.onclose=()=>stat.textContent='disconnected';
ws.onmessage=ev=>{createImageBitmap(ev.data).then(b=>{
  if(cv.width!==b.width||cv.height!==b.height){cv.width=b.width;cv.height=b.height;}
  cx.drawImage(b,0,0); b.close(); frames++;
  const now=performance.now(); if(now-last>1000){stat.textContent=frames+' fps · '+cv.width+'×'+cv.height;frames=0;last=now;}
});};
function send(o){ if(ws.readyState===1) ws.send(JSON.stringify(o)); }
let drag=false,btn=0,px=0,py=0;
cv.addEventListener('contextmenu',e=>e.preventDefault());
cv.addEventListener('mousedown',e=>{drag=true;btn=e.button;px=e.clientX;py=e.clientY;cv.style.cursor='grabbing';});
window.addEventListener('mouseup',()=>{drag=false;cv.style.cursor='grab';});
window.addEventListener('mousemove',e=>{ if(!drag)return; const dx=e.clientX-px,dy=e.clientY-py; px=e.clientX;py=e.clientY;
  if(btn===1||(btn===0&&e.shiftKey)) send({t:'pan',dx:dx,dy:dy});
  else if(btn===2) send({t:'dolly',amount:(dx-dy)*0.05});
  else send({t:'orbit',dx:dx,dy:dy}); });
cv.addEventListener('wheel',e=>{e.preventDefault(); send({t:'dolly',amount:(e.deltaY>0?-1:1)});},{passive:false});
window.addEventListener('keydown',e=>{ if(e.target.tagName==='INPUT')return;
  if('f01357'.includes(e.key)) send({t:'key',k:e.key}); });
document.querySelectorAll('[data-k]').forEach(b=>b.onclick=()=>send({t:'key',k:b.dataset.k}));
document.getElementById('codec').onchange=e=>send({t:'codec',v:e.target.value});
const path=document.getElementById('path');
function openPath(){ if(path.value.trim()) send({t:'load',path:path.value.trim()}); }
document.getElementById('openbtn').onclick=openPath;
path.addEventListener('keydown',e=>{ if(e.key==='Enter') openPath(); });
const drop=document.getElementById('drop'),file=document.getElementById('file');
drop.onclick=()=>file.click();
file.onchange=()=>{ if(file.files[0]) upload(file.files[0]); };
['dragenter','dragover'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add('hot');}));
['dragleave','drop'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove('hot');}));
drop.addEventListener('drop',e=>{ if(e.dataTransfer.files[0]) upload(e.dataTransfer.files[0]); });
async function upload(f){ stat.textContent='uploading '+f.name+'…';
  try{ const r=await fetch('/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
       const j=await r.json(); stat.textContent=j.ok?('loaded '+j.path):('upload failed: '+(j.error||'')); }
  catch(err){ stat.textContent='upload error'; } }
let rt; window.addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(sendSize,200);});
</script></body></html>)HTML";

// A browser command parsed from a WebSocket text frame.
struct Nav {
  enum Type { Orbit, Pan, Dolly, Key, Resize, Load, Codec } type;
  float dx = 0, dy = 0, amount = 0;
  std::string str;  // key / load path / codec name
  int w = 0, h = 0;
};

std::string SafeBasename(const std::string &name) {
  size_t slash = name.find_last_of("/\\");
  std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
  if (base.empty() || base == "." || base == "..") base = "upload.usdz";
  return base;
}
}  // namespace

// Orbit-camera streaming server: one civetweb context, a /stream WebSocket and
// the embedded viewer at /. The render loop runs on the calling (main) thread so
// RenderImage + BVH access stay single-threaded; civetweb worker threads only
// enqueue input and register/unregister live connections.
class StreamRenderServer {
 public:
  explicit StreamRenderServer(const Options &opt)
      : opt_(opt),
        idle_codec_(opt.stream_codec == "qoi" ? "qoi" : "png"),
        motion_max_(std::max(16, opt.stream_motion_res)),
        motion_q_(std::min(100, std::max(1, opt.stream_motion_quality))),
        idle_ms_(std::max(0, opt.stream_idle_ms)) {}

  // Build (or rebuild) the resident scene/BVH from `path`. Returns false on
  // failure (the previous scene, if any, is kept).
  bool load(const std::string &path) {
    Options o = opt_;
    o.input = path;
    auto nrc = std::make_unique<RenderContext>();
    if (!BuildRenderContext(o, *nrc)) {
      std::cerr << "stream: failed to load " << path << "\n";
      return false;
    }
    const bool first = !rc_;
    rc_ = std::move(nrc);
    opt_.input = path;
    // Keep the browser's requested resolution across reloads; only adopt the
    // scene default on the very first load (before any resize arrives).
    if (first) {
      full_w_ = rc_->width;
      full_h_ = rc_->height;
    }
    seedOrbitFromBounds();
    applyCamera();
    return true;
  }

  bool start(int port) {
    const std::string ports = std::to_string(port);
    const char *options[] = {"listening_ports", ports.c_str(), "num_threads",
                             "4", nullptr};
    ctx_web_ = mg_start(nullptr, this, options);
    if (!ctx_web_) {
      std::cerr << "lusdrender stream: failed to start HTTP server on port "
                << port << "\n";
      return false;
    }
    mg_set_websocket_handler(ctx_web_, "/stream", wsConnect, wsReady, wsData,
                             wsClose, this);
    mg_set_request_handler(ctx_web_, "/upload", uploadHandler, this);
    mg_set_request_handler(ctx_web_, "/", httpHandler, this);
    std::cerr << "lusdrender stream server listening on http://localhost:"
              << port << "/ (WebSocket /stream)\n";
    return true;
  }

  // Block on the render loop until stopped. Renders a small low-quality JPEG when
  // the view changes (motion), then one full-resolution lossless frame once
  // stable. The latest encoded frame is cached and re-sent for a short window
  // after each render / new connection (beats civetweb's connect/read race).
  void runLoop() {
    using clock = std::chrono::steady_clock;
    for (;;) {
      std::vector<Nav> batch;
      bool motion = false, resend = false;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(120), [&] {
          return stop_ || (!conns_.empty() && (dirty_ || resend_ > 0));
        });
        if (stop_) return;
        batch.swap(input_);
        motion = dirty_ && !conns_.empty();
        if (motion) dirty_ = false;
        resend = !motion && resend_ > 0 && !conns_.empty();
      }
      for (const Nav &n : batch) applyNav(n);

      const auto now = clock::now();
      if (motion) {
        last_activity_ = now;
        need_refine_ = true;
        renderAndPush(/*motion=*/true);
      } else if (need_refine_ && !conns_.empty() &&
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - last_activity_).count() >= idle_ms_) {
        need_refine_ = false;
        renderAndPush(/*motion=*/false);  // full-res lossless refine
      } else if (resend) {
        pushLast();
        std::lock_guard<std::mutex> lk(mu_);
        if (resend_ > 0) --resend_;
      }
    }
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (ctx_web_) {
      mg_stop(ctx_web_);
      ctx_web_ = nullptr;
    }
  }

 private:
  static StreamRenderServer *self(void *u) {
    return static_cast<StreamRenderServer *>(u);
  }

  // ---- civetweb callbacks (run on worker threads) ----
  static int wsConnect(const mg_connection *, void *) { return 0; }

  static void wsReady(mg_connection *c, void *u) {
    StreamRenderServer *s = self(u);
    std::lock_guard<std::mutex> lk(s->mu_);
    s->conns_.push_back(c);
    if (s->last_frame_.empty()) s->dirty_ = true;  // first client: initial frame
    s->resend_ = kResendN;
    s->cv_.notify_all();
  }

  static void wsClose(const mg_connection *c, void *u) {
    StreamRenderServer *s = self(u);
    std::lock_guard<std::mutex> lk(s->mu_);
    s->conns_.erase(std::remove(s->conns_.begin(), s->conns_.end(),
                                const_cast<mg_connection *>(c)),
                    s->conns_.end());
  }

  static int wsData(mg_connection *, int bits, char *data, size_t len, void *u) {
    const int opcode = bits & 0xf;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
    if (opcode != MG_WEBSOCKET_OPCODE_TEXT || !data || len == 0) return 1;
    json j = json::parse(std::string(data, len), nullptr, /*exceptions=*/false);
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string()) return 1;
    const std::string t = j["t"].get<std::string>();
    auto num = [&](const char *k) -> float {
      return (j.contains(k) && j[k].is_number()) ? j[k].get<float>() : 0.f;
    };
    auto str = [&](const char *k) -> std::string {
      return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : "";
    };
    Nav n;
    if (t == "orbit") {
      n.type = Nav::Orbit; n.dx = num("dx"); n.dy = num("dy");
    } else if (t == "pan") {
      n.type = Nav::Pan; n.dx = num("dx"); n.dy = num("dy");
    } else if (t == "dolly") {
      n.type = Nav::Dolly; n.amount = num("amount");
    } else if (t == "key") {
      n.type = Nav::Key; n.str = str("k");
    } else if (t == "resize") {
      n.type = Nav::Resize; n.w = int(num("w")); n.h = int(num("h"));
    } else if (t == "load") {
      n.type = Nav::Load; n.str = str("path");
      if (n.str.empty()) return 1;
    } else if (t == "codec") {
      n.type = Nav::Codec; n.str = str("v");
    } else {
      return 1;
    }
    self(u)->enqueue(std::move(n));
    return 1;
  }

  static int httpHandler(mg_connection *conn, void *) {
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
              "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s",
              int(std::strlen(kViewerHtml)), kViewerHtml);
    return 200;
  }

  // POST /upload?name=foo.usdz — body is the raw file. Saves under the system
  // temp dir and queues a Load.
  static int uploadHandler(mg_connection *conn, void *u) {
    StreamRenderServer *s = self(u);
    const mg_request_info *ri = mg_get_request_info(conn);
    auto reply = [&](bool ok, const std::string &msg) {
      json r = ok ? json{{"ok", true}, {"path", msg}}
                  : json{{"ok", false}, {"error", msg}};
      const std::string body = r.dump();
      mg_printf(conn,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s",
                int(body.size()), body.c_str());
      return 200;
    };
    if (!ri || std::strcmp(ri->request_method, "POST") != 0)
      return reply(false, "POST required");
    char nameBuf[512] = "upload.usdz";
    if (ri->query_string)
      mg_get_var(ri->query_string, std::strlen(ri->query_string), "name",
                 nameBuf, sizeof(nameBuf));
    std::error_code ec;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "lusdrender_uploads";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path out = dir / SafeBasename(nameBuf);
    std::ofstream ofs(out, std::ios::binary | std::ios::trunc);
    if (!ofs) return reply(false, "cannot write temp file");
    char buf[65536];
    int rd;
    long long total = 0;
    while ((rd = mg_read(conn, buf, sizeof(buf))) > 0) {
      ofs.write(buf, rd);
      total += rd;
    }
    ofs.close();
    if (total <= 0) return reply(false, "empty upload");
    Nav n;
    n.type = Nav::Load;
    n.str = out.string();
    s->enqueue(std::move(n));
    return reply(true, out.string());
  }

  void enqueue(Nav n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (input_.size() < 4096) input_.push_back(std::move(n));
    dirty_ = true;
    cv_.notify_all();
  }

  // ---- camera / render (run on the loop thread) ----
  void seedOrbitFromBounds() {
    const Vec3 ext = Sub(rc_->bounds.hi, rc_->bounds.lo);
    radius_ = std::max(1.0e-3f, Length(ext) * 0.5f);
    center_ = Mul(Add(rc_->bounds.lo, rc_->bounds.hi), 0.5f);
    az_ = 30.0; el_ = 20.0; dist_scale_ = 2.5; pan_ = Vec3{0, 0, 0};
  }

  // Position the camera on the orbit sphere around center_ (+ pan offset).
  void applyCamera() {
    if (!rc_) return;
    const double az = az_ * kPi / 180.0;
    const double el = std::max(-89.0, std::min(89.0, el_)) * kPi / 180.0;
    const double dist = double(radius_) * dist_scale_;
    const Vec3 up = AxisVec(rc_->up_axis);
    const Vec3 ref = (std::fabs(up.y) < 0.9f) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 e0 = Normalize(Cross(up, ref));
    const Vec3 e1 = Normalize(Cross(up, e0));
    const float ce = float(std::cos(el)), se = float(std::sin(el));
    const Vec3 horiz = Add(Mul(e0, float(std::cos(az)) * ce),
                           Mul(e1, float(std::sin(az)) * ce));
    const Vec3 dir = Add(horiz, Mul(up, se));
    const Vec3 target = Add(center_, pan_);
    const Vec3 eye = Add(target, Mul(dir, float(dist)));
    CameraFrame &c = rc_->camera;
    c.origin = eye;
    c.forward = Normalize(Sub(target, eye));
    c.right = Normalize(Cross(c.forward, up));
    if (Length(c.right) < 1.0e-6f) c.right = Vec3{1, 0, 0};
    c.up = Normalize(Cross(c.right, c.forward));
    c.ortho = false;
    c.znear = std::max(1.0e-4f, float(dist) * 1.0e-3f);
    c.zfar = float(dist) + radius_ * 4.0f;
  }

  void applyNav(const Nav &n) {
    switch (n.type) {
      case Nav::Orbit:
        az_ += n.dx * 0.4;
        el_ = std::max(-89.0, std::min(89.0, el_ + n.dy * 0.4));
        applyCamera();
        break;
      case Nav::Pan: {
        const float s = radius_ * 0.0025f;
        pan_ = Add(pan_, Add(Mul(rc_->camera.right, -n.dx * s),
                             Mul(rc_->camera.up, n.dy * s)));
        applyCamera();
        break;
      }
      case Nav::Dolly:
        dist_scale_ *= std::exp(-double(n.amount) * 0.1);
        dist_scale_ = std::max(1.0e-3, std::min(1.0e4, dist_scale_));
        applyCamera();
        break;
      case Nav::Key: {
        const std::string &k = n.str;
        if (k == "f") { seedOrbitFromBounds(); }
        else if (k == "1") { az_ = 0;  el_ = 0; }     // Front
        else if (k == "3") { az_ = 90; el_ = 0; }     // Right
        else if (k == "7") { el_ = 89; }              // Top
        else if (k == "0") { az_ = 45; el_ = 30; }    // Iso
        applyCamera();
        break;
      }
      case Nav::Resize:
        if (n.w > 0 && n.h > 0) { full_w_ = n.w; full_h_ = n.h; }
        break;
      case Nav::Load:
        load(n.str);  // keeps old scene on failure
        break;
      case Nav::Codec:
        if (n.str == "png" || n.str == "qoi") idle_codec_ = n.str;
        break;
    }
  }

  void renderAndPush(bool motion) {
    if (!rc_) return;
    int w = full_w_, h = full_h_;
    if (motion && std::max(w, h) > motion_max_) {
      const double s = double(motion_max_) / double(std::max(w, h));
      w = std::max(1, int(w * s));
      h = std::max(1, int(h * s));
    }
    rc_->opt.width = w;
    lightusd::Image img = RenderImage(
        rc_->scene, &rc_->direct, rc_->tris, rc_->flat_mats, rc_->lights,
        rc_->ibl.valid ? &rc_->ibl : nullptr, rc_->camera, rc_->opt, h,
        rc_->textures.empty() ? nullptr : &rc_->textures,
        rc_->tri_uvs.empty() ? nullptr : &rc_->tri_uvs,
        /*tri_uvs1*/ nullptr,
        rc_->use_tlas ? rc_->tlas : nullptr,
        rc_->use_tlas ? &rc_->blas : nullptr,
        rc_->use_tlas ? &rc_->instances : nullptr,
        rc_->tri_colors.empty() ? nullptr : &rc_->tri_colors,
        rc_->tri_normals.empty() ? nullptr : &rc_->tri_normals,
        rc_->volumes.empty() ? nullptr : &rc_->volumes,
        rc_->flat_openpbr_mats.empty() ? nullptr : &rc_->flat_openpbr_mats,
        rc_->triangle_chunks.empty() ? nullptr : &rc_->triangle_chunks,
        rc_->backplates.empty() ? nullptr : &rc_->backplates,
        rc_->mtlx_geomprop_requests.empty() ? nullptr : &rc_->mtlx_geomprop_requests,
        rc_->tri_geomprops.empty() ? nullptr : &rc_->tri_geomprops,
        rc_->tri_geomprops_valid.empty() ? nullptr : &rc_->tri_geomprops_valid);

    lightusd::image::WriteOption wopt;
    if (motion) {
      wopt.format = lightusd::image::WriteImageFormat::JPEG;
      wopt.jpeg_quality = motion_q_;
    } else if (idle_codec_ == "qoi") {
      wopt.format = lightusd::image::WriteImageFormat::QOI;
    } else {
      wopt.format = lightusd::image::WriteImageFormat::PNG;
    }
    auto enc = lightusd::image::WriteImageToMemory(img, wopt);
    if (!enc) {
      std::cerr << "stream: encode failed: " << enc.error() << "\n";
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      last_frame_ = std::move(enc.value());
      // Motion frames are superseded by the next one, so a dropped frame self-heals;
      // the crisp idle refine must land, so re-send it for a short window.
      if (!motion) resend_ = kResendN;
    }
    pushLast();
  }

  void pushLast() {
    std::lock_guard<std::mutex> lk(mu_);
    if (last_frame_.empty()) return;
    for (mg_connection *c : conns_) {
      mg_lock_connection(c);
      mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY,
                         reinterpret_cast<const char *>(last_frame_.data()),
                         last_frame_.size());
      mg_unlock_connection(c);
    }
  }

  static constexpr int kResendN = 4;   // ticks to re-send the crisp/connect frame

  Options opt_;
  std::unique_ptr<RenderContext> rc_;
  std::string idle_codec_;
  int motion_max_, motion_q_, idle_ms_;
  int full_w_ = 960, full_h_ = 540;

  mg_context *ctx_web_ = nullptr;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<mg_connection *> conns_;
  std::vector<Nav> input_;
  std::vector<uint8_t> last_frame_;
  int resend_ = 0;
  bool dirty_ = true;
  bool stop_ = false;
  bool need_refine_ = false;
  std::chrono::steady_clock::time_point last_activity_{};

  // Orbit state.
  Vec3 center_{0, 0, 0};
  Vec3 pan_{0, 0, 0};
  float radius_ = 1.0f;
  double az_ = 30.0, el_ = 20.0, dist_scale_ = 2.5;
};

int RunStreamServer(const Options &opt) {
  StreamRenderServer server(opt);
  if (!server.load(opt.input)) return EXIT_FAILURE;
  if (opt.stats) {
    // PrintRTStats needs the context; load() built it internally, so skip the
    // one-shot stats dump here (the per-frame path stays quiet).
  }
  if (!server.start(opt.stream_http)) return EXIT_FAILURE;
  std::cerr << "Press Ctrl-C to stop.\n";
  server.runLoop();
  server.stop();
  return EXIT_SUCCESS;
}

}  // namespace lusdr
#endif  // LUSDRENDER_WITH_STREAM
