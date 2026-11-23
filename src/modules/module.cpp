#include "modules/module.h"

#include <algorithm>
#include <format>

namespace mods {

std::string vkName(int vk) {
  if (!vk) return "—";
  if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
  if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
  if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::format("Num{}", vk - VK_NUMPAD0);
  static const std::pair<int, const char*> named[] = {
      {VK_SPACE, "Space"},    {VK_SHIFT, "Shift"},      {VK_CONTROL, "Ctrl"},
      {VK_MENU, "Alt"},       {VK_TAB, "Tab"},          {VK_CAPITAL, "Caps"},
      {VK_RETURN, "Enter"},   {VK_BACK, "Backspace"},   {VK_ESCAPE, "Esc"},
      {VK_INSERT, "Insert"},  {VK_DELETE, "Delete"},    {VK_HOME, "Home"},
      {VK_END, "End"},        {VK_PRIOR, "PageUp"},     {VK_NEXT, "PageDown"},
      {VK_LEFT, "Left"},      {VK_RIGHT, "Right"},      {VK_UP, "Up"},
      {VK_DOWN, "Down"},      {VK_LBUTTON, "LMB"},      {VK_RBUTTON, "RMB"},
      {VK_MBUTTON, "MMB"},    {VK_XBUTTON1, "Mouse4"},  {VK_XBUTTON2, "Mouse5"},
  };
  for (auto& [k, n] : named)
    if (k == vk) return n;
  return std::format("0x{:02X}", vk);
}

Module::Module(const char* id, const char* name, unsigned glyph, ImU32 color,
               const char* category, const char* desc, bool wide, bool defEnabled, int defKey)
    : id_(id), name_(name), category_(category), desc_(desc), glyph_(glyph), color_(color),
      wide_(wide) {
  enabled = defEnabled;
  addKey("key", "Клавиша", defKey);
}

Setting& Module::addBool(const char* id, const char* label, bool def) {
  Setting s;
  s.type = Setting::Type::Bool;
  s.id = id;
  s.label = label;
  s.b = def;
  settings_.push_back(std::move(s));
  return settings_.back();
}

Setting& Module::addFloat(const char* id, const char* label, float def, float mn, float mx,
                          const char* fmt) {
  Setting s;
  s.type = Setting::Type::Float;
  s.id = id;
  s.label = label;
  s.f = def;
  s.min = mn;
  s.max = mx;
  s.fmt = fmt;
  settings_.push_back(std::move(s));
  return settings_.back();
}

Setting& Module::addEnum(const char* id, const char* label, std::vector<const char*> options,
                         int defIndex) {
  Setting s;
  s.type = Setting::Type::Enum;
  s.id = id;
  s.label = label;
  s.options = std::move(options);
  s.index = std::clamp(defIndex, 0, std::max(0, (int)s.options.size() - 1));
  settings_.push_back(std::move(s));
  return settings_.back();
}

Setting& Module::addKey(const char* id, const char* label, int defVk) {
  Setting s;
  s.type = Setting::Type::Key;
  s.id = id;
  s.label = label;
  s.vk = defVk;
  settings_.push_back(std::move(s));
  return settings_.back();
}

Setting* Module::setting(const std::string& id) {
  for (auto& s : settings_)
    if (s.id == id) return &s;
  return nullptr;
}

bool Module::boolVal(const char* id, bool def) const {
  for (auto& s : settings_)
    if (s.id == id && s.type == Setting::Type::Bool) return s.b;
  return def;
}

float Module::floatVal(const char* id, float def) const {
  for (auto& s : settings_)
    if (s.id == id && s.type == Setting::Type::Float) return s.f;
  return def;
}

int Module::intVal(const char* id, int def) const {
  for (auto& s : settings_) {
    if (s.id != id) continue;
    if (s.type == Setting::Type::Enum) return s.index;
    if (s.type == Setting::Type::Key) return s.vk;
  }
  return def;
}

// ---- registry ----

Module& ModuleRegistry::add(std::unique_ptr<Module> m) {
  mods_.push_back(std::move(m));
  return *mods_.back();
}

Module* ModuleRegistry::find(const std::string& id) {
  for (auto& m : mods_)
    if (m->id() == id) return m.get();
  return nullptr;
}

void ModuleRegistry::updateEnabled(const ModuleContext& ctx) {
  for (auto& m : mods_) {
    if (!m->enabled) {
      m->live.clear();
      m->liveRows.clear();
      continue;
    }
    m->update(ctx);
  }
}

nlohmann::json ModuleRegistry::toJson() const {
  nlohmann::json out = nlohmann::json::object();
  for (auto& m : mods_) {
    nlohmann::json sj = nlohmann::json::object();
    for (auto& s : m->settings()) {
      switch (s.type) {
        case Setting::Type::Bool: sj[s.id] = s.b; break;
        case Setting::Type::Float: sj[s.id] = s.f; break;
        case Setting::Type::Enum: sj[s.id] = s.index; break;
        case Setting::Type::Key: sj[s.id] = s.vk; break;
      }
    }
    nlohmann::json mj = {{"enabled", m->enabled}};
    if (!sj.empty()) mj["settings"] = std::move(sj);
    out[m->id()] = std::move(mj);
  }
  return out;
}

void ModuleRegistry::applyJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  for (auto& [id, mj] : j.items()) {
    Module* m = find(id);
    if (!m || !mj.is_object()) continue;
    if (auto v = mj.find("enabled"); v != mj.end() && v->is_boolean())
      m->enabled = v->get<bool>();
    auto sj = mj.find("settings");
    if (sj == mj.end() || !sj->is_object()) continue;
    for (auto& [sid, val] : sj->items()) {
      Setting* s = m->setting(sid);
      if (!s) continue; // setting dropped or renamed since the config was written
      switch (s->type) {
        case Setting::Type::Bool:
          if (val.is_boolean()) s->b = val.get<bool>();
          break;
        case Setting::Type::Float:
          if (val.is_number()) s->f = std::clamp((float)val.get<double>(), s->min, s->max);
          break;
        case Setting::Type::Enum:
          if (val.is_number_integer()) {
            int i = val.get<int>();
            if (i >= 0 && i < (int)s->options.size()) s->index = i;
          }
          break;
        case Setting::Type::Key:
          if (val.is_number_integer()) s->vk = std::clamp(val.get<int>(), 0, 0xFE);
          break;
      }
    }
  }
}

void ModuleRegistry::markChanged() {
  ++changeVersion_;
  lastChange_ = std::chrono::steady_clock::now();
}

} // namespace mods
