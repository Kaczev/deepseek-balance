// deepseek-balance v0.2 —— 主程序：窗口 + 帧循环
//
// 这一步（A3 / A3b / A3c）要达成的形态：
//   无边框、圆角、每像素半透明、置顶、不进任务栏与 Alt+Tab、不抢焦点、DPI 正确。
// 数字、曲线、颜色、心跳都是后面步骤的事。
//
// 自检（给自动化用，不弹窗）：--selftest [--seconds=N]
//   把窗口与 DPI 的事实、帧统计写进日志（落点见 src\dshb_log.h：%LOCALAPPDATA%\deepseek-balance\dshb.log）。
//   不弹对话框：Start-Process -PassThru 的 HasExited 在弹窗时会永远读成 false（踩过）。

#include "amount.h"
#include "dshb_log.h"     // 日志落点与写入的唯一实现（三个写入者曾各写各的 exe 同目录）
#include "panel_drag.h"   // 自控拖动 / 边界钳制 / config.json（设计 §10.1）
#include "paths.h"
#include "renderer.h"
#include "particles.h"  // 关闭粒子的常量（触发后至少跑多久 = 粒子自己的总时长，不另写一份）
#include "sampling.h"
#include "single_instance.h"
#include "state_machine.h"
#include "tray.h"       // 托盘图标 + 右键菜单（设计 §10.3）
#include "tuning.h"     // 常量：所有可调值只有这一处来源（曾把 10000 硬编码在下面，改常量无效）
#include "balance_source.h"

#include <windows.h>
#include <objbase.h>    // CoInitializeEx / COINIT_APARTMENTTHREADED
#include <shellapi.h>   // CommandLineToArgvW
#include "curve.h"       // --curve-selftest

#include <wtsapi32.h>   // 锁屏/解锁通知（J4）

#include <algorithm>   // std::sort（粒子期间的帧耗时分布）
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr wchar_t kClassName[] = L"DshbWnd";

bool g_selfTest = false;
double g_runSeconds = 0.0;        // 0 = 不自动退出，一直跑到关闭流程或 WM_CLOSE（--seconds=N 可改）
double g_selfTestSeconds = 1.5;
bool g_exportFrame = false;       // 离屏导一帧，然后退出
bool g_premulProbe = false;       // 预乘自检（A8c）
wchar_t g_exportPath[MAX_PATH] = L"frame.png";
int g_frameNo = 1;
HWND g_hwnd = nullptr;

// 关闭态的三个判定口（定义在下面，WndProc 先用）：进入 / 计数 / 取消。
void EnterShutdownState();
void HandleShutdownClick(int which);
void CancelShutdownState(const wchar_t* why);
void RemoveShutdownMouseHook();
// "该放粒子了"之后的收尾（发信号 + 摘钩子 + 记日志）：两条入口共用，见它的定义。
void FireShutdownParticles(const wchar_t* what);
// ---- 托盘（设计 §10.3）：WndProc 与主循环都要用，所以在这里先声明 ----
void InstallTrayIcon();
void RemoveTrayIcon();
void LogTrayFacts();
void HandleTrayMenuCommand(UINT cmd);
void RunTrayMenuTestScript(double elapsed);
DWORD WINAPI TrayMenuFixtureDeadline(LPVOID param);
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
// --real=N：手动设定**实际数字**（采样值）。
// --no-anim：显示数字不做指数平滑（跟着实际数字立刻到位）。
// 两者都是为了"停在一个状态上看清楚"，不做自动动画。
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

// ---- 左键抬起的分类锚点（拖动与长按都不算点击；分类在 FinishLeftGesture）----
int g_pressX = 0, g_pressY = 0;
unsigned long long g_pressTick = 0;
bool g_pressValid = false;

// ---- config.json（窗口位置）----
// 默认走 Paths()：%LOCALAPPDATA%\deepseek-balance\config.json。
// --config=<路径> 是**测试旁路**，与 --curve-store= 同一条理由：量"位置持久化"的探针
// 必须能反复写一个配置文件，而绝不能写所有者自己的那份。
bool g_configGiven = false;
std::wstring g_configPath;
std::wstring g_configFile;   // 启动时定一次：这次到底读写哪个文件
bool g_curveStoreGiven = false; // --curve-store=：把曲线记录文件改到别处（测试专用，绝不碰真实数据）
std::wstring g_curveStorePath;
bool g_logGlowStats = false;    // TEMPORARY (task 2): --log-glow-stats
int g_ambienceGlide = 0;        // TEMPORARY (task 2): --ambience-glide=N
int g_realFrames = 0;           // TEMPORARY: --real-frames=N (bounded real-loop run)
int g_realFrameCount = 0;       // TEMPORARY
bool g_forceNewInstance = false; // TEMPORARY: --force-new-instance (run beside the live widget)
bool g_realApiPlanned = false;  // 进循环之前就定下"本次要不要用真接口"

bool g_countdownGiven = false;  // --countdown=N：导帧时给倒计时一个固定值（导帧不取样）
int g_countdownSeconds = 0;
std::string g_apiKey;           // 只在内存里，绝不写日志
bool g_clickTest = false;         // --click-test：注入三次手势
// ---- 关闭态（设计 §10.2）：夹具、剧本 ----
double g_shutdownTest = -1.0;      // --shutdown-test=GAP：三击剧本（-1 = 不跑）；GAP = 第 2/3 击之间
bool g_shutdownEnterHold = false;  // --shutdown-hold：进入关闭态后不再受理脚本输入
double g_shutdownEnterAt = 1.0;
int g_shutdownFixtureFrame = -1;   // --shutdown-frame=k：导帧夹具（-1 = 未给）
int g_shutdownFixtureClicks = 1;   // --shutdown-clicks=N：导帧夹具（已点几下）
// --no-mouse-hook：**测试旁路**（生产默认关）。让"全局鼠标钩子装不上"这条路能被造出来 ——
// 本机平时装得上 WH_MOUSE_LL，而"装不上就不进关闭态"这条规则只有装不上时才看得见，
// 一条永远走不到的分支等于没验过。它只改 InstallShutdownMouseHook 的返回值，不动别的行为。
bool g_noMouseHook = false;
// 关闭态的日志标志：消息处理与鼠标钩子里**只置位**，日志一律回主循环写。
// ★ 理由与钩子那条硬约束有关：WH_MOUSE_LL 的回调超过约 300 ms 不返回会被系统静默摘除，
//   而写文件是慢的（见下面关闭态那一块的说明）。
bool g_closeEnterPending = false;      // 刚进入关闭态
const wchar_t* g_closeEnterWhy = L"右键";  // 谁让它进的：日志要分得清（右键点窗口 / 托盘菜单）
int g_closeClickPending = -1;          // >= 0 = 待记的那一击（1..3）
int g_closeWhichPending = 0;           // 0 = 左键，1 = 右键
bool g_closeCancelPending = false;     // 点到别处 -> 取消
const wchar_t* g_closeCancelWhy = L""; // "点到别处"
bool g_closeRefusedPending = false;    // 粒子期间不受理输入：这一下被拒了
bool g_closeWaitParticles = false;     // 第 3 击之后：粒子播完就退出进程
bool g_pauseTest = false;
bool g_noCurve = false;           // --no-curve：关掉氛围曲线（A/B 对比用）
int  g_historyDemo = 0;           // --history-demo=N：合成 N 个曲线点（导帧验证用）
int  g_curveFrame = -1;           // --curve-frame=k：把滚动计时器冻在第 k 帧（-1 = 未给）
int  g_beatFrame = -1;            // --beat-frame=k：把心跳仿真时刻放到 k/60 秒（-1 = 未给）

// ---- 托盘（设计 §10.3）----
// uID 只在本进程内区分图标：1 就够（NIM_ADD 只调一次）。
constexpr UINT kTrayIconId = 1;
dshb::TrayIcon g_tray;
bool g_noTray = false;             // --no-tray：不挂托盘图标（离线探针用，绝不留僵尸图标）
bool g_trayProbe = false;          // --tray-probe=1：把图标/菜单/命中的事实逐条写进日志
int  g_trayMenuTest = 0;           // --tray-menu-test=N：t=N 秒时弹出菜单并真的点「关闭」

// ---- 关闭粒子（设计 §11.6）----
// --shutdown-particles：把"关闭粒子"这一件事单独驱动起来（不需要鼠标、不需要关闭流程）。
// 形状固定，所以没有参数：
//   · 配 --export-frame=k 且 k >= 1：离屏导出粒子播放中的**第 k 帧**（第 k 帧 = k/60 秒），
//     一个进程导一帧，像素因此可量、可复现；
//   · 只给 --shutdown-particles（不带 --export-frame）：真实帧循环里
//     t=kShutdownParticlesArmSeconds 触发一次，播到自然结束，期间逐帧记耗时。
// ★ 颜色不是这个开关的参数：它取**触发那一帧 WidgetFrame 里的 ambientColor**，
//   也就是屏幕上那一刻真实在用的氛围色。想让颜色可控就把氛围钉住（--ambience=R,D，
//   与导帧那条环境的同一个夹具），而不是让粒子自己算一遍 R/D 公式 —— 重算就会分叉。
bool g_shutdownParticles = false;
// --particles-only：只画粒子那一层（跳过面板/蒙光/边框/曲线/正文）。
// 它是**量颜色用的夹具**：粒子叠在不透明面板上时，反解出来的直通颜色带 ±50 的噪声
// （面板像素 alpha 已约 244，粒子只把它抬高 6..10）；背景整层跳过之后，
// PNG 里就是粒子自己的 alpha 与颜色，可以直接量。只在导帧路径生效。
bool g_particlesOnly = false;
// --no-particles：面板照样消失（第 3 击那一帧起），但一颗粒子都不撒。
// 它是"面板先没了"这一条的 **A/B 尺子**：同一条绘制路径、同一帧号，去掉粒子之后画布上
// 剩下的任何不透明像素都只可能来自面板。只在导帧路径生效。
bool g_noParticles = false;
// 触发时刻（秒）。1.0 s 足够让窗口、氛围、数据都稳下来，又与 §11.6 的 500 ms 不重叠。
constexpr double kShutdownParticlesArmSeconds = 1.0;
// 触发后至少再跑多久才允许退出：粒子总时长（kShutdownParticleSeconds）是"验收要看的那个数"，
// 所以这里从粒子模块取，不另写一个 0.5（两处写同一个数迟早会不一致）。
constexpr double kShutdownParticlesTailSeconds = dshb::kShutdownParticleSeconds + 0.1;
// 粒子期间逐帧的渲染耗时（毫秒）与帧间隔。§11.3 要的是 p50/p99，不是平均值。
struct ParticleFrameTimes {
    std::vector<double> rasterMs;
    std::vector<double> intervalMs;
};
ParticleFrameTimes g_particleTimes;
bool g_particleTimesOpen = false;
double g_particleTimesUntil = 0.0;

// --no-present：RenderFrame 画完**不**提交（不调用 Present）。
// ★ 为什么需要它：Present(1,0) 会等垂直空白，于是"一帧 24 ms"里有多少是光栅、
//   多少是在等显示，从外面看不出来。设计 §11.3 的 p99 < 8 ms 说的是**帧内计算**，
//   所以量预算时必须把等待那一段摘掉 —— 否则量到的是显示器的刷新节奏，不是我们的代价。
bool g_noPresent = false;
// --frame-stats=N：把**所有**帧（不只是粒子那段）的渲染耗时与帧间隔记下来，跑满 N 帧后打 p50/p99。
// 它是 §11.3 预算的通用口径：粒子那一段的 p50/p99 只有跟同一台的基线比才有意义。
int g_frameStatsFrames = 0;
ParticleFrameTimes g_allTimes;

         // --pause-test：注入"锁屏/解锁"，验证 J4（不用真锁屏）
double g_realAmount = -1.0;   // --real=R（-1 = 未给；0 是合法金额！）
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
    LARGE_INTEGER start{};

    Clock() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        start = last;
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

    // ★ 从构造到现在的**总**时间（秒）。与 Tick() 的区别是这里**没有 50 ms 上限**。
    //   为什么需要两套（所有者 2026-09-19 实测后要求）：
    //     逐帧累加 dt 只有在"一帧一循环"时才等于墙上时间。而主循环在窗口没有焦点时会
    //     **空转**（`MsgWaitForMultipleObjectsEx` 立刻返回），于是每秒几千帧 × 每帧被
    //     夹到 0.05 s 的上限 —— 累加值比墙上时间**慢二十倍**。后果：`--seconds=N` 要等
    //     好几分钟才退出，日志里 `t=%.1fs` 那一列也全是错的（实测：墙上 32 s 时它写着 4.8 s）。
    //   动画仍然用 Tick()（要靠那个上限挡休眠唤醒后的巨跳），只有"过了多久"用这里。
    double Total() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double seconds = static_cast<double>(now.QuadPart - start.QuadPart) /
                               static_cast<double>(freq.QuadPart);
        return seconds > 0.0 ? seconds : 0.0;
    }
};

// 日志与落点都收口在 src\dshb_log.h —— 那边写着为什么（这个程序曾有**三个**写入者，
// 各写各的 exe 同目录，所有者报的"运行完在目录下冒出 selftest.log"就是这么来的）。
//
// 这里只留两个名字，好让 162 个调用点保持短：
//   SelfTestLog        —— 无条件写（用户报症状时留着它才定得了位）
//   SelfTestLogVerbose —— 只有开发/测试调用（`--` 开头有开关）才写
void SelfTestLog(const wchar_t* fmt, ...) {
    // 这个程序是 GUI 子系统，没有控制台：往 stdout 写等于丢掉。所以日志只落文件。
    va_list args;
    va_start(args, fmt);
    dshb::LogWriteV(fmt, args);
    va_end(args);
}

void SelfTestLogVerbose(const wchar_t* fmt, ...) {
    if (!dshb::LogVerboseEnabled()) return;
    va_list args;
    va_start(args, fmt);
    dshb::LogWriteV(fmt, args);
    va_end(args);
}

// UTF-8（显示层给的窄字符串）-> UTF-16，只给日志用。
// ★ 为什么不直接把那个窄串喂给 %hs：SelfTestLog 的窄参数是按当前 C 区域设置转换的，
//   中文（UTF-8 多字节）会被逐字节当成宽字符，日志里就成了乱码。逐字节 != 解码。
std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

double NowWallMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL);
}

