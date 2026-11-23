#pragma once
#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "mc/game_model.h"
#include "memory/process.h"

namespace mods {

// Per-frame world state handed to every module. Owned by App; valid for the
// duration of update() only.
struct ModuleContext {
  mem::Process* proc = nullptr;
  mc::GameModel* model = nullptr;
  bool attached = false;
  bool modelValid = false;
  float dt = 0.f;          // frame delta, seconds
  ImVec2 winPos{0, 0};     // screen-space origin of the game client area
  ImVec2 winSize{0, 0};
  bool playerValid = false; // player snapshot, read by App once per frame
  mc::EntitySnapshot player;
};

// One user-tunable module option. The menu renders a row per type; id is the
// key under config.json -> modules -> <moduleId> -> settings.
struct Setting {
  enum class Type { Bool, Float, Enum, Key };
  Type type = Type::Bool;
  std::string id;
  std::string label;
  bool b = false;                   // Bool
  float f = 0.f;                    // Float
  float min = 0.f, max = 100.f;     // Float
  const char* fmt = "%.0f";         // Float
  std::vector<const char*> options; // Enum
  int index = 0;                    // Enum (index into options)
  int vk = 0;                       // Key (virtual-key code, 0 = unbound)
};

std::string vkName(int vk); // display name of a virtual-key code

// A module: tile metadata + settings + a per-frame update. The base class is a
// complete metadata-only placeholder; subclasses override update().
class Module {
public:
  Module(const char* id, const char* name, unsigned glyph, ImU32 color, const char* category,
         const char* desc, bool wide, bool defEnabled, int defKey = 0);
  virtual ~Module() = default;

  const std::string& id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::string& category() const { return category_; }
  const std::string& desc() const { return desc_; }
  unsigned glyph() const { return glyph_; }
  ImU32 color() const { return color_; }
  bool wide() const { return wide_; }

  bool enabled = false;

  // live tile counter ("12 ent") and panel rows, refreshed by update()
  std::string live;
  std::vector<std::pair<std::string, std::string>> liveRows;

  Setting& addBool(const char* id, const char* label, bool def);
  Setting& addFloat(const char* id, const char* label, float def, float mn, float mx,
                    const char* fmt);
  Setting& addEnum(const char* id, const char* label, std::vector<const char*> options,
                   int defIndex);
  Setting& addKey(const char* id, const char* label, int defVk);
  Setting* setting(const std::string& id);
  const std::vector<Setting>& settings() const { return settings_; }
  std::vector<Setting>& settings() { return settings_; }

  virtual void update(const ModuleContext& ctx) {}

protected:
  bool boolVal(const char* id, bool def = false) const;
  float floatVal(const char* id, float def) const;
  int intVal(const char* id, int def) const; // Enum index / Key vk

private:
  std::string id_, name_, category_, desc_;
  unsigned glyph_ = 0;
  ImU32 color_ = 0;
  bool wide_ = false;
  std::vector<Setting> settings_;
};

class ModuleRegistry {
public:
  Module& add(std::unique_ptr<Module> m);
  Module* find(const std::string& id);
  const std::vector<std::unique_ptr<Module>>& all() const { return mods_; }

  // Tick enabled modules; disabled ones get their live data cleared.
  void updateEnabled(const ModuleContext& ctx);

  // Persistence: {"<moduleId>": {"enabled": bool, "settings": {id: value}}}
  nlohmann::json toJson() const;
  void applyJson(const nlohmann::json& j);

  // Bumped on every user edit; drives debounced config saves.
  void markChanged();
  uint64_t changeVersion() const { return changeVersion_; }
  std::chrono::steady_clock::time_point lastChange() const { return lastChange_; }

private:
  std::vector<std::unique_ptr<Module>> mods_;
  uint64_t changeVersion_ = 0;
  std::chrono::steady_clock::time_point lastChange_{};
};

} // namespace mods
