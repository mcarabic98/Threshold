#pragma once
#include <imgui.h>

// Metro palette shared by the menu chrome and module tile metadata.
namespace ui {

inline constexpr ImU32 C_BG = IM_COL32(0x0e, 0x11, 0x16, 255);
inline constexpr ImU32 C_PANEL = IM_COL32(0x16, 0x1b, 0x22, 255);
inline constexpr ImU32 C_TILE = IM_COL32(0x25, 0x2c, 0x36, 255);
inline constexpr ImU32 C_TILE_HOVER = IM_COL32(0x2d, 0x35, 0x41, 255);
inline constexpr ImU32 C_TEXT = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 C_DIM = IM_COL32(0x8b, 0x95, 0xa3, 255);
inline constexpr ImU32 C_ACCENT = IM_COL32(0x00, 0xa4, 0xef, 255);

// tile accent colors (Metro palette)
inline constexpr ImU32 C_BLUE = IM_COL32(0x00, 0xa4, 0xef, 255);
inline constexpr ImU32 C_GREEN = IM_COL32(0x7f, 0xba, 0x00, 255);
inline constexpr ImU32 C_RED = IM_COL32(0xf2, 0x50, 0x22, 255);
inline constexpr ImU32 C_YELLOW = IM_COL32(0xff, 0xb9, 0x00, 255);
inline constexpr ImU32 C_PURPLE = IM_COL32(0x8e, 0x44, 0xad, 255);
inline constexpr ImU32 C_TEAL = IM_COL32(0x00, 0xab, 0xa9, 255);
inline constexpr ImU32 C_DARKRED = IM_COL32(0xe5, 0x14, 0x00, 255);

} // namespace ui
