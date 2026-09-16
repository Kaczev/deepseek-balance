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

// 探针出口的签名里用到（renderer.h 故意不引 d2d1.h：它对外只是一组包装）。
struct ID2D1RenderTarget;

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

// ---------------------------------------------------------------------------
// 内蒙光的 alpha 剖面（"E 蒙光.md" §3.2、"E 施工单.md" 甲.4）
// ---------------------------------------------------------------------------
//  这是烘遮罩时用的**同一支函数**，所以"文档里的数"与"屏幕上的像素"不可能各说
//  一套。它不碰任何 D2D 状态，可以被离线链接（探针就是这么核对它的）。
//  ★ 输入输出都是 DIP：面板外一律 0（本层是**内**蒙光，不是外面那圈外光晕）。
//    剖面 = 整板底噪 + 内侧 5 DIP 内唇 + 底部 17 DIP 透光。
//  ★ 返回值**不含**强度倍率 k(R,D)：k 在 Pick() 里乘进彩色层，所以清一色遮罩
//    永远不用重烘。
float InnerGlowAlphaAt(float xDip, float yDip);

// ---------------------------------------------------------------------------
// 内蒙光的重烘记账（"E 施工单.md" 甲.4 的代价问题）
// ---------------------------------------------------------------------------
//  为什么要记：光强是**烘进彩色层**的，所以 k 每跨过 1/255 就要重烘一整张
//  475x289 的位图。对一个 60 fps 的挂件，这可能是真正的开销 —— 而"我觉得不会"
//  不是凭据。这里记的是：覆盖率烘了几次（应当永远是 1）、彩色层烘了几次、
//  彩色层单次最坏耗时（毫秒）、以及颜色滑行一共跑了几帧。
//  ★ 每帧都会变的两帧之间只需读一次；字段是单调累加的，不重置。
struct InnerGlowBakeCounters {
    int coverageBakes = 0;        // 覆盖率烘的次数（启动一次 = 1）
    double coverageWorstMs = 0.0; // 覆盖率那一次花了多少毫秒
    int tintBakes = 0;            // 彩色层重烘次数
    double tintWorstMs = 0.0;     // 彩色层单次最坏耗时（毫秒）
    double tintTotalMs = 0.0;     // 彩色层累计耗时（毫秒）
    int frames = 0;               // 画过多少帧（Pick 被调用的次数）
};
const InnerGlowBakeCounters& InnerGlowBakeStats();

// 临时诊断（曲线塌到左边那次排查）：曲线采样点的实测范围。绘制期间只往内存里写，
// 由 main 在绘制**之后**落盘（绘制路径里做文件 I/O 会让进程崩，本项目已踩过两次）。
// 默认空字符串；只有 DSHB_CURVE_DEBUG 时才有内容。
const std::string& CurveDebugText();

// 单次调用到底重烘了没有（探针用：它按帧重放一次滑行，需要区分"贴缓存"与"重烘"）。
// 实现读的是和 Pick 同一个计数器，所以它不会说谎。
bool GlowTintBakedOnLastPick();

// 探针用的 GlowCache 出口（.dsh/scratch/amb/glowbake.cpp）。
// ★ 为什么必须 export：重烘代价只能在"逐帧重放一次真实滑行"里量出来，而挂件一次
//   导帧只画一帧 —— 用它自己量不出重烘次数。这个类只是把内部的 GlowCache
//   **原样**转出来（成员函数直接转发），所以探针量到的就是生产那条代码。
//   生产路径不碰它（没有任何调用点）。
class GlowCacheForProbe {
public:
    GlowCacheForProbe();
    ~GlowCacheForProbe();
    GlowCacheForProbe(const GlowCacheForProbe&) = delete;
    GlowCacheForProbe& operator=(const GlowCacheForProbe&) = delete;

    void EnsureCoverage(const CanvasSize& canvas);
    // 与生产里的那次调用完全同一个函数（同一个 isExport 语义）。
    void Pick(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& frame,
              bool isExport);
    int tintBakesThisCall() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// 氛围颜色/强度的纯函数出口（滑行探针要按状态取色，不能自己抄一份颜色表）。
AmbienceColor AmbienceTargetColorForProbe(double ratio, double depth);
float GlowIntensityForProbe(double ratio, double depth);

}  // namespace dshb
