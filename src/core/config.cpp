#include "core/config.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <windows.h>

namespace config {

std::filesystem::path appDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

bool load(Config& out) {
  std::ifstream f(appDir() / L"config.json");
  if (!f) return false;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    if (auto v = j.find("menuKey"); v != j.end() && v->is_number_integer())
      out.menuKey = v->get<int>();
    if (auto v = j.find("exitKey"); v != j.end() && v->is_number_integer())
      out.exitKey = v->get<int>();
    if (auto v = j.find("modules"); v != j.end() && v->is_object()) out.modules = *v;
    return true;
  } catch (const std::exception& e) {
    spdlog::warn("config parse failed: {}", e.what());
    return false;
  }
}

bool save(const Config& cfg) {
  nlohmann::json j;
  j["menuKey"] = cfg.menuKey;
  j["exitKey"] = cfg.exitKey;
  if (!cfg.modules.empty()) j["modules"] = cfg.modules;
  std::ofstream f(appDir() / L"config.json");
  if (!f) return false;
  f << j.dump(2) << "\n";
  return true;
}

} // namespace config
