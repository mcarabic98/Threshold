#include "app/app.h"

#include <spdlog/spdlog.h>
#include <imgui_impl_win32.h>

#include <format>
#include <memory>
#include <set>
#include <thread>

#include <imgui.h>

#include "core/util.h"
#include "modules/esp.h"
#include "modules/fov.h"
#include "modules/hud.h"
#include "modules/radar.h"
#include "modules/tracers.h"
#include "ui/theme.h"

// Declared under '#if 0' in imgui_impl_win32.h by design — forward-declare here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace app {

namespace {
constexpr int HK_MENU = 1;
constexpr int HK_QUIT = 2;
}

int App::run(const config::Config& cfg) {
  cfg_ = cfg;
  buildRegistry();
  mods_.applyJson(cfg_.modules); // restore enabled state + settings + binds

  // Fabric intermediary names (1.21.x); MCP/Forge names come later via mappings.
  targets_ = {
      "net/minecraft/class_310",  // Minecraft / MinecraftClient
      "net/minecraft/class_746",  // LocalPlayer
      "net/minecraft/class_742",  // AbstractClientPlayer
      "net/minecraft/class_1657", // Player
      "net/minecraft/class_1297", // Entity
  };

  if (!ov_.create(GetModuleHandleW(nullptr), L"Threshold overlay")) return 1;

  ov_.hook_ = [this](HWND h, UINT m, WPARAM wp, LPARAM lp, bool& handled) -> LRESULT {
    if (m == WM_HOTKEY) {
      if (wp == HK_MENU) {
        menuOpen_ = !menuOpen_;
        ov_.setClickThrough(!menuOpen_);
        handled = true;
        return 0;
      }
      if (wp == HK_QUIT) {
        quit_ = true;
        handled = true;
        return 0;
      }
    }
    handled = ImGui_ImplWin32_WndProcHandler(h, m, wp, lp) != 0;
    return 0;
  };

  if (!rend_.init(ov_.hwnd(), 640, 400)) {
    spdlog::error("renderer init failed");
    ov_.destroy();
    return 1;
  }

  // Visible from the start: status panel while scanning; snaps to MC on attach.
  menuOpen_ = true;
  ov_.setClickThrough(false);
  ov_.show(true);

  RegisterHotKey(ov_.hwnd(), HK_MENU, MOD_NOREPEAT, cfg.menuKey);
  RegisterHotKey(ov_.hwnd(), HK_QUIT, MOD_NOREPEAT, cfg.exitKey);
  spdlog::info("overlay ready. INSERT - menu, END - quit");

  MSG msg = {};
  while (!quit_) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit_ = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_DESTROY) quit_ = true;
    }
    if (quit_) break;

    if (attached_) {
      if (!targetAlive()) {
        spdlog::warn("Minecraft window/process lost - detaching");
        detach();
        continue;
      }
      pollTarget();
    } else {
      auto now = std::chrono::steady_clock::now();
      if (now - lastScan_ > std::chrono::milliseconds(500)) {
        lastScan_ = now;
        tryAttach();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    frame();
  }

  detach();
  syncConfig(true); // flush pending module edits on the way out
  UnregisterHotKey(ov_.hwnd(), HK_MENU);
  UnregisterHotKey(ov_.hwnd(), HK_QUIT);
  rend_.shutdown();
  ov_.destroy();
  return 0;
}

