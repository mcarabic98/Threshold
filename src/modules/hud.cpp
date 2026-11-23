#include "modules/hud.h"

#include <format>

#include <imgui.h>

#include "ui/theme.h"

namespace mods {

FpsModule::FpsModule()
    : Module("fps", "FPS", 0xE9D9, ui::C_BLUE, "HUD", "Частота кадров на экране.",
             /*wide=*/true, /*defEnabled=*/true) {}

void FpsModule::update(const ModuleContext&) {
  live = std::format("{:.0f} fps", ImGui::GetIO().Framerate);
}

CoordsModule::CoordsModule()
    : Module("coords", "Координаты", 0xE7C3, ui::C_PURPLE, "HUD", "Текущие XYZ и направление.",
             /*wide=*/true, /*defEnabled=*/true) {}

void CoordsModule::update(const ModuleContext& ctx) {
  if (!ctx.playerValid) {
    liveRows.clear();
    return;
  }
  const auto& p = ctx.player;
  // empty label keeps the old panel look: the value is drawn as-is
  liveRows = {{"",
               std::format("x {:.2f}  y {:.2f}  z {:.2f}   yaw {:.1f}  pitch {:.1f}", p.x, p.y,
                           p.z, p.yaw, p.pitch)}};
}

} // namespace mods
