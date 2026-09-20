// dragprobe -- 面板的自控拖动：跟手、**边缘吸附**、钳制、松手即存、拖动期间的帧率。
//
//   dragprobe                    全跑（离线判据 + 真机注入拖动）
//   dragprobe --no-live          只跑离线部分（不想动鼠标、或没有交互桌面时）
//   dragprobe --real-config      真机部分读写**所有者自己那个** config.json（默认写 %TEMP%）
//   dragprobe --park=X,Y         真机部分最后把面板停在哪（默认 1600,240；超出工作区会被钳）
//   dragprobe --keep-temp        留下临时文件（默认跑完删掉）
//   dragprobe --verbose          把每一步的光标位置与窗口矩形也打出来
//   dragprobe --exe=PATH         被测程序（默认与本探针同目录的 dshb.exe）
//
// 为什么要有这个探针，而不是"人拖一下看看"：这些判据都是**可以量的数** ——
//   ① 跟手不漂：窗口位移与光标位移是整数，必须逐像素相等；
//   ② 吸附：面板的哪条边进了 24px、吸到工作区边上没有、拖走之后有没有粘住，都是像素级的事；
//   ③ 松手即存、重开原位：文件里是几个整数，重开后的窗口矩形也是几个整数；
//   ④ 钳制：判据是**面板**的可见面积比 >= 50%（本步改的口径），算得出来；
//   ⑤ 拖动不顿：帧间隔只有在窗口真的跟着动的那几秒里才有意义。
// 人眼看不见 1 像素的漂移，也看不出"重开是不是落在同一个像素上"。
//
// ★ 它拿到的是真东西，不是一份复制品：
//   · 几何 / 吸附 / 钳制 / 持久化：直接链接 src/panel_drag.cpp，调的就是生产代码那几个函数；
//   · 真机那一段：**启动真的 dshb.exe**，用 SetCursorPos + SendInput 注入真实鼠标输入，
//     再用 GetWindowRect 量它自己移动了多少。走的是 WndProc 里那条真路。
// ★ 它绝不碰所有者的数据：子进程一律带 --curve-store=<%TEMP%>；config 默认也换到 %TEMP%，
//   只有 --real-config 那一次才用真路径（那一次的意义就是验证真路径）。
// ★ 它自己声明 DPI 感知：这台机器缩放 200%，一个非感知进程看到的窗口矩形与光标坐标都被
//   系统**虚拟化**过（正好一半），拿它做拖动算术会得到 2 倍的偏差 —— 量错了还以为代码错。
#include "panel_drag.h"
#include "paths.h"
#include "renderer.h"   // kCanvasWidthDip / kCanvasHeightDip：窗口尺寸的唯一出处

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }
};

bool g_verbose = false;
bool g_live = true;
bool g_realConfig = false;
bool g_keepTemp = false;
std::wstring g_exePath;

std::string Num(long long value) { return std::to_string(value); }

std::string F2(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

std::string Pct(double fraction) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f%%", fraction * 100.0);
    return buf;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::string RectStr(const RECT& r) {
    return "(" + Num(r.left) + "," + Num(r.top) + ")-(" + Num(r.right) + "," + Num(r.bottom) + ")";
}

std::string OneLine(const std::string& text) {
    std::string out;
    for (char c : text) out += (c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (wchar_t* slash = wcsrchr(buf, L'\\')) *(slash + 1) = L'\0';
    return buf;
}

// 子进程的日志落点。★ 2026-09-19 起**不再**是"exe 旁边那个 selftest.log"：
// 所有者报"运行完在目录下冒出日志文件"，产品的日志挪到了 %LOCALAPPDATA%\deepseek-balance\dshb.log。
//   形状与 --curve-store= 同一条规矩：测试的落点由测试给，不碰产品的落点。
//   （定义放在 TempPath 之后 —— 下面那段就是它。）
unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
    return (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

std::string ReadAppended(const std::wstring& path, unsigned long long from) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return std::string();
    _fseeki64(f, static_cast<long long>(from), SEEK_SET);
    std::string blob;
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) blob.append(buf, got);
    std::fclose(f);
    return blob;
}

std::wstring TempPath(const std::wstring& leaf) {
    wchar_t dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, dir) == 0) return std::wstring();
    return std::wstring(dir) + leaf;
}

// 子进程的日志落点：探针自己的临时文件，通过 --log-file= 交给子进程，自己读同一个。
// ★ 为什么不用产品那个落点（%LOCALAPPDATA%\...\dshb.log，唯一出处是 dshb::Paths().log）：
//   日志只有一个落点之后，探针拉起的子进程会与所有者正在跑的挂件**写同一个文件**，
//   而这里是按**字节偏移**读它（先记大小、再读新增部分）—— 两个进程同时写会互相干扰，
//   而且探针的噪声会进所有者会去看的那份日志。
std::wstring LogPath() {
    return TempPath(L"dshb-dragprobe-" + std::to_wstring(GetCurrentProcessId()) + L"-log.log");
}

bool ReadFileText(const std::wstring& path, std::string* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, got);
    std::fclose(f);
    return true;
}

bool WriteFileText(const std::wstring& path, const char* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const size_t n = std::strlen(text);
    const size_t written = std::fwrite(text, 1, n, f);
    std::fclose(f);
    return written == n;
}

RECT RectOf(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}

POINT TopLeftOf(HWND hwnd) {
    const RECT r = RectOf(hwnd);
    return POINT{r.left, r.top};
}

SIZE SizeOf(HWND hwnd) {
    const RECT r = RectOf(hwnd);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

// 窗口左上角 -> "实体区（可见面板）里某一点"的屏幕坐标，再**收进屏幕里**。
// 为什么必须收：窗口被钳在屏边时实体区有一半在屏外，而注入是按屏幕坐标来的，屏外那半点
// 不到；反过来，窗口在屏中间时这个函数就是恒等变换。另外按压点必须落在**实体区**里：
// 画布四周那圈透明余量按 alpha 命中测试是鼠标穿透的，点在余量上收不到 WM_LBUTTONDOWN。
// 实体区在画布里的偏移就是 kMarginDip（renderer.h: 画布 = 实体 + 2×余量），而画布 = 窗口
// （缩放固定 1.0）。
bool VisibleEntityPoint(const RECT& win, const RECT& work, int preferX, int preferY, POINT* out) {
    const int inset = 8;
    const int left = (win.left + dshb::kMarginDip > work.left + inset)
                         ? win.left + dshb::kMarginDip
                         : work.left + inset;
    const int top = (win.top + dshb::kMarginDip > work.top + inset) ? win.top + dshb::kMarginDip
                                                                    : work.top + inset;
    const int right = (win.left + dshb::kMarginDip + dshb::kEntityWidthDip < work.right - inset)
                          ? win.left + dshb::kMarginDip + dshb::kEntityWidthDip
                          : work.right - inset;
    const int bottom = (win.top + dshb::kMarginDip + dshb::kEntityHeightDip < work.bottom - inset)
                           ? win.top + dshb::kMarginDip + dshb::kEntityHeightDip
                           : work.bottom - inset;
    if (right <= left || bottom <= top) return false;
    int x = win.left + dshb::kMarginDip + preferX;
    int y = win.top + dshb::kMarginDip + preferY;
    if (x < left) x = left;
    if (x > right) x = right;
    if (y < top) y = top;
    if (y > bottom) y = bottom;
    *out = POINT{x, y};
    return true;
}

// 屏幕上某一点是哪个窗口 —— 注入之前必须先问这一句话。
bool PointIs(HWND hwnd, POINT p) { return WindowFromPoint(p) == hwnd; }

// 那一点上到底是谁（候选位置被挡时用来说清"是谁挡的"，不猜）。
std::string WindowDescAt(POINT p) {
    const HWND at = WindowFromPoint(p);
    if (!at) return "(没有窗口)";
    wchar_t cls[64]{};
    GetClassNameW(at, cls, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(at, &pid);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "hwnd=0x%p class=%s pid=%lu", static_cast<void*>(at),
                  Narrow(cls).c_str(), pid);
    return buf;
}

bool SetCursor(POINT p) { return SetCursorPos(p.x, p.y) != FALSE; }

bool ButtonInput(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    return SendInput(1, &in, sizeof(in)) == 1;
}

// 日志行的中文里夹着 ASCII 数字，按 "键=" 取数就够（不猜格式，也不做整行解析）。
bool NumberAfter(const std::string& line, const char* key, double* out) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return false;
    const char* p = line.c_str() + at + std::strlen(key);
    char* end = nullptr;
    const double value = std::strtod(p, &end);
    if (end == p) return false;
    *out = value;
    return true;
}

// 取 "(x,y)" 这样的一对整数（日志里全用这个写法：期望值、实际值、各段位移）。
bool ReadPair(const std::string& line, const char* key, long* first, long* second) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return false;
    const char* p = line.c_str() + at + std::strlen(key);
    char* end = nullptr;
    const long x = std::strtol(p, &end, 10);
    if (end == p || *end != ',') return false;
    const long y = std::strtol(end + 1, &end, 10);
    if (*end != ')') return false;
    *first = x;
    *second = y;
    return true;
}

// ---------------------------------------------------------------------------
// 等窗口落位
// ---------------------------------------------------------------------------

// 等到窗口左上角 == want（或超时）。返回最终位置，等了多久写进 waitedMs。
// 为什么必须等：注入的移动是**异步**的（消息要走到 pump 才被处理），注入完立刻读矩形读到的
// 往往是上一次的结果 —— 那样量出来的"偏差"是采样问题，不是代码问题。
POINT WaitForTopLeft(HWND hwnd, POINT want, int timeoutMs, int* waitedMs) {
    const unsigned long long start = GetTickCount64();
    POINT got = TopLeftOf(hwnd);
    for (;;) {
        got = TopLeftOf(hwnd);
        if (got.x == want.x && got.y == want.y) break;
        if (GetTickCount64() - start >= static_cast<unsigned long long>(timeoutMs)) break;
        Sleep(1);
    }
    if (waitedMs) *waitedMs = static_cast<int>(GetTickCount64() - start);
    return got;
}

// 等到矩形连续三次读数都一样（被钳制之后不知道该期望哪个值，只能等它停下来）。
POINT WaitForStableTopLeft(HWND hwnd, int timeoutMs, int* waitedMs) {
    const unsigned long long start = GetTickCount64();
    POINT last = TopLeftOf(hwnd);
    int same = 0;
    for (;;) {
        Sleep(5);
        const POINT now = TopLeftOf(hwnd);
        same = (now.x == last.x && now.y == last.y) ? same + 1 : 0;
        last = now;
        if (same >= 3) break;
        if (GetTickCount64() - start >= static_cast<unsigned long long>(timeoutMs)) break;
    }
    if (waitedMs) *waitedMs = static_cast<int>(GetTickCount64() - start);
    return last;
}

// ---------------------------------------------------------------------------
// 被测进程
// ---------------------------------------------------------------------------

struct Child {
    PROCESS_INFORMATION pi{};
    HWND hwnd = nullptr;
    unsigned long long logFrom = 0;
    bool started = false;
};

struct WindowSearch {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindChildWindow(HWND hwnd, LPARAM param) {
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"DshbWnd") == 0) {
        search->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND WaitForChildWindow(DWORD pid, int timeoutMs) {
    const unsigned long long start = GetTickCount64();
    for (;;) {
        WindowSearch search{};
        search.pid = pid;
        EnumWindows(FindChildWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.found;
        if (GetTickCount64() - start >= static_cast<unsigned long long>(timeoutMs)) return nullptr;
        Sleep(20);
    }
}

bool LaunchChild(Child* child, const std::wstring& configPath, const std::wstring& curvePath,
                 int seconds) {
    child->logFrom = FileSize(LogPath());

    std::wstring cmd = L"\"" + g_exePath + L"\" --force-new-instance";
    // --log-file=：把子进程的日志指到本次探针自己的临时文件。★ 必须有：日志只有一个落点，
    //   而所有者可能正跑着一个挂件，两个进程写同一个文件会互相干扰（下面按字节偏移读它）。
    cmd += L" --log-file=\"" + LogPath() + L"\"";
    // --force-new-instance：所有者自己那个实例可能正跑着。--api=off：量帧率时不能有后台
    // HTTP 线程插进来。--curve-store=：测试绝不能写所有者的 curve.json。
    if (!configPath.empty()) cmd += L" --config=\"" + configPath + L"\"";
    cmd += L" --curve-store=\"" + curvePath + L"\" --api=off --selftest --seconds=" +
           std::to_wstring(seconds);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &child->pi)) {
        std::printf("[live] CreateProcess 失败 err=%lu\n", GetLastError());
        return false;
    }
    child->started = true;
    child->hwnd = WaitForChildWindow(child->pi.dwProcessId, 8000);
    return child->hwnd != nullptr;
}

