// deepseek-balance v0.2 —— 渲染器：DirectComposition + D3D11 翻转模型
//
// 路径由 A0 实验裁决（设计文档 §12.3.3b）：两路视觉一致，代价差约 15 倍，
// 定案走这条路。四条在 A0 里踩过、必须遵守的规矩：
//   1. 用 CreateSwapChainForComposition 的窗口必须带 WS_EX_NOREDIRECTIONBITMAP
//   2. BeginDraw / EndDraw 不能嵌套（嵌套会让 EndDraw 返回 WRONG_STATE 且整帧被丢弃）
//   3. 画布尺寸用 GetDpiForWindow，不是 GetDpiForSystem
//   4. DPI 变更时重建交换链与画布

#pragma once

#include <windows.h>

#include <string>

#include <cstdint>

namespace dshb {

// 画布尺寸（DIP）：实体视觉区 315x129，四周各 80 的外扩透明余量 → 475x289
constexpr int kEntityWidthDip = 315;
constexpr int kEntityHeightDip = 129;
constexpr int kMarginDip = 80;
constexpr int kCanvasWidthDip = kEntityWidthDip + kMarginDip * 2;
constexpr int kCanvasHeightDip = kEntityHeightDip + kMarginDip * 2;
constexpr int kCornerRadiusDip = 12;

// 设计基准色 #6c89f6（充足档）
constexpr uint32_t kBaseColorBgra = 0xF6896C;  // 0xAABBGGRR 里的 RGB 部分

struct CanvasSize {
    int widthPx = 0;
    int heightPx = 0;
    float scale = 1.0f;
};

class Renderer {
public:
    ~Renderer();

    // 按窗口所在显示器的 DPI 算出画布物理尺寸。必须在 Create 之前调用。
    static CanvasSize SizeForWindow(HWND hwnd);

    bool Create(HWND hwnd, const CanvasSize& size);
    void Destroy();

    // 重建画布（DPI 变更时用）
    bool Resize(HWND hwnd);

    // 画一帧并提交。返回 Present 的结果（便于这里直接发现失败）。
    HRESULT RenderFrame(double elapsedSeconds);

    // ★ 输入形状：DirectComposition 的窗口矩形整体都会吃鼠标事件，
    //   透明余量不会自动穿透（A4b 实测）。所以必须自己把窗口区域收到实体区之内。
    //   particlesSpillout = true 时扩到整个画布，用于粒子飞出实体区的那段时间。
    bool ApplyInputRegion(bool particlesSpillout);

    // ★ 离屏导帧：用同一份绘制代码渲染到一张离屏位图并写成 PNG。
    //   用途是"用像素说话"——居中错位、颜色、残影、粒子越界这些肉眼判不了的东西，
    //   都要靠它量。它渲染的是同一套画面，所以屏幕上的错在 PNG 里也会错。
    bool ExportFrame(const wchar_t* path, double elapsedSeconds);

    // ★ 预乘自检（A8c）：渲一张"50% 透明纯红块"到位图，并回读像素。
    //   预期 (128,0,0,128) 而不是 (255,0,0,128)——后者说明没预乘。
    bool PremulProbe(uint8_t* outBgra, int* outX, int* outY);

    // ★ 有人重复启动了程序（A11）。这个窗口不会抢焦点、也不进 Alt+Tab，
    //   所以"把已有窗口提到前台"没有落点——只能用一次可见反馈代替。
    void BeginActivationFlash(double nowSeconds);
    double ActivationFlash(double nowSeconds) const;

    // ★ 调试浮层（B6 的可见部分）：只在带调试开关时才有内容。
    //   它是眼睛，不是产品界面——正文的中文文案属于 C 阶段。
    //   文本统一用宽字符：DirectWrite 直接吃 wchar_t，省掉一次运行时编码转换
    //   （那类"图省事"的转换最容易在中文上出错）。
    void SetDebugText(std::wstring text) { debugText_ = std::move(text); }
    const std::wstring& debugText() const { return debugText_; }

    const CanvasSize& size() const { return size_; }
    bool ready() const { return ready_; }

private:
    HWND hwnd_ = nullptr;
    CanvasSize size_{};
    bool ready_ = false;
    bool spillout_ = false;
    double flashStart_ = -1000.0;   // 负值表示"没在闪"
    std::wstring debugText_;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace dshb
