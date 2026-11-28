#include "modules/radar.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <imgui.h>

#include "ui/theme.h"

namespace mods {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kCatColors[8] = {
    IM_COL32(255, 100, 100, 220), // 0 Mob
    IM_COL32(255, 230, 100, 220), // 1 Player
    IM_COL32(150, 150, 255, 220), // 2 ArmorStand
    IM_COL32(255, 170, 80, 220),  // 3 Projectile
    IM_COL32(100, 255, 140, 220), // 4 Item
    IM_COL32(220, 220, 220, 220), // 5 Living
    IM_COL32(160, 160, 160, 220), // 6
    IM_COL32(160, 160, 160, 220), // 7
};
} // namespace

RadarModule::RadarModule()
    : Module("radar", "Радар", 0xE909, ui::C_PURPLE, "Визуал",
             "2D-радар сверху: сущности вокруг игрока, повернут по yaw.",
             /*wide=*/false, /*defEnabled=*/false) {
  addFloat("range", "Радиус", 64.f, 16.f, 256.f, "%.0f м");
  addBool("mobs", "Мобы", true);
  addBool("players", "Игроки", true);
  addBool("items", "Предметы", false);
  addBool("misc", "Прочее", false);
}

void RadarModule::update(const ModuleContext& ctx) {
  if (!ctx.playerValid || !ctx.model) return;

  ImGuiIO& io = ImGui::GetIO();
  const float radius = 92.f;
  const float margin = 16.f;
  ImVec2 center(io.DisplaySize.x - radius - margin, margin + radius + 14.f);

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Background
  dl->AddCircleFilled(center, radius, IM_COL32(10, 10, 14, 160), 64);
  dl->AddCircle(center, radius, IM_COL32(90, 90, 110, 200), 64, 1.5f);
  dl->AddCircle(center, radius * 0.5f, IM_COL32(70, 70, 90, 120), 64, 1.f);
  dl->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
              IM_COL32(70, 70, 90, 100));
  dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
              IM_COL32(70, 70, 90, 100));

  // Forward indicator (always "up" on radar - radar rotates with player)
  dl->AddLine(center, ImVec2(center.x, center.y - radius + 6.f), IM_COL32(255, 255, 255, 120), 1.f);

  // Player marker
  dl->AddCircleFilled(center, 3.5f, IM_COL32(255, 255, 255, 255));

  const float yawRad = ctx.player.yaw * kPi / 180.f;
  const float cosYaw = std::cos(yawRad);
  const float sinYaw = std::sin(yawRad);
  const float range = floatVal("range", 64.f);
  const bool showMobs = boolVal("mobs", true), showPlayers = boolVal("players", true);
  const bool showItems = boolVal("items"), showMisc = boolVal("misc");

  int shown = 0;
  std::vector<mc::EspEntity> ents;
  double origin[3] = {ctx.player.x, ctx.player.y, ctx.player.z};
  if (!ctx.model->readEntities(*ctx.proc, ents, origin, range)) return;
  // same interpolation as ESP: rendered position = cur + delta*(partial-1)
  double partial = ctx.model->tickPartial();
  double interp = partial - 1.0;
  for (const auto& e : ents) {
    if (e.self) continue;
    switch (e.category) {
      case (int)mc::EntCat::Mob: if (!showMobs) continue; break;
      case (int)mc::EntCat::Player: if (!showPlayers) continue; break;
      case (int)mc::EntCat::Item: if (!showItems) continue; break;
      default: if (!showMisc) continue; break;
    }

    double ex = (e.bbMin[0] + e.bbMax[0]) * 0.5 + (e.cur[0] - e.prev[0]) * interp;
    double ez = (e.bbMin[2] + e.bbMax[2]) * 0.5 + (e.cur[2] - e.prev[2]) * interp;
    float dx = float(ex - ctx.player.x);
    float dz = float(ez - ctx.player.z);

    // Rotate relative to player yaw: forward = up on radar
    float rx = dx * cosYaw - dz * sinYaw;
    float rz = dx * sinYaw + dz * cosYaw;

    float dist = std::sqrt(rx * rx + rz * rz);
    if (dist > range) {
      rx = rx / dist * range;
      rz = rz / dist * range;
      dist = range;
    }

    float px = center.x + (rx / range) * (radius - 4.f);
    float py = center.y - (rz / range) * (radius - 4.f);

    ImU32 col = kCatColors[e.category & 7];
    float dotR = (e.category == 1) ? 3.5f : 2.5f;
    dl->AddCircleFilled(ImVec2(px, py), dotR, col);
    shown++;
  }

  std::string info = std::format("RADAR  {}  |  {}m", shown, (int)range);
  ImVec2 ts = ImGui::CalcTextSize(info.c_str());
  dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - radius - 16.f),
              IM_COL32(200, 200, 210, 220), info.c_str());
}

} // namespace mods