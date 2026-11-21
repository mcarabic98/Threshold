#include "ui/menu.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x + b.x, a.y + b.y);
}
static ImVec2 operator-(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x - b.x, a.y - b.y);
}

namespace ui {

ImFont* g_iconFont = nullptr;

// ---- Metro palette ----
static constexpr ImU32 C_BG = IM_COL32(0x0e, 0x11, 0x16, 255);
static constexpr ImU32 C_PANEL = IM_COL32(0x16, 0x1b, 0x22, 255);
static constexpr ImU32 C_TILE = IM_COL32(0x25, 0x2c, 0x36, 255);
static constexpr ImU32 C_TILE_HOVER = IM_COL32(0x2d, 0x35, 0x41, 255);
static constexpr ImU32 C_TEXT = IM_COL32(255, 255, 255, 255);
static constexpr ImU32 C_DIM = IM_COL32(0x8b, 0x95, 0xa3, 255);
static constexpr ImU32 C_ACCENT = IM_COL32(0x00, 0xa4, 0xef, 255);

// tile accent colors (Metro palette)
static constexpr ImU32 C_BLUE = IM_COL32(0x00, 0xa4, 0xef, 255);
static constexpr ImU32 C_GREEN = IM_COL32(0x7f, 0xba, 0x00, 255);
static constexpr ImU32 C_RED = IM_COL32(0xf2, 0x50, 0x22, 255);
static constexpr ImU32 C_YELLOW = IM_COL32(0xff, 0xb9, 0x00, 255);
static constexpr ImU32 C_PURPLE = IM_COL32(0x8e, 0x44, 0xad, 255);
static constexpr ImU32 C_TEAL = IM_COL32(0x00, 0xab, 0xa9, 255);
static constexpr ImU32 C_DARKRED = IM_COL32(0xe5, 0x14, 0x00, 255);

// layout
static constexpr float NAV_W = 140.f;
static constexpr float PANEL_W = 230.f;
static constexpr float HEADER_H = 56.f;
static constexpr float TILE = 94.f;
static constexpr float GAP = 6.f;
static constexpr float PAD = 12.f;

// modules (local layout, shadows header ModuleDef which is unused here)

static ModuleDef g_modules[] = {
    {"esp",     "ESP",       0xE81C, C_RED,    "Combat", "Подсветка сущностей: мобы, игроки, предметы. Цвет по типу.", true, false, "—"},
    {"aim",     "Aimbot",    0xE7B3, C_GREEN,  "Combat", "Плавное наведение на ближайшую цель.", false, false, "R"},
    {"hitbox",  "Хитбоксы",  0xE7A8, C_TEAL,   "Combat", "Показ хитбоксов существ в мире.", false, false, "F3+B"},
    {"bright",  "Яркость",   0xE706, C_YELLOW, "Визуал", "Полная яркость в тёмных местах.", false, false, "B"},
    {"zoom",    "Зум",       0xE721, C_BLUE,   "Визуал", "Приближение камеры по клавише.", false, false, "C"},
    {"fps",     "FPS",       0xE9D9, C_BLUE,   "HUD",    "Частота кадров на экране.", true, true, "—"},
    {"coords",  "Координаты",0xE7C3, C_PURPLE, "HUD",    "Текущие XYZ и направление.", true, true, "—"},
    {"keys",    "Клавиши",   0xE765, C_DARKRED,"HUD",    "Нажатия WASD и мыши.", false, false, "—"},
    {"ping",    "Пинг",      0xE8BD, C_TEAL,   "HUD",    "Задержка до сервера.", false, false, "—"},
};
static constexpr int kNumModules = sizeof(g_modules) / sizeof(g_modules[0]);

static ModuleDef* findMod(const char* id) {
  for (auto& m : g_modules)
    if (m.id == id) return &m;
  return nullptr;
}

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

static void drawSliderMetro(ImDrawList* dl, ImVec2 pos, float w, float v, float mn, float mx,
                            ImU32 color) {
  float frac = (v - mn) / (mx - mn);
  frac = std::clamp(frac, 0.f, 1.f);
  float barH = 3.f;
  float cy = pos.y + 8;
  dl->AddRectFilled(ImVec2(pos.x, cy - barH / 2), ImVec2(pos.x + w, cy + barH / 2), C_TILE);
  dl->AddRectFilled(ImVec2(pos.x, cy - barH / 2), ImVec2(pos.x + w * frac, cy + barH / 2), color);
  dl->AddRectFilled(ImVec2(pos.x + w * frac - 3, cy - 8), ImVec2(pos.x + w * frac + 3, cy + 8),
                    color);
}


// ---- Menu implementation ----

void Menu::draw(MenuState& st) {
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
  drawGrid(st);
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

void Menu::drawTile(ModuleDef& m, ImVec2 pos, ImVec2 size, bool selected) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = m.enabled ? m.color : C_TILE;
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
  drawIcon(dl, m.iconGlyph, pos + ImVec2(10, size.y * 0.28f), 28.f, txtCol);

  // name (bottom left)
  dl->AddText(ImVec2(pos.x + 8, pos.y + size.y - 20), txtCol, m.name.c_str());

  // live counter (bottom right)
  if (m.enabled && !m.live.empty()) {
    ImVec2 ls = ImGui::CalcTextSize(m.live.c_str());
    dl->AddText(ImVec2(pos.x + size.x - ls.x - 8, pos.y + size.y - 26),
                IM_COL32(255, 255, 255, 180), m.live.c_str());
  }

  // click handling
  ImGui::SetCursorScreenPos(pos);
  ImGui::PushID(m.id.c_str());
  ImGui::InvisibleButton("##tile", size);
  if (ImGui::IsItemHovered()) dl->AddRectFilled(pos, pos + size, IM_COL32(255, 255, 255, 15));
  if (ImGui::IsItemClicked()) {
    if (selModuleId_ == m.id) m.enabled = !m.enabled;
    selModuleId_ = m.id;
  }
  ImGui::PopID();
}

void Menu::drawGrid(MenuState& st) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  float gx = wp.x + NAV_W;
  float gw = ws.x - NAV_W - PANEL_W;

  // sync ESP live counter
  if (auto* esp = findMod("esp")) {
    esp->live = esp->enabled ? std::format("{} ent", st.espEntities) : "";
  }

  float x = gx + PAD;
  float y = wp.y + HEADER_H + PAD;
  float maxX = gx + gw - PAD;
  float maxY = wp.y + ws.y - PAD;

  for (int i = 0; i < kNumModules; ++i) {
    auto& m = g_modules[i];
    if (selCategory_[0] != 0 && strcmp(selCategory_.c_str(), "Все") != 0 &&
        m.category != selCategory_)
      continue;
    float tw = m.wide ? TILE * 2 + GAP : TILE;
    if (x + tw > maxX) {
      x = gx + PAD;
      y += TILE + GAP;
    }
    if (y + TILE > maxY) break;
    drawTile(m, ImVec2(x, y), ImVec2(tw, TILE), selModuleId_ == m.id);
    x += tw + GAP;
  }

  // dummy to keep ImGui happy
  ImGui::SetCursorScreenPos(ImVec2(gx, maxY));
  ImGui::Dummy(ImVec2(gw, 0));
}

void Menu::drawSwitch(ImVec2 pos, bool on) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  drawSwImpl(dl, pos, on);
}

void Menu::drawToggleRow(const char* label, bool* val, float tx, float* y) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(tx, *y));
  ImGui::PushID(label);
  ImGui::InvisibleButton("##tgl", ImVec2(40, 16));
  drawSwImpl(dl, ImVec2(tx, *y), *val);
  if (ImGui::IsItemClicked()) *val = !*val;
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
  ImGui::PopID();
  ImGui::PopItemWidth();
  *y += 24;
}

void Menu::drawTextRow(const char* label, const char* val, float tx, float* y) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(tx, *y), C_DIM, label);
  dl->AddText(ImVec2(tx + 100, *y), C_TEXT, val);
  *y += 18;
}

void Menu::drawPanel(MenuState& st) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 ws = ImGui::GetWindowSize();
  float px = wp.x + ws.x - PANEL_W;
  float py = wp.y + HEADER_H;
  float ph = ws.y - HEADER_H;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  dl->AddRectFilled(ImVec2(px, py), ImVec2(wp.x + ws.x, py + ph), C_PANEL);

  ModuleDef* sel = findMod(selModuleId_.c_str());
  float y = py + 14;
  float tx = px + PAD;
  float cw = PANEL_W - PAD * 2;

  if (!sel) {
    dl->AddText(ImVec2(tx, y), C_DIM, "Выберите модуль");
    return;
  }

  // category + name
  dl->AddText(ImVec2(tx, y), C_ACCENT, sel->category.c_str());
  y += 18;
  dl->AddText(nullptr, 22.f, ImVec2(tx, y), C_TEXT, sel->name.c_str());
  y += 30;

  // description (wrapped)
  {
    std::string dStr = sel->desc;
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
    if (ImGui::IsItemClicked()) sel->enabled = !sel->enabled;
    ImGui::PopID();
    dl->AddText(ImVec2(tx + 50, y + 1), C_TEXT, sel->enabled ? "Включено" : "Выключено");
    y += 28;
  }

  // ESP-specific settings
  if (sel->id == "esp") {
    y += 6;
    drawSliderRow("FOV", &st.espFov, 30.f, 110.f, "%.0f°", tx, &y, cw);
    drawSliderRow("Дистанция", &st.espDistance, 16.f, 256.f, "%.0f м", tx, &y, cw);
    y += 4;
    drawToggleRow("Мобы", &st.espShowMobs, tx, &y);
    drawToggleRow("Игроки", &st.espShowPlayers, tx, &y);
    drawToggleRow("Предметы", &st.espShowItems, tx, &y);
    drawToggleRow("Прочее", &st.espShowMisc, tx, &y);
    y += 6;
  }

  // key binding
  dl->AddText(ImVec2(tx, y), C_DIM, "Клавиша");
  y += 16;
  {
    ImGui::SetCursorScreenPos(ImVec2(tx, y));
    ImGui::PushID("panel_key");
    ImVec2 ksz = ImVec2(cw, 26);
    dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx, y) + ksz, C_TILE);
    dl->AddText(ImVec2(tx + 8, y + 5), C_TEXT,
                listenKey_ ? "Нажмите клавишу…" : sel->key.c_str());
    ImGui::InvisibleButton("##key", ksz);
    if (ImGui::IsItemClicked()) listenKey_ = !listenKey_;
    ImGui::PopID();
    y += 32;
  }

  // live data section
  if (py + ph - y > 50) {
    dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + cw, y + 2), C_TILE);
    y += 8;
    dl->AddText(ImVec2(tx, y), C_DIM, "ЖИВЫЕ ДАННЫЕ");
    y += 18;
    if (sel->id == "esp") {
      char buf[128];
      snprintf(buf, sizeof(buf), "Сущностей: %zu", st.espEntities);
      dl->AddText(ImVec2(tx, y), C_TEXT, buf);
      y += 16;
      snprintf(buf, sizeof(buf), "FOV: %.0f°", st.espFov);
      dl->AddText(ImVec2(tx, y), C_TEXT, buf);
      y += 16;
      snprintf(buf, sizeof(buf), "Дистанция: %.0f м", st.espDistance);
      dl->AddText(ImVec2(tx, y), C_TEXT, buf);
      y += 16;
    } else if (sel->id == "coords") {
      dl->AddText(ImVec2(tx, y), C_TEXT, st.playerLine.c_str());
      y += 16;
    }
  }
}

} // namespace ui
