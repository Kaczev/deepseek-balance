// trayprobe -- 托盘图标与右键菜单的离线证明（设计 §10.3、F2b、F4）。
//
//   trayprobe                  跑全部检查（不碰通知区域：不 NIM_ADD、不弹菜单）
//   trayprobe --out=<目录>      把每档 HICON 导成 PNG，并连浅色/深色底对照图一起导出来
//
// 它验四件事，每件只有一种问法：
//   1. **图标资源真的在 exe 里**：本程序带 src/tray.rc，所以这里读到的 RCDATA 与
//      dshb.exe 里那份是**同一份字节**（都来自编译期的 icon.png），而不是从磁盘读 PNG。
//      "重新编译不需要外部文件存在"要验的正是"字节在二进制里"。
//   2. **alpha 保住了**：1141×1049 缩到 16/20/24/32 之后，四角必须仍然 alpha=0，
//      四条边的中点也必须 alpha=0（黑方块就是"四角不透明"的样子）。
//      浅色底上的对照图是给人看的，而判据是数：四角 alpha、边缘的 alpha 斜坡。
//   3. **命中测试**：我们自己图标的矩形内 -> 不取消；菜单矩形内 -> 不取消；
//      别的程序的托盘图标 / 时钟 / 任务栏空白 / 桌面 -> **一律取消**（这次修的就是这条）。
//   4. **菜单里只有「关闭」**：GetMenuItemCount + GetMenuStringW，比截图硬。
//
// ★ 缩放与建图标用的是 src/tray.cpp 里**同一个** detail::CreateScaledIcon：探针里再抄
//   一遍那段代码，量到的就是抄件而不是托盘里那个东西。
// ★ 为什么必须是独立进程：图标资源在 exe 里，托盘矩形的来源是外壳
//   （Shell_NotifyIconGetRect），菜单窗口属于创建它的进程。在 dshb 进程里"顺手量一下"
//   量到的是 dshb 的内部状态，量不出"外壳到底给了什么"。
#include "tray.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

bool Req(const char* id, const std::string& what, bool ok, const std::string& evidence) {
    std::printf("%s: %s | %s", ok ? "PASS" : "FAIL", id, what.c_str());
    if (!evidence.empty()) std::printf(" | %s", evidence.c_str());
    std::printf("\n");
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
    }
    return ok;
}

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// 宽格式串（日志里那些带 L"..." 的句子用这个）：结果仍按 UTF-8 出来，
// 与控制台代码页一致，重定向到文件也不会乱码。
std::wstring FmtW(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, args);
    va_end(args);
    return std::wstring(buf);
}

std::wstring Widen(const char* utf8) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), need);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// ---------------------------------------------------------------------------
// 像素：32bppBGRA（直通，非预乘），自上而下
// ---------------------------------------------------------------------------
struct Image {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> bgra;

    unsigned char At(int x, int y, int channel) const {
        return bgra[(static_cast<std::size_t>(y) * w + x) * 4 + channel];
    }
    unsigned char Alpha(int x, int y) const { return At(x, y, 3); }
};

IWICImagingFactory* Wic() {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return nullptr;
    }
    return wic;
}

// 32bppBGRA（直通）-> PNG。★ 必须声明成 32bppBGRA 让 WIC 去做预乘：声明成 PBGRA 会把
// 直通数据当预乘读，导出的整体偏暗（renderer.cpp 里踩过同一个坑，这里不重犯）。
bool SavePng(const std::wstring& path, const Image& img) {
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* bag = nullptr;
    do {
        if (FAILED(wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, &bag))) break;
        if (FAILED(frame->Initialize(bag))) break;
        if (FAILED(frame->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h)))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;
        const UINT stride = static_cast<UINT>(img.w) * 4;
        if (FAILED(frame->WritePixels(static_cast<UINT>(img.h), stride,
                                      static_cast<UINT>(img.bgra.size()),
                                      const_cast<BYTE*>(img.bgra.data())))) {
            break;
        }
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);
    if (bag) bag->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    wic->Release();
    return ok;
}

