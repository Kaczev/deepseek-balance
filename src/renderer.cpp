// deepseek-balance v0.2 —— 渲染器实现
//
// 这一步只画"骨架可见的东西"：圆角面板 + 一个跟着时间走的方块。
// 数字、曲线、颜色、心跳都是后面步骤的事（实施步骤 C/D/E）。

#include "renderer.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <objbase.h>      // CoCreateInstance
#include <wincodec.h>     // 离屏导帧用

#include <cmath>   // std::fmod
#include <cstring> // std::strcmp
#include <string>

namespace dshb {

namespace {

// 布局诊断开关（临时）。定义必须在使用它的 SetLayoutProbe 之前——C++ 里
// 名字要先声明，这一条我在别处已经踩过一次，不再踩。
bool g_layoutProbe = false;

// ★★ 两条用血换来的规矩：
//   1. **绝不在绘制路径里做文件 I/O**。试过两次，两次都崩（0xC0000409），
//      连"导出模式下只画一帧所以安全"这个想法也是错的。
//      正确做法是：绘制期间只往内存里记，画完由外面调用 DumpLayoutProbe 写出去。
//   2. 诊断代码也是代码，它一样会把程序弄崩。所以它要被挡在正常运行之外。
struct LayoutProbeData {
    bool enabled = false;
    bool filled = false;
    float centerX = 0, symbolW = 0, digitsW = 0, left = 0;
    float boxLeft = 0, boxTop = 0, numberTop = 0, boxRight = 0;
};
LayoutProbeData g_probe;

void LayoutProbe(const char* tag, float a, float b, float c, float d) {
    if (!g_probe.enabled) return;
    if (std::strcmp(tag, "number") == 0) {
        g_probe.centerX = a;
        g_probe.symbolW = b;
        g_probe.digitsW = c;
        g_probe.left = d;
    } else if (std::strcmp(tag, "boxes") == 0) {
        g_probe.boxLeft = a;
        g_probe.boxTop = b;
        g_probe.numberTop = c;
        g_probe.boxRight = d;
    }
    g_probe.filled = true;
}

}  // namespace

void SetLayoutProbe(bool on) {
    g_layoutProbe = on;
    g_probe.enabled = on;
}

// 画完之后由外面调用：把绘制期间记下的数值写出去。
// **不在绘制路径里写文件**——那会把进程弄崩。
void DumpLayoutProbe() {
    if (!g_probe.enabled || !g_probe.filled) return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *(slash + 1) = L'\0';
    std::wstring path = exe;
    path += L"layout.log";

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[layout] 实体区中心 cx=%.2f 符号宽=%.2f 数字宽=%.2f 合并块左缘=%.2f\n",
                 g_probe.centerX, g_probe.symbolW, g_probe.digitsW, g_probe.left);
        fwprintf(f, L"[layout] 数字顶=%.2f 标题框=%.2f,%.2f 右边界=%.2f\n", g_probe.numberTop,
                 g_probe.boxLeft, g_probe.boxTop, g_probe.boxRight);
        const float blockCenter = g_probe.left + (g_probe.symbolW + g_probe.digitsW) * 0.5f;
        fwprintf(f, L"[layout] 合并块中心=%.2f 与实体区中心之差=%.2f（目标：接近 0）\n",
                 blockCenter, blockCenter - g_probe.centerX);
        fclose(f);
    }
}

namespace {

constexpr float kPanelOpacity = 0.95f;

// ★ 颜色纪律（A8c，已按实测纠正过一次）：
//   **Direct2D 画刷要的是直通（straight）颜色**——预乘是 D2D 按目标 alpha 模式
//   内部做的。曾经在这里手动预乘，结果预乘了两次：50% 纯红读出来是 64 而不是 128。
//   症状不会报错，只会让半透明处整体偏暗。
//   约定：代码里写设计色（直通 RGBA），交给 D2D；只有**离屏位图回读**和
//   手工写位图时才需要自己预乘。
const D2D1_COLOR_F StraightRgba(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}

// 设计色（直通的 0-1 分量）。设计文档里的十六进制色号一律换算到这里。
struct Rgba {
    float r, g, b, a;
};

// #6c89f6（充足档基准色）
constexpr Rgba kBaseColor{108.0f / 255.0f, 137.0f / 255.0f, 246.0f / 255.0f, kPanelOpacity};
constexpr Rgba kWhite{1.0f, 1.0f, 1.0f, 0.9f};

// ---------------------------------------------------------------------------
// 预乘自检用的画面（A8c）：一个 50% 不透明的纯红方块。
// 它与正常画面走同一条 PaintScene，所以"导出的 PNG"和"屏幕"验的是同一个东西。
// ---------------------------------------------------------------------------
enum class SceneMode { Normal, PremulProbe };
SceneMode g_sceneMode = SceneMode::Normal;

// 当前要画的正文。由 Renderer::SetWidgetFrame 填，绘制函数只读。
WidgetFrame g_widgetFrame{};

// 调试浮层用的 DirectWrite 工厂与文本格式。懒创建：不带调试开关时一行都不建。
IDWriteFactory* DebugWriteFactory() {
    static IDWriteFactory* factory = nullptr;
    if (!factory) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory)))) {
            factory = nullptr;
        }
    }
    return factory;
}

