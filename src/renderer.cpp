// deepseek-balance v0.2 鈥斺€?娓叉煋鍣ㄥ疄鐜?
//
// 杩欎竴姝ュ彧鐢?楠ㄦ灦鍙鐨勪笢瑗?锛氬渾瑙掗潰鏉?+ 涓€涓窡鐫€鏃堕棿璧扮殑鏂瑰潡銆?
// 鏁板瓧銆佹洸绾裤€侀鑹层€佸績璺抽兘鏄悗闈㈡楠ょ殑浜嬶紙瀹炴柦姝ラ C/D/E锛夈€?

#include "renderer.h"
#include "heartbeat.h"   // kBeatAMax：窗口区域要留出心跳的行程
#include "curve.h"   // 单调三次插值（氛围曲线）
#include "roll_axis.h"
#include "widget_display.h"   // WidgetFrame / CurvePoint（含每点的颜色）/ AmbienceColor

#include <d2d1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <objbase.h>      // CoCreateInstance
#include <wincodec.h>     // 绂诲睆瀵煎抚鐢?

#include <cmath>   // std::fmod
#include <cstring> // std::strcmp
#include <string>
#include <vector>

namespace dshb {

// ---------------------------------------------------------------------------
// 内蒙光（"E 蒙光.md" §1-§7、"E 施工单.md" 甲.4）—— 先声明，后定义
// ---------------------------------------------------------------------------
//  它是**面板自己背景的最下面一层**：画在面板底色之上、其余一切（边框、激活环、
//  曲线、正文）之前。剖面（alpha 的形状）烘一次，彩色层只在颜色/强度真的变了时重烘。
//
//  ★ 为什么不是"烘覆盖率 + 每帧 FillOpacityMask"（设计阶段的方案）：
//    量出来的事实是 **FillOpacityMask 在软件（WIC）渲染目标上根本不能用** ——
//    不论遮罩是 A8 / R8 / BGRA、不论内容模式是 GRAPHICS 还是 TEXT_NATURAL、
//    不论画笔是 solid 还是 bitmap，EndDraw 一律 D2DERR_WRONG_STATE(0x88990001)，
//    整帧被丢弃；而导帧走的正是软件目标，导帧又是本项目唯一的验证手段。
//    对照组：DrawBitmap 在同一个目标上成功（.dsh\scratch\amb\glowprobe.cpp 可复现）。
//    所以改成"预乘彩色层 + 每帧一次 DrawBitmap"：**两个渲染目标上都成立**，
//    而且屏幕与导帧走同一条代码路径（这才有"导出的 PNG 就是屏幕"这句话）。
//
//  ★ 为什么分成两层缓存：覆盖率是 137,375 个像素各算一次距离场（贵，但永不随状态
//    变）；彩色层只是把覆盖率和当前颜色相乘（便宜，但每帧都可能变一点）。

// ---------------------------------------------------------------------------
// 内蒙光的声明区（定义在文件后面：它要用匿名 namespace 里的烘焙函数）
// ---------------------------------------------------------------------------
// --- 重烘计数与耗时（renderer.h 的 InnerGlowBakeStats 是它的只读出口）---
InnerGlowBakeCounters g_glowBake;
std::string g_curveDebug;   // 临时：DSHB_CURVE_DEBUG 诊断

void PaintInnerGlow(ID2D1RenderTarget* rt, const WidgetFrame& f, ID2D1Bitmap* tinted);
float InnerGlowAlphaAt(float xDip, float yDip);
std::vector<uint8_t> InnerGlowMaskPixels(const CanvasSize& canvas);
ID2D1Bitmap* BakeTintedGlow(ID2D1RenderTarget* rt, const CanvasSize& canvas,
                            const std::vector<uint8_t>& coverage, const D2D1_COLOR_F& colour,
                            float intensity);

// 内蒙光的缓存：**覆盖率遮罩烘一次**（与颜色无关），彩色层只在颜色/强度真的变了时重烘。
//  ★ 必须在匿名 namespace **之外**（成员函数要用匿名 namespace 里的烘焙函数，
//    而那些函数在文件后面才定义，所以这里只写前置声明 + inline 定义在下面）。
struct GlowCache {
    std::vector<uint8_t> coverage;      // 覆盖率（0..255），与颜色无关，烘一次
    ID2D1Bitmap* tinted = nullptr;      // 覆盖率 x 颜色 x k 的预乘彩色层
    bool hasTint = false;
    float lastR = -1.0f;
    float lastG = -1.0f;
    float lastB = -1.0f;
    float lastK = -1.0f;
    bool lastWasExport = false;         // 上一次建位图用的是不是软件目标
    bool everBuilt = false;

    void EnsureCoverage(const CanvasSize& canvas) {
        if (coverage.empty()) coverage = InnerGlowMaskPixels(canvas);
    }

    // 换渲染目标（屏幕 <-> 导帧）时必须重建位图：位图归属创建它的目标。
    // 覆盖率**不用**重算 —— 那是与目标无关的纯数值。
    void Forget(bool isExport) {
        if (!everBuilt || lastWasExport == isExport) return;
        if (tinted) { tinted->Release(); tinted = nullptr; }
        hasTint = false;
        everBuilt = false;
    }

    // 这一帧要贴的那张彩色层；不需要重烘时直接返回上一张。
    ID2D1Bitmap* Pick(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& f,
                      bool isExport);

    void Release() {
        if (tinted) { tinted->Release(); tinted = nullptr; }
        hasTint = false;
        everBuilt = false;
        coverage.clear();
    }
};

// 匿名 namespace 从这里开始：下面的布局探针、场景绘制辅助与内蒙光烘焙函数都是
// 本翻译单元私有的（内蒙光的**声明**区故意留在它外面，见上面）。
namespace {

// 氛围曲线开关：--no-curve 关掉它，用于 A/B 对比（关掉后文字位置必须逐像素不变）

// 甯冨眬璇婃柇寮€鍏筹紙涓存椂锛夈€傚畾涔夊繀椤诲湪浣跨敤瀹冪殑 SetLayoutProbe 涔嬪墠鈥斺€擟++ 閲?
// 鍚嶅瓧瑕佸厛澹版槑锛岃繖涓€鏉℃垜鍦ㄥ埆澶勫凡缁忚俯杩囦竴娆★紝涓嶅啀韪┿€?
bool g_layoutProbe = false;

// 鈽呪槄 涓ゆ潯鐢ㄨ鎹㈡潵鐨勮鐭╋細
//   1. **缁濅笉鍦ㄧ粯鍒惰矾寰勯噷鍋氭枃浠?I/O**銆傝瘯杩囦袱娆★紝涓ゆ閮藉穿锛?xC0000409锛夛紝
//      杩?瀵煎嚭妯″紡涓嬪彧鐢讳竴甯ф墍浠ュ畨鍏?杩欎釜鎯虫硶涔熸槸閿欑殑銆?
//      姝ｇ‘鍋氭硶鏄細缁樺埗鏈熼棿鍙線鍐呭瓨閲岃锛岀敾瀹岀敱澶栭潰璋冪敤 DumpLayoutProbe 鍐欏嚭鍘汇€?
//   2. 璇婃柇浠ｇ爜涔熸槸浠ｇ爜锛屽畠涓€鏍蜂細鎶婄▼搴忓紕宕┿€傛墍浠ュ畠瑕佽鎸″湪姝ｅ父杩愯涔嬪銆?
struct LayoutProbeData {
    bool enabled = false;
    bool filled = false;
    float centerX = 0, symbolW = 0, digitsW = 0, left = 0;
    float boxLeft = 0, boxTop = 0, numberTop = 0, boxRight = 0;
    float lineH = 0;   // 相邻两个数字的垂直间距 h（排版引擎给的 line advance）
};
LayoutProbeData g_probe;

void LayoutProbe(const char* tag, float a, float b, float c, float d) {
    if (!g_probe.enabled) return;
    if (std::strcmp(tag, "number") == 0) {
        g_probe.centerX = a;
        g_probe.symbolW = b;
        g_probe.digitsW = c;
        g_probe.left = d;
    } else if (std::strcmp(tag, "pitch") == 0) {
        g_probe.lineH = a;
    } else if (std::strcmp(tag, "boxes") == 0) {
        g_probe.boxLeft = a;
        g_probe.boxTop = b;
        g_probe.numberTop = c;
        g_probe.boxRight = d;
    }
    g_probe.filled = true;
}

}  // namespace

// 数字绘制模式（见 renderer.h）：定义必须在 dshb 作用域里，不能落进上面的匿名 namespace，
// 否则 main.cpp 链接时找不到 dshb::g_digitDrawMode。

int g_digitDrawMode = 0;

// 氛围曲线开关：--no-curve 关掉它，用于 A/B 对比（关掉后文字位置必须逐像素不变）
bool g_curveEnabled = true;
bool g_symbolHover = false;   // 鼠标悬停在币种符号上
void SetSymbolHover(bool on) { g_symbolHover = on; }

// 氛围曲线：点由显示层算好（规格 §3），渲染层只连线——
// 采样密度 1 像素一个点，所以肉眼看到的是连续曲线，不会出现折角。
// ★ 这里不再做"整条重采样 + 逐点逼近"（规格 §4 明确替换掉的那套）：
//   横向滚动与纵向缓动现在都是显示层里"帧号 k 的纯函数"，渲染层再插一层平滑
//   只会让导帧量到的位置和公式对不上。
namespace { float g_lastLinePitch = 0.0f; }
float LastLinePitchDip() { return g_lastLinePitch; }

void SetCurveEnabled(bool on) { g_curveEnabled = on; }

void SetLayoutProbe(bool on) {
    g_layoutProbe = on;
    g_probe.enabled = on;
}

// 鐢诲畬涔嬪悗鐢卞闈㈣皟鐢細鎶婄粯鍒舵湡闂磋涓嬬殑鏁板€煎啓鍑哄幓銆?
// **涓嶅湪缁樺埗璺緞閲屽啓鏂囦欢**鈥斺€旈偅浼氭妸杩涚▼寮勫穿銆?
void DumpLayoutProbe() {
    if (!g_probe.enabled || !g_probe.filled) return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *(slash + 1) = L'\0';
    std::wstring path = exe;
    path += L"layout.log";

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[layout] 瀹炰綋鍖轰腑蹇?cx=%.2f 绗﹀彿瀹?%.2f 鏁板瓧瀹?%.2f 鍚堝苟鍧楀乏缂?%.2f\n",
                 g_probe.centerX, g_probe.symbolW, g_probe.digitsW, g_probe.left);
        fwprintf(f, L"[layout] 鏁板瓧椤?%.2f 鏍囬妗?%.2f,%.2f 鍙宠竟鐣?%.2f\n", g_probe.numberTop,
                 g_probe.boxLeft, g_probe.boxTop, g_probe.boxRight);
        fwprintf(f, L"[layout] h(=line advance, 相邻数字间距) = %.4f DIP  [scale 1.0 时等于像素]\n",
                 g_probe.lineH);
        const float blockCenter = g_probe.left + (g_probe.symbolW + g_probe.digitsW) * 0.5f;
        fwprintf(f, L"[layout] 鍚堝苟鍧椾腑蹇?%.2f 涓庡疄浣撳尯涓績涔嬪樊=%.2f锛堢洰鏍囷細鎺ヨ繎 0锛塡n",
                 blockCenter, blockCenter - g_probe.centerX);
        fclose(f);
    }
}