// HICON -> 32bppBGRA（直通）。走 GetIconInfo + GetDIBits：32 位图标的 alpha 就在 DIB
// 的第四个字节里，而"alpha 有没有进图标"正是要量的东西。（GetIconInfo 给的位图必须删，
// 否则是 GDI 句柄泄漏。）
// ★ 尺寸从位图自己身上读（GetObject），**不**由调用方报一个尺寸进来：图标内容可以不是
//   正方形（icon.png 是 1141×1049，等比缩放之后画布里的内容就不是正方形），拿一个想当然
//   的尺寸去 GetDIBits 会直接失败 —— 这正是这里第一版踩的坑。
bool HiconToImage(HICON icon, Image* out) {
    if (!icon || !out) return false;
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    BITMAP bm{};
    if (GetObject(ii.hbmColor, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth > 0 &&
        bm.bmHeight > 0) {
        const int w = bm.bmWidth;
        const int h = bm.bmHeight;
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(bih);
        bih.biWidth = w;
        bih.biHeight = -h;   // 自上而下，与探针内部的行序一致
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        out->w = w;
        out->h = h;
        out->bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
        HDC dc = GetDC(nullptr);
        const int lines = GetDIBits(dc, ii.hbmColor, 0, static_cast<UINT>(h), out->bgra.data(),
                                    reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        ok = lines == h;
    }
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return ok;
}

// 叠在纯色底上（人眼判据那张）。浅色底是主判据：黑方块在浅底上一眼就看见。
Image CompositeOver(const Image& icon, int r, int g, int b) {
    Image out;
    out.w = icon.w;
    out.h = icon.h;
    out.bgra.assign(icon.bgra.size(), 0);
    for (std::size_t i = 0; i < icon.bgra.size(); i += 4) {
        const double a = icon.bgra[i + 3] / 255.0;
        out.bgra[i + 0] = static_cast<unsigned char>(icon.bgra[i + 0] * a + b * (1.0 - a) + 0.5);
        out.bgra[i + 1] = static_cast<unsigned char>(icon.bgra[i + 1] * a + g * (1.0 - a) + 0.5);
        out.bgra[i + 2] = static_cast<unsigned char>(icon.bgra[i + 2] * a + r * (1.0 - a) + 0.5);
        out.bgra[i + 3] = 255;
    }
    return out;
}

struct IconStats {
    int transparentPx = 0;   // alpha == 0
    int opaquePx = 0;        // alpha == 255
    int partialPx = 0;       // 0 < alpha < 255
    int maxAlpha = 0;
};

IconStats Measure(const Image& img) {
    IconStats s{};
    for (std::size_t i = 0; i < img.bgra.size(); i += 4) {
        const int a = img.bgra[i + 3];
        if (a == 0) ++s.transparentPx;
        else if (a == 255) ++s.opaquePx;
        else ++s.partialPx;
        if (a > s.maxAlpha) s.maxAlpha = a;
    }
    return s;
}

std::wstring g_outDir;

// ---------------------------------------------------------------------------
// 1+2) 图标：资源在不在、各档 alpha 保没保住、导出证据
// ---------------------------------------------------------------------------
void CheckIcons() {
    // 裸字节只为看"大小 + PNG 签名"。缩放不在这里做：见 detail::CreateScaledIcon。
    std::vector<unsigned char> png;
    if (HRSRC res = FindResourceW(nullptr, L"DSHB_ICON_PNG", RT_RCDATA)) {
        const DWORD size = SizeofResource(nullptr, res);
        HGLOBAL loaded = LoadResource(nullptr, res);
        const void* data = loaded ? LockResource(loaded) : nullptr;
        if (data && size > 0) {
            png.assign(static_cast<const unsigned char*>(data),
                       static_cast<const unsigned char*>(data) + size);
        }
    }
    if (!Req("T1", "图标资源打进二进制（本 exe 自带的 RCDATA，不是从磁盘读 PNG）", !png.empty(),
             png.empty() ? "FindResourceW 找不到 RCDATA \"DSHB_ICON_PNG\""
                         : Fmt("RCDATA 字节数=%zu", png.size()))) {
        return;
    }

    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const bool sigOk = png.size() > 8 && std::memcmp(png.data(), sig, 8) == 0;
    Req("T2", "内嵌字节的 PNG 签名正确", sigOk,
        Fmt("前 8 字节=%02X %02X %02X %02X %02X %02X %02X %02X", png[0], png[1], png[2], png[3],
            png[4], png[5], png[6], png[7]));

    // 解码链就是 TrayIcon::Install 用的那一条（detail::OpenEmbeddedIconSource）。
    void* stream = nullptr;
    void* decoder = nullptr;
    unsigned int srcW = 0, srcH = 0, fmtData1 = 0;
    void* factory = nullptr;
    GUID fmt{};
    void* frame = dshb::detail::OpenEmbeddedIconSource(&stream, &decoder, &factory, &srcW, &srcH,
                                                      &fmt);
    if (!Req("T3", "内嵌 PNG 能被解码并转成带 alpha 的格式（运行时就是这么拿图标的）",
             frame != nullptr,
             frame ? Fmt("源尺寸=%ux%u 转换后像素格式 Data1=0x%08lX", srcW, srcH,
                         static_cast<unsigned long>(fmt.Data1))
                   : "解码链失败")) {
        dshb::detail::ReleaseIconSource(frame, stream, decoder, factory);
        return;
    }

    // ★ 这一步是**必须**的，不是保险：实测这张 icon.png 解码后默认是 24bppBGR（无 alpha），
    //   透明底会被拍成白色 —— 缩到 16 px 就是"任务栏上带一块白底"。所以这里判的是
    //   "转换之后确实是带 alpha 的 32bpp"，而不是"解码器给的恰好带 alpha"。
    //   带 alpha 的两种都是合格答案（BGRA 直通 / PBGRA 预乘），缩放那一步两种都认。
    const bool hasAlpha = IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) ||
                          IsEqualGUID(fmt, GUID_WICPixelFormat32bppPBGRA);
    Req("T4", "强制转换之后源带 alpha 通道（32bppBGRA / 32bppPBGRA 二者之一）", hasAlpha,
        Fmt("转换后 Data1=0x%08lX（32bppBGRA=0x6F8D8A6E / 32bppPBGRA=0x6F8D8A6F）",
            static_cast<unsigned long>(fmt.Data1)));

    const unsigned int sizes[] = {16, 20, 24, 32};
    for (unsigned int px : sizes) {
        Image img;
        img.w = static_cast<int>(px);
        img.h = static_cast<int>(px);
        img.bgra.assign(static_cast<std::size_t>(px) * px * 4, 0);
        unsigned int contentW = 0, contentH = 0;
        HICON icon = dshb::detail::CreateScaledIcon(frame, px, img.bgra.data(), &contentW, &contentH);
        if (!icon) {
            Req(Fmt("T5-%u", px).c_str(), Fmt("%ux%u HICON 生成成功", px, px), false,
                "CreateScaledIcon 返回空");
            continue;
        }
        const IconStats st = Measure(img);
        const int last = static_cast<int>(px) - 1;
        // ★ 判据只放**能证明**的那一条：四角必须是 alpha=0。
        //   "浅色任务栏下不是黑方块"在像素层的意思就是"方块的四角透了"，而 alpha 一旦在
        //   某一步被丢掉（解码时拍平成 24bpp、或 CreateIconIndirect 没带 alpha），四角
        //   立刻变成 255 —— T7 再从 HICON 读回来交叉验证一次。
        // ★ 四边中点**不能**当判据：实测源图是"整块黑色圆角方块"满幅铺到边界，
        //   四条边的中点本来就是不透明的（源图 (570,0)=(0,0,0,255)）。拿它当判据等于
        //   给测试编了一条源图不满足的假设。真实覆盖率在 T6 里量并如实报出来。
        const bool cornersClear = img.Alpha(0, 0) == 0 && img.Alpha(last, 0) == 0 &&
                                  img.Alpha(0, last) == 0 && img.Alpha(last, last) == 0;
        Req(Fmt("T5-%u", px).c_str(),
            Fmt("%ux%u：四角全透明（浅色任务栏下不是黑方块）", px, px), cornersClear,
            Fmt("四角 alpha=(%d,%d,%d,%d)；四边中点 alpha=(上%d 下%d 左%d 右%d)；透明%d 不透明%d "
                "半透明%d 最大alpha=%d",
                img.Alpha(0, 0), img.Alpha(last, 0), img.Alpha(0, last), img.Alpha(last, last),
                img.Alpha(px / 2, 0), img.Alpha(px / 2, last), img.Alpha(0, px / 2),
                img.Alpha(last, px / 2), st.transparentPx, st.opaquePx, st.partialPx, st.maxAlpha));

        // ★ 边缘"有没有硬锈"是数得出来的：缩放的插值若把边缘做成硬切，就不会有
        //   0 < alpha < 255 的像素（从 0 直接跳到 255）。有半透明像素说明是抗锯齿的斜坡。
        Req(Fmt("T6-%u", px).c_str(),
            Fmt("%ux%u：边缘有抗锯齿斜坡（半透明像素 > 0，不是硬切）", px, px), st.partialPx > 0,
            Fmt("半透明像素=%d 个（占 %u 个像素的 %.1f%%）", st.partialPx, px * px,
                100.0 * st.partialPx / static_cast<double>(px * px)));

        // T6b：如实报出这块图标在 16 px 下的"黑得有多满"。**这不是判据**，是给所有者看的事实：
        // 源图是整块黑色圆角方块（透明只占四角），缩到 16 px 之后黑区覆盖率很高。
        // 要不要给图标留内边距是**资产侧的设计决定**，代码这边只负责别让它再多出一块白底/黑底。
        const int opaquePercent = static_cast<int>(100.0 * st.opaquePx / (px * px) + 0.5);
        if (px == 16) {
            const Image onLight = CompositeOver(img, 0xF0, 0xF0, 0xF0);
            long long sum = 0;
            for (std::size_t i = 0; i < onLight.bgra.size(); i += 4) {
                sum += onLight.bgra[i] + onLight.bgra[i + 1] + onLight.bgra[i + 2];
            }
            const double meanLuma = sum / (3.0 * px * px);
            Req("T6b", "16x16 叠在浅色底(#f0f0f0)上的实际观感：如实报出平均亮度（非判据）", true,
                Fmt("不透明像素占 %d%%，叠加后平均通道值=%.1f/255（底是 240；数值越低=这块图标"
                    "越黑越满，源图是整块黑色圆角方块、没有内边距）",
                    opaquePercent, meanLuma));
        }

        if (!g_outDir.empty()) {
            SavePng(g_outDir + FmtW(L"icon-%ux%u.png", px, px), img);
            SavePng(g_outDir + FmtW(L"icon-%ux%u-on-light.png", px, px),
                    CompositeOver(img, 0xF0, 0xF0, 0xF0));
            SavePng(g_outDir + FmtW(L"icon-%ux%u-on-dark.png", px, px),
                    CompositeOver(img, 0x20, 0x20, 0x20));
        }

        // 从 HICON 再读回来（不只信我们手里那份缓冲）：托盘拿到的就是这个 HICON，
        // 若 CreateIconIndirect 把 alpha 丢了，这里读回来的角像素会是 255。
        Image roundTrip;
        if (HiconToImage(icon, &roundTrip)) {
            const int rtAlpha = roundTrip.Alpha(0, 0);
            Req(Fmt("T7-%u", px).c_str(),
                Fmt("%ux%u：HICON 里读回来的角像素 alpha 仍然是 0（alpha 真的进了图标）", px, px),
                rtAlpha == 0,
                Fmt("GetIconInfo+GetDIBits 读回 %dx%d 角 alpha=%d", roundTrip.w, roundTrip.h,
                    rtAlpha));
            if (!g_outDir.empty()) {
                SavePng(g_outDir + FmtW(L"icon-%ux%u-roundtrip.png", px, px), roundTrip);
            }
        } else {
            Req(Fmt("T7-%u", px).c_str(), "HICON 能读回像素", false,
                "GetIconInfo/GetDIBits 失败");
        }
        DestroyIcon(icon);
    }

    dshb::detail::ReleaseIconSource(frame, stream, decoder, factory);
}

// ---------------------------------------------------------------------------
// 3) 命中测试（纯函数：按坐标问）
// ---------------------------------------------------------------------------
void CheckClassifier() {
    dshb::HitGeometry g{};
    g.icon = RECT{1800, 1040, 1824, 1064};   // 我们自己的图标：任务栏右侧一个 24x24 格子
    g.iconSource = dshb::IconRectSource::System;
    g.menu = RECT{1600, 990, 1860, 1030};    // 菜单弹在图标左上方：260x40
    g.menuValid = true;

    struct Case {
        const char* what;
        POINT pt;
        dshb::OutsideHit want;
    };
    const Case cases[] = {
        {"我们自己的图标正中", POINT{1812, 1052}, dshb::OutsideHit::Ignore},
        {"我们自己的图标左上角", POINT{1800, 1040}, dshb::OutsideHit::Ignore},
        {"我们自己的图标右下角内侧", POINT{1823, 1063}, dshb::OutsideHit::Ignore},
        {"我们的菜单正中", POINT{1730, 1010}, dshb::OutsideHit::Ignore},
        {"我们的菜单左上角", POINT{1600, 990}, dshb::OutsideHit::Ignore},
        {"图标右边一像素（格子之外）", POINT{1824, 1052}, dshb::OutsideHit::Cancel},
        {"图标正上方一像素", POINT{1812, 1039}, dshb::OutsideHit::Cancel},
        // ★ 下面这两条就是这次修掉的毛病：别的程序的托盘图标紧挨着我们，点击必须取消。
        {"别的程序的托盘图标（左邻的格子）", POINT{1776, 1052}, dshb::OutsideHit::Cancel},
        {"别的程序的托盘图标（再左一个）", POINT{1752, 1052}, dshb::OutsideHit::Cancel},
        {"时钟区域（TrayNotifyWnd 右侧）", POINT{1900, 1052}, dshb::OutsideHit::Cancel},
        {"任务栏空白处", POINT{900, 1052}, dshb::OutsideHit::Cancel},
        {"开始按钮附近", POINT{30, 1052}, dshb::OutsideHit::Cancel},
        {"桌面正中", POINT{900, 500}, dshb::OutsideHit::Cancel},
        {"菜单右下角之外", POINT{1861, 1031}, dshb::OutsideHit::Cancel},
    };

    int mismatch = 0;
    std::string detail;
    for (const Case& c : cases) {
        const dshb::OutsideHit got = dshb::ClassifyOutsideClick(c.pt, g);
        if (got != c.want) ++mismatch;
        if (!detail.empty()) detail += "; ";
        detail += Fmt("(%ld,%ld)%s=%s", c.pt.x, c.pt.y, c.what,
                      got == dshb::OutsideHit::Cancel ? "取消" : "不取消");
    }
    Req("T8",
        Fmt("%zu 个落点全部判对（只有我们自己的图标与菜单不取消）",
            sizeof(cases) / sizeof(cases[0])),
        mismatch == 0, detail);

    // 图标矩形还不知道时（安装失败 / 还没装 / 折进了溢出区）：一切外部点击都取消 ——
    // "宁可多取消"那一侧。
    dshb::HitGeometry none{};
    none.iconSource = dshb::IconRectSource::None;
    Req("T9", "图标矩形未知时一律取消（宁可多取消，不误吞）",
        dshb::ClassifyOutsideClick(POINT{1812, 1052}, none) == dshb::OutsideHit::Cancel &&
            dshb::ClassifyOutsideClick(POINT{900, 500}, none) == dshb::OutsideHit::Cancel,
        "iconSource=None 时：原来图标的位置与桌面两处都判取消");

    // ★★ T10：**取不到矩形 = 我们不知道图标在哪 = 一律取消**，包括托盘区域内部。
    //
    //   这一条是 2026-09-19 修掉的**第二处同类偏离**：旧实现在 Shell_NotifyIconGetRect
    //   失败时，把**整块 TrayNotifyWnd 的矩形**当成图标矩形存下来（那时叫 Degraded），
    //   而命中测试只看矩形 —— 于是"点别人的托盘图标/时钟不取消"这个毛病从"按窗口类判"
    //   原样搬到了"按矩形判"这条路上。同一类错误换了个判据而已。
    //   现在那一档整个删掉了（IconRectSource 只有 None/System 两档）：取不到 -> None。
    //
    //   构造出来的形状就是旧实现的退化值：rect = 整块托盘区域（含**别人的**图标与时钟），
    //   而 iconSource 是"我们不知道"。三个落点必须全是 Cancel，一个都不能是 Ignore。
    dshb::HitGeometry degraded = g;
    degraded.iconSource = dshb::IconRectSource::None;   // 取不到 = None
    degraded.icon = RECT{1700, 1030, 1990, 1074};       // 曾经被当成"图标矩形"的整块托盘区
    const dshb::OutsideHit inOwn =
        dshb::ClassifyOutsideClick(POINT{1812, 1052}, degraded);      // 我们图标大致的位置
    const dshb::OutsideHit inOther =
        dshb::ClassifyOutsideClick(POINT{1752, 1052}, degraded);      // 别的程序的图标
    const dshb::OutsideHit inClock =
        dshb::ClassifyOutsideClick(POINT{1960, 1052}, degraded);      // 时钟区域
    const dshb::OutsideHit outside =
        dshb::ClassifyOutsideClick(POINT{900, 500}, degraded);        // 桌面
    Req("T10",
        "图标矩形取不到时不拿整块托盘区域顶替：托盘区内(含别人的图标/时钟)与区外**一律取消**",
        inOwn == dshb::OutsideHit::Cancel && inOther == dshb::OutsideHit::Cancel &&
            inClock == dshb::OutsideHit::Cancel && outside == dshb::OutsideHit::Cancel,
        Fmt("rect=(1700,1030,1990,1074) source=None：我们图标位置=%s 别人的图标=%s 时钟=%s "
            "区外桌面=%s（四个都必须取消）",
            inOwn == dshb::OutsideHit::Cancel ? "取消" : "不取消",
            inOther == dshb::OutsideHit::Cancel ? "取消" : "不取消",
            inClock == dshb::OutsideHit::Cancel ? "取消" : "不取消",
            outside == dshb::OutsideHit::Cancel ? "取消" : "不取消"));

    // T10b：同一个矩形、但**来源是 System**（系统真给了精确矩形）时，那里面就是"我们的图标"。
    // 它不是给 T10 打补丁，而是把 T10 的另一半钉住：判据是**来源**，不是矩形本身 ——
    // 否则 T10 可以通过"InsideIcon 永远返回 false"这种把功能删掉的写法骗过去。
    dshb::HitGeometry exact = degraded;
    exact.iconSource = dshb::IconRectSource::System;
    Req("T10b", "同一块矩形来源=System 时它就是我们的图标（判据是来源，不是矩形）",
        dshb::ClassifyOutsideClick(POINT{1812, 1052}, exact) == dshb::OutsideHit::Ignore,
        "source=System + 同一块矩形 -> 矩形内不取消（对照 T10：source=None 时同一处取消）");
}

// ---------------------------------------------------------------------------
// 4) 菜单：只有一项
// ---------------------------------------------------------------------------
void CheckMenu() {
    dshb::TrayIcon tray;
    HMENU menu = tray.BuildMenu();
    if (!menu) {
        Req("T11", "菜单能建起来", false, "BuildMenu 返回空");
        return;
    }
    const int count = GetMenuItemCount(menu);
    wchar_t text[128] = L"";
    const int len = (count > 0) ? GetMenuStringW(menu, 0, text, 128, MF_BYPOSITION) : 0;
    const unsigned int state = (count > 0) ? GetMenuState(menu, 0, MF_BYPOSITION) : 0;

    Req("T11", "菜单项数 = 1", count == 1, Fmt("GetMenuItemCount=%d", count));
    Req("T12", "唯一那一项的文字是「关闭」", len > 0 && std::wcscmp(text, L"关闭") == 0,
        Fmt("GetMenuStringW(0)=L\"%ls\"（%d 个字符）", text, len));
    Req("T13", "这一项可点（不是灰的、不是分隔符）",
        (state & (MF_GRAYED | MF_DISABLED | MF_SEPARATOR)) == 0, Fmt("GetMenuState=0x%04X", state));

    const UINT cmd = GetMenuItemID(menu, 0);
    Req("T14", "这一项的 id 就是 kTrayMenuClose", cmd == dshb::kTrayMenuClose,
        Fmt("GetMenuItemID(0)=%u kTrayMenuClose=%u", cmd, dshb::kTrayMenuClose));

    // 被否掉的那些项一个都不许出现（设计 §10.3：没有「显示/隐藏窗口」、没有第二个「退出」）。
    bool hasForbidden = false;
    std::wstring found;
    for (int i = 0; i < count; ++i) {
        wchar_t t[128] = L"";
        if (GetMenuStringW(menu, i, t, 128, MF_BYPOSITION) > 0) {
            if (std::wcsstr(t, L"显示") || std::wcsstr(t, L"隐藏") || std::wcsstr(t, L"退出")) {
                hasForbidden = true;
                found += t;
            }
        }
    }
    Req("T15", "没有「显示/隐藏窗口」、也没有第二个「退出」", !hasForbidden,
        hasForbidden ? Fmt("发现了：%ls", found.c_str()) : "全部项里都没有这几个词");
    tray.DestroyMenu();
}

// ---------------------------------------------------------------------------
// T16..T18：消息约定与"图标矩形从哪来"
// ---------------------------------------------------------------------------
void CheckMessageContract() {
    Req("T16", "托盘消息与菜单约定的常量（main.cpp 的 WndProc 就按它们分派）",
        dshb::kMsgTrayIcon == WM_APP + 2 && dshb::kTrayMenuClose == 1 &&
            std::wcscmp(dshb::kTrayCloseText, L"关闭") == 0,
        Fmt("kMsgTrayIcon=0x%04X（WM_APP+1 已被单实例的 kMsgActivate 占用，所以从 +2 起）"
            " kTrayMenuClose=%u 菜单文字=L\"%ls\"",
            dshb::kMsgTrayIcon, dshb::kTrayMenuClose, dshb::kTrayCloseText));

    HWND trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notifyWnd = trayWnd ? FindWindowExW(trayWnd, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT r{};
    const bool notifyOk = notifyWnd && GetWindowRect(notifyWnd, &r);
    Req("T17", "托盘区域（TrayNotifyWnd）在这台机器上存在（决定能否拿到真实坐标）", notifyOk,
        notifyOk ? Fmt("TrayNotifyWnd 矩形=(%ld,%ld,%ld,%ld) 尺寸=%ldx%ld", r.left, r.top, r.right,
                       r.bottom, r.right - r.left, r.bottom - r.top)
                 : "找不到 Shell_TrayWnd/TrayNotifyWnd");

    // 探针还没 NIM_ADD，所以这里**预期**就是取不到；dshb 跑起来之后同一句话会给出精确矩形，
    // 两次的差别本身就是证据。
    dshb::TrayIcon probe;
    dshb::IconRectSource src = dshb::IconRectSource::None;
    const RECT none = probe.IconRect(&src);
    Req("T18", "还没有 NIM_ADD 时图标矩形取不到（预期 None）",
        src == dshb::IconRectSource::None,
        Fmt("IconRectSource=%d 矩形=(%ld,%ld,%ld,%ld)", static_cast<int>(src), none.left, none.top,
            none.right, none.bottom));

    // T19：屏幕上那个菜单面板**是一个真窗口**，而且**不是**系统菜单。
    //
    // ★ 这两句为什么值得单独验：这一版把 TrackPopupMenu（模态、收不到输入、永不返回）换成了
    //   自建面板，而面板要能被点中，前提是"窗口类注册成功、窗口真的能创建出来" ——
    //   注册失败的后果不是崩溃，是**右键点托盘图标什么都不会发生**（静默失效）。
    //   类名必须是我们自己的：用系统的 "#32768" 又回到"分不清是谁的菜单"那条老路。
    // ★ 探针是控制台进程、没有消息循环，所以这里不点它（点了也没人处理）——
    //   "点得动"那件事由真进程的 --tray-menu-test 在屏幕上证明（SendInput + 日志）。
    {
        dshb::TrayIcon panelTray;   // 构建期就把面板实现链进来（不是死代码）
        HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        HWND panel = owner ? panelTray.PopupMenu(owner) : nullptr;
        const bool panelOk = panel != nullptr && IsWindow(panel);
        wchar_t cls[64] = L"";
        if (panel) GetClassNameW(panel, cls, 64);
        const bool ownClass = panelOk && std::wcscmp(cls, L"DshbTrayMenu") == 0;
        // 同一份 `HMENU`（探针量"有哪些项"的那个）与面板是同一对常量，所以顺手复量一次项数。
        HMENU probeMenu = panelTray.BuildMenu();
        const int menuCount = probeMenu ? GetMenuItemCount(probeMenu) : -1;
        panelTray.DestroyMenu();
        if (owner) DestroyWindow(owner);
        Req("T19", "菜单面板是自己那个类（真窗口、不是系统 #32768 菜单）", panelOk && ownClass,
            Fmt("面板=%ls 类名=L\"%ls\" 同一份菜单的项数=%d", panelOk ? L"已创建" : L"没创建出来",
                cls, menuCount));
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--out=", 6) == 0) {
            std::wstring w = Widen(argv[i] + 6);
            if (!w.empty() && w.back() != L'\\') w += L'\\';
            g_outDir = w;
        }
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("FAIL: COM 初始化失败 hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 9;
    }

    std::printf("== trayprobe：托盘图标与菜单 ==\n");
    std::printf("-- 本进程自带图标资源：下面读到的 RCDATA 就是 dshb.exe 里那一份字节\n");
    if (!g_outDir.empty()) {
        std::printf("-- PNG 导出目录：%ls\n", g_outDir.c_str());
    } else {
        std::printf("-- 未给 --out=，跳过 PNG 导出（像素证据要加 --out=<目录>）\n");
    }

    CheckIcons();
    CheckClassifier();
    CheckMenu();
    CheckMessageContract();

    std::printf("-- 小计：通过 %d，失败 %d\n", g_passed, g_failed);
    CoUninitialize();
    return g_failed == 0 ? 0 : 1;
}
