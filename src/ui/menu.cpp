#include "ui/menu.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <windows.h>

#include "ui/theme.h"

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x + b.x, a.y + b.y);
}
static ImVec2 operator-(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x - b.x, a.y - b.y);
}

namespace ui {

ImFont* g_iconFont = nullptr;

// layout
static constexpr float NAV_W = 140.f;
static constexpr float PANEL_W = 230.f;
static constexpr float HEADER_H = 56.f;
static constexpr float TILE = 94.f;
static constexpr float GAP = 6.f;
static constexpr float PAD = 12.f;

static const char* catNames[] = {"Все", "Combat", "Визуал", "HUD", "Настройки"};
static constexpr int kNumCats = 5;

// ---- helpers ----

static void drawIcon(ImDrawList* dl, unsigned glyph, ImVec2 pos, float size, ImU32 col) {
  if (g_iconFont) {
    ImGui::PushFont(g_iconFont);
    dl->AddText(nullptr, size, pos, col, reinterpret_cast<const char*>(&glyph),
                reinterpret_cast<const char*>(&glyph) + 4);
    ImGui::PopFont();
  } else {
    // fallback: draw a filled square
    dl->AddRectFilled(pos, pos + ImVec2(size * 0.5f, size * 0.5f), col);
  }
}

static void drawSwImpl(ImDrawList* dl, ImVec2 pos, bool on) {
  // Metro toggle: 40x16 with 14x14 sliding thumb
  ImU32 border = on ? C_ACCENT : C_DIM;
  ImU32 bg = on ? C_ACCENT : IM_COL32(0, 0, 0, 60);
  ImU32 thumb = on ? C_TEXT : C_DIM;
  dl->AddRectFilled(pos, pos + ImVec2(40, 16), bg);
  dl->AddRect(pos, pos + ImVec2(40, 16), border, 0.f, 0, 1.5f);
  ImVec2 tp = on ? pos + ImVec2(40 - 14, 1) : pos + ImVec2(1, 1);
  dl->AddRectFilled(tp, tp + ImVec2(14, 14), thumb);
}

// ---- Menu implementation ----

void Menu::draw(MenuState& st, mods::ModuleRegistry& reg) {
  reg_ = &reg;
  // a capture left pending across hidden frames is stale: the menu was closed
  if (listenTarget_ && ImGui::GetFrameCount() != lastDrawFrame_ + 1) listenTarget_ = nullptr;
  lastDrawFrame_ = ImGui::GetFrameCount();
  pollKeyCapture();

  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, C_BG);

  ImGui::Begin("##metro_menu", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(wp, wp + ws, C_BG);
  dl->AddRect(wp, wp + ws, C_TILE, 0.f, 0, 1.f); // subtle border

  drawHeader(st);
  drawNav(st);
  drawGrid();
  drawPanel(st);

  ImGui::End();
  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(3);
}

void Menu::drawHeader(MenuState& st) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float w = ImGui::GetWindowSize().x;

  dl->AddRectFilled(wp, wp + ImVec2(w, HEADER_H), C_BG);
  dl->AddRectFilled(wp, wp + ImVec2(w, 2), C_ACCENT);

  // caption
  dl->AddText(ImVec2(wp.x + PAD, wp.y + 8), C_ACCENT, "МЕНЮ МОДОВ");
  dl->AddText(nullptr, 22.f, ImVec2(wp.x + PAD, wp.y + 24), C_TEXT, "Threshold");

  // status
  ImU32 dot = st.attached ? (st.jvmOk ? IM_COL32(0x7f, 0xba, 0x00, 255)
                                      : IM_COL32(255, 180, 0, 255))
                          : C_DIM;
  dl->AddCircleFilled(ImVec2(wp.x + w - PAD - 60, wp.y + HEADER_H / 2), 5, dot);
  const char* status = st.attached ? (st.jvmOk ? "attached" : "partial") : "off";
  dl->AddText(ImVec2(wp.x + w - PAD - 130, wp.y + HEADER_H / 2 - 8), C_DIM, status);

  // drag area
  ImGui::SetCursorScreenPos(wp);
  ImGui::InvisibleButton("##hdr_drag", ImVec2(w - 150, HEADER_H));
  if (ImGui::IsItemActive()) {
    ImVec2 d = ImGui::GetIO().MouseDelta;
    ImGui::SetWindowPos(ImGui::GetWindowPos() + d);
  }
  ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + HEADER_H));
}