IDWriteTextFormat* DebugTextFormat() {
    static IDWriteTextFormat* fmt = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        IDWriteFactory* dw = DebugWriteFactory();
        if (dw) {
            // 字号是 DIP，所以任何 DPI 下观感一致；行距给正常值，浮层信息不挤
            if (FAILED(dw->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &fmt))) {
                fmt = nullptr;
            } else {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
    }
    return fmt;
}

void PaintPremulProbe(ID2D1RenderTarget* rt) {
    rt->Clear(D2D1::ColorF(0, 0.0f));
    ID2D1SolidColorBrush* red = nullptr;
    // 直通颜色交给 D2D；不要在这里再乘 alpha
    if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 0.0f, 0.0f, 0.5f), &red)) && red) {
        rt->FillRectangle(D2D1::RectF(40.0f, 40.0f, 100.0f, 100.0f), red);
        red->Release();
    }
}

// ---------------------------------------------------------------------------
// 画面内容：一处定义，屏幕与离屏导帧共用同一份代码。
// 这样"导出的 PNG 是对的"才能推出"屏幕上的也是对的"。
// 注意：本函数自己 BeginDraw / EndDraw，调用者不要再套一层（A0 的坑：
// 嵌套会让 EndDraw 返回 D2DERR_WRONG_STATE 并且整帧被丢弃）。
// ---------------------------------------------------------------------------
// 激活反馈：一次 1.2 秒的描边脉冲。用余弦做出的"起-落"曲线，
// 首尾都归零，所以不会突然出现或突然消失（设计 §9.6 的连续性要求）。
constexpr double kFlashSeconds = 1.2;

double FlashPulse(double nowSeconds, double flashStart) {
    const double t = nowSeconds - flashStart;
    if (t < 0.0 || t > kFlashSeconds) return 0.0;
    const double phase = t / kFlashSeconds;              // 0..1
    const double wave = 0.5 - 0.5 * cos(2.0 * 3.14159265 * phase);  // 0→1→0
    return wave;
}

// 文本格式的取用口。字号都是 DIP，所以观感与 DPI 无关。
enum class FontRole { Title, Number, Unit, Estimate, Debug };

IDWriteTextFormat* TextFormatFor(FontRole role) {
    IDWriteFactory* dw = DebugWriteFactory();
    if (!dw) return nullptr;

    struct Slot {
        IDWriteTextFormat* fmt = nullptr;
        bool tried = false;
    };
    static Slot slots[5];

    const int idx = static_cast<int>(role);
    Slot& slot = slots[idx];
    if (!slot.tried) {
        slot.tried = true;
        // 中文正文用雅黑；数字用同一族的等宽数字（tnum）避免滚动时左右抖
        const wchar_t* family = (role == FontRole::Debug) ? L"Consolas" : L"Microsoft YaHei UI";
        float size = 13.0f;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        switch (role) {
        case FontRole::Title: size = 11.0f; break;
        case FontRole::Number: size = 40.0f; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case FontRole::Unit: size = 18.0f; break;
        case FontRole::Estimate: size = 12.0f; break;
        case FontRole::Debug: size = 13.0f; break;
        }
        if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &slot.fmt))) {
            slot.fmt = nullptr;
        } else {
            slot.fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            slot.fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            slot.fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    return slot.fmt;
}

// 量一段文字在给定格式下的宽度（DIP）。
// 居中、布局余量判断都靠它——"差不多居中"靠眼睛是判不出来的。
float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt) {
    if (text.empty() || !fmt) return 0.0f;
    IDWriteFactory* dw = DebugWriteFactory();
    if (!dw) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                    4096.0f, 256.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    float width = 0.0f;
    if (SUCCEEDED(layout->GetMetrics(&m))) width = m.widthIncludingTrailingWhitespace;
    layout->Release();
    return width;
}