namespace {


// 鈽?棰滆壊绾緥锛圓8c锛屽凡鎸夊疄娴嬬籂姝ｈ繃涓€娆★級锛?
//   **Direct2D 鐢诲埛瑕佺殑鏄洿閫氾紙straight锛夐鑹?*鈥斺€旈涔樻槸 D2D 鎸夌洰鏍?alpha 妯″紡
//   鍐呴儴鍋氱殑銆傛浘缁忓湪杩欓噷鎵嬪姩棰勪箻锛岀粨鏋滈涔樹簡涓ゆ锛?0% 绾孩璇诲嚭鏉ユ槸 64 鑰屼笉鏄?128銆?
//   鐥囩姸涓嶄細鎶ラ敊锛屽彧浼氳鍗婇€忔槑澶勬暣浣撳亸鏆椼€?
//   绾﹀畾锛氫唬鐮侀噷鍐欒璁¤壊锛堢洿閫?RGBA锛夛紝浜ょ粰 D2D锛涘彧鏈?*绂诲睆浣嶅浘鍥炶**鍜?
//   鎵嬪伐鍐欎綅鍥炬椂鎵嶉渶瑕佽嚜宸遍涔樸€?
// UTF-8 (std::string) -> UTF-16 (std::wstring).
//
// WHY this is needed rather than the tempting one-liner:
//     std::wstring t(s.begin(), s.end());
// that does NOT decode anything -- it takes each BYTE of the UTF-8 string and makes it
// one wchar_t. For ASCII (the amounts) it happens to look right, which is why the bug
// survived; for Chinese it produces mojibake on screen. Measured: the estimate line
// rendered as "鎸夊綋鍓嶉€熷害" before this fix.
std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

const D2D1_COLOR_F StraightRgba(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}

// 璁捐鑹诧紙鐩撮€氱殑 0-1 鍒嗛噺锛夈€傝璁℃枃妗ｉ噷鐨勫崄鍏繘鍒惰壊鍙蜂竴寰嬫崲绠楀埌杩欓噷銆?
struct Rgba {
    float r, g, b, a;
};

// #6c89f6锛堝厖瓒虫。鍩哄噯鑹诧級

// ---------------------------------------------------------------------------
// 棰勪箻鑷鐢ㄧ殑鐢婚潰锛圓8c锛夛細涓€涓?50% 涓嶉€忔槑鐨勭函绾㈡柟鍧椼€?
// 瀹冧笌姝ｅ父鐢婚潰璧板悓涓€鏉?PaintScene锛屾墍浠?瀵煎嚭鐨?PNG"鍜?灞忓箷"楠岀殑鏄悓涓€涓笢瑗裤€?
// ---------------------------------------------------------------------------
enum class SceneMode { Normal, PremulProbe };
SceneMode g_sceneMode = SceneMode::Normal;

// 褰撳墠瑕佺敾鐨勬鏂囥€傜敱 Renderer::SetWidgetFrame 濉紝缁樺埗鍑芥暟鍙銆?
WidgetFrame g_widgetFrame{};

// 璋冭瘯娴眰鐢ㄧ殑 DirectWrite 宸ュ巶涓庢枃鏈牸寮忋€傛噿鍒涘缓锛氫笉甯﹁皟璇曞紑鍏虫椂涓€琛岄兘涓嶅缓銆?
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
            // 瀛楀彿鏄?DIP锛屾墍浠ヤ换浣?DPI 涓嬭鎰熶竴鑷达紱琛岃窛缁欐甯稿€硷紝娴眰淇℃伅涓嶆尋
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
    // 鐩撮€氶鑹蹭氦缁?D2D锛涗笉瑕佸湪杩欓噷鍐嶄箻 alpha
    if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 0.0f, 0.0f, kProbeRedAlpha), &red)) && red) {
        rt->FillRectangle(D2D1::RectF(40.0f, 40.0f, 100.0f, 100.0f), red);
        red->Release();
    }
}

// ---------------------------------------------------------------------------
// 鐢婚潰鍐呭锛氫竴澶勫畾涔夛紝灞忓箷涓庣灞忓甯у叡鐢ㄥ悓涓€浠戒唬鐮併€?
// 杩欐牱"瀵煎嚭鐨?PNG 鏄鐨?鎵嶈兘鎺ㄥ嚭"灞忓箷涓婄殑涔熸槸瀵圭殑"銆?
// 娉ㄦ剰锛氭湰鍑芥暟鑷繁 BeginDraw / EndDraw锛岃皟鐢ㄨ€呬笉瑕佸啀濂椾竴灞傦紙A0 鐨勫潙锛?
// 宓屽浼氳 EndDraw 杩斿洖 D2DERR_WRONG_STATE 骞朵笖鏁村抚琚涪寮冿級銆?
// ---------------------------------------------------------------------------
// 婵€娲诲弽棣堬細涓€娆?1.2 绉掔殑鎻忚竟鑴夊啿銆傜敤浣欏鸡鍋氬嚭鐨?璧?钀?鏇茬嚎锛?
// 棣栧熬閮藉綊闆讹紝鎵€浠ヤ笉浼氱獊鐒跺嚭鐜版垨绐佺劧娑堝け锛堣璁?搂9.6 鐨勮繛缁€ц姹傦級銆?
constexpr double kFlashSeconds = 1.2;

double FlashPulse(double nowSeconds, double flashStart) {
    const double t = nowSeconds - flashStart;
    if (t < 0.0 || t > kFlashSeconds) return 0.0;
    const double phase = t / kFlashSeconds;              // 0..1
    const double wave = 0.5 - 0.5 * cos(2.0 * 3.14159265 * phase);  // 0鈫?鈫?
    return wave;
}

// 鏂囨湰鏍煎紡鐨勫彇鐢ㄥ彛銆傚瓧鍙烽兘鏄?DIP锛屾墍浠ヨ鎰熶笌 DPI 鏃犲叧銆?
// Current number font size in DIP. The render path sets it from the digit count before
// asking for the flexible number format; 0 means "not set yet".
static float g_numberFontSizeDip = 0.0f;
// 整块数字（数字 + ¥）的横向位置：列数变化时平滑滑动，而不是瞬移半个字宽。
// 这是跨帧的量，所以放在进程内；导出路径每帧一个新进程，导出图永远取到位值。
static float g_numberX = 0.0f;
static bool g_numberXValid = false;

// 币种符号的矩形（像素），每帧刷新；点击命中测试要用
static float g_symbolL = 0.0f, g_symbolT = 0.0f, g_symbolR = 0.0f, g_symbolB = 0.0f;
// 心跳位移在**本帧**实际用掉的像素数。唯一的真相来源：绘制变换与命中矩形都读它，
// 所以两者不可能对不上（本项目反复踩过"一个值两个来源"的坑）。
static float g_lastBeatDyPx = 0.0f;
static bool g_symbolValid = false;
static float g_blockShift = 0.0f;   // 整块数字当帧的横向位移（符号要跟着它走）

// FONT SIZES (DIP) for the balance number, indexed by how many digits it shows
enum class FontRole { Title, Number, NumberFlex, Unit, Estimate, Debug };

IDWriteTextFormat* TextFormatFor(FontRole role) {
    IDWriteFactory* dw = DebugWriteFactory();
    if (!dw) return nullptr;

    struct Slot {
        IDWriteTextFormat* fmt = nullptr;
        bool tried = false;
    };
    static Slot slots[6];

    const int idx = static_cast<int>(role);
    Slot& slot = slots[idx];
    if (!slot.tried) {
        slot.tried = true;
        // 涓枃姝ｆ枃鐢ㄩ泤榛戯紱鏁板瓧鐢ㄥ悓涓€鏃忕殑绛夊鏁板瓧锛坱num锛夐伩鍏嶆粴鍔ㄦ椂宸﹀彸鎶?
        const wchar_t* family = (role == FontRole::Debug) ? L"Consolas" : L"Microsoft YaHei UI";
        float flexSize = 0.0f;
        float size = 13.0f;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        if (role == FontRole::NumberFlex) flexSize = g_numberFontSizeDip;
        switch (role) {
        case FontRole::Title: size = kTitleSizeDip; break;
        case FontRole::Number: size = kNumberFixedSizeDip; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case FontRole::Unit: size = kCurrencySizeDip; break;
        case FontRole::Estimate: size = kEstimateSizeDip; break;
        case FontRole::Debug: size = kDebugSizeDip; break;
        case FontRole::NumberFlex:
            size = (flexSize > 0.0f) ? flexSize : kNumberFixedSizeDip;
            weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
            break;
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

// 閲忎竴娈垫枃瀛楀湪缁欏畾鏍煎紡涓嬬殑瀹藉害锛圖IP锛夈€?
// 灞呬腑銆佸竷灞€浣欓噺鍒ゆ柇閮介潬瀹冣€斺€?宸笉澶氬眳涓?闈犵溂鐫涙槸鍒や笉鍑烘潵鐨勩€?
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

// 閲忎竴涓叉枃瀛楅噷姣忎釜瀛楃鐨勫師鐐逛綅缃紙DIP锛岀浉瀵规帓鐗堟宸︿笂瑙掞級銆?
// 鈽?涓轰粈涔堝繀椤婚噺鑰屼笉鏄畻锛欴irectWrite 浼氭妸瀛楀舰鏀惧湪琛屾閲岀殑鏌愪釜鍩虹嚎浣嶇疆锛?
//   鑰岃繖涓亸绉诲彇鍐充簬瀛椾綋搴﹂噺锛岀寽涓嶅嚭鏉ャ€備箣鍓嶅嚑鐗堝氨鏄潬鐚滃亸绉伙紝浜庢槸鏁板瓧
//   鐢绘銆佽瑁佸埌鍒殑鏁板瓧锛堢敾闈笂鍑虹幇 01.0 杩欑鍊硷級銆傝繖閲岀洿鎺ラ棶鎺掔増寮曟搸銆?
void MeasureCharOrigins(const std::wstring& text, IDWriteTextFormat* fmt,
                        std::vector<float>* xs, float* lineHeight) {
    xs->clear();
    if (text.empty() || !fmt) return;
    IDWriteFactory* dw = DebugWriteFactory();
    if (!dw) return;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 4096.0f,
                                    256.0f, &layout)) ||
        !layout) {
        return;
    }

    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(layout->GetMetrics(&m)) && lineHeight) *lineHeight = m.height;

    // 閫愬瓧绗﹂棶瀹冪殑鐐逛綅缃€傜皣鍙兘鏄瀛楃锛屾墍浠ユ寜绨囨帹杩涖€?
    UINT32 pos = 0;
    while (pos < text.size()) {
        DWRITE_HIT_TEST_METRICS h{};
        FLOAT px = 0.0f, py = 0.0f;
        if (FAILED(layout->HitTestTextPosition(pos, FALSE, &px, &py, &h))) break;
        xs->push_back(px);
        UINT32 advance = (h.length > 0) ? h.length : 1;
        // 琛ラ綈涓棿璺宠繃鐨勫瓧绗︼紙淇濊瘉 xs 涓?text 閫愬瓧绗﹀榻愶級
        for (UINT32 k = 1; k < advance && (pos + k) < text.size(); ++k) {
            xs->push_back(px);
        }
        pos += advance;
    }
    layout->Release();
}

// 閲忓崟涓瓧绗︾殑瀹藉害锛圖IP锛夈€傞€愪綅婊氬姩瑕佹妸姣忎竴浣嶇敾鍦ㄥ悇鑷殑鏍煎瓙閲岋紝
// 鎵€浠ラ渶瑕佹瘡涓瓧绗﹀崟鐙殑浣嶇疆鈥斺€旀暣浣撻噺瀹藉害鐨勬柟娉曞湪杩欓噷涓嶅鐢ㄣ€?
float MeasureCharWidth(wchar_t ch, IDWriteTextFormat* fmt) {
    if (!fmt) return 0.0f;
    const wchar_t s[2] = {ch, 0};
    return MeasureTextWidth(std::wstring(s), fmt);
}

