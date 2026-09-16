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
#include "tuning.h"     // 常量：所有可调值只有这一处来源（曾把 10000 硬编码在下面，改常量无效）
#include "balance_source.h"

#include <windows.h>
#include <objbase.h>    // CoInitializeEx / COINIT_APARTMENTTHREADED
#include <shellapi.h>   // CommandLineToArgvW
#include "curve.h"       // --curve-selftest

#include <wtsapi32.h>   // 锁屏/解锁通知（J4）

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

// Defined further down; declared here because WndProc (which runs before them) calls them.
void InstallEscHook();

void RemoveEscHook();
bool g_running = true;
dshb::Renderer* g_renderer = nullptr;
double g_elapsed = 0.0;          // 单调时钟累计秒数；帧循环和 WndProc 共用
bool g_debug = false;            // --debug：显示调试浮层
bool g_selftestB = false;        // --selftest-b：金额解析与状态机的自检
bool g_layoutProbe = false;      // --layout-probe：导出模式下打印布局数值
int g_dpiOverride = 0;           // --dpi=N：覆盖画布缩放（0 = 用窗口真实 DPI）
double g_uiScale = 1.0;          // --ui-scale=N：视觉缩放（1.0 = 按 DPI，设计意图）
double g_rollTo = 0.0;           // --roll-to=N：滚动抓帧的目标金额（0 = 用默认 99.50）
int g_rollFrames = 0;            // --roll=N：是否导出滚动瞬间（N 只用于日志，步数看 g_rollSteps）
int g_rollSteps = 0;             // --roll=N：跳变之后推进多少帧（1/60 秒一步）
bool g_rollLoop = false;         // --roll=loop：每 2 秒来回跳一次，用肉眼反复看滚动
// --fixed-amount=N: 把余额钉在 N，不再取样、不再变化。0 = 关闭。
// 用途：做动画时内容必须先站住不动，否则分不清画面变化是动画造成的还是新采样造成的。
double g_fixedAmount = 0.0;
// --real=N：手动设定**实际数字**（采样值）。--display=M：手动设定**显示数字**并冻结。
// --no-anim：显示数字不做指数平滑（跟着实际数字立刻到位）。
// 三者都是为了"停在一个状态上看清楚"，不做自动动画。
bool g_realGiven = false;
bool g_lastGiven = false;
bool g_fixedGiven = false;
bool g_rollGiven = false;   // 是否真的传了 --roll（默认 g_rollFrames=0 与 --roll=0 无法区分）

// ---- 真接口（J1/J2/J4/J6）----
// 默认在有 Key 时启用；--api=off 关掉（回到假数据源，调试用）。HTTP 在后台线程。
bool g_apiOff = false;          // --api=off
bool g_apiOnce = false;         // --api-once：只取一次（自检用）
bool g_realApiOn = false;       // 真接口是否已启动（启动后就不再喂假数据源）
std::wstring g_apiHost = L"api.deepseek.com";
int g_apiPort = 443;
bool g_apiPlainHttp = false;
int g_apiTimeoutMs = 5000;
int g_apiIntervalMs = static_cast<int>(dshb::kApiIntervalMs);   // 唯一来源：常量（曾在这里硬编码 10000，改常量无效）
dshb::BalanceSource g_apiSource;

// ---- 币种点击（只认单击；拖动与长按都不算）----
int g_pressX = 0, g_pressY = 0;
unsigned long long g_pressTick = 0;
bool g_pressValid = false;
bool g_currenciesGiven = false;   // --currencies=：合成一条多币种样本（验证切换用）
std::string g_currenciesSpec;   // 形如 "CNY:19.20,USD:2.70"
bool g_realApiPlanned = false;  // 进循环之前就定下"本次要不要用真接口"

bool g_countdownGiven = false;  // --countdown=N：导帧时给倒计时一个固定值（导帧不取样）
int g_countdownSeconds = 0;
std::string g_apiKey;           // 只在内存里，绝不写日志
bool g_clickTest = false;         // --click-test：注入三次手势
bool g_pauseTest = false;
bool g_noCurve = false;           // --no-curve：关掉氛围曲线（A/B 对比用）
int  g_historyDemo = 0;           // --history-demo=N：合成 N 个曲线点（导帧验证用）
int  g_curveFrame = -1;           // --curve-frame=k：把滚动计时器冻在第 k 帧（-1 = 未给）
         // --pause-test：注入"锁屏/解锁"，验证 J4（不用真锁屏）
double g_realAmount = -1.0;   // --real=R（-1 = 未给；0 是合法金额！）
double g_displayAmount = 0.0;   // 已弃用（所有者改为 --last）
double g_lastAmount = -1.0;   // --last=L（-1 = 未给）
int g_frames = -1;              // --frames=k：已经运算了多少帧（-1 = 未给）
// --seq=v0,v1,v2 ...：每 --step 秒把实际数字换成下一个（L 自动取上一次的实际数字）。
std::vector<double> g_seq;
double g_seqStep = 3.0;
int g_seqIdx = -1;
bool g_noAnim = false;
// --crisp：坐标取 S/n 的整数部分（静止读数清晰）。默认用连续坐标（有"两格之间"的中间态）。
bool g_crisp = false;
// --phase=P：强行指定行程进度 0..1（>=0 生效），用来停在任意一刻看轮子位置。
double g_phaseOverride = -1.0;
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

// 左键"抬起"时的判定。**只认单击**：
//   移动超过 4px  -> 算拖动，不切换
//   按住超过 600ms -> 算长按，不切换
// 命中测试用渲染层每帧发布的符号矩形（像素坐标）。

