#include "overlay/overlay_window.h"

#include <spdlog/spdlog.h>

namespace overlay {

namespace {
constexpr const wchar_t* kClassName = L"ThresholdOverlay";
}

bool OverlayWindow::create(HINSTANCE hInst, const wchar_t* title) {
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = &OverlayWindow::wndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.cbWndExtra = sizeof(OverlayWindow*);
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    spdlog::error("RegisterClassExW failed, gle={}", GetLastError());
    return false;
  }

  DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
             WS_EX_NOACTIVATE;
  hwnd_ = CreateWindowExW(ex, kClassName, title, WS_POPUP, 100, 100, 640, 400, nullptr,
                          nullptr, hInst, nullptr);
  if (!hwnd_) {
    spdlog::error("CreateWindowExW(overlay) failed, gle={}", GetLastError());
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  return true;
}

void OverlayWindow::destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void OverlayWindow::show(bool on) {
  if (hwnd_) ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlayWindow::setClickThrough(bool on) {
  if (!hwnd_ || on == clickThrough_) return;
  LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
  if (on) {
    ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  } else {
    ex &= ~((LONG_PTR)WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  }
  SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
  // When becoming interactive, allow activation so ImGui receives focus/clicks.
  // When going click-through, keep the window non-activating to avoid stealing
  // focus from the game.
  UINT swp = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED;
  if (on) swp |= SWP_NOACTIVATE;
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, swp);
  if (!on) {
    // Best-effort: hotkey path should allow this; fallback to SetFocus.
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
  }
  clickThrough_ = on;
}

bool OverlayWindow::setBounds(int x, int y, int w, int h) {
  if (!hwnd_) return false;
  RECT r = {};
  GetWindowRect(hwnd_, &r);
  bool changed = r.left != x || r.top != y || (r.right - r.left) != w || (r.bottom - r.top) != h;
  if (changed)
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  return changed;
}

LRESULT CALLBACK OverlayWindow::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = (OverlayWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (self && self->hook_) {
    bool handled = false;
    LRESULT r = self->hook_(h, msg, wp, lp, handled);
    if (handled) return r;
  }
  switch (msg) {
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (self) self->hwnd_ = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

} // namespace overlay