void App::tryAttach() {
  auto cands = mc::findCandidates();
  size_t javaCount = 0;
  for (auto& c : cands)
    if (c.hasJvm && c.is64Bit) ++javaCount;
  spdlog::debug("scan: {} visible windows, {} java candidates", cands.size(), javaCount);

  auto best = mc::pickBest(cands);
  // THRESHOLD_PID=<pid> pins the target when several Java games are running
  // (testing multi-version support without closing the other instances).
  char pin[32]{};
  if (GetEnvironmentVariableA("THRESHOLD_PID", pin, sizeof(pin)) > 0) {
    DWORD want = (DWORD)strtoul(pin, nullptr, 10);
    for (auto& c : cands)
      if (c.hasJvm && c.pid == want) {
        best = c;
        break;
      }
  }
  if (!best) return;
  target_ = *best;

  // Read-only attach by design; writes are opt-in per feature later.
  if (!proc_.attach(best->pid, /*withWrite=*/false)) return;
  if (!jvm_.resolve(proc_)) {
    spdlog::error("attached to pid {} but jvm.dll analysis failed", best->pid);
    detach();
    return;
  }
  heap_.resolve(proc_, jvm_);

  // Mappings for the game version (window title -> version+loader -> table).
  std::string version = mc::versionFromTitle(best->title);
  std::string loader = mc::loaderFromTitle(best->title);
  model_.setGameVersion(version);
  mapping_ = mc::loadMappings(config::appDir() / L"mappings", version, loader);
  targets_.clear();
  {
    std::set<std::string> s;
    for (auto& [k, n] : mapping_.classes) s.insert(n);
    // Probe classes whose InstanceKlass* is needed later (zoom state etc.) —
    // the scan records their klass address by name (see HeapScanResult).
    // ok_zoomer classes: hosts for the CP-walk + the Zoom state itself.
    for (const char* probe : {
             "page/langeweile/ok_zoomer/OkZoomerClientMod",
             "page/langeweile/ok_zoomer/utils/ZoomUtils",
             "page/langeweile/ok_zoomer/utils/ForgeZoomUtils",
             "page/langeweile/ok_zoomer/events/ManageZoomEvent",
             "page/langeweile/ok_zoomer/zoom/Zoom",
         })
      s.insert(probe);
    targets_.assign(s.begin(), s.end());
  }

  attached_ = true;
  menuOpen_ = false;
  ov_.show(true);
  ov_.setClickThrough(true);
  spdlog::info("attached: pid={} window='{}' mappings='{}' ({} targets)", best->pid,
               util::toUtf8(best->title),
               mapping_.builtin ? "builtin " + mapping_.version : mapping_.version,
               targets_.size());
  startScan();
}

void App::startScan() {
  if (scanState_.load() == 1) return;
  pollScan();
  scanCancel_ = false;
  {
    wchar_t t[256]{};
    GetWindowTextW(target_.hwnd, t, 256);
    lastScanTitle_ = t;
  }
  scanState_ = 1;
  scanThread_ = std::thread([this] {
    lastHeap_ = heap_.scan(proc_, jvm_, targets_, &scanCancel_);
    if (lastHeap_.ok)
      spdlog::info("heap scan: {} objects in {} regions ({:.1f}s), net/minecraft/*: {}, "
                   "target hits: {}",
                   lastHeap_.objectsSeen, lastHeap_.regionsScanned, lastHeap_.seconds,
                   lastHeap_.mcObjects, lastHeap_.targetCounts.size());
    else
      spdlog::warn("heap scan failed: {}", lastHeap_.error);

    // Locate the Minecraft class: instance object -> narrow klass id ->
    // decoded InstanceKlass address; then init the navigation model. The
    // sequential walk occasionally misses the singleton entirely (skipped
    // region tag, resync gap) — recover it from GameRenderer's back-reference.
    auto mcName = mapping_.className("minecraft");
    bool haveMc = mcName && !lastHeap_.targetAddrs[*mcName].empty();
    if (!haveMc && mcName) haveMc = recoverMcSingleton();
    if (haveMc) {
      initModelFromScan();
    } else if (mcName) {
      spdlog::warn("mc model: no '{}' instances found in scan", *mcName);
    } else {
      spdlog::warn("mc model: mappings define no 'minecraft' class");
    }
    lastScanEnd_ = std::chrono::steady_clock::now();
    scanState_ = 2;
  });
}

void App::pollScan() {
  if (scanState_.load() == 2 && scanThread_.joinable()) {
    scanThread_.join();
    scanState_ = 0;
  }
}

bool App::initModelFromScan() {
  auto mcName = mapping_.className("minecraft");
  if (!mcName) return false;
  auto& addrs = lastHeap_.targetAddrs[*mcName];
  if (addrs.empty()) return false;

  const jvm::JvmEnv& env = jvm_.env();
  uint64_t obj = addrs.front();
  if (env.compressedKlasses) {
    auto id = proc_.read<uint32_t>(obj + 8);
    if (!id) return false;
    mcKlass_ = env.narrowKlassBase + ((uint64_t)*id << env.narrowKlassShift);
  } else {
    auto k = proc_.read<uint64_t>(obj + 8);
    if (!k) return false;
    mcKlass_ = *k;
  }
  spdlog::info("mc model: {} instance @ {}, klass @ {}", *mcName, util::hex(obj),
               util::hex(mcKlass_));
  bool ok = model_.init(proc_, jvm_, klassReader_, lastHeap_, heap_.heapStart(), heap_.heapEnd(),
                        mapping_);
  if (ok) autoRescans_ = 0;
  return ok;
}