void Menu::drawNav(MenuState& st) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float ny = wp.y + HEADER_H;
  float nh = ws.y - HEADER_H;

  dl->AddRectFilled(ImVec2(wp.x, ny), ImVec2(wp.x + NAV_W, ny + nh), C_BG);

  ImGui::SetCursorScreenPos(ImVec2(wp.x + PAD, ny + 12));

  for (int c = 0; c < kNumCats; ++c) {
    bool on = selCategory_ == catNames[c];
    ImVec2 bp = ImGui::GetCursorScreenPos();
    ImVec2 bs = ImVec2(NAV_W - PAD * 2, 28);
    // accent bar on left edge
    if (on) dl->AddRectFilled(bp, bp + ImVec2(3, bs.y), C_ACCENT);
    dl->AddText(ImVec2(bp.x + 10, bp.y + 5), on ? C_TEXT : C_DIM, catNames[c]);
    ImGui::SetCursorScreenPos(bp);
    char id[32];
    snprintf(id, sizeof(id), "##nav%d", c);
    ImGui::InvisibleButton(id, bs);
    if (ImGui::IsItemClicked()) selCategory_ = catNames[c];
    ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bs.y + 2));
  }

  // profile label at bottom
  dl->AddText(ImVec2(wp.x + PAD, ny + nh - 24), C_DIM, "ПРОФИЛЬ: PVP");

  // set cursor for next section
  ImGui::SetCursorScreenPos(ImVec2(wp.x + NAV_W, wp.y + HEADER_H));
  ImGui::Dummy(ImVec2(0, 0));
}

void Menu::drawTile(mods::Module& m, ImVec2 pos, ImVec2 size, bool selected) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = m.enabled ? m.color() : C_TILE;
  ImU32 txtCol = m.enabled ? IM_COL32(255, 255, 255, 255) : C_DIM;

  dl->AddRectFilled(pos, pos + size, bg);

  // selected border
  if (selected) {
    dl->AddRect(pos, pos + size, C_TEXT, 0.f, 0, 2.f);
    dl->AddRect(pos + ImVec2(3, 3), pos + size - ImVec2(3, 3), C_BG, 0.f, 0, 1.f);
  }

  // status text (top right)
  dl->AddText(ImVec2(pos.x + size.x - 36, pos.y + 5),
              m.enabled ? IM_COL32(255, 255, 255, 200) : C_DIM, m.enabled ? "вкл" : "выкл");

  // icon (centered-ish)
  drawIcon(dl, m.glyph(), pos + ImVec2(10, size.y * 0.28f), 28.f, txtCol);

  // name (bottom left)
  dl->AddText(ImVec2(pos.x + 8, pos.y + size.y - 20), txtCol, m.name().c_str());

  // live counter (bottom right)
  if (m.enabled && !m.live.empty()) {
    ImVec2 ls = ImGui::CalcTextSize(m.live.c_str());
    dl->AddText(ImVec2(pos.x + size.x - ls.x - 8, pos.y + size.y - 26),
                IM_COL32(255, 255, 255, 180), m.live.c_str());
  }

  // click handling
  ImGui::SetCursorScreenPos(pos);
  ImGui::PushID(m.id().c_str());
  ImGui::InvisibleButton("##tile", size);
  if (ImGui::IsItemHovered()) dl->AddRectFilled(pos, pos + size, IM_COL32(255, 255, 255, 15));
  if (ImGui::IsItemClicked()) {
    if (selModuleId_ == m.id()) {
      m.enabled = !m.enabled;
      reg_->markChanged();
    }
    selModuleId_ = m.id();
  }
  ImGui::PopID();
}