// 骞虫粦缂撳姩锛氶€愪綅婊氬姩鐨勮鎰熷叏鍦ㄨ繖閲屻€傜嚎鎬т細鏄惧緱鏈烘鍙戦椃锛?
// 杩欐潯鏇茬嚎涓ょ鎱€佷腑闂村揩锛屽儚榻胯疆鎷ㄨ繃涓€鏍笺€?
double Smoothstep(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return x * x * (3.0 - 2.0 * x);
}

// 鍦ㄤ竴琛岄噷鐢讳竴娈垫枃瀛楋紝妯悜灞呬腑瀵归綈鍒?centerX锛堢敾甯冨潗鏍囷級
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

// 姝ｆ枃锛圕 闃舵锛夛細鏍囬鍏肩姸鎬佽銆佷綑棰濇暟瀛椼€佸竵绉嶇鍙枫€佹竻闆堕浼般€?
//
// 鎺掑竷鐞嗙敱锛堣璁?搂9.2锛岀鍙蜂綅缃粡鎵€鏈夎€呮寚瀹氾級锛?
//   鏁板瓧鏄富瑙掞紝鎵€浠ュ畠鏈€澶э紱鏍囬灏忋€佹斁宸︿笂锛涙竻闆堕浼版斁搴曢儴銆?
//   甯佺绗﹀彿**鏀惧悗缂€**锛?00.00楼锛夆€斺€旀墍鏈夎€呮槑纭寚瀹氾紝涓嶆敼銆?
//   鏇茬嚎鏄?*姘涘洿**锛屼笌鏁板瓧鍙犲姞鍦ㄥ悓涓€涓尯鍩燂紙D 闃舵锛夛紝涓嶆槸"鍏堝湪鏇茬嚎涓婃柟鍐嶆斁鏁板瓧"銆?
void PaintAmbientCurve(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& f) {
    if (!g_curveEnabled) return;
    const float s = canvas.scale;
    const float x0 = kMarginDip * s;
    const float x1 = (kMarginDip + kEntityWidthDip) * s;
    const float bandTop = (kMarginDip + kCurveBandTopDip) * s;
    const float bandBottom = (kMarginDip + kCurveBandBottomDip) * s;

    // 归一化坐标 -> 画布坐标：x 横跨实体区，y 从带子顶(0)到底(1)。
    // ★ 带子位置只由常量决定，**与币种无关**：换币种不重新布局，
    //   每个币种各自铺满同一条带子（所有者：切币种时曲线位置不变）。
    auto px = [&](float xn) { return x0 + xn * (x1 - x0); };
    auto py = [&](float yn) { return bandTop + yn * (bandBottom - bandTop); };

    // 要画的点列（归一化）。渲染层只管连线，不知道余额从哪来——按规格 §3，点已经
    // 是显示层算好的最终位置（横向滚动、纵向缓动都算完了），这里一个都不再改。
    std::vector<std::pair<float, float>> pts;

    // ★ 每个采样点自己的颜色（"E 蒙光.md" §3.1 末句：节点恰好是那一刻的氛围颜色，
    //   节点之间渐变过去）。渲染层拿到的只有**每个控制点的颜色**，段内怎么分配由
    //   这里决定：按**段**插值，而不是按 u 插值 —— 单调三次在 u 上不是匀速的，
    //   按 u 插值会让颜色在某些段里跑得比曲线本身快。
    //   segOf[i] = 采样点 i 落在哪一段（控制点 k → k+1），tLocal[i] = 该段内的比例。
    std::vector<AmbienceColor> colOf;
    std::vector<float> segStartX;

    if (!f.curveHasData || f.curve.size() < 2) {
        // 没有数据 = 平的（所有者规则），画在带子中线，用**当前 C**。
        pts.push_back({0.0f, 0.5f});
        pts.push_back({1.0f, 0.5f});
        colOf.push_back(f.ambientColor);
        colOf.push_back(f.ambientColor);
        segStartX.push_back(0.0f);
        segStartX.push_back(0.0f);
    } else {
        // 单调三次插值（curve.h）：我们只在采样时刻知道余额，区间内的形状是插出来的；
        // 单调插值保证不过冲（普通样条会画出从未出现过的余额）。
        // ★ 只按 u∈[0,1] 采样 = 横向裁剪到实体区（规格 §3）：越界的段自然画不出来，
        //   左右两端正好落在实体区的两条边上。
        std::vector<double> xs;
        std::vector<double> ys;
        for (const CurvePoint& p : f.curve) {
            xs.push_back(static_cast<double>(p.x));
            ys.push_back(static_cast<double>(p.y));
        }
        dshb::MonotoneCurve mc;
        mc.Build(xs, ys);
        const int steps = 300;
        // 每段的起止 u：按控制点的 x 划分（x 是单调的，显示层保证）。
        std::vector<float> uStart(f.curve.size(), 0.0f);
        for (std::size_t k = 1; k < f.curve.size(); ++k) {
            const float dx = f.curve[k].x - f.curve[k - 1].x;
            uStart[k] = (dx > 0.0f) ? (uStart[k - 1] + dx) : uStart[k - 1];
        }
        const float uSpan = (uStart.back() > 0.0f) ? uStart.back() : 1.0f;
        for (std::size_t k = 0; k < uStart.size(); ++k) uStart[k] /= uSpan;

        std::size_t seg = 0;
        for (int i = 0; i <= steps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(steps);
            pts.push_back({u, static_cast<float>(mc.Eval(static_cast<double>(u)))});
            // 找到这个采样点所在的段（段号只增不减：下面用 while 前进）。
            while (seg + 2 < uStart.size() && u >= uStart[seg + 1]) ++seg;
            const float a = uStart[seg];
            const float b = (seg + 1 < uStart.size()) ? uStart[seg + 1] : 1.0f;
            const float t = (b > a) ? ((u - a) / (b - a)) : 0.0f;
            const CurvePoint& n0 = f.curve[seg];
            const CurvePoint& n1 = f.curve[(seg + 1 < f.curve.size()) ? (seg + 1) : seg];
            // 两个端点里只要有一个没有颜色，就用**当前 C** 画这一段：
            //   老文件里的点没有颜色（写颜色字段之前存的），那是"没测过"，
            //   用当前色是唯一不编造历史的画法（widget_display.h CurvePoint::hasColor）。
            const AmbienceColor c0 = n0.hasColor ? AmbienceColor{n0.cr, n0.cg, n0.cb}
                                                 : f.ambientColor;
            const AmbienceColor c1 = n1.hasColor ? AmbienceColor{n1.cr, n1.cg, n1.cb}
                                                 : f.ambientColor;
            AmbienceColor c;
            c.r = c0.r + (c1.r - c0.r) * t;
            c.g = c0.g + (c1.g - c0.g) * t;
            c.b = c0.b + (c1.b - c0.b) * t;
            colOf.push_back(c);
            segStartX.push_back(px(u));
        }
    }
    if (pts.size() < 2) return;

    // ★ 每段单独画：段的几何与原来**逐点相同**（同一份采样、同一份单调三次求值），
    //   所以曲线的位置一个像素都没动，动的只有落在它上面的颜色。
    //   工厂与几何对象各只取一次，循环里只重开一次 sink（300 段/帧，不建 300 个对象）。
    ID2D1Factory* fac = nullptr;
    rt->GetFactory(&fac);
    if (!fac) return;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(fac->CreatePathGeometry(&geo)) || !geo) {
        fac->Release();
        return;
    }
    ID2D1SolidColorBrush* cb = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(
            StraightRgba(kCurveColorR, kCurveColorG, kCurveColorB, kCurveAlpha), &cb)) ||
        !cb) {
        geo->Release();
        fac->Release();
        return;
    }
    // 诊断（临时，只在 DSHB_CURVE_DEBUG 时写一次文件；**不在绘制路径里做 I/O**——
    // 这里只是把一段文字塞进内存，写文件由 main 在绘制之后调用 DumpCurveDebug 完成）。
    {
        static bool dumped = false;
        if (!dumped && GetEnvironmentVariableA("DSHB_CURVE_DEBUG", nullptr, 0) > 0) {
            dumped = true;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "control=%llu pts=%llu  xRange=[%.1f..%.1f]  yBand=[%.1f..%.1f]  "
                          "first=(%.1f,%.1f) mid=(%.1f,%.1f) last=(%.1f,%.1f)",
                          static_cast<unsigned long long>(f.curve.size()),
                          static_cast<unsigned long long>(pts.size()),
                          static_cast<double>(px(pts.front().first)),
                          static_cast<double>(px(pts.back().first)),
                          static_cast<double>(py(0.0f)), static_cast<double>(py(1.0f)),
                          static_cast<double>(px(pts.front().first)),
                          static_cast<double>(py(pts.front().second)),
                          static_cast<double>(px(pts[pts.size() / 2].first)),
                          static_cast<double>(py(pts[pts.size() / 2].second)),
                          static_cast<double>(px(pts.back().first)),
                          static_cast<double>(py(pts.back().second)));
            g_curveDebug = buf;
            for (std::size_t k = 0; k < f.curve.size() && k < 13; ++k) {
                char line[128];
                std::snprintf(line, sizeof(line), "\n  ctrl[%llu] x=%.4f y=%.4f hasColor=%d",
                              static_cast<unsigned long long>(k),
                              static_cast<double>(f.curve[k].x),
                              static_cast<double>(f.curve[k].y),
                              f.curve[k].hasColor ? 1 : 0);
                g_curveDebug += line;
            }
        }
    }

    // ★ 一次 Open / 一次 Close：**这是唯一能用的写法**。
    //   回归的根因就是这里被写成"每段重开一次同一个几何"：`ID2D1PathGeometry` 不允许多次
    //   Open/Close（第二次 Open 返回 D2DERR_WRONG_STATE 0x88990001），于是循环在第一次
    //   之后就 break，全曲线只有**最左边那一段**被画出来 —— 实测 x[78..82]、20 个像素，
    //   而它本该横跨实体区 x=80..395。
    //   ★ 逐点颜色（"E 蒙光.md" §3.1）与这个限制冲突：一个几何只能被一种画笔画一次。
    //     目前的做法是退回"单色整条"，**逐点颜色重新变成待办**（不能再用重开几何那条路，
    //     见 .dsh\scratch\amb\report 里的说明）。宁可要一条正确的单色曲线，
    //     不要一条只有左端 20 个像素的彩色曲线。
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || !sink) {
        cb->Release();
        geo->Release();
        fac->Release();
        return;
    }
    sink->BeginFigure(D2D1::Point2F(px(pts[0].first), py(pts[0].second)),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        sink->AddLine(D2D1::Point2F(px(pts[i].first), py(pts[i].second)));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    cb->SetColor(StraightRgba(kCurveColorR, kCurveColorG, kCurveColorB, kCurveAlpha));
    rt->DrawGeometry(geo, cb, kCurveWidthDip * s);

    if (!pts.empty()) {
        // 回归排查（曲线塌到左边缘）留下的唯一痕迹：把这条几何的**实测范围**留在
        // 内存里，只写一次。它当初就是这么被找出来的（`geo->GetBounds` 报 inf，
        // 因为几何是空的 —— 每段重开一次 Open/Close 是错的）。
        // 默认空字符串；只有 DSHB_CURVE_DEBUG 时才有内容，由 main 在绘制之后落盘。
        static bool dumped = false;
        if (!dumped && GetEnvironmentVariableA("DSHB_CURVE_DEBUG", nullptr, 0) > 0) {
            dumped = true;
            D2D1_RECT_F bounds = D2D1::RectF(0, 0, 0, 0);
            geo->GetBounds(nullptr, &bounds);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "control=%llu pts=%llu xRange=[%.1f..%.1f] "
                          "geoBounds=[%.1f..%.1f]x[%.1f..%.1f]",
                          static_cast<unsigned long long>(f.curve.size()),
                          static_cast<unsigned long long>(pts.size()),
                          static_cast<double>(px(pts.front().first)),
                          static_cast<double>(px(pts.back().first)),
                          static_cast<double>(bounds.left), static_cast<double>(bounds.right),
                          static_cast<double>(bounds.top), static_cast<double>(bounds.bottom));
            g_curveDebug = buf;
        }
    }

    cb->Release();
    geo->Release();
    fac->Release();
}