void StopChild(Child* child) {
    if (!child->started) return;
    if (WaitForSingleObject(child->pi.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(child->pi.hProcess, 0);
        WaitForSingleObject(child->pi.hProcess, 2000);
    }
    CloseHandle(child->pi.hThread);
    CloseHandle(child->pi.hProcess);
    child->started = false;
    child->hwnd = nullptr;
}

// 追加日志里以 prefix 开头的行。归属靠 **logFrom**（启动前记下的文件大小）：
// 所有者自己那个实例如果也在写这个日志，写的是 [api]/[commit] 之类的行，不会混进来。
std::vector<std::wstring> ChildLines(const Child& child, const wchar_t* prefix) {
    const std::string text = ReadAppended(LogPath(), child.logFrom);
    const std::wstring wide = Wide(text);
    std::vector<std::wstring> out;
    std::size_t pos = 0;
    while (pos < wide.size()) {
        std::size_t eol = wide.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = wide.size();
        std::wstring line = wide.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        const std::size_t plen = std::wcslen(prefix);
        if (line.size() >= plen && line.compare(0, plen, prefix) == 0) out.push_back(line);
    }
    return out;
}

std::wstring FirstLineWith(const std::vector<std::wstring>& lines, const wchar_t* needle) {
    for (const std::wstring& line : lines) {
        if (line.find(needle) != std::wstring::npos) return line;
    }
    return std::wstring();
}

int CountWith(const std::vector<std::wstring>& lines, const wchar_t* needle) {
    int count = 0;
    for (const std::wstring& line : lines) {
        if (line.find(needle) != std::wstring::npos) ++count;
    }
    return count;
}

// 等子进程把某一行写进日志。为什么要等：日志是子进程写的，探针读到的只是"此刻盘上的
// 内容"，两边没有别的同步手段；一次性读会在竞态下报出假的"没找到"。
std::wstring WaitForLine(const Child& child, const wchar_t* needle, int timeoutMs) {
    const unsigned long long start = GetTickCount64();
    for (;;) {
        const std::wstring found = FirstLineWith(ChildLines(child, L"[drag]"), needle);
        if (!found.empty()) return found;
        if (GetTickCount64() - start >= static_cast<unsigned long long>(timeoutMs)) return std::wstring();
        Sleep(40);
    }
}

// ---------------------------------------------------------------------------
// 面板几何与小工具（吸附/钳制/持久化三段的共用口径）
// ---------------------------------------------------------------------------

// 1:1 的面板：窗口 475x289 → 面板 (80,80)-(395,209)。三段里凡是"合成输入"的地方都用它。
const dshb::PanelGeometry kPanel1to1{};

std::string PanelRectStr(POINT windowTopLeft, const dshb::PanelGeometry& panel) {
    const RECT r = dshb::PanelRectForWindow(windowTopLeft, panel);
    return "(" + Num(r.left) + "," + Num(r.top) + ")-(" + Num(r.right) + "," + Num(r.bottom) + ")";
}

// 面板每条边距工作区对应边的**带符号**距离：正数 = 面板在工作区里面。
// 吸附的判据就是这个数（对方是 0）。
std::string GapsStr(POINT windowTopLeft, const dshb::PanelGeometry& panel, const RECT& work) {
    const RECT r = dshb::PanelRectForWindow(windowTopLeft, panel);
    return "面板距工作区 左" + Num(r.left - work.left) + " 右" + Num(work.right - r.right) + " 上" +
           Num(r.top - work.top) + " 下" + Num(work.bottom - r.bottom);
}

// 一次吸附的完整证据：喂入位置 → 吸附后位置 → 面板矩形（窗口坐标与屏幕像素都要）→
// 面板贴住的是哪条边（贴住 = 那条边到工作区对应边的距离变成 0）。
std::string SnapEvidence(const char* what, POINT fed, POINT got, const dshb::PanelGeometry& panel,
                         const RECT& work) {
    const RECT e = dshb::PanelRectForWindow(got, panel);
    const bool atLeft = e.left == work.left;
    const bool atRight = e.right == work.right;
    const bool atTop = e.top == work.top;
    const bool atBottom = e.bottom == work.bottom;
    std::string out = std::string(what) + "：喂窗口(" + Num(fed.x) + "," + Num(fed.y) + ") → 吸附后(" +
                      Num(got.x) + "," + Num(got.y) + ")；面板屏幕像素 " + PanelRectStr(got, panel) +
                      "（面板在窗口里的偏移 " + Num(panel.margin) + "，尺寸 " + Num(panel.width) +
                      "x" + Num(panel.height) + "）；" + GapsStr(got, panel, work) + "；贴住的边=";
    std::string edges;
    if (atLeft) edges += "左";
    if (atRight) edges += "右";
    if (atTop) edges += "上";
    if (atBottom) edges += "下";
    out += edges.empty() ? "（一条都没有）" : edges;
    return out;
}

// 钳制的证据：窗口矩形 + **面板**矩形 + 两个面积比（面板是判据，窗口只作对照）。
std::string ClampEvidence(const char* what, POINT fed, POINT got, const dshb::PanelGeometry& panel,
                          const RECT& work) {
    const RECT window{got.x, got.y, got.x + 2 * panel.margin + panel.width,
                      got.y + 2 * panel.margin + panel.height};
    const RECT entity = dshb::PanelRectForWindow(got, panel);
    return std::string(what) + " 喂(" + Num(fed.x) + "," + Num(fed.y) + ")->停在(" + Num(got.x) +
           "," + Num(got.y) + ") 窗口 " + RectStr(window) + " 面板 " + PanelRectStr(got, panel) +
           " 面板可见面积=" + Pct(dshb::VisibleAreaFraction(entity, work)) + "（窗口可见面积=" +
           Pct(dshb::VisibleAreaFraction(window, work)) + "）";
}

// ---------------------------------------------------------------------------
// (A) 跟手不漂（离线）：把同一串光标位置喂给生产代码那两个函数
// ---------------------------------------------------------------------------
void CaseFollowMath(Harness* h, const RECT& work) {
    const RECT window{work.left + 700, work.top + 300, work.left + 700 + dshb::kCanvasWidthDip,
                      work.top + 300 + dshb::kCanvasHeightDip};
    const POINT press{window.left + 120, window.top + 120};

    // 锚点口径：光标 − 窗口左上角（按下那一刻）。
    const POINT anchor = dshb::DragOffsetFor(window, press);
    const POINT anchorWant{press.x - window.left, press.y - window.top};

    // 一条拐弯而且会反向的路径：直线位移相等可以被"每帧加固定量"那种错误实现碰对，
    // 拐弯 + 反向的不行（那样窗口会漂到别处去）。
    const int dxs[] = {12, 12, 12, 12, 12,  12,  12,  12,  -9, -9,
                       -9, -9, -9, -9, 5,   5,   5,   5,   5,  5};
    const int dys[] = {7,  7,  7,  7,  -13, -13, -13, -13, 11, 11,
                       11, 11, 11, 11, -4,  -4,  -4,  -4,  -4, -4};
    const int steps = 20;

    int worstDeviation = 0;
    POINT lastCursor = press;
    std::string trail;
    for (int i = 0; i < steps; ++i) {
        lastCursor.x += dxs[i];
        lastCursor.y += dys[i];
        // 生产代码算出来的位置
        const POINT got = dshb::DragTopLeft(anchor, lastCursor);
        // "1:1 跟手"的定义：窗口位移 = 光标位移
        const POINT want{window.left + (lastCursor.x - press.x),
                         window.top + (lastCursor.y - press.y)};
        const int devX = got.x - want.x;
        const int devY = got.y - want.y;
        if (std::abs(devX) > worstDeviation) worstDeviation = std::abs(devX);
        if (std::abs(devY) > worstDeviation) worstDeviation = std::abs(devY);
        if (g_verbose) {
            std::printf("    follow 光标=(%ld,%ld) 位置=(%ld,%ld)\n", lastCursor.x, lastCursor.y,
                        got.x, got.y);
        }
    }
    const POINT firstGot = dshb::DragTopLeft(anchor, POINT{press.x + dxs[0], press.y + dys[0]});
    const POINT lastTopLeft = dshb::DragTopLeft(anchor, lastCursor);
    trail = "第1步(" + Num(firstGot.x) + "," + Num(firstGot.y) + ") -> 第20步(" +
            Num(lastTopLeft.x) + "," + Num(lastTopLeft.y) + ")";

    const bool anchorOk = anchor.x == anchorWant.x && anchor.y == anchorWant.y;
    h->Req("follow-math", "锚点口径 = 光标 − 窗口左上角；位置 = 光标 − 锚点 → 位移逐像素相等",
           "按下点(" + Num(press.x) + "," + Num(press.y) + ") 窗口 " + RectStr(window) +
               "：锚点=(" + Num(anchor.x) + "," + Num(anchor.y) + ")（定义值 (" +
               Num(anchorWant.x) + "," + Num(anchorWant.y) + ")）；走 20 步拐弯路径 " + trail +
               "；总位移 窗口=(" + Num(lastTopLeft.x - window.left) + "," +
               Num(lastTopLeft.y - window.top) + ") 光标=(" + Num(lastCursor.x - press.x) + "," +
               Num(lastCursor.y - press.y) + ")；最大偏差=" + Num(worstDeviation) + "px",
           anchorOk && worstDeviation == 0);
}

// ---------------------------------------------------------------------------
// (B0) 面板几何：窗口像素 → 面板矩形（吸附与"可见面积"都建立在这一步上）
// ---------------------------------------------------------------------------
void CasePanelGeometry(Harness* h) {
    // 设计值：画布 475x289 = 面板 315x129 + 两倍 80 余量。s=1 时必须逐个对上；
    // s≠1（--ui-scale/--dpi 改的就是窗口像素尺寸）时必须按同一个 s 缩放。
    struct Case {
        int windowW;
        int windowH;
        int wantMargin;
        int wantWidth;
        int wantHeight;
    };
    const Case cases[] = {
        {475, 289, 80, 315, 129},     // s = 1（默认）
        {950, 578, 160, 630, 258},    // s = 2（--ui-scale=2）
        {570, 347, 96, 378, 155},     // s = 1.2（--ui-scale=1.2，两个轴都取整后仍一致）
    };
    std::string evidence;
    bool all = true;
    for (const Case& c : cases) {
        const dshb::PanelGeometry p = dshb::PanelForWindow(SIZE{c.windowW, c.windowH});
        const bool ok = p.margin == c.wantMargin && p.width == c.wantWidth &&
                        p.height == c.wantHeight;
        if (!ok) all = false;
        if (!evidence.empty()) evidence += "; ";
        evidence += "窗口 " + Num(c.windowW) + "x" + Num(c.windowH) + " -> margin=" +
                    Num(p.margin) + " 面板=" + Num(p.width) + "x" + Num(p.height) + "（期望 " +
                    Num(c.wantMargin) + "/" + Num(c.wantWidth) + "x" + Num(c.wantHeight) + "）";
    }

    // 非等比窗口（探针合成输入）：两个轴各按自己的轴缩放，面板仍必须在窗口里，
    // 而不是"一半对一半错"的几何 —— 那种面板在屏幕上根本不存在，量出来的偏差也解释不了。
    {
        const dshb::PanelGeometry bad = dshb::PanelForWindow(SIZE{950, 289});
        const bool perAxis = bad.margin == 160 && bad.width == 630 && bad.height == 129;
        if (!perAxis) all = false;
        evidence += "; 窗口 950x289（非等比）-> margin=" + Num(bad.margin) + " 面板=" +
                    Num(bad.width) + "x" + Num(bad.height) + "（宽度按 cx、高度按 cy 各算各的）";
    }

    // 面板必须**在窗口里**，而且窗口就是画布：窗口 - 面板 = 两倍余量。
    {
        const dshb::PanelGeometry p{};
        const RECT r = dshb::PanelRectForWindow(POINT{1000, 500}, p);
        const bool inside = r.left == 1080 && r.top == 580 && r.right == 1395 && r.bottom == 709;
        if (!inside) all = false;
        evidence += "; 窗口左上角(1000,500) -> 面板 " + RectStr(r) + "（== (1080,580)-(1395,709)）";
    }
    h->Req("panel-geometry",
           "面板矩形由窗口尺寸推出（不写死 80/315/129）：s=1 / s=2 / s=1.2 三档都对，"
           "非等比窗口按各自轴算",
           evidence, all);
}

// ---------------------------------------------------------------------------
// (B) 钳制（离线）：喂边界输入，量结果面积
// ---------------------------------------------------------------------------
void CaseClamp(Harness* h, const RECT& realWork) {
    const dshb::PanelGeometry panel{};
    const RECT work{0, 0, 1920, 1080};   // 合成的显示器（真机上只有一块，量不到这些输入）
    const RECT tiny{0, 0, 320, 200};
    const int halfW = (panel.width + 1) / 2;
    const int halfH = (panel.height + 1) / 2;

    // ★ 期望值按**面板口径**重算（这是本步的口径变更，不是回归）：
    //   "面板最多从某条边出去半个面板" → 窗口边界的平移量是 (面板尺寸 − 半面板)：
    //   左极限 = −(315−158) − 80 = −237、右极限 = 1920 − 158 − 80 = 1682、
    //   上极限 = −(129−65) − 80 = −144、下极限 = 1080 − 65 − 80 = 935。
    //   旧口径（按窗口 475x289 算）给的是 −237/1682/−144/935 的**同一组数** —— 巧合：
    //   窗口半宽 238 = 面板剩余 157 + 80 余量 + 1 取整。所以四角的坐标没变，
    //   变的只有"停在那个坐标时可见的面积是多少"：旧数 50.11%（窗口），
    //   新数 100%×50% = 50.00% 左右（面板某一轴恰好露出一半）。
    struct Case {
        const char* what;
        POINT desired;
    };
    const Case cases[] = {
        {"屏内（不该动）", POINT{700, 400}},
        {"左上角外", POINT{-4000, -4000}},
        {"右上角外", POINT{9000, -4000}},
        {"左下角外", POINT{-4000, 9000}},
        {"右下角外", POINT{9000, 9000}},
        {"左侧多越界 1px（临界）", POINT{work.left - (panel.width - halfW) - panel.margin - 1, 400}},
        {"右侧多越界 1px（临界）", POINT{work.right - halfW - panel.margin + 1, 400}},
    };
    std::string evidence;
    bool all = true;
    for (const Case& c : cases) {
        const POINT got = dshb::ClampWindowTopLeftIn(c.desired, panel, work);
        const RECT entity = dshb::PanelRectForWindow(got, panel);
        const double fraction = dshb::VisibleAreaFraction(entity, work);
        const bool keepHalf = fraction >= dshb::kMinVisibleAreaFraction - 1e-9;
        // 面板每个轴至少露一半（面积判据的逐轴形式），且面板总能看见一块。
        const bool inside = entity.left >= work.left - (panel.width - halfW) &&
                            entity.left <= work.right - halfW &&
                            entity.top >= work.top - (panel.height - halfH) &&
                            entity.top <= work.bottom - halfH;
        // "屏内"那一条还要保证**没有被多钳**：位置在屏内时钳制必须是恒等变换，
        // 否则用户拖到屏幕中间也会发现窗口不在光标底下。
        const bool untouched = (std::strcmp(c.what, "屏内（不该动）") != 0) ||
                               (got.x == c.desired.x && got.y == c.desired.y);
        if (!keepHalf || !inside || !untouched) all = false;
        if (!evidence.empty()) evidence += "; ";
        evidence += ClampEvidence(c.what, c.desired, got, panel, work);
    }
    evidence += "；合成工作区 1920x1080，面板 " + Num(panel.width) + "x" + Num(panel.height) +
                "（+两倍余量 " + Num(panel.margin) + " = 窗口 " +
                Num(panel.width + 2 * panel.margin) + "x" + Num(panel.height + 2 * panel.margin) +
                "），半面板=" + Num(halfW) + "/" + Num(halfH) + "（面积判据要求它向上取整）";
    h->Req("clamp-edge",
           "四条边/四个角/两侧临界：**面板**可见面积一律 >= 50%，且屏内位置不被改动",
           evidence, all);

    // ★ 判据的核心差异，单独一条：**面板**可见面积掉到一半以下时，窗口口径可能还在放行。
    //   两个比都要算出来 —— "这次真的收紧了"只能拿两个数说话。
    {
        const RECT fedEntity = dshb::PanelRectForWindow(POINT{work.left - 250, work.top + 30}, panel);
        const RECT fedWindow{work.left - 250, work.top + 30, work.left - 250 + 2 * panel.margin + panel.width,
                             work.top + 30 + 2 * panel.margin + panel.height};
        const double fedPanelFraction = dshb::VisibleAreaFraction(fedEntity, work);
        const double fedWindowFraction = dshb::VisibleAreaFraction(fedWindow, work);

        // 同一个位置再往外一点：新口径拒绝，并把它拉回极限。
        const POINT deeper{work.left - 317, 400};
        const POINT got = dshb::ClampWindowTopLeftIn(deeper, panel, work);
        const RECT gotEntity = dshb::PanelRectForWindow(got, panel);
        const RECT deeperWindow{deeper.x, deeper.y, deeper.x + 2 * panel.margin + panel.width,
                                deeper.y + 2 * panel.margin + panel.height};
        h->Req("clamp-panel-over-window",
               "口径收紧的实证：面板可见 < 50% 的位置被**拒绝**并拉回极限（同一个位置的外框"
               "可见占比单独报出来，供对照）",
               "位置(" + Num(deeper.x) + ",400)：面板可见=" +
                   Pct(dshb::VisibleAreaFraction(dshb::PanelRectForWindow(deeper, panel), work)) +
                   "（< 50%，新口径拒），同一个位置的窗口可见=" +
                   Pct(dshb::VisibleAreaFraction(deeperWindow, work)) + "；钳制后停在(" +
                   Num(got.x) + "," + Num(got.y) + ") 面板 " + PanelRectStr(got, panel) +
                   " 面板可见=" + Pct(dshb::VisibleAreaFraction(gotEntity, work)) + "；另：(" +
                   Num(work.left - 250) + "," + Num(work.top + 30) + ") 面板可见=" + Pct(fedPanelFraction) +
                   " 窗口可见=" + Pct(fedWindowFraction),
               dshb::VisibleAreaFraction(dshb::PanelRectForWindow(deeper, panel), work) <
                       dshb::kMinVisibleAreaFraction &&
                   dshb::VisibleAreaFraction(gotEntity, work) >= dshb::kMinVisibleAreaFraction - 1e-9 &&
                   fedPanelFraction < dshb::kMinVisibleAreaFraction);
    }

    // 不变量：任意输入下，钳制之后**面板**可见面积都 >= 50%。用一组极端输入逐个重算 ——
    // 代码里那段"两个极限区"的分支是推出来的，不能只靠注释相信。
    {
        const int xs[] = {-4000, -1000, -500, -318, -317, -238, -237, -157, 0, 700, 1564, 1582,
                          1583, 1682, 1683, 9000};
        const int ys[] = {-4000, -1000, -300, -145, -144, -65, 0, 400, 870, 935, 936, 9000};
        int checked = 0;
        int worstAt = 0;
        double worst = 1.0;
        for (int x : xs) {
            for (int y : ys) {
                const POINT got = dshb::ClampWindowTopLeftIn(POINT{x, y}, panel, work);
                const RECT entity = dshb::PanelRectForWindow(got, panel);
                const double f = dshb::VisibleAreaFraction(entity, work);
                ++checked;
                if (f < worst) {
                    worst = f;
                    worstAt = (y == ys[0]) ? x : y;   // 只为定位是哪一格，数值本身是结论
                }
            }
        }
        h->Req("clamp-area-invariant",
               "不变量：任意输入钳制后**面板**可见面积 >= 50%（含比屏还靠外的输入）",
               Num(checked) + " 组输入（x∈{" + Num(xs[0]) + "…" + Num(xs[15]) + "} y∈{" + Num(ys[0]) +
                   "…" + Num(ys[11]) + "}）全部重算：最小面板可见面积=" + Pct(worst) +
                   "（出现在某组 xy，参考值=" + Num(worstAt) + "）",
               worst >= dshb::kMinVisibleAreaFraction - 1e-9 && checked == 16 * 12);
    }

    // 窗口比工作区还大（合成输入，真机上量不到）：判据取**面板**与工作区比 ——
    // 窗口 400x250 时面板 265x112，320x200 的工作区里两个轴都放得下，所以这不是"面板塞不下"
    // 的情况。规则要对任何输入给出一个满足判据的答案，量的就是这一条（外加"面板没有跑到窗口
    // 外面去"这个根本前提：推论错了面板就会画在窗口外，屏幕上什么也看不到）。
    {
        const SIZE big{400, 250};
        const dshb::PanelGeometry bigPanel = dshb::PanelForWindow(big);
        const POINT got = dshb::ClampWindowTopLeftIn(POINT{-9999, -9999}, bigPanel, tiny);
        const RECT entity = dshb::PanelRectForWindow(got, bigPanel);
        const double fraction = dshb::VisibleAreaFraction(entity, tiny);
        const RECT window{got.x, got.y, got.x + 2 * bigPanel.margin + bigPanel.width,
                          got.y + 2 * bigPanel.margin + bigPanel.height};
        const bool panelInsideWindow = entity.left >= window.left && entity.top >= window.top &&
                                       entity.right <= window.right &&
                                       entity.bottom <= window.bottom;
        h->Req("clamp-too-big",
               "合成输入（窗口比工作区大）：钳制仍给出满足判据的答案（面板可见面积 >= 50%），"
               "且面板始终在窗口里",
               "窗口 400x250 → 面板 " + Num(bigPanel.width) + "x" + Num(bigPanel.height) +
                   "（margin " + Num(bigPanel.margin) + "）塞进 320x200 的工作区：喂(-9999,-9999) -> "
                   "停在(" + Num(got.x) + "," + Num(got.y) + ") 窗口 " + RectStr(window) + " 面板 " +
                   RectStr(entity) + " 可见面积=" + Pct(fraction),
               fraction >= dshb::kMinVisibleAreaFraction - 1e-9 && panelInsideWindow);
    }

    // 真实显示器：工作区由 MonitorFromPoint/GetMonitorInfo 取（生产代码走的就是这条路）。
    {
        // ★ 变量名不能叫 far：windef.h 里 far/near 是空宏（16 位时代的遗产），
        //   写成 `const POINT far = ...` 会被展开成 `const POINT = ...`。
        const POINT farAway = dshb::ClampWindowTopLeft(
            POINT{realWork.right + 100000, realWork.bottom + 100000}, panel,
            POINT{realWork.left + 10, realWork.top + 10});
        const RECT entity = dshb::PanelRectForWindow(farAway, panel);
        const double fraction = dshb::VisibleAreaFraction(entity, realWork);
        h->Req("clamp-real-monitor",
               "MonitorFromPoint + GetMonitorInfo 这条路：钳到光标所在显示器的工作区",
               "真实工作区 " + RectStr(realWork) + "；喂一个远在天边的位置 -> 停在(" +
                   Num(farAway.x) + "," + Num(farAway.y) + ") 面板 " + PanelRectStr(farAway, panel) +
                   " 可见面积=" + Pct(fraction),
               fraction >= dshb::kMinVisibleAreaFraction - 1e-9);
    }
}

// ---------------------------------------------------------------------------
// (B2) 吸附（离线）：面板的最近一条边进范围就吸、两轴独立、拖走即脱
// ---------------------------------------------------------------------------
void CaseSnap(Harness* h, const RECT& work) {
    const dshb::PanelGeometry panel{};

    // ---- 1) 四条边各吸一次 ----
    {
        // 每条边都从"面板边距工作区边 10px（< 24 就该吸）"的位置喂进去；光标放在**未吸附**
        // 面板里、离要吸的那条边 100px（吸附量受"光标仍要在面板里"限制，这一条要按真实
        // 光标位置量）。期望值直接写成"面板的边贴住工作区的边"推出来的窗口坐标。
        struct EdgeCase {
            const char* what;
            POINT desired;     // 未吸附时想要的窗口左上角
            POINT want;        // 期望吸附后的窗口左上角
            const char* edge;  // 贴住的边
        };
        const int m = panel.margin;
        const int w = panel.width;
        const int hgt = panel.height;
        // ★ 名字不能叫 far：windef.h 里 far/near 是空宏（16 位时代的遗产），`far.y` 会被
        //   展开成 `.y` —— 报出来的是"聚合初始化语法错误"，与真正的原因隔了十万八千里。
        //   （这个文件在 clamp-real-monitor 那一处早就写过这条提醒，这里又踩了一次。）
        const int awayX = work.left + 600;
        const int awayY = work.top + 400;
        // ★ 每一条都按"**未吸附**面板贴在哪儿"写，窗口坐标由 − margin 推出来（不是按窗口算）。
        //   面板那条要吸的边只比工作区的边超出 10px（其余都在屏内），所以判据（最近那条边距
        //   10px ≤ 24）必命中。四条边的期望值都是"面板那条边正好落在工作区边上"。
        //   光标只是**记在证据里**：吸附量在两条外边（右/下）上必然大于光标到那条边的距离，
        //   面板会相对光标滑一点 —— 这是"面板贴住屏边"这条目标的必然代价，不是缺陷
        //   （光标是否还在面板里，由 snap-keeps-cursor 用一条吸得动的边单独量）。
        const EdgeCase cases[] = {
            {"左", POINT{work.left + 10 - m, awayY - m}, POINT{work.left - m, awayY - m}, "left"},
            {"右", POINT{work.right - w + 10 - m, awayY - m}, POINT{work.right - w - m, awayY - m},
             "right"},
            {"上", POINT{awayX - m, work.top + 10 - m}, POINT{awayX - m, work.top - m}, "top"},
            {"下", POINT{awayX - m, work.bottom - hgt + 10 - m}, POINT{awayX - m, work.bottom - hgt - m},
             "bottom"},
        };
        const int inset = 100;   // 光标放在面板里、离要吸的那条边 100px（记录用）
        std::string evidence;
        bool all = true;
        for (const EdgeCase& c : cases) {
            const bool toRight = (std::strcmp(c.edge, "right") == 0);
            const bool toBottom = (std::strcmp(c.edge, "bottom") == 0);
            const POINT cursor{toRight ? work.right - inset : work.left + inset,
                               toBottom ? work.bottom - inset : work.top + inset};
            const POINT got = dshb::SnapTopLeftIn(c.desired, panel, work);
            const RECT e = dshb::PanelRectForWindow(got, panel);
            const bool exact = got.x == c.want.x && got.y == c.want.y;
            const bool flush = (std::strcmp(c.edge, "left") == 0 && e.left == work.left) ||
                               (std::strcmp(c.edge, "right") == 0 && e.right == work.right) ||
                               (std::strcmp(c.edge, "top") == 0 && e.top == work.top) ||
                               (std::strcmp(c.edge, "bottom") == 0 && e.bottom == work.bottom);
            if (!exact || !flush) all = false;
            if (!evidence.empty()) evidence += "; ";
            evidence += SnapEvidence(c.what, c.desired, got, panel, work) + "（按下时记录的光标(" +
                        Num(cursor.x) + "," + Num(cursor.y) + ")）";
        }
        h->Req("snap-edges",
               "四条边各吸一次：面板的最近一条边进入 24px 就吸到工作区边上（吸的是面板，不是光标）",
               evidence, all);
    }

    // ---- 2) ±1px 临界：面板边距工作区边 24px 吸、25px 不吸 ----
    {
        std::string evidence;
        bool all = true;
        const int gaps[] = {24, 25};
        const char* names[] = {"左", "右", "上", "下"};
        for (int axis = 0; axis < 4; ++axis) {
            for (int gap : gaps) {
                const bool isLeft = (axis == 0);
                const bool isRight = (axis == 1);
                const bool isTop = (axis == 2);
                const bool wantSnap = (gap <= dshb::kSnapDistancePx);
                // 未吸附时面板的边距工作区对应边正好 gap 像素（判据就是这个数）。
                POINT desired{work.left + 600 - panel.margin, work.top + 400 - panel.margin};
                if (isLeft) desired.x = work.left + gap - panel.margin;
                if (isRight) desired.x = work.right - panel.width - gap - panel.margin;
                if (isTop) desired.y = work.top + gap - panel.margin;
                if (axis == 3) desired.y = work.bottom - panel.height - gap - panel.margin;
                // 光标放在面板里、离要吸的那条边 100px 的地方（这一条量的是吸附量本身，
                // 光标在不在面板里由 snap-keeps-cursor 单独量）。
                const int inset = 100;
                const POINT cursor{isLeft   ? work.left + inset
                                   : isRight ? work.right - inset
                                             : work.left + 600,
                                   isTop    ? work.top + inset
                                   : axis == 3 ? work.bottom - inset
                                               : work.top + 400};
                const POINT got = dshb::SnapTopLeftIn(desired, panel, work);
                const bool moved = got.x != desired.x || got.y != desired.y;
                // 吸了就必须**正好**贴住边；没吸就必须一个像素都不动（不能"吸一点点"）。
                const RECT e = dshb::PanelRectForWindow(got, panel);
                const bool flush = (isLeft && e.left == work.left) || (isRight && e.right == work.right) ||
                                   (isTop && e.top == work.top) ||
                                   (axis == 3 && e.bottom == work.bottom);
                const bool ok = (moved == wantSnap) && (!wantSnap || flush);
                if (!ok) all = false;
                if (!evidence.empty()) evidence += "; ";
                evidence += std::string(names[axis]) + "边距工作区 " + Num(gap) + "px：喂(" +
                            Num(desired.x) + "," + Num(desired.y) + ") -> (" + Num(got.x) + "," +
                            Num(got.y) + ")" + (moved ? "（吸）" : "（没吸）") + " " +
                            GapsStr(got, panel, work);
            }
        }
        h->Req("snap-threshold",
               "±1px 临界：面板边距工作区边 24px 吸、25px 不吸（判据是面板的边，不是光标）",
               evidence, all);
    }

    // ---- 3) 四个角：两轴同时吸 ----
    {
        struct Corner {
            const char* what;
            POINT desired;
            POINT want;
            POINT cursor;
        };
        const int m = panel.margin;
        const int w = panel.width;
        const int hgt = panel.height;
        // 面板的两条边各距工作区的两条边 12px（都进范围）
        const int d = 12;
        const Corner corners[] = {
            {"左上", POINT{work.left + d - m, work.top + d - m}, POINT{work.left - m, work.top - m},
             POINT{work.left + 100, work.top + 100}},
            {"右上", POINT{work.right - w - d - m, work.top + d - m},
             POINT{work.right - w - m, work.top - m},
             POINT{work.right - 100, work.top + 100}},
            {"左下", POINT{work.left + d - m, work.bottom - hgt - d - m},
             POINT{work.left - m, work.bottom - hgt - m},
             POINT{work.left + 100, work.bottom - 100}},
            {"右下", POINT{work.right - w - d - m, work.bottom - hgt - d - m},
             POINT{work.right - w - m, work.bottom - hgt - m},
             POINT{work.right - 100, work.bottom - 100}},
        };
        std::string evidence;
        bool all = true;
        for (const Corner& c : corners) {
            const POINT got = dshb::SnapTopLeftIn(c.desired, panel, work);
            const RECT e = dshb::PanelRectForWindow(got, panel);
            const bool exact = got.x == c.want.x && got.y == c.want.y;
            const bool cursorInside = c.cursor.x >= e.left && c.cursor.x < e.right &&
                                      c.cursor.y >= e.top && c.cursor.y < e.bottom;
            if (!exact || !cursorInside) all = false;
            if (!evidence.empty()) evidence += "; ";
            evidence += std::string(c.what) + "角：喂(" + Num(c.desired.x) + "," + Num(c.desired.y) +
                        ") → 吸附后(" + Num(got.x) + "," + Num(got.y) + ")，面板 " +
                        PanelRectStr(got, panel) + "，贴住 " + GapsStr(got, panel, work);
        }
        h->Req("snap-corners", "四角：两块轴各自独立判定，所以一个角上两块轴同时吸",
               evidence, all);
    }

    // ---- 4) 拖走立即脱开（不粘滞）：整条拖动序列逐像素核对 ----
    {
        // 按下时光标离面板左边 200px，面板左边距工作区左 380px。整条序列：
        // 未吸附的面板左边距屏 380（远，不吸）→ 24（吸住，面板贴住屏左边）→ 23（还吸着）
        // → 50（拖回屏内、出了 24px 范围，**立刻**脱开回到跟手值）。
        const POINT pressCursor{work.left + 300 + 200, work.top + 260};
        const RECT startWindow{work.left + 300, work.top + 160, work.left + 300 + 475,
                               work.top + 160 + 289};
        const POINT anchor = dshb::DragOffsetFor(startWindow, pressCursor);
        const int steps[] = {0, -356, -1, 30, -5};
        POINT cursor = pressCursor;
        std::string evidence;
        bool all = true;
        bool sawSnap = false;
        bool sawDetach = false;
        // 每一步都重算一遍（位置是光标坐标的纯函数，不依赖上一步的任何状态）。
        struct Step {
            int gapBefore;
            POINT cursor;
            POINT desired;
            POINT snapped;
            POINT final;
            bool snap;
        };
        std::vector<Step> trace;
        for (std::size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            cursor.x += steps[i];
            Step s{};
            s.cursor = cursor;
            s.desired = dshb::DragTopLeft(anchor, cursor);
            s.snapped = dshb::SnapTopLeftIn(s.desired, panel, work);
            s.final = dshb::ClampWindowTopLeftIn(s.snapped, panel, work);
            s.gapBefore = dshb::PanelRectForWindow(s.desired, panel).left - work.left;
            s.snap = s.snapped.x != s.desired.x || s.snapped.y != s.desired.y;
            trace.push_back(s);
        }
        for (std::size_t i = 0; i < trace.size(); ++i) {
            const Step& s = trace[i];
            const RECT after = dshb::PanelRectForWindow(s.snapped, panel);
            const int gapAfter = after.left - work.left;
            if (s.snap) {
                // 吸住时：真实距离在 0~24px 之间，且吸附后**正好**贴住边。
                sawSnap = true;
                if (s.gapBefore < 0 || s.gapBefore > dshb::kSnapDistancePx) all = false;
                if (gapAfter != 0) all = false;
                if (s.final.x != s.snapped.x || s.final.y != s.snapped.y) all = false;
            } else if (sawSnap) {
                // 脱离：未吸附的面板已经出了 24px 范围（判据回到"不吸"），窗口位置**逐像素**
                // 等于 光标 − 锚点（没有任何粘滞状态），而且这一条一直成立到拖动结束。
                sawDetach = true;
                if (s.final.x != s.desired.x || s.final.y != s.desired.y) all = false;
            }
            if (!s.snap && s.final.x != s.desired.x) all = false;   // 没吸时不许被别的东西挪走
            if (!evidence.empty()) evidence += "; ";
            evidence += "步" + Num(static_cast<long long>(i)) + " 光标位移" + Num(steps[i]) +
                        " 未吸附时面板左边距屏" + Num(s.gapBefore) + "px";
            if (s.snap) {
                evidence += " → 吸住，窗口(" + Num(s.snapped.x) + "," + Num(s.snapped.y) +
                            ")（跟手值本会是 (" + Num(s.desired.x) + "," + Num(s.desired.y) +
                            ")），面板左边距屏 " + Num(gapAfter) + "px";
            } else {
                evidence += " → 不吸，窗口(" + Num(s.final.x) + "," + Num(s.final.y) +
                            ") == 光标−锚点(" + Num(s.desired.x) + "," + Num(s.desired.y) + ")";
            }
        }
        (void)trace;
        if (g_verbose) {
            for (const Step& s : trace) {
                std::printf("    snap-seq 光标=(%ld,%ld) 未吸附=(%ld,%ld) 吸附=(%ld,%ld) 最终=(%ld,%ld)\n",
                            s.cursor.x, s.cursor.y, s.desired.x, s.desired.y, s.snapped.x, s.snapped.y,
                            s.final.x, s.final.y);
            }
        }
        h->Req("snap-release",
               "拖动中实时吸、拖走立即脱开：吸住那一步贴住边；之后未吸附面板一离开 24px 范围，"
               "窗口位置就**逐像素**回到 光标 − 锚点（没有粘滞状态）",
               "按下光标(" + Num(pressCursor.x) + "," + Num(pressCursor.y) + ") 锚点(" +
                   Num(anchor.x) + "," + Num(anchor.y) + ")（= 光标 − 窗口左上角，窗口起点(" +
                   Num(startWindow.left) + "," + Num(startWindow.top) + ")）；" + evidence,
               all && sawSnap && sawDetach);
    }

    // ---- 5) 吸住之后面板仍在光标下，且锤制修正量单独报出来 ----
    {
        // 光标拖在面板中央（离要吸的那条边 157px）：吸附量只有 24px，面板不会从光标底下
        // 走掉 —— 这一条量的就是"吸住之后光标还在面板里"，以及吸附量本身是多少。
        const int m = panel.margin;
        const POINT desired{work.left + 24 - m, work.top + 400 - m};   // 面板左边距屏边 24px
        const POINT cursor{work.left + 24 + 157, work.top + 400 + 64};  // 光标在面板中央附近
        const POINT got = dshb::SnapTopLeftIn(desired, panel, work);
        const RECT e = dshb::PanelRectForWindow(got, panel);
        const bool cursorInside = cursor.x >= e.left && cursor.x < e.right && cursor.y >= e.top &&
                                  cursor.y < e.bottom;
        const int snapX = got.x - desired.x;
        const int idealX = (work.left - m) - desired.x;
        h->Req("snap-keeps-cursor",
               "吸住之后光标仍在面板里（吸附只把那 24px 门限内的差挪掉，面板不会从手指底下滑走）",
               "光标(" + Num(cursor.x) + "," + Num(cursor.y) + ") 距面板左边 157px；未吸附窗口(" +
                   Num(desired.x) + "," + Num(desired.y) + ") 理想吸附量 x=" + Num(idealX) +
                   "px → 实际吸到 x=" + Num(got.x) + "（吸了 " + Num(snapX) + "px），面板 " +
                   PanelRectStr(got, panel) + "，" + GapsStr(got, panel, work) + "，光标仍在面板内=" +
                   (cursorInside ? "是" : "否"),
               cursorInside && snapX == idealX && e.left == work.left);
    }

    // ---- 6) 吸附是"位置的函数"：同一个位置喂两次必须给出同一个答案 ----
    {
        // 面板右边距工作区右边 7px（进范围），光标放在面板中间偏右 —— 同一个输入两次同答案。
        const POINT desired{work.right - panel.margin - panel.width + 7, work.top + 300 - panel.margin};
        const POINT cursor{desired.x + panel.margin + 100, desired.y + panel.margin + 60};
        const POINT a = dshb::SnapTopLeftIn(desired, panel, work);
        const POINT b = dshb::SnapTopLeftIn(desired, panel, work);
        const bool same = a.x == b.x && a.y == b.y;
        h->Req("snap-pure",
               R"(吸附没有状态：同一个输入给同一个答案（所以"拖走脱开"是恒等式，不是状态机）)",
               "喂(" + Num(desired.x) + "," + Num(desired.y) + ") 两次 -> (" + Num(a.x) + "," +
                   Num(a.y) + ") 与 (" + Num(b.x) + "," + Num(b.y) + ")",
               same);
    }
}

// ---------------------------------------------------------------------------
// (C) config.json 的读写与容错
// ---------------------------------------------------------------------------
void CaseConfig(Harness* h, const std::wstring& tempDir, const RECT& work) {
    const SIZE size{dshb::kCanvasWidthDip, dshb::kCanvasHeightDip};
    const dshb::PanelGeometry panel{};

    // 夹具文件都在 %TEMP%，用完删掉（--keep-temp 留着看）。四条固定的名字写在这里，
    // 是为了让这一段的每个用例都用同一个目录、而不是各自拼一个路径（也顺便保证
    // "临时文件用完删干净"这条不是靠每个分支自己记得）。
    const std::wstring missingPath = tempDir + L"dshb-dragprobe-missing.json";
    const std::wstring brokenPath = tempDir + L"dshb-dragprobe-broken.json";
    const std::wstring farPath = tempDir + L"dshb-dragprobe-far.json";
    const std::wstring edgePath = tempDir + L"dshb-dragprobe-edge.json";
    const std::wstring roundtripPath = tempDir + L"dshb-dragprobe-roundtrip.json";
    const std::wstring noDirPath = tempDir + L"dshb-dragprobe-nodir\\config.json";

    // 1) 文件不存在
    {
        const std::wstring& path = missingPath;
        DeleteFileW(path.c_str());
        std::wstring why;
        POINT got{};
        const bool ok = !dshb::LoadWindowPos(path, size, &got, &why);
        h->Req("config-missing", "文件不存在 -> 读失败（调用方退回 240,240）",
               "LoadWindowPos 返回 " + std::string(ok ? "false" : "true") + "，原因：\"" +
                   Narrow(why) + "\"",
               ok);
    }

    // 2) 损坏的文件
    {
        const std::wstring& path = brokenPath;
        WriteFileText(path, "{\"windowX\": 240, \"windowY\":");
        std::wstring why;
        POINT got{};
        const bool ok = !dshb::LoadWindowPos(path, size, &got, &why);
        h->Req("config-broken", "JSON 损坏 -> 读失败，不崩",
               "内容=\"{\\\"windowX\\\": 240, \\\"windowY\\\":\"；结果=" +
                   std::string(ok ? "读失败（正确）" : "竟然读成功了") + "，原因：\"" + Narrow(why) +
                   "\"",
               ok);
    }

    // 3) 坐标不在任何显示器内 -> 拒；**面板还能看见至少一半**的位置 -> 收
    {
        std::wstring why;
        const bool wroteFar =
            dshb::SaveWindowPos(farPath, POINT{work.left + 900000, work.top + 900000}, &why);
        POINT got{};
        std::wstring why2;
        const bool rejected = wroteFar && !dshb::LoadWindowPos(farPath, size, &got, &why2);

        // ★ 反向的一条同样重要：钳制本来就允许面板半个身子悬在屏外，那种位置的左上角可能
        //   已经在屏外。只判"左上角在某块显示器上"的实现会把它丢掉，用户看到的是
        //   "拖到屏幕边上、重开又回到中间"。
        // ★ 这条判据的口径本步改了（窗口 → 面板），所以期望值也变了：
        //   x = work.left − 237 是**新的**极限（面板右边还剩 158px = 半面板，面板可见 50.16%）。
        //   旧口径也有个极限，但两个极限不是同一件事：外框可见 50% 的位置上（旧"合法"的
        //   x = work.left − 317）面板其实只剩 24.7% 可见，所以那种位置现在会被**拒**。
        //   表格里两行都要量，否则"收紧了"这句话没有证据。
        struct Accept {
            const char* what;
            POINT pos;
            bool wantAccept;
        };
        const int m = panel.margin;   // = 80（s=1）
        const Accept accepts[] = {
            {"面板露出 50.16%（x 正好在新的极限上）",
             POINT{work.left - (panel.width - (panel.width + 1) / 2) - m, work.top + 50}, true},
            {"再往外 1px（面板只剩 49.84%）",
             POINT{work.left - (panel.width - (panel.width + 1) / 2) - m - 1, work.top + 50}, false},
            {"外框可见 37%（旧口径也放行的那种位置，面板只剩 19%）",
             POINT{work.left - 300, work.top + 63}, false},
        };
        std::string detail;
        bool all = rejected;
        for (const Accept& a : accepts) {
            const std::wstring path = edgePath + std::to_wstring(a.pos.x) + L".json";
            std::wstring why3;
            const bool wrote = dshb::SaveWindowPos(path, a.pos, &why3);
            POINT back{};
            std::wstring why4;
            const bool accepted =
                wrote && dshb::LoadWindowPos(path, size, &back, &why4) && back.x == a.pos.x &&
                back.y == a.pos.y;
            const RECT entity = dshb::PanelRectForWindow(a.pos, panel);
            const double fraction = dshb::VisibleAreaFraction(entity, work);
            if (accepted != a.wantAccept) all = false;
            if (!detail.empty()) detail += "；";
            detail += std::string(a.what) + " (" + Num(a.pos.x) + "," + Num(a.pos.y) + ") 面板可见=" +
                      Pct(fraction) + " -> " +
                      (accepted ? (a.wantAccept ? "被接受（正确）" : "被接受（错）")
                                : (a.wantAccept ? "被拒（错）" : "被拒（正确）"));
            if (!g_keepTemp) DeleteFileW(path.c_str());
        }
        h->Req("config-offscreen",
               "不在任何显示器内 -> 拒；**面板**至少一半可见的贴边位置 -> 收（口径变更后"
               "有一批以前合法、现在被拒的位置）",
               "(" + Num(work.left + 900000) + "," + Num(work.top + 900000) + ") -> " +
                   std::string(rejected ? "被拒" : "被接受（错）") + "（原因：\"" + Narrow(why2) +
                   "\"）；" + detail,
               all);
    }

    // 4) 往返 + 别人的字段必须原样留着
    {
        const std::wstring& path = roundtripPath;
        WriteFileText(path,
                      "{\n  \"windowX\": 111,\n  \"windowY\": 222,\n  \"futureSetting\": \"keep "
                      "me\",\n  \"nested\": {\"a\": [1, 2, {\"b\": \"x\\ny\"}], \"c\": null},\n  "
                      "\"z\": true\n}\n");
        POINT before{};
        std::wstring why;
        const bool loaded = dshb::LoadWindowPos(path, size, &before, &why) && before.x == 111 &&
                            before.y == 222;
        std::wstring why2;
        const POINT movedTo{work.left + 200, work.top + 120};
        const bool saved = dshb::SaveWindowPos(path, movedTo, &why2);
        POINT after{};
        std::wstring why3;
        const bool reloaded =
            dshb::LoadWindowPos(path, size, &after, &why3) && after.x == movedTo.x &&
            after.y == movedTo.y;

        std::string text;
        ReadFileText(path, &text);
        const bool kept = text.find("futureSetting") != std::string::npos &&
                          text.find("keep me") != std::string::npos &&
                          text.find("nested") != std::string::npos &&
                          text.find("\"z\"") != std::string::npos &&
                          text.find("true") != std::string::npos &&
                          text.find("null") != std::string::npos &&
                          text.find("x\\ny") != std::string::npos;
        // 坐标要**按新的那对数**判（以前写死 "333"/"444"，那是旧夹具的坐标）。
        const bool coordsReplaced = text.find(Num(movedTo.x)) != std::string::npos &&
                                    text.find(Num(movedTo.y)) != std::string::npos &&
                                    text.find("111") == std::string::npos;
        h->Req("config-roundtrip", "读写往返：换成新坐标，别人的字段原样留着（文件以后会加设置）",
               "(111,222)->读回(" + Num(before.x) + "," + Num(before.y) + ")；写入(" +
                   Num(movedTo.x) + "," + Num(movedTo.y) + ") 后读回(" + Num(after.x) + "," +
                   Num(after.y) + ")；futureSetting/nested/z/null/转义仍在=" +
                   std::string(kept ? "是" : "否") + "；新坐标已替换=" +
                   std::string(coordsReplaced ? "是" : "否") + "；文件现在的内容：" + OneLine(text),
               loaded && saved && reloaded && kept && coordsReplaced);
    }

    // 5) 写不进去（目录不存在）：返回 false、说得出原因、不崩
    {
        std::wstring why;
        const bool failed = !dshb::SaveWindowPos(noDirPath, POINT{10, 10}, &why);
        h->Req("config-write-fail", "写失败（目录不存在）-> false + 原因，不崩",
               "原因：\"" + Narrow(why) + "\"", failed);
    }

    // 用完删干净（--keep-temp 留着看）。删除只删夹具文件，不碰任何别的东西。
    if (!g_keepTemp) {
        const std::wstring paths[] = {missingPath, brokenPath, farPath, edgePath, roundtripPath};
        for (const std::wstring& path : paths) DeleteFileW(path.c_str());
    }
}

// ---------------------------------------------------------------------------
// (D) 真机：注入真实鼠标输入，量真实窗口
// ---------------------------------------------------------------------------
struct DragOutcome {
    bool injected = false;
    int maxDeviation = 0;
    int maxWaitMs = 0;
    POINT finalTopLeft{};
    std::string why;
};

// 一次"按下 → 沿着 path 走 → 还按着"。expectExact=false 用于"往屏外推"的拖动：
// 那种情况下窗口会被钳住，期望值本来就不该到达，只能等它停下来。
DragOutcome InjectDrag(HWND hwnd, POINT press, const std::vector<POINT>& path, bool expectExact) {
    DragOutcome out{};
    SetCursor(press);
    Sleep(2);
    if (!PointIs(hwnd, press)) {
        out.why = "按压点(" + Num(press.x) + "," + Num(press.y) +
                  ")上的窗口不是被测窗口（WindowFromPoint 判定）—— **没有按下**";
        return out;
    }
    const POINT startTopLeft = TopLeftOf(hwnd);
    if (!ButtonInput(true)) {
        out.why = "SendInput(LEFTDOWN) 被拒";
        return out;
    }
    out.injected = true;
    // ★ 按下之后要**停一下**再动光标：窗口过程是在它自己那一拍里读 GetCursorPos 来定锚点的，
    //   注入完立刻把光标甩走的话，它读到的已经是移动后的位置 —— 整个拖动就带着一个常数偏移
    //   （实测：偏移正好等于第一步的位移）。人的第一次移动也在按下之后几十毫秒才发生，
    //   所以这里等 60ms 才是"像人那样拖"，不是在迁就某个实现。
    Sleep(60);

    for (const POINT& step : path) {
        SetCursor(step);
        if (!expectExact) {
            Sleep(30);
            continue;
        }
        const POINT want{startTopLeft.x + (step.x - press.x), startTopLeft.y + (step.y - press.y)};
        int waited = 0;
        const POINT got = WaitForTopLeft(hwnd, want, 300, &waited);
        if (waited > out.maxWaitMs) out.maxWaitMs = waited;
        const int devX = got.x - want.x;
        const int devY = got.y - want.y;
        if (std::abs(devX) > out.maxDeviation) out.maxDeviation = std::abs(devX);
        if (std::abs(devY) > out.maxDeviation) out.maxDeviation = std::abs(devY);
        if (g_verbose) {
            std::printf("    step 光标=(%ld,%ld) 期望=(%ld,%ld) 实测=(%ld,%ld) 等了%dms\n", step.x,
                        step.y, want.x, want.y, got.x, got.y, waited);
        }
    }
    out.finalTopLeft = TopLeftOf(hwnd);
    return out;
}

void ReleaseDrag() {
    ButtonInput(false);
    Sleep(40);
}

// 把窗口拖到指定位置（用于"停在某个好位置"以及每次边界测试之前把它放回屏中间）。
bool DragTo(HWND hwnd, POINT destination, const RECT& work, Harness* h, const char* id,
            std::string* detail) {
    const RECT win = RectOf(hwnd);
    POINT press{};
    if (!VisibleEntityPoint(win, work, 40, 40, &press)) {
        if (detail) *detail = "窗口在屏上没有可点的实体区";
        h->Req(id, "注入拖动", detail ? *detail : "", false);
        return false;
    }
    const POINT offset{press.x - win.left, press.y - win.top};
    const POINT target{destination.x + offset.x, destination.y + offset.y};
    const DragOutcome out = InjectDrag(hwnd, press, std::vector<POINT>{target}, true);
    if (!out.injected) {
        if (detail) *detail = out.why;
        h->Req(id, "注入拖动", out.why, false);
        return false;
    }
    const POINT finalTopLeft = WaitForStableTopLeft(hwnd, 400, nullptr);
    ReleaseDrag();
    const bool ok = finalTopLeft.x == destination.x && finalTopLeft.y == destination.y;
    if (detail) {
        *detail = "想放在(" + Num(destination.x) + "," + Num(destination.y) + ") -> 实测(" +
                  Num(finalTopLeft.x) + "," + Num(finalTopLeft.y) + ")，最大偏差=" +
                  Num(out.maxDeviation) + "px";
    }
    h->Req(id, "把面板拖到屏内指定位置（1:1，且钳制不该插手）", detail ? *detail : "", ok);
    return ok;
}

// 一次"把面板推到某条屏边"的拖动：按住靠**对侧**的点，把光标推到工作区边上。
// 为什么按对侧：窗口位置 = 光标 − 锚点，锚点在窗口左边时窗口才伸得出去。
void DragOffEdge(HWND hwnd, char which, const RECT& work, Harness* h, const char* id) {
    const RECT win = RectOf(hwnd);
    POINT press{};
    int preferX = 0;
    int preferY = 0;
    switch (which) {
    case 'R': preferX = 16; preferY = 64; break;
    case 'L': preferX = dshb::kEntityWidthDip - 16; preferY = 64; break;
    case 'U': preferX = dshb::kEntityWidthDip / 2; preferY = dshb::kEntityHeightDip - 16; break;
    default: preferX = dshb::kEntityWidthDip / 2; preferY = 16; break;
    }
    if (!VisibleEntityPoint(win, work, preferX, preferY, &press)) {
        h->Req(id, "拖出屏外之后仍要 >= 50% 面积在工作区内", "窗口在屏上没有可点的实体区", false);
        return;
    }
    POINT target = press;
    switch (which) {
    case 'R': target.x = work.right - 1; break;
    case 'L': target.x = work.left; break;
    case 'U': target.y = work.top; break;
    default: target.y = work.bottom - 1; break;
    }
    const DragOutcome out = InjectDrag(hwnd, press, std::vector<POINT>{target}, false);
    if (!out.injected) {
        h->Req(id, "拖出屏外之后仍要 >= 50% 面积在工作区内", out.why, false);
        return;
    }
    int waited = 0;
    WaitForStableTopLeft(hwnd, 400, &waited);
    const RECT finalRect = RectOf(hwnd);
    // 判据是**面板**的可见面积（本步改的口径），窗口外框只作对照：面板贴边时外框本来就
    // 越界 80px（那圈透明余量），拿外框算会得出一个吓人的数而与用户看到的东西无关。
    // ★ 口径变更的直接后果：贴边的位置现在是"面板的那条边 == 工作区的那条边"（面板整块在屏内），
    //   不再是"窗口一半悬在屏外"。所以这里量的是**面板贴住那条边**，而不是"越界"。
    const RECT finalEntity = dshb::PanelRectForWindow(POINT{finalRect.left, finalRect.top}, kPanel1to1);
    const double fraction = dshb::VisibleAreaFraction(finalEntity, work);
    const bool flushAtEdge = (which == 'R' && finalEntity.right == work.right) ||
                             (which == 'L' && finalEntity.left == work.left) ||
                             (which == 'U' && finalEntity.top == work.top) ||
                             (which == 'D' && finalEntity.bottom == work.bottom);
    const char* name = (which == 'R') ? "右" : (which == 'L') ? "左" : (which == 'U') ? "上" : "下";
    h->Req(id, "推到屏边之后**面板**仍要 >= 50% 面积在工作区内，且面板正好贴住那条边",
           std::string("往") + name + "拖：按下(" + Num(press.x) + "," + Num(press.y) + ") 光标推到(" +
               Num(target.x) + "," + Num(target.y) + ") -> 窗口停在 " + RectStr(finalRect) +
               "（面板 " + RectStr(finalEntity) + "），面板可见面积=" + Pct(fraction) +
               "，面板贴住工作区那条边=" + (flushAtEdge ? "是" : "否"),
           fraction >= dshb::kMinVisibleAreaFraction - 1e-9 && flushAtEdge);
    ReleaseDrag();
}

struct LiveResult {
    bool ran = false;
    POINT park{};
    std::string why;
};

LiveResult RunLive(Harness* h, POINT park) {
    LiveResult result{};
    const std::wstring token = L"dshb-dragprobe-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring configPath =
        g_realConfig ? dshb::Paths().config : TempPath(token + L"-config.json");
    const std::wstring curvePath = TempPath(token + L"-curve.json");
    const SIZE size{dshb::kCanvasWidthDip, dshb::kCanvasHeightDip};

    POINT savedCursor{};
    GetCursorPos(&savedCursor);

    // 临时文件在这段结束时**一定**要删干净，早退路径也一样 —— 所以交给一个小对象，
    // 而不是在每个 return 之前记得调一次（第一版就是那样，失败路径在 %TEMP% 里漏了 5 个文件）。
    // 真实 config 永远不删（--real-config 的意义就是用它）。
    struct TempFiles {
        std::wstring config;
        std::wstring curve;
        bool keep = false;
        bool realConfig = false;
        ~TempFiles() {
            if (keep) return;
            if (!realConfig && !config.empty()) DeleteFileW(config.c_str());
            if (!curve.empty()) DeleteFileW(curve.c_str());
        }
    } tempFiles{configPath, curvePath, g_keepTemp, g_realConfig};

    std::printf("\n[live] 被测程序: %s\n", Narrow(g_exePath).c_str());
    std::printf("[live] config 文件: %s（%s）\n", Narrow(configPath).c_str(),
                g_realConfig ? "所有者真实路径" : "临时文件");
    std::printf("[live] 曲线文件（测试用，避免写所有者的 curve.json）: %s\n",
                Narrow(curvePath).c_str());

    const dshb::WorkArea area = dshb::WorkAreaForPoint(POINT{0, 0});
    if (!area.valid) {
        result.why = "取不到显示器工作区";
        return result;
    }
    const RECT work = area.rect;
    // 停靠点先收进工作区（合法位置）。用的是**面板**口径的钳制（与生产同一条路）。
    park = dshb::ClampWindowTopLeftIn(park, kPanel1to1, work);
    std::printf("[live] 工作区 %s，窗口应为 %ldx%ld；停靠点 (%ld,%ld)\n", RectStr(work).c_str(),
                size.cx, size.cy, park.x, park.y);

    // ---- 0) 没有 config 时退回 (240,240)（一次短跑，不注入任何输入）----
    {
        Child child{};
        const std::wstring freshPath = TempPath(token + L"-fresh.json");
        DeleteFileW(freshPath.c_str());
        if (!LaunchChild(&child, freshPath, curvePath, 4)) {
            result.why = "子进程起不来，或 8 秒内没建出窗口";
            StopChild(&child);
            return result;
        }
        Sleep(150);
        const RECT r = RectOf(child.hwnd);
        const std::wstring line = WaitForLine(child, L"退回", 2000);
        h->Req("live-default-position", "没有 config.json 时起点必须是 (240,240)",
               "窗口矩形=" + RectStr(r) + "；子进程日志：" +
                   (line.empty() ? std::string("(没找到)") : "\"" + Narrow(line) + "\""),
               r.left == dshb::kWindowXDefault && r.top == dshb::kWindowYDefault && !line.empty());
        StopChild(&child);
    }

    // ---- 1) 从 config.json 起动 ----
    Child child{};
    POINT startPosition{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        const POINT candidates[] = {
            POINT{work.right - size.cx - 200, work.top + 200},
            POINT{work.right - size.cx - 200, work.bottom - size.cy - 200},
            POINT{work.left + 200, work.bottom - size.cy - 200},
            POINT{work.left + (work.right - work.left - size.cx) / 2, work.top + 120},
        };
        startPosition = candidates[attempt];
        std::wstring why;
        if (!dshb::SaveWindowPos(configPath, startPosition, &why)) {
            result.why = "写不了 config：" + Narrow(why);
            return result;
        }
        if (!LaunchChild(&child, configPath, curvePath, 25)) {
            result.why = "子进程起不来，或 8 秒内没建出窗口";
            StopChild(&child);
            return result;
        }
        const RECT r = RectOf(child.hwnd);
        (void)r;
        // ★ 必须**等**：窗口建出来之后要先画一帧，DirectComposition 才有内容；在那之前
        //   整个窗口是透明的，按 alpha 命中测试会漏到下面的窗口去（第一版就是这样，
        //   四个候选位置全被判成"被别的窗口挡着"，一次注入都没发生）。
        bool clear = false;
        std::string blocked;
        const unsigned long long waitStart = GetTickCount64();
        for (;;) {
            const RECT cur = RectOf(child.hwnd);
            POINT points[4]{};
            const bool have =
                VisibleEntityPoint(cur, work, 16, 64, &points[0]) &&
                VisibleEntityPoint(cur, work, dshb::kEntityWidthDip - 16, 64, &points[1]) &&
                VisibleEntityPoint(cur, work, dshb::kEntityWidthDip / 2, 16, &points[2]) &&
                VisibleEntityPoint(cur, work, dshb::kEntityWidthDip / 2,
                                   dshb::kEntityHeightDip - 16, &points[3]);
            clear = have;
            blocked.clear();
            if (have) {
                for (const POINT& p : points) {
                    if (!PointIs(child.hwnd, p)) {
                        clear = false;
                        blocked += " (" + Num(p.x) + "," + Num(p.y) + ")上=" + WindowDescAt(p);
                    }
                }
            } else {
                blocked = " 算不出可点的实体区（窗口 " + RectStr(cur) + "，工作区 " +
                          RectStr(work) + "）";
            }
            if (clear) break;
            if (GetTickCount64() - waitStart >= 2500) break;
            Sleep(40);
        }
        (void)r;
        if (clear) break;
        std::printf("[live] 候选起点(%ld,%ld) 不能安全注入：%s\n", startPosition.x,
                    startPosition.y, blocked.c_str());
        StopChild(&child);
        child = Child{};
        if (attempt == 3) {
            result.why = "四个按压点都被别的窗口占着，没有能安全注入的地方（最后一条：" + blocked +
                         "）";
            return result;
        }
    }
    result.ran = true;

    const RECT startRect = RectOf(child.hwnd);
    const SIZE sizeGot = SizeOf(child.hwnd);
    {
        const std::wstring line = WaitForLine(child, L"起点取自", 2000);
        h->Req("live-start-from-config", "起点由 config.json 决定（写进去什么就落在什么位置）",
               "config 里写 (" + Num(startPosition.x) + "," + Num(startPosition.y) + ") -> 窗口矩形 " +
                   RectStr(startRect) + "，尺寸 " + Num(sizeGot.cx) + "x" + Num(sizeGot.cy) +
                   "（应为 " + Num(size.cx) + "x" + Num(size.cy) + "）；子进程日志：" +
                   (line.empty() ? std::string("(没找到)") : "\"" + Narrow(line) + "\""),
               startRect.left == startPosition.x && startRect.top == startPosition.y &&
                   sizeGot.cx == size.cx && sizeGot.cy == size.cy);
    }

    // ---- 2) 跟手不漂（真机）----
    {
        POINT press{};
        if (!VisibleEntityPoint(startRect, work, 40, 40, &press)) {
            result.why = "起点窗口上没有可点的实体区";
            StopChild(&child);
            return result;
        }
        const int dxs[] = {12, 12, 12, 12, 12,  12,  12,  12,  -9, -9,
                           -9, -9, -9, -9, 5,   5,   5,   5,   5,  5};
        const int dys[] = {7,  7,  7,  7,  -13, -13, -13, -13, 11, 11,
                           11, 11, 11, 11, -4,  -4,  -4,  -4,  -4, -4};
        std::vector<POINT> path;
        POINT cursor = press;
        for (int i = 0; i < 20; ++i) {
            cursor.x += dxs[i];
            cursor.y += dys[i];
            path.push_back(cursor);
        }
        const DragOutcome out = InjectDrag(child.hwnd, press, path, true);
        if (!out.injected) {
            StopChild(&child);
            result.why = out.why;
            return result;
        }
        const RECT endRect = RectOf(child.hwnd);
        const POINT expect{startRect.left + (cursor.x - press.x), startRect.top + (cursor.y - press.y)};
        h->Req("live-follow", "真机跟手：20 步拐弯路径，窗口位移逐像素等于光标位移",
               "按下(" + Num(press.x) + "," + Num(press.y) + ") 起点 " + RectStr(startRect) +
                   " -> 终点 " + RectStr(endRect) + "；期望左上角(" + Num(expect.x) + "," +
                   Num(expect.y) + ")；路径总位移 光标=(" + Num(cursor.x - press.x) + "," +
                   Num(cursor.y - press.y) + ") 窗口=(" + Num(endRect.left - startRect.left) + "," +
                   Num(endRect.top - startRect.top) + ")；最大偏差=" + Num(out.maxDeviation) +
                   "px；单步最长等待 " + Num(out.maxWaitMs) + "ms",
               out.maxDeviation == 0 && endRect.left == expect.x && endRect.top == expect.y);
        ReleaseDrag();
    }

    // ---- 3) 四条边各拖一次（每次先把面板放回屏中间，否则按压点会落在屏外）----
    {
        const POINT middle{work.left + 200, work.top + 200};
        std::string detail;
        const char* edges = "RLUD";
        for (int i = 0; i < 4; ++i) {
            if (!DragTo(child.hwnd, middle, work, h, "live-recenter", &detail)) break;
            char id[32];
            std::snprintf(id, sizeof(id), "live-clamp-%c", edges[i]);
            DragOffEdge(child.hwnd, edges[i], work, h, id);
        }
        // 放回屏内，免得后面量持久化时窗口贴在边上（停靠点本身就在屏内）
        DragTo(child.hwnd, middle, work, h, "live-recenter-2", &detail);
    }

    // ---- 4) 停在指定位置 ----
    {
        std::string detail;
        DragTo(child.hwnd, park, work, h, "live-park", &detail);
    }

    // ---- 4b) 真机吸附：把光标停在"面板右边距右屏边 24px 以内"的位置，量真实窗口 ----
    //      ★ 这一步是**最后**一次拖动，所以下面的持久化两条量的正是它：松手写进 config.json
    //        的必须是吸附后的坐标，重开也落在那个位置上。
    {
        const RECT win = RectOf(child.hwnd);
        POINT press{};
        if (!VisibleEntityPoint(win, work, 120, 64, &press)) {
            h->Req("live-snap-right", "真机吸附：面板贴住工作区右边", "窗口上没有可点的实体区",
                   false);
        } else {
            // 生产里的顺序是"先吸附、再钳制"，探针照同一条路算期望值（不自己另写一套）。
            // 面板右边距右屏边 17px（进 24px 范围，所以那一轴一定会吸）。
            const POINT plain{work.right - kPanel1to1.margin - kPanel1to1.width + 17, win.top};
            const POINT targetCursor{press.x + (plain.x - win.left), press.y};
            const POINT snapped = dshb::SnapTopLeftIn(plain, kPanel1to1, work);
            const POINT clamped = dshb::ClampWindowTopLeftIn(snapped, kPanel1to1, work);
            const DragOutcome out =
                InjectDrag(child.hwnd, press, std::vector<POINT>{targetCursor}, false);
            int waited = 0;
            const POINT got = WaitForStableTopLeft(child.hwnd, 400, &waited);
            const RECT entity = dshb::PanelRectForWindow(got, kPanel1to1);
            const bool cursorInside = targetCursor.x >= entity.left && targetCursor.x < entity.right &&
                                      targetCursor.y >= entity.top && targetCursor.y < entity.bottom;
            h->Req("live-snap-right",
                   "真机：面板边进入 24px 就吸到工作区边上（窗口停在吸附值，不是跟手值），"
                   "且光标仍在面板里",
                   "按下(" + Num(press.x) + "," + Num(press.y) + ")（离面板左边 " +
                       Num(press.x - win.left - kPanel1to1.margin) + "px）光标推到(" +
                       Num(targetCursor.x) + "," + Num(targetCursor.y) + ")；不吸附时窗口会在(" +
                       Num(plain.x) + "," + Num(plain.y) + ")，吸附值(" + Num(snapped.x) + "," +
                       Num(snapped.y) + ")，钳制后(" + Num(clamped.x) + "," + Num(clamped.y) +
                       ")；实测窗口停在(" + Num(got.x) + "," + Num(got.y) + ")（等了 " +
                       Num(waited) + "ms），面板 " + PanelRectStr(got, kPanel1to1) + "，" +
                       GapsStr(got, kPanel1to1, work) + "，光标仍在面板内=" +
                       (cursorInside ? "是" : "否"),
                   out.injected && got.x == clamped.x && got.y == clamped.y &&
                       entity.right == work.right && cursorInside);
            ReleaseDrag();
            result.park = TopLeftOf(child.hwnd);
        }
    }

    // ---- 5) 等它自己退出（--seconds 到了就退，[render] 那行汇总才会写出来）----
    {
        const unsigned long long start = GetTickCount64();
        while (WaitForSingleObject(child.pi.hProcess, 100) == WAIT_TIMEOUT &&
               GetTickCount64() - start < 35000) {
        }
    }
    const std::vector<std::wstring> lines = ChildLines(child, L"[drag]");
    std::printf("\n[live] 子进程这一次运行写下的 [drag] 行：\n");
    for (const std::wstring& line : lines) std::printf("    %s\n", Narrow(line).c_str());
    double runFrames = 0.0;
    double runElapsed = 0.0;
    double runAvgRender = 0.0;
    {
        const std::string tail = ReadAppended(LogPath(), child.logFrom);
        std::printf("[live] 子进程日志尾部（[render] 汇总那一行在最后）：\n");
        std::size_t pos = 0;
        int printed = 0;
        std::vector<std::string> tailLines;
        while (pos < tail.size()) {
            std::size_t eol = tail.find('\n', pos);
            if (eol == std::string::npos) eol = tail.size();
            tailLines.push_back(tail.substr(pos, eol - pos));
            pos = eol + 1;
        }
        for (std::size_t i = 0; i < tailLines.size(); ++i) {
            const std::string& line = tailLines[i];
            if (line.find("[render] frames=") != std::string::npos) {
                NumberAfter(line, "frames=", &runFrames);
                NumberAfter(line, "elapsed=", &runElapsed);
                NumberAfter(line, "平均单帧=", &runAvgRender);
            }
            if (line.find("[render] frames=") != std::string::npos ||
                line.find("[win] 窗口矩形") != std::string::npos ||
                line.find("[win] 自检完成") != std::string::npos) {
                std::printf("    %s\n", line.c_str());
                ++printed;
            }
        }
        if (printed == 0) std::printf("    (没找到 [render]/[win] 汇总行)\n");
    }
    {
        // ① 偏差：**没被钳制**的拖动，偏差必须逐像素为 0（那是被测程序自己算出来的数，
        //    与探针量到的最大偏差互为交叉验证）。被钳制的那几次本来就该偏 —— 窗口停在
        //    屏边、位移小于光标位移正是钳制的定义，所以它们单独查：偏差必须**等于**钳制量。
        const int releases = CountWith(lines, L"[drag] 松手");
        const int zeroDev = CountWith(lines, L"偏差=(0,0)");
        const int clamped = CountWith(lines, L"钳制=是");
        const int clampNotes = CountWith(lines, L"钳制生效：不钳制时想要");
        int pairsChecked = 0;
        int pairsOk = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string out = OneLine(Narrow(lines[i]));
            if (out.find("[drag] 松手") == std::string::npos ||
                out.find("钳制=是") == std::string::npos) {
                continue;
            }
            for (std::size_t j = i; j < lines.size(); ++j) {
                const std::string note = OneLine(Narrow(lines[j]));
                if (note.find("钳制生效：不钳制时想要") == std::string::npos) continue;
                long devX = 0;
                long devY = 0;
                long wantX = 0;
                long wantY = 0;
                long gotX = 0;
                long gotY = 0;
                const bool have = ReadPair(out, "偏差=(", &devX, &devY) &&
                                  ReadPair(note, "想要 (", &wantX, &wantY) &&
                                  ReadPair(note, "实际停在 (", &gotX, &gotY);
                ++pairsChecked;
                // 偏差的定义是 **实际 − 期望**（窗口位移减光标位移），钳制量是 期望 − 实际，
                // 两者差一个符号 —— 探针第一版把符号写反了，于是 0/4（量错不是产品错）。
                if (have && gotX - wantX == devX && gotY - wantY == devY) ++pairsOk;
                break;
            }
        }
        h->Req("live-release-deviation",
               "没被钳制的拖动偏差逐像素为 0；被钳制的那些偏差必须等于钳制量",
               Num(releases) + " 次松手 = " + Num(zeroDev) + " 次 偏差=(0,0) + " + Num(clamped) +
                   " 次 钳制=是；钳制生效行 " + Num(clampNotes) + " 条，其中偏差与钳制量对得上的 " +
                   Num(pairsOk) + "/" + Num(pairsChecked),
               releases > 0 && zeroDev == releases - clamped && clampNotes == clamped &&
                   pairsChecked == clamped && pairsOk == pairsChecked);

        // ② 帧：拖动期间**不能出现停顿**。这里刻意不拿"最短那几段拖动的平均帧间隔"去跟整段
        //    平均比：recenter 那种拖动整个动作只有 2~3 帧，平均值是噪声（探针第一版就是这么
        //    比的，36.03 vs 30.14 直接判失败，而那 3 帧里最长的一帧也只有 36.56ms）。
        //    判据要对着**故障的样子**写：系统拖动接管渲染循环时，帧间隔会掉到几百毫秒
        //    （设计 §10.1.1 的"顿住/停跳"）。所以查两件事 ——
        //      ① 绝对量：拖动期间最长的一帧间隔 < 100ms（人眼能看见的顿）；
        //      ② 相对量：最长那一帧 <= 整段平均帧间隔的 2.5 倍（这台机器上整段平均 30ms
        //         由渲染耗时 22.9ms/帧决定，是**拖动之前就有的**开销）。
        //    两条都用"最长的那一帧"，因为它才是"顿"的最小可见单位。
        double frames = 0.0;
        double slowestSegmentMs = 0.0;
        double fastestSegmentMs = 0.0;
        double worstInterval = 0.0;
        double worstRender = 0.0;
        int frameLines = 0;
        for (const std::wstring& line : lines) {
            const std::string text = OneLine(Narrow(line));
            if (text.find("拖动期间") == std::string::npos) continue;
            double f = 0.0;
            double a = 0.0;
            double w = 0.0;
            double ar = 0.0;
            double wr = 0.0;
            if (!NumberAfter(text, "frames=", &f) || !NumberAfter(text, "平均帧间隔=", &a) ||
                !NumberAfter(text, "最长帧间隔=", &w) || !NumberAfter(text, "平均单帧=", &ar) ||
                !NumberAfter(text, "最长单帧=", &wr)) {
                continue;
            }
            ++frameLines;
            frames += f;
            if (a > slowestSegmentMs) slowestSegmentMs = a;
            if (fastestSegmentMs == 0.0 || a < fastestSegmentMs) fastestSegmentMs = a;
            if (w > worstInterval) worstInterval = w;
            if (wr > worstRender) worstRender = wr;
        }
        const double runInterval = (runFrames > 0.0) ? (runElapsed * 1000.0 / runFrames) : 0.0;
        char evidence[640];
        std::snprintf(evidence, sizeof(evidence),
                      "%d 段拖动合计 %d 帧：最慢一段平均 %.2fms/帧，最快一段平均 %.2fms/帧，"
                      "最长的一帧间隔 %.2fms(=整段平均的 %.2f 倍)，最长的一次渲染 %.2fms；"
                      "基准（整段，含空闲）frames=%.0f elapsed=%.2fs -> 平均 %.2fms/帧(%.1fHz) "
                      "平均单帧=%.2fms",
                      frameLines, static_cast<int>(frames), slowestSegmentMs, fastestSegmentMs,
                      worstInterval, runInterval > 0.0 ? worstInterval / runInterval : 0.0,
                      worstRender, runFrames, runElapsed, runInterval,
                      runInterval > 0.0 ? 1000.0 / runInterval : 0.0, runAvgRender);
        h->Req("live-drag-frames",
               "拖动期间没有停顿：最长一帧 < 100ms，且 <= 整段平均的 2.5 倍（系统拖动接管时会掉到"
               "几百毫秒）",
               evidence,
               frameLines > 0 && frames > 0.0 && runInterval > 0.0 && worstInterval < 100.0 &&
                   worstInterval <= runInterval * 2.5 && worstRender <= runAvgRender * 2.0);
        std::printf("[live] %s\n", evidence);

        // ③ 真拖动不能进"这次提起算拖动还是算点击"那条判定。判据是**一条都不到**：那个函数
        //    只在 WM_LBUTTONUP 里被调用，一旦被调用就必定写一行 [click]（三种结果各一行，
        //    见 main.cpp 的 FinishLeftGesture）。所以"这个进程写了 0 行 [click]"就等于
        //    "这 11 次拖动一次都没被交给那条判定"—— 真拖动不会在松手时被当成点击，这是可量
        //    的，不是靠读代码相信。（本用例没有 --click-test，所以 [click] 只可能来自真手势。）
        int clickLines = 0;
        {
            const std::string wholeTail = ReadAppended(LogPath(), child.logFrom);
            std::size_t at = wholeTail.find("[click]");
            while (at != std::string::npos) {
                ++clickLines;
                at = wholeTail.find("[click]", at + 7);
            }
        }
        h->Req("live-drag-not-a-click",
               "真拖动不进单击判定（入口一次都没被调用）—— 真拖动因此不可能被当成一次点击",
               "子进程这 25 秒里写了 " + Num(clickLines) + " 行 [click]（本用例共 " +
                   Num(releases) + " 次拖动松手）；那条判定另行用 --click-test 逐一验证",
               releases > 0 && clickLines == 0);
    }
    StopChild(&child);

    // ---- 6) 松手即存：文件里就是刚才那个坐标 ----
    {
        std::string text;
        ReadFileText(configPath, &text);
        POINT fromFile{};
        std::wstring why;
        const bool read = dshb::LoadWindowPos(configPath, size, &fromFile, &why);
        h->Req("live-saved-position",
               "松手时写了一次，文件里的坐标 == 松手那一刻的窗口位置（这是**吸附过**的那一次）",
               "文件内容：" + OneLine(text) + "；解析 -> (" + Num(fromFile.x) + "," +
                   Num(fromFile.y) + ")，松手时实测 (" + Num(result.park.x) + "," +
                   Num(result.park.y) + ")",
               read && fromFile.x == result.park.x && fromFile.y == result.park.y);
    }

    // ---- 7) 重开原位 ----
    {
        Child again{};
        if (!LaunchChild(&again, configPath, curvePath, 4)) {
            h->Req("live-reopen", "重开位置 == config.json 里的坐标", "第二个子进程起不来", false);
        } else {
            Sleep(300);
            const RECT r = RectOf(again.hwnd);
            const std::wstring line = WaitForLine(again, L"起点取自", 2000);
            h->Req("live-reopen", "重开位置 == config.json 里的坐标（吸附后的坐标重开后仍在原位）",
                   "第二个进程的窗口矩形 " + RectStr(r) + "；它自己的日志：" +
                       (line.empty() ? std::string("(没找到)") : "\"" + Narrow(line) + "\"") +
                       "；config 里是 (" + Num(result.park.x) + "," + Num(result.park.y) + ")",
                   r.left == result.park.x && r.top == result.park.y);
            StopChild(&again);
        }
    }

    SetCursorPos(savedCursor.x, savedCursor.y);   // 光标还回去（这台机器上有人正用着它）
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    POINT park{1600, 240};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-live") {
            g_live = false;
        } else if (arg == "--real-config") {
            g_realConfig = true;
        } else if (arg == "--keep-temp") {
            g_keepTemp = true;
        } else if (arg == "--verbose") {
            g_verbose = true;
        } else if (arg.rfind("--exe=", 0) == 0) {
            g_exePath = Wide(arg.substr(6));
        } else if (arg.rfind("--park=", 0) == 0) {
            // 手写解析而不是 sscanf：sscanf 在 MSVC 上是弃用函数（C4996），
            // 而这个探针要保持"零警告"，否则真警告会被淹掉。
            const char* p = arg.c_str() + 7;
            char* end = nullptr;
            const long x = std::strtol(p, &end, 10);
            if (end && *end == ',') {
                const long y = std::strtol(end + 1, &end, 10);
                if (end && *end == '\0') park = POINT{static_cast<long>(x), static_cast<long>(y)};
            }
        } else {
            std::printf("dragprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: dragprobe [--no-live] [--real-config] [--keep-temp] [--verbose] "
                        "[--park=X,Y] [--exe=PATH]\n");
            return 64;
        }
    }
    SetConsoleOutputCP(CP_UTF8);

    // ★ 第一件事就声明 DPI 感知：这台机器缩放 200%，一个非感知进程读到的窗口矩形与光标
    //   坐标都被系统虚拟化过（正好一半），拿它做拖动算术会得到 2 倍的偏差 —— 量错了还会
    //   以为代码错。必须在建任何窗口之前调用。
    const BOOL dpiOk = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (g_exePath.empty()) g_exePath = ExeDir() + L"dshb.exe";

    Harness h;
    std::printf("dragprobe: 面板自控拖动（跟手 / 钳制 / 松手即存 / 拖动不顿）\n");
    std::printf("几何/钳制/持久化调的是 src/panel_drag.cpp（生产代码同一份）；DPI 感知=%s\n",
                dpiOk ? "已声明 Per-Monitor V2" : "声明失败（坐标被虚拟化，下面的数不可信）");

    dshb::WorkArea area = dshb::WorkAreaForPoint(POINT{0, 0});
    const RECT work = area.valid ? area.rect : RECT{0, 0, 1920, 1080};
    const std::wstring tempDir = TempPath(L"");

    CaseFollowMath(&h, work);
    CasePanelGeometry(&h);
    CaseClamp(&h, work);
    CaseSnap(&h, work);
    CaseConfig(&h, tempDir, work);

    LiveResult live{};
    if (g_live) {
        live = RunLive(&h, park);
        if (!live.ran) std::printf("SKIP: live | 真机注入那一段没跑成：%s\n", live.why.c_str());
    } else {
        std::printf("SKIP: live | --no-live：不做真机注入\n");
    }

    std::printf("\nchecks: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& failure : h.failures) std::printf("failed line: %s\n", failure.c_str());
    const bool ok = h.failed == 0 && h.passed > 0;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