void Menu::drawGrid() {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  float gx = wp.x + NAV_W;
  float gw = ws.x - NAV_W - PANEL_W;

  float x = gx + PAD;
  float y = wp.y + HEADER_H + PAD;
  float maxX = gx + gw - PAD;
  float maxY = wp.y + ws.y - PAD;

  for (auto& pm : reg_->all()) {
    auto& m = *pm;
    if (selCategory_ != "Все" && m.category() != selCategory_) continue;
    float tw = m.wide() ? TILE * 2 + GAP : TILE;
    if (x + tw > maxX) {
      x = gx + PAD;
      y += TILE + GAP;
    }
    if (y + TILE > maxY) break;
    drawTile(m, ImVec2(x, y), ImVec2(tw, TILE), selModuleId_ == m.id());
    x += tw + GAP;
  }

  // dummy to keep ImGui happy
  ImGui::SetCursorScreenPos(ImVec2(gx, maxY));
  ImGui::Dummy(ImVec2(gw, 0));
}

void Menu::drawToggleRow(const char* label, bool* val, float tx, float* y) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(tx, *y));
  ImGui::PushID(label);
  ImGui::InvisibleButton("##tgl", ImVec2(40, 16));
  drawSwImpl(dl, ImVec2(tx, *y), *val);
  if (ImGui::IsItemClicked()) {
    *val = !*val;
    reg_->markChanged();
  }
  ImGui::PopID();
  dl->AddText(ImVec2(tx + 50, *y), C_TEXT, label);
  *y += 24;
}

void Menu::drawSliderRow(const char* label, float* val, float mn, float mx, const char* fmt,
                         float tx, float* y, float cw) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(tx, *y), C_DIM, label);
  *y += 16;
  ImGui::SetCursorScreenPos(ImVec2(tx, *y));
  ImGui::PushItemWidth(cw);
  ImGui::PushID(label);
  ImGui::SliderFloat("##sl", val, mn, mx, fmt);
  if (ImGui::IsItemDeactivatedAfterEdit()) reg_->markChanged();
  ImGui::PopID();
  ImGui::PopItemWidth();
  *y += 24;
}

void Menu::drawEnumRow(mods::Setting& s, float tx, float* y, float cw) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(tx, *y), C_DIM, s.label.c_str());
  *y += 16;

  // stepper: [<] current option [>], wraps around
  const char* val = s.options.empty() ? "-" : s.options[s.index];
  dl->AddRectFilled(ImVec2(tx + 24, *y), ImVec2(tx + cw - 24, *y + 20), C_TILE);
  ImVec2 ts = ImGui::CalcTextSize(val);
  dl->AddText(ImVec2(tx + (cw - ts.x) * 0.5f, *y + 3), C_TEXT, val);

  auto arrow = [&](const char* id, const char* glyph, float ax, int delta) {
    ImGui::SetCursorScreenPos(ImVec2(ax, *y));
    ImGui::PushID(id);
    ImGui::InvisibleButton(id, ImVec2(20, 20));
    bool hover = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    dl->AddText(ImVec2(ax + 6, *y + 3), hover ? C_ACCENT : C_DIM, glyph);
    if (clicked && !s.options.empty()) {
      int n = (int)s.options.size();
      s.index = ((s.index + delta) % n + n) % n;
      reg_->markChanged();
    }
  };
  ImGui::PushID(s.id.c_str());
  arrow("##l", "<", tx, -1);
  arrow("##r", ">", tx + cw - 20, +1);
  ImGui::PopID();
  *y += 26;
}

void Menu::drawKeyRow(mods::Setting& s, float tx, float* y, float cw) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(tx, *y), C_DIM, s.label.c_str());
  *y += 16;

  bool listening = listenTarget_ == &s;
  ImVec2 ksz = ImVec2(cw, 26);
  dl->AddRectFilled(ImVec2(tx, *y), ImVec2(tx, *y) + ksz, C_TILE);
  if (listening) dl->AddRect(ImVec2(tx, *y), ImVec2(tx, *y) + ksz, C_ACCENT, 0.f, 0, 1.5f);
  std::string shown = listening ? "Нажмите клавишу… (Esc — отмена)" : mods::vkName(s.vk);
  dl->AddText(ImVec2(tx + 8, *y + 5), C_TEXT, shown.c_str());

  ImGui::SetCursorScreenPos(ImVec2(tx, *y));
  ImGui::PushID(s.id.c_str());
  ImGui::InvisibleButton("##key", ksz);
  if (ImGui::IsItemClicked()) {
    if (listening) {
      listenTarget_ = nullptr; // second click cancels
    } else {
      listenTarget_ = &s;
      // keys held right now (the click itself) must not become the bind
      for (int k = 0; k < 256; ++k) keySnap_[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
    }
  }
  ImGui::PopID();
  *y += 32;
}