// The sequential walk can miss the Minecraft singleton entirely: its region
// may fall outside the tag filter, or the walk desyncs around it. Missing one
// or two singleton objects does not justify a slower walk — instead recover
// the instance from objects the walk does find reliably: GameRenderer holds
// the only field typed L<class_310>; back-reference.
bool App::recoverMcSingleton() {
  auto mcName = mapping_.className("minecraft");
  if (!mcName) return false;
  if (!lastHeap_.targetAddrs[*mcName].empty()) return true;
  auto grName = mapping_.className("gameRenderer");
  if (!grName) return false;
  auto& grs = lastHeap_.targetAddrs[*grName];
  if (grs.empty()) {
    spdlog::warn("mc model: recovery anchor '{}' not found in scan either", *grName);
    return false;
  }

  const jvm::JvmEnv& env = jvm_.env();
  const std::string mcDesc = "L" + *mcName + ";";
  uint64_t heapLo = heap_.heapStart(), heapHi = heap_.heapEnd();

  for (size_t gi = 0; gi < grs.size() && gi < 4; ++gi) {
    uint64_t gr = grs[gi];

    // GameRenderer klass -> field table -> the oop field typed as Minecraft.
    uint64_t grKlass = 0;
    if (env.compressedKlasses) {
      auto k = proc_.read<uint32_t>(gr + 8);
      if (!k) continue;
      grKlass = env.narrowKlassBase + ((uint64_t)*k << env.narrowKlassShift);
    } else {
      auto k = proc_.read<uint64_t>(gr + 8);
      if (!k) continue;
      grKlass = *k;
    }
    std::vector<jvm::FieldDesc> fs;
    std::string err;
    if (!klassReader_.readFields(proc_, jvm_, grKlass, fs, &err)) {
      spdlog::debug("mc model: recovery: fields of GameRenderer klass {} failed: {}",
                    util::hex(grKlass), err);
      continue;
    }
    const jvm::FieldDesc* ref = nullptr;
    for (auto& f : fs)
      if (!f.isStatic && f.descriptor == mcDesc) ref = &f;
    if (!ref) continue;
    spdlog::info("mc model: recovery: '{}' instance @ {} holds back-ref '{}' at +{}",
                 *grName, util::hex(gr), ref->name, ref->offset);

    // The heap-oop decode may not be settled yet — try the standard
    // hypotheses and validate each candidate by decoding its klass.
    uint64_t rawVal = 0;
    if (env.compressedOops) {
      if (auto r = proc_.read<uint32_t>(gr + ref->offset)) rawVal = *r;
    } else {
      if (auto r = proc_.read<uint64_t>(gr + ref->offset)) rawVal = *r;
    }
    if (rawVal == 0) {
      spdlog::debug("mc model: recovery: back-ref oop is null at {}+{} (stale instance?)",
                    util::hex(gr), ref->offset);
      continue;
    }

    std::vector<std::pair<uint64_t, int>> hypos;
    if (env.compressedOops) {
      if (env.narrowOopBase != 0 || env.narrowOopShift != 0)
        hypos.emplace_back(env.narrowOopBase, env.narrowOopShift);
      hypos.emplace_back(0, 3);
      if (heapLo) {
        hypos.emplace_back(heapLo, 0);
        hypos.emplace_back(heapLo, 3);
      }
    } else {
      hypos.emplace_back(0, 0); // uncompressed oops need no decode
    }

    for (auto& [base, shift] : hypos) {
      uint64_t addr = env.compressedOops ? base + (rawVal << shift) : rawVal;
      if (heapLo && (addr < heapLo || addr >= heapHi)) {
        spdlog::debug("mc model: recovery: base=0x{:X} shift={} -> {} outside heap range",
                      base, shift, util::hex(addr));
        continue;
      }

      auto mark = proc_.read<uint64_t>(addr);
      auto kid = proc_.read<uint32_t>(addr + 8);
      if (!kid || !*kid) {
        spdlog::debug("mc model: recovery: base=0x{:X} shift={} -> {} unreadable or kid=0",
                      base, shift, util::hex(addr));
        continue;
      }
      uint64_t k2 = env.compressedKlasses
                        ? env.narrowKlassBase + ((uint64_t)*kid << env.narrowKlassShift)
                        : *kid;
      std::string name;
      if (auto lh = proc_.read<uint32_t>(k2 + env.offKlassLayoutHelper); lh && *lh) {
        if (env.offKlassName >= 0) {
          if (auto sym = proc_.read<uint64_t>(k2 + env.offKlassName); sym && *sym)
            if (auto n = jvm_.readSymbol(proc_, *sym)) name = *n;
        }
        // Fallback when the klass-name path comes up empty: fingerprint the
        // Minecraft class by its unique LocalPlayer-typed field from the
        // mapping rules.
        if (name.empty()) {
          std::vector<jvm::FieldDesc> mfs;
          std::string merr;
          auto playerRule = mapping_.field("player");
          auto plName = mapping_.className("localPlayer");
          if (playerRule && plName &&
              klassReader_.readFields(proc_, jvm_, k2, mfs, &merr)) {
            std::string pdesc = "L" + *plName + ";";
            for (auto& f : mfs)
              if (!f.isStatic && f.name == playerRule->name && f.descriptor == pdesc) {
                name = *mcName; // fingerprint match
                break;
              }
          }
        }
      }

      spdlog::info("mc model: recovery: candidate @ {} (base=0x{:X} shift={}) mark=0x{:X} "
                   "kid=0x{:X} klass @ {} name='{}' | {}",
                   util::hex(addr), base, shift, mark ? *mark : 0, *kid, util::hex(k2), name,
                   heap_.regionSummary(proc_, jvm_, addr));
      if (name == *mcName) {
        lastHeap_.targetAddrs[*mcName].push_back(addr);
        spdlog::info("mc model: singleton '{}' recovered via '{}' back-reference", *mcName,
                     *grName);
        return true;
      }
    }
  }
  spdlog::warn("mc model: recovery via '{}' back-reference found no valid instance",
               *grName);
  return false;
}