// 在一行里画一段文字，横向居中对齐到 centerX（画布坐标）
void DrawCentered(ID2D1RenderTarget* rt, const std::wstring& text, IDWriteTextFormat* fmt,
                  float centerX, float topY, const D2D1_COLOR_F& color, float scale) {
    if (text.empty() || !fmt) return;
    IDWriteFactory* dw = DebugWriteFactory();
    if (!dw) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(color, &brush)) || !brush) return;

    IDWriteTextLayout* layout = nullptr;
    if (SUCCEEDED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                       static_cast<float>(rt->GetSize().width), 256.0f,
                                       &layout)) &&
        layout) {
        const float w = MeasureTextWidth(text, fmt);
        rt->DrawTextLayout(D2D1::Point2F(centerX - w * 0.5f, topY), layout, brush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
        layout->Release();
    }
    brush->Release();
    (void)scale;
}

// 正文（C 阶段）：标题兼状态行、余额数字、币种符号、清零预估。
//
// 排布理由（设计 §9.2）：
//   数字是主角，所以它最大；标题小、放左上；清零预估放底部。
//   币种符号**放前缀**（¥12.34），不是后缀——中文习惯里 12.34¥ 读起来像单位换算。
void PaintWidgetText(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& f) {
    if (g_sceneMode != SceneMode::Normal) return;

    const float s = canvas.scale;
    const float cx = (kMarginDip + kEntityWidthDip * 0.5f) * s;   // 实体区横向中心
    const float top = kMarginDip * s;

    IDWriteTextFormat* titleFmt = TextFormatFor(FontRole::Title);
    IDWriteTextFormat* numFmt = TextFormatFor(FontRole::Number);
    IDWriteTextFormat* unitFmt = TextFormatFor(FontRole::Unit);
    IDWriteTextFormat* estFmt = TextFormatFor(FontRole::Estimate);

    // 标题兼状态行：左上角。状态变了文字就换，不只靠颜色编码。
    if (f.statusText && titleFmt) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 0.85f), &b)) && b) {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                    f.statusText, static_cast<UINT32>(wcslen(f.statusText)), titleFmt,
                    kEntityWidthDip * s, 64.0f, &layout)) &&
                layout) {
                rt->DrawTextLayout(D2D1::Point2F((kMarginDip + 12.0f) * s, (kMarginDip + 8.0f) * s),
                                   layout, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
                layout->Release();
            }
            b->Release();
        }
    }

    // 余额数字：居中。数字与符号一起量宽度，保证"整体"居中而不是"数字"居中。
    {
        const std::wstring digits(f.amountText.begin(), f.amountText.end());
        const std::wstring symbol = f.currencySymbol ? f.currencySymbol : L"";
        const float digitsW = MeasureTextWidth(digits, numFmt);
        const float symbolW = symbol.empty() ? 0.0f : MeasureTextWidth(symbol, unitFmt);
        const float gap = symbol.empty() ? 0.0f : 2.0f * s;
        const float totalW = symbolW + gap + digitsW;
        const float left = cx - totalW * 0.5f;
        const float numberTop = top + 34.0f * s;

        // 诊断：把输入与结果都打出来（只在 --layout-probe 时写文件）
        LayoutProbe("number", cx, symbolW, digitsW, left);
        LayoutProbe("boxes", (kMarginDip + 12.0f) * s, (kMarginDip + 8.0f) * s,
                    numberTop, (kMarginDip + kEntityWidthDip) * s);

        if (!symbol.empty()) {
            ID2D1SolidColorBrush* b = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 0.9f), &b)) && b) {
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                        symbol.c_str(), static_cast<UINT32>(symbol.size()), unitFmt, 256.0f, 64.0f,
                        &layout)) &&
                    layout) {
                    // 符号与数字的基线大致对齐：符号字号小，往下压一点
                    rt->DrawTextLayout(D2D1::Point2F(left, numberTop + 14.0f * s), layout, b,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
                    layout->Release();
                }
                b->Release();
            }
        }

        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 1.0f), &b)) && b) {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                    digits.c_str(), static_cast<UINT32>(digits.size()), numFmt, 2048.0f, 128.0f,
                    &layout)) &&
                layout) {
                rt->DrawTextLayout(D2D1::Point2F(left + symbolW + gap, numberTop), layout, b,
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                layout->Release();
            }
            b->Release();
        }
    }

    // 清零预估：底部居中小字。C9 之前这里是空的——**不编假数据**。
    if (!f.zeroTimeText.empty() && estFmt) {
        const std::wstring t(f.zeroTimeText.begin(), f.zeroTimeText.end());
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 0.75f), &b)) && b) {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                    t.c_str(), static_cast<UINT32>(t.size()), estFmt, kEntityWidthDip * s, 64.0f,
                    &layout)) &&
                layout) {
                const float w = MeasureTextWidth(t, estFmt);
                rt->DrawTextLayout(D2D1::Point2F(cx - w * 0.5f, (kMarginDip + kEntityHeightDip - 26.0f) * s),
                                   layout, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
                layout->Release();
            }
            b->Release();
        }
    }
}