void Menu::pollKeyCapture() {
  if (!listenTarget_) return;
  for (int k = 1; k < 256; ++k) {
    bool down = (GetAsyncKeyState(k) & 0x8000) != 0;
    if (down && !keySnap_[k]) {
      mods::Setting* s = listenTarget_;
      listenTarget_ = nullptr;
      if (k != VK_ESCAPE) { // Esc cancels, any other newly pressed key binds
        s->vk = k;
        reg_->markChanged();
      }
      return;
    }
    keySnap_[k] = down;
  }
}

void Menu::drawPanel(MenuState& st) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  float px = wp.x + ws.x - PANEL_W;
  float py = wp.y + HEADER_H;
  float ph = ws.y - HEADER_H;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  dl->AddRectFilled(ImVec2(px, py), ImVec2(wp.x + ws.x, py + ph), C_PANEL);

  mods::Module* sel = reg_->find(selModuleId_);
  float y = py + 14;
  float tx = px + PAD;
  float cw = PANEL_W - PAD * 2;

  if (!sel) {
    dl->AddText(ImVec2(tx, y), C_DIM, "Выберите модуль");
    return;
  }

  // category + name
  dl->AddText(ImVec2(tx, y), C_ACCENT, sel->category().c_str());
  y += 18;
  dl->AddText(nullptr, 22.f, ImVec2(tx, y), C_TEXT, sel->name().c_str());
  y += 30;

  // description (wrapped)
  {
    std::string dStr = sel->desc();
    const char* d = dStr.c_str();
    float wrapW = cw;
    ImVec2 sz = ImGui::CalcTextSize(d);
    if (sz.x > wrapW) {
      // find split point
      size_t mid = dStr.size() / 2;
      while (mid > 0 && d[mid] != ' ') --mid;
      if (mid == 0) mid = dStr.size() / 2;
      dl->AddText(ImVec2(tx, y), C_DIM, std::string(d, mid).c_str());
      y += 15;
      dl->AddText(ImVec2(tx, y), C_DIM, d + mid + (d[mid] == ' ' ? 1 : 0));
      y += 15;
    } else {
      dl->AddText(ImVec2(tx, y), C_DIM, d);
      y += 15;
    }
  }
  y += 8;

  // toggle
  {
    ImGui::SetCursorScreenPos(ImVec2(tx, y));
    ImGui::PushID("panel_toggle");
    ImGui::InvisibleButton("##tgl", ImVec2(40, 16));
    drawSwImpl(dl, ImVec2(tx, y), sel->enabled);
    if (ImGui::IsItemClicked()) {
      sel->enabled = !sel->enabled;
      reg_->markChanged();
    }
    ImGui::PopID();
    dl->AddText(ImVec2(tx + 50, y + 1), C_TEXT, sel->enabled ? "Включено" : "Выключено");
    y += 28;
  }

  // settings in declaration order; keybinds go last
  y += 6;
  for (auto& s : sel->settings()) {
    switch (s.type) {
      case mods::Setting::Type::Bool:
        drawToggleRow(s.label.c_str(), &s.b, tx, &y);
        break;
      case mods::Setting::Type::Float:
        drawSliderRow(s.label.c_str(), &s.f, s.min, s.max, s.fmt, tx, &y, cw);
        break;
      case mods::Setting::Type::Enum: drawEnumRow(s, tx, &y, cw); break;
      case mods::Setting::Type::Key: break;
    }
  }
  y += 6;
  for (auto& s : sel->settings())
    if (s.type == mods::Setting::Type::Key) drawKeyRow(s, tx, &y, cw);

  // live data section
  if (py + ph - y > 50 && !sel->liveRows.empty()) {
    dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + cw, y + 2), C_TILE);
    y += 8;
    dl->AddText(ImVec2(tx, y), C_DIM, "ЖИВЫЕ ДАННЫЕ");
    y += 18;
    for (auto& [k, v] : sel->liveRows) {
      if (y + 16 > py + ph - PAD) break;
      dl->AddText(ImVec2(tx, y), C_TEXT, (k.empty() ? v : k + ": " + v).c_str());
      y += 16;
    }
  }
}

} // namespace ui
