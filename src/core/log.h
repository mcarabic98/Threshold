#pragma once
#include <filesystem>

namespace tlog {

// Console + rotating file sink. Call once at startup.
void init(const std::filesystem::path& filePath);

} // namespace tlog
