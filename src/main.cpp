#include <spdlog/spdlog.h>

#include <windows.h>

#include "app/app.h"
#include "core/config.h"
#include "core/log.h"

int main() {
  // Real pixel coordinates for overlay tracking on scaled displays.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetConsoleOutputCP(CP_UTF8);

  config::Config cfg;
  config::load(cfg);
  tlog::init(config::appDir() / L"threshold.log");
  spdlog::info("Threshold - external client base, built {} {}", __DATE__, __TIME__);

  app::App app;
  return app.run(cfg);
}