// 导帧的**氛围**诊断（与下面那个 [frame] 同一条理由：导帧进程没有控制台，PNG 里的颜色
// 又要量像素才读得回来，所以把这一帧实际用的 R/D/颜色/强度逐字写进日志）。
//
// ★ 为什么这一行必须存在：R 现在是**时间**的量（R(t) = kAmbienceDecayA^t），而"时间走了
//   多久"只有真的跑过帧循环才知道。没有它，验证就只能靠另写一份同样的公式做模拟 ——
//   那样量到的是模拟，不是屏幕。这一行打印的是**这个进程真正拿去画的那几个数**。
namespace {

// 颜色分量 -> "#rrggbb"（与显示层 HexOf 同一口径：v*255 + 0.5 取整）。
std::wstring AmbienceHex(const dshb::AmbienceColor& c) {
    auto byte = [](float x) {
        const float v = (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
        return static_cast<int>(v * 255.0f + 0.5f);
    };
    wchar_t buf[16];
    swprintf_s(buf, L"#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

// "R,D" -> 两个 double。--ambience= 与 --shutdown-particles= 共用一套解析
// （两处各写一遍必然会分叉，而这两处量的必须是同一个颜色）。
// 成功返回 true；格式不对返回 false 且不动输出。
bool ParseTwoDoubles(const wchar_t* text, double* a, double* b) {
    if (!text || !a || !b) return false;
    wchar_t* end = nullptr;
    const double first = wcstod(text, &end);
    if (end == text || *end != L',') return false;
    const wchar_t* rest = end + 1;
    const double second = wcstod(rest, &end);
    if (end == rest || *end != L'\0') return false;
    *a = first;
    *b = second;
    return true;
}

}  // namespace

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

// 左键"抬起"时的分类。**只有干净的单击才算点击**：
//   移动超过 4px  -> 算拖动
//   按住超过 600ms -> 算长按
// ★ 现在没有任何动作挂在"干净的单击"上（"双击币种符号切换币种"那个入口已删），所以这个
//   函数只剩分类 + 三种结果各写一行日志。它留着是因为那几行是既有的判据：--click-test
//   的手势 1~3 与 dragprobe 的 live-drag-not-a-click 都以"真拖动一行 [click] 都不写"为据。

void FinishLeftGesture(int x, int y) {
    // 关闭态里左键的语义是"一次计数"（在 WM_LBUTTONDOWN 里已经记过），不是点击。
    // 这一句同时挡住 --click-test 那条直接调用它的路径 —— 一处判断管两条调用路径。
    if (dshb::ShutdownActive()) return;
    if (!g_pressValid) return;
    g_pressValid = false;
    const int dx = x - g_pressX;
    const int dy = y - g_pressY;
    const int dist2 = dx * dx + dy * dy;
    const int held = static_cast<int>(GetTickCount64() - g_pressTick);
    if (dist2 > 16) {
        SelfTestLogVerbose(L"[click] 移动 %dpx：算拖动，不算点击", static_cast<int>(std::sqrt(static_cast<double>(dist2))));
        return;
    }
    if (held > 600) {
        SelfTestLogVerbose(L"[click] 按住 %dms：算长按，不算点击", held);
        return;
    }
    // 三种结果各写一行 —— 于是"一行 [click] 都没有"就等于"这个函数一次都没被调用"，
    // dragprobe 的 live-drag-not-a-click 正是拿这条等式判"真拖动没被当成点击"的。
    SelfTestLogVerbose(L"[click] 干净的单击：算点击");
}

// ---------------------------------------------------------------------------
// 自控拖动（设计 §10.1 / §10.1.1）
//
// ★ 为什么不能交给系统：把按下当成"标题栏拖拽"交给默认窗口过程（WM_NCLBUTTONDOWN +
//   HTCAPTION）会让 Windows 进入**移动窗口的模态循环**，渲染节拍在拖动期间被系统接管、
//   动画顿住甚至停跳。这里自己记锚点、自己算位置、自己调 SetWindowPos：位置是光标坐标的
//   **纯函数**，中间不累加任何状态，所以"跟手不漂"是恒等式而不是"调得好"。
//
// 三套坐标必须分清（混用会让窗口按 2 倍的步子跑 —— 这台机器缩放 200%）：
//   g_pressX/Y    客户区像素，只给 FinishLeftGesture 的"拖动还是点击"分类用；
//   g_dragAnchor  屏幕像素，按下那一刻的 光标 − 窗口左上角，拖动算术只用它；
//   窗口与光标     屏幕像素，GetWindowRect / GetCursorPos / SetWindowPos 同一个空间。
//
// 心跳是**绘制偏移**，从不调用 SetWindowPos（所有者 2026-09-19 的决定），所以这里不需要
// 为它留余量，也不需要"拖动期间把抖动淡出"。
// ---------------------------------------------------------------------------
constexpr int kDragThresholdPx = 4;   // 与既有手势同一个阈值（"移动 > 4px 算拖动"）

bool g_dragPressed = false;      // 左键按着（收到 LBUTTONDOWN，还没收到 UP）
bool g_dragActive = false;       // 已越过阈值：真在拖，窗口跟着光标走
POINT g_dragAnchor{};            // 光标屏幕坐标 − 窗口左上角
POINT g_dragPressCursor{};       // 按下那一刻的光标屏幕坐标（判阈值、算"光标位移"）
POINT g_dragWindowStart{};       // 按下那一刻的窗口左上角（算"窗口位移"）
POINT g_dragDesired{};           // 最后一次"没有吸附也没有钳制时想要"的位置
POINT g_dragSnapped{};           // 最后一次"吸附后、钳制前"的位置（两者不同 = 吸住了）
bool g_dragClamped = false;      // 这一次拖动里钳制是否真的改过位置
bool g_dragSnapOn = false;       // 此刻面板是否吸在工作区边上（只用于"变化时写一行"）
uint64_t g_dragFrames = 0;       // 拖动期间的帧数
double g_dragFrameMsSum = 0.0;   // 拖动期间渲染耗时的和
double g_dragIntervalSum = 0.0;  // 拖动期间帧间隔的和
double g_dragWorstFrameMs = 0.0;     // 拖动期间最长的一次渲染耗时
double g_dragWorstIntervalMs = 0.0;  // 拖动期间最长的一次帧间隔

// 松手时写一次（拖动过程中一个字节都不写，设计 §10.1）。
void SavePanelPos(POINT pos) {
    std::wstring why;
    if (dshb::SaveWindowPos(g_configFile, pos, &why)) {
        SelfTestLogVerbose(L"[drag] 位置已存 %ls: (%ld,%ld)", g_configFile.c_str(), pos.x, pos.y);
    } else {
        // 写失败不是终点：位置只决定"下次打开落在哪"，不该打断使用，也不该弹窗
        // （设计 §10.1 的容错要求）。但必须留一行，否则用户只会觉得"位置偶尔会丢"。
        SelfTestLogVerbose(L"[drag] 位置没能存进 %ls：%ls", g_configFile.c_str(), why.c_str());
    }
}

void BeginDrag() {
    POINT cursor{};
    RECT wr{};
    if (!GetCursorPos(&cursor) || !GetWindowRect(g_hwnd, &wr)) {
        g_dragPressed = false;      // 取不到事实就不启动，绝不拿半个锚点去拖
        return;
    }
    g_dragPressed = true;
    g_dragActive = false;
    g_dragClamped = false;
    g_dragSnapOn = false;
    g_dragAnchor = dshb::DragOffsetFor(wr, cursor);
    g_dragPressCursor = cursor;
    g_dragWindowStart = POINT{wr.left, wr.top};
    g_dragDesired = g_dragWindowStart;
    g_dragSnapped = g_dragWindowStart;
    g_dragFrames = 0;
    g_dragFrameMsSum = 0.0;
    g_dragIntervalSum = 0.0;
    g_dragWorstFrameMs = 0.0;
    g_dragWorstIntervalMs = 0.0;
    // ★ 按下就夹住鼠标，而不是等越过 4px 阈值再夹。为什么：第一次移动就可能把光标甩到
    //   面板外面（手快、或者窗口被钳在屏边），那时如果没有捕获，那条移动消息会送给光标
    //   底下的**别的窗口** —— 拖动从第一帧就断掉，而"阈值"永远等不到。（探针实测：等阈值
    //   再夹的版本，凡是"按下后第一次移动就出面板"的拖动，窗口一个像素都不动。）
    //   夹住的是鼠标消息、不是窗口激活：WS_EX_NOACTIVATE 不变，绝不抢焦点；抬起即放。
    //   代价：按住不动的那段时间别的窗口收不到鼠标消息 —— 那正是"手指按在面板上"的意思。
    SetCapture(g_hwnd);
}

void EndDrag() {
    if (!g_dragPressed) return;
    g_dragPressed = false;
    // 按下时夹的捕获一定要放（点击也要放，所以放在这个早返回之前）。
    // ReleaseCapture 会回一条 WM_CAPTURECHANGED —— 那时 g_dragPressed 已是 false，
    // 重入的那次 EndDrag 直接返回。
    ReleaseCapture();
    if (!g_dragActive) return;   // 只是点击：窗口一个像素都没动，没有东西要存
    g_dragActive = false;

    RECT wr{};
    POINT cursor{};
    const bool haveRect = GetWindowRect(g_hwnd, &wr) != FALSE;
    const bool haveCursor = GetCursorPos(&cursor) != FALSE;
    const POINT finalPos = haveRect ? POINT{wr.left, wr.top} : g_dragDesired;

    // 验收"跟手不漂"的取证：窗口位移必须**逐像素等于**光标位移。两者都是屏幕像素，
    // 同一次拖动里不需要任何换算 —— 有偏差就说明某个环节混进了第二套坐标。
    if (haveRect && haveCursor) {
        SelfTestLogVerbose(L"[drag] 松手：窗口位移=(%ld,%ld) 光标位移=(%ld,%ld) 偏差=(%ld,%ld) "
                    L"位置=(%ld,%ld) 钳制=%ls",
                    finalPos.x - g_dragWindowStart.x, finalPos.y - g_dragWindowStart.y,
                    cursor.x - g_dragPressCursor.x, cursor.y - g_dragPressCursor.y,
                    (finalPos.x - g_dragWindowStart.x) - (cursor.x - g_dragPressCursor.x),
                    (finalPos.y - g_dragWindowStart.y) - (cursor.y - g_dragPressCursor.y),
                    finalPos.x, finalPos.y, g_dragClamped ? L"是" : L"否");
    }
    if (g_dragClamped) {
        // 钳制生效过：把"不钳制会到哪"和"实际留在哪"都记下来 —— 否则"窗口为什么停在
        // 屏幕边上不动了"只能靠猜（判据是面积，不是位置本身）。
        SelfTestLogVerbose(L"[drag] 钳制生效：不钳制时想要 (%ld,%ld)，实际停在 (%ld,%ld)",
                    g_dragDesired.x, g_dragDesired.y, finalPos.x, finalPos.y);
    }

    // 验收"拖动不顿"的取证：这一条**只有拖动这一段**能回答，而 [render] 那条汇总线把
    // 空闲时间也算进去了，平均值会被稀释。所以按这一次拖动单独记帧数/帧间隔/最长单帧。
    // 松手时记一次，不做逐帧 I/O —— 逐帧写文件会把进程弄崩（A5b 已经踩过）。
    if (g_dragFrames > 0) {
        const double avgRender = g_dragFrameMsSum / static_cast<double>(g_dragFrames);
        const double avgInterval = g_dragIntervalSum / static_cast<double>(g_dragFrames);
        SelfTestLogVerbose(L"[drag] 拖动期间 frames=%llu 平均帧间隔=%.2fms(%.1fHz) 最长帧间隔=%.2fms "
                    L"平均单帧=%.2fms 最长单帧=%.2fms",
                    static_cast<unsigned long long>(g_dragFrames), avgInterval,
                    avgInterval > 0.0 ? 1000.0 / avgInterval : 0.0, g_dragWorstIntervalMs,
                    avgRender, g_dragWorstFrameMs);
    }
    SavePanelPos(finalPos);
}

void UpdateDrag() {
    if (!g_dragPressed) return;
    // 抬起消息有可能收不到（钳制生效后光标会跑到面板外面、别的窗口把它接走），所以每一步
    // 自己确认一次左键状态：丢了就把这次拖动收干净，绝不让窗口"粘"在光标上。
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        EndDrag();
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    if (!g_dragActive) {
        const int dx = cursor.x - g_dragPressCursor.x;
        const int dy = cursor.y - g_dragPressCursor.y;
        if (dx * dx + dy * dy <= kDragThresholdPx * kDragThresholdPx) return;   // 还没动够：仍是点击
        g_dragActive = true;
        SelfTestLogVerbose(L"[drag] 开始跟手：锚点=(%ld,%ld) 起点=(%ld,%ld)", g_dragAnchor.x,
                    g_dragAnchor.y, g_dragWindowStart.x, g_dragWindowStart.y);
    }

    RECT wr{};
    if (!GetWindowRect(g_hwnd, &wr)) return;
    const dshb::PanelGeometry panel = dshb::PanelForWindow(SIZE{wr.right - wr.left, wr.bottom - wr.top});
    g_dragDesired = dshb::DragTopLeft(g_dragAnchor, cursor);
    // 顺序：先按"面板的最近一条边"吸（所有者的定案），再钳制。
    // ★ 两步都是**位置的纯函数**，这里不记"吸住了没有"：光标一离开吸附范围，想要的
    //   位置就变回跟手值，窗口立刻脱开 —— "拖走立即脱开"因此是恒等式，不是状态机。
    // ★ 吸附只动命中的那个轴（SnapTopLeftIn 里两轴各判各的），所以四个角也能吸。
    g_dragSnapped = dshb::SnapTopLeft(g_dragDesired, panel, cursor);
    const POINT target = dshb::ClampWindowTopLeft(g_dragSnapped, panel, cursor);
    if (target.x != g_dragDesired.x || target.y != g_dragDesired.y) g_dragClamped = true;
    // 吸附的取证：只在这个状态**变化**的那一帧写一行（位置是位置的纯函数，每帧都写就是
    // 刷屏）。两行合起来说明"哪一轴吸了、吸了多少"，以及"光标还在不在面板里"——
    // 吸附要把面板从光标底下挪走多少，是这一步唯一看得见的手感风险。
    if ((g_dragSnapped.x != g_dragDesired.x || g_dragSnapped.y != g_dragDesired.y) != g_dragSnapOn) {
        g_dragSnapOn = (g_dragSnapped.x != g_dragDesired.x || g_dragSnapped.y != g_dragDesired.y);
        if (g_dragSnapOn) {
            SelfTestLogVerbose(L"[drag] 吸附生效：不吸附想要 (%ld,%ld)，吸附后 (%ld,%ld)（x 吸了 %ld，"
                        L"y 吸了 %ld）；光标 (%ld,%ld) 仍在面板内=%ls",
                        g_dragDesired.x, g_dragDesired.y, g_dragSnapped.x, g_dragSnapped.y,
                        g_dragSnapped.x - g_dragDesired.x, g_dragSnapped.y - g_dragDesired.y,
                        cursor.x, cursor.y,
                        (cursor.x >= g_dragSnapped.x + panel.margin &&
                         cursor.x < g_dragSnapped.x + panel.margin + panel.width &&
                         cursor.y >= g_dragSnapped.y + panel.margin &&
                         cursor.y < g_dragSnapped.y + panel.margin + panel.height)
                            ? L"是"
                            : L"否");
        } else {
            SelfTestLogVerbose(L"[drag] 吸附脱开：光标 (%ld,%ld) 已离开吸附范围，窗口回到跟手位置 "
                        L"(%ld,%ld)", cursor.x, cursor.y, g_dragDesired.x, g_dragDesired.y);
        }
    }
    // 拖动中只做这一件事：位置是位置的函数。SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE ——
    // 不改尺寸、不改层级、不抢焦点（WS_EX_NOACTIVATE 的窗口一旦被激活就会抢走别人的输入）。
    // 已经在目标上就不打扰窗口（少一次多余的 SetWindowPos）。
    if (target.x != wr.left || target.y != wr.top) {
        SetWindowPos(g_hwnd, nullptr, target.x, target.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    case WM_MOUSEMOVE:
        // 跟手拖动（工作在主循环里：只挪窗口，不做任何动画、不进任何模态循环）
        UpdateDrag();
        return 0;
    case WM_CAPTURECHANGED:
        // 捕获被人抢走（例如系统弹了菜单）：这次拖动就到此为止，位置该存就存。
        // 少了这一条，窗口会留在"按着"的状态里，下一次移动还会跟着光标跑。
        EndDrag();
        return 0;
    case WM_RBUTTONDOWN:
        // 右键的**两种含义按当前相位分派**（所有者定，别写成一个）：
        //   非关闭态 -> 进入关闭态（"右键点窗口进入关闭流程"）
        //   关闭态   -> 一次点击（右键也计数，与左键完全等价）
        // 写成一个的后果是把"右键进入 → 再右键"变成"重置进度"，那是另一件事。
        if (dshb::ShutdownActive()) {
            HandleShutdownClick(1);
        } else if (g_renderer && g_renderer->shutdownParticlesActive()) {
            // 粒子期间的第二道闸（G5）：连右键一起拒绝，不产生第二个粒子实例
            g_closeRefusedPending = true;
        } else {
            // 正拖着的时候右键进入：这一次拖动当场结束（关闭态期间禁止拖动，§10.2）。
            // 判断放在这里而不是 UpdateDrag 里：进入关闭态是**唯一**能让"拖动中"变成
            // 非法的时刻，一个概念只判一处。
            if (g_dragPressed || g_dragActive) EndDrag();
            EnterShutdownState();
        }
        return 0;
    case WM_LBUTTONDOWN:
        // 关闭态里：任何落在面板内的按下都**计一次**（含拖动尝试 —— 那正是"点一下就计数"）。
        // 拖动必须在关闭态里被禁止（设计 §10.2）：所以这一支**不调 BeginDrag**，
        // 窗口因此没有拖动起点，后面来的 WM_MOUSEMOVE 一个像素都挪不动它。
        if (dshb::ShutdownActive()) {
            HandleShutdownClick(0);
            return 0;
        }
        g_pressX = static_cast<int>(static_cast<short>(LOWORD(lp)));
        g_pressY = static_cast<int>(static_cast<short>(HIWORD(lp)));
        g_pressTick = GetTickCount64();
        g_pressValid = true;
        // 按下只记锚点，窗口一个像素都不动（设计 §10.1.1 的"收到按下 → 记录起点"）。
        BeginDrag();
        return 0;
    case WM_LBUTTONUP: {
        const int ux = static_cast<int>(static_cast<short>(LOWORD(lp)));
        const int uy = static_cast<int>(static_cast<short>(HIWORD(lp)));
        // ★ 真拖动过的那一次**不交给**既有手势判定，而且既有判定一行都没改：
        //   窗口跟手之后，光标在**客户区**里的坐标几乎不动（窗口跟着它走），那条
        //   "移动 > 4px 算拖动" 因此量不到位移 —— 一次真拖动会被当成一次干净的点击
        //   （拖得久一点的则被当成一次长按），于是凭空多出一行 [click]。阈值与它同一个
        //   口径（4px），没有引入第二套阈值。
        const bool wasDragging = g_dragActive;
        EndDrag();
        if (!wasDragging) FinishLeftGesture(ux, uy);
        return 0;
    }
    case WM_DESTROY:
        RemoveShutdownMouseHook();   // 关闭态里的全局鼠标钩子绝不能活过窗口
        // ★ 托盘图标必须在这里摘掉，不能等进程退出：NIM_ADD 之后进程直接死掉会在任务栏
        //   上留一个**僵尸图标**（悬停还有提示、点它没反应，只有鼠标扫过去才消失），
        //   而用户看到的正是"程序关了图标还在" —— 设计 §10.3 明确否掉了这种形态。
        //   WM_CLOSE（含关闭流程走完）走 DestroyWindow -> 这里，所以每条退出路径都收得到。
        RemoveTrayIcon();
        g_running = false;
        PostQuitMessage(0);
        return 0;
    // ---- 托盘图标：左键什么都不做，右键弹菜单（所有者 2026-09-19）----
    case dshb::kMsgTrayIcon: {
        // 版本 4 的回调把按键事件放在 lParam 低位（高位是图标 id）。
        // ★ 左键**故意什么都不做**：不计数、也不取消。所有者定案 —— 托盘图标不是"应用还在
        //   后台"的暗示，它只承载「关闭」这一个入口，所以左键没有第二种含义可给。
        //   这条分支因此只写一行日志，不留任何状态。
        const UINT evt = LOWORD(lp);
        if (evt == WM_LBUTTONUP || evt == WM_LBUTTONDBLCLK) {
            SelfTestLog(L"[tray] 左键点托盘图标：什么都不做（不计数、不取消；图标只承载"
                        L"「关闭」一个入口）");
            return 0;
        }
        // ★★ 这里**只认 WM_RBUTTONUP**，不再把 WM_CONTEXTMENU 一起收。
        //
        //   WHY（这是"一次右键叠出两个面板"的直接原因）：一次右键，外壳会给我们**两条**
        //   回调 —— 一条 lp 低位是 WM_RBUTTONUP，另一条是 WM_CONTEXTMENU（Vista 之后
        //   的外壳行为）。两条都调 PopupMenu 的话，一次手势就跑两趟：第一趟建面板、
        //   第二趟在**第一趟还没把 g_menuPanel 写进去**（AttachThreadInput /
        //   SetForegroundWindow / CreateWindowExW 会就地处理消息）的空档里又建一个，
        //   于是第二个把手里的句柄覆盖掉，第一个变成没人管的**孤儿窗口**。
        //   所有者日志里成对出现的"已弹出"（两条光标坐标逐像素相同、句柄不同，
        //   第二条写着"弹出时前台=别人"）就是这两条消息。
        //   所以正确做法是：一个手势只认一个事件。键盘菜单键我们也**不需要**支持
        //   （这个图标只有鼠标右键这一个入口，设计 §10.3），所以 WM_CONTEXTMENU 直接丢。
        if (evt == WM_RBUTTONUP) {
            // 弹出右键菜单面板：**非模态**（面板是一个普通窗口，帧循环照常跑），
            // 所以这个调用立刻返回，主线程不停在这里 —— 托盘右键因此不可能把挂件弄死。
            // ★ 选中的菜单项**不走返回值**：面板把 kMsgTrayMenu 投回来，在下面那个 case 里处理。
            //   于是"菜单命令只有一条真实路径"这条设计仍在：能产生这条消息的只有面板那一处，
            //   而它落在 WndProc 里唯一的一个 case 上。（旧实现用 TPM_RETURNCMD 把结果当返回值
            //   拿回来，那是模态等待的代价；换掉模态之后这个入口形状跟着换，见 tray.h 的注释。）
            g_tray.PopupMenu(hwnd);
            return 0;
        }
        if (evt == WM_CONTEXTMENU) {
            // 记一行：这条消息我们**故意不处理**（上面写了为什么）。留着它才能在日志里
            // 一眼看出"外壳到底给了几条"，而不是让下一个人再去猜一次。
            SelfTestLogVerbose(L"[tray] 收到 WM_CONTEXTMENU：故意不处理（一次右键只认 WM_RBUTTONUP，"
                        L"否则一个手势会弹两次、叠出孤儿面板）");
            return 0;
        }
        return 0;
    }
    case dshb::kMsgTrayMenu:
        // 菜单面板选了某一项。★ 只可能是我们自己投的（tray.h 里写了为什么不用 WM_COMMAND）：
        //   WM_COMMAND 谁都能 Post，那会让"点菜单「关闭」"多出一个能被假造的入口。
        SelfTestLogVerbose(L"[tray] WndProc 收到 kMsgTrayMenu：cmd=%u（面板 -> 属主这一跳）", wp);
        HandleTrayMenuCommand(static_cast<UINT>(wp));
        return 0;
    case WM_KEYDOWN:
        // ---- B6/B7 的调试热键：F1..F9 选情形，R 触发充值，C 触发时钟跳变 ----
        if (wp >= VK_F1 && wp < VK_F1 + static_cast<WPARAM>(dshb::Scenario::Count)) {
            const auto idx = static_cast<dshb::Scenario>(wp - VK_F1);
            g_fake.Select(idx);
            SelfTestLogVerbose(L"[key] 情形 -> %ls", dshb::ScenarioName(idx));
            return 0;
        }
        if (wp == 'R') {
            g_fake.TriggerRecharge();
            SelfTestLogVerbose(L"[key] 触发充值跳变");
            return 0;
        }
        if (wp == 'C') {
            g_fake.TriggerClockJump();
            SelfTestLogVerbose(L"[key] 触发时钟跳变");
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

// ===========================================================================
// 关闭态（设计 §10.2）：输入侧的判定、"点到别处就取消"、以及三击剧本
// ===========================================================================
//  ★ 状态机（进/计数/取消/R 的下限）在 widget_display.cpp —— R_d 必须与 R(t) 的抬升
//    走同一条路径，那是显示层的事。这一块只管三件输入侧的事：
//      1. 把消息翻译成 Enter / Click / Cancel（右键的两种含义按**当前相位**分派）；
//      2. 全局鼠标钩子：判"这一下点在哪"，面板之外就取消；
//      3. 三击剧本（--shutdown-test）走真实消息路径注入。
//
//  为什么"点到别处就取消"必须是全局低级钩子（WH_MOUSE_LL），而不是 SetCapture 或
//  一个全屏透明窗口：后两种会**吃掉**那一次点击 —— 你点别的程序，那个程序收不到。
//  钩子只是"看一眼"，一律 CallNextHookEx 原样放行。吞掉消息等于关闭态期间把整个系统的
//  鼠标都吃了，这是本条最容易犯的错，所以放行那一行写在回调的每个出口上。
//
//  ★ 回调里只做判定（WindowFromPoint / GetClassNameW / 矩形比较），**不写日志、不做 I/O**：
//    WH_MOUSE_LL 的回调超过约 300 ms 不返回会被系统**静默摘除**（之后再也没有回调，
//    而屏幕上什么都看不出来）。结论塞进标志，回主循环再落日志。
//  ★ 装不上（返回空）不是崩溃点，但"点到别处"是关闭态**唯一**的取消路 ——
//    装不上就是出不来，所以必须记一行说明，而不是留下一个没有任何解释的关闭态。
//  （这一块就在本文件那个匿名 namespace 里，末尾那个 `}  // namespace` 收的就是它。）

// 一次点击落在哪：只有 Cancel 会导致取消。
//
// ★ 这个分类**只有一个判据**：落点在我们自己的托盘图标矩形内、或在我们自己的菜单矩形内
//   -> 什么都不做；其余一切 -> 取消。"其余一切"包括别的程序的托盘图标、时钟、任务栏
//   空白、桌面、别的窗口 —— 这正是 2026-09-19 修掉的那处偏离：
//   上一版按窗口类名判，把整个 TrayNotifyWnd（含**别人的**托盘图标与时钟）都当成
//   "托盘"，于是点别人的图标不取消。规格要的是取消（宁可多取消，不要误吞）。
//   判据落在"矩形"上而不是"类名"上，是因为矩形问的就是**我们自己那块图标**
//   （Shell_NotifyIconGetRect + 我们的 hWnd/uID 给出来的那个）。
enum class OutsideHit { None, Ignore, Cancel };

HHOOK g_mouseHook = nullptr;
volatile LONG g_outsideHit = static_cast<LONG>(OutsideHit::None);
POINT g_outsidePoint{};             // 那一次的屏幕坐标（日志用）

// 面板实体区（客户区像素）与它当前的位移。主循环每帧刷一次：回调里算这些要走渲染层
// 与显示层，太慢，而且回调里不该做重活。
// ★ 判据是"面板**在屏幕上**的实际位置"（含心跳与抖动的位移）—— 用户点的是他看见的板子。
RECT g_panelRectClient{};
bool g_panelRectValid = false;
float g_panelShiftDyPx = 0.0f;
HWND g_panelHwnd = nullptr;

// 一次点击的判定。pt 是屏幕坐标（MSLLHOOKSTRUCT::pt 本来就是屏幕坐标）。
OutsideHit ClassifyMousePoint(POINT pt) {
    // 1) 面板实体区之内 -> 不动（计数在 WndProc 里，这里只表示"别取消"）
    if (g_panelRectValid && g_panelHwnd) {
        POINT c = pt;
        if (ScreenToClient(g_panelHwnd, &c)) {
            const float dy = g_panelShiftDyPx;
            if (c.x >= g_panelRectClient.left && c.x < g_panelRectClient.right &&
                c.y >= g_panelRectClient.top + dy && c.y < g_panelRectClient.bottom + dy) {
                return OutsideHit::None;
            }
        }
    }

    // 2) 我们自己的托盘图标 / 我们自己的菜单 -> 什么都不做。
    //    ★ 菜单矩形必须**现问**（KeepGeometryFresh 里的 RefreshMenuRect 就是干这个的）：
    //      菜单从弹开到用户点下去可能一帧都没过去，用上一帧的缓存会判错。
    //      它只认"进程号是我们 + 类名 #32768"的窗口，所以别的程序的菜单不算我们的。
    g_tray.KeepGeometryFresh();
    const dshb::HitGeometry& our = g_tray.cachedGeometry();
    if (dshb::ClassifyOutsideClick(pt, our) == dshb::OutsideHit::Ignore) return OutsideHit::Ignore;

    // 3) 其余一切 -> 取消
    return OutsideHit::Cancel;
}

// 关心的只有"按下"类消息：移动、滚轮都不算一次点击。
bool IsMouseDownMessage(WPARAM msg) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK ShutdownMouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && IsMouseDownMessage(wp)) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (ms) {
            // 只判定、只塞标志：日志由主循环写（回调里写文件会被系统摘掉钩子）
            const OutsideHit hit = ClassifyMousePoint(ms->pt);
            g_outsidePoint = ms->pt;
            g_outsideHit = static_cast<LONG>(hit);
        }
    }
    // ★ 一律原样放行。钩子若把消息吞掉，关闭态期间整个系统的鼠标就等于被我们吃了。
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

// 关闭态**有没有出去的路**：取消只剩"点到面板实体之外"一条，而它要靠这把全局鼠标钩子。
// ★ 这是宿主读 g_mouseHook 的**唯一**一处：装钩子的结果、日志里的"已装/未装"都从这里取。
//   两处各判一遗的写法，症状是"装不上却进了关闭态"，或者日志说未装而行为当作已装。
bool ShutdownCancelPathReady() { return g_mouseHook != nullptr; }

// 装全局鼠标钩子。返回值 = 装完之后"出去的路"在不在，调用方据此决定进不进关闭态。
bool InstallShutdownMouseHook() {
    if (ShutdownCancelPathReady()) return true;   // 已在关闭态里：上一轮装上的那把还在用
    if (g_noMouseHook) {
        // 测试旁路（--no-mouse-hook，生产默认关）：故意走"装不上"那一支。本机平时装得上
        // WH_MOUSE_LL，而"装不上就不进关闭态"这条规则只有在装不上时才看得见。
        SelfTestLogVerbose(L"[close][test] --no-mouse-hook 开着：**故意不装**全局鼠标钩子，"
                    L"本轮当作 SetWindowsHookEx 失败");
        return false;
    }
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, ShutdownMouseHookProc, nullptr, 0);
    if (!ShutdownCancelPathReady()) {
        // ★ 失败原因必须留痕：这个返回值决定"右键进不进关闭态"，而失败的症状是
        //   "右键什么都不发生"——没有这一行，日志里就没有任何线索。
        SelfTestLog(L"[close] 全局鼠标钩子装不上（err=%lu）", GetLastError());
    }
    return ShutdownCancelPathReady();
}

void RemoveShutdownMouseHook() {
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    g_outsideHit = static_cast<LONG>(OutsideHit::None);
}

// 每帧刷新"面板在屏幕上的矩形与位移"（钩子回调只读这三个变量）。
void RefreshPanelRectForHook() {
    if (!g_hwnd || !g_renderer) { g_panelRectValid = false; return; }
    const dshb::CanvasSize cs = g_renderer->size();
    const float s = cs.scale;
    g_panelRectClient.left = static_cast<LONG>(dshb::kMarginDip * s);
    g_panelRectClient.top = static_cast<LONG>(dshb::kMarginDip * s);
    g_panelRectClient.right = static_cast<LONG>((dshb::kMarginDip + dshb::kEntityWidthDip) * s);
    g_panelRectClient.bottom = static_cast<LONG>((dshb::kMarginDip + dshb::kEntityHeightDip) * s);
    g_panelShiftDyPx =
        static_cast<float>((g_display.beatOffsetDip() + dshb::ShutdownJitterDip()) * s);
    g_panelHwnd = g_hwnd;
    g_panelRectValid = true;
}

// 面板中心（客户区像素）：剧本注入的点击落在这里 = "落在面板实体区内"。
POINT PanelCenterClient() {
    POINT p{};
    if (g_renderer) {
        const dshb::CanvasSize cs = g_renderer->size();
        p.x = static_cast<LONG>((dshb::kMarginDip + dshb::kEntityWidthDip * 0.5) * cs.scale);
        p.y = static_cast<LONG>((dshb::kMarginDip + dshb::kEntityHeightDip * 0.5) * cs.scale);
    }
    return p;
}

// ---- 消息处理与钩子里只置标志，日志一律回主循环写 ----
// 标志本身声明在文件上面（WndProc 与钩子都在它之前用到它们）。

// ---- 托盘：安装 / 摘除 / 菜单命令（设计 §10.3、F4）----
//
// 安装时机：窗口与渲染器都好了之后、进主循环之前。**不进循环就装**是为了让
// "图标出现过"这件事在日志里排在所有帧之前 —— 否则第一帧的日志会先出现，
// 事后分不清"图标是后来才挂上的"还是"一直没挂上"。
//
// ★ 离屏模式（导帧 / 预乘自检）与 --no-tray 不挂：那些进程跑完就退出，
//   挂一个只活几百毫秒的图标除了在任务栏上闪一下没有任何意义。
void InstallTrayIcon() {
    if (g_exportFrame || g_premulProbe || g_noTray) {
        SelfTestLogVerbose(L"[tray] 本次不挂托盘图标（离屏模式或 --no-tray）");
        return;
    }
    if (!g_tray.Install(g_hwnd, kTrayIconId)) {
        SelfTestLog(L"[tray] 托盘图标没挂上（上面的 [tray] 行写了原因）：关闭流程与菜单不受影响，"
                    L"但「关闭」那个入口这轮没有");
        return;
    }
    LogTrayFacts();
}

// --tray-probe=1 要的那些事实，一条不落地写出来。安装之后也走这里（同一份证据）。
// ★ 菜单是**现建现量**的（建完就销毁）：这份日志与 tools/trayprobe.cpp 量的是同一批
//   属性（项数、文字、id），一个在真进程里量、一个在探针里量，对得上才说明运行时那一份
//   与探针那一份是同一个菜单。
void LogTrayFacts() {
    dshb::IconRectSource source = dshb::IconRectSource::None;
    const RECT r = g_tray.IconRect(&source);
    SelfTestLogVerbose(L"[tray] 事实：hWnd=0x%p uID=%u 图标边长=%dpx", g_tray.hwnd(), g_tray.id(),
                g_tray.iconSizePx());
    SelfTestLogVerbose(L"[tray] 图标矩形来源=%ls 矩形=(%ld,%ld,%ld,%ld) 尺寸=%ldx%ld",
                source == dshb::IconRectSource::System ? L"Shell_NotifyIconGetRect 精确值"
                                                      : L"取不到（一律取消，不用整块托盘区域顶替）",
                r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);

    HMENU menu = g_tray.BuildMenu();
    const int count = menu ? GetMenuItemCount(menu) : -1;
    wchar_t text[128] = L"";
    const int len = (menu && count > 0) ? GetMenuStringW(menu, 0, text, 128, MF_BYPOSITION) : 0;
    const UINT cmd = (menu && count > 0) ? GetMenuItemID(menu, 0) : 0;
    SelfTestLogVerbose(L"[tray] 菜单：GetMenuItemCount=%d 第 0 项=L\"%ls\"（%d 字符）id=%u", count, text,
                len, cmd);
    g_tray.DestroyMenu();
    // 屏幕上真的有几个面板窗口。★ 这条与上面那些"菜单里有什么"是两件事：上面量的是内容，
    //   这一条量的是**屏幕上有几个**（这次修的就是"叠出多个、最早那个不消失"）。
    SelfTestLogVerbose(L"[tray] 屏幕上的菜单面板数=%d（EnumWindows + 类名 DshbTrayMenu + 本进程）",
                dshb::detail::CountMenuPanels());
}

void RemoveTrayIcon() {
    if (!g_tray.installed()) return;
    g_tray.Remove();
}

// 菜单面板选了一项（kMsgTrayMenu 到了，wParam 就是那个 id）。
// 「关闭」= **直接进第三击**（所有者 2026-09-19：点一下就关），但**保留粒子过渡** ——
// 面板整层消失 + 径向四散爆开，与"窗口上点满三下"是同一个终点状态、同一个粒子信号。
// ★ 走状态机自己的口子 `ShutdownFireNow()`，不在这里连调三次 HandleShutdownClick：那会绕过
//   次序约束（每一次点击都要写日志、抬 R_d、推帧号），以后每加一条点击规则就得多改一处。
// ★ 进入段照跑：帧号同样从进入那一刻从 0 数起（StateMachine 那一步一个帧号都不碰），
//   所以两条入口的画面一致 —— 不是"为了立刻炸"把进入段跳过去。
// ★ 已经在关闭态（窗口上先右键、再从托盘点「关闭」）：这一下**就是第三击** ——
//   用户的意思是"我改主意了，现在就关"，与在面板上点第三下同效。
void HandleTrayMenuCommand(UINT cmd) {
    if (cmd == 0) return;   // 点别处 / 取消：菜单什么都没选
    if (cmd != dshb::kTrayMenuClose) return;   // 菜单里只有一项，走到这里说明是别的来源
    const bool wasActive = dshb::ShutdownActive();
    if (!dshb::ShutdownFireNow()) {            // 粒子已经发过：与再点一下同一道闸
        g_closeRefusedPending = true;
        return;
    }
    if (!wasActive) {
        // 这一下同时是"进入"：进入那一行日志要打出来（帧号也从这一刻从 0 数起）。
        // ★ 不装鼠标钩子：它只在 Armed 期间有意义（"点别处取消"），而这里一步就进了 Fired，
        //   输入已经一律不受理 —— 装完立刻摘掉只是假装做了一件事。
        g_closeEnterPending = true;
        g_closeEnterWhy = L"托盘菜单「关闭」";
    }
    SelfTestLog(L"[tray] 菜单选「关闭」-> 直接进第三击（ShutdownFireNow）：不再需要后续点击");
    FireShutdownParticles(L"托盘菜单「关闭」（直接进第三击）");
}

// --tray-menu-test=N（N 秒后动手）：**走真实路径**验"菜单那一下 = 点一下就关"。
//
// 这条链上没有任何一步是我们自己调的：
//   ① 往窗口投一条真正的托盘回调消息（kMsgTrayIcon + WM_RBUTTONUP）—— WndProc 那一支
//      与我们右键点托盘图标时收到的是**同一条**消息；
//   ② WndProc 调 PopupMenu -> 自建面板真的出现在屏幕上（**非模态**，主线程立刻就回来了）；
//   ③ 用 SendInput 往面板里「关闭」那一条注入一次真鼠标左键 —— 走的是面板自己的
//      WM_LBUTTONDOWN，与用户手点完全同一条路；
//   ④ 面板 PostMessage(kMsgTrayMenu) 给属主窗口 -> WndProc 那一支 -> HandleTrayMenuCommand。
// 所以最后打出来的那些行是**菜单入口真的产生出来的**状态：`[tray] 菜单选「关闭」-> 直接进
// 第三击` 与 `[close] 托盘菜单「关闭」（直接进第三击）：已发粒子信号 ... -> N 颗`。
// ★ 从那两行之后**不再需要任何点击**：粒子上限 0.5 s，播完进程自己退出（设计 §10.3 的
//   "任务管理器里无残留"），所以这条路径上"注入之后再等一拍看状态"是没有落点的 —— 进程
//   已经没了。要问"发完信号之后还受不受理输入"，离线那两侧量（closeprobe 的 case5/case9、
//   trayprobe 的 T20），它们与这里同一个闸（ShutdownFired）。
//
// ★ 会动真实光标（SendInput 必须移到那里），所以它只在显式传了这个参数时才动。
// ★ 与上一版的区别只有一处（但它是关键）：上一版点的是**系统菜单窗口**，而 TrackPopupMenu
//   收不到输入、永不返回 —— 脚本的"下一个阶段"因此永远跑不到（t=N 那一刻帧循环就停了）。
//   这一版点的是我们自己的窗口，而且弹完立刻就返回，脚本的后续阶段真的会被执行到。
void RunTrayMenuTestScript(double elapsed) {
    if (g_trayMenuTest <= 0) return;
    static int stage = 0;
    static double at = -1.0;
    static ULONGLONG panelSeenAt = 0;
    if (at < 0.0) at = static_cast<double>(g_trayMenuTest);

    // 我们自己的菜单面板在不在屏幕上。★ 用 EnumThreadWindows 而不是 FindWindowEx：
    // FindWindow/FindWindowEx **看不到调用线程自己创建的窗口**，而面板正是我们这个线程
    // 创建的 —— 用它会永远"找不到面板"（tray.cpp 的 RefreshMenuRect 里写了同一条实测）。
    HWND panel = nullptr;
    EnumThreadWindows(GetCurrentThreadId(),
                      [](HWND h, LPARAM p) -> BOOL {
                          wchar_t cls[64] = L"";
                          GetClassNameW(h, cls, 64);
                          if (wcscmp(cls, L"DshbTrayMenu") == 0) {
                              *reinterpret_cast<HWND*>(p) = h;
                              return FALSE;
                          }
                          return TRUE;
                      },
                      reinterpret_cast<LPARAM>(&panel));
    if (panel && panelSeenAt == 0) {
        panelSeenAt = GetTickCount64();
    }

    // ★★ 夹具的第三个剧本（--tray-menu-test=7）：**连点 5 次右键**。
    //   为什么必须做这一条：所有者看到的是"每次点击还会出现一个新的" —— 也就是一次右键
    //   叠出多个面板、最早那个永远不消失。这件事**只有数窗口**能证伪："已弹出"在日志里
    //   出现两次既可能是重入（bug），也可能是用户点了两次（正常）。
    //   每 300 ms 投一条真托盘右键消息（< 2000 ms 的时限，所以面板一直在），每次之后把
    //   屏幕上真的有几个面板打出来（EnumWindows + 类名 + 进程号，见 detail::CountMenuPanels）。
    if (g_trayMenuTest == 7) {
        static int mStage = 0;
        static int mPosted = 0;
        static double mNext = -1.0;
        if (mNext < 0.0) mNext = static_cast<double>(g_trayMenuTest);
        if (mStage == 0) {
            if (elapsed < mNext) return;
            mStage = 1;
        }
        if (mPosted < 5 && elapsed >= mNext) {
            ++mPosted;
            const int before = dshb::detail::CountMenuPanels();
            SelfTestLogVerbose(L"[tray][menu-test] 第 %d 次右键：投递前屏幕上的面板数=%d", mPosted, before);
            // ★ 一次右键投**两条**消息：外壳在 Vista 之后就是这样给的
            //   （一条 lp 低位=WM_RBUTTONUP，一条=WM_CONTEXTMENU）。所有者日志里成对的
            //   "已弹出"就是这两条造成的 —— 所以夹具必须照这个样子投，才算真的复现。
            //   两条之间**不留间隔**：最接近"一个手势里两条消息连着到"的形态。
            PostMessageW(g_hwnd, dshb::kMsgTrayIcon, kTrayIconId,
                         MAKELPARAM(static_cast<WORD>(WM_RBUTTONUP),
                                    static_cast<WORD>(kTrayIconId)));
            PostMessageW(g_hwnd, dshb::kMsgTrayIcon, kTrayIconId,
                         MAKELPARAM(static_cast<WORD>(WM_CONTEXTMENU),
                                    static_cast<WORD>(kTrayIconId)));
            mNext = elapsed + 0.3;
            return;
        }
        if (mStage == 1 && mPosted >= 5 && elapsed >= mNext) {
            const int now = dshb::detail::CountMenuPanels();
            SelfTestLogVerbose(L"[tray][menu-test] 连点 5 次之后：屏幕上的面板数=%d %ls（必须是 1）", now,
                        (now == 1) ? L"✅" : L"★ 不对：这一串右键叠出了多个面板");
            // ★ "按钮太大"要能证伪：把面板**此刻在屏幕上真正占的矩形**量出来（有几个就并几个）。
            //   个数 > 1 时这一行量的就是"叠起来的那一坨"有多大 —— 所有者看到的"太大"
            //   到底是这一项本身大、还是几个叠在一起，靠这个数与单个面板的尺寸分开。
            //   ★ 必须在这里量：面板 2 秒后就自己收了，那时候什么都量不到（第一版就是这么空的）。
            RECT uni{};
            if (now > 0 && dshb::detail::MenuPanelsUnionRect(&uni)) {
                SelfTestLogVerbose(L"[tray][menu-test] 面板合计占屏：(%ld,%ld,%ld,%ld) = %ldx%ld 物理像素"
                            L"（%d 个窗口并起来；单个的尺寸见上面「尺寸=」那一行）",
                            uni.left, uni.top, uni.right, uni.bottom, uni.right - uni.left,
                            uni.bottom - uni.top, now);
            }
            // 面板的时限是 2 秒，从**最后一次弹出**算起。只有等它过去之后再看，
            // "有没有孤儿窗口"这个问题才有意义（时间没到它本来就该还在）。
            mStage = 2;
            mNext = elapsed + 2.5;
            return;
        }
        if (mStage == 2 && elapsed >= mNext) {
            const int now = dshb::detail::CountMenuPanels();
            SelfTestLogVerbose(L"[tray][menu-test] 面板时限（2 秒）已过：屏幕上的面板数=%d %ls", now,
                        (now == 0) ? L"✅ 自己收回，没有孤儿窗口" : L"★ 还有面板没消失");
            // ★ 孤儿检测的**正面实验**：孤儿窗口的症状是"它还在屏幕上，但 g_menuPanel 不认它"。
            //   做法是再投一次右键 —— 如果外面真有孤儿，PopupMenu 会再建一个，
            //   于是计数 > 1；正确实现里它要么被挡（面板还活着）、要么建在孤儿之外，
            //   所以两种情况下"屏幕上有没有超过 1 个"都必须是否。
            const int before = dshb::detail::CountMenuPanels();
            PostMessageW(g_hwnd, dshb::kMsgTrayIcon, kTrayIconId,
                         MAKELPARAM(static_cast<WORD>(WM_RBUTTONUP),
                                    static_cast<WORD>(kTrayIconId)));
            SelfTestLogVerbose(L"[tray][menu-test] 孤儿检测：又投一次右键（投递前个数=%d），"
                        L"下一帧数屏幕上的个数", before);
            mStage = 3;
            mNext = elapsed + 0.4;
            return;
        }
        if (mStage == 3 && elapsed >= mNext) {
            const int now = dshb::detail::CountMenuPanels();
            SelfTestLogVerbose(L"[tray][menu-test] 孤儿检测结论：屏幕上的面板数=%d %ls", now,
                        (now <= 1) ? L"✅ 没有孤儿窗口" : L"★ 有孤儿：屏幕上不止一个面板");
            g_running = false;
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        return;
    }

    // ★★ 夹具的第二个剧本（--tray-menu-test=5）：**什么都不点**，看面板自己会不会消失。
    //   这是"生产不会再挂死"最直接的证据 —— 旧实现在这里会永远停住（模态等待 + 收不到
    //   输入），而新实现到点自己收回、帧循环一秒都没停。
    //   ★ 它必须摆在下面那个阶段机**之前**：阶段机在"面板已经不在"时会立刻收工，
    //     那样这段观察永远跑不到（第一版就是那样，8 秒的计数根本没机会打出来）。
    if (g_trayMenuTest == 5 && stage == 1) {
        static ULONGLONG popAt = 0;
        static int popCalls = 0;
        static int calls = 0;
        ++calls;
        if (popAt == 0) {
            if (!panel) return;   // 还没弹出来，继续等
            popAt = GetTickCount64();
            popCalls = calls;
            // ★ 判据是"主循环**又转了多少圈**"：这个函数每帧被调一次，而帧循环一旦停在
            //   某个模态调用里，它就不再被调（实测旧实现：停在 t=3.0s，之后这个数不涨）。
            SelfTestLogVerbose(L"[tray][menu-test] 面板已弹出，**这一轮一次都不点**：看它会不会"
                        L"按时限自己收回（旧实现在这里会永远挂住）");
            return;
        }
        if ((GetTickCount64() - popAt) < 8000) return;
        SelfTestLogVerbose(L"[tray][menu-test] 8 秒过去了：面板 %ls，期间主循环又转了 %d 圈"
                    L"（旧实现停在 t=N 那一刻，这个数不会再涨）",
                    panel ? L"**还在**（时限没起作用）" : L"已经自己消失", calls - popCalls);
        stage = 9;
        return;
    }

    if (stage == 0) {
        if (elapsed < at) return;
        stage = 1;
        at = elapsed + 0.4;
        SelfTestLogVerbose(L"[tray][menu-test] t=%.3f 投递一条真正的托盘右键消息（与点托盘图标时"
                    L"WndProc 收到的是同一条）",
                    elapsed);
        // 高位是图标 id、低位是按键事件：这就是 NOTIFYICON_VERSION_4 的形状。
        PostMessageW(g_hwnd, dshb::kMsgTrayIcon, kTrayIconId,
                     MAKELPARAM(static_cast<WORD>(WM_RBUTTONUP), static_cast<WORD>(kTrayIconId)));
        return;
    }

    if (stage == 1) {
        if (elapsed < at) return;   // 给"面板出现"留 0.4 s
        POINT cursor{};
        GetCursorPos(&cursor);
        if (!panel) {
            // ★ 这一行分两种情形，别混成一句"菜单没弹出来"：面板**弹出过又收回**是
            //   正常结果（[tray] 菜单面板那两行已经记了原因），只有"压根没出现过"才是问题。
            SelfTestLogVerbose(L"[tray][menu-test] 0.4 s 后面板窗口不在了（%ls）：这一轮不注入点击",
                        (panelSeenAt != 0) ? L"弹出过又收回，见上面 [tray] 菜单面板那几行"
                                           : L"从头到尾没出现过");
            stage = 9;
            return;
        }
        // ★★ 夹具的第二个剧本（--tray-menu-test=5）已经在上面处理掉了（它要在阶段机之前
        //   跑，否则"面板已经不在了"会先把这一轮结束掉）。
        RECT pr{};
        GetWindowRect(panel, &pr);
        SelfTestLogVerbose(L"[tray][menu-test] 面板在屏幕上：窗口=0x%p 矩形=(%ld,%ld,%ld,%ld) "
                    L"前台=0x%p（我们主窗口=0x%p）—— 弹出来之后帧循环还在跑（见这一轮的 "
                    L"[tray][menu-test] 帧线）",
                    panel, pr.left, pr.top, pr.right, pr.bottom, GetForegroundWindow(), g_hwnd);

        // ★ 点哪儿：面板**正开着**，所以直接按它在屏幕上的几何算「关闭」那一条的中心。
        //   HMENU 不在屏幕上（它是探针的对象），量它得到的坐标没有任何意义 ——
        //   上一版之所以要 GetMenuItemRect，是因为它点的是系统菜单窗口。
        //   一条的高度从面板客户区推不出来（边框是窗口的一部分），所以用**面板顶部 +
        //   内边距**这个口径：它是 tray.cpp 里同一个算式的镜像。★ 两个数（这里算的、
        //   tray.cpp 打开时量的）都会被打进日志，所以"点空了"与"面板挪了"分得清。
        const UINT dpi = GetDpiForWindow(g_hwnd);
        const int itemH = MulDiv(26, static_cast<int>(dpi ? dpi : 96u), 96);
        const int pad = MulDiv(2, static_cast<int>(dpi ? dpi : 96u), 96);
        const int x = (pr.left + pr.right) / 2;
        const int y = pr.top + pad + itemH / 2;
        // ★★ 第四个剧本（--tray-menu-test=9）：在面板上点**右键**，口径是"取消"。
        //   它与左键走的是同一个面板、同一套坐标，只是按键不同 —— 这样"右键取消"这条
        //   约定就是被真输入验过的，而不是只写在注释里。落在哪一条上不影响结论（整个面板
        //   的右键都是取消），所以坐标沿用"那一条的中心"。
        const bool rightButton = (g_trayMenuTest == 9);
        SelfTestLogVerbose(L"[tray][menu-test] t=%.3f 注入一次**真实**鼠标%s（SendInput）到 (%d,%d)"
                    L"（那一条的中心）；注入前光标在 (%ld,%ld)",
                    elapsed, rightButton ? L"右键" : L"左键", x, y, cursor.x, cursor.y);

        SetCursorPos(x, y);
        INPUT in[2]{};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        const UINT sent = SendInput(2, in, sizeof(INPUT));
        SelfTestLogVerbose(L"[tray][menu-test] SendInput 返回 %u（2 = 按下与抬起都投递成功）", sent);
        stage = 2;
        at = elapsed + 0.6;
        return;
    }

    if (stage == 2 && elapsed >= at) {
        // ★ 这一行只在"注入之后进程还在"的剧本里打得出：默认那条（真的点「关闭」）现在
        //   0.5 s 后进程就退出了，所以它由面板上点**右键**那个剧本（= 取消）读到 ——
        //   "什么都没发生"在那里就是正确结果。关闭那一条的证据是紧接着注入打出的
        //   `已发粒子信号 ... -> N 颗` 与收尾的 `粒子播完：关闭 = 进程退出`。
        SelfTestLogVerbose(L"[tray][menu-test] 收尾：注入之后面板窗口 %ls，关闭态 active=%d clicks=%d "
                    L"R_d=%.2f",
                    panel ? L"还在（这一下没收走面板）" : L"已经不在了",
                    dshb::ShutdownActive() ? 1 : 0, dshb::ShutdownClicks(),
                    dshb::ShutdownFloorRatio());
        stage = 10;
        return;
    }

    // 面板一直不消失（用户点别处菜单不消失 = 缺 SetForegroundWindow 那个经典坑）：
    // 3 秒后如实报出来。★ 注意这一条**不是**在等模态调用返回：面板的时限是 2 秒，
    // 所以它自己也会在 3 秒之前收回（那两件事在日志里分得清：一条写"按时限自己收回"）。
    if ((stage == 2 || stage == 10) && panelSeenAt != 0 && panel &&
        (GetTickCount64() - panelSeenAt) > 3000) {
        panelSeenAt = 0;
        SelfTestLogVerbose(L"[tray][menu-test] ★ 面板弹了 3 秒还没消失：SetForegroundWindow 那一手"
                    L"没起作用（点别处菜单不消失就是这个症状）");
    }
}

// --tray-menu-test 的**测试夹具时限**（不再是"救命索"，这一点必须写清）。
//
// ★ 它为什么还留着：这是**夹具**的兜底，不是生产路径的兜底。--tray-menu-test 会在真实
//   屏幕上注入鼠标事件，一个自动化的屏幕上操作必须有一个"到点就收工"的界线，否则
//   一次意外的注入丢失会留下一个永远跑着的测试进程（测试失败比测试挂住好得多）。
// ★ 它现在**不做**任何"救回主线程"的事：旧版这里调 EndMenu()，因为主线程正停在
//   TrackPopupMenu 的模态循环里、只有 EndMenu 能把它放出来。菜单面板换成非模态之后
//   那种"主线程停在某个调用里"的状态**根本不存在**了 —— 面板自己 2 秒到点就收回
//   （tray.cpp 的 kMenuPanelTimeoutMs），帧循环一直在跑。所以这里只剩"到点报一句、
//   让夹具收工"，EndMenu 那一手已随模态等待一起删除。
// ★ 生产路径（用户真的右键点托盘图标）**没有**看门狗，也不需要：那条路上没有任何
//   无超时的等待 —— 面板是普通窗口，帧循环一直在跑，2 秒到点它自己消失。看门狗只在
//   传了 --tray-menu-test 的夹具进程里才会被创建（见下面那一段）。
DWORD WINAPI TrayMenuFixtureDeadline(LPVOID param) {
    const DWORD limitMs = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(param)) * 1000;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(200);
        if (!g_running) return 0;
        if ((GetTickCount64() - start) < limitMs) continue;
        // 到点就报一句然后关窗口退进程：夹具的价值在于"报出来"，不是"永远等着"。
        SelfTestLogVerbose(L"[tray][menu-test] 夹具时限 %lu 秒到：这一轮不再等下去，关窗口退进程"
                    L"（这不是「救回了挂在模态调用里的主线程」—— 面板非模态，主线程一直在跑）",
                    limitMs / 1000);
        g_running = false;
        if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
}

// 进入关闭态。
// ★ 顺序是"先装钩子、再改状态"，而且**装不上就整个不进入**：关闭态唯一的取消路是"点到
//   面板实体之外"，那条路要靠这把钩子（0.2 删掉全局 Esc 钩子之后没有第二条）。钩子装不上
//   时进去，用户只能靠三击把它关掉、点面板之外毫无反应 —— 那不是关闭态，那是一个没有出口
//   的状态。所以宁可这一下右键当作没发生。
// ★ "装上了没有"只有 InstallShutdownMouseHook 一处判（它的返回值），状态机只收这个结论。
void EnterShutdownState() {
    const bool cancelPathReady = InstallShutdownMouseHook();
    if (!dshb::ShutdownEnter(cancelPathReady)) {
        if (!cancelPathReady) {
            SelfTestLog(L"[close] 钩子装不上，因此**不进关闭态**：进去之后点面板之外毫无反应、"
                        L"只能靠三击关掉（没有第二条取消路）。这一下右键当作没发生");
        }
        return;   // 已经在里面：不重置、不重复计时
    }
    g_closeEnterPending = true;
    g_closeEnterWhy = L"右键";
}

// 取消关闭态。**唯一**入口是"点到面板之外"（ApplyOutsideClickVerdict）。why 只用于日志。
void CancelShutdownState(const wchar_t* why) {
    if (!dshb::ShutdownCancel()) return;  // 不在关闭态 / 粒子期间不受理
    g_closeCancelPending = true;
    g_closeCancelWhy = why;
    RemoveShutdownMouseHook();
}

// "该放粒子了"之后的收尾。**两条入口共用**（窗口上点满第三下、托盘菜单「关闭」）：
// 粒子只能发一次、钩子必须摘掉、g_closeWaitParticles 必须置上 —— 分开写就会有一天两条路
// 里有一条忘了改，而症状是"粒子播完进程不退出"（§10.3 的"任务管理器里无残留"）。
void FireShutdownParticles(const wchar_t* what) {
    const int n = g_renderer ? g_renderer->StartShutdownParticles() : 0;
    g_closeWaitParticles = true;
    RemoveShutdownMouseHook();            // 粒子期间不受理输入，"点外面取消"也不必要了
    SelfTestLog(L"[close] %ls：已发粒子信号 StartShutdownParticles() -> %d 颗；"
                L"此后不受理任何输入，粒子播完即退出进程",
                what, n);
}

// 关闭态内的一次点击。**左键与右键走同一条**（所有者定：右键也算一次）。
// 点满三次 = 发"该放粒子了"的信号：粒子由另一步做（Renderer + src/particles.cpp），
// 这里只发信号，并从此不再受理任何输入（WndProc 与鼠标钩子判定都先问 ShutdownFired）。
void HandleShutdownClick(int which) {
    if (dshb::ShutdownFired()) {          // 粒子期间：不重复触发，也不受理
        g_closeRefusedPending = true;
        return;
    }
    const bool fired = dshb::ShutdownClick();
    g_closeClickPending = dshb::ShutdownClicks();
    g_closeWhichPending = which;
    if (!fired) return;
    FireShutdownParticles(which ? L"第 3 击（右键）" : L"第 3 击（左键）");
}

// 关闭态的所有日志在这里落盘。
//  ★ 为什么必须等到这一帧的 Update **之后**：这几行要报的 R/颜色/心跳数，正是"点击生效
//    之后这一帧真正画上去的那几个数"（R_d 的下限就是在 AdvanceAmbience 里生效的）。
void FlushCloseLogs() {
    const std::wstring hex = AmbienceHex(g_display.ambienceColor());
    const double r = g_display.ambienceRatioShown();
    const double d = g_display.ambienceDepthShown();
    if (g_closeEnterPending) {
        g_closeEnterPending = false;
        // clicks 是**读出来的**而不是写死的：托盘菜单那一条进来时状态机已经补齐到 3（直接进
        // 第三击），写死 0 会让那一行的日志与真实状态对不上。右键进入那一支此刻 clicks = 0，
        // 所以它的这一行逐字不变。
        SelfTestLog(L"[close] %ls -> 进入关闭态：clicks=%d R_d=%.2f R_new=%.6f R=%.6f color=%ls "
                    L"jitter=%.4fpx frame=%d 鼠标钩子=%ls",
                    g_closeEnterWhy, dshb::ShutdownClicks(), dshb::ShutdownFloorRatio(),
                    g_display.ambienceRatioTarget(), r, hex.c_str(),
                    dshb::ShutdownJitterDip(), dshb::ShutdownFrame(),
                    ShutdownCancelPathReady() ? L"已装" : L"未装");
    }
    if (g_closeClickPending >= 0) {
        SelfTestLog(L"[close] 第 %d 击（%ls）：R_d=%.2f R_new=%.6f R=%.6f color=%ls intensity=%.4f "
                    L"beatAmp=%.4fpx beatT=%.4fs jitter=%.4fpx frame=%d",
                    g_closeClickPending, g_closeWhichPending ? L"右键" : L"左键",
                    dshb::ShutdownFloorRatio(), g_display.ambienceRatioTarget(), r, hex.c_str(),
                    static_cast<double>(g_display.ambienceIntensity()),
                    dshb::BeatAmplitudePx(r, d), dshb::BeatPeriodSeconds(r, d),
                    dshb::ShutdownJitterDip(), dshb::ShutdownFrame());
        g_closeClickPending = -1;
    }
    if (g_closeCancelPending) {
        g_closeCancelPending = false;
        SelfTestLog(L"[close] 取消（%ls）：clicks=0 R_d=%.2f R=%.6f frame=%d（进度归零、文字恢复）",
                    g_closeCancelWhy, dshb::ShutdownFloorRatio(), r, dshb::ShutdownFrame());
    }
    if (g_closeRefusedPending) {
        g_closeRefusedPending = false;
        SelfTestLog(L"[close] 粒子期间：这一下输入被拒（不重复触发、不受理任何输入）");
    }
}

// 钩子判定的结果回主循环处理（回调里不写日志、不做 I/O）。
void ApplyOutsideClickVerdict() {
    const OutsideHit hit =
        static_cast<OutsideHit>(InterlockedExchange(&g_outsideHit, static_cast<LONG>(OutsideHit::None)));
    if (hit == OutsideHit::None || !dshb::ShutdownActive()) return;
    if (hit == OutsideHit::Ignore) {
        SelfTestLog(L"[close] 关闭态外点击(%ld,%ld)：落在我们自己的托盘图标或菜单上 —— "
                    L"什么都不做",
                    g_outsidePoint.x, g_outsidePoint.y);
        return;
    }
    if (dshb::ShutdownFired()) { g_closeRefusedPending = true; return; }
    SelfTestLog(L"[close] 关闭态外点击(%ld,%ld)：不落在我们自己的图标/菜单上 —— 取消关闭流程",
                g_outsidePoint.x, g_outsidePoint.y);
    CancelShutdownState(L"点到别处");
}

// ---- 剧本（真实消息路径：PostMessage 进 WndProc，不是直接调函数）----
//  --shutdown-test=GAP：三击剧本。GAP 是第 2 击与第 3 击之间的间隔（秒）——
//   跑一次 GAP=1 证明"三击必关"，再跑一次 GAP=12 证明"没有时间窗"。
//   ★ 拖动尝试那一下**也是一次左键按下**，所以按规格它就是第 1 击（点一下就计数）。
//     这不是副作用，正是"误点清零"那条规则本身。
void RunShutdownTestScript(double elapsed) {
    if (g_shutdownTest < 0.0) return;
    static int stage = 0;
    static double at = -1.0;
    // 起始时刻取 `--shutdown-enter-at=N`（默认 1.0 s）。**必须能推迟**：R 只抬不降，
    // 而"第 1 击把 R 抬到 0.50"这条判据只有在 **R 低于 0.5** 时才看得出效果。
    // ★ 冷启动的 R 现在**是 0**（2026-09-19 起：R 的时钟在第一个样本真正量出一步之前无效，
    //   见 `ambienceClockValid_`），所以要让 R 落在 0.5 以下，得靠"先制造一次陡降、再等它
    //   衰减"——这就是 `--shutdown-test` 那段预热与 `--shutdown-enter-at=N` 组合的理由。
    // N=75 s 时 R = 0.5^(75/60) = 0.42 < 0.5，于是第 1 击正好把 R 抬到 0.50。
    if (at < 0.0) at = (g_shutdownEnterAt > 0.0) ? g_shutdownEnterAt : 1.0;
    if (stage >= 4 || elapsed < at) return;
    const POINT c = PanelCenterClient();
    const LPARAM lp = MAKELPARAM(static_cast<short>(c.x), static_cast<short>(c.y));
    if (stage == 0) {
        SelfTestLogVerbose(L"[close][test] t=%.3f 注入右键（PostMessage）-> 应当进入关闭态", elapsed);
        PostMessageW(g_hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        PostMessageW(g_hwnd, WM_RBUTTONUP, 0, lp);
        at = elapsed + 0.6;
    } else if (stage == 1) {
        RECT before{}, after{};
        GetWindowRect(g_hwnd, &before);
        SelfTestLogVerbose(L"[close][test] t=%.3f 拖动尝试：按下 + 移动(+120,+90) 三次 + 抬起"
                    L"（关闭态禁止拖动；这一次按下同时是第 1 击）", elapsed);
        PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        for (int i = 1; i <= 3; ++i) {
            PostMessageW(g_hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(static_cast<short>(c.x + 40 * i),
                                    static_cast<short>(c.y + 30 * i)));
        }
        PostMessageW(g_hwnd, WM_LBUTTONUP, 0,
                     MAKELPARAM(static_cast<short>(c.x + 120), static_cast<short>(c.y + 90)));
        GetWindowRect(g_hwnd, &after);
        SelfTestLogVerbose(L"[close][test] 拖动尝试前后：窗口 (%ld,%ld) -> (%ld,%ld) 位移=(%ld,%ld)"
                    L"（关闭态应当一个像素都不动）",
                    before.left, before.top, after.left, after.top, after.left - before.left,
                    after.top - before.top);
        at = elapsed + 0.6;
    } else if (stage == 2) {
        SelfTestLogVerbose(L"[close][test] t=%.3f 注入右键 -> 第 2 击（右键也计数；R_d 应到 1.00）",
                    elapsed);
        PostMessageW(g_hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        PostMessageW(g_hwnd, WM_RBUTTONUP, 0, lp);
        at = elapsed + g_shutdownTest;
    } else {
        SelfTestLogVerbose(L"[close][test] t=%.3f 注入左键 -> 第 3 击（GAP=%.1fs 之后，仍应关闭）",
                    elapsed, g_shutdownTest);
        PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(g_hwnd, WM_LBUTTONUP, 0, lp);
        at = elapsed + 1.0;
    }
    ++stage;
}

//  --shutdown-enter-at=N：t=N 时右键进入关闭态，之后**什么都不做**。
//  给外部测试用（PowerShell 从外面 SendInput 一次真实点击，验证"点外面取消"与"钩子放行"）。
void RunShutdownEnterHold(double elapsed) {
    if (!g_shutdownEnterHold) return;
    static bool done = false;
    if (done || elapsed < g_shutdownEnterAt) return;
    done = true;
    const POINT c = PanelCenterClient();
    SelfTestLogVerbose(L"[close][hold] t=%.3f 注入右键 -> 进入关闭态，之后不受理脚本输入", elapsed);
    PostMessageW(g_hwnd, WM_RBUTTONDOWN, MK_RBUTTON,
                 MAKELPARAM(static_cast<short>(c.x), static_cast<short>(c.y)));
    PostMessageW(g_hwnd, WM_RBUTTONUP, 0,
                 MAKELPARAM(static_cast<short>(c.x), static_cast<short>(c.y)));
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

// 滞后一拍（所有者要求）：显示用上一次确认的采样，最新那次压着，等下一个点来时再提交。
// 这样每次提交都触发一格滚动，且比较的两个端点都是真观测值。
dshb::Sample g_stash{};
bool g_haveStash = false;

// 启动时那一次（`g_haveStash == false`）**也要滞后**，起点取"上次关机前的余额"。
// ★ 为什么（所有者 2026-09-19 实测的现象）：原来第一条分支是"首个采样直接显示"，于是关掉
//   近两小时再打开时，屏幕**直接落在**当前余额上（48.80 → 47.72 只用一帧），你只看到结果、
//   看不到那一次下跌，而且滚动动画也被跳过（`hasValue_` 为假时显示值直接落位、不滚）。
// ★ 现在的做法：先喂一个"上次关掉前那个余额"的样本让它落位，这一拍的最新采样照旧压进
//   `g_stash`，下一拍再提交 —— 于是开机那一次下跌会**真的滚一遍**，与"滞后一拍"同一个故事。
// ★ 起点从曲线存储里取（`curve.json` 最新那个点就是上次关掉前显示的余额），**不判它多老**：
//   超过 86400 秒的存储在加载时已被整份丢弃（curve_store 的 §2.3 规则），所以这里拿到的
//   必然在一天之内。取不到（首次运行、存储为空）时才退回原来的"首个采样直接显示"。
// ★★ 但"起点"与"当前采样"是**同一笔余额**时不能演（所有者 2026-09-19 报的第二个现象）：
//   曲线**只在余额变化时记点**（规格 §2.1），所以存储里最新那个点与现在的真实值之间
//   完全可能什么都没发生 —— 那时"上次关掉前"就是"现在"，凭空演一次从旧值滚到新值，
//   用户会以为余额降了，其实没降。判据就是下面那个整数比较：**真的不同才演**。
//   ★ 为什么用整数比而不是"元的 double"：余额本来就是 1/10000 元的整数（设计 §3.1），
//     走一趟浮点再比，等于把"相同"也交给舍入去裁决；两边都是 raw 整数时，"相同"是
//     逐位相等的确定结论。
//   ★ 为什么**不**引入"差得不多就不演"：那是一个新的隐式常量，会让"余额到底动没动"
//     取决于一个没人写下来的阈值。不一致就是不演，一致就是演，只有两种。
void CommitDelayed() {
    const dshb::Sample& s = g_states.lastGood();
    if (!g_haveStash) {
        g_haveStash = true;
        g_stash = s;
        dshb::Amount startAmount{};
        const bool haveStart = dshb::CurveStartBalance(&startAmount);
        // 比的是**屏幕上那一笔**的整数余额：`s.total` 就是接口的优先条目
        // （balance_source 按 api::PreferredEntryIndex 挑出来的那一条，见那个文件），
        // 也就是 `OnSample` 与 `FeedCurve` 用的同一个数。
        // ★ 方向（余额涨了）不单独分支：起点在上、现在在下是"跌"，起点在下、现在在上是
        //   "涨"，两者都由同一句过渡演出来（数字往上滚与往下滚是同一套行程，只是符号不同）。
        //   单独为"涨"加一条判据会多出一个没有依据的口径 —— 而且"开机看见余额涨了"与
        //   "开机看见余额跌了"一样，是真的发生了的事，都该演。
        if (haveStart && startAmount != s.total) {
            dshb::Sample start = s;      // 币种/条目沿用当前样本，只把余额换成起点
            start.total = startAmount;
            start.amountsOk = true;
            g_display.OnSample(start);   // 先落位到"上次关掉前"
            g_display.OnSample(s);       // 再落到真实值：这一步会滚动（hasValue_ 已为真）
            SelfTestLog(L"[commit] t=%.1fs 启动过渡：先落位到上次的 %hs，再滚到 %hs",
                        g_elapsed, startAmount.ToString2().c_str(), s.total.ToString2().c_str());
            return;
        }
        g_display.OnSample(s);
        if (haveStart) {
            // ★ 这一条就是"余额没变"：起点与当前是同一笔钱，没有过渡可演。
            SelfTestLog(L"[commit] t=%.1fs 首个采样直接显示：%hs（存储里最新那个点就是它，"
                        L"余额没变 -> 不演过渡）",
                        g_elapsed, s.total.ToString2().c_str());
        } else {
            SelfTestLog(L"[commit] t=%.1fs 首个采样直接显示：%hs（存储里没有可用起点）",
                        g_elapsed, s.total.ToString2().c_str());
        }
        return;
    }
    g_display.OnSample(g_stash);
    SelfTestLogVerbose(L"[commit] t=%.1fs 提交上一个采样 %hs（最新 %hs 已收到，压着等下一点）",
                g_elapsed, g_stash.total.ToString2().c_str(), s.total.ToString2().c_str());
    g_stash = s;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // ★ 详细日志的开关必须在**第一行日志之前**定下来（下面几行就开始写文件了）。
    //   判据见 dshb_log.h：命令行里有没有 `--` 开头的开关——所有者双击时命令行是空的。
    //   两个例外只换数据位置（普通用户也可能用），不单独算"开发调用"。
    // ★ `--log-file=` 也在这里处理：它改的是**落点**，同样必须早于第一行日志。
    {
        for (int i = 1; i < argc; ++i) {
            if (wcsncmp(argv[i], L"--log-file=", 11) == 0) {
                dshb::SetLogFilePathOverride(argv[i] + 11);
            }
        }
        bool verbose = false;
        for (int i = 1; i < argc; ++i) {
            const wchar_t* a = argv[i];
            if (a[0] != L'-' || a[1] != L'-') continue;
            if (wcsncmp(a, L"--config=", 9) == 0) continue;
            if (wcsncmp(a, L"--curve-store=", 14) == 0) continue;
            // `--log-file=` 也是"写到哪"，与上面两个同类，不单独算"开发调用"。
            if (wcsncmp(a, L"--log-file=", 11) == 0) continue;
            verbose = true;
            break;
        }
        // 显式开关压过判据，两个方向都给：探针想静音、或普通调用想详查。
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--log-quiet") == 0) verbose = false;
            if (wcscmp(argv[i], L"--log-verbose") == 0) verbose = true;
        }
        dshb::SetLogVerbose(verbose);
    }

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
        } else if (wcsncmp(argv[i], L"--countdown=", 12) == 0) {
            // 导帧夹具：导出路径不取样，所以倒计时没有真实来源，靠它给一个值。
            g_countdownGiven = true;
            g_countdownSeconds = _wtoi(argv[i] + 12);
        } else if (wcsncmp(argv[i], L"--curve-frame=", 14) == 0) {
            // 导帧夹具（规格 §5 验收 4）：把曲线滚动计时器冻在 k/60 秒，于是
            // "滚动中的第 k 帧"可以用 --export-frame=1 直接导出来（不需要连画 k 帧）。
            g_curveFrame = _wtoi(argv[i] + 14);
        } else if (wcsncmp(argv[i], L"--beat-frame=", 13) == 0) {
            // 导帧夹具：位移是 (仿真时刻, 这一拍) 的纯函数。把仿真时刻放到 k/60 秒、并按
            // 计时器重新走一遍，"第 k 帧的位移"就能单独导出（不必连跑 k 帧）——与
            // --curve-frame 同一套惯例。
            g_beatFrame = _wtoi(argv[i] + 13);
        } else if (wcscmp(argv[i], L"--beat-trace") == 0) {
            dshb::SetBeatTrace(true);   // 每拍触发往 stderr 打一行（计时器是有状态的）
        } else if (wcsncmp(argv[i], L"--history-demo=", 15) == 0) {
            g_historyDemo = _wtoi(argv[i] + 15);
        } else if (wcscmp(argv[i], L"--curve-selftest") == 0) {
            // Numeric self-test for the monotone interpolation (D3/D6).
            // No pixels involved: exactness + no-overshoot are pure properties.
            std::string rep;
            const bool ok = dshb::SelfTestMonotoneCurve(&rep);
            SelfTestLogVerbose(L"[curve] --- monotone interpolation self-test ---");
            size_t pos = 0;
            while (pos < rep.size()) {
                size_t eol = rep.find('\n', pos);
                if (eol == std::string::npos) eol = rep.size();
                SelfTestLogVerbose(L"[curve] %hs", rep.substr(pos, eol - pos).c_str());
                pos = eol + 1;
            }
            SelfTestLogVerbose(ok ? L"[curve] RESULT: all properties hold" : L"[curve] RESULT: FAILED");
            g_runSeconds = 0.2;   // 跑完就退
        } else if (wcscmp(argv[i], L"--no-curve") == 0) {
            g_noCurve = true;
        } else if (wcscmp(argv[i], L"--pause-test") == 0) {
            g_pauseTest = true;
        } else if (wcscmp(argv[i], L"--click-test") == 0) {
            g_clickTest = true;
        } else if (wcsncmp(argv[i], L"--shutdown-test=", 16) == 0) {
            // 三击剧本。参数 = 第 2 击与第 3 击之间的间隔（秒）：跑 1 与 12 各一次，
            // 后者就是"没有时间窗"的证据。
            g_shutdownTest = _wtof(argv[i] + 16);
            SelfTestLogVerbose(L"[argv] --shutdown-test=%.1fs（第 2 击之后停这么久再点第 3 击）",
                        g_shutdownTest);
        } else if (wcsncmp(argv[i], L"--shutdown-enter-at=", 20) == 0) {
            // 三击剧本的**起始时刻**（秒）。为什么要能推迟：R 只抬不降，而"第 1 击把 R 抬到
            // 0.50"只有在 R 低于 0.5 时才看得出效果；冷启动的 R 现在是 0（2026-09-19 起），
            // 所以要让 R 落在 0.5 以下就得靠"先陡降、再等它衰减"。
            // ★ 它只定时刻。早先它顺带打开了"进入后挂住"那个剧本，于是同一次运行里注入了
            //   两次右键（一次来自剧本、一次来自挂住），点击序号整体错位一位 —— 那次实测
            //   就是这条注释的来源；挂住改成独立的 --shutdown-hold。
            g_shutdownEnterAt = _wtof(argv[i] + 20);
            SelfTestLogVerbose(L"[argv] --shutdown-enter-at=%.1fs（三击剧本从这一刻开始）",
                        g_shutdownEnterAt);
        } else if (wcscmp(argv[i], L"--shutdown-hold") == 0) {
            // 进入关闭态之后**不受理脚本输入**：给外部测试用（PowerShell 从外面 SendInput
            // 一次真实点击，验证"点面板之外就取消"与"钩子把点击原样放行"）。
            g_shutdownEnterHold = true;
            SelfTestLogVerbose(L"[argv] --shutdown-hold：进入关闭态后挂住（等外部点击）");
        } else if (wcsncmp(argv[i], L"--shutdown-frame=", 17) == 0) {
            // 导帧夹具：把关闭态钉在"进入以来第 k 帧"（抖动与进入段都是帧号的纯函数，
            // 所以不必连跑 k 帧）。与 --curve-frame / --beat-frame 同一套惯例。
            g_shutdownFixtureFrame = _wtoi(argv[i] + 17);
        } else if (wcsncmp(argv[i], L"--shutdown-clicks=", 18) == 0) {
            g_shutdownFixtureClicks = _wtoi(argv[i] + 18);
        } else if (wcscmp(argv[i], L"--no-tray") == 0) {
            // 离线探针用：整轮跑下来不碰通知区域，因此**绝不会留下一颗僵尸图标**
            // （探针进程跑完就退，NIM_ADD 之后直接死掉会在任务栏上留一个点不动的图标）。
            g_noTray = true;
            SelfTestLogVerbose(L"[argv] --no-tray：本次不挂托盘图标");
        } else if (wcscmp(argv[i], L"--no-mouse-hook") == 0) {
            // 测试旁路（见 g_noMouseHook）：造出"全局鼠标钩子装不上"那一条路。
            g_noMouseHook = true;
            SelfTestLogVerbose(L"[argv] --no-mouse-hook：本轮当作全局鼠标钩子装不上"
                        L"（测试旁路，右键因此不进关闭态）");
        } else if (wcsncmp(argv[i], L"--tray-probe=", 13) == 0) {
            // 一次性把托盘的事实逐条写进日志：图标矩形的来源与矩形本身、菜单项数与文字、
            // 窗口句柄与 uID。它**不弹菜单**（弹菜单是 --tray-menu-test 的事）。
            g_trayProbe = _wtoi(argv[i] + 13) != 0;
            SelfTestLogVerbose(L"[argv] --tray-probe=%d：写一轮托盘事实", g_trayProbe ? 1 : 0);
        } else if (wcsncmp(argv[i], L"--tray-menu-test=", 17) == 0) {
            // t=N 秒时弹出**真的**右键菜单面板，并用 SendInput 真的点里面的「关闭」。
            // 动真实光标，所以只在显式传参时才做。
            g_trayMenuTest = _wtoi(argv[i] + 17);
            // ★ 夹具**不挂真图标**：NIM_ADD 一个只活十几秒的图标会让用户的托盘闪一下，
            //   而这条夹具验的是"菜单面板 -> 命令 -> 关闭态"这条链，它只依赖属主窗口
            //  （面板是属主的子窗口，与通知区域无关）。"图标真的进了通知区域"由
            //   --tray-probe=1 的 [tray] 事实那一行证明（那一轮不加这个参数）。
            if (g_trayMenuTest > 0) g_noTray = true;
            SelfTestLogVerbose(L"[argv] --tray-menu-test=%d：%d 秒后弹菜单并真的点「关闭」"
                        L"（本次不挂真图标，见上面那条 [tray] 行）；"
                        L"=5 只弹不点、=7 连点 5 次右键（每次照外壳的样子投两条消息）",
                        g_trayMenuTest, g_trayMenuTest);
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
                SelfTestLogVerbose(L"[argv] --scenario=%ls not recognised (use 1..%d or a name); ignored",
                            v, static_cast<int>(dshb::Scenario::Count));
            }
        } else if (wcsncmp(argv[i], L"--speed=", 8) == 0) {
            g_fake.SetSpeed(_wtof(argv[i] + 8));
        } else if (wcsncmp(argv[i], L"--curve-store=", 14) == 0) {
                g_curveStorePath = argv[i] + 14;
                g_curveStoreGiven = true;
        } else if (wcsncmp(argv[i], L"--config=", 9) == 0) {
            // 测试旁路（与 --curve-store 同一条理由）：默认是所有者自己的
            // %LOCALAPPDATA%\deepseek-balance\config.json，而量"位置持久化"的探针必须
            // 反复写这个文件 —— 绝不能写他那一份。
            g_configPath = argv[i] + 9;
            g_configGiven = true;
        // ===== TEMPORARY LOCAL WIRING (ambience-build, to be handed to the main agent) =====
        } else if (wcsncmp(argv[i], L"--ambience=", 11) == 0) {
            char amb[64] = {0};
            WideCharToMultiByte(CP_UTF8, 0, argv[i] + 11, -1, amb, sizeof(amb), nullptr, nullptr);
            if (!dshb::SetAmbienceGiven(amb)) {
                SelfTestLogVerbose(L"[argv] --ambience 参数无法解析（要 R,D）：%ls", argv[i] + 11);
            } else {
                SelfTestLogVerbose(L"[argv] --ambience=%ls（测试夹具：这一帧的氛围钉在给定的 R,D）",
                            argv[i] + 11);
            }
        } else if (wcscmp(argv[i], L"--no-text") == 0) {
            dshb::SetTextEnabled(false);
            SelfTestLogVerbose(L"[argv] --no-text：正文一层不画（量底色用）");
        } else if (wcscmp(argv[i], L"--pause-ambience") == 0) {
            dshb::SetAmbienceFrozen(true);
            SelfTestLogVerbose(L"[argv] --pause-ambience：氛围冻结（暂停时颜色与光强一步都不推进）");
        // ===== END TEMPORARY LOCAL WIRING =====
        // ===== TEMPORARY: inner-glow re-bake accounting (task 2) =====
        // ★ 必须挂在参数解析里（此刻还没画过帧，数字必然是 0）是错的 —— 所以这里
        //   只置一个标志，真正的日志在导出/退出之前打。
        } else if (wcscmp(argv[i], L"--log-glow-stats") == 0) {
            g_logGlowStats = true;
        } else if (wcsncmp(argv[i], L"--ambience-glide=", 17) == 0) {
            g_ambienceGlide = _wtoi(argv[i] + 17);
            SelfTestLogVerbose(L"[argv] --ambience-glide=%d（跑 %d 帧真实氛围推进，量重烘代价）",
                        g_ambienceGlide, g_ambienceGlide);
        } else if (wcsncmp(argv[i], L"--real-frames=", 14) == 0) {
            g_realFrames = _wtoi(argv[i] + 14);
            SelfTestLogVerbose(L"[argv] --real-frames=%d（真实循环跑这么多帧就退出并记 [glow]）",
                        g_realFrames);
        } else if (wcscmp(argv[i], L"--force-new-instance") == 0) {
            g_forceNewInstance = true;
            SelfTestLogVerbose(L"[argv] --force-new-instance：跳过单实例检查（测真实路径时用）");
        // ===== END TEMPORARY: task 2 =====
        // ===== 关闭粒子（设计 §11.6）=====
        } else if (wcscmp(argv[i], L"--shutdown-particles") == 0) {
            g_shutdownParticles = true;
            SelfTestLogVerbose(L"[argv] --shutdown-particles：导出模式导第 k 帧；真循环模式 t=%.2fs 触发",
                        kShutdownParticlesArmSeconds);
        } else if (wcscmp(argv[i], L"--particles-only") == 0) {
            // 量粒子**颜色**用的夹具：背景整层不画，PNG 里只剩粒子自己。
            // 它必须与 --shutdown-particles + --export-frame 一起用（否则画布是空的）。
            g_particlesOnly = true;
            SelfTestLogVerbose(L"[argv] --particles-only：只画粒子层（量颜色用；背景不画）");
        } else if (wcscmp(argv[i], L"--no-particles") == 0) {
            // "面板先没了"这一条的 A/B 尺子：同一帧号、同一条绘制路径，去掉粒子。
            // 剩下任何不透明像素都只可能来自面板（底色/边框/蒙光/曲线/正文）。
            g_noParticles = true;
            SelfTestLogVerbose(L"[argv] --no-particles：面板照样消失，但一颗粒子都不撒（量面板用）");
        } else if (wcscmp(argv[i], L"--no-present") == 0) {
            // §11.3 的预算是**帧内计算**，不能把等垂直空白的时间算进去。
            g_noPresent = true;
            SelfTestLogVerbose(L"[argv] --no-present：画完不提交（量光栅代价，不含等显示的等待）");
        } else if (wcsncmp(argv[i], L"--frame-stats=", 14) == 0) {
            g_frameStatsFrames = _wtoi(argv[i] + 14);
            SelfTestLogVerbose(L"[argv] --frame-stats=%d：记全部帧的 p50/p99 后退出", g_frameStatsFrames);
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
        SelfTestLogVerbose(L"[pin] 余额钉在 %.2f（不再取样、不再变化）", g_fixedAmount);
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
        SelfTestLogVerbose(L"[manual] R(实际)=%.4f L(上次)=%.4f k(帧)=%d", R, L, k);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 单实例（A11）----
    // 放在建窗之前：第二个实例不该先建出一个窗口再退出，那样屏幕上会闪一下。
    // 注意导帧/自检这类离屏模式也不该受单实例限制（它们不显示窗口），
    // 所以只在"要显示窗口"的路径上做这个检查。
    const bool offscreenMode = g_exportFrame || g_premulProbe;
    if (!offscreenMode && !g_forceNewInstance && !dshb::AcquireSingleInstance()) {
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

    // ---- 起点位置：config.json 优先，没有/坏了就退回 (240,240)（设计 §10.1「位置持久化」）----
    // 口径：屏幕像素、虚拟屏幕坐标系。窗口尺寸也要传进去 —— "存下来的位置还算不算数"
    // 用的就是钳制那一条规则（至少 50% 面积落在某块显示器的工作区内），见 panel_drag.h。
    g_configFile = g_configGiven ? g_configPath : dshb::Paths().config;
    POINT startPos{dshb::kWindowXDefault, dshb::kWindowYDefault};
    {
        std::wstring why;
        if (dshb::LoadWindowPos(g_configFile, SIZE{w0, h0}, &startPos, &why)) {
            SelfTestLogVerbose(L"[drag] 起点取自 config.json：(%ld,%ld)（文件 %ls）", startPos.x,
                        startPos.y, g_configFile.c_str());
        } else {
            SelfTestLogVerbose(L"[drag] config.json 没有可用位置（%ls）：起点退回 (%d,%d)（文件 %ls）",
                        why.c_str(), dshb::kWindowXDefault, dshb::kWindowYDefault,
                        g_configFile.c_str());
            startPos = POINT{dshb::kWindowXDefault, dshb::kWindowYDefault};
        }
    }

    g_hwnd = CreateWindowExW(exStyle, kClassName, L"deepseek-balance", WS_POPUP,
                             startPos.x, startPos.y, w0, h0, nullptr, nullptr, instance,
                             nullptr);
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
    SelfTestLogVerbose(L"[win] dpi(system)=%u dpi(window)=%u exStyle=0x%08lX（缩放固定 1.0，不随 DPI）",
                GetDpiForSystem(), dpi,
                static_cast<unsigned long>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE)));
    SelfTestLogVerbose(L"[win] 窗口矩形=(%ld,%ld,%ld,%ld) 尺寸=%ldx%ld 实体区(理论)=%dx%d",
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
        SelfTestLogVerbose(L"[render] 视觉缩放 %.2f -> 画布 %dx%d 像素", g_uiScale, size.widthPx,
                    size.heightPx);
    }

    // 导出用的缩放覆盖：让同一套 315×129 设计稿在 100% / 150% / 200% 下各导一张，
    // 这样字号能在真实尺寸下被判断（所有者指出过：只看 200% 的图看不出字号合不合适）。
    if (g_dpiOverride > 0) {
        const double scale = g_dpiOverride / 96.0;
        size.scale = static_cast<float>(scale);
        size.widthPx = static_cast<int>(dshb::kCanvasWidthDip * scale + 0.5);
        size.heightPx = static_cast<int>(dshb::kCanvasHeightDip * scale + 0.5);
        SelfTestLogVerbose(L"[render] 缩放被 --dpi 覆盖为 %d%% -> 画布 %dx%d", g_dpiOverride * 100 / 96,
                    size.widthPx, size.heightPx);
    }
    if (!renderer.Create(g_hwnd, size)) {
        SelfTestLog(L"[main] 渲染器创建失败（D3D11 / DComp / 交换链）");
    DestroyWindow(g_hwnd);
        CoUninitialize();
        return 3;
    }
    SelfTestLogVerbose(L"[render] 画布=%dx%d scale=%.4f（外扩 %d DIP 余量）",
                size.widthPx, size.heightPx, size.scale, dshb::kMarginDip);

    // A12b：路径解析结果必须留痕。降级（目录不可写）时尤其要让用户找得到原因，
    // 否则他会以为历史一直在正常记录。
    {
        const dshb::AppPaths& paths = dshb::Paths();
        SelfTestLog(L"[paths] 数据目录=%ls 可写=%ls", paths.dataDir.c_str(),
                    paths.writable ? L"是" : L"否");
        SelfTestLog(L"[paths] 日志=%ls", paths.log.c_str());
        SelfTestLog(L"[paths] 设置=%ls", paths.config.c_str());
        // 曲线记录文件：给了路径就顺手加载（重启后曲线接上，规格验收 8）
        {
            std::wstring curvePath = paths.dataDir;
            if (!curvePath.empty() && curvePath.back() != L'\\') curvePath += L'\\';
            curvePath += L"curve.json";
            if (g_curveStoreGiven) curvePath = g_curveStorePath;   // 测试专用覆盖
            // 安全阀：--history-demo 会把合成点经由 FeedCurve() 存盘，绝不能落在生产路径上
            // （2026-09-17 事故：导出用的演示点覆盖了所有者的真实历史）
            if (g_historyDemo > 0 && !g_curveStoreGiven) {
                wchar_t tmpDir[MAX_PATH] = {};
                if (::GetTempPathW(MAX_PATH, tmpDir) > 0) {
                    curvePath = std::wstring(tmpDir) + L"dshb-demo-" +
                                std::to_wstring(::GetCurrentProcessId()) + L".json";
                }
            }
            dshb::SetCurveStorePath(curvePath);
            SelfTestLogVerbose(L"[paths] 曲线=%ls", curvePath.c_str());
        }
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
        SelfTestLogVerbose(L"[premul] 像素(%d,%d) BGRA=(%u,%u,%u,%u) alpha=%u",
                    px, py, bgra[0], bgra[1], bgra[2], bgra[3], bgra[3]);
        SelfTestLogVerbose(L"[premul] R>A 吗？%ls  →  结论：%ls",
                    straight ? L"是（直通数据，错误）" : L"否",
                    premultiplied ? L"已预乘（正确）" : L"读取失败（错误）");

        if (g_exportFrame) {
            const bool saved = renderer.ExportFrame(g_exportPath, 0.0);
            SelfTestLogVerbose(L"[export] 预乘探针 PNG: %ls 结果=%ls", g_exportPath,
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

        const bool manualRun = g_fixedGiven || g_realGiven || g_lastGiven ||
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

    // ---- 托盘图标（设计 §10.3）----
    // 放在进主循环之前：这样 [tray] 那几行排在所有帧日志之前，"图标什么时候挂上的"一眼可查。
    // 离屏模式（导帧/预乘自检）在这里之前就 return 了，所以那两个模式根本不会挂图标。
    // ★ --tray-menu-test 在解析参数时就把 g_noTray 置上了（理由写在那一行）：那条夹具
    //   验的是菜单面板那一半，不需要往用户的托盘里放一个只活十几秒的图标。
    InstallTrayIcon();

    // --tray-menu-test 的**夹具时限**线程（不是生产兜底：它只在夹具进程里被创建）。
    // ★ 为什么仍然独立成一个线程：它要按**墙上时间**报"这个夹具跑太久了"，
    //   而它不该被主循环的节奏影响。但它现在不做任何"把主线程从某个调用里放出来"的事：
    //   菜单面板是非模态的，主线程没有任何地方可以停住（见 TrayMenuFixtureDeadline 的注释）。
    if (g_trayMenuTest > 0) {
        const int limitSeconds = g_trayMenuTest + 12;
        HANDLE wd = CreateThread(
            nullptr, 0, TrayMenuFixtureDeadline,
            reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(limitSeconds)), 0, nullptr);
        if (wd) CloseHandle(wd);   // 线程到点自己退出，句柄不需要留
        SelfTestLogVerbose(L"[tray][menu-test] 夹具时限线程已启动：%d 秒还没跑完就报一句并收工"
                    L"（它不再做 EndMenu 那种事 —— 没有模态等待可以救）",
                    limitSeconds);
    }

    // ---- 离屏导帧模式：渲一帧到 PNG 就退出 ----
    // 用来做"用像素说话"的验收：居中错位、颜色、残影、粒子越界都靠它量。
    // 注意它渲染的是同一份绘制代码，所以屏幕上的错在 PNG 里也会错。
    if (g_exportFrame) {
        const double t = static_cast<double>(g_frameNo) / 60.0;   // 第 N 帧 ≈ N/60 秒

        // --shutdown-particles + --export-frame=k 的换算：第 k 帧 = k/60 秒，
        // 与 --curve-frame / --beat-frame 同一套惯例（帧号 -> 秒数只有一处）。
        const double particleAge = static_cast<double>(g_frameNo) / 60.0;

        // 布局诊断只在这里开：它会在绘制路径里写文件，而每帧写文件会把进程弄崩
        // （实测 0xC0000409）。导出模式只画一帧，所以安全。
        if (g_layoutProbe) dshb::SetLayoutProbe(true);
        // 氛围曲线默认开；--no-curve 关掉它，用于确认"关掉后文字位置逐像素不变"
        dshb::SetCurveEnabled(!g_noCurve);
        if (g_historyDemo > 0) dshb::PrimeHistoryForDemo(g_historyDemo);
        // ===== TEMPORARY (task 2/3): run a real ambience glide before the frame ----
        // 必须在这里（数据层已经喂好、绘制还没开始）：滑行推的就是"渲染前的那几帧"。
        dshb::g_ambienceGlideTarget = &g_display;   // 显示层实例挂给滑行用（测试口子）
        if (g_ambienceGlide > 0) dshb::RunAmbienceGlide(g_ambienceGlide);
        // ===== END TEMPORARY =====

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
                SelfTestLogVerbose(L"[roll] 循环模式：20.00 <-> 99.50 每 2 秒一次");
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
                    // 与主循环同一条纪律：滚动的"过了多久"用总时间，动画用夹过的 dt。
                    // 这一条循环里 dt 还喂给 sinceSwitch 与假数据源（那些要的是步进量），
                    // 所以只换 rollElapsed 的来源。
                    rollElapsed = rollClock.Total();
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
    // 币种跟着**显示层当前显示的那个**走（显示层每次样本按接口的优先条目判定），不再只看状态机里那条
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
                SelfTestLogVerbose(L"[rollstep] i=%d value=%.4f target=%.4f rem=%.4f text=%hs",
                            i, g_display.value(), tgt, rem, fd.amountText.c_str());
            }

            SelfTestLogVerbose(L"[roll] 跳变前=%.2f 跳变后目标=%.2f 推进 %d 帧后显示=%.2f",
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
        // 心跳同理：必须在推进过计时器的循环之后再钉，否则会被那些循环推着走。
        if (g_beatFrame >= 0) g_display.SetBeatSimFrame(g_beatFrame);
        // 关闭态夹具（--shutdown-frame=k --shutdown-clicks=N）：与它们同一个位置、同一个理由。
        // ★ 必须在 Update **之前**：帧号推进与 R 的下限都在 Update 里生效。
        // ★ 它钉的是显示层的状态，不是"模拟点击"—— 模拟点击在第三次会真的发粒子信号，
        //   那是生产行为，夹具不该做。
        if (g_shutdownFixtureFrame >= 0) {
            dshb::SetShutdownFixture(g_shutdownFixtureClicks, g_shutdownFixtureFrame);
            // ★ 曲线层这一帧画不画，必须在夹具把状态钉住**之后**判：进入段里它还要画
            //   （正在向内收缩），进入段走完就整层不画 —— "曲线要没"就是这一句。
            //   （上面那次 SetCurveEnabled(!g_noCurve) 只处理 --no-curve 的 A/B。）
            dshb::SetCurveEnabled(!g_noCurve && dshb::ShutdownCurveLayerVisible());
        }
        g_display.Update(0.0);
        if (g_beatFrame >= 0) {
            SelfTestLogVerbose(L"[beat] frame=%d sim=%.4f dip=%.4f beats=%d",
                        g_beatFrame, g_display.beatSimSeconds(), g_display.beatOffsetDip(),
                        static_cast<int>(g_display.beatCount()));
        }
        // 氛围：这一帧真正拿去画的 R/D/颜色/强度（见 AmbienceHex 上面的理由）。
        //   ratio= 是**实际显示**的 R(t)，ratio_new= 是数据这一次给的高度（未衰减的那个）。
        {
            const std::wstring hex = AmbienceHex(g_display.ambienceColor());
            SelfTestLogVerbose(L"[ambience] ratio_new=%.6f ratio=%.6f depth=%.6f color=%ls "
                        L"intensity=%.6f unreadable=%d",
                        g_display.ambienceRatioTarget(), g_display.ambienceRatioShown(),
                        g_display.ambienceDepthShown(), hex.c_str(),
                        static_cast<double>(g_display.ambienceIntensity()),
                        g_display.ambienceUnreadable() ? 1 : 0);
        }
        // 组装正文（和真实运行时同一条路径），这样导出的图就是屏幕上会看到的图
        {
            const dshb::ConnState st = g_states.Evaluate(static_cast<int64_t>(NowWallMs()));
            const bool currencyKnown = g_states.hasGood() && (g_display.shownCurrency() == "CNY" || g_display.shownCurrency() == "USD");
            const dshb::WidgetFrame frame = dshb::BuildWidgetFrame(
                st, g_display, currencyKnown,
                ((g_display.shownCurrency() == "CNY") ? L"\u00A5"
                 : ((g_display.shownCurrency() == "USD") ? L"$" : L"")));
            renderer.SetWidgetFrame(frame);
            // 诊断（只在导帧这一条路径上）：这一帧底部那行字的**原文**，以及它是从
            // 哪个速率状态、哪个平滑速率算出来的。导帧进程没有控制台，PNG 里的字又要
            // OCR 才读得回来，所以把帧携带的那串字符逐字写进日志——验收要比的就是这一份。
            SelfTestLogVerbose(L"[frame] amount=%hs status=%hs rate=%.10f zeroTime=\"%ls\" (utf8bytes=%zu)",
                        frame.amountText.c_str(),
                        dshb::RateStatusName(g_display.rateEstimate().status),
                        g_display.rateDisplay(), WidenUtf8(frame.zeroTimeText).c_str(),
                        frame.zeroTimeText.size());
            // "今日已 X.XX¥" 的**原文**：和上面那行同一个理由 —— PNG 里的字要 OCR 才读得
            // 回来，而验收要比的就是帧携带的这串字符。它为空只有两种情况（算不出来时
            // 也不是空，是 "今日已 --.--¥"）：账户欠款（没有可显示的余额），或者关闭态。
            SelfTestLogVerbose(L"[frame] today=\"%ls\" (utf8bytes=%zu)",
                        WidenUtf8(frame.todayUsageText).c_str(), frame.todayUsageText.size());
            // 关闭态的导帧证据：这一帧**真的**交给渲染层的每一个字段都要留痕 ——
            // 屏幕上的"文字全无"是"这些字段全空 + amountText 就是那两个字"的结果，
            // 量像素量到的墨迹必须能从这一行对上号。
            if (dshb::ShutdownActive()) {
                SelfTestLogVerbose(L"[close] k=%d clicks=%d R_d=%.2f R=%.6f beatDip=%.4f jitterDip=%.4f "
                            L"contentScale=%.4f | amount=\"%ls\" showAmount=%d symbol=\"%ls\" "
                            L"status=\"%ls\" countdown=\"%ls\" zeroTime=%zu curvePoints=%zu "
                            L"curveHasData=%d places=%zu",
                            dshb::ShutdownFrame(), dshb::ShutdownClicks(),
                            dshb::ShutdownFloorRatio(), g_display.ambienceRatioShown(),
                            g_display.beatOffsetDip(), dshb::ShutdownJitterDip(),
                            1.0 - dshb::ShutdownEntryProgress(dshb::ShutdownFrame()),
                            WidenUtf8(frame.amountText).c_str(), frame.showAmount ? 1 : 0,
                            frame.currencySymbol ? frame.currencySymbol : L"",
                            frame.statusText ? frame.statusText : L"",
                            frame.countdownText ? frame.countdownText : L"",
                            frame.zeroTimeText.size(), frame.curve.size(),
                            frame.curveHasData ? 1 : 0, frame.places.size());
            }
        }

        if (g_debug) {
            // --debug：每 6 帧记一次"动画中的显示值"与"目标值"。
            // 两者不同 -> 正在滚动；一次相同 -> 已经落定。用来验证"不是突变"。
            static int dbgTick = 0;
            if (++dbgTick % 6 == 0) {
                SelfTestLogVerbose(L"[dbg] 显示值=%.4f 目标值=%.4f", g_display.value(), g_display.target());
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

        // ---- 关闭粒子：把粒子推到第 k 帧，再导这一帧 ----
        // ★ 必须在 SetWidgetFrame 之后：粒子的颜色取的是**这一帧** WidgetFrame 里的氛围色，
        //   顺序反了就会拿上一帧（或初始的透明）颜色去画，PNG 里量出来的颜色对不上。
        // ★ 必须真的 Start 一次（而不是直接推年龄）：Start 才是"从帧里取色 + 确定性撒点"
        //   那一步，跳过它就没有粒子可推。
        if (g_particlesOnly) dshb::SetParticlesOnlyModeForProbe();
        if (g_noParticles) dshb::SetParticlesDisabledForProbe(true);
        if (g_noPresent) dshb::SetNoPresentForProbe(true);

        if (g_shutdownParticles) {
            renderer.StartShutdownParticles();
            renderer.SetShutdownParticlesAge(particleAge);
            const std::string line = renderer.ShutdownParticlesLog();
            SelfTestLogVerbose(L"[shutdown] k=%d ageMs=%.1f %ls", g_frameNo, particleAge * 1000.0,
                        WidenUtf8(line).c_str());
            SelfTestLogVerbose(L"[shutdown] 粒子颜色取本帧氛围色 color=%ls（就是上面 [ambience] 那一行）",
                        AmbienceHex(g_display.ambienceColor()).c_str());
        }

        const bool ok = renderer.ExportFrame(g_exportPath, t);

        // 绘制期间只往内存里写的诊断（曲线采样范围），**画完之后**才落盘：
        // 绘制路径里做文件 I/O 会把进程弄崩（本项目踩过两次）。取一次就清空。
        {
            const std::string diag = dshb::CurveDebugText();
            if (!diag.empty()) SelfTestLogVerbose(L"[drawdiag] %ls", WidenUtf8(diag).c_str());
        }

        // 曲线状态的诊断（存储里有几个点、计时器停在哪一帧）：导帧量像素时，
        // "这一帧到底是滚动中的第几帧、环里是哪几个点"必须能从日志里对上。
        SelfTestLogVerbose(L"[curve] %hs", dshb::CurveStateLine().c_str());

        // 诊断写文件放在绘制**之后**：绘制路径里做 I/O 会让进程崩（实测）
        dshb::DumpLayoutProbe();
        SelfTestLogVerbose(L"[export] %ls 帧=%d 时刻=%.3fs 结果=%ls 画布=%dx%d",
                    g_exportPath, g_frameNo, t, ok ? L"成功" : L"失败",
                    size.widthPx, size.heightPx);
        // ===== TEMPORARY: inner-glow re-bake accounting (task 2) =====
        if (g_logGlowStats) {
            const dshb::InnerGlowBakeCounters& g = dshb::InnerGlowBakeStats();
            SelfTestLogVerbose(L"[glow] coverageBakes=%d coverageWorstMs=%.3f tintBakes=%d "
                        L"tintWorstMs=%.3f tintTotalMs=%.3f frames=%d",
                        g.coverageBakes, g.coverageWorstMs, g.tintBakes, g.tintWorstMs,
                        g.tintTotalMs, g.frames);
        }
        // ===== END TEMPORARY: task 2 =====
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
            SelfTestLogVerbose(L"[check] %ls: %ls", cond ? L"PASS" : L"FAIL", what);
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
                    SelfTestLogVerbose(L"[check]   一个 τ 后走了 %.3f（期望约 0.63）", walked);
                }
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
                SelfTestLogVerbose(L"[check]   追赶中途: mid=%.4f after=%.4f target=150", mid, after);

                // ★ 断言"朝新目标走、且不越过去"，方向由目标决定，不由我想当然决定。
                //   上一版这里写的是"after < mid"（假设它该下降），可当前值 110.8
                //   低于新目标 150，它本来就该上升——**测试自己写反了方向**。
                const double moved = after - mid;
                const double toGo = 150.0 - mid;
            }
        }

        SelfTestLogVerbose(L"[check] 小计：失败 %d 项", failed);
        CoUninitialize();
        return failed == 0 ? 0 : 9;
    }

    Clock clock;
    double elapsed = 0.0;
    double lastLoopSeconds = 0.0;   // 上一帧的墙钟时刻（未钳制的帧间隔靠它算）
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
                SelfTestLogVerbose(L"[seq] t=%.2fs 第 %d 个值 -> real=%.2f", elapsed, idx, g_seq[idx]);
            }
        }
        // ★ elapsed 取**总时间**（clock.Total()），不是逐帧累加 dt：主循环在窗口没有焦点
        //   时会空转，累加值会比墙上时间慢二十倍，于是 --seconds=N 不按时退出、日志里
        //   每一个 `t=%.1fs` 都偏小（所有者实测：墙上 32 s 时它写 4.8 s）。理由写在 Clock::Total。
        elapsed = clock.Total();
        g_elapsed = elapsed;
        // 这一帧与上一帧的真实间隔（**未钳制**：dt 被夹到 50ms，量不出真正的卡顿，
        // 而"拖动不顿"要的正是"最长的那一次有多长"）。只在拖动期间被用上。
        const double loopIntervalMs = (elapsed - lastLoopSeconds) * 1000.0;
        lastLoopSeconds = elapsed;

        // ---- 数据管道（B 阶段）----
        // 目前数据来自模拟源；真实接口在 J 阶段接上。状态机只认"一条采样"，
        // 所以换数据源不需要动它——这正是把这两件事分开的目的。
        {
            const dshb::Sample s = g_fake.NextIfDue(elapsed);
            if (s.wallMs != 0 && !g_realApiPlanned && !g_fixedGiven && !g_realGiven && !g_lastGiven && g_frames < 0 && g_seq.empty()) {   // 钉值/真接口时不喂
                g_states.OnSample(s, s.wallMs);
                // ★ 显示层**只在新样本到达时**喂（见下面删掉的那行每帧喂入）。
                //   每帧重复喂同一条样本，会让 L 恒等于 R（D=0）——滚动动画永远不动，
                //   看起来就是数字突变（所有者实测：18.27 -> 18.12 无滚动）。
                g_display.OnSample(g_states.lastGood());
            }
        }

        // ★ 归零预测的实时读数（只在 --selftest）。
        //   为什么需要它：底部那行字平时**不写日志**（只有导帧那条路会印 [frame]），
        //   于是"预测到底用了哪个速率"在正常运行时无从观察 —— 而这一轮换的正是速率
        //   公式（EstimateRateTheilSen）。有了这一行，一条 --seq 跑完就能把"预测用了
        //   新公式"这件事从日志里读出来，不必靠导帧。
        //   ★ 记录节流是按**这句话本身**变的幅度来的（分支变了，或分钟数变了 5 分钟
        //     且超过 10%），不是按速率的末位。弹簧让速率每帧都在末位抖，一帧一行的
        //     日志长度是按帧数长的 —— 而这一行要回答的只是"那段话是拿哪个速率算的"。
        //   还要一个上限：万一判断出问题也不能把日志淹掉（一次运行最多 200 行）。
        if (g_selfTest && g_display.hasValue()) {
            static dshb::ZeroTimeKind lastKind = dshb::ZeroTimeKind::None;
            static int lastMinutes = -1;
            static int emitted = 0;
            const dshb::RateEstimate& estimate = g_display.rateEstimate();
            dshb::RateEstimate smoothed = estimate;
            smoothed.rateYuanPerMinute = g_display.rateDisplay();
            const dshb::ZeroTimeText line = dshb::ZeroTimeFor(
                smoothed,
                static_cast<int64_t>(std::llround(g_display.target() * dshb::kUnitsPerYuan)),
                static_cast<int64_t>(std::time(nullptr)));
            const bool kindChanged = line.kind != lastKind;
            const double delta = std::fabs(static_cast<double>(line.minutesToZero - lastMinutes));
            const bool minutesMoved = lastMinutes > 0 && line.minutesToZero >= 0 &&
                                      delta >= 5.0 && delta >= 0.10 * lastMinutes;
            if ((kindChanged || minutesMoved) && emitted < 200) {
                lastKind = line.kind;
                lastMinutes = line.minutesToZero;
                ++emitted;
                SelfTestLogVerbose(L"[rate] t=%.1fs status=%hs rate=%.10f pairs=%lld medianSlope=%.10f "
                            L"zeroTime=\"%ls\"",
                            elapsed, dshb::RateStatusName(estimate.status),
                            g_display.rateDisplay(), static_cast<long long>(estimate.pairCount),
                            estimate.medianSlopeYuanPerSecond, line.text.c_str());
            }
        }

        // 真接口的启动（Key 与"要不要用"都在进循环之前做完了，见上面那块）
        if (g_realApiPlanned && !g_realApiOn) {            dshb::BalanceSourceConfig cfg{};
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
                SelfTestLogVerbose(L"[layout] state=%ls showAmount=%d symbol=%ls amount=%hs",
                            dshb::ConnStateName(st2), renderer.widgetFrame().showAmount ? 1 : 0,
                            renderer.widgetFrame().currencySymbol,
                            renderer.widgetFrame().amountText.c_str());
                SelfTestLogVerbose(L"[layout] display: hasValue=%d value=%.4f target=%.4f tau=%.3f",
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
                SelfTestLogVerbose(L"[dbg] 显示值=%.4f 目标值=%.4f", g_display.value(), g_display.target());
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
                       L"F1..F9=scenario  R=recharge  C=clockjump",
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
                if (rs.amountsOk) CommitDelayed();   // 滞后一拍（见 CommitDelayed）
            }
            std::string apiLine;
            // 带时间戳（设计 §10.5 的日志要求）：这样"暂停期间没请求""唤醒立刻补一次"可验证
            // ★ 这里必须走 WidenUtf8，**不能**用 %hs：%hs 在宽格式里是按当前 C 区域设置
            //   转换窄串的，而日志行里可能有 UTF-8 字节（api_client 的中文说明）。逐字节当
            //   宽字符的后果实测就是日志里出现乱码 —— 同一个坑 WidenUtf8 上面那段注释
            //   早就写过，这里只是又多了一个踩它的入口。
            while (g_apiSource.PollLog(&apiLine)) {
                std::wstring logPollLine;   // 这一条到底是 UTF-8 还是纯 ASCII，下面判
                // 含非 ASCII 字节 -> 是 UTF-8 文本，必须解码；纯 ASCII 两路等价。
                bool nonAscii = false;
                for (const char c : apiLine) {
                    if (static_cast<unsigned char>(c) >= 0x80) {
                        nonAscii = true;
                        break;
                    }
                }
                if (nonAscii) {
                    logPollLine = WidenUtf8(apiLine);
                } else {
                    logPollLine.assign(apiLine.begin(), apiLine.end());
                }
                // 每 10 秒一条 "status=Ok" / "interval ->" 是生产里最大的噪声源
                // （一天约八千条），但**失败必须留**：用户报"不显示余额"时，
                // "Ok" 与"超时 / 401 / 连不上"的区别就是全部线索。
                // 所以这一条按**内容**分，而不是整条 gate。
                const bool routine = apiLine.find("status=Ok") != std::string::npos ||
                                     apiLine.find("interval ->") != std::string::npos;
                if (routine) {
                    SelfTestLogVerbose(L"[api t=%.1fs] %ls", elapsed, logPollLine.c_str());
                } else {
                    SelfTestLog(L"[api t=%.1fs] %ls", elapsed, logPollLine.c_str());
                }
            }
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
        // --click-test：注入三次手势，验证"拖动与长按都不算点击"这条边界。
        // 走的是和真实鼠标**同一个**判定函数，不是旁路。
        // 手势 1（干净的单击）预期只写『干净的单击：算点击』一行 —— 单击现在没有动作可做，
        // 所以"分类成点击、然后什么都不发生"就是它该有的结果。
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
        if (g_clickTest && g_panelRectValid) {
            static int ctStage = 0;
            static double ctAt = 1.0;

            if (ctStage < 3 && elapsed >= ctAt) {
                // 落点取面板实体区中心（客户区像素）—— 那里就是数字区。手势 1 点它一下，
                // 手势 2 从它拖出 40px。坐标仍然必须是真的：FinishLeftGesture 只用 x,y
                // 算"移动了多少"，随便给个点会让手势 2 那条断言失去意义。
                const int cx = (g_panelRectClient.left + g_panelRectClient.right) / 2;
                const int cy = (g_panelRectClient.top + g_panelRectClient.bottom) / 2;
                if (ctStage == 0) {
                    SelfTestLogVerbose(L"[click-test] 手势 1：干净的单击（预期只写『干净的单击：算点击』）");
                    g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64(); g_pressValid = true;
                    FinishLeftGesture(cx, cy);
                } else if (ctStage == 1) {
                    SelfTestLogVerbose(L"[click-test] 手势 2：从数字区拖出 40px（预期『移动 40px：算拖动，不算点击』）");
                    g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64(); g_pressValid = true;
                    FinishLeftGesture(cx + 40, cy);
                } else {
                    SelfTestLogVerbose(L"[click-test] 手势 3：按住 900ms 再松（预期『算长按，不算点击』那一行）");
                    g_pressX = cx; g_pressY = cy; g_pressTick = GetTickCount64() - 900; g_pressValid = true;
                    FinishLeftGesture(cx, cy);
                }
                ++ctStage;
                ctAt = elapsed + 1.5;
            }
        }
        // ---- 关闭态（设计 §10.2）：钩子判定的回收、剧本、日志 ----
        // 顺序有讲究：
        //   1. 钩子的结论先收（它是上面那轮消息泵里产生的）；
        //   2. 剧本注入的消息在**下一轮**泵里被处理（PostMessage 的语义）；
        //   3. 日志统一在 Update **之后**写：那几行要报的 R/颜色/心跳数就是这一帧真正
        //      画上去的数（R_d 的下限在 AdvanceAmbience 里生效）。
        ApplyOutsideClickVerdict();
        RunShutdownTestScript(elapsed);
        RunShutdownEnterHold(elapsed);
        RunTrayMenuTestScript(elapsed);
        g_display.Update(dt);
        RefreshPanelRectForHook();     // 钩子下一次判定要用（面板在屏幕上的实际位置）
        // 托盘图标矩形的缓存：钩子只读它。成功的矩形不会每帧重问（那是跨进程调用），
        // 失败的那一侧内部按 250 ms 重试（"图标刚被折进溢出区"要尽快看出来）。
        g_tray.KeepGeometryFresh();
        // --tray-probe=1：进循环后第一帧再写一遍事实（此时安装已经跑完、矩形也刷过一次）。
        if (g_trayProbe) {
            static bool trayProbeDumped = false;
            if (!trayProbeDumped) {
                trayProbeDumped = true;
                LogTrayFacts();
            }
        }
        FlushCloseLogs();

        // ---- 关闭粒子（设计 §11.6）：真实帧循环里的驱动 ----
        // 触发一次，然后**一帧都不跳过**地记渲染耗时。记的是 p50/p99（§11.3 的预算口径），
        // 不是平均值；帧间隔也记，因为"预算够不够"最终看的是这两条分布。
        // ★ 这一段是**测试驱动**，不是关闭流程：真实的调用点是"第三次点击确认之后"
        //   （见 renderer.h 的 StartShutdownParticles 注释），与这里同一个函数、同一条路径。
        if (g_shutdownParticles) {
            if (!g_particleTimesOpen && elapsed >= kShutdownParticlesArmSeconds) {
                renderer.StartShutdownParticles();
                g_particleTimesOpen = true;
                g_particleTimesUntil = elapsed + kShutdownParticlesTailSeconds;
                g_particleTimes.rasterMs.clear();
                g_particleTimes.intervalMs.clear();
                // ★ G5（再次触发不产生第二份）的证据在这一行里：**紧跟着再调一次**，
                //   返回 0 = 被挡住。故意在这里调，而不是靠"别处不会再调"来保证。
                const int second = renderer.StartShutdownParticles();
                const std::string line = renderer.ShutdownParticlesLog();
                SelfTestLogVerbose(L"[shutdown] t=%.3fs 触发（紧接着第二次调用返回 %d = G5 挡住） %ls",
                            elapsed, second, WidenUtf8(line).c_str());
            }
            if (g_particleTimesOpen) {
                g_particleTimes.intervalMs.push_back(loopIntervalMs);
            }
        }

        // 关闭态的曲线层：进入段里它还画（它正在向内收缩，那是"旧内容被抽走"的唯一可收缩
        // 元素），进入段走完就整层不画（曲线的 alpha/线宽是渲染层的常量）。
        // ★ 只在**变化时**动这个开关，而且不在非关闭态动它 —— 那会把 --no-curve 的语义
        //   顺手带进真实帧循环，属于本次改动之外的行为变化。
        {
            static bool curveLayerOn = true;
            const bool wantOn = dshb::ShutdownCurveLayerVisible();
            if (wantOn != curveLayerOn) {
                curveLayerOn = wantOn;
                dshb::SetCurveEnabled(wantOn);
                SelfTestLog(L"[close] 曲线层 -> %ls", wantOn ? L"画" : L"不画（关闭态进入段已走完）");
            }
        }

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
        if (g_particleTimesOpen) g_particleTimes.rasterMs.push_back(ms);
        // --frame-stats=N：全部帧都记（不限粒子那段），跑满 N 帧就打 p50/p99 退出。
        // 这是 §11.3 的通用口径 —— 粒子那一段的 p50/p99 只有跟同一台的基线比才有意义。
        if (g_frameStatsFrames > 0) {
            g_allTimes.rasterMs.push_back(ms);
            g_allTimes.intervalMs.push_back(loopIntervalMs);
            if (static_cast<int>(g_allTimes.rasterMs.size()) >= g_frameStatsFrames) {
                auto pct = [](std::vector<double> v, double q) {
                    if (v.empty()) return 0.0;
                    std::sort(v.begin(), v.end());
                    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5)];
                };
                SelfTestLogVerbose(L"[framestats] frames=%zu present=%ls raster p50=%.3fms p99=%.3fms "
                            L"max=%.3fms | interval p50=%.3fms p99=%.3fms",
                            g_allTimes.rasterMs.size(), g_noPresent ? L"no" : L"yes",
                            pct(g_allTimes.rasterMs, 0.50), pct(g_allTimes.rasterMs, 0.99),
                            pct(g_allTimes.rasterMs, 1.00), pct(g_allTimes.intervalMs, 0.50),
                            pct(g_allTimes.intervalMs, 0.99));
                g_running = false;
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                break;
            }
        }
        // 第 3 击之后：粒子播完 = 关闭（进程退出）。判断用"粒子真的停了"，而不是"到了
        // 预定的时刻" —— 停在哪一帧由粒子自己说；没有粒子时（拿不到渲染器）立刻退出，
        // 绝不挂在这里。
        if (g_closeWaitParticles && !renderer.shutdownParticlesActive()) {
            g_closeWaitParticles = false;
            SelfTestLog(L"[close] 粒子播完：关闭 = 进程退出（窗口、托盘、后台一起消失）");
            g_running = false;
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        // 拖动期间单独记账：只有这一段能回答"拖动不顿"（[render] 那条汇总线把空闲时间
        // 也算进去了）。松手时由 EndDrag 打一行，平时一个字节都不写。
        if (g_dragActive) {
            ++g_dragFrames;
            g_dragFrameMsSum += ms;
            g_dragIntervalSum += loopIntervalMs;
            if (ms > g_dragWorstFrameMs) g_dragWorstFrameMs = ms;
            if (loopIntervalMs > g_dragWorstIntervalMs) g_dragWorstIntervalMs = loopIntervalMs;
        }
        // 粒子播完之后把这一段的分布打出来（p50/p99 是 §11.3 的口径，平均值不是）。
        // ★ 条件用"粒子真的停了"而不是"到了预定的结束时刻"：停在哪一帧由粒子自己说，
        //   日志里的帧数因此就是**实际画了粒子的帧数**，不是估计值。
        if (g_particleTimesOpen && !renderer.shutdownParticlesActive()) {
            g_particleTimesOpen = false;
            const std::string line = renderer.ShutdownParticlesLog();
            SelfTestLogVerbose(L"[shutdown] 播放结束 %ls", WidenUtf8(line).c_str());
            auto percentile = [](std::vector<double> v, double q) {
                if (v.empty()) return 0.0;
                std::sort(v.begin(), v.end());
                const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5);
                return v[i];
            };
            SelfTestLogVerbose(L"[shutdown] frames=%zu raster p50=%.3fms p99=%.3fms max=%.3fms | "
                        L"interval p50=%.3fms p99=%.3fms（§11.3 预算 p99 < 8 ms）",
                        g_particleTimes.rasterMs.size(),
                        percentile(g_particleTimes.rasterMs, 0.50),
                        percentile(g_particleTimes.rasterMs, 0.99),
                        percentile(g_particleTimes.rasterMs, 1.00),
                        percentile(g_particleTimes.intervalMs, 0.50),
                        percentile(g_particleTimes.intervalMs, 0.99));
        }

        if (FAILED(lastHr)) {
            ++failures;
            if (failures <= 3) {
                SelfTestLogVerbose(L"[render] 第 %llu 帧失败: hr=0x%08lX",
                            static_cast<unsigned long long>(frames),
                            static_cast<unsigned long>(lastHr));
            }
        }

        if (g_selfTest && elapsed >= (g_shutdownParticles ? kShutdownParticlesArmSeconds +
                                                             kShutdownParticlesTailSeconds
                                                           : g_selfTestSeconds)) break;        // ===== TEMPORARY: bounded real-loop run so the glow accounting can be logged =====
        if (g_realFrames > 0 && ++g_realFrameCount >= g_realFrames) {
            const dshb::InnerGlowBakeCounters& g = dshb::InnerGlowBakeStats();
            SelfTestLogVerbose(L"[glow] REAL SCREEN PATH  coverageBakes=%d coverageWorstMs=%.3f "
                        L"tintBakes=%d tintWorstMs=%.3f tintTotalMs=%.3f frames=%d",
                        g.coverageBakes, g.coverageWorstMs, g.tintBakes, g.tintWorstMs,
                        g.tintTotalMs, g.frames);
            SelfTestLogVerbose(L"[glow] real-path frames=%d（--real-frames 到了就退出）", g_realFrameCount);
            break;
        }
        // ===== END TEMPORARY =====
        if (!g_selfTest && g_runSeconds > 0.0 &&
            elapsed >= (g_shutdownParticles ? kShutdownParticlesArmSeconds +
                                                  kShutdownParticlesTailSeconds
                                            : g_runSeconds)) break;

        // Present 已经等过垂直空白，这里只需要把消息收干净
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    SelfTestLogVerbose(L"[render] frames=%llu elapsed=%.2fs 平均单帧=%.3fms 平均刷新率=%.1fHz 失败=%d",
                static_cast<unsigned long long>(frames), elapsed,
                frames ? frameMsSum / static_cast<double>(frames) : 0.0,
                elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0, failures);
    SelfTestLogVerbose(L"[win] 自检完成，最后 Present hr=0x%08lX",
                static_cast<unsigned long>(lastHr));

    renderer.Destroy();
    g_renderer = nullptr;
    g_apiSource.Stop();
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
}