void PaintWidgetText(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& f) {
    if (g_sceneMode != SceneMode::Normal) return;
    // ★ 验收口子（命令行 --no-text，测试专用）：正文一层都不画。
    //   为什么需要它：要量"白字对**它自己那层底色**的对比度"，必须知道底色是多少，
    //   而整帧里文字墨迹正好盖在要量的那些像素上（第一次量就量到了字形本身，
    //   得到 1.00:1 —— 那是"白字对白字"，不是对比度）。所以导一张没有文字的同一帧，
    //   从它读出真实底色，再用它当尺子去量有文字那一帧。正常运行时这一行不生效。
    if (!TextEnabled()) return;

    const float s = canvas.scale;
    const float cx = (kMarginDip + kEntityWidthDip * 0.5f) * s;   // 瀹炰綋鍖烘í鍚戜腑蹇?
    const float top = kMarginDip * s;

    IDWriteTextFormat* titleFmt = TextFormatFor(FontRole::Title);
    IDWriteTextFormat* numFmt = TextFormatFor(FontRole::Number);
    IDWriteTextFormat* unitFmt = TextFormatFor(FontRole::Unit);
    IDWriteTextFormat* estFmt = TextFormatFor(FontRole::Estimate);

    // 鏍囬鍏肩姸鎬佽锛氬乏涓婅銆傜姸鎬佸彉浜嗘枃瀛楀氨鎹紝涓嶅彧闈犻鑹茬紪鐮併€?
    if (f.statusText && titleFmt) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kEdgeTextColorR, kEdgeTextColorG, kEdgeTextColorB, kTitleAlpha), &b)) && b) {
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
    // 右上角：刷新倒计时。一个纯数字，每秒变一次，**不做滚动动画**。
    // 字号与标题同一档（15 号），位置是标题的镜像：右对齐、同样的边距。
    if (f.countdownText && f.countdownText[0] != L'\0' && titleFmt) {
        const float cw = MeasureTextWidth(f.countdownText, titleFmt);
        const float cx2 = (kMarginDip + kEntityWidthDip - kTitleInsetXDip) * s - cw;
        const float cy2 = (kMarginDip + kTitleInsetYDip) * s;
        ID2D1SolidColorBrush* cb = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kEdgeTextColorR, kEdgeTextColorG, kEdgeTextColorB, kTitleAlpha), &cb)) && cb) {
            IDWriteTextLayout* cl = nullptr;
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                    f.countdownText, static_cast<UINT32>(wcslen(f.countdownText)), titleFmt,
                    256.0f, 64.0f, &cl)) &&
                cl) {
                rt->DrawTextLayout(D2D1::Point2F(cx2, cy2), cl, cb, D2D1_DRAW_TEXT_OPTIONS_NONE);
                cl->Release();
            }
            cb->Release();
        }
    }

    // 浣欓鏁板瓧锛氬眳涓€傛暟瀛椾笌绗﹀彿涓€璧烽噺瀹藉害锛屼繚璇?鏁翠綋"灞呬腑鑰屼笉鏄?鏁板瓧"灞呬腑銆?
    {
        const std::wstring digits = Widen(f.amountText);
        const std::wstring symbol = f.currencySymbol ? f.currencySymbol : L"";
        const std::wstring& measureText = digits;

        // 璇婃柇锛氭覆鏌撳眰瀹為檯鎷垮埌鐨勬枃鏈笌婊氬姩閲忥紙鍙湪 --layout-probe 鏃惰褰曪級
        {
            char buf[256];
        std::snprintf(buf, sizeof(buf), "text=%s", f.amountText.c_str());
            LayoutProbe(buf, 0, 0, 0, 0);
        }

        // ADAPTIVE SIZE + CENTRING.
        //
        // digits. Measured with the panel at 315 DIP: even 99999.99 has 74 px of clearance on
        // each side, so the old per-digit table was shrinking the layout for no reason.
        g_numberFontSizeDip = kNumberFixedSizeDip;
        IDWriteTextFormat* numFmt2 = TextFormatFor(FontRole::NumberFlex);
        if (!numFmt2) numFmt2 = numFmt;

        const float digitsW = MeasureTextWidth(measureText, numFmt2);
        const float symbolW = symbol.empty() ? 0.0f : MeasureTextWidth(symbol, unitFmt);
        const float gap = symbol.empty() ? 0.0f : kAmountSymbolGapDip * s;
        const float totalW = digitsW + gap + symbolW;
        const float left = cx - totalW * 0.5f;
        const float entityMidY = (kMarginDip + kEntityHeightDip * 0.5f) * s;
        const float numberTop = entityMidY - kNumberTopOffsetDip * s;

        LayoutProbe("number", cx, symbolW, digitsW, left);
        LayoutProbe("boxes", (kMarginDip + kTitleInsetXDip) * s, (kMarginDip + kTitleInsetYDip) * s,
                    numberTop, (kMarginDip + kEntityWidthDip) * s);

        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kTextColorR, kTextColorG, kTextColorB, kAmountAlpha), &b)) && b) {
            // Draw one character at a time, at the origin the string layout reports for
            // it, and centre the block on the measured ink rather than on the layout width.
            // WHY per character: a layout's width includes side bearings, so centring on it
            // leaves the visible digits off-centre (measured: the block sat 6.5 px left of
            // the panel centre, because a leading "1" carries a wide left side bearing).
            std::vector<float> charXs;
            // 相邻两个数字的垂直间距 h：**必须问排版引擎**，不能用字号顶替。
            // 曾经拿字号(40)当行高，结果是数字被裁半截、滚动结束时跳一行——都是这个值错了。
            float lineH = 0.0f;
            g_lastLinePitch = 0.0f;   // 见 LastLinePitchDip
            MeasureCharOrigins(measureText, numFmt2, &charXs, &lineH);
            g_lastLinePitch = lineH;
            LayoutProbe("pitch", lineH, 0, 0, 0);
            const std::wstring& target = measureText;
            size_t firstDigit = 0;
            for (size_t k = 0; k < target.size(); ++k) {
                if (target[k] >= L'0' && target[k] <= L'9') { firstDigit = k; break; }
            }

            // Horizontal centring from measured ink: the first digit's left inset and the
            // last digit's right edge, so the ink box is centred on cx.
            float inkInsetDip = 0.0f;
            IDWriteTextLayout* one = nullptr;
            const wchar_t chBuf[2] = {target[firstDigit], 0};
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(chBuf, 1, numFmt2, 256.0f, 128.0f,
                                                               &one)) &&
                one) {
                DWRITE_OVERHANG_METRICS o0{};
                one->GetOverhangMetrics(&o0);
                inkInsetDip = -o0.left;
                one->Release();
            }
            const float inkW = MeasureTextWidth(measureText, numFmt2) - inkInsetDip * s;
            // 目标位置：按当前列数把整块（数字 + ¥）居中。
            const float targetX = cx - (inkW + gap + symbolW) * 0.5f - inkInsetDip * s;
            // ★ 横向缓动：列数一变，目标位置会跳半个字宽；让实际位置追上去，
            //   于是数字是"滑"过去而不是"瞬移"。风格与滚动一致：每帧把残差乘上 rate。
            g_symbolValid = false;   // 每帧先作废；真的画了符号才置回 true
            // 符号与数字必须用**同一个坐标系**：数字画在 g_numberX + charXs[i]，
            // 所以符号的起点就是"数字墨迹宽 + 间距"，不能再用外层的 left（实测会跑到左边）
            if (!g_numberXValid) { g_numberX = targetX; g_numberXValid = true; }
            else {
                g_numberX = targetX + (g_numberX - targetX) * kNumberShiftRate;
                if (std::fabs(g_numberX - targetX) < kNumberShiftSnapDip) g_numberX = targetX;
            }
            const float inkLeft = g_numberX;
            // ★ 位移必须在**缓动之后**算：放在初始化之前时，第一帧 g_numberX 还是 0，
            //   位移会算成 -targetX，符号被推到数字左边（实测就是这么错的）。
            g_blockShift = g_numberX - targetX;

            // 两条路二选一：整串一次画完（默认）或逐位按坐标画。
            // ★ 曾经写成"逐位接在整串之后"，于是同一个字被画了两遍——墨迹位置一模一样，
            //   但边缘抗锯齿叠加，多出约 600 个像素的差异。必须互斥。
            if (g_digitDrawMode == 0 && f.places.empty()) {
            for (size_t i = 0; i < target.size(); ++i) {
                const float chX = (i < charXs.size()) ? (inkLeft + charXs[i]) : inkLeft;
                IDWriteTextLayout* li = nullptr;
                const wchar_t cb[2] = {target[i], 0};
                if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(cb, 1, numFmt2, 256.0f, 128.0f,
                                                                   &li)) &&
                    li) {
                    rt->DrawTextLayout(D2D1::Point2F(chX, numberTop), li, b,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
                    li->Release();
                }
            }
            }

            // ---- 逐位按坐标画（--digit-draw=axis / user）----
            //
            // 静止位置 = 现在这条静态路径画出来的位置（numberTop）。每位在自己的列上，
            // 按 offset(d) 上下平移，并用一行高的窗口裁剪；窗口高度就是 h（相邻数字间距），
            // 所以静止时一个数字能完整装下（实测墨迹 33 px < h 52 px），滚动中才切到两个。
            if (!f.places.empty() || g_digitDrawMode != 0) {
                // S = 显示数字，直接取要画的这段文本（不再另传管线，也保证
                // "坐标里用的 S" 与 "屏幕上写的字" 一定是同一个数）。
                dshb::Amount shownAmount{};
                dshb::ParseAmount(f.amountText.c_str(), &shownAmount);
                const float h = lineH * s;   // 行距（像素）
                const float inkTopDip = 9.863f * s;   // 单字布局的墨迹顶端内缩（实测）
                const float inkH = 33.0f * s;         // 墨迹高度（实测）
                const float winMid = numberTop + inkTopDip + inkH * 0.5f;
                const float winTop = winMid - h * 0.5f;
                const float winBottom = winMid + h * 0.5f;
                for (size_t i = 0; i < target.size(); ++i) {
                    const float chX = (i < charXs.size()) ? (inkLeft + charXs[i]) : inkLeft;
                    const int place = dshb::axis::PlaceOfSlot(f.amountText, static_cast<int>(i));
                    if (place == dshb::axis::kNoPlace) {
                        // 小数点等非数字字符：原位画出，不参与滚动
                        IDWriteTextLayout* lp = nullptr;
                        const wchar_t cp[2] = {target[i], 0};
                        if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(cp, 1, numFmt2, 256.0f,
                                                                          128.0f, &lp)) &&
                            lp) {
                            rt->DrawTextLayout(D2D1::Point2F(chX, numberTop), lp, b,
                                               D2D1_DRAW_TEXT_OPTIONS_NONE);
                            lp->Release();
                        }
                        continue;
                    }
                    // 坐标来源优先级：1) 帧里带的每位坐标（显示层算好的，静止=整数、滚动中=连续）
                //                  2) 没有时按 --digit-draw 的口径现算（对照用）
                double frameCoord = 0.0;
                bool haveFrameCoord = false;
                for (const dshb::axis::PlaceCoord& pc : f.places) {
                    if (pc.place == place) { frameCoord = pc.coord; haveFrameCoord = true; break; }
                }
                if (haveFrameCoord) {
                // ★ 整数高位（十位及以上）：坐标滚到低于 1 就**不画**，于是这一列随滚动消失；
                //   反向滚动时坐标从 0 涨过 1 才出现。个位（place 0）与小数位恒画。
                if (place >= 1 && frameCoord < 1.0) continue;
                    const int base = static_cast<int>(std::floor(frameCoord));
                    const double frac = frameCoord - static_cast<double>(base);
                    const float yb = numberTop + static_cast<float>(frac * h);   // 所有者：正在走的那位往下
                    const int lo = ((base % 10) + 10) % 10;
                    const int hi = (lo + 1) % 10;
                    const bool cutLo = (yb + inkTopDip < winTop || yb + inkTopDip + inkH > winBottom);
                    if (cutLo) {
                        rt->PushAxisAlignedClip(D2D1::RectF(chX, winTop, chX + h, winBottom),
                                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    }
                    const int draw2[2] = {lo, hi};
                    for (int k2 = 0; k2 < 2; ++k2) {
                        const float y = yb - static_cast<float>(k2) * h;   // hi 从上面补进来
                        if (y + inkTopDip + inkH < winTop || y + inkTopDip > winBottom) continue;
                        IDWriteTextLayout* ld = nullptr;
                        const wchar_t cd[2] = {static_cast<wchar_t>(L'0' + draw2[k2]), 0};
                        if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(cd, 1, numFmt2, 256.0f,
                                                                          128.0f, &ld)) &&
                            ld) {
                            rt->DrawTextLayout(D2D1::Point2F(chX, y), ld, b,
                                               D2D1_DRAW_TEXT_OPTIONS_NONE);
                            ld->Release();
                        }
                    }
                    if (cutLo) rt->PopAxisAlignedClip();
                    continue;
                }
                const dshb::axis::PlaceState st = (g_digitDrawMode == 3)
                                                          ? dshb::axis::StateAtRest(shownAmount, place)
                                                          : dshb::axis::StateAt(shownAmount, place);
                    // 只在"这个数字真的会被切到"时才开裁剪层。
                    // 静止时数字完整落在窗口内，开裁剪只会让 D2D 换一套抗锯齿：墨迹位置
                    // 一模一样，却有几百个像素的细微差别（实测 593 px）。不开裁剪时静止帧与
                    // 原来的整串绘制逐像素相同——这条等价关系正是"坐标系没改坏静止画面"的证据。
                    bool needClip = false;
                    for (int d2 = 0; d2 < 10 && !needClip; ++d2) {
                        const double off2 = (g_digitDrawMode == 2)
                                                ? dshb::axis::OffsetOfUserFormula(st, d2, h)
                                                : dshb::axis::OffsetOf(st, d2, h);
                        const float y2 = numberTop + static_cast<float>(off2);
                        if (y2 + inkTopDip + inkH < winTop || y2 + inkTopDip > winBottom) continue;
                        if (y2 + inkTopDip < winTop || y2 + inkTopDip + inkH > winBottom) needClip = true;
                    }
                    if (needClip) {
                        rt->PushAxisAlignedClip(D2D1::RectF(chX, winTop, chX + h, winBottom),
                                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    }
                    for (int digit = 0; digit < 10; ++digit) {
                        const double off = (g_digitDrawMode == 2)
                            ? dshb::axis::OffsetOfUserFormula(st, digit, h)
                            : dshb::axis::OffsetOf(st, digit, h);
                        const float y = numberTop + static_cast<float>(off);
                        if (y + inkTopDip + inkH < winTop || y + inkTopDip > winBottom) continue;
                        IDWriteTextLayout* ld = nullptr;
                        const wchar_t cd[2] = {static_cast<wchar_t>(L'0' + digit), 0};
                        if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(cd, 1, numFmt2, 256.0f,
                                                                          128.0f, &ld)) &&
                            ld) {
                            rt->DrawTextLayout(D2D1::Point2F(chX, y), ld, b,
                                               D2D1_DRAW_TEXT_OPTIONS_NONE);
                            ld->Release();
                        }
                    }
                    if (needClip) rt->PopAxisAlignedClip();
                }
            }
        }

        // 绗﹀彿鍦ㄥ悗锛堟墍鏈夎€呮寚瀹氾級銆傜鍙峰瓧鍙峰皬锛屽線涓嬪帇涓€鐐硅鍩虹嚎澶ц嚧瀵归綈銆?
        if (!symbol.empty()) {
            ID2D1SolidColorBrush* sb = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kTextColorR, kTextColorG, kTextColorB, (g_symbolHover ? kCurrencyHoverAlpha : kCurrencyAlpha)), &sb)) && sb) {
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                        symbol.c_str(), static_cast<UINT32>(symbol.size()), unitFmt, 256.0f, 64.0f,
                        &layout)) &&
                    layout) {
                    const float symbolX = left + digitsW + gap + g_blockShift;
                    const float symbolY = numberTop + 14.0f * s;
                    g_symbolL = symbolX;
                    g_symbolT = symbolY;
                    g_symbolR = symbolX + (symbolW > 0.0f ? symbolW : 24.0f * s);
                    g_symbolB = symbolY + 40.0f * s;   // 命中框给点余量，不必精确到行距
                    g_symbolValid = true;
                    rt->DrawTextLayout(D2D1::Point2F(symbolX, symbolY),
                                       layout, sb, D2D1_DRAW_TEXT_OPTIONS_NONE);
                    layout->Release();
                }
                sb->Release();
            }
        }
    }

    // 娓呴浂棰勪及锛氬簳閮ㄥ眳涓皬瀛椼€侰9 涔嬪墠杩欓噷鏄┖鐨勨€斺€?*涓嶇紪鍋囨暟鎹?*銆?
    if (!f.zeroTimeText.empty() && estFmt) {
        const std::wstring t = Widen(f.zeroTimeText);
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kEdgeTextColorR, kEdgeTextColorG, kEdgeTextColorB, kEstimateAlpha), &b)) && b) {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                    t.c_str(), static_cast<UINT32>(t.size()), estFmt, kEntityWidthDip * s, 64.0f,
                    &layout)) &&
                layout) {
                // 按排版宽度居中。**不要**改成"按墨迹居中"：这一版我试过，把 overhang 的符号
                // 用反了，文案被推到中线右边 45 px（实测 x 中心 282，而中线是 237.5）。
                // 按排版宽度居中的残差只有左偏 4 px（末尾是半角括号，右侧边距大），可接受。
                const float w = MeasureTextWidth(t, estFmt);
                rt->DrawTextLayout(
                    D2D1::Point2F(cx - w * 0.5f, (kMarginDip + kEntityHeightDip - kEstimateInsetDip) * s),
                    layout, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            b->Release();
        }
    }
}
// 内蒙光与 GlowCache 都已经在文件开头声明（那里定义，因为它要用本 namespace 里的
// 烘焙函数）。这里直接定义 PaintScene。