// Runs every frame while attached. The full heap scan is slow, so a missing
// model is first attacked cheaply: re-run recovery + model init (field tables
// are cached) — that alone covers menu -> world transitions. A fresh scan is
// only started when the window title moved on (loading finished, world
// entered) or after a bounded idle period.
void App::ensureModel() {
  if (!attached_ || !jvm_.ok() || !lastHeap_.ok) return;
  if (scanState_.load() != 0) return; // a scan owns lastHeap_ / model_
  auto mcName = mapping_.className("minecraft");
  if (!mcName) return;

  auto now = std::chrono::steady_clock::now();
  bool haveMc = !lastHeap_.targetAddrs[*mcName].empty();

  if (!haveMc) {
    if (now - lastRecoverTry_ >= std::chrono::seconds(3)) {
      lastRecoverTry_ = now;
      haveMc = recoverMcSingleton();
    }
    if (!haveMc) {
      wchar_t t[256]{};
      GetWindowTextW(target_.hwnd, t, 256);
      std::wstring cur(t);
      bool titleChanged = !cur.empty() && cur != lastScanTitle_;
      bool idleTooLong = now - lastScanEnd_ > std::chrono::seconds(60);
      if (autoRescans_ < 5 &&
          ((titleChanged && now - lastScanEnd_ > std::chrono::seconds(5)) || idleTooLong)) {
        if (titleChanged)
          spdlog::info("heap: window title changed ('{}' -> '{}') - rescanning",
                       util::toUtf8(lastScanTitle_), util::toUtf8(cur));
        else
          spdlog::info("heap: target still missing after {} auto-rescans - rescanning",
                       autoRescans_);
        ++autoRescans_;
        startScan();
      }
      return;
    }
  }

  if (!model_.valid() && now - lastModelTry_ >= std::chrono::seconds(3)) {
    lastModelTry_ = now;
    initModelFromScan();
  }
}

void App::dumpMcFields() {
  if (!mcKlass_) {
    spdlog::warn("dump fields: no class_310 klass yet (scan first)");
    return;
  }
  klassReader_.dumpClass(proc_, jvm_, mcKlass_, "Minecraft");

  // java/lang/String: tiny class with known fields — cracks the fi_stream
  // format trivially.
  if (auto arr = jvm_.vmClassesArray()) {
    spdlog::debug("vmClasses array @ {}", util::hex(*arr));
    for (int i = 0; i < 200; ++i) {
      auto k = proc_.read<uint64_t>((uintptr_t)(*arr + (uint64_t)i * 8));
      if (!k || !*k || *k < 0x10000) continue;
      const jvm::JvmEnv& env = jvm_.env();
      auto sym = proc_.read<uint64_t>((uintptr_t)(*k + env.offKlassName));
      if (!sym || !*sym) continue;
      auto name = jvm_.readSymbol(proc_, *sym);
      if (!name) continue;
      if (i < 30) spdlog::debug("vmClass[{}] = '{}' @ {}", i, *name, util::hex(*k));
      if (*name == "java/lang/Thread") {
        klassReader_.dumpClass(proc_, jvm_, *k, "java/lang/Thread");
        break;
      }
    }
  }
}

