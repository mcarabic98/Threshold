#pragma once
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

#include "jvm/heap_reader.h"
#include "jvm/jvm_runtime.h"
#include "mc/game_model.h"
#include "mc/mappings.h"
#include "memory/process.h"

namespace ui {

extern ImFont* g_iconFont; // Segoe MDL2 Assets, set by the renderer

// Metro-style module definition (tile in the grid).
struct ModuleDef {
  std::string id;
  std::string name;
  unsigned iconGlyph; // Segoe MDL2 Assets codepoint
  ImU32 color;
  std::string category;
  std::string desc;
  bool enabled = false;
  bool wide = false;
  std::string key;
  std::string live;
};

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

  // esp
  bool espEnabled = true;
  size_t espEntities = 0;
  std::string espError;
  float espFov = 70.f;
  bool espFovOverridden = false;
  float espDistance = 128.f;
  bool espShowMobs = true;
  bool espShowPlayers = true;
  bool espShowItems = false;
  bool espShowMisc = false;
  float espBoxOpacity = 100.f;

  std::function<void()> onRescan;
  std::function<void()> onDump;
  std::function<void()> onDumpFields;
};

class Menu {
public:
  void draw(MenuState& st);
  ImFont* iconFont = nullptr; // Segoe MDL2 Assets, set by renderer

private:
  void drawHeader(MenuState& st);
  void drawNav(MenuState& st);
  void drawGrid(MenuState& st);
  void drawPanel(MenuState& st);
  void drawTile(ModuleDef& m, ImVec2 pos, ImVec2 size, bool selected);
  void drawSwitch(ImVec2 pos, bool on);
  void drawToggleRow(const char* label, bool* val, float tx, float* y);
  void drawSliderRow(const char* label, float* val, float mn, float mx, const char* fmt,
                     float tx, float* y, float cw);
  void drawTextRow(const char* label, const char* val, float tx, float* y);
  std::string selModuleId_ = "esp";
  std::string selCategory_ = "Все";
  bool listenKey_ = false;
};

} // namespace ui