void PaintScene(ID2D1RenderTarget* rt, const CanvasSize& canvas, double elapsedSeconds,
                double flashAmount, const std::wstring& debugText, GlowCache* glow,
                bool isExport) {
    // 心跳位移：平移**整个面板内容**（底色、蒙光、文字、曲线、边框一起动）。
    //  ★ 窗口真实位置一帧都不改 -> "移动窗口触发系统贴边吸附"结构上不可能发生，
    //    设计 §9.4 的 E9 因此不需要实现，也不需要任何守卫。
    //  ★ 必须**无条件**设置变换：dy==0 时若跳过，会把上一帧的平移留在目标上。
    g_lastBeatDyPx = g_widgetFrame.beatOffsetDip * canvas.scale;
    rt->SetTransform(g_lastBeatDyPx == 0.0f
                         ? D2D1::Matrix3x2F::Identity()
                         : D2D1::Matrix3x2F::Translation(0.0f, g_lastBeatDyPx));

    if (g_sceneMode == SceneMode::PremulProbe) {
        rt->SetTransform(D2D1::Matrix3x2F::Identity());   // 探针路径不参与心跳位移
        PaintPremulProbe(rt);
        return;
    }    rt->Clear(D2D1::ColorF(0, 0.0f));   // 鐢诲竷鏁翠綋閫忔槑锛屽鎵╀綑閲忓繀椤诲畬鍏ㄩ€?

    const float s = canvas.scale;
    const float cx = kMarginDip * s;
    const float cy = kMarginDip * s;
    const float ew = kEntityWidthDip * s;
    const float eh = kEntityHeightDip * s;
    const float radius = kCornerRadiusDip * s;

    const D2D1_COLOR_F base = StraightRgba(kPanelColorR, kPanelColorG, kPanelColorB, kPanelOpacity);

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(base, &brush)) && brush) {
        const D2D1_ROUNDED_RECT rr =
            D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + ew, cy + eh), radius, radius);
        rt->FillRoundedRectangle(rr, brush);
        brush->Release();
    }

    // 内蒙光：必须在**面板底色之后、边框之前** —— 它是背景的一部分，不是罩在边框上的
    // 一层雾。原来画在边框之后，边框会被强烈的光晕染（所有者实测："边缘没有贴合"）。
    PaintInnerGlow(rt, g_widgetFrame, glow->Pick(rt, canvas, g_widgetFrame, isExport));

    // 面板边框：颜色 #afb2b7、线宽 2 DIP（常量在 tuning.h）
    {
        ID2D1SolidColorBrush* bb = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(
                StraightRgba(kBorderColorR, kBorderColorG, kBorderColorB, 1.0f), &bb)) && bb) {
            const D2D1_ROUNDED_RECT rb =
                D2D1::RoundedRect(D2D1::RectF(cx, cy, cx + ew, cy + eh), radius, radius);
            rt->DrawRoundedRectangle(rb, bb, kBorderWidthDip * s);
            bb->Release();
        }
    }

    // 杩欓噷鍘熸潵鏈変竴涓?璺熺潃鏃堕棿璧扮殑鐧借壊鏂瑰潡"锛岀敤閫斿彧鏄瘉鏄庡抚寰幆鍦ㄨ窇銆?
    // 鏁板瓧涓婂睆涔嬪悗瀹冨氨鍙樻垚鍣０浜嗏€斺€旇€屼笖瀹冨帇鍦ㄦ暟瀛椾笂锛岃繕鍜?鏇茬嚎鏄皼鍥淬€?
    // 涓庢暟瀛楀彔鍔?鐨勮璁″啿绐併€傛墍浠ュ垹鎺夛細甯у惊鐜槸鍚﹀湪璺戯紝璋冭瘯娴眰閲岀殑
    // 鍒锋柊鐜囨暟瀛楀凡缁忚兘鍥炵瓟銆傝繖涓綅缃暀缁?D 闃舵鐨勬洸绾裤€?

    // 婵€娲诲弽棣堬紙A11锛夛細涓€鍦堢敱绮楀埌缁嗐€佸啀娑堝け鐨勬弿杈广€傞€忔槑搴﹁窡 flashAmount 璧般€?
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

    // 姝ｆ枃锛圕 闃舵锛夛細鏍囬銆佹暟瀛椼€佺鍙枫€佹竻闆堕浼?
    // 氛围曲线：必须在文字之前画（= 数字后面）
    // ★ 内蒙光（"E 蒙光.md" §1）：面板自己背景的**最下面一层** ——
    //   晚于面板底色（否则整层被底板盖掉）、早于其余一切（边框、激活环、曲线、正文）。
    //   这就是所有者说的"比文字和曲线更早渲染、是面板自己的背景最底下的一部分"。
    // （内蒙光已上移到面板边框**之前**绘制 —— 顺序见下面 FillRoundedRectangle 之后那一处。
    //   原来它在边框之后，导致边框被光罩住：所有者实测发现，2026-09-17。）

    PaintAmbientCurve(rt, canvas, g_widgetFrame);

    PaintWidgetText(rt, canvas, g_widgetFrame);

    // 璋冭瘯娴眰锛圔6锛夛細鍙湪甯﹁皟璇曞紑鍏虫椂鏈夊唴瀹广€傜敾鍦ㄧ敾甯冨乏涓婅锛?
    // 瑕嗙洊鍦ㄤ綑閲忓尯涓娾€斺€斿畠鏄溂鐫涳紝涓嶆槸浜у搧鐣岄潰銆?
    if (!debugText.empty()) {
        IDWriteFactory* dw = DebugWriteFactory();
        IDWriteTextFormat* fmt = DebugTextFormat();
        if (dw && fmt) {
            ID2D1SolidColorBrush* textBrush = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(kWarnR, kWarnG, kWarnB, kWarnAlpha),
                                                    &textBrush)) && textBrush) {
                // 鈽?璧?鍏堟帓鐗堛€佸啀鐢?杩欐潯姝ｈ矾銆?
                //   涓嶈鎯崇潃鍦?IDWriteFactory 涓婃壘 DrawText / DrawTextW锛氶偅涓垚鍛樹笉瀛樺湪锛?
                //   鑰?dwrite.h 鐨勫悕瀛楁槧灏勫張浼氳閿欒淇℃伅鎸囧悜甯﹀悗缂€鐨勫悕瀛楋紝寰堝鏄撴煡閿欐柟鍚戙€?
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

// ---------------------------------------------------------------------------
// 内蒙光（"E 蒙光.md" §1-§7、"E 施工单.md" 甲.4）
// ---------------------------------------------------------------------------
//  它是**面板自己背景的最下面一层**：画在面板底色之上、其余一切（边框、激活环、
//  曲线、正文）之前。剖面（alpha 的形状）烘一次，颜色与强度每帧变。
//
//  ★ 为什么剖面要烘：它是 137,375 个像素各算一次距离场。逐帧做等于把"一次乘加"
//    变成十几万次开方与幂运算。
//  ★ 为什么强度倍率 k 不烘进去：k 随 R/D 每帧都可能动，烘进去就得每帧重烘 ——
//    而 k 正好可以走画笔的不透明度（"E 施工单.md" 甲.4 的"单独暴露的强度倍率"）。
//
// 内蒙光的 alpha 剖面：**只作用在面板轮廓之内**。
//  ★ 这是**唯一的**剖面定义：烘遮罩用它，探针也用同一支函数核对，所以"文档里的数"
//    与"屏幕上的像素"不可能各说一套。它不碰任何 D2D 状态，可以被离线链接。
//
//  距离场约定（写清楚，这里的符号错过一次）：
//    先算 `d = sqrt(max(dx,0)^2 + max(dy,0)^2) + min(max(dx,dy),0) - kCornerRadiusDip`，
//    这个表达式的符号是 **d < 0 在轮廓内、d = 0 在轮廓上、d > 0 在轮廓外**。
//    （圆角矩形里面时 dx、dy 都 <= 0，于是根号项为 0、min 项取较大的那个负数，
//      再减半径 —— 结果是负的；出到外面根号项转正，结果就是正的。）
//    所以 `insideDip = -d` 才是"离轮廓多远（仅面板内有意义）"。
//
//  ⚠ 回归：这一段原来写的是 `const float outside = <上面那个 d>; if (outside > 0) return 0;`
//    —— 把符号读反了。后果是**面板内一律返回 0、面板外整张画布都上色**，
//    而且越往外越亮（d 越负 -> -d 越大 -> 斜坡越大），形状变成了外光晕，
//    底噪也加到了轮廓之外。所有者看到的"边缘没有贴合、出界了"就是这个。
float InnerGlowAlphaAt(float xDip, float yDip) {
    const float cx = kMarginDip + kEntityWidthDip * 0.5f;
    const float cy = kMarginDip + kEntityHeightDip * 0.5f;
    const float hw = kEntityWidthDip * 0.5f - kCornerRadiusDip;
    const float hh = kEntityHeightDip * 0.5f - kCornerRadiusDip;
    float dx = std::fabs(xDip - cx) - hw;
    float dy = std::fabs(yDip - cy) - hh;
    // ★ kGlowCornerRot180（所有者 2026-09-17 指令）：只把**角象限**里的圆角旋转 180 度。
    //   现象的准确描述是"圆角是内凹的"（凹凸方向反了），旋转 180 度即把凹变凸。
    //   四段直边不受影响：那里 dx 或 dy ≤ 0，条件不成立。
    if (kGlowCornerRot180 && dx > 0.0f && dy > 0.0f) {
        // ★ 平滑过渡，而不是在象限边界硬切。原来直接取负，于是在 dx=0 / dy=0 两条线
        //   上出现直角台阶（所有者 2026-09-17 在四个角都看到了，并圈出了那两条边）。
        //   现在用"离象限边界多远"（= min(dx,dy)）在 kCornerRadiusDip 之内做 smoothstep：
        //   边界处权重 0（与直边连续）、深入角内权重 1（保留填平的效果）。
        const float edge = (dx < dy) ? dx : dy;          // >= 0，越大越深入角
        float w = edge / kCornerRadiusDip;
        if (w > 1.0f) w = 1.0f;
        w = w * w * (3.0f - 2.0f * w);                   // smoothstep
        dx = dx * (1.0f - 2.0f * w);
        dy = dy * (1.0f - 2.0f * w);
    }
    const float ax = (dx > 0.0f) ? dx : 0.0f;
    const float ay = (dy > 0.0f) ? dy : 0.0f;
    const float sdf = std::sqrt(ax * ax + ay * ay) +
                      ((dx > dy) ? dy : dx) - kCornerRadiusDip;
    const float insideDip = -sdf;               // > 0 仅当点在轮廓之内
    if (insideDip <= 0.0f) return 0.0f;         // 面板外（含轮廓上）一律 0

    // 三个项**都只在面板内**累加，各自的定义域写在旁边：
    //   整板底噪：整个面板内部处处都有（这是"内蒙光"，不是边缘光）
    //   内唇    ：只在贴边 kGlowInLipDip 那一圈
    //   底部透光：只在离面板底边 kGlowInVertDip 以内
    float a = kGlowInFloorAlpha;
    if (insideDip < kGlowInLipDip) {
        const float t = 1.0f - insideDip / kGlowInLipDip;
        a += kGlowInLipAlpha * t * t;
    }
    const float fromBottom = (kMarginDip + kEntityHeightDip) - yDip;
    if (fromBottom >= 0.0f && fromBottom < kGlowInVertDip) {
        const float t = 1.0f - fromBottom / kGlowInVertDip;
        a += kGlowInVertAlpha * t * t;
    }
    return a;
}

// 烘一次：整张画布大小的 8 位单通道遮罩的**像素**（CPU 侧的真值）。
// ★ 逐像素求距离（每像素 4 个固定网格子采样做抗锯齿），**不做整幅降采样** ——
//   5 DIP 的内唇太薄，降采样会把峰值抹掉两成（设计阶段量过）。
// ★ 屏幕路径与导帧路径**共用这一份像素**：两条路各建一次位图对象，但数值来源
//   只有这一个函数，所以"导出的 PNG"与"屏幕上的画面"不可能对不上。
std::vector<uint8_t> InnerGlowMaskPixels(const CanvasSize& canvas) {
    const UINT w = static_cast<UINT>(canvas.widthPx);
    const UINT h = static_cast<UINT>(canvas.heightPx);
    std::vector<uint8_t> pixels(static_cast<std::size_t>(w) * h, 0);
    if (w == 0 || h == 0) return pixels;
    const float invScale = (canvas.scale > 0.0f) ? (1.0f / canvas.scale) : 1.0f;
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            // 4 个子采样用固定网格（0.25 / 0.75），所以每个都严格落在本像素内 ——
            // 不会借到邻居的像素（那会让整个剖面偏半个像素）。
            float sum = 0.0f;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const float px = (static_cast<float>(x) + 0.25f + 0.5f * sx) * invScale;
                    const float py = (static_cast<float>(y) + 0.25f + 0.5f * sy) * invScale;
                    // ★ kGlowFlip180：把采样点绕画布中心旋转 180 度。画布中心与面板中心
                    //   重合（都是 (237.5,144.5)），所以这等价于"两对对角互换"，正是所有者要的。
                    const float wDip = static_cast<float>(w) * invScale;
                    const float hDip = static_cast<float>(h) * invScale;
                    sum += kGlowFlip180 ? InnerGlowAlphaAt(wDip - px, hDip - py)
                                        : InnerGlowAlphaAt(px, py);
                }
            }
            const float a = sum * 0.25f;
            pixels[static_cast<std::size_t>(y) * w + x] = static_cast<uint8_t>(
                a <= 0.0f ? 0 : (a >= 1.0f ? 255 : static_cast<int>(a * 255.0f + 0.5f)));
        }
    }
    return pixels;
}