void App::detach() {
  if (attached_) {
    spdlog::info("detached");
    attached_ = false;
  }
  scanCancel_ = true;
  if (scanThread_.joinable()) scanThread_.join();
  scanState_ = 0;
  ov_.show(false);
  proc_.detach();
  jvm_.reset();
  heap_ = {};
  model_ = {};
  mapping_ = {};
  target_ = {};
  lastScan_ = std::chrono::steady_clock::now();
}

bool App::targetAlive() {
  if (!IsWindow(target_.hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(target_.hwnd, &pid);
  return pid == target_.pid && proc_.valid();
}

void App::pollTarget() {
  RECT rc = {};
  if (!GetClientRect(target_.hwnd, &rc)) return;
  POINT tl = {0, 0};
  ClientToScreen(target_.hwnd, &tl);
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;
  ov_.setBounds(tl.x, tl.y, w, h);
  rend_.resize(w, h);
}

void App::frame() {
  pollScan(); // join finished background scan before touching its results
  fillMenuState();
  ensureModel(); // cheap recovery / init retry while the model is missing

  // module context: one snapshot of everything a module may read this frame
  mods::ModuleContext ctx;
  ctx.proc = &proc_;
  ctx.model = &model_;
  ctx.registry = &mods_;
  ctx.attached = attached_;
  ctx.modelValid = attached_ && model_.valid();
  ctx.dt = ImGui::GetIO().DeltaTime;
  ctx.playerValid = lastPsValid_;
  ctx.player = lastPs_;
  if (ctx.modelValid) {
    mc::CameraSnapshot cam;
    if (model_.readCamera(proc_, cam)) ctx.camera = cam;
  }
  if (attached_) {
    RECT rc = {};
    POINT tl = {0, 0};
    if (GetClientRect(target_.hwnd, &rc) && ClientToScreen(target_.hwnd, &tl)) {
      ctx.winPos = ImVec2((float)tl.x, (float)tl.y);
      ctx.winSize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    }
  }

  // Update FOV provider first so other modules see the current effective FOV.
  if (auto* fovMod = mods::FovModule::get(mods_)) {
    fovMod->update(ctx);
    ctx.fovDeg = fovMod->fovDeg();
  }

  rend_.beginFrame();
  mods_.updateEnabled(ctx); // inside the frame: render modules draw here
  if (menuOpen_) menu_.draw(menuState_, mods_);
  rend_.endFrame();

  syncConfig(false); // debounced: only writes once edits settle
}

void App::buildRegistry() {
  mods_.add(std::make_unique<mods::FovModule>());
  mods_.add(std::make_unique<mods::EspModule>());
  mods_.add(std::make_unique<mods::RadarModule>());
  mods_.add(std::make_unique<mods::TracersModule>());
  mods_.add(std::make_unique<mods::Module>("aim", "Aimbot", 0xE7B3, ui::C_GREEN, "Combat",
                                     "Плавное наведение на ближайшую цель.", false, false, 'R'));
  mods_.add(std::make_unique<mods::Module>("hitbox", "Хитбоксы", 0xE7A8, ui::C_TEAL, "Combat",
                                     "Показ хитбоксов существ в мире.", false, false, VK_F3));
  mods_.add(std::make_unique<mods::Module>("bright", "Яркость", 0xE706, ui::C_YELLOW, "Визуал",
                                     "Полная яркость в тёмных местах.", false, false, 'B'));
  mods_.add(std::make_unique<mods::Module>("zoom", "Зум", 0xE721, ui::C_BLUE, "Визуал",
                                     "Приближение камеры по клавише.", false, false, 'C'));
  mods_.add(std::make_unique<mods::FpsModule>());
  mods_.add(std::make_unique<mods::CoordsModule>());
  mods_.add(std::make_unique<mods::Module>("keys", "Клавиши", 0xE765, ui::C_DARKRED, "HUD",
                                     "Нажатия WASD и мыши.", false, false));
  mods_.add(std::make_unique<mods::Module>("ping", "Пинг", 0xE8BD, ui::C_TEAL, "HUD",
                                     "Задержка до сервера.", false, false));
}

void App::syncConfig(bool force) {
  if (mods_.changeVersion() == savedCfgVersion_) return; // nothing to save
  if (!force && std::chrono::steady_clock::now() - mods_.lastChange() < std::chrono::seconds(2))
    return; // wait for the user to stop dragging
  cfg_.modules = mods_.toJson();
  if (config::save(cfg_)) savedCfgVersion_ = mods_.changeVersion();
}

void App::fillMenuState() {
  auto& st = menuState_;
  st.attached = attached_;
  st.jvmOk = jvm_.ok();
  st.pid = target_.pid;
  st.windowTitle = util::toUtf8(target_.title);
  st.processPath = util::abbreviatePath(util::toUtf8(target_.processPath), 48);
  st.jdkVersion = jvm_.version().raw.empty() ? "unknown" : jvm_.version().raw;
  st.mappingsLine = mapping_.builtin ? mapping_.version + " (builtin)" : mapping_.version;
  st.vmStructEntries = jvm_.structs().count();
  st.jvmModuleLine = jvm_.ok()
                         ? util::hex(jvm_.module().base) + " / " +
                               std::to_string(jvm_.module().size / (1024 * 1024)) + " MB"
                         : "-";
  st.statusLine = attached_ ? (jvm_.ok() ? (scanState_.load() == 1
                                               ? "attached - scanning heap..."
                                               : "attached - jvm resolved")
                                         : "attached - jvm unresolved")
                            : "searching for Minecraft...";

  st.keyOffsets.clear();
  auto addField = [&](const char* t, const char* f) {
    if (auto v = jvm_.structs().field(t, f))
      st.keyOffsets.push_back({std::string(t) + "::" + f, std::format("0x{:X}", (int64_t)*v)});
  };
  auto addStatic = [&](const char* t, const char* f) {
    if (auto v = jvm_.structs().staticDatum(t, f))
      st.keyOffsets.push_back({std::string(t) + "::" + f + " @", util::hex(*v)});
  };
  addField("oopDesc", "_mark");
  addField("Klass", "_layout_helper");
  addField("InstanceKlass", "_fields");
  addField("Symbol", "_length");
  addField("Symbol", "_body");
  addStatic("Universe", "_collectedHeap");
  addStatic("Universe", "_narrow_oop_base");

  // heap
  // player (also feeds the module context this frame)
  mc::EntitySnapshot es;
  if (attached_ && model_.valid() && model_.readPlayer(proc_, es) && es.valid) {
    lastPs_ = es;
    lastPsValid_ = true;
    st.playerValid = true;
    st.playerLine = std::format("x {:.2f}  y {:.2f}  z {:.2f}   yaw {:.1f}  pitch {:.1f}", es.x,
                                es.y, es.z, es.yaw, es.pitch);
  } else {
    lastPsValid_ = false;
    st.playerValid = attached_ && model_.valid();
    st.playerLine = es.error.empty() ? "-" : es.error;
  }

  // lastHeap_ is written by the scan thread while scanState_ == 1 — skip the
  // reads there instead of racing the result assignment.
  if (scanState_.load() != 1) {
    st.heapOk = heap_.ready() && lastHeap_.ok;
    st.heapG1 = heap_.g1();
    st.heapSizeMode = heap_.calibratedMode();
    st.heapLine = lastHeap_.ok
                      ? std::format("{} regions, {} objects, {:.1f}s", lastHeap_.regionsScanned,
                                    lastHeap_.objectsSeen, lastHeap_.seconds)
                      : (lastHeap_.error.empty() ? "-" : lastHeap_.error);
    st.mcObjects = lastHeap_.mcObjects;
    st.targetHits.assign(lastHeap_.targetCounts.begin(), lastHeap_.targetCounts.end());
  }
  st.onRescan = [this]() { startScan(); };
  st.onDump = [this]() {
    for (const char* f : {"G1CollectedHeap", "HeapRegion", "GrowableArray", "CollectedHeap",
                          "MemRegion", "oopDesc", "arrayOop", "Klass", "Universe"})
      jvm_.dumpMatches(f);
  };
  st.onDumpFields = [this]() { dumpMcFields(); };
}

} // namespace app
