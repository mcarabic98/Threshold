#pragma once
#include <filesystem>

namespace config {

struct Config {
  int menuKey = 0x2D; // VK_INSERT
  int exitKey = 0x23; // VK_END
};

std::filesystem::path appDir();
bool load(Config& out);
bool save(const Config& cfg);

} // namespace config
