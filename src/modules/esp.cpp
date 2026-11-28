#include "modules/esp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <imgui.h>

#include "ui/theme.h"

namespace mods {

namespace {

// Project world -> screen with a view-projection matrix built from the
// camera basis (position + forwards/up/left) and a vertical FOV.
bool projectPoint(const float view[16], const float proj[16], double x, double y, double z,
                  int w, int h, ImVec2* out) {
  // view transform
  double vx = view[0] * x + view[4] * y + view[8] * z + view[12];
  double vy = view[1] * x + view[5] * y + view[9] * z + view[13];
  double vz = view[2] * x + view[6] * y + view[10] * z + view[14];
  double vw = view[3] * x + view[7] * y + view[11] * z + view[15];
  // projection
  double px = proj[0] * vx + proj[4] * vy + proj[8] * vz + proj[12] * vw;
  double py = proj[1] * vx + proj[5] * vy + proj[9] * vz + proj[13] * vw;
  double pw = proj[3] * vx + proj[7] * vy + proj[11] * vz + proj[15] * vw;
  if (pw <= 0.001) return false;
  out->x = (float)((px / pw * 0.5 + 0.5) * w);
  out->y = (float)((1.0 - (py / pw * 0.5 + 0.5)) * h);
  return true;
}

constexpr ImU32 catColors[(int)mc::EntCat::Count] = {
    IM_COL32(255, 60, 60, 230),   // Mob      — red
    IM_COL32(60, 130, 255, 230),  // Player   — blue
    IM_COL32(180, 180, 180, 200), // ArmorStand — gray
    IM_COL32(255, 150, 40, 210),  // Projectile — orange
    IM_COL32(255, 220, 60, 210),  // Item     — yellow
    IM_COL32(60, 255, 120, 210),  // Living   — green
    IM_COL32(220, 220, 220, 190), // Misc     — white
};
constexpr const char* catNames[(int)mc::EntCat::Count] = {"Mob", "Player", "ArmorStand",
                                                          "Projectile", "Item", "Living",
                                                          "Other"};

} // namespace

EspModule::EspModule()
    : Module("esp", "ESP", 0xE81C, ui::C_RED, "Combat",
             "Подсветка сущностей: мобы, игроки, предметы. Цвет по типу.",
             /*wide=*/false, /*defEnabled=*/true) {
  addFloat("distance", "Дистанция", 128.f, 16.f, 256.f, "%.0f м");
  addBool("mobs", "Мобы", true);
  addBool("players", "Игроки", true);
  addBool("items", "Предметы", false);
  addBool("misc", "Прочее", false);
  addBool("selfbox", "Свой бокс", false);
}

void EspModule::update(const ModuleContext& ctx) {
  if (!ctx.attached || !ctx.modelValid || !ctx.proc || !ctx.model) return;
  const mem::Process& proc = *ctx.proc;
  mc::GameModel& model = *ctx.model; // read*() are non-const: they fill field caches

  const float maxDist = floatVal("distance", 128.f);
  const bool showMobs = boolVal("mobs", true), showPlayers = boolVal("players", true);
  const bool showItems = boolVal("items"), showMisc = boolVal("misc");

  mc::CameraSnapshot cam;
  if (!model.readCamera(proc, cam) || !cam.valid) return;
  std::vector<mc::EspEntity> ents;
  if (!model.readEntities(proc, ents, cam.pos, maxDist)) return;

  // apply category + distance filters
  ents.erase(std::remove_if(ents.begin(), ents.end(),
                            [&](const mc::EspEntity& e) {
                              double mx = (e.bbMin[0] + e.bbMax[0]) * 0.5;
                              double my = (e.bbMin[1] + e.bbMax[1]) * 0.5;
                              double mz = (e.bbMin[2] + e.bbMax[2]) * 0.5;
                              double ddx = mx - cam.pos[0], ddy = my - cam.pos[1],
                                     ddz = mz - cam.pos[2];
                              double dst = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                              if (dst > maxDist) return true;
                              if (e.self && !boolVal("selfbox", false)) return true;
                              switch (e.category) {
                                case (int)mc::EntCat::Mob: return !showMobs;
                                case (int)mc::EntCat::Player: return !showPlayers;
                                case (int)mc::EntCat::Item: return !showItems;
                                default: return !showMisc;
                              }
                            }),
            ents.end());

  ImVec2 winPos = ctx.winPos;
  int w = (int)ctx.winSize.x, h = (int)ctx.winSize.y;
  if (w <= 0 || h <= 0) return;

  // view matrix rows: right = -left, up, -forward (right-handed, -Z forward)
  float r[3] = {-cam.left[0], -cam.left[1], -cam.left[2]};
  float u[3] = {cam.up[0], cam.up[1], cam.up[2]};
  float f[3] = {cam.forwards[0], cam.forwards[1], cam.forwards[2]};
  float view[16] = {
      r[0], u[0], -f[0], 0.f,
      r[1], u[1], -f[1], 0.f,
      r[2], u[2], -f[2], 0.f,
      0.f,  0.f,  0.f,   1.f,
  };
  float px = 0, py = 0, pz = 0;
  // translation column: -R * pos
  px = -(r[0] * cam.pos[0] + r[1] * cam.pos[1] + r[2] * cam.pos[2]);
  py = -(u[0] * cam.pos[0] + u[1] * cam.pos[1] + u[2] * cam.pos[2]);
  pz = (f[0] * cam.pos[0] + f[1] * cam.pos[1] + f[2] * cam.pos[2]);
  view[12] = px;
  view[13] = py;
  view[14] = pz;

  const float fovDeg = std::clamp(ctx.fovDeg, 30.f, 170.f);
  float fovY = fovDeg * 3.14159265f / 180.f;
  float aspect = (float)w / (float)h;
  float tz = 1.f / std::tan(fovY * 0.5f);
  float farZ = 1000.f, nearZ = 0.05f;
  float proj[16] = {0};
  proj[0] = tz / aspect;
  proj[5] = tz;
  proj[10] = farZ / (nearZ - farZ);
  proj[11] = -1.f;
  proj[14] = nearZ * farZ / (nearZ - farZ);

  live = std::format("{} ent", ents.size());
  liveRows = {
      {"Сущностей", std::format("{}", ents.size())},
      {"FOV", std::format("{:.0f}°", fovDeg)},
      {"Дистанция", std::format("{:.0f} м", maxDist)},
  };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImU32 colorTxt = IM_COL32(255, 255, 255, 230);

  // Render positions come from the model's tick cache: the interpolated
  // target follows the game's own prev + (cur - prev) * partialTick formula,
  // and a short chase-smoother (GameModel) absorbs the residual phase error
  // from RPM latency. The AABB matches cur, so the render shift is
  // rendered - cur.
  double cx2 = cam.pos[0], cy2 = cam.pos[1], cz2 = cam.pos[2];
  for (auto& e : ents) {
    double dx = e.rendered[0] - e.cur[0];
    double dy = e.rendered[1] - e.cur[1];
    double dz = e.rendered[2] - e.cur[2];
    // project the 8 AABB corners, shifted by the interpolation delta
    ImVec2 minS(1e9f, 1e9f), maxS(-1e9f, -1e9f);
    int okCount = 0;
    for (int c = 0; c < 8; ++c) {
      double cx = ((c & 1) ? e.bbMax[0] : e.bbMin[0]) + dx;
      double cy = ((c & 2) ? e.bbMax[1] : e.bbMin[1]) + dy;
      double cz = ((c & 4) ? e.bbMax[2] : e.bbMin[2]) + dz;
      ImVec2 s;
      if (projectPoint(view, proj, cx, cy, cz, w, h, &s)) {
        ++okCount;
        minS.x = std::min(minS.x, s.x);
        minS.y = std::min(minS.y, s.y);
        maxS.x = std::max(maxS.x, s.x);
        maxS.y = std::max(maxS.y, s.y);
      }
    }
    if (okCount < 8) continue; // fully or partially behind the camera
    if (maxS.x - minS.x < 2 || maxS.y - minS.y < 2) continue;

    ImU32 color = e.self ? IM_COL32(255, 255, 255, 200) : catColors[e.category & 7];
    dl->AddRect(ImVec2(winPos.x + minS.x, winPos.y + minS.y),
                ImVec2(winPos.x + maxS.x, winPos.y + maxS.y), color, 0.f, 0, 1.5f);

    // center for distance
    double mx = ((e.bbMin[0] + e.bbMax[0]) * 0.5) + dx;
    double my = ((e.bbMin[1] + e.bbMax[1]) * 0.5) + dy;
    double mz = ((e.bbMin[2] + e.bbMax[2]) * 0.5) + dz;
    double dist = std::sqrt((mx - cx2) * (mx - cx2) + (my - cy2) * (my - cy2) +
                            (mz - cz2) * (mz - cz2));

    const char* cls = e.className.c_str();
    if (const char* slash = std::strrchr(cls, '/')) cls = slash + 1;
    auto label = std::format("[{}] {} {:.0f}m", e.self ? "You" : catNames[e.category & 7],
                             e.self ? "" : cls, dist);
    dl->AddText(ImVec2(winPos.x + minS.x, winPos.y + minS.y - 14.f), colorTxt, label.c_str());
  }
}

} // namespace mods
