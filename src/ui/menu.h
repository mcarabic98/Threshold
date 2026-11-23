#pragma once
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

#include "modules/module.h"

namespace ui {

extern ImFont* g_iconFont; // Segoe MDL2 Assets, set by the renderer

struct MenuState {
  bool attached = false;
  bool jvmOk = false;
  uint32_t pid = 0;
  std::string statusLine;
  std::string windowTitle;
  std::string processPath;
  std::string jdkVersion;
  std::string mappingsLine;
  std::string jvmModuleLine;
  size_t vmStructEntries = 0;
  std::vector<std::pair<std::string, std::string>> keyOffsets;

  // heap
  bool heapOk = false;
  bool heapG1 = false;
  int heapSizeMode = 0;
  std::string heapLine;
  size_t mcObjects = 0;
  std::vector<std::pair<std::string, size_t>> targetHits;

  // player
  bool playerValid = false;
  std::string playerLine;

  std::function<void()> onRescan;
  std::function<void()> onDump;
  std::function<void()> onDumpFields;
};

// Metro menu driven by the module registry: tiles come from the registry,
// the right panel renders each module's settings by type.
class Menu {
public:
  void draw(MenuState& st, mods::ModuleRegistry& reg);
  ImFont* iconFont = nullptr; // Segoe MDL2 Assets, set by renderer

private:
  void drawHeader(MenuState& st);
  void drawNav(MenuState& st);
  void drawGrid();
  void drawPanel(MenuState& st);
  void drawTile(mods::Module& m, ImVec2 pos, ImVec2 size, bool selected);
  void drawToggleRow(const char* label, bool* val, float tx, float* y);
  void drawSliderRow(const char* label, float* val, float mn, float mx, const char* fmt,
                     float tx, float* y, float cw);
  void drawEnumRow(mods::Setting& s, float tx, float* y, float cw);
  void drawKeyRow(mods::Setting& s, float tx, float* y, float cw);
  void pollKeyCapture(); // bind capture by async key scan: works without focus

  mods::ModuleRegistry* reg_ = nullptr; // set in draw(); edits mark it dirty
  std::string selModuleId_ = "esp";
  std::string selCategory_ = "Все";
  mods::Setting* listenTarget_ = nullptr; // setting being rebound, null = idle
  bool keySnap_[256] = {};                // key-down state at capture start
  int lastDrawFrame_ = -2;                // ImGui frame of the previous draw()
};

} // namespace ui