// 把上面那份像素变成一张位图。
// ★ 格式必须和**这台渲染目标自己的**格式一致：FillOpacityMask 的遮罩位图与目标
//   格式不一致时 EndDraw 直接报 D2DERR_WRONG_STATE(0x88990001)，整帧被丢掉
//   —— 这一条是**量出来的**，不是推的：glowprobe 在软件 WIC 目标上试过 A8 / R8 /
//   BGRA(只写 alpha) 三种，FillOpacityMask 之后 EndDraw 全部 FAIL。
//   所以这里按目标的实际格式建：BGRA 目标就建 BGRA（覆盖率同时写进 B、G、R、A ——
//   预乘口径下白色 + alpha 的四个通道本来就该相等），单通道目标就直接给单通道。
ID2D1Bitmap* InnerGlowMaskBitmap(ID2D1RenderTarget* rt, const CanvasSize& canvas,
                                 const std::vector<uint8_t>& pixels) {
    if (!rt || pixels.empty()) return nullptr;
    const UINT w = static_cast<UINT>(canvas.widthPx);
    const UINT h = static_cast<UINT>(canvas.heightPx);
    const D2D1_PIXEL_FORMAT target = rt->GetPixelFormat();
    const bool targetIsBgra = (target.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                               target.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    const bool targetIsSingle =
        (target.format == DXGI_FORMAT_A8_UNORM || target.format == DXGI_FORMAT_R8_UNORM);

    if (targetIsBgra) {
        std::vector<uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const uint8_t a = pixels[i];
            bgra[i * 4 + 0] = a;   // B
            bgra[i * 4 + 1] = a;   // G
            bgra[i * 4 + 2] = a;   // R
            bgra[i * 4 + 3] = a;   // A
        }
        const D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(target.format, target.alphaMode));
        ID2D1Bitmap* made = nullptr;
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), static_cast<UINT32>(w) * 4,
                                       &props, &made)) &&
            made) {
            return made;
        }
        return nullptr;
    }
    if (targetIsSingle) {
        const D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(target.format, target.alphaMode));
        ID2D1Bitmap* made = nullptr;
        std::vector<uint8_t> copy(pixels);
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(w, h), copy.data(), static_cast<UINT32>(w),
                                       &props, &made)) &&
            made) {
            return made;
        }
    }
    return nullptr;   // 既不是 BGRA 也不是单通道：这一帧不画蒙光，其余照常
}

