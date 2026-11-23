#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace config {

struct Config {
  int menuKey = 0x2D; // VK_INSERT
  int exitKey = 0x23; // VK_END
  // per-module state; the schema is owned by mods::ModuleRegistry
  nlohmann::json modules = nlohmann::json::object();
};

std::filesystem::path appDir();
bool load(Config& out);
bool save(const Config& cfg);

} // namespace config
