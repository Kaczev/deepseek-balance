// deepseek-balance v0.2 —— 渲染器实现
//
// 这一步只画"骨架可见的东西"：圆角面板 + 一个跟着时间走的方块。
// 数字、曲线、颜色、心跳都是后面步骤的事（实施步骤 C/D/E）。

#include "renderer.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>

#include <cmath>   // std::fmod

namespace dshb {

namespace {

constexpr float kPanelOpacity = 0.95f;

double NowSeconds() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

}  // namespace

struct Renderer::Impl {
    ID3D11Device* device = nullptr;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    ID2D1Bitmap1* target = nullptr;
    IDCompositionDevice* compDevice = nullptr;
    IDCompositionTarget* compTarget = nullptr;
    IDCompositionVisual* compVisual = nullptr;

    void ReleaseAll() {
        if (target) { target->Release(); target = nullptr; }
        if (dc) { dc->Release(); dc = nullptr; }
        if (d2dDevice) { d2dDevice->Release(); d2dDevice = nullptr; }
        if (d2dFactory) { d2dFactory->Release(); d2dFactory = nullptr; }
        if (compVisual) { compVisual->Release(); compVisual = nullptr; }
        if (compTarget) { compTarget->Release(); compTarget = nullptr; }
        if (compDevice) { compDevice->Release(); compDevice = nullptr; }
        if (swapchain) { swapchain->Release(); swapchain = nullptr; }
        if (dxgiDevice) { dxgiDevice->Release(); dxgiDevice = nullptr; }
        if (device) { device->Release(); device = nullptr; }
    }
};

Renderer::~Renderer() { Destroy(); }

CanvasSize Renderer::SizeForWindow(HWND hwnd) {
    // ★ 用窗口所在显示器的 DPI。GetDpiForSystem 在多屏不同缩放时会把画布按主屏算，
    //   而窗口可能落在另一块屏上，内容就会被裁或留空边（A0 实验室用的就是后者）。
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) dpi = GetDpiForSystem();
    CanvasSize s;
    s.scale = static_cast<float>(dpi) / 96.0f;
    s.widthPx = static_cast<int>(kCanvasWidthDip * s.scale + 0.5f);
    s.heightPx = static_cast<int>(kCanvasHeightDip * s.scale + 0.5f);
    return s;
}

