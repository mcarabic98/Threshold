#include "modules/tracers.h"

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
    IM_COL32(255, 90, 90, 220),  // 0 Mob
    IM_COL32(255, 230, 90, 220), // 1 Player
    IM_COL32(140, 140, 255, 180),// 2 ArmorStand
    IM_COL32(255, 170, 70, 220), // 3 Projectile
    IM_COL32(90, 255, 130, 220), // 4 Item
    IM_COL32(210, 210, 210, 200),// 5 Living
    IM_COL32(150, 150, 150, 180),
    IM_COL32(150, 150, 150, 180),
};

bool worldToScreen(const float vp[16], ImVec2 screen, const float w[3], ImVec2& out) {
  float x = vp[0] * w[0] + vp[4] * w[1] + vp[8] * w[2] + vp[12];
  float y = vp[1] * w[0] + vp[5] * w[1] + vp[9] * w[2] + vp[13];
  float z = vp[2] * w[0] + vp[6] * w[1] + vp[10] * w[2] + vp[14];
  float wclip = vp[3] * w[0] + vp[7] * w[1] + vp[11] * w[2] + vp[15];
  if (wclip <= 0.001f) return false;
  float ndcX = x / wclip, ndcY = y / wclip;
  if (ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f) return false;
  out.x = (ndcX * 0.5f + 0.5f) * screen.x;
  out.y = (1.f - (ndcY * 0.5f + 0.5f)) * screen.y;
  return true;
}
} // namespace

TracersModule::TracersModule()
    : Module("tracers", "Трейсеры", 0xE945, ui::C_CYAN, "Визуал",
             "Линии от центра экрана к сущностям.",
             /*wide=*/false, /*defEnabled=*/false) {}

void TracersModule::update(const ModuleContext& ctx) {
  if (!ctx.playerValid || !ctx.model || !ctx.camera.valid) return;

  // Build a simple view-projection from camera snapshot (same basis as ESP)
  const auto& cam = ctx.camera;
  ImVec2 screen = ImGui::GetIO().DisplaySize;
  float aspect = screen.x / std::max(screen.y, 1.f);

  // Build VP matrix from camera basis
  auto normalize = [](float v[3]) {
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; }
  };
  float fwd[3] = {cam.forwards[0], cam.forwards[1], cam.forwards[2]};
  float up[3]  = {cam.up[0], cam.up[1], cam.up[2]};
  float left[3]= {cam.left[0], cam.left[1], cam.left[2]};
  normalize(fwd); normalize(up); normalize(left);

  // View matrix: rows are right(-left), up, -forward
  float r[3] = {-left[0], -left[1], -left[2]};
  float view[16] = {
    r[0], up[0], -fwd[0], 0,
    r[1], up[1], -fwd[1], 0,
    r[2], up[2], -fwd[2], 0,
    0,    0,     0,       1
  };
  float px = -(view[0]*cam.pos[0] + view[4]*cam.pos[1] + view[8]*cam.pos[2]);
  float py = -(view[1]*cam.pos[0] + view[5]*cam.pos[1] + view[9]*cam.pos[2]);
  float pz = -(view[2]*cam.pos[0] + view[6]*cam.pos[1] + view[10]*cam.pos[2]);
  view[12]=px; view[13]=py; view[14]=pz;

  float fovY = std::clamp(ctx.fovDeg, 30.f, 170.f) * kPi / 180.f;
  float tz = 1.f / std::tan(fovY * 0.5f);
  float nearZ = 0.05f, farZ = 1000.f;
  float proj[16] = {};
  proj[0] = tz / aspect; proj[5] = tz;
  proj[10] = farZ / (nearZ - farZ); proj[11] = -1.f;
  proj[14] = nearZ * farZ / (nearZ - farZ);

  // vp = proj * view
  float vp[16] = {};
  for (int c = 0; c < 4; ++c)
    for (int r2 = 0; r2 < 4; ++r2)
      for (int k = 0; k < 4; ++k)
        vp[c*4 + r2] += proj[k*4 + r2] * view[c*4 + k];

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 origin(screen.x * 0.5f, screen.y * 0.5f);

  std::vector<mc::EspEntity> ents;
  if (!ctx.model->readEntities(*ctx.proc, ents)) return;
  for (const auto& e : ents) {
    if (e.self) continue;
    int cat = e.category & 7;
    // Only draw for combat-relevant categories by default: mobs, players, living
    if (cat != 0 && cat != 1 && cat != 5) continue;

    float wx = float((e.bbMin[0] + e.bbMax[0]) * 0.5);
    float wy = float((e.bbMin[1] + e.bbMax[1]) * 0.5);
    float wz = float((e.bbMin[2] + e.bbMax[2]) * 0.5);
    float w[3] = {wx, wy, wz};

    ImVec2 sp;
    if (!worldToScreen(vp, screen, w, sp)) continue;

    ImU32 col = kCatColors[cat];
    dl->AddLine(origin, sp, col, 1.2f);
  }
}

} // namespace mods