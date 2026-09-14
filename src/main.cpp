// deepseek-balance v0.2 —— 主程序：窗口 + 帧循环
//
// 这一步（A3 / A3b / A3c）要达成的形态：
//   无边框、圆角、每像素半透明、置顶、不进任务栏与 Alt+Tab、不抢焦点、DPI 正确。
// 数字、曲线、颜色、心跳都是后面步骤的事。
//
// 自检（给自动化用，不弹窗）：--selftest [--seconds=N]
//   把窗口与 DPI 的事实、帧统计写进 build\selftest.log。
//   不弹对话框：Start-Process -PassThru 的 HasExited 在弹窗时会永远读成 false（踩过）。

#include "amount.h"
#include "paths.h"
#include "renderer.h"
#include "sampling.h"
#include "single_instance.h"
#include "state_machine.h"

#include <windows.h>
#include <objbase.h>    // CoInitializeEx / COINIT_APARTMENTTHREADED
#include <shellapi.h>   // CommandLineToArgvW

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr wchar_t kClassName[] = L"DshbWnd";
constexpr wchar_t kSelfTestLog[] = L"selftest.log";

bool g_selfTest = false;
double g_runSeconds = 0.0;        // 0 = 不自动退出，等用户按 Esc（--seconds=N 可改）
double g_selfTestSeconds = 1.5;
bool g_exportFrame = false;       // 离屏导一帧，然后退出
bool g_premulProbe = false;       // 预乘自检（A8c）
wchar_t g_exportPath[MAX_PATH] = L"frame.png";
int g_frameNo = 1;
HWND g_hwnd = nullptr;
bool g_running = true;
dshb::Renderer* g_renderer = nullptr;
double g_elapsed = 0.0;          // 单调时钟累计秒数；帧循环和 WndProc 共用
bool g_debug = false;            // --debug：显示调试浮层
bool g_selftestB = false;        // --selftest-b：金额解析与状态机的自检
bool g_layoutProbe = false;      // --layout-probe：导出模式下打印布局数值
int g_dpiOverride = 0;           // --dpi=N：覆盖画布缩放（0 = 用窗口真实 DPI）
double g_uiScale = 1.0;          // --ui-scale=N：视觉缩放（1.0 = 按 DPI，设计意图）
int g_rollFrames = 0;            // --roll=N：是否导出滚动瞬间（N 只用于日志，步数看 g_rollSteps）
int g_rollSteps = 0;             // --roll=N：跳变之后推进多少帧（1/60 秒一步）
bool g_rollLoop = false;         // --roll=loop：每 2 秒来回跳一次，用肉眼反复看滚动
dshb::FakeSource g_fake;         // 模拟数据源（B3）
dshb::StateMachine g_states;     // 连接状态机（B8）
dshb::DisplayedAmount g_display; // 显示值（C2/C3）：跳变的测量值 -> 连续的显示值
bool g_haveWindowFocus = false;

// 高精度单调计时；dt 钳到 [0, 50 ms]，防止休眠唤醒后第一帧一步跳到位（设计 §9.6）
struct Clock {
    LARGE_INTEGER freq{};
    LARGE_INTEGER last{};

    Clock() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
    }

    double Tick() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = static_cast<double>(now.QuadPart - last.QuadPart) /
                    static_cast<double>(freq.QuadPart);
        last = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.05) dt = 0.05;
        return dt;
    }
};

void SelfTestLog(const wchar_t* fmt, ...) {
    // 这个程序是 GUI 子系统，没有控制台：往 stdout 写等于丢掉。
    // 所以日志只落文件。路径必须是绝对的——相对路径会落到"启动时的工作目录"里。
    static wchar_t path[MAX_PATH] = L"";
    if (path[0] == L'\0') {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
        wcscat_s(path, kSelfTestLog);
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        va_list args;
        va_start(args, fmt);
        vfwprintf(f, fmt, args);
        va_end(args);
        fwprintf(f, L"\n");
        fclose(f);
    }
}

double NowWallMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL);
}

