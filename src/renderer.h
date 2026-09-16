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

#include "widget_display.h"

#include "tuning.h"

#include <cstdint>

namespace dshb {

// 画布尺寸（DIP）：实体视觉区 315x129，四周各 80 的外扩透明余量 → 475x289
// ★ 所有外观常量集中在 tuning.h（字号/面板尺寸/圆角/颜色/数字落点与间距）。
//   要调外观就改那一个文件，不要在这个头文件里加新的魔法数字。
// 画布尺寸由实体区 + 四周外扩余量推出（默认 315x129 + 80×2 = 475x289）。
constexpr int kCanvasWidthDip = kEntityWidthDip + kMarginDip * 2;
constexpr int kCanvasHeightDip = kEntityHeightDip + kMarginDip * 2;

struct CanvasSize {
    int widthPx = 0;
    int heightPx = 0;
    float scale = 1.0f;
};

// 币种符号在屏幕上的矩形（**像素**坐标）。点击命中测试要用。
// 每帧由渲染层写入；还没画过符号时 valid=false。
struct SymbolRect {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    bool valid = false;
};
SymbolRect CurrencySymbolRect();

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

    // ★ 正文（C 阶段）：标题/状态行、余额数字、币种符号、清零预估。
    //   渲染层只认这个结构——它不关心余额是怎么来的，也不该知道
    //   "余额为 0" 与 "查不到" 有什么区别（那是状态机的事）。
    void SetWidgetFrame(const WidgetFrame& frame);
    const WidgetFrame& widgetFrame() const { return widget_; }

    const CanvasSize& size() const { return size_; }
    bool ready() const { return ready_; }

private:
    HWND hwnd_ = nullptr;
    CanvasSize size_{};
    bool ready_ = false;
    bool spillout_ = false;
    double flashStart_ = -1000.0;   // 负值表示"没在闪"
    std::wstring debugText_;
    WidgetFrame widget_{};
    struct Impl;
    Impl* impl_ = nullptr;
};

// 布局诊断开关（临时）：打开后把"算出来画在哪、实际量到多宽"记在内存里。
// ★ 写文件必须由外面在**绘制结束之后**调用 DumpLayoutProbe 完成：
//   在绘制路径里做文件 I/O 会让进程崩溃（实测 0xC0000409，试了两次）。
// 数字绘制模式：0 = 整串一次画完（默认）；1 = 逐位按坐标画（修正公式）；2 = 逐位按所有者的原式画（对照用）
extern int g_digitDrawMode;
// 最近一次绘制时量到的行距 h（相邻两个数字的间距，DIP）。0 = 还没画过。
float LastLinePitchDip();
void SetLayoutProbe(bool on);

    // 氛围曲线开关（--no-curve：关掉后文字位置必须逐像素不变，用于 A/B 对比）
    void SetCurveEnabled(bool on);

    // 鼠标是否悬停在币种符号上（悬停时符号变暗一档，提示可点击）
    void SetSymbolHover(bool on);
void DumpLayoutProbe();

}  // namespace dshb
