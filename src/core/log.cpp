#include "core/log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <chrono>
#include <memory>
#include <vector>

namespace tlog {

void init(const std::filesystem::path& filePath) {
  std::vector<spdlog::sink_ptr> sinks;

  auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console->set_pattern("%H:%M:%S |%^%=8l%$| %v");
  sinks.push_back(console);

  try {
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        filePath.wstring(), 5 * 1024 * 1024, 3);
    file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    sinks.push_back(file);
  } catch (...) {
    // Locked/invalid log path must not stop the app; console logging still works.
  }

  auto logger = std::make_shared<spdlog::logger>("threshold", sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::debug);
  spdlog::set_default_logger(logger);
  spdlog::flush_every(std::chrono::seconds(1));
}

} // namespace tlog
