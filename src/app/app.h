#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "jvm/heap_reader.h"
#include "jvm/jvm_runtime.h"
#include "jvm/klass_reader.h"
#include "mc/finder.h"
#include "mc/game_model.h"
#include "mc/mappings.h"
#include "memory/process.h"
#include "overlay/overlay_window.h"
#include "overlay/renderer.h"
#include "ui/menu.h"

namespace app {

class App {
public:
  int run(const config::Config& cfg);

private:
  void tryAttach();
  void detach();
  bool targetAlive();
  void pollTarget();
  void frame();
  void fillMenuState();
  void startScan(); // background heap scan + model init
  void pollScan();  // join finished scan thread
  void dumpMcFields();
  bool recoverMcSingleton(); // splice missed singleton via back-references
  bool initModelFromScan();  // mc instance -> klass -> GameModel init
  void ensureModel();        // cheap per-frame retry while the model is invalid

  // game state
  mem::Process proc_;
  jvm::JvmRuntime jvm_;
  jvm::HeapReader heap_;
  jvm::HeapScanResult lastHeap_;
  jvm::KlassReader klassReader_;
  mc::GameModel model_;
  mc::MappingSet mapping_;
  uint64_t mcKlass_ = 0; // InstanceKlass* of the Minecraft class
  std::vector<std::string> targets_;
  mc::McCandidate target_{};
  bool attached_ = false;
  bool menuOpen_ = false;
  bool quit_ = false;
  std::chrono::steady_clock::time_point lastScan_{};
  std::chrono::steady_clock::time_point lastScanEnd_{};
  std::chrono::steady_clock::time_point lastModelTry_{};
  std::chrono::steady_clock::time_point lastRecoverTry_{};
  std::wstring lastScanTitle_;
  int autoRescans_ = 0; // bounded auto rescans since the last model init

  // background scan
  std::thread scanThread_;
  std::atomic<int> scanState_{0}; // 0 idle, 1 running, 2 finished
  std::atomic<bool> scanCancel_{false};

  // overlay
  overlay::OverlayWindow win_;
  overlay::Renderer rend_;
  ui::Menu menu_;
  ui::MenuState menuState_;
  bool espEnabled_ = true;
  std::chrono::steady_clock::time_point lastPosChange_{};
  mc::EntitySnapshot lastPlayer_{};
  bool hasLastPlayer_ = false;
  std::chrono::steady_clock::time_point lastFovRead_{};
  float cachedFov_ = 70.f;
  void drawEsp();
};

} // namespace app