// 内蒙光的覆盖率遮罩、彩色层与绘制 —— 定义见文件开头的声明区。
// （GlowCache 的定义必须在匿名 namespace **之外**：它的成员函数要用匿名 namespace
//  里的烘焙函数，而那些函数在文件后面才定义，所以前面只留前置声明。）
inline ID2D1Bitmap* GlowCache::Pick(ID2D1RenderTarget* rt, const CanvasSize& canvas,
                                    const WidgetFrame& f, bool isExport) {
    g_glowBake.frames += 1;   // 记账：Pick 每帧被调一次 = 这一帧画了蒙光
    if (!rt) return nullptr;
    EnsureCoverage(canvas);
    Forget(isExport);
    const float k = (f.ambientIntensity < 0.0f)
                        ? 0.0f
                        : ((f.ambientIntensity > 1.0f) ? 1.0f : f.ambientIntensity);
    if (k <= 0.0f) return nullptr;   // 这一帧不画蒙光
    // 量化到 4/255：颜色稳下来之后就不再重烘。
    // ★ 为什么不是 1/255：量出来的（.dsh\scratch\amb\glowbake.cpp，逐帧重放一次
    //   10 秒滑行）：1/255 会重烘 **280 次**、单次最坏 3.2 ms（≈ 一帧的 19%），
    //   累计 0.43 s；放到 4/255 只重烘 **88 次**、累计 0.14 s，而 4/255 的色阶差
    //   在近黑面板上完全看不出来（每通道 ≤ 4，且它本身还在被缓动）。放到 8/255
    //   是 46 次 / 0.07 s —— 那是给"真机上仍嫌重"留的下一档。
    // ★ 为什么不能像原计划那样"烘一张中性遮罩 + 每帧用画笔乘"：那条路要
    //   ID2D1DeviceContext::DrawBitmap(bitmap, ..., brush)，而导帧走的是
    //   ID2D1RenderTarget（WIC 软件目标），**它根本没有带画笔的那个重载**
    //   （编译期就报 C2661）。用了它，导出的 PNG 里就没有蒙光 —— 而导帧是本项目
    //   唯一的验证手段。见 renderer.h 里 InnerGlowBakeCounters 的说明。
    const float q = 4.0f / 255.0f;
    const bool same = hasTint && lastWasExport == isExport &&
                      std::fabs(f.ambientColor.r - lastR) < q &&
                      std::fabs(f.ambientColor.g - lastG) < q &&
                      std::fabs(f.ambientColor.b - lastB) < q && std::fabs(k - lastK) < q;
    if (same) return tinted;

    LARGE_INTEGER bt0{}, bt1{}, bfreq{};
    QueryPerformanceFrequency(&bfreq);
    QueryPerformanceCounter(&bt0);
    ID2D1Bitmap* made =
        BakeTintedGlow(rt, canvas, coverage,
                       D2D1::ColorF(f.ambientColor.r, f.ambientColor.g, f.ambientColor.b, 1.0f), k);
    QueryPerformanceCounter(&bt1);
    {
        const double ms = static_cast<double>(bt1.QuadPart - bt0.QuadPart) * 1000.0 /
                          static_cast<double>(bfreq.QuadPart);
        g_glowBake.tintBakes += 1;
        g_glowBake.tintTotalMs += ms;
        if (ms > g_glowBake.tintWorstMs) g_glowBake.tintWorstMs = ms;
    }
    if (!made) return tinted;        // 重烘失败：沿用上一张，画面最多颜色旧一帧
    if (tinted) tinted->Release();
    tinted = made;
    hasTint = true;
    everBuilt = true;
    lastWasExport = isExport;
    lastR = f.ambientColor.r;
    lastG = f.ambientColor.g;
    lastB = f.ambientColor.b;
    lastK = k;
    return tinted;
}

// 把"遮罩覆盖率 × 当前颜色 × 当前强度"烘成一张预乘 ARGB 位图。
// ★ 为什么不是"烘覆盖率 + 每帧 FillOpacityMask"（设计阶段的方案）：
//   量出来的事实是 **FillOpacityMask 在软件（WIC）渲染目标上根本不能用**
//   —— 不论遮罩是 A8 / R8 / BGRA、不论内容模式是 GRAPHICS 还是 TEXT_NATURAL、
//   不论画笔是 solid 还是 bitmap，EndDraw 一律 D2DERR_WRONG_STATE(0x88990001)，
//   整帧被丢弃。而导帧走的正是软件目标，导帧又是本项目唯一的验证手段。
//   对照组：DrawBitmap 在同一个目标上成功（见 .dsh\scratch\amb\glowprobe.cpp）。
//   所以改成"预乘彩色层 + 每帧一次 DrawBitmap"：**两个渲染目标上都成立**，
//   而且屏幕与导帧走同一条代码路径（这才有"导出的 PNG 就是屏幕"这句话）。
//
// ★ 重烘的时机：颜色或强度每通道变化 ≥ 1/255 时。颜色是逐通道缓动的连续量，
//   它稳定下来之后就不再重烘；缓动期间最多几十次。137k 像素的乘法在 CPU 上
//   是亚毫秒级，所以这个代价远小于"每帧用失效的 API 丢弃整帧"。
ID2D1Bitmap* BakeTintedGlow(ID2D1RenderTarget* rt, const CanvasSize& canvas,
                            const std::vector<uint8_t>& coverage, const D2D1_COLOR_F& colour,
                            float intensity) {
    if (!rt || coverage.empty()) return nullptr;
    const UINT w = static_cast<UINT>(canvas.widthPx);
    const UINT h = static_cast<UINT>(canvas.heightPx);
    const D2D1_PIXEL_FORMAT target = rt->GetPixelFormat();
    // 只支持 32bpp 目标：蒙光是"一条预乘彩色层"，单通道目标上没有意义。
    if (target.format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        target.format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return nullptr;
    }
    const float k = (intensity < 0.0f) ? 0.0f : ((intensity > 1.0f) ? 1.0f : intensity);
    const float alphaOf = k;
    const float r = colour.r * alphaOf;
    const float g = colour.g * alphaOf;
    const float b = colour.b * alphaOf;

    std::vector<uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        // 预乘：RGB = 颜色 × 覆盖率 × k，A = 覆盖率 × k。
        const float a = static_cast<float>(coverage[i]) / 255.0f;
        const float pa = a * alphaOf;
        bgra[i * 4 + 0] = static_cast<uint8_t>(b * a * 255.0f + 0.5f);
        bgra[i * 4 + 1] = static_cast<uint8_t>(g * a * 255.0f + 0.5f);
        bgra[i * 4 + 2] = static_cast<uint8_t>(r * a * 255.0f + 0.5f);
        bgra[i * 4 + 3] = static_cast<uint8_t>(pa * 255.0f + 0.5f);
    }
    const D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(target.format, target.alphaMode));
    ID2D1Bitmap* made = nullptr;
    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), static_cast<UINT32>(w) * 4,
                                   &props, &made)) &&
        made) {
        return made;
    }
    return nullptr;
}

// 每帧一次：把烘好的彩色层贴上去（一次 DrawBitmap，不需要画笔）。// ★ 不逐帧模糊、不用 effect graph、不逐帧建几何；重烘只在颜色真的变了时发生。
void PaintInnerGlow(ID2D1RenderTarget* rt, const WidgetFrame& f, ID2D1Bitmap* tinted) {
    if (!tinted) return;
    if (!(f.ambientIntensity > 0.0f)) return;   // k = 0：这一帧不画（关掉蒙光的那一帧）
    const D2D1_SIZE_F size = tinted->GetSize();
    const D2D1_RECT_F dest = D2D1::RectF(0.0f, 0.0f, size.width, size.height);
    // ★ 硬边界：蒙光只允许画在**面板矩形之内**。
    //   为什么要有它：遮罩自己声称"轮廓之外一律为 0"，但屏幕上实测到面板下沿之外
    //   3 像素仍有蒙光色（#161b31，约 20%）—— 也就是说画上去的东西与遮罩不一致。
    //   在查清那一步之前，这里先用一个绝对的裁剪把它兜住：无论漏光从哪来，物理上画不出去。
    //   裁剪矩形与面板同源（同一个 kMarginDip / 实体区尺寸），并且跟着当前变换走，
    //   所以心跳位移时它一起移动，不会把光切掉。
    const float sFromBitmap =
        size.width / static_cast<float>(2 * kMarginDip + kEntityWidthDip);
    const D2D1_RECT_F clip = D2D1::RectF(
        kMarginDip * sFromBitmap, kMarginDip * sFromBitmap,
        (kMarginDip + kEntityWidthDip) * sFromBitmap,
        (kMarginDip + kEntityHeightDip) * sFromBitmap);
    // ★ 真正的贴合：用**面板自己的圆角几何**当遮罩，而不是外接矩形。
    //   烘焙出来的遮罩在角上和这条圆角轮廓并不一致（所有者实测：左下角看起来像是用了
    //   对面那个角的弧）。用几何遮罩之后，蒙光能在哪里出现由**几何**决定：
    //   与边框用的是同一个 kCornerRadiusDip，所以两者的圆角必然重合。
    ID2D1Factory* fac = nullptr;
    rt->GetFactory(&fac);
    ID2D1RoundedRectangleGeometry* maskGeo = nullptr;
    if (fac) {
        const float r = kCornerRadiusDip * sFromBitmap;
        fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(clip, r, r), &maskGeo);
    }
    if (maskGeo) {
        const D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
            D2D1::InfiniteRect(), maskGeo, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->PushLayer(lp, nullptr);
    }
    rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->DrawBitmap(tinted, &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    rt->PopAxisAlignedClip();
    if (maskGeo) { rt->PopLayer(); maskGeo->Release(); }
}

// 临时诊断：曲线采样点的实测范围（只在 DSHB_CURVE_DEBUG 时非空）。
const std::string& CurveDebugText() { return g_curveDebug; }

// 探针用：把烘焙出来的遮罩字节给出去（生产路径不调用）。
const std::vector<unsigned char>& InnerGlowMaskPixelsForProbe(const CanvasSize& canvas) {
    static std::vector<unsigned char> cache;
    if (cache.empty()) cache = InnerGlowMaskPixels(canvas);
    return cache;
}

const InnerGlowBakeCounters& InnerGlowBakeStats() { return g_glowBake; }

// ---------------------------------------------------------------------------
// 探针出口（.dsh/scratch/amb/glowbake.cpp）：只是把内部实现原样转出来
// ---------------------------------------------------------------------------
//  生产中没有任何调用点。它存在的唯一理由是：重烘代价只能靠"逐帧重放一次真实滑行"
//  量出来，而挂件一次导帧只画一帧。
int g_glowTintBakedOnLastPick = 0;
bool GlowTintBakedOnLastPick() { return g_glowTintBakedOnLastPick != 0; }

struct GlowCacheForProbe::Impl {
    GlowCache cache;
};

GlowCacheForProbe::GlowCacheForProbe() : impl_(new Impl()) {}
GlowCacheForProbe::~GlowCacheForProbe() {
    if (impl_) {
        impl_->cache.Release();
        delete impl_;
        impl_ = nullptr;
    }
}

void GlowCacheForProbe::EnsureCoverage(const CanvasSize& canvas) {
    if (impl_) impl_->cache.EnsureCoverage(canvas);
}

