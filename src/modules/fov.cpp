#include "modules/fov.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "ui/theme.h"

namespace mods {

FovModule::FovModule()
    : Module("fov", "FOV", 0xE7B3, ui::C_YELLOW, "Визуал",
             "Централизованный FOV: авто-чтение из игры, ручной оверрайд, "
             "адаптация под зум/изменения.",
             /*wide=*/false, /*defEnabled=*/true) {
  addBool("fovManual", "FOV вручную", false);
  addFloat("fov", "FOV", 70.f, 30.f, 110.f, "%.0f°");
}

void FovModule::update(const ModuleContext& ctx) {
  const bool fovManual = boolVal("fovManual");
  const float fovSet = std::clamp(floatVal("fov", 70.f), 30.f, 110.f);

  if (fovManual) {
    effectiveFov_ = fovSet;
  } else if (ctx.attached && ctx.modelValid && ctx.proc && ctx.model) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastFovRead_).count() > 1.0) {
      lastFovRead_ = now;
      float fileFov = ctx.model->readFovFromFile(*ctx.proc);
      float heapFov = ctx.model->readFov(*ctx.proc);
      if (fileFov > 1.f)
        cachedFov_ = fileFov;
      else if (heapFov > 1.f)
        cachedFov_ = heapFov;
      spdlog::debug("fov module: cached {:.1f} deg", cachedFov_);
    }
    effectiveFov_ = std::clamp(cachedFov_, 30.f, 170.f);
  } else {
    effectiveFov_ = cachedFov_;
  }

  if (enabled) {
    live = std::format("{:.0f}°", effectiveFov_);
    liveRows = {
        {"FOV", std::format("{:.1f}°", effectiveFov_)},
        {"Mode", fovManual ? "Manual" : "Auto"},
        {"Source", fovManual ? "Setting" : "Game"},
    };
  }
}

FovModule* FovModule::get(ModuleRegistry& reg) {
  Module* m = reg.find("fov");
  return m ? dynamic_cast<FovModule*>(m) : nullptr;
}

} // namespace mods