void FinishLeftGesture(int x, int y) {
    if (!g_pressValid) return;
    g_pressValid = false;
    const int dx = x - g_pressX;
    const int dy = y - g_pressY;
    const int dist2 = dx * dx + dy * dy;
    const int held = static_cast<int>(GetTickCount64() - g_pressTick);
    if (dist2 > 16) {
        SelfTestLog(L"[click] 移动 %dpx：算拖动，不切换", static_cast<int>(std::sqrt(static_cast<double>(dist2))));
        return;
    }
    if (held > 600) {
        SelfTestLog(L"[click] 按住 %dms：算长按，不切换", held);
        return;
    }
    const dshb::SymbolRect sr = dshb::CurrencySymbolRect();
    if (!sr.valid) return;
    const float pad = 6.0f;
    const bool inside = (x >= sr.l - pad && x <= sr.r + pad && y >= sr.t - pad && y <= sr.b + pad);
    SelfTestLog(L"[click] 点(%d,%d) 符号矩形[%d,%d..%d,%d] 命中=%ls", x, y, static_cast<int>(sr.l),
                static_cast<int>(sr.t), static_cast<int>(sr.r), static_cast<int>(sr.b),
                inside ? L"是" : L"否");
    if (!inside) return;
    const std::string next = g_display.NextCurrency();
    if (next.empty()) {
        SelfTestLog(L"[click] 当前只有一个币种：忽略");
        return;
    }
    g_display.SelectCurrency(next);
    const double switched = g_display.lastSwitchTarget();
    const wchar_t* symNow = (g_display.shownCurrency() == "CNY") ? L"¥"
                          : ((g_display.shownCurrency() == "USD") ? L"$" : L"(无)");
    if (switched >= 0.0) {
        SelfTestLog(L"[click] 切换 -> %hs：符号=%ls 数字=%.2f", next.c_str(), symNow, switched);
    } else {
        SelfTestLog(L"[click] 切换 -> %hs：符号=%ls 数字=--.--（该币种本次没有数据）",
                    next.c_str(), symNow);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // ---- 暂停与唤醒（J4）----
    // 暂停期间：不发请求，且**显示当作没有值**（所有者：暂停期间当成值无值）。
    // 唤醒：立刻补一次。
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            if (g_apiSource.started()) g_apiSource.Pause();
            g_display.MarkUnreadable();
            SelfTestLog(L"[pause] 系统休眠：暂停轮询，显示当作无值");
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            if (g_apiSource.started()) g_apiSource.ResumeNow();
            SelfTestLog(L"[pause] 系统唤醒：立刻补一次取样");
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) {
            if (g_apiSource.started()) g_apiSource.Pause();
            g_display.MarkUnreadable();
            SelfTestLog(L"[pause] 锁屏：暂停轮询，显示当作无值");
        } else if (wp == WTS_SESSION_UNLOCK) {
            if (g_apiSource.started()) g_apiSource.ResumeNow();
            SelfTestLog(L"[pause] 解锁：立刻补一次取样");
        }
        return 0;
    case WM_LBUTTONDOWN:
        g_pressX = static_cast<int>(static_cast<short>(LOWORD(lp)));
        g_pressY = static_cast<int>(static_cast<short>(HIWORD(lp)));
        g_pressTick = GetTickCount64();
        g_pressValid = true;
        return 0;
    case WM_LBUTTONUP:
        FinishLeftGesture(static_cast<int>(static_cast<short>(LOWORD(lp))),
                          static_cast<int>(static_cast<short>(HIWORD(lp))));
        return 0;
    case WM_DESTROY:
        RemoveEscHook();
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
        // 记一行：双击第二次时的可见反馈因此可验证（以前类名不匹配，静默失败）
        SelfTestLog(L"[single] 收到重复启动通知：闪一次描边");
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

// ---------------------------------------------------------------------------
// Esc closes the widget.
//
// WHY a keyboard hook instead of WM_KEYDOWN: this window is deliberately
// non-activating (it must never steal focus from what the user is doing), so it
// never receives keyboard messages -- the WM_KEYDOWN branch in WndProc below is
// unreachable in practice. A low-level hook sees the key before any window does
// and needs no focus at all. It is installed only while the widget is on screen,
// never in the offscreen export / self-test runs.
//
// The hook does NOT swallow the key: it returns CallNextHookEx unconditionally, so
// Esc still reaches whatever the user was actually typing into.
// ---------------------------------------------------------------------------
HHOOK g_escHook = nullptr;

LRESULT CALLBACK EscHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        // Any Escape closes it, injected or not. NOTE: do NOT filter on the LLKHF_INJECTED
        // bit here -- measured, that bit is set on ordinary hardware key presses too (a
        // keyboard hook receiving events destined for another process sees injected=1),
        // so filtering on it silently disables the feature. Esc is harmless to accept.
        if (kb && kb->vkCode == VK_ESCAPE) {
            SelfTestLog(L"[key] Esc -> closing the widget");
            g_running = false;
            if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(g_escHook, code, wp, lp);
}

void InstallEscHook() {
    if (!g_escHook) g_escHook = SetWindowsHookExW(WH_KEYBOARD_LL, EscHookProc, nullptr, 0);
}

void RemoveEscHook() {
    if (g_escHook) { UnhookWindowsHookEx(g_escHook); g_escHook = nullptr; }
}

}  // namespace

