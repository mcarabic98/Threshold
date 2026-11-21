#pragma once
#include <functional>
#include <windows.h>

namespace overlay {

// Borderless transparent topmost window sized to the Minecraft client rect.
// Lives in its own process (no injection): DWM composes it over the game.
class OverlayWindow {
public:
  using Hook = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>;

  bool create(HINSTANCE hInst, const wchar_t* title);
  void destroy();
  HWND hwnd() const { return hwnd_; }

  void show(bool on);
  void setClickThrough(bool on);
  // Position overlay exactly over the MC client rect; true if moved/resized.
  bool setBounds(int x, int y, int w, int h);

  // Called before default handling: input routing, hotkeys, ImGui.
  Hook hook_;

private:
  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

  HWND hwnd_ = nullptr;
  bool clickThrough_ = true;
};

} // namespace overlay
