#include "modules/fov.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "core/util.h"
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

    // Zoom compensation: OkZoomer-style mods keep an animated zoom divisor
    // (fov_eff = fov_base / divisor) in a static field. The class is probed
    // by the heap scan; static-only classes (no heap instances) are found
    // through the constant pools of sibling mod classes. The divisor is read
    // per frame because it eases while zooming. Nothing found -> divisor = 1.
    double divisor = 1.0;
    static const char* kZoomClass = "page/langeweile/ok_zoomer/zoom/Zoom";
    static const char* kZoomHosts[] = {
        "page/langeweile/ok_zoomer/OkZoomerClientMod",
        "page/langeweile/ok_zoomer/utils/ZoomUtils",
        "page/langeweile/ok_zoomer/utils/ForgeZoomUtils",
        "page/langeweile/ok_zoomer/events/ManageZoomEvent",
        "page/langeweile/ok_zoomer/zoom/Zoom",
    };
    uint64_t zk = 0;
    for (const char* zn : kZoomHosts)
      if ((zk = ctx.model->probeKlass(zn)) != 0) break;
    if (!zk) {
      std::vector<uint64_t> hosts;
      for (const char* hn : kZoomHosts)
        if (uint64_t h = ctx.model->probeKlass(hn)) hosts.push_back(h);
      zk = ctx.model->findKlassByCP(*ctx.proc, hosts, kZoomClass);
    }
    if (zk) {
      double d = 0;
      if (ctx.model->readStaticDouble(*ctx.proc, zk, "zoomDivisor", &d) && d > 0.01)
        divisor = d;
      static bool logged = false;
      if (!logged) {
        logged = true;
        spdlog::info("zoom: state klass @ {} divisor={:.3f}", util::hex(zk), divisor);
      }
    }
    zoomDivisor_ = divisor;
    effectiveFov_ = std::clamp(effectiveFov_ / (float)std::clamp(zoomDivisor_, 0.05, 64.0),
                               1.f, 170.f);
  } else {
    effectiveFov_ = cachedFov_;
  }

  if (enabled) {
    live = std::format("{:.0f}°", effectiveFov_);
    liveRows = {
        {"FOV", std::format("{:.1f}°", effectiveFov_)},
        {"Mode", fovManual ? "Manual" : "Auto"},
        {"Source", fovManual ? "Setting" : "Game"},
        {"Zoom", std::format("x{:.2f}", zoomDivisor_)},
    };
  }
}

FovModule* FovModule::get(ModuleRegistry& reg) {
  Module* m = reg.find("fov");
  return m ? dynamic_cast<FovModule*>(m) : nullptr;
}

} // namespace mods