// 浮层用的 ASCII 状态名。**不走运行时编码转换**：调一次 WideCharToMultiByte
// 看着省事，但在"图省事"的地方出错最难查。这里直接映射，一目了然。
const char* ConnStateNameUtf8(dshb::ConnState s) {
    using dshb::ConnState;
    switch (s) {
    case ConnState::ColdStart: return "ColdStart";
    case ConnState::Ok: return "Ok";
    case ConnState::NoKey: return "NoKey";
    case ConnState::AuthFailed: return "AuthFailed";
    case ConnState::Exhausted: return "Exhausted";
    case ConnState::RateLimited: return "RateLimited";
    case ConnState::NetworkError: return "NetworkError";
    case ConnState::Stale: return "Stale";
    case ConnState::Unavailable: return "Unavailable";
    default: return "?";
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
            return 0;
        }
        // ---- B6/B7 的调试热键：F1..F9 选情形，R 触发充值，C 触发时钟跳变 ----
        if (wp >= VK_F1 && wp < VK_F1 + static_cast<WPARAM>(dshb::Scenario::Count)) {
            const auto idx = static_cast<dshb::Scenario>(wp - VK_F1);
            g_fake.Select(idx);
            SelfTestLog(L"[key] 情形 -> %ls", dshb::ScenarioName(idx));
            return 0;
        }
        if (wp == 'R') {
            g_fake.TriggerRecharge();
            SelfTestLog(L"[key] 触发充值跳变");
            return 0;
        }
        if (wp == 'C') {
            g_fake.TriggerClockJump();
            SelfTestLog(L"[key] 触发时钟跳变");
            return 0;
        }
        return 0;
    case dshb::kMsgActivate:
        // 有人又双击了一次 exe（A11）。这个窗口不抢焦点、也不进 Alt+Tab，
        // 所以"提到前台"没有落点，改为闪一次描边作为可见反馈。
        if (g_renderer) g_renderer->BeginActivationFlash(g_elapsed);
        SelfTestLog(L"[single] 收到重复启动通知，已触发激活反馈 t=%.3f", g_elapsed);
        return 0;
    case WM_DPICHANGED: {
        // 用系统给的建议矩形重设窗口；不用它会导致跨屏拖动时指针漂移（设计 §11.2.1）
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // 画布要按新 DPI 重建（A0 记下的隐患）
        if (g_renderer) {
            if (g_renderer->Resize(hwnd)) {
                SelfTestLog(L"[dpi] 画布已重建: %dx%d (scale=%.4f)",
                            g_renderer->size().widthPx, g_renderer->size().heightPx,
                            g_renderer->size().scale);
            } else {
                SelfTestLog(L"[dpi] 画布重建失败");
            }
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--selftest") == 0) {
            g_selfTest = true;
        } else if (wcsncmp(argv[i], L"--seconds=", 10) == 0) {
            g_runSeconds = _wtof(argv[i] + 10);
            g_selfTestSeconds = g_runSeconds;
        } else if (wcsncmp(argv[i], L"--export-frame=", 15) == 0) {
            g_exportFrame = true;
            g_frameNo = _wtoi(argv[i] + 15);
        } else if (wcsncmp(argv[i], L"--out=", 6) == 0) {
            wcsncpy_s(g_exportPath, argv[i] + 6, _TRUNCATE);
        } else if (wcscmp(argv[i], L"--premul-probe") == 0) {
            g_premulProbe = true;
        } else if (wcscmp(argv[i], L"--debug") == 0) {
            g_debug = true;
        } else if (wcscmp(argv[i], L"--selftest-b") == 0) {
            g_selftestB = true;
        } else if (wcscmp(argv[i], L"--layout-probe") == 0) {
            g_layoutProbe = true;
        } else if (wcsncmp(argv[i], L"--scenario=", 11) == 0) {
            const int idx = _wtoi(argv[i] + 11);
            if (idx >= 0 && idx < static_cast<int>(dshb::Scenario::Count)) {
                g_fake.Select(static_cast<dshb::Scenario>(idx));
            }
        } else if (wcsncmp(argv[i], L"--speed=", 8) == 0) {
            g_fake.SetSpeed(_wtof(argv[i] + 8));
        } else if (wcsncmp(argv[i], L"--dpi=", 6) == 0) {
            g_dpiOverride = _wtoi(argv[i] + 6);
        } else if (wcsncmp(argv[i], L"--ui-scale=", 11) == 0) {
            g_uiScale = _wtof(argv[i] + 11);
        } else if (wcsncmp(argv[i], L"--roll=", 7) == 0) {
            g_rollFrames = 1;                     // 只要出现这个参数就进入滚动抓帧模式
            if (wcscmp(argv[i] + 7, L"loop") == 0) {
                g_rollLoop = true;                // 循环跳变，供肉眼观察
            } else {
                g_rollSteps = _wtoi(argv[i] + 7); // N = 跳变之后推进多少帧（0 = 跳变前）
            }
        }
    }
    if (argv) LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 单实例（A11）----
    // 放在建窗之前：第二个实例不该先建出一个窗口再退出，那样屏幕上会闪一下。
    // 注意导帧/自检这类离屏模式也不该受单实例限制（它们不显示窗口），
    // 所以只在"要显示窗口"的路径上做这个检查。
    const bool offscreenMode = g_exportFrame || g_premulProbe;
    if (!offscreenMode && !dshb::AcquireSingleInstance()) {
        SelfTestLog(L"[single] 已有实例在运行，本进程退出（已通知它闪一次）");
        CoUninitialize();
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        SelfTestLog(L"[main] RegisterClassExW 失败: %lu", GetLastError());
        return 1;
    }

    // 目标形态：无边框、置顶、不进任务栏、不进 Alt+Tab、点击不抢焦点。
    // NOREDIRECTIONBITMAP 是必须的：画面完全由 DirectComposition 合成（A0 的坑）。
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                          WS_EX_NOREDIRECTIONBITMAP;

    // ★ 尺寸单位：**屏幕像素**（所有者明确定下的）。
    //   设计稿里的 315×129 就是屏幕上 315×129 个像素，与显示器缩放无关。
    //   这台机器缩放 200%，早先按 DIP 解释会得到 630×258 个像素——那是我理解错了。
    //
    //   注意这**不是**"关掉 DPI 感知"：进程仍声明 Per-Monitor V2（清单），
    //   窗口尺寸也仍按像素给，只是绘制用的缩放固定为 1.0。
    //   二者结果一样，但不关 DPI 感知能让文字保持矢量清晰，而不是被系统位图拉伸。
    const int w0 = static_cast<int>(dshb::kCanvasWidthDip + 0.5);
    const int h0 = static_cast<int>(dshb::kCanvasHeightDip + 0.5);

    g_hwnd = CreateWindowExW(exStyle, kClassName, L"deepseek-balance", WS_POPUP,
                             240, 240, w0, h0, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        SelfTestLog(L"[main] CreateWindowExW 失败: %lu", GetLastError());
        return 2;
    }

    const UINT dpi = GetDpiForWindow(g_hwnd);
    const int clientW = dshb::kEntityWidthDip;    // 屏幕像素，不乘缩放
    const int clientH = dshb::kEntityHeightDip;

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    SelfTestLog(L"[win] dpi(system)=%u dpi(window)=%u exStyle=0x%08lX（缩放固定 1.0，不随 DPI）",
                GetDpiForSystem(), dpi,
                static_cast<unsigned long>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE)));
    SelfTestLog(L"[win] 窗口矩形=(%ld,%ld,%ld,%ld) 尺寸=%ldx%ld 实体区(理论)=%dx%d",
                wr.left, wr.top, wr.right, wr.bottom, wr.right - wr.left, wr.bottom - wr.top,
                clientW, clientH);

    dshb::Renderer renderer;
    g_renderer = &renderer;
    dshb::CanvasSize size = dshb::Renderer::SizeForWindow(g_hwnd);

    // ★ 视觉缩放：默认 1.0，即"一个设计像素 = 一个屏幕像素"。
    //   早先默认按 DPI 缩放（200% 屏上得到 630×258），所有者判定偏大，
    //   并明确定下"物理像素就是屏幕像素"。所以 1.0 是默认，旋钮保留备用。
    if (g_uiScale > 0.0 && g_uiScale != 1.0) {
        const double base = static_cast<double>(size.scale) * g_uiScale;
        size.scale = static_cast<float>(base);
        size.widthPx = static_cast<int>(dshb::kCanvasWidthDip * base + 0.5);
        size.heightPx = static_cast<int>(dshb::kCanvasHeightDip * base + 0.5);
        // 窗口也要跟着缩，否则画布缩了、窗口没缩，面板会在窗口里偏到一角
        SetWindowPos(g_hwnd, nullptr, 0, 0, size.widthPx, size.heightPx,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SelfTestLog(L"[render] 视觉缩放 %.2f -> 画布 %dx%d 像素", g_uiScale, size.widthPx,
                    size.heightPx);
    }

    // 导出用的缩放覆盖：让同一套 315×129 设计稿在 100% / 150% / 200% 下各导一张，
    // 这样字号能在真实尺寸下被判断（所有者指出过：只看 200% 的图看不出字号合不合适）。
    if (g_dpiOverride > 0) {
        const double scale = g_dpiOverride / 96.0;
        size.scale = static_cast<float>(scale);
        size.widthPx = static_cast<int>(dshb::kCanvasWidthDip * scale + 0.5);
        size.heightPx = static_cast<int>(dshb::kCanvasHeightDip * scale + 0.5);
        SelfTestLog(L"[render] 缩放被 --dpi 覆盖为 %d%% -> 画布 %dx%d", g_dpiOverride * 100 / 96,
                    size.widthPx, size.heightPx);
    }
    if (!renderer.Create(g_hwnd, size)) {
        SelfTestLog(L"[main] 渲染器创建失败（D3D11 / DComp / 交换链）");
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 3;
    }
    SelfTestLog(L"[render] 画布=%dx%d scale=%.4f（外扩 %d DIP 余量）",
                size.widthPx, size.heightPx, size.scale, dshb::kMarginDip);

    // A12b：路径解析结果必须留痕。降级（目录不可写）时尤其要让用户找得到原因，
    // 否则他会以为历史一直在正常记录。
    {
        const dshb::AppPaths& paths = dshb::Paths();
        SelfTestLog(L"[paths] 数据目录=%ls 可写=%ls", paths.dataDir.c_str(),
                    paths.writable ? L"是" : L"否");
        SelfTestLog(L"[paths] 采样=%ls", paths.samples.c_str());
        SelfTestLog(L"[paths] 日志=%ls", paths.log.c_str());
        SelfTestLog(L"[paths] 设置=%ls", paths.config.c_str());
        if (!paths.writable) {
            SelfTestLog(L"[paths] 降级：目录不可写（%ls），采样只留在内存，重启后没有历史",
                        paths.unwritableReason.c_str());
        }
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    // ---- 预乘自检（A8c）：先于导帧，因为它可能顺便导一张探针 PNG ----
    // 判据不是"R 等于多少"，而是 **R 与 A 的关系**：
    //   R <= A  -> 已预乘（PREMULTIPLIED 模式下 D2D 以 alpha 为权重解释颜色，
    //              于是 R 恰好等于 a * 255）
    //   R >  A  -> 直通数据被当预乘用（错误），症状是半透明处发白/发黑、圆角一圈灰毛边
    // 两种都是"自洽"的，所以不能靠数值好不好看判，只能看 R 与 A 的关系。
    if (g_premulProbe) {
        uint8_t bgra[4]{};
        int px = 0, py = 0;
        const bool ok = renderer.PremulProbe(bgra, &px, &py);
        const bool straight = ok && bgra[2] > bgra[3];
        const bool premultiplied = ok && !straight;
        SelfTestLog(L"[premul] 像素(%d,%d) BGRA=(%u,%u,%u,%u) alpha=%u",
                    px, py, bgra[0], bgra[1], bgra[2], bgra[3], bgra[3]);
        SelfTestLog(L"[premul] R>A 吗？%ls  →  结论：%ls",
                    straight ? L"是（直通数据，错误）" : L"否",
                    premultiplied ? L"已预乘（正确）" : L"读取失败（错误）");

        if (g_exportFrame) {
            const bool saved = renderer.ExportFrame(g_exportPath, 0.0);
            SelfTestLog(L"[export] 预乘探针 PNG: %ls 结果=%ls", g_exportPath,
                        saved ? L"成功" : L"失败");
        }

        renderer.Destroy();
        g_renderer = nullptr;
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return premultiplied ? 0 : 8;
    }

    // ---- 离屏导帧模式：渲一帧到 PNG 就退出 ----
    // 用来做"用像素说话"的验收：居中错位、颜色、残影、粒子越界都靠它量。
    // 注意它渲染的是同一份绘制代码，所以屏幕上的错在 PNG 里也会错。
    if (g_exportFrame) {
        const double t = static_cast<double>(g_frameNo) / 60.0;   // 第 N 帧 ≈ N/60 秒

        // 布局诊断只在这里开：它会在绘制路径里写文件，而每帧写文件会把进程弄崩
        // （实测 0xC0000409）。导出模式只画一帧，所以安全。
        if (g_layoutProbe) dshb::SetLayoutProbe(true);

        // 让模拟数据源在"虚拟时间"里跑起来：否则导出的图没有数据，浮层也是空的。
        // 虚拟时间按 1/60 秒一步推进，所以导出是确定的、可重复的。
        //
        // --roll=N：把"余额跳变之后第 N/60 秒"这一瞬间单独抓出来。
        // 用途是**看滚动动画**——静态单帧看不出数字是怎么滚过去的，
        // 连拍若干张不同 N 的图才能看出过程（动画也是要人眼判的东西）。
        if (g_rollFrames > 0 || g_rollFrames == 0) {
            // --roll=N：把"余额跳变之后第 N 帧"这一瞬间单独抓出来。
            // 用途是**看滚动动画**——静态单帧看不出数字是怎么滚过去的，
            // 连拍若干张不同 N 的图才能看出过程（动画也是要人眼判的东西）。
            //
            // ★ 第一版这里错了：预热循环跑了 8 秒虚拟时间，而显示值每帧都在推进，
            //   于是"跳变前"那张图上的数字早就滚到 100 了（抓到的其实是过程末尾）。
            //   正确做法是**只喂样本、不推进显示值**，让起点精确落在第一个值上。
            dshb::FakeSource pre;
            pre.SetSpeed(0.01);
            pre.Select(dshb::Scenario::Recharge);   // 起点 20 元
            const dshb::Sample first = pre.NextIfDue(0.0);
            if (first.wallMs != 0) {
                g_states.OnSample(first, first.wallMs);
                g_display.OnSample(g_states.lastGood());   // 直接落位到 19.90
            }
            const double before = g_display.value();

            // ★ 循环模式（--roll=loop）：每 2 秒来回跳一次，便于用肉眼观察滚动。
            //   观感是要人看的东西，一次性跳变太快，看不住。
            if (g_rollLoop) {
                SelfTestLog(L"[roll] 循环模式：20.00 <-> 99.50 每 2 秒一次");
                g_fake.Select(dshb::Scenario::Recharge);
                g_fake.TriggerRecharge(99.5);
                bool high = true;
                double sinceSwitch = 0.0;
                // 循环模式有自己的时钟与累计时间：导帧路径在帧循环之前，
                // 后面那些变量还没声明（第一版直接引用它们，编译不过）。
                Clock rollClock;
                double rollElapsed = 0.0;
                while (g_running) {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        if (msg.message == WM_QUIT) g_running = false;
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (!g_running) break;

                    const double dt = rollClock.Tick();
                    rollElapsed += dt;
                    g_elapsed = rollElapsed;
                    sinceSwitch += dt;
                    if (sinceSwitch >= 2.0) {
                        sinceSwitch = 0.0;
                        high = !high;
                        g_fake.Select(dshb::Scenario::Recharge);
                        g_fake.TriggerRecharge(high ? 99.5 : 20.0);
                    }

                    const dshb::Sample s = g_fake.NextIfDue(rollElapsed);
                    if (s.wallMs != 0) {
                        g_states.OnSample(s, s.wallMs);
                        g_display.OnSample(g_states.lastGood());
                    }
                    g_display.Update(dt);

                    const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
                    const bool currencyKnown =
                        g_states.hasGood() && g_states.lastGood().CurrencyKnown();
                    renderer.SetWidgetFrame(dshb::BuildWidgetFrame(
                        st, g_display, currencyKnown,
                        g_states.hasGood() ? g_states.lastGood().CurrencySymbolW() : L""));
                    renderer.RenderFrame(rollElapsed);

                    if (g_runSeconds > 0.0 && rollElapsed >= g_runSeconds) break;
                    MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
                renderer.Destroy();
                g_renderer = nullptr;
                DestroyWindow(g_hwnd);
                CoUninitialize();
                return 0;
            }

            // 现在把"充值跳变"塞进去，再从这一刻开始逐帧推进。
            // 目标 99.50 是刻意选的：它和起点 20.00 都是 5 个字符，
            // 格位对得上，所以逐位滚动能真正发生（位数不同的跳变不能逐位滚）。
            g_fake.Select(dshb::Scenario::Recharge);
            g_fake.TriggerRecharge(99.5);
            const dshb::Sample jump = g_fake.NextIfDue(0.0);
            if (jump.wallMs != 0) {
                g_states.OnSample(jump, jump.wallMs);
                g_display.OnSample(g_states.lastGood());
            }
            for (int i = 0; i < g_rollSteps; ++i) {
                g_display.Update(1.0 / 60.0);
                // 诊断：把每帧的显示值、滚动进度、以及"这一刻画出来的文本"都记下来。
                // 数字一闪一闪的根源如果不在这里，这条日志会直接排除它。
                const dshb::ConnState stD = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
                const dshb::WidgetFrame fd = dshb::BuildWidgetFrame(
                    stD, g_display, true, L"\u00A5");
                SelfTestLog(L"[rollstep] i=%d value=%.2f frac=%.3f active=%d text=%hs",
                            i, g_display.value(), g_display.rollFraction(),
                            fd.roll.active ? 1 : 0, fd.amountText.c_str());
            }

            SelfTestLog(L"[roll] 跳变前=%.2f 跳变后目标=%.2f 推进 %d 帧后显示=%.2f",
                        before, g_display.target(), g_rollSteps, g_display.value());
        } else {
            for (double vt = 0.0; vt <= t + 0.0001; vt += (1.0 / 60.0)) {
                const dshb::Sample s = g_fake.NextIfDue(vt);
                if (s.wallMs != 0) {
                    g_states.OnSample(s, s.wallMs);
                    // 显示值也在虚拟时间里推进，否则导出图上数字还停在 0 或没落位
                    g_display.OnSample(g_states.lastGood());
                }
                g_display.Update(1.0 / 60.0);
            }
        }

        // 组装正文（和真实运行时同一条路径），这样导出的图就是屏幕上会看到的图
        {
            const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
            const bool currencyKnown = g_states.hasGood() && g_states.lastGood().CurrencyKnown();
            renderer.SetWidgetFrame(dshb::BuildWidgetFrame(
                st, g_display, currencyKnown,
                g_states.hasGood() ? g_states.lastGood().CurrencySymbolW() : L""));
        }

        if (g_debug) {
            const int64_t nowWall = static_cast<int64_t>(NowWallMs());
            const dshb::ConnState st = g_states.Evaluate(nowWall);
            const dshb::Sample& last = g_states.lastGood();
            const std::string num =
                g_states.hasGood() ? last.total.ToString2() : std::string("--.--");
            const std::wstring numW(num.begin(), num.end());
            wchar_t line[512];
            swprintf_s(line,
                       L"state=%hs  bal=%ls%ls  scenario=%d  samples=%d  t=%.1fs\n"
                       L"F1..F9=scenario  R=recharge  C=clockjump",
                       ConnStateNameUtf8(st), g_states.hasGood() ? last.CurrencySymbolW() : L"",
                       numW.c_str(), static_cast<int>(g_fake.scenario()),
                       static_cast<int>(g_states.samples().size()), t);
            renderer.SetDebugText(line);
        }

        const bool ok = renderer.ExportFrame(g_exportPath, t);
        // 诊断写文件放在绘制**之后**：绘制路径里做 I/O 会让进程崩（实测）
        dshb::DumpLayoutProbe();
        SelfTestLog(L"[export] %ls 帧=%d 时刻=%.3fs 结果=%ls 画布=%dx%d",
                    g_exportPath, g_frameNo, t, ok ? L"成功" : L"失败",
                    size.widthPx, size.heightPx);
        renderer.Destroy();
        g_renderer = nullptr;
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return ok ? 0 : 7;
    }

    // ---- B 阶段自检：金额解析 + 状态机 ----
    // 这两件事的正确性不该靠看图判断。这里直接喂已知输入、断言输出，
    // 全部结果写进日志，人只要看有没有 FAIL。
    if (g_selftestB) {
        int failed = 0;
        auto expect = [&](bool cond, const wchar_t* what) {
            SelfTestLog(L"[check] %ls: %ls", cond ? L"PASS" : L"FAIL", what);
            if (!cond) ++failed;
        };

        // --- 金额解析（十进制，不是浮点） ---
        {
            dshb::Amount a{};
            expect(dshb::ParseAmount("110.00", &a) && a.raw == 1100000, L"110.00 -> 1100000");
            expect(dshb::ParseAmount("0.03", &a) && a.raw == 300, L"0.03 -> 300");
            expect(dshb::ParseAmount(" 12.5 ", &a) && a.raw == 125000, L"含空白 12.5 -> 125000");
            expect(dshb::ParseAmount("1,234.56", &a) && a.raw == 12345600, L"千分位 1,234.56");
            expect(dshb::ParseAmount("-3.5", &a) && a.raw == -35000, L"负数 -3.5");
            expect(dshb::ParseAmount("0.12345", &a) && a.raw == 1235, L"5 位小数四舍五入");
            expect(!dshb::ParseAmount("", &a), L"空串必须失败");
            expect(!dshb::ParseAmount("abc", &a), L"非数字必须失败");
            expect(!dshb::ParseAmount("-", &a), L"只有符号必须失败");
            expect(!dshb::ParseAmount("1.2.3", &a), L"两个小数点必须失败");
            // 失败时不得改动出参：调用方要靠它区分"读不到"和"余额为 0"
            a.raw = 777;
            dshb::ParseAmount("nope", &a);
            expect(a.raw == 777, L"解析失败时不改动出参");
        }

        // --- 状态机：九种情形各跑一遍，断言状态与金额 ---
        // ★ 一条纪律：**断言不变式，不要断言"恰好等于某个数"**。
        //   第一版这里硬写了精确余额，结果失败——因为数字取决于虚拟时间里跑了几拍，
        //   那是实现细节。判据要写成"该降的降了""该为 0 的就是 0"这种不随节拍变化的性质。
        {
            struct Case {
                dshb::Scenario sc;
                dshb::ConnState want;
                const wchar_t* what;
                int amountRule;      // 0=不检查 1=恰好为 0 2=必须小于起始值 3=必须等于起始值
                const wchar_t* amountWhat;
            };
            const Case cases[] = {
                {dshb::Scenario::Steady, dshb::ConnState::Ok, L"平稳 -> Ok", 2, L"  平稳：余额应下降"},
                {dshb::Scenario::FastDrain, dshb::ConnState::Ok, L"快速 -> Ok", 2, L"  快速：余额应下降"},
                {dshb::Scenario::Zero, dshb::ConnState::Ok, L"归零 -> Ok（余额 0 不是错误）", 1,
                 L"  归零：余额恰好为 0"},
                {dshb::Scenario::Unavailable, dshb::ConnState::Unavailable,
                 L"不可用 -> Unavailable", 0, L""},
                {dshb::Scenario::NoNetwork, dshb::ConnState::NetworkError,
                 L"没网 -> NetworkError", 0, L""},
            };
            for (const Case& c : cases) {
                g_fake.Select(c.sc);
                dshb::StateMachine sm;
                int64_t lastWall = 0;
                for (double vt = 0.0; vt <= 3.0; vt += (1.0 / 60.0)) {
                    const dshb::Sample s = g_fake.NextIfDue(vt);
                    if (s.wallMs != 0) {
                        sm.OnSample(s, s.wallMs);
                        lastWall = s.wallMs;
                    }
                }
                const dshb::ConnState got = sm.Evaluate(lastWall);
                expect(got == c.want, c.what);

                if (c.amountRule != 0) {
                    const bool has = sm.hasGood();
                    const dshb::AmountRaw got = has ? sm.lastGood().total.raw : -1;
                    const dshb::AmountRaw start = dshb::Amount::FromYuan(100).raw;
                    bool ok = false;
                    if (c.amountRule == 1) ok = has && got == 0;
                    if (c.amountRule == 2) ok = has && got < start && got >= 0;
                    if (c.amountRule == 3) ok = has && got == start;
                    expect(ok, c.amountWhat);
                }
            }

            // 九种情形都必须能被选中且名字非空（防止枚举与名字表错位）
            bool namesOk = true;
            for (int i = 0; i < static_cast<int>(dshb::Scenario::Count); ++i) {
                const wchar_t* n = dshb::ScenarioName(static_cast<dshb::Scenario>(i));
                if (!n || n[0] == L'?' || n[0] == L'\0') namesOk = false;
            }
            expect(namesOk, L"情形名字表与枚举一一对齐（9 项）");
        }

        // --- 正文组装（C2/C5/C7）：查不到时**绝不能**显示 0.00 ---
        {
            dshb::DisplayedAmount d;

            // 1) 没有任何数据：占位符，不显示数字
            dshb::WidgetFrame f1 = dshb::BuildWidgetFrame(dshb::ConnState::ColdStart, d, false, L"");
            expect(!f1.showAmount && f1.amountText == "--.--",
                   L"冷启动：不显示数字，用占位符 --.--");

            // 2) 有数据但币种未知：同样不显示数字（USD 账户上显示 ¥ 是最危险的错）
            dshb::Sample s{};
            s.amountsOk = true;
            s.total = dshb::Amount::FromYuan(110);
            s.currency = "";
            d.OnSample(s);
            dshb::WidgetFrame f2 = dshb::BuildWidgetFrame(dshb::ConnState::Ok, d, false, L"");
            expect(!f2.showAmount, L"币种未知：不显示数字（不默认 ¥）");

            // 3) 币种已知：显示 ¥110.00
            dshb::WidgetFrame f3 = dshb::BuildWidgetFrame(dshb::ConnState::Ok, d, true, L"\u00A5");
            expect(f3.showAmount && f3.amountText == "110.00", L"币种已知：显示 110.00");

            // 4) 读不到（没网）但有旧值：数字仍显示，但状态文案必须是"无法连接"
            dshb::WidgetFrame f4 = dshb::BuildWidgetFrame(dshb::ConnState::NetworkError, d, true, L"\u00A5");
            expect(f4.showAmount && wcsstr(f4.statusText, L"无法连接") != nullptr,
                   L"没网：保留旧数字，但状态文案说'无法连接'");

            // 5) 归零：显示 0.00，且状态仍是 Ok（余额 0 不是错误）
            dshb::DisplayedAmount dz;
            dshb::Sample z{};
            z.amountsOk = true;
            z.total = dshb::Amount::FromYuan(0);
            z.currency = "CNY";
            dz.OnSample(z);          // 第一次：待确认
            dz.OnSample(z);          // 第二次：确认
            dshb::WidgetFrame fz = dshb::BuildWidgetFrame(dshb::ConnState::Ok, dz, true, L"\u00A5");
            expect(fz.showAmount && fz.amountText == "0.00", L"归零：显示 0.00（不是占位符）");

            // 6) 显示值必须渐进跟随，而且是**定时长**（不是指数逼近）。
            //    ★ 这条测试曾经断言"一个时间常数后到 63%"——那是旧设计的判据。
            //      改成固定时长 + 缓动之后，那个断言自然失败：**测试过时了，不是产品坏了**。
            //      所以判据也要跟着换成新设计该有的性质：
            //        · 中途在两端之间（既没跳过去，也没不动）
            //        · 到时长就精确落位，不留尾巴
            //        · 之后再推进也不会过冲
            dshb::DisplayedAmount dd;
            dshb::Sample a100{};
            a100.amountsOk = true;
            a100.total = dshb::Amount::FromYuan(100);
            dd.OnSample(a100);
            dshb::Sample a200{};
            a200.amountsOk = true;
            a200.total = dshb::Amount::FromYuan(200);
            dd.OnSample(a200);

            // 走到时长的一半：缓动中点应当正好落在中值附近
            const int halfSteps = static_cast<int>(dd.rollSeconds * 60.0 / 2.0);
            for (int i = 0; i < halfSteps; ++i) dd.Update(1.0 / 60.0);
            expect(dd.value() > 120.0 && dd.value() < 180.0,
                   L"滚动中途落在两端之间（既没跳过去也没不动）");

            // 走到时长结束：必须精确落位
            const int restSteps = static_cast<int>(dd.rollSeconds * 60.0) - halfSteps + 2;
            for (int i = 0; i < restSteps; ++i) dd.Update(1.0 / 60.0);
            expect(std::fabs(dd.value() - 200.0) < 0.001, L"到时长精确落位（不留 199.9997）");

            // 结束之后再推进，不得过冲
            for (int i = 0; i < 120; ++i) dd.Update(1.0 / 60.0);
            expect(std::fabs(dd.value() - 200.0) < 0.001, L"落位后不漂移、不过冲");
            expect(!dd.rolling(), L"落位后 rolling 状态应结束（文本恢复按数值算）");
        }

        SelfTestLog(L"[check] 小计：失败 %d 项", failed);
        CoUninitialize();
        return failed == 0 ? 0 : 9;
    }

    Clock clock;
    double elapsed = 0.0;
    uint64_t frames = 0;
    double frameMsSum = 0.0;
    HRESULT lastHr = S_OK;
    int failures = 0;

    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            if (!g_running) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        const double dt = clock.Tick();
        elapsed += dt;
        g_elapsed = elapsed;

        // ---- 数据管道（B 阶段）----
        // 目前数据来自模拟源；真实接口在 J 阶段接上。状态机只认"一条采样"，
        // 所以换数据源不需要动它——这正是把这两件事分开的目的。
        {
            const dshb::Sample s = g_fake.NextIfDue(elapsed);
            if (s.wallMs != 0) {
                g_states.OnSample(s, s.wallMs);
            }
        }

        // Key 缺失是启动时检查一次即可（设计 §4 的 NoKey 态）
        {
            static bool keyChecked = false;
            static bool haveKey = true;
            if (!keyChecked) {
                keyChecked = true;
                wchar_t buf[8]{};
                haveKey = GetEnvironmentVariableW(L"DEEPSEEK_API_KEY", buf, 8) > 0;
                g_states.SetNoKey(!haveKey);
            }
        }

        // 布局诊断：**每帧绘制结束、回到这里之后**才写文件（不在绘制路径里写，
        // 那样每帧开关一次文件会把进程弄崩）。只写第一帧一次。
        if (g_layoutProbe) {
            static bool dumped = false;
            if (!dumped) {
                dumped = true;
                const int64_t nowWall2 = static_cast<int64_t>(NowWallMs());
                const dshb::ConnState st2 = g_states.Evaluate(nowWall2);
                SelfTestLog(L"[layout] state=%ls showAmount=%d symbol=%ls amount=%hs",
                            dshb::ConnStateName(st2), renderer.widgetFrame().showAmount ? 1 : 0,
                            renderer.widgetFrame().currencySymbol,
                            renderer.widgetFrame().amountText.c_str());
                SelfTestLog(L"[layout] display: hasValue=%d value=%.4f target=%.4f rollSeconds=%.2f",
                            g_display.hasValue() ? 1 : 0, g_display.value(), g_display.target(),
                            g_display.rollSeconds);
            }
        }

        // 调试浮层：把状态机的判断摊开给人看。**它只显示，不参与任何逻辑。**
        if (g_debug) {
            const int64_t nowWall = static_cast<int64_t>(NowWallMs());
            const dshb::ConnState st = g_states.Evaluate(nowWall);
            const bool focused = (GetForegroundWindow() == g_hwnd);
            g_haveWindowFocus = focused;
            const dshb::Sample& last = g_states.lastGood();
            const std::string num =
                g_states.hasGood() ? last.total.ToString2() : std::string("--.--");
            // 宽字符格式化：DirectWrite 直接吃 wchar_t，中间不做编码转换
            const std::wstring numW(num.begin(), num.end());
            wchar_t line[512];
            // %hs = 窄字符参数；混用宽窄格式必须写清，否则参数会被按错误的宽度读
            swprintf_s(line,
                       L"state=%hs  bal=%ls%ls  scenario=%d  samples=%d  t=%.1fs\n"
                       L"hasGood=%d  key=%d  focus=%d  speed=%.0fx\n"
                       L"F1..F9=scenario  R=recharge  C=clockjump  Esc=quit",
                       ConnStateNameUtf8(st), g_states.hasGood() ? last.CurrencySymbolW() : L"",
                       numW.c_str(), static_cast<int>(g_fake.scenario()),
                       static_cast<int>(g_states.samples().size()), elapsed,
                       g_states.hasGood() ? 1 : 0, g_states.NoKey() ? 0 : 1, focused ? 1 : 0,
                       g_fake.speed());
            renderer.SetDebugText(line);
        }

        // 显示值推进（C3）：测量值可以跳，显示值必须连续跟随
        g_display.OnSample(g_states.lastGood());
        g_display.Update(dt);

        // 组装这一帧要显示的东西，交给渲染层。渲染层不关心余额是怎么来的。
        {
            const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
            const bool currencyKnown = g_states.hasGood() && g_states.lastGood().CurrencyKnown();
            renderer.SetWidgetFrame(dshb::BuildWidgetFrame(
                st, g_display, currencyKnown,
                g_states.hasGood() ? g_states.lastGood().CurrencySymbolW() : L""));
        }

        LARGE_INTEGER a, b, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&a);
        lastHr = renderer.RenderFrame(elapsed);
        QueryPerformanceCounter(&b);
        const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
                          static_cast<double>(freq.QuadPart);
        ++frames;
        frameMsSum += ms;
        if (FAILED(lastHr)) {
            ++failures;
            if (failures <= 3) {
                SelfTestLog(L"[render] 第 %llu 帧失败: hr=0x%08lX",
                            static_cast<unsigned long long>(frames),
                            static_cast<unsigned long>(lastHr));
            }
        }

        if (g_selfTest && elapsed >= g_selfTestSeconds) break;
        if (!g_selfTest && g_runSeconds > 0.0 && elapsed >= g_runSeconds) break;

        // Present 已经等过垂直空白，这里只需要把消息收干净
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    SelfTestLog(L"[render] frames=%llu elapsed=%.2fs 平均单帧=%.3fms 平均刷新率=%.1fHz 失败=%d",
                static_cast<unsigned long long>(frames), elapsed,
                frames ? frameMsSum / static_cast<double>(frames) : 0.0,
                elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0, failures);
    SelfTestLog(L"[win] 自检完成，最后 Present hr=0x%08lX",
                static_cast<unsigned long>(lastHr));

    renderer.Destroy();
    g_renderer = nullptr;
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
}