bool Renderer::Create(HWND hwnd, const CanvasSize& size) {
    Destroy();
    hwnd_ = hwnd;
    size_ = size;
    impl_ = new Impl();
    Impl& d = *impl_;

    // ---- D3D11 设备 ----
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &d.device, &got, nullptr))) {
        return false;
    }
    if (FAILED(d.device->QueryInterface(IID_PPV_ARGS(&d.dxgiDevice)))) return false;

    // ---- 翻转模型交换链 + 预乘 alpha（每像素透明的关键）----
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool ok = false;
    do {
        if (FAILED(d.dxgiDevice->GetAdapter(&adapter))) break;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) break;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(size_.widthPx);
        desc.Height = static_cast<UINT>(size_.heightPx);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory->CreateSwapChainForComposition(d.device, &desc, nullptr, &d.swapchain))) {
            break;
        }
        ok = true;
    } while (false);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (!ok) return false;

    // ---- D2D 设备上下文绑到后备缓冲 ----
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 nullptr, reinterpret_cast<void**>(&d.d2dFactory)))) {
        return false;
    }
    if (FAILED(d.d2dFactory->CreateDevice(d.dxgiDevice, &d.d2dDevice))) return false;
    if (FAILED(d.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d.dc))) return false;

    IDXGISurface* surface = nullptr;
    if (FAILED(d.swapchain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const HRESULT hrBitmap = d.dc->CreateBitmapFromDxgiSurface(surface, &props, &d.target);
    surface->Release();
    if (FAILED(hrBitmap)) return false;
    d.dc->SetTarget(d.target);

    // ---- DirectComposition：把交换链挂到窗口上 ----
    if (FAILED(DCompositionCreateDevice(d.dxgiDevice, __uuidof(IDCompositionDevice),
                                        reinterpret_cast<void**>(&d.compDevice)))) {
        return false;
    }
    if (FAILED(d.compDevice->CreateTargetForHwnd(hwnd_, TRUE, &d.compTarget))) return false;
    if (FAILED(d.compDevice->CreateVisual(&d.compVisual))) return false;
    if (FAILED(d.compVisual->SetContent(d.swapchain))) return false;
    if (FAILED(d.compTarget->SetRoot(d.compVisual))) return false;
    if (FAILED(d.compDevice->Commit())) return false;

    // 区域必须跟着画布走：DPI 变更或重建之后都要重设，否则可点区域会和画面对不上
    ApplyInputRegion(false);

    ready_ = true;
    return true;
}

bool Renderer::Resize(HWND hwnd) {
    if (!impl_) return false;
    const CanvasSize want = SizeForWindow(hwnd);
    if (want.widthPx == size_.widthPx && want.heightPx == size_.heightPx) return true;
    // 交换链尺寸变了就得整块重建：翻转模型不允许对后备缓冲直接 ResizeBuffers 后再绑 D2D 目标，
    // 重建比就地改更省事，也更不容易留下悬空的目标位图。
    return Create(hwnd, want);
}

void Renderer::Destroy() {
    if (impl_) {
        impl_->ReleaseAll();
        delete impl_;
        impl_ = nullptr;
    }
    ready_ = false;
}

bool Renderer::ApplyInputRegion(bool particlesSpillout) {
    if (!impl_ || !hwnd_) return false;
    if (spillout_ == particlesSpillout && spillout_ == true) {
        // 已经扩到全画布，不用重复设置
    }

    // 区域坐标是窗口坐标（无边框窗口的窗口矩形 == 客户区）
    int left = 0, top = 0, right = size_.widthPx, bottom = size_.heightPx;
    if (!particlesSpillout) {
        const int m = static_cast<int>(kMarginDip * size_.scale + 0.5f);
        left = m;
        top = m;
        right = m + static_cast<int>(kEntityWidthDip * size_.scale + 0.5f);
        bottom = m + static_cast<int>(kEntityHeightDip * size_.scale + 0.5f);
    }
    const int radius = particlesSpillout
        ? 0
        : static_cast<int>(kCornerRadiusDip * size_.scale + 0.5f);

    HRGN region = particlesSpillout
        ? CreateRectRgn(left, top, right, bottom)
        : CreateRoundRectRgn(left, top, right + 1, bottom + 1, radius * 2, radius * 2);
    if (!region) return false;

    // SetWindowRgn 成功后区域归系统所有，不能再 DeleteObject
    if (SetWindowRgn(hwnd_, region, TRUE) == 0) {
        DeleteObject(region);
        return false;
    }
    spillout_ = particlesSpillout;
    return true;
}

HRESULT Renderer::RenderFrame(double elapsedSeconds) {
    if (!ready_ || !impl_) return E_FAIL;
    Impl& d = *impl_;

    // 注意：EndDraw 是唯一会报错的一步；BeginDraw 返回 void。
    // 这里自己 BeginDraw/EndDraw，画内容的函数不要再各调一次（A0 的坑）。
    d.dc->BeginDraw();
    d.dc->Clear(D2D1::ColorF(0, 0.0f));

    const float s = size_.scale;
    const float cx = kMarginDip * s;
    const float cy = kMarginDip * s;
    const float ew = kEntityWidthDip * s;
    const float eh = kEntityHeightDip * s;
    const float radius = kCornerRadiusDip * s;

    const D2D1_COLOR_F base = D2D1::ColorF(108.0f / 255, 137.0f / 255, 246.0f / 255, kPanelOpacity);
    const D2D1_COLOR_F premul =
        D2D1::ColorF(base.r * base.a, base.g * base.a, base.b * base.a, base.a);

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(d.dc->CreateSolidColorBrush(premul, &brush)) && brush) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(cx, cy, cx + ew, cy + eh), radius, radius);
        d.dc->FillRoundedRectangle(rr, brush);
        brush->Release();
    }

    // 一个跟着时间走的方块：证明帧循环在跑、画面在刷新（后面会被真正的曲线取代）
    ID2D1SolidColorBrush* white = nullptr;
    if (SUCCEEDED(d.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &white)) && white) {
        const float side = 28.0f * s;
        const float travel = ew - side * 2;
        const float t = static_cast<float>(elapsedSeconds * 60.0);
        const float x = cx + side + std::fmod(t, travel);
        const float y = cy + eh * 0.5f - side * 0.5f;
        d.dc->FillRectangle(D2D1::RectF(x, y, x + side, y + side), white);
        white->Release();
    }

    const HRESULT hrEnd = d.dc->EndDraw();
    if (FAILED(hrEnd)) {
        // A0 的教训：这里失败时 Present 仍会返回 S_OK，画面上却什么都没有，
        // 所以必须在 EndDraw 这一层就能看见失败。
        return hrEnd;
    }
    return d.swapchain->Present(1, 0);
}

}  // namespace dshb