// 从 DSH 的凭据文件里取 Key：~/.dsh/.credentials.yaml 的 refs.DEEPSEEK_API_KEY。
//
// ★ 为什么需要它：这台机器上 DEEPSEEK_API_KEY **不在环境变量里**（进程里没有、
//   用户级/机器级也没设），DSH 把模型凭据存在自己的全局文件里，旧版工具就是从那里拿的。
//   只做最小解析：找 refs 段里那一行，因此不引入 YAML 依赖。
//   值绝不打印、绝不写日志（设计 §10.5 / J6），只报用了哪个来源。
static bool ReadKeyFromDshCredentials(std::wstring* out) {
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return false;
    const std::wstring path = std::wstring(profile) + L"\\.dsh\\.credentials.yaml";

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
    std::string blob;
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), fp)) > 0) blob.append(buf, got);
    std::fclose(fp);

    // 逐行扫：只认 "  DEEPSEEK_API_KEY: <值>" 这一行
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eol = blob.find('\n', pos);
        if (eol == std::string::npos) eol = blob.size();
        std::string line = blob.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.compare(i, 16, "DEEPSEEK_API_KEY") != 0) continue;
        size_t c = line.find(':', i);
        if (c == std::string::npos) continue;
        std::string v = line.substr(c + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
            v = v.substr(1, v.size() - 2);
        }
        // 只接受看起来像 Key 的值（避免把占位符当成 Key）
        if (v.size() < 20 || v.compare(0, 3, "sk-") != 0) continue;
        out->assign(v.begin(), v.end());   // 值本身是 ASCII
        return true;
    }
    return false;
}

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
        } else if (wcscmp(argv[i], L"--click-demo") == 0) {   // 已移除（所有者不要演示窗）
            SelfTestLog(L"[argv] --click-demo 已移除（所有者：不要演示窗口），忽略");
        } else if (wcsncmp(argv[i], L"--countdown=", 12) == 0) {
            // 导帧夹具：导出路径不取样，所以倒计时没有真实来源，靠它给一个值。
            g_countdownGiven = true;
            g_countdownSeconds = _wtoi(argv[i] + 12);
        } else if (wcsncmp(argv[i], L"--curve-frame=", 14) == 0) {
            // 导帧夹具（规格 §5 验收 4）：把曲线滚动计时器冻在 k/60 秒，于是
            // "滚动中的第 k 帧"可以用 --export-frame=1 直接导出来（不需要连画 k 帧）。
            g_curveFrame = _wtoi(argv[i] + 14);
        } else if (wcsncmp(argv[i], L"--history-demo=", 15) == 0) {
            g_historyDemo = _wtoi(argv[i] + 15);
        } else if (wcscmp(argv[i], L"--curve-selftest") == 0) {
            // Numeric self-test for the monotone interpolation (D3/D6).
            // No pixels involved: exactness + no-overshoot are pure properties.
            std::string rep;
            const bool ok = dshb::SelfTestMonotoneCurve(&rep);
            SelfTestLog(L"[curve] --- monotone interpolation self-test ---");
            size_t pos = 0;
            while (pos < rep.size()) {
                size_t eol = rep.find('\n', pos);
                if (eol == std::string::npos) eol = rep.size();
                SelfTestLog(L"[curve] %hs", rep.substr(pos, eol - pos).c_str());
                pos = eol + 1;
            }
            SelfTestLog(ok ? L"[curve] RESULT: all properties hold" : L"[curve] RESULT: FAILED");
            g_runSeconds = 0.2;   // 跑完就退
        } else if (wcscmp(argv[i], L"--no-curve") == 0) {
            g_noCurve = true;
        } else if (wcscmp(argv[i], L"--pause-test") == 0) {
            g_pauseTest = true;
        } else if (wcscmp(argv[i], L"--click-test") == 0) {
            g_clickTest = true;
        } else if (wcsncmp(argv[i], L"--currencies=", 13) == 0) {
            // 合成一条带**全部币种条目**的样本：形如 CNY:19.20,USD:2.70。
            // 用途：真实账户只有 CNY，切换功能没有真数据可测，所以给一个测试夹具。
            g_currenciesGiven = true;
            const wchar_t* v = argv[i] + 13;
            char narrow[512]{};
            WideCharToMultiByte(CP_UTF8, 0, v, -1, narrow, sizeof(narrow), nullptr, nullptr);
            std::string spec(narrow);
            std::string cur;
            size_t pos = 0;
            while (pos <= spec.size()) {
                const size_t comma = spec.find(',', pos);
                const std::string item = spec.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
                const size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    cur += item.substr(0, colon) + "=" + item.substr(colon + 1) + " ";
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            g_currenciesSpec = cur;
        } else if (wcscmp(argv[i], L"--api=off") == 0) {
            g_apiOff = true;
        } else if (wcscmp(argv[i], L"--api-once") == 0) {
            g_apiOnce = true;
        } else if (wcsncmp(argv[i], L"--api-host=", 11) == 0) {
            g_apiHost = argv[i] + 11;
        } else if (wcsncmp(argv[i], L"--api-port=", 11) == 0) {
            g_apiPort = _wtoi(argv[i] + 11);
        } else if (wcscmp(argv[i], L"--api-plain-http") == 0) {
            g_apiPlainHttp = true;
        } else if (wcsncmp(argv[i], L"--api-timeout-ms=", 17) == 0) {
            g_apiTimeoutMs = _wtoi(argv[i] + 17);
        } else if (wcsncmp(argv[i], L"--api-interval-ms=", 18) == 0) {
            g_apiIntervalMs = _wtoi(argv[i] + 18);
        } else if (wcsncmp(argv[i], L"--scenario=", 11) == 0) {
            // Accepts a 1-based number (as printed by ScenarioName) or an ASCII alias.
            // A name that is not recognised USED to be swallowed by _wtoi and silently
            // select scenario 0 - so "--scenario=NoNetwork" quietly showed scenario 1.
            // An unknown value is now reported instead of guessed at.
            const wchar_t* v = argv[i] + 11;
            static const wchar_t* kAliases[] = {L"steady", L"fast", L"low", L"recharge", L"zero",
                                               L"unavailable", L"nonetwork", L"stale", L"clockjump"};
            int idx = -1;
            const int num = _wtoi(v);
            if (num >= 1 && num <= static_cast<int>(dshb::Scenario::Count)) {
                idx = num - 1;                     // 1-based, matching the printed names
            } else {
                for (int a = 0; a < static_cast<int>(dshb::Scenario::Count); ++a) {
                    if (_wcsicmp(v, kAliases[a]) == 0) { idx = a; break; }
                }
            }
            if (idx >= 0) {
                g_fake.Select(static_cast<dshb::Scenario>(idx));
            } else {
                SelfTestLog(L"[argv] --scenario=%ls not recognised (use 1..%d or a name); ignored",
                            v, static_cast<int>(dshb::Scenario::Count));
            }
        } else if (wcsncmp(argv[i], L"--speed=", 8) == 0) {
            g_fake.SetSpeed(_wtof(argv[i] + 8));
        } else if (wcsncmp(argv[i], L"--dpi=", 6) == 0) {
            g_dpiOverride = _wtoi(argv[i] + 6);
        } else if (wcsncmp(argv[i], L"--ui-scale=", 11) == 0) {
            g_uiScale = _wtof(argv[i] + 11);
        } else if (wcsncmp(argv[i], L"--roll-to=", 10) == 0) {
            g_rollTo = _wtof(argv[i] + 10);       // 滚动抓帧的目标金额

        } else if (wcsncmp(argv[i], L"--real=", 7) == 0) {
            g_realAmount = _wtof(argv[i] + 7);
            g_realGiven = true;
        } else if (wcsncmp(argv[i], L"--seq=", 6) == 0) {
            const wchar_t* csv = argv[i] + 6;
            double v = 0.0;
            while (swscanf_s(csv, L"%lf", &v) == 1) {
                g_seq.push_back(v);
                const wchar_t* comma = wcschr(csv, L',');
                if (!comma) break;
                csv = comma + 1;
            }
        } else if (wcsncmp(argv[i], L"--step=", 7) == 0) {
            g_seqStep = _wtof(argv[i] + 7);
        } else if (wcsncmp(argv[i], L"--last=", 7) == 0) {
            g_lastAmount = _wtof(argv[i] + 7);
            g_lastGiven = true;
        } else if (wcsncmp(argv[i], L"--frames=", 9) == 0) {
            g_frames = _wtoi(argv[i] + 9);
        } else if (wcsncmp(argv[i], L"--display=", 10) == 0) {
            g_displayAmount = _wtof(argv[i] + 10);
        } else if (wcscmp(argv[i], L"--no-anim") == 0) {
            g_noAnim = true;
        } else if (wcsncmp(argv[i], L"--phase=", 8) == 0) {
            g_phaseOverride = _wtof(argv[i] + 8);   // 停在行程的哪一刻（0..1）
        } else if (wcscmp(argv[i], L"--crisp") == 0) {
            g_crisp = true;
        } else if (wcsncmp(argv[i], L"--digit-draw=", 13) == 0) {
            // 0/static = 整串一次画完；1/axis = 逐位按坐标画（修正公式）；2/user = 所有者原式
            const wchar_t* v = argv[i] + 13;
            if (wcscmp(v, L"axis") == 0) dshb::g_digitDrawMode = 1;
            else if (wcscmp(v, L"user") == 0) dshb::g_digitDrawMode = 2;
            else if (wcscmp(v, L"rest") == 0) dshb::g_digitDrawMode = 3;
            else dshb::g_digitDrawMode = _wtoi(v);
        } else if (wcsncmp(argv[i], L"--fixed-amount=", 15) == 0) {
            g_fixedAmount = _wtof(argv[i] + 15);
            g_fixedGiven = true;
        } else if (wcsncmp(argv[i], L"--roll=", 7) == 0) {
            g_rollFrames = 1;                     // 只要出现这个参数就进入滚动抓帧模式
            g_rollGiven = true;
            if (wcscmp(argv[i] + 7, L"loop") == 0) {
                g_rollLoop = true;                // 循环跳变，供肉眼观察
            } else {
                g_rollSteps = _wtoi(argv[i] + 7); // N = 跳变之后推进多少帧（0 = 跳变前）
            }
        }
    }
    if (argv) LocalFree(argv);

    // --fixed-amount：在这里、任何数据源之前喂一次就够。
    // 状态机与显示值都只认 SAMPLE，所以只钉显示值会让状态机空着（屏幕上变成 --.--），
    // 只钉状态机又会被后面的滚动设置覆盖（实测落在 99.80）。一个喂入口 + 三处跳过。
    if (g_fixedGiven) {
        dshb::Sample fs{};
        fs.wallMs = NowWallMs();
        fs.monotonicMs = static_cast<int64_t>(GetTickCount64());
        fs.transportOk = true;
        fs.httpStatus = 200;
        fs.isAvailable = true;
        fs.amountsOk = true;
        fs.currency = "CNY";
        fs.total = dshb::Amount::FromYuanDouble(g_fixedAmount);
        g_states.OnSample(fs, fs.wallMs);
        g_display.OnSample(g_states.lastGood());
        SelfTestLog(L"[pin] 余额钉在 %.2f（不再取样、不再变化）", g_fixedAmount);
    }

    // --currencies=：合成一条带**全部币种条目**的样本。
    // 用途：真实账户只有 CNY，切换币种没有真数据可测，所以给一个夹具，
    // 走的就是真实样本那条路（entries + 优先条目），不是特例分支。
    if (g_currenciesGiven) {
        dshb::Sample cs{};
        cs.wallMs = NowWallMs();
        cs.monotonicMs = static_cast<int64_t>(GetTickCount64());
        cs.transportOk = true;
        cs.httpStatus = 200;
        cs.isAvailable = true;
        cs.currency = "CNY";
        cs.amountsOk = false;
        std::string spec = g_currenciesSpec;
        size_t pos = 0;
        while (pos < spec.size()) {
            const size_t sp = spec.find(' ', pos);
            const std::string item = spec.substr(pos, (sp == std::string::npos) ? std::string::npos : sp - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                dshb::CurrencyAmount ca{};
                ca.currency = item.substr(0, eq);
                if (dshb::ParseAmount(item.substr(eq + 1), &ca.total)) ca.ok = true;
                if (ca.currency == "CNY" && ca.ok) {
                    cs.total = ca.total;
                    cs.amountsOk = true;
                }
                cs.entries.push_back(ca);
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        g_states.OnSample(cs, cs.wallMs);
        if (cs.amountsOk) g_display.OnSample(g_states.lastGood());
        SelfTestLog(L"[cur] 合成样本：%hs 条", std::to_string(cs.entries.size()).c_str());
    }

    // 手动设定三件参数（所有者定的接口）：实际数字 R、上次的实际数字 L、运算了 k 帧。
    //   --real=R    实际数字
    //   --last=L    上次的实际数字（不给则等于 R，于是 D=0、轮子不动）
    //   --frames=k  已经运算了多少帧（不给则 0，即停在行程起点）
    // 位置完全由这三个量决定：coord_n = floor(L/n) + D_n × (1 − rate^k)
    if (g_realGiven || g_lastGiven || g_frames >= 0) {
        const double R = (g_realGiven) ? g_realAmount
                                               : ((g_lastGiven) ? g_lastAmount : 0.0);
        const double L = (g_lastGiven) ? g_lastAmount : R;
        const int k = (g_frames >= 0) ? g_frames : 0;
        dshb::Sample ms{};
        ms.wallMs = NowWallMs();
        ms.monotonicMs = static_cast<int64_t>(GetTickCount64());
        ms.transportOk = true;
        ms.httpStatus = 200;
        ms.isAvailable = true;
        ms.amountsOk = true;
        ms.currency = "CNY";
        ms.total = dshb::Amount::FromYuanDouble(R);
        g_states.OnSample(ms, ms.wallMs);
        g_display.OnSample(g_states.lastGood());
        g_display.SetManual(L, R, k);
        SelfTestLog(L"[manual] R(实际)=%.4f L(上次)=%.4f k(帧)=%d", R, L, k);
    }

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

    // 注册锁屏/解锁通知（J4）：不注册就收不到 WM_WTSSESSION_CHANGE
    if (g_hwnd) WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);

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
        // 曲线记录文件：给了路径就顺手加载（重启后曲线接上，规格验收 8）
        {
            std::wstring curvePath = paths.dataDir;
            if (!curvePath.empty() && curvePath.back() != L'\\') curvePath += L'\\';
            curvePath += L"curve.json";
            dshb::SetCurveStorePath(curvePath);
            SelfTestLog(L"[paths] 曲线=%ls", curvePath.c_str());
        }
        if (!paths.writable) {
            SelfTestLog(L"[paths] 降级：目录不可写（%ls），采样只留在内存，重启后没有历史",
                        paths.unwritableReason.c_str());
        }
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    // Esc closes the widget. Installed here (the widget is on screen from now on) and
    // removed in WM_DESTROY. See EscHookProc for why a low-level hook is needed.
    InstallEscHook();

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

    // ---- 真接口"要不要用"在**进入循环之前**定下来（J1/J2）----
    //
    // ★ 为什么必须提前：假数据源在主循环里**先**喂一帧，真接口要到同一轮稍后才启动，
    //   于是开头会闪出假值；如果第一次真请求慢或失败，那个假值就一直挂着
    //   （实测：所有者打开后卡在 99.80 不动，正是这条顺序造成的）。
    //   先决定、后喂数据，就不可能出现假值——打开就是 --.--，真值到了才出现数字。
    {
        wchar_t keyBuf[512]{};
        bool haveKey = GetEnvironmentVariableW(L"DEEPSEEK_API_KEY", keyBuf, 512) > 0;
        if (haveKey) {
            SelfTestLog(L"[api] Key 取自环境变量 DEEPSEEK_API_KEY");
        } else {
            std::wstring fromStore;
            if (ReadKeyFromDshCredentials(&fromStore)) {
                wcsncpy_s(keyBuf, fromStore.c_str(), _TRUNCATE);
                haveKey = true;
                SelfTestLog(L"[api] Key 取自 DSH 凭据文件 refs.DEEPSEEK_API_KEY（不打印内容）");
            }
        }
        g_states.SetNoKey(!haveKey);

        const bool manualRun = g_currenciesGiven || g_fixedGiven || g_realGiven || g_lastGiven ||
                               g_frames >= 0 || !g_seq.empty();
        g_realApiPlanned = haveKey && !g_apiOff && !manualRun;
        if (g_realApiPlanned) {
            char narrow[1024]{};
            const int nc = WideCharToMultiByte(CP_UTF8, 0, keyBuf, -1, narrow, sizeof(narrow),
                                               nullptr, nullptr);
            if (nc > 0) g_apiKey.assign(narrow);
        } else if (!haveKey) {
            SelfTestLog(L"[api] 未找到 Key（环境变量与 DSH 凭据都没有）：走 NoKey 态，不发请求");
        } else {
            SelfTestLog(L"[api] 本次不用真接口（--api=off 或手动/演示参数）");
        }
    }

    if (g_countdownGiven) {
        dshb::SetCountdownText(std::to_wstring(g_countdownSeconds).c_str());
    }

    // ---- 离屏导帧模式：渲一帧到 PNG 就退出 ----
    // 用来做"用像素说话"的验收：居中错位、颜色、残影、粒子越界都靠它量。
    // 注意它渲染的是同一份绘制代码，所以屏幕上的错在 PNG 里也会错。
    if (g_exportFrame) {
        const double t = static_cast<double>(g_frameNo) / 60.0;   // 第 N 帧 ≈ N/60 秒

        // 布局诊断只在这里开：它会在绘制路径里写文件，而每帧写文件会把进程弄崩
        // （实测 0xC0000409）。导出模式只画一帧，所以安全。
        if (g_layoutProbe) dshb::SetLayoutProbe(true);
        // 氛围曲线默认开；--no-curve 关掉它，用于确认"关掉后文字位置逐像素不变"
        dshb::SetCurveEnabled(!g_noCurve);
        if (g_historyDemo > 0) dshb::PrimeHistoryForDemo(g_historyDemo);

        // 让模拟数据源在"虚拟时间"里跑起来：否则导出的图没有数据，浮层也是空的。
        // 虚拟时间按 1/60 秒一步推进，所以导出是确定的、可重复的。
        //
        // --roll=N：把"余额跳变之后第 N/60 秒"这一瞬间单独抓出来。
        // 用途是**看滚动动画**——静态单帧看不出数字是怎么滚过去的，
        // 连拍若干张不同 N 的图才能看出过程（动画也是要人眼判的东西）。
        // 只在真的传了 --roll 时预热：否则连 --scenario 导出都会被预热成 19.90，
        // 于是"没有值 -> --.--"这条根本没法用像素验证（踩过）。
        if (g_rollGiven && !g_fixedGiven && !g_realGiven && !g_lastGiven && g_frames < 0) {
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
    // 币种跟着**显示层当前选中的那个**走（点符号可切换），不再只看状态机里那条
    const std::string shownCode = g_display.shownCurrency();
    const bool currencyKnown = g_states.hasGood() && (shownCode == "CNY" || shownCode == "USD");
    const wchar_t* shownSym = (shownCode == "CNY") ? L"\u00A5" : ((shownCode == "USD") ? L"$" : L"");
    renderer.SetWidgetFrame(dshb::BuildWidgetFrame(st, g_display, currencyKnown, shownSym));
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

            // 现在把跳变塞进去，再从这一刻开始逐帧推进。
            // 目标金额可用 --roll-to= 指定：默认 99.50（大跳变，看飞转），
            // 指定成 20.01 之类就能看**微小变化**——那时应当只有最右一位挪 1/10 格，
            // 其余几位纹丝不动。这是里程表与"整块换掉"最容易分辨的地方。
            // 起点 19.90 与 20.01 都是 5 个字符，格位对得上。
            const double rollTarget = (g_rollTo > 0.0) ? g_rollTo : 99.5;
            g_fake.Select(dshb::Scenario::Recharge);
            g_fake.TriggerRecharge(rollTarget);
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
                // 打印"还剩多少比例没走完"：连续函数应当每帧等比缩小，
                // 而且与帧率无关（这里固定 1/60 秒一步）。
                const double tgt = g_display.target();
                const double span = 19.90 - tgt;
                const double rem = (span == 0.0) ? 0.0 : (g_display.value() - tgt) / span;
                SelfTestLog(L"[rollstep] i=%d value=%.4f target=%.4f rem=%.4f text=%hs",
                            i, g_display.value(), tgt, rem, fd.amountText.c_str());
            }

            SelfTestLog(L"[roll] 跳变前=%.2f 跳变后目标=%.2f 推进 %d 帧后显示=%.2f",
                        before, g_display.target(), g_rollSteps, g_display.value());
        } else {
            for (double vt = 0.0; !g_fixedGiven && !g_realGiven && !g_lastGiven && g_frames < 0 && vt <= t + 0.0001; vt += (1.0 / 60.0)) {
                const dshb::Sample s = g_fake.NextIfDue(vt);
                if (s.wallMs != 0) {
                    g_states.OnSample(s, s.wallMs);
                    // 显示值也在虚拟时间里推进，否则导出图上数字还停在 0 或没落位
                    g_display.OnSample(g_states.lastGood());
                }
                g_display.Update(1.0 / 60.0);
            }
        }

        // 渲染前先让显示层刷一次每位坐标（dt=0：追赶系数为 0，坐标直接落在整数目标上）。
        // 不刷的话 places 是空的，渲染层会退回整串绘制——静止画面看起来一样，
        // 但滚动时就没有逐位坐标可用了。实测：漏掉这一步时 99.50 的坐标是空的。
        //
        // ★ --curve-frame=k：必须在**所有推进过滚动计时器的循环之后**再冻结。
        //   那些虚拟时间循环（下面的 for / --roll 分支）每步都会 Update(1/60)，
        //   先冻结就会被它们推着走，导出来的就不是第 k 帧了。
        if (g_curveFrame >= 0) dshb::SetCurveScrollFrame(g_curveFrame);
        g_display.Update(0.0);
        // 组装正文（和真实运行时同一条路径），这样导出的图就是屏幕上会看到的图
        {
            const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
            const bool currencyKnown = g_states.hasGood() && (g_display.shownCurrency() == "CNY" || g_display.shownCurrency() == "USD");
            renderer.SetWidgetFrame(dshb::BuildWidgetFrame(
                st, g_display, currencyKnown,
                ((g_display.shownCurrency() == "CNY") ? L"\u00A5"
                 : ((g_display.shownCurrency() == "USD") ? L"$" : L""))));
        }

        if (g_debug) {
            // --debug：每 6 帧记一次"动画中的显示值"与"目标值"。
            // 两者不同 -> 正在滚动；一次相同 -> 已经落定。用来验证"不是突变"。
            static int dbgTick = 0;
            if (++dbgTick % 6 == 0) {
                SelfTestLog(L"[dbg] 显示值=%.4f 目标值=%.4f", g_display.value(), g_display.target());
            }
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

        // 曲线状态的诊断（存储里有几个点、计时器停在哪一帧）：导帧量像素时，
        // "这一帧到底是滚动中的第几帧、环里是哪几个点"必须能从日志里对上。
        SelfTestLog(L"[curve] %hs", dshb::CurveStateLine().c_str());

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

            // 6) 显示值必须渐进跟随，而且运动由**速率（τ）**决定，不由时长决定。
            //    ★ 这条测试的判据换过两次，每次都是因为设计变了、断言没跟上：
            //      指数逼近时代断言"一个 τ 后到 63%"，还在用；
            //      中间一度改成"固定时长 + 缓动"，那时断言"中途在中值附近"——
            //      **测试过时不是产品坏了**，但也不能放着不管。
            //    ★ 判据换成"速率 + 连续函数"该有的性质（所有者的纠正）：
            //      · 运动由 τ 决定：一个 τ 之后应走掉约 63%
            //      · 差到看不见就截断（吸附），最后一位干脆落定
            //      · 落位后不漂移、不过冲
            dshb::DisplayedAmount dd;
            dshb::Sample a100{};
            a100.amountsOk = true;
            a100.total = dshb::Amount::FromYuan(100);
            dd.OnSample(a100);
            dshb::Sample a200{};
            a200.amountsOk = true;
            a200.total = dshb::Amount::FromYuan(200);
            dd.OnSample(a200);   // 从 100 追向 200

            // 推进正好一个 τ（用细小步长逼近连续时间），应走掉约 63% 的差距
            {
                const double step = dd.tau / 200.0;
                for (int i = 0; i < 200; ++i) dd.Update(step);
                const double walked = (dd.value() - 100.0) / 100.0;
                if (walked < 0.6 || walked > 0.66) {
                    SelfTestLog(L"[check]   一个 τ 后走了 %.3f（期望约 0.63）", walked);
                }
                expect(walked > 0.6 && walked < 0.66, L"一个时间常数后走掉约 63%（速率决定运动）");
            }

            // 继续推进：应当被截断吸附到精确目标
            for (int i = 0; i < 600; ++i) dd.Update(1.0 / 60.0);
            expect(dd.value() == 200.0, L"截断后精确落位（不是 199.9997）");
            expect(!dd.rolling(), L"落位后 rolling 结束");

            // 落位后继续推进：不得漂移、不得过冲
            for (int i = 0; i < 120; ++i) dd.Update(1.0 / 60.0);
            expect(dd.value() == 200.0, L"落位后不漂移、不过冲");

            // 中途来新样本：从当前位置接着追，不跳变。
            // ★ 判据要写成**不变量**，而且用例的数值差距要明显大于截断阈值——
            //   第一版用 100->200 走一帧再改 150，差距落在 snapYuan 附近，
            //   吸附与否全看浮点尾数，断言于是变成掷骰子。测试本身要选
            //   "答案明确"的用例，不该去考边界。
            {
                dshb::DisplayedAmount de;
                dshb::Sample s0{};
                s0.amountsOk = true;
                s0.total = dshb::Amount::FromYuan(100);
                de.OnSample(s0);
                dshb::Sample s300{};
                s300.amountsOk = true;
                s300.total = dshb::Amount::FromYuan(300);
                de.OnSample(s300);                 // 目标 300，差距 200，远大于阈值
                de.Update(1.0 / 60.0);
                const double mid = de.value();
                expect(mid > 100.0 && mid < 200.0, L"追赶途中：位置严格在两端之间");

                dshb::Sample s150{};
                s150.amountsOk = true;
                s150.total = dshb::Amount::FromYuan(150);
                de.OnSample(s150);                 // 目标改成更低的 150
                expect(de.value() == mid, L"新样本不改动当前显示值（只有目标是突变的）");

                de.Update(1.0 / 60.0);              // 一帧最多走 5.4% 的差距
                const double after = de.value();
                // 总是打印，别让它藏在条件里——上一版就是这么白查一轮的
                SelfTestLog(L"[check]   追赶中途: mid=%.4f after=%.4f target=150", mid, after);

                // ★ 断言"朝新目标走、且不越过去"，方向由目标决定，不由我想当然决定。
                //   上一版这里写的是"after < mid"（假设它该下降），可当前值 110.8
                //   低于新目标 150，它本来就该上升——**测试自己写反了方向**。
                const double moved = after - mid;
                const double toGo = 150.0 - mid;
                expect(moved != 0.0 && ((moved > 0) == (toGo > 0)) && std::fabs(moved) < std::fabs(toGo),
                       L"接着朝新目标走：方向对、一帧不跳过去");
            }
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

        // --seq：每 g_seqStep 秒把实际数字换成序列里的下一个。
        // 换值时喂一条样本 -> 显示层的 OnSample 会把 L 记成"上一次的实际数字"，
        // 于是三件参数（R / L / k）自动齐备，正是所有者要的动画实验。
        if (!g_seq.empty()) {
            const int want = static_cast<int>(elapsed / g_seqStep);
            const int idx = (want < static_cast<int>(g_seq.size())) ? want
                                                                    : static_cast<int>(g_seq.size()) - 1;
            if (idx != g_seqIdx) {
                g_seqIdx = idx;
                dshb::Sample sq{};
                sq.wallMs = NowWallMs();
                sq.monotonicMs = static_cast<int64_t>(GetTickCount64());
                sq.transportOk = true;
                sq.httpStatus = 200;
                sq.isAvailable = true;
                sq.amountsOk = true;
                sq.currency = "CNY";
                sq.total = dshb::Amount::FromYuanDouble(g_seq[idx]);
                g_states.OnSample(sq, sq.wallMs);
                g_display.OnSample(g_states.lastGood());
                // ★ 余额为 0 需要"连续两次"才确认（防瞬时 0 把界面闪成灰色）。
                //   序列里只喂一次 0，会被这条规则挡掉（实测：real=0 完全不生效）。
                //   这里补喂一次让确认成立。
                if (g_seq[idx] == 0.0) g_display.OnSample(g_states.lastGood());
                SelfTestLog(L"[seq] t=%.2fs 第 %d 个值 -> real=%.2f", elapsed, idx, g_seq[idx]);
            }
        }
        elapsed += dt;
        g_elapsed = elapsed;

        // ---- 数据管道（B 阶段）----
        // 目前数据来自模拟源；真实接口在 J 阶段接上。状态机只认"一条采样"，
        // 所以换数据源不需要动它——这正是把这两件事分开的目的。
        {
            const dshb::Sample s = g_fake.NextIfDue(elapsed);
            if (s.wallMs != 0 && !g_realApiPlanned && !g_currenciesGiven && !g_fixedGiven && !g_realGiven && !g_lastGiven && g_frames < 0 && g_seq.empty()) {   // 钉值/真接口/合成样本时不喂
                g_states.OnSample(s, s.wallMs);
                // ★ 显示层**只在新样本到达时**喂（见下面删掉的那行每帧喂入）。
                //   每帧重复喂同一条样本，会让 L 恒等于 R（D=0）——滚动动画永远不动，
                //   看起来就是数字突变（所有者实测：18.27 -> 18.12 无滚动）。
                g_display.OnSample(g_states.lastGood());
            }
        }

        // 真接口的启动（Key 与"要不要用"都在进循环之前做完了，见上面那块）
        if (g_realApiPlanned && !g_realApiOn) {
            dshb::BalanceSourceConfig cfg{};
            cfg.host = g_apiHost;
            cfg.port = g_apiPort;
            cfg.plainHttp = g_apiPlainHttp;
            cfg.timeoutMs = g_apiTimeoutMs;
            cfg.intervalMs = g_apiIntervalMs;
            cfg.once = g_apiOnce;
            cfg.apiKey = g_apiKey;   // 循环之前读好的，绝不打印
            g_apiSource.Start(cfg);
            g_realApiOn = true;
            SelfTestLog(L"[api] 真接口已启动：host=%ls port=%d 起始间隔=%dms 超时=%dms",
                        g_apiHost.c_str(), g_apiPort, g_apiIntervalMs, g_apiTimeoutMs);
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
                SelfTestLog(L"[layout] display: hasValue=%d value=%.4f target=%.4f tau=%.3f",
                            g_display.hasValue() ? 1 : 0, g_display.value(), g_display.target(),
                            g_display.tau);
            }
        }

        // 调试浮层：把状态机的判断摊开给人看。**它只显示，不参与任何逻辑。**
        if (g_debug) {
            // --debug：每 6 帧记一次"动画中的显示值"与"目标值"。
            // 两者不同 -> 正在滚动；一次相同 -> 已经落定。用来验证"不是突变"。
            static int dbgTick = 0;
            if (++dbgTick % 6 == 0) {
                SelfTestLog(L"[dbg] 显示值=%.4f 目标值=%.4f", g_display.value(), g_display.target());
            }
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

        // 显示值推进（C3）：测量值可以跳，显示值必须连续跟随。
        // 手动模式（--real / --display / --fixed-amount）下不喂样本，否则会把冻结的值改掉。
        const bool manualMode = (g_fixedGiven || g_realGiven || g_lastGiven || g_frames >= 0 || !g_seq.empty());
        g_display.SetCrisp(g_crisp);
        if (g_phaseOverride >= 0.0) g_display.SetPhaseOverride(g_phaseOverride);
        // ---- 真接口：UI 线程只做"取样本、取日志"两件事（HTTP 在后台线程，§11.1）----
        if (g_realApiOn) {
            dshb::Sample rs{};
            while (g_apiSource.Poll(&rs)) {
                g_states.OnSample(rs, rs.wallMs);
                // 只有成功的样本才动显示值；失败时保持原样（所有者："先当作没变"）
                if (rs.amountsOk) g_display.OnSample(g_states.lastGood());   // 取消延迟一拍：直接提交最新采样
            }
            std::string apiLine;
            // 带时间戳（设计 §10.5 的日志要求）：这样"暂停期间没请求""唤醒立刻补一次"可验证
            while (g_apiSource.PollLog(&apiLine)) SelfTestLog(L"[api t=%.1fs] %hs", elapsed, apiLine.c_str());
            // 连续失败到第 5 次才显示 --.--（阈值在 tuning.h）。
            // 跨过阈值/恢复各记一行：J6 要求日志能还原"为什么显示成这样"。
            static bool gaveUp = false;
            const int fails = g_apiSource.consecutiveFailures();
            if (fails >= dshb::kUnreadableAfterFailures && !gaveUp) {
                gaveUp = true;
                g_display.MarkUnreadable();
                SelfTestLog(L"[api] 连续失败 %d 次：显示 --.--（阈值 %d）", fails,
                            dshb::kUnreadableAfterFailures);
            } else if (fails == 0 && gaveUp) {
                gaveUp = false;
                SelfTestLog(L"[api] 采样恢复成功：显示恢复实时余额");
            }
        }

        // 右上角刷新倒计时：每秒变一次，纯数字，不做滚动动画。
        // 只有真接口开着才有意义——假数据源没有"下一次请求"。
        if (g_realApiOn) {
            const int msLeft = g_apiSource.msUntilNextFetch();
            const int secsLeft = (msLeft + 999) / 1000;
            dshb::SetCountdownText(std::to_wstring(secsLeft).c_str());
        }

        // ★ 这里原来每帧都喂一次 g_display.OnSample(g_states.lastGood())，已删除：
        //   同一条样本重复喂 -> lastReal_ 与 target_ 永远相等 -> 行程为 0 -> 没有滚动，
        //   只剩突变。（假数据源与真接口现在都在"新样本到达"处分发。）
        // --click-test：注入三次手势，验证"只有单击才切换"这条规则。
        // 走的是和真实鼠标**同一个**判定函数，不是旁路。
        // --pause-test：注入"锁屏 -> 解锁"，走的是与真实系统通知**同一个** WndProc 分支
        if (g_pauseTest) {
            static int ps = 0;
            if (ps == 0 && elapsed >= 3.0) {
                ps = 1;
                PostMessageW(g_hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
            } else if (ps == 1 && elapsed >= 9.0) {
                ps = 2;
                PostMessageW(g_hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
            }
        }
        if (g_clickTest) {
            static int ctStage = 0;
            static double ctAt = 1.0;

            if (ctStage < 3 && elapsed >= ctAt) {
                const dshb::SymbolRect sr = dshb::CurrencySymbolRect();
                if (sr.valid) {
                    const int cx = static_cast<int>((sr.l + sr.r) * 0.5f);
                    const int cy = static_cast<int>((sr.t + sr.b) * 0.5f);
                    if (ctStage == 0) {
                        SelfTestLog(L"[click-test] 手势 1：干净的单击（应当切换）");
                        g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64(); g_pressValid = true;
                        FinishLeftGesture(cx, cy);
                    } else if (ctStage == 1) {
                        SelfTestLog(L"[click-test] 手势 2：从符号拖出 40px（不应切换）");
                        g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64(); g_pressValid = true;
                        FinishLeftGesture(cx + 40, cy);
                    } else {
                        SelfTestLog(L"[click-test] 手势 3：按住 900ms 再松（不应切换）");
                        g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64() - 900; g_pressValid = true;
                        FinishLeftGesture(cx, cy);
                    }
                    ++ctStage;
                    ctAt = elapsed + 1.5;
                }
            }
        }
        g_display.Update(dt);

        // 组装这一帧要显示的东西，交给渲染层。渲染层不关心余额是怎么来的。
        {
            const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
            const bool currencyKnown = g_states.hasGood() && (g_display.shownCurrency() == "CNY" || g_display.shownCurrency() == "USD");
            renderer.SetWidgetFrame(dshb::BuildWidgetFrame(
                st, g_display, currencyKnown,
                ((g_display.shownCurrency() == "CNY") ? L"\u00A5"
                 : ((g_display.shownCurrency() == "USD") ? L"$" : L""))));
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
    g_apiSource.Stop();
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
}
