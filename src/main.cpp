// deepseek-balance v0.2 —— 主程序：窗口 + 帧循环
//
// 这一步（A3 / A3b / A3c）要达成的形态：
//   无边框、圆角、每像素半透明、置顶、不进任务栏与 Alt+Tab、不抢焦点、DPI 正确。
// 数字、曲线、颜色、心跳都是后面步骤的事。
//
// 自检（给自动化用，不弹窗）：--selftest [--seconds=N]
//   把窗口与 DPI 的事实、帧统计写进 build\selftest.log。
//   不弹对话框：Start-Process -PassThru 的 HasExited 在弹窗时会永远读成 false（踩过）。

#include "renderer.h"

#include <windows.h>
#include <objbase.h>    // CoInitializeEx / COINIT_APARTMENTTHREADED
#include <shellapi.h>   // CommandLineToArgvW

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
wchar_t g_exportPath[MAX_PATH] = L"frame.png";
int g_frameNo = 1;
HWND g_hwnd = nullptr;
bool g_running = true;
dshb::Renderer* g_renderer = nullptr;

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
        }
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
        }
    }
    if (argv) LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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

    // 先按主屏 DPI 开一个窗口，拿到 HWND 之后再用窗口所在显示器的 DPI 算真实画布。
    // （窗口落在哪块屏要等它存在才知道，所以顺序只能是"先建窗、后定尺寸"。）
    const UINT sysDpi = GetDpiForSystem();
    const double sysScale = static_cast<double>(sysDpi) / 96.0;
    const int w0 = static_cast<int>(dshb::kCanvasWidthDip * sysScale + 0.5);
    const int h0 = static_cast<int>(dshb::kCanvasHeightDip * sysScale + 0.5);

    g_hwnd = CreateWindowExW(exStyle, kClassName, L"deepseek-balance", WS_POPUP,
                             240, 240, w0, h0, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        SelfTestLog(L"[main] CreateWindowExW 失败: %lu", GetLastError());
        return 2;
    }

    const UINT dpi = GetDpiForWindow(g_hwnd);
    const int clientW = static_cast<int>(dshb::kEntityWidthDip * (dpi / 96.0) + 0.5);
    const int clientH = static_cast<int>(dshb::kEntityHeightDip * (dpi / 96.0) + 0.5);

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    SelfTestLog(L"[win] dpi(system)=%u dpi(window)=%u exStyle=0x%08lX",
                sysDpi, dpi, static_cast<unsigned long>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE)));
    SelfTestLog(L"[win] 窗口矩形=(%ld,%ld,%ld,%ld) 尺寸=%ldx%ld 实体区(理论)=%dx%d",
                wr.left, wr.top, wr.right, wr.bottom, wr.right - wr.left, wr.bottom - wr.top,
                clientW, clientH);

    dshb::Renderer renderer;
    g_renderer = &renderer;
    const dshb::CanvasSize size = dshb::Renderer::SizeForWindow(g_hwnd);
    if (!renderer.Create(g_hwnd, size)) {
        SelfTestLog(L"[main] 渲染器创建失败（D3D11 / DComp / 交换链）");
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 3;
    }
    SelfTestLog(L"[render] 画布=%dx%d scale=%.4f（外扩 %d DIP 余量）",
                size.widthPx, size.heightPx, size.scale, dshb::kMarginDip);

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    // ---- 离屏导帧模式：渲一帧到 PNG 就退出 ----
    // 用来做"用像素说话"的验收：居中错位、颜色、残影、粒子越界都靠它量。
    // 注意它渲染的是同一份绘制代码，所以屏幕上的错在 PNG 里也会错。
    if (g_exportFrame) {
        const double t = static_cast<double>(g_frameNo) / 60.0;   // 第 N 帧 ≈ N/60 秒
        const bool ok = renderer.ExportFrame(g_exportPath, t);
        SelfTestLog(L"[export] %ls 帧=%d 时刻=%.3fs 结果=%ls 画布=%dx%d",
                    g_exportPath, g_frameNo, t, ok ? L"成功" : L"失败",
                    size.widthPx, size.heightPx);
        renderer.Destroy();
        g_renderer = nullptr;
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return ok ? 0 : 7;
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