void PaintScene(ID2D1RenderTarget* rt, const CanvasSize& canvas, double elapsedSeconds,
                double flashAmount, const std::wstring& debugText) {
    if (g_sceneMode == SceneMode::PremulProbe) {
        PaintPremulProbe(rt);
        return;
    }    rt->Clear(D2D1::ColorF(0, 0.0f));   // 画布整体透明，外扩余量必须完全透

    const float s = canvas.scale;
    const float cx = kMarginDip * s;
    const float cy = kMarginDip * s;
    const float ew = kEntityWidthDip * s;
    const float eh = kEntityHeightDip * s;
    const float radius = kCornerRadiusDip * s;

    const D2D1_COLOR_F base = StraightRgba(kBaseColor.r, kBaseColor.g, kBaseColor.b, kBaseColor.a);

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(base, &brush)) && brush) {
        const D2D1_ROUNDED_RECT rr =
            D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + ew, cy + eh), radius, radius);
        rt->FillRoundedRectangle(rr, brush);
        brush->Release();
    }

    // 一个跟着时间走的方块：证明帧循环在跑、画面在刷新（后面会被真正的曲线取代）
    ID2D1SolidColorBrush* white = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(
            StraightRgba(kWhite.r, kWhite.g, kWhite.b, kWhite.a), &white)) && white) {
        const float side = 28.0f * s;
        const float travel = ew - side * 2;
        const float x = cx + side + std::fmod(static_cast<float>(elapsedSeconds * 60.0), travel);
        const float y = cy + eh * 0.5f - side * 0.5f;
        rt->FillRectangle(D2D1::RectF(x, y, x + side, y + side), white);
        white->Release();
    }

    // 激活反馈（A11）：一圈由粗到细、再消失的描边。透明度跟 flashAmount 走。
    if (flashAmount > 0.001) {
        ID2D1SolidColorBrush* ring = nullptr;
        const float a = static_cast<float>(flashAmount) * 0.9f;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 1.0f, 1.0f, a), &ring)) && ring) {
            const float inset = 2.0f * s + static_cast<float>(1.0 - flashAmount) * 6.0f * s;
            const D2D1_ROUNDED_RECT rr2 = D2D1::RoundedRect(
                D2D1::RectF(cx + inset, cy + inset, cx + ew - inset, cy + eh - inset),
                radius * 0.65f, radius * 0.65f);
            rt->DrawRoundedRectangle(rr2, ring, 3.0f * s);
            ring->Release();
        }
    }

    // 正文（C 阶段）：标题、数字、符号、清零预估
    PaintWidgetText(rt, canvas, g_widgetFrame);

    // 调试浮层（B6）：只在带调试开关时有内容。画在画布左上角，
    // 覆盖在余量区上——它是眼睛，不是产品界面。
    if (!debugText.empty()) {
        IDWriteFactory* dw = DebugWriteFactory();
        IDWriteTextFormat* fmt = DebugTextFormat();
        if (dw && fmt) {
            ID2D1SolidColorBrush* textBrush = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 0.94f, 0.6f, 0.95f),
                                                    &textBrush)) && textBrush) {
                // ★ 走"先排版、再画"这条正路。
                //   不要想着在 IDWriteFactory 上找 DrawText / DrawTextW：那个成员不存在，
                //   而 dwrite.h 的名字映射又会让错误信息指向带后缀的名字，很容易查错方向。
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(dw->CreateTextLayout(
                        debugText.c_str(), static_cast<UINT32>(debugText.size()), fmt,
                        static_cast<float>(canvas.widthPx), static_cast<float>(canvas.heightPx),
                        &layout)) &&
                    layout) {
                    rt->DrawTextLayout(D2D1::Point2F(8.0f * s, 6.0f * s), layout, textBrush,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
                    layout->Release();
                }
                textBrush->Release();
            }
        }
    }
}

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

void Renderer::SetWidgetFrame(const WidgetFrame& frame) {
    widget_ = frame;
    g_widgetFrame = frame;
}

void Renderer::BeginActivationFlash(double nowSeconds) {
    flashStart_ = nowSeconds;
}