void GlowCacheForProbe::Pick(ID2D1RenderTarget* rt, const CanvasSize& canvas,
                             const WidgetFrame& frame, bool isExport) {
    if (!impl_) return;
    const int before = g_glowBake.tintBakes;
    impl_->cache.Pick(rt, canvas, frame, isExport);
    g_glowTintBakedOnLastPick = (g_glowBake.tintBakes != before) ? 1 : 0;
}

int GlowCacheForProbe::tintBakesThisCall() const { return g_glowTintBakedOnLastPick; }

AmbienceColor AmbienceTargetColorForProbe(double ratio, double depth) {
    return AmbienceTargetColor(ratio, depth);
}

float GlowIntensityForProbe(double ratio, double depth) {
    return static_cast<float>((kGlowInK0 + kGlowInK1 * ratio) * (1.0 - kGlowInD * depth));
}
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
    // ★ 内蒙光的缓存：覆盖率烘一次（与颜色无关），彩色层只在颜色/强度真的变了时重烘。
    //   换渲染目标（屏幕 <-> 导帧）时只重建位图，覆盖率数值不用重算。
    GlowCache glow;
    void ReleaseAll() {
        glow.Release();
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
    // 鈽?缂╂斁鍥哄畾涓?1.0锛?*涓€涓璁″儚绱?= 涓€涓睆骞曞儚绱?*锛堟墍鏈夎€呭畾涓嬬殑鍗曚綅锛夈€?
    //   杩欓噷鏃╁厛鏄?dpi/96锛屼簬鏄?200% 缂╂斁鐨勫睆涓婂緱鍒?630脳258锛屾墍鏈夎€呭垽瀹氬亸澶с€?
    //   璁捐绋跨殑 315脳129 鎸囩殑鏄睆骞曞儚绱狅紝鎵€浠ヤ笉涔?DPI銆?
    //
    //   涓轰粈涔堜笉鐢?杩涚▼涓嶅０鏄?DPI 鎰熺煡"鏉ヨ揪鍒板悓鏍锋晥鏋滐細閭ｆ牱绐楀彛浼氳绯荤粺浣嶅浘鎷変几锛?
    //   鏂囧瓧浼氱硦銆備繚鎸?Per-Monitor V2 + 缂╂斁 1.0锛屾枃瀛椾粛鏄煝閲忔竻鏅扮殑銆?
    (void)hwnd;
    CanvasSize s;
    s.scale = 1.0f;
    s.widthPx = kCanvasWidthDip;
    s.heightPx = kCanvasHeightDip;
    return s;
}

bool Renderer::Create(HWND hwnd, const CanvasSize& size) {
    Destroy();
    hwnd_ = hwnd;
    size_ = size;
    impl_ = new Impl();
    Impl& d = *impl_;

    // ---- D3D11 璁惧 ----
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &d.device, &got, nullptr))) {
        return false;
    }
    if (FAILED(d.device->QueryInterface(IID_PPV_ARGS(&d.dxgiDevice)))) return false;

    // ---- 缈昏浆妯″瀷浜ゆ崲閾?+ 棰勪箻 alpha锛堟瘡鍍忕礌閫忔槑鐨勫叧閿級----
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

    // ---- D2D 璁惧涓婁笅鏂囩粦鍒板悗澶囩紦鍐?----
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

    // ---- 内蒙光的覆盖率遮罩：启动烘一次（"E 施工单.md" 甲.4）----
    // 烘失败不是致命错误：没有遮罩 = 这一层不画（面板保持原样），其余一切照常。
    // 宁可少一层氛围，也不要因为装饰让整块面板起不来。
    // ★ 只烘**覆盖率**（与颜色无关）：彩色层在每帧的 Pick() 里按需重烘。
    //   覆盖率是 137,375 个像素各算一次距离场，只在**启动时**做一次；把它计时记下来
    //   （"廉价"这句话要有凭据，不能靠感觉）。
    {
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        d.glow.EnsureCoverage(size_);
        QueryPerformanceCounter(&t1);
        g_glowBake.coverageBakes = 1;
        g_glowBake.coverageWorstMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                                     static_cast<double>(freq.QuadPart);
    }

    // ---- DirectComposition锛氭妸浜ゆ崲閾炬寕鍒扮獥鍙ｄ笂 ----
    if (FAILED(DCompositionCreateDevice(d.dxgiDevice, __uuidof(IDCompositionDevice),
                                        reinterpret_cast<void**>(&d.compDevice)))) {
        return false;
    }
    if (FAILED(d.compDevice->CreateTargetForHwnd(hwnd_, TRUE, &d.compTarget))) return false;
    if (FAILED(d.compDevice->CreateVisual(&d.compVisual))) return false;
    if (FAILED(d.compVisual->SetContent(d.swapchain))) return false;
    if (FAILED(d.compTarget->SetRoot(d.compVisual))) return false;
    if (FAILED(d.compDevice->Commit())) return false;

    // 鍖哄煙蹇呴』璺熺潃鐢诲竷璧帮細DPI 鍙樻洿鎴栭噸寤轰箣鍚庨兘瑕侀噸璁撅紝鍚﹀垯鍙偣鍖哄煙浼氬拰鐢婚潰瀵逛笉涓?
    ApplyInputRegion(false);

    ready_ = true;
    return true;
}

bool Renderer::Resize(HWND hwnd) {
    if (!impl_) return false;
    const CanvasSize want = SizeForWindow(hwnd);
    if (want.widthPx == size_.widthPx && want.heightPx == size_.heightPx) return true;
    // 浜ゆ崲閾惧昂瀵稿彉浜嗗氨寰楁暣鍧楅噸寤猴細缈昏浆妯″瀷涓嶅厑璁稿鍚庡缂撳啿鐩存帴 ResizeBuffers 鍚庡啀缁?D2D 鐩爣锛?
    // 閲嶅缓姣斿氨鍦版敼鏇寸渷浜嬶紝涔熸洿涓嶅鏄撶暀涓嬫偓绌虹殑鐩爣浣嶅浘銆?
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
        // 宸茬粡鎵╁埌鍏ㄧ敾甯冿紝涓嶇敤閲嶅璁剧疆
    }

    // 鍖哄煙鍧愭爣鏄獥鍙ｅ潗鏍囷紙鏃犺竟妗嗙獥鍙ｇ殑绐楀彛鐭╁舰 == 瀹㈡埛鍖猴級
    int left = 0, top = 0, right = size_.widthPx, bottom = size_.heightPx;
    if (!particlesSpillout) {
        const int m = static_cast<int>(kMarginDip * size_.scale + 0.5f);
        left = m;
        top = m;
        right = m + static_cast<int>(kEntityWidthDip * size_.scale + 0.5f);
        bottom = m + static_cast<int>(kEntityHeightDip * size_.scale + 0.5f);
        // ★ 心跳位移：面板内容最多上下移动 kBeatAMax DIP，而 SetWindowRgn 会**同时裁剪绘制**
        //   （不只是命中测试），所以区域必须留出这段行程 —— 否则位移到最低点时面板下边缘
        //   被系统切掉（所有者在屏幕上实测到的那条）。代价是边缘多算 kBeatAMax 像素可点，
        //   相对于 80 DIP 的透明余量可以忽略。
        const int slack = static_cast<int>(kBeatAMax * size_.scale + 0.5f);
        // ★ 只向下留余量：心跳位移是单边的（dip >= 0，只往下），上沿不需要余量。
        //   上沿留余量会把画布上本来就存在、以前被裁掉的内容露出来（我引进过一次）。
        bottom += slack;
    }
    const int radius = particlesSpillout
        ? 0
        : static_cast<int>(kCornerRadiusDip * size_.scale + 0.5f);

    HRGN region = particlesSpillout
        ? CreateRectRgn(left, top, right, bottom)
        : CreateRoundRectRgn(left, top, right + 1, bottom + 1, radius * 2, radius * 2);
    if (!region) return false;

    // SetWindowRgn 鎴愬姛鍚庡尯鍩熷綊绯荤粺鎵€鏈夛紝涓嶈兘鍐?DeleteObject
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

    // 娉ㄦ剰锛欵ndDraw 鏄敮涓€浼氭姤閿欑殑涓€姝ワ紱BeginDraw 杩斿洖 void銆?
    // 杩欓噷鑷繁 BeginDraw/EndDraw锛岀敾鍐呭鐨勫嚱鏁颁笉瑕佸啀鍚勮皟涓€娆★紙A0 鐨勫潙锛夈€?
    d.dc->BeginDraw();
    PaintScene(d.dc, size_, elapsedSeconds, ActivationFlash(elapsedSeconds), debugText_,
               &d.glow, /*isExport=*/false);
    const HRESULT hrEnd = d.dc->EndDraw();
    if (FAILED(hrEnd)) {
        // A0 鐨勬暀璁細杩欓噷澶辫触鏃?Present 浠嶄細杩斿洖 S_OK锛岀敾闈笂鍗翠粈涔堥兘娌℃湁锛?
        // 鎵€浠ュ繀椤诲湪 EndDraw 杩欎竴灞傚氨鑳界湅瑙佸け璐ャ€?
        return hrEnd;
    }
    return d.swapchain->Present(1, 0);
}

SymbolRect CurrencySymbolRect() {
    // ★ 命中框必须跟着心跳位移走，否则点击差 g_lastBeatDyPx 像素（币种符号命中、拖动判定都靠它）。
    //   数值直接取本帧绘制时用的那一个，不重算、不猜。
    SymbolRect r{};
    r.l = g_symbolL; r.t = g_symbolT + g_lastBeatDyPx;
    r.r = g_symbolR; r.b = g_symbolB + g_lastBeatDyPx;
    r.valid = g_symbolValid;
    return r;
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

        // 鍚屼竴浠界粯鍒朵唬鐮侊紝鍙槸鐢诲埌绂诲睆浣嶅浘涓婏細灞忓箷涓婄殑閿欏湪杩欓噷涔熶細閿?
        // ★ 内蒙光的遮罩要用**这个渲染目标自己的**位图：glowPixels 是同一份数值，
        //   但 GPU 位图不能被软件（WIC）渲染目标采样。少了这一步，导出的 PNG 里
        //   就没有蒙光 —— 而验收正是拿导出的 PNG 量的。
        rt->BeginDraw();
        // ★ 同一个 GlowCache，但 isExport = true：彩色层必须用**这个**软件渲染目标
        //   自己的位图（位图归属创建它的目标），覆盖率数值则复用同一份。
        PaintScene(rt, size_, elapsedSeconds, 0.0, debugText_, &d.glow, /*isExport=*/true);
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
            // 鈽?婧愪綅鍥炬槸**棰勪箻** alpha锛屽繀椤诲瀹炲０鏄庝负 PBGRA锛岃 WIC 鍘诲仛棰勪箻鈫掔洿閫氱殑杞崲銆?
            //   澹版槑鎴?32bppBGRA锛堢洿閫氾級浼氭妸棰勪箻鏁版嵁褰撶洿閫氳锛屽鍑虹殑 PNG 鏁翠綋鍋忔殫鈥斺€?
            //   鑰?PNG 鏄悗闈㈡墍鏈夊儚绱犵骇楠屾敹鐨勪緷鎹紝閿欎簡浼氫竴璺獥涓嬪幓銆?
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

    // 璁╁鍑轰笌灞忓箷閮芥覆鏌撳悓涓€涓帰閽堢敾闈細鍚﹀垯"浠?PNG 閲岄噺"閲忕殑鏄埆鐨勪笢瑗匡紙韪╄繃锛?
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
