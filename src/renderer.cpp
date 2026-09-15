// deepseek-balance v0.2 鈥斺€?娓叉煋鍣ㄥ疄鐜?
//
// 杩欎竴姝ュ彧鐢?楠ㄦ灦鍙鐨勪笢瑗?锛氬渾瑙掗潰鏉?+ 涓€涓窡鐫€鏃堕棿璧扮殑鏂瑰潡銆?
// 鏁板瓧銆佹洸绾裤€侀鑹层€佸績璺抽兘鏄悗闈㈡楠ょ殑浜嬶紙瀹炴柦姝ラ C/D/E锛夈€?

#include "renderer.h"

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

namespace dshb {

namespace {

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
        const float blockCenter = g_probe.left + (g_probe.symbolW + g_probe.digitsW) * 0.5f;
        fwprintf(f, L"[layout] 鍚堝苟鍧椾腑蹇?%.2f 涓庡疄浣撳尯涓績涔嬪樊=%.2f锛堢洰鏍囷細鎺ヨ繎 0锛塡n",
                 blockCenter, blockCenter - g_probe.centerX);
        fclose(f);
    }
}

namespace {

constexpr float kPanelOpacity = 0.95f;

// 鈽?棰滆壊绾緥锛圓8c锛屽凡鎸夊疄娴嬬籂姝ｈ繃涓€娆★級锛?
//   **Direct2D 鐢诲埛瑕佺殑鏄洿閫氾紙straight锛夐鑹?*鈥斺€旈涔樻槸 D2D 鎸夌洰鏍?alpha 妯″紡
//   鍐呴儴鍋氱殑銆傛浘缁忓湪杩欓噷鎵嬪姩棰勪箻锛岀粨鏋滈涔樹簡涓ゆ锛?0% 绾孩璇诲嚭鏉ユ槸 64 鑰屼笉鏄?128銆?
//   鐥囩姸涓嶄細鎶ラ敊锛屽彧浼氳鍗婇€忔槑澶勬暣浣撳亸鏆椼€?
//   绾﹀畾锛氫唬鐮侀噷鍐欒璁¤壊锛堢洿閫?RGBA锛夛紝浜ょ粰 D2D锛涘彧鏈?*绂诲睆浣嶅浘鍥炶**鍜?
//   鎵嬪伐鍐欎綅鍥炬椂鎵嶉渶瑕佽嚜宸遍涔樸€?
const D2D1_COLOR_F StraightRgba(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}

// 璁捐鑹诧紙鐩撮€氱殑 0-1 鍒嗛噺锛夈€傝璁℃枃妗ｉ噷鐨勫崄鍏繘鍒惰壊鍙蜂竴寰嬫崲绠楀埌杩欓噷銆?
struct Rgba {
    float r, g, b, a;
};

// #6c89f6锛堝厖瓒虫。鍩哄噯鑹诧級
constexpr Rgba kBaseColor{108.0f / 255.0f, 137.0f / 255.0f, 246.0f / 255.0f, kPanelOpacity};
constexpr Rgba kWhite{1.0f, 1.0f, 1.0f, 0.9f};

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
    if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 0.0f, 0.0f, 0.5f), &red)) && red) {
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

// FONT SIZES (DIP) for the balance number, indexed by how many digits it shows
// (the currency sign is not counted). The number is deliberately NOT one fixed size:
// as the balance gains digits the panel would otherwise overflow, so the size steps
// down and the block stays centred -- the layout expands and contracts with the number.
// Values are appearance parameters: change them to taste, they are the only place the
// sizes live.
const float kNumberSizeByDigits[] = {40.0f, 40.0f, 40.0f, 40.0f, 38.0f,
                                     34.0f, 30.0f, 27.0f, 24.0f};
const int kNumberSizeCount =
    static_cast<int>(sizeof(kNumberSizeByDigits) / sizeof(kNumberSizeByDigits[0]));

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
        case FontRole::Title: size = 12.5f; break;   // owner: a little bigger than the 11 it was
        case FontRole::Number: size = 40.0f; weight = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
        case FontRole::Unit: size = 18.0f; break;
        case FontRole::Estimate: size = 12.0f; break;
        case FontRole::Debug: size = 13.0f; break;
        case FontRole::NumberFlex:
            size = (flexSize > 0.0f) ? flexSize : kNumberSizeByDigits[0];
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
void PaintWidgetText(ID2D1RenderTarget* rt, const CanvasSize& canvas, const WidgetFrame& f) {
    if (g_sceneMode != SceneMode::Normal) return;

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

    // 浣欓鏁板瓧锛氬眳涓€傛暟瀛椾笌绗﹀彿涓€璧烽噺瀹藉害锛屼繚璇?鏁翠綋"灞呬腑鑰屼笉鏄?鏁板瓧"灞呬腑銆?
    {
        const std::wstring digits(f.amountText.begin(), f.amountText.end());
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
        // The number is not drawn at one fixed size: as the balance gains digits it would
        // otherwise run into the panel edges. The size steps down with the digit count
        // (kNumberSizeByDigits) and the block stays centred horizontally, so the layout
        // expands and contracts with the number instead of overflowing.
        int digitCount = 0;
        for (wchar_t ch : measureText) {
            if (ch >= L'0' && ch <= L'9') ++digitCount;
        }
        const int sizeIdx = (digitCount < kNumberSizeCount) ? digitCount : (kNumberSizeCount - 1);
        g_numberFontSizeDip = kNumberSizeByDigits[sizeIdx];
        IDWriteTextFormat* numFmt2 = TextFormatFor(FontRole::NumberFlex);
        if (!numFmt2) numFmt2 = numFmt;

        const float digitsW = MeasureTextWidth(measureText, numFmt2);
        const float symbolW = symbol.empty() ? 0.0f : MeasureTextWidth(symbol, unitFmt);
        const float gap = symbol.empty() ? 0.0f : 2.0f * s;
        const float totalW = digitsW + gap + symbolW;
        const float left = cx - totalW * 0.5f;
        const float entityMidY = (kMarginDip + kEntityHeightDip * 0.5f) * s;
        const float numberTop = entityMidY - 26.5f * s;

        LayoutProbe("number", cx, symbolW, digitsW, left);
        LayoutProbe("boxes", (kMarginDip + 12.0f) * s, (kMarginDip + 8.0f) * s,
                    numberTop, (kMarginDip + kEntityWidthDip) * s);

        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 1.0f), &b)) && b) {
            // Draw one character at a time, at the origin the string layout reports for
            // it, and centre the block on the measured ink rather than on the layout width.
            // WHY per character: a layout's width includes side bearings, so centring on it
            // leaves the visible digits off-centre (measured: the block sat 6.5 px left of
            // the panel centre, because a leading "1" carries a wide left side bearing).
            std::vector<float> charXs;
            float lineH = 0.0f;
            MeasureCharOrigins(measureText, numFmt2, &charXs, &lineH);

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
            const float inkLeft = cx - (inkW + gap + symbolW) * 0.5f - inkInsetDip * s;

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

        // 绗﹀彿鍦ㄥ悗锛堟墍鏈夎€呮寚瀹氾級銆傜鍙峰瓧鍙峰皬锛屽線涓嬪帇涓€鐐硅鍩虹嚎澶ц嚧瀵归綈銆?
        if (!symbol.empty()) {
            ID2D1SolidColorBrush* sb = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1, 1, 1, 0.9f), &sb)) && sb) {
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(DebugWriteFactory()->CreateTextLayout(
                        symbol.c_str(), static_cast<UINT32>(symbol.size()), unitFmt, 256.0f, 64.0f,
                        &layout)) &&
                    layout) {
                    rt->DrawTextLayout(D2D1::Point2F(left + digitsW + gap, numberTop + 14.0f * s),
                                       layout, sb, D2D1_DRAW_TEXT_OPTIONS_NONE);
                    layout->Release();
                }
                sb->Release();
            }
        }
    }

    // 娓呴浂棰勪及锛氬簳閮ㄥ眳涓皬瀛椼€侰9 涔嬪墠杩欓噷鏄┖鐨勨€斺€?*涓嶇紪鍋囨暟鎹?*銆?
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
    }    rt->Clear(D2D1::ColorF(0, 0.0f));   // 鐢诲竷鏁翠綋閫忔槑锛屽鎵╀綑閲忓繀椤诲畬鍏ㄩ€?

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
    PaintWidgetText(rt, canvas, g_widgetFrame);

    // 璋冭瘯娴眰锛圔6锛夛細鍙湪甯﹁皟璇曞紑鍏虫椂鏈夊唴瀹广€傜敾鍦ㄧ敾甯冨乏涓婅锛?
    // 瑕嗙洊鍦ㄤ綑閲忓尯涓娾€斺€斿畠鏄溂鐫涳紝涓嶆槸浜у搧鐣岄潰銆?
    if (!debugText.empty()) {
        IDWriteFactory* dw = DebugWriteFactory();
        IDWriteTextFormat* fmt = DebugTextFormat();
        if (dw && fmt) {
            ID2D1SolidColorBrush* textBrush = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(StraightRgba(1.0f, 0.94f, 0.6f, 0.95f),
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
    PaintScene(d.dc, size_, elapsedSeconds, ActivationFlash(elapsedSeconds), debugText_);
    const HRESULT hrEnd = d.dc->EndDraw();
    if (FAILED(hrEnd)) {
        // A0 鐨勬暀璁細杩欓噷澶辫触鏃?Present 浠嶄細杩斿洖 S_OK锛岀敾闈笂鍗翠粈涔堥兘娌℃湁锛?
        // 鎵€浠ュ繀椤诲湪 EndDraw 杩欎竴灞傚氨鑳界湅瑙佸け璐ャ€?
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

        // 鍚屼竴浠界粯鍒朵唬鐮侊紝鍙槸鐢诲埌绂诲睆浣嶅浘涓婏細灞忓箷涓婄殑閿欏湪杩欓噷涔熶細閿?
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