double Renderer::ActivationFlash(double nowSeconds) const {
    return FlashPulse(nowSeconds, flashStart_);
}

bool Renderer::ApplyInputRegion(bool particlesSpillout) {    if (!impl_ || !hwnd_) return false;
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
    PaintScene(d.dc, size_, elapsedSeconds, ActivationFlash(elapsedSeconds), debugText_);
    const HRESULT hrEnd = d.dc->EndDraw();
    if (FAILED(hrEnd)) {
        // A0 的教训：这里失败时 Present 仍会返回 S_OK，画面上却什么都没有，
        // 所以必须在 EndDraw 这一层就能看见失败。
        return hrEnd;
    }
    return d.swapchain->Present(1, 0);
}

bool Renderer::ExportFrame(const wchar_t* path, double elapsedSeconds) {
    if (!impl_ || !path) return false;
    Impl& d = *impl_;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }

    IWICBitmap* bitmap = nullptr;
    bool ok = false;
    ID2D1RenderTarget* rt = nullptr;
    do {
        if (FAILED(wic->CreateBitmap(static_cast<UINT>(size_.widthPx),
                                     static_cast<UINT>(size_.heightPx),
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &bitmap))) {
            break;
        }
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(d.d2dFactory->CreateWicBitmapRenderTarget(bitmap, props, &rt)) || !rt) break;

        // 同一份绘制代码，只是画到离屏位图上：屏幕上的错在这里也会错
        rt->BeginDraw();
        PaintScene(rt, size_, elapsedSeconds, 0.0, debugText_);
        if (FAILED(rt->EndDraw())) break;

        IWICBitmapEncoder* encoder = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* bag = nullptr;
        bool encoded = false;
        do {
            if (FAILED(wic->CreateStream(&stream))) break;
            if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) break;
            if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
            if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
            if (FAILED(encoder->CreateNewFrame(&frame, &bag))) break;
            if (FAILED(frame->Initialize(bag))) break;
            if (FAILED(frame->SetSize(static_cast<UINT>(size_.widthPx),
                                      static_cast<UINT>(size_.heightPx)))) break;
            // ★ 源位图是**预乘** alpha，必须如实声明为 PBGRA，让 WIC 去做预乘→直通的转换。
            //   声明成 32bppBGRA（直通）会把预乘数据当直通读，导出的 PNG 整体偏暗——
            //   而 PNG 是后面所有像素级验收的依据，错了会一路骗下去。
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            if (FAILED(frame->SetPixelFormat(&fmt))) break;
            if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
            if (FAILED(frame->Commit())) break;
            if (FAILED(encoder->Commit())) break;
            encoded = true;
        } while (false);
        if (bag) bag->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        ok = encoded;
    } while (false);

    if (rt) rt->Release();
    if (bitmap) bitmap->Release();
    wic->Release();
    (void)elapsedSeconds;
    return ok;
}

bool Renderer::PremulProbe(uint8_t* outBgra, int* outX, int* outY) {
    if (!impl_ || !outBgra) return false;
    Impl& d = *impl_;

    // 让导出与屏幕都渲染同一个探针画面：否则"从 PNG 里量"量的是别的东西（踩过）
    g_sceneMode = SceneMode::PremulProbe;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }

    IWICBitmap* bitmap = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    bool ok = false;
    do {
        if (FAILED(wic->CreateBitmap(static_cast<UINT>(size_.widthPx),
                                     static_cast<UINT>(size_.heightPx),
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &bitmap))) {
            break;
        }
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(d.d2dFactory->CreateWicBitmapRenderTarget(bitmap, props, &rt)) || !rt) break;

        rt->BeginDraw();
        PaintPremulProbe(rt);
        if (FAILED(rt->EndDraw())) break;

        const int sampleX = 70;
        const int sampleY = 70;
        if (outX) *outX = sampleX;
        if (outY) *outY = sampleY;

        IWICBitmapLock* lock = nullptr;
        WICRect rc{sampleX, sampleY, 1, 1};
        if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, &lock)) || !lock) break;
        UINT cb = 0;
        BYTE* data = nullptr;
        if (SUCCEEDED(lock->GetDataPointer(&cb, &data)) && data && cb >= 4) {
            outBgra[0] = data[0];  // B
            outBgra[1] = data[1];  // G
            outBgra[2] = data[2];  // R
            outBgra[3] = data[3];  // A
            ok = true;
        }
        lock->Release();
    } while (false);

    if (rt) rt->Release();
    if (bitmap) bitmap->Release();
    wic->Release();
    return ok;
}

}  // namespace dshb
