#pragma once
#include <cstdint>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace overlay {

// Own D3D11 device in this process. The swapchain is a composition swapchain
// (DXGI_ALPHA_MODE_PREMULTIPLIED) bound to the window through
// DirectComposition: DWM composites per-pixel alpha over whatever is under
// the overlay window, including the game.
class Renderer {
public:
  bool init(HWND hwnd, int w, int h);
  void shutdown();

  void beginFrame();
  void endFrame();
  void resize(int w, int h);

  int width() const { return w_; }
  int height() const { return h_; }

private:
  bool createTargets();
  void style();

  HWND hwnd_ = nullptr;
  int w_ = 0, h_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
};

} // namespace overlay
