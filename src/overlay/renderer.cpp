#include "overlay/renderer.h"
#include "ui/menu.h"

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace overlay {

using Microsoft::WRL::ComPtr;

bool Renderer::init(HWND hwnd, int w, int h) {
  hwnd_ = hwnd;
  w_ = w;
  h_ = h;

  ComPtr<IDXGIFactory2> factory;
  {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
    if (FAILED(hr)) {
      spdlog::error("D3D11CreateDevice failed: 0x{:08X}", (unsigned)hr);
      return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dev_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
      spdlog::error("failed to acquire DXGI factory");
      return false;
    }
  }

  // Composition swapchain with true per-pixel premultiplied alpha, bound to
  // the window via DirectComposition. A plain hwnd swapchain (AlphaMode
  // IGNORE) DirectFlips straight to the monitor and renders opaque black —
  // the DWM never applies alpha to it.
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = (UINT)w;
  sd.Height = (UINT)h;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  HRESULT hr = factory->CreateSwapChainForComposition(dev_.Get(), &sd, nullptr, &swap_);
  if (FAILED(hr)) {
    spdlog::error("CreateSwapChainForComposition failed: 0x{:08X}", (unsigned)hr);
    return false;
  }

  ComPtr<IDXGIDevice> dxgiDev;
  if (FAILED(dev_.As(&dxgiDev)) ||
      FAILED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp_)))) {
    spdlog::error("DCompositionCreateDevice failed: 0x{:08X}", (unsigned)GetLastError());
    return false;
  }
  if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE /*topmost*/, &dcompTarget_)) ||
      FAILED(dcomp_->CreateVisual(&dcompVisual_)) ||
      FAILED(dcompVisual_->SetContent(swap_.Get())) ||
      FAILED(dcompTarget_->SetRoot(dcompVisual_.Get())) || FAILED(dcomp_->Commit())) {
    spdlog::error("DirectComposition target/visual setup failed: 0x{:08X}", (unsigned)hr);
    return false;
  }

  if (!createTargets()) {
    spdlog::error("render target creation failed");
    return false;
  }

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  // Segoe UI with Cyrillic
  {
    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x04FF,
                                     0x2000, 0x206F, 0x2190, 0x22FF, 0};
    std::string fontPath = std::string(winDir) + "\\Fonts\\segoeui.ttf";
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f, nullptr, ranges);
  }

  // Segoe MDL2 Assets for Metro icons (PUA glyphs 0xE000-0xF8FF)
  {
    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string mdl2Path = std::string(winDir) + "\\Fonts\\segmdl2.ttf";
    static const ImWchar mdl2Range[] = {0xE700, 0xE800, 0};
    io.Fonts->AddFontFromFileTTF(mdl2Path.c_str(), 15.0f, nullptr, mdl2Range);
  }

  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(dev_.Get(), ctx_.Get());

  // provide the icon font to the menu (second font = MDL2)
  if (io.Fonts->Fonts.Size > 1) ui::g_iconFont = io.Fonts->Fonts[1];

  style();
  return true;
}

bool Renderer::createTargets() {
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  rtv_.Reset();
  return SUCCEEDED(dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

void Renderer::style() {
  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 6.f;
  s.FrameRounding = 4.f;
  s.GrabRounding = 4.f;
  s.ScrollbarRounding = 4.f;
  s.WindowBorderSize = 1.f;
}

void Renderer::beginFrame() {
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  const float clear[4] = {0.f, 0.f, 0.f, 0.f};
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);
  ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
}

void Renderer::endFrame() {
  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  swap_->Present(0, 0); // no vsync: minimize overlay latency
}

void Renderer::resize(int w, int h) {
  if (!swap_ || w <= 0 || h <= 0 || (w == w_ && h == h_)) return;
  w_ = w;
  h_ = h;
  rtv_.Reset();
  if (FAILED(swap_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0)))
    spdlog::warn("ResizeBuffers failed");
  createTargets();
}

void Renderer::shutdown() {
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  rtv_.Reset();
  dcompVisual_.Reset();
  dcompTarget_.Reset();
  dcomp_.Reset();
  swap_.Reset();
  ctx_.Reset();
  dev_.Reset();
}

} // namespace overlay
