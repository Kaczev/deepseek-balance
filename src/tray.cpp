// 托盘图标与右键菜单的实现（设计 §10.3、F4）。头部注释里写了为什么单独一份模块。
//
// 这一份里三件事都只做一遍：
//   1. icon.png（RCDATA 内嵌）-> 多尺寸 HICON。**保留 alpha**、用 WIC 的高质量重采样器
//      （不是最近邻）—— 浅色任务栏下的"黑方块"正是这两件事没做对。
//   2. NIM_ADD / NIM_DELETE 与图标矩形（Shell_NotifyIconGetRect + NOTIFYICONIDENTIFIER）。
//   3. 「关闭」那一项菜单，以及"点的是不是我们自己的图标/菜单"这个判断。

#include "tray.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <shellapi.h>    // Shell_NotifyIconW / Shell_NotifyIconGetRect / NOTIFYICONIDENTIFIER
#include <wincodec.h>    // IWICImagingFactory / 缩放器（高质量重采样）

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace dshb {
namespace {

// .rc 里的资源名："DSHB_ICON_PNG"（ASCII）。FindResourceW 找字符串名，两处必须逐字一致 ——
// 写成整数 101 就谁也找不到谁（.rc 那行不加引号才是整数），而症状是"图标资源读不到"。
constexpr wchar_t kIconResourceName[] = L"DSHB_ICON_PNG";

// 我们自己那个菜单面板的窗口类名。
//
// ★★ 为什么**不**用系统的 "#32768"：那是所有弹出菜单共用的名字，而这一份面板是自建窗口，
//   用真名反而让自己没法被认出来（见 RefreshMenuRect 里那一步）。
//   旧实现靠 "#32768" 认"我们的菜单正弹着"，那个类名同时属于本进程**所有**的弹出菜单；
//   换成这个名字之后，"是不是我们"变成类名就够，不必再猜。
constexpr wchar_t kMenuPanelClass[] = L"DshbTrayMenu";

// 菜单面板的尺寸。这里的数字是**逻辑像素**（dip），落成窗口时按窗口 DPI 换算 ——
// 硬编码像素会让 200% 缩放下那个「关闭」小得点不中。
constexpr int kMenuItemHeightDip = 26;   // 「关闭」这一条的高度
constexpr int kMenuPaddingDip = 2;       // 上下内边距（四周的边框也在这一层里）

// 菜单面板自己收回的时限（毫秒）。
// ★★ 这是"生产不会再挂死"的最后一道兼底，不是装饰：面板是**非模态**的，主循环照常跑，
//   所以即使一次输入都没到（例如面板没拿到激活、或注入的事件丢了），它到点也会自己消失。
//   旧实现没有这道门 —— TrackPopupMenu 是模态等待，一旦收不到输入就永远不返回。
//   2 秒是给 --tray-menu-test 的注入留的余量（脚本在弹出后 0.4 s 才点）。
constexpr UINT_PTR kMenuPanelTimeoutMs = 2000;

// 日志（定义在本文件下面：托盘模块必须自己能说话，见那一段的注释）。
// ★ 为什么这里先声明一次：这个模块的两处日志（菜单面板、图标安装）在文件里前后分开，
//   而日志函数只有一份定义 —— 声明放在最上面，谁用到谁就不用管定义在哪。
void TrayLog(const wchar_t* fmt, ...);

// 面板窗口过程（定义在下面；PopupMenu 要拿它的地址去注册窗口类）。
LRESULT CALLBACK MenuPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ---- 菜单面板：屏幕上的那一份状态 ----
// ★ 为什么是**文件级静态**而不是 TrayIcon 的成员：窗口过程是 C 函数，拿不到对象；
//   而托盘图标整个进程只有一个（见 main.cpp 的 g_tray），所以这份状态天然是单例。
HWND g_menuPanel = nullptr;      // 面板正开着时的窗口句柄，否则 nullptr（MenuPanelBuildingSentinel() 除外）
HWND g_menuOwner = nullptr;      // 面板要通知的那个窗口（= 我们的主窗口）

// ★★ 重入闸用的"正在建"哨兵值：PopupMenu 一进来就先把 g_menuPanel 置成它，等面板真的
//   建好再换成真句柄。
//
//   WHY（2026-09-19 实测，所有者日志里的铁证）：所有者右击一次托盘图标，日志里出现
//   **两条** "菜单面板已弹出"，两条的光标位置逐像素相同、面板句柄却不同，第二条还写着
//   "弹出时前台=别人"。也就是说 PopupMenu 在一次右键里跑了**两趟**，而第二趟进来时
//   第一趟的面板已经建好却没被看见 —— 唯一的解释是第一趟正卡在
//   AttachThreadInput / SetForegroundWindow(hwnd) / CreateWindowExW 这几步里，
//   这几步都会**同步投递窗口消息**（激活、输入队列合并），于是排在队列里的下一条托盘
//   回调被就地处理，PopupMenu 就这样在第一趟的肚子里又跑了一趟。
//   后果正是所有者看到的三个症状：第二个面板把 g_menuPanel 覆盖掉，第一个变成**没人管的
//   孤儿窗口**（永远不消失）；而那个孤儿还活着、于是它还能收到点击，可 g_menuOwner 早已
//   被另一趟清成 NULL —— "点了没用"（PostMessage(NULL) 什么也不做）与"每次点击又多一个"
//   （孤儿面板 + 新面板同时在屏幕上）都从这里来。
//
//   所以闸门必须**在任何可能重入的调用之前**关上。哨兵而不是 bool：读写同一个变量，
//   守卫处只有一个条件，不存在"两个标志不同步"这种第二套状态。
//   （写成函数而不是 constexpr：reinterpret_cast 不是常量表达式，MSVC 的 C2131 就是这个。）
HWND MenuPanelBuildingSentinel() {
    return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(1));
}

// 面板句柄是不是"活的"。
// ★ 为什么必须有这一步：只有非空判断时，一个已经销毁（或被别的路径覆盖）的句柄会被
//   当成"面板还开着"，于是再也不建新的、也不建在正确位置。IsWindow 问的是系统。
bool MenuPanelAlive() {
    return g_menuPanel != nullptr && g_menuPanel != MenuPanelBuildingSentinel() && IsWindow(g_menuPanel);
}

// 屏幕上**现在**有几个我们的面板窗口（本进程、类名 kMenuPanelClass），顺便把它们的并集
// 矩形算出来（out 可以是 nullptr = 只要个数）。
// ★ 用 EnumWindows（全部顶层窗口）再按 进程号 + 类名 过滤，因为要回答的就是"屏幕上真的
//   有几个"；这只在夹具与探针里被调用（每帧调它会把窗口管理器问一遍，没必要）。
int CountMenuPanelsImpl(RECT* out = nullptr) {
    struct Ctx {
        DWORD pid;
        int count;
        RECT uni;
        bool have;
    } ctx{GetCurrentProcessId(), 0, RECT{}, false};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid) return TRUE;   // 别的进程的窗口不看
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, kMenuPanelClass) != 0) return TRUE;
            RECT r{};
            if (!GetWindowRect(h, &r)) return TRUE;
            ++c->count;
            if (!c->have) {
                c->uni = r;
                c->have = true;
            } else {
                if (r.left < c->uni.left) c->uni.left = r.left;
                if (r.top < c->uni.top) c->uni.top = r.top;
                if (r.right > c->uni.right) c->uni.right = r.right;
                if (r.bottom > c->uni.bottom) c->uni.bottom = r.bottom;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (out && ctx.have) *out = ctx.uni;
    return ctx.count;
}

// 面板里属于「关闭」那一条的矩形。**只有这一处定义** —— 绘制、命中测试、日志、探针坐标
// 都从这里问，所以"画在哪"与"点在哪"不可能分叉（这个坑本项目踩过不止一次：两处各算一遍
// 判据，迟早会不一致）。
// rectSpace：Client = 客户区坐标（WM_PAINT / WM_LBUTTONDOWN 用的），Screen = 屏幕坐标
// （日志与注入点击用的）。
// ★ 面板里**只有这一项**：上边距到（下边距-上边距），横向是除去边框之外的整个宽度。
// ★★ 这个函数**不做 DIP 换算**：内边距本身就是按窗口 DPI 算出来的物理像素，再乘一次
//   就会把"这一条"算到窗口外面去（实测：命中测试判成"点空白"，行为是取消）。
enum class RectSpace { Client, Screen };

RECT MenuItemRect(HWND panel, RectSpace space) {
    RECT rc{};
    GetClientRect(panel, &rc);
    const UINT dpi = GetDpiForWindow(panel);
    const int pad = MulDiv(kMenuPaddingDip, static_cast<int>(dpi ? dpi : 96u), 96);
    RECT r{rc.left, rc.top + pad, rc.right, rc.bottom - pad};
    if (space == RectSpace::Client) return r;
    POINT tl{r.left, r.top};
    POINT br{r.right, r.bottom};
    ClientToScreen(panel, &tl);
    ClientToScreen(panel, &br);
    return RECT{tl.x, tl.y, br.x, br.y};
}

void DrainMenuPanel() {
    if (!g_menuPanel) return;
    HWND panel = g_menuPanel;
    // 先清状态再 DestroyWindow：DestroyWindow 会同步送 WM_DESTROY/WM_NCDESTROY，
    // 那里面再碰 g_menuPanel 就会踩到正在销毁的句柄。
    g_menuPanel = nullptr;
    KillTimer(panel, 1);
    DestroyWindow(panel);
}

// 那个时限到了。把它记成一条**独立**的日志（"被时限收回"与"用户点了一下"是两件事，
// 混成一句会让证据读不出来）。
void CloseMenuPanelOnTimeout() {
    if (!MenuPanelAlive()) {
        // 句柄不是活的（已经销毁 / 正在建）：什么都不做，只把状态清干净。
        // 不清的话下一次 PopupMenu 会以为"面板还开着"而拒绝建新的 —— 那正是"右击没反应"。
        g_menuPanel = nullptr;
        g_menuOwner = nullptr;
        return;
    }
    TrayLog(L"[tray] 菜单面板弹了 %u ms 没收到任何输入：按时限自己收回（非模态，进程一直活着）",
            static_cast<unsigned>(kMenuPanelTimeoutMs));
    DrainMenuPanel();
    g_menuOwner = nullptr;
}

// 面板上真正被点中了：把命令**投给属主窗口**，在那一轮的 WndProc 里处理。
// ★ 为什么用 PostMessage 而不是在这里直接调 HandleTrayMenuCommand：托盘模块不认识
//   关闭态（那是 main.cpp 的事），而"菜单命令只有一条真实路径"这条设计要保持 ——
//   面板只产生一条 kMsgTrayMenu 消息，处理它的地方只有 WndProc 一处。
//
// ★★ px/py 是**消息里那两个坐标**（客户区），由面板的窗口过程原样传进来。
//   "点在里面吗"必须**真的拿它跟 MenuItemRect 比**：第一版那句话是按 which 写死的
//   （which==0 就打印"是"），于是"矩形是 0x0、什么都没点到"也会被打成"是，就是这一条" ——
//   日志由此骗了下一个人一整轮（所有者日志里那一串 0x0 的"是"就是这么来的）。
void ActivateMenuPanel(int which, int px, int py) {
    HWND panel = MenuPanelAlive() ? g_menuPanel : nullptr;
    const RECT itemClient = panel ? MenuItemRect(panel, RectSpace::Client) : RECT{};
    const RECT item = panel ? MenuItemRect(panel, RectSpace::Screen) : RECT{};
    const bool inside = panel && px >= itemClient.left && px < itemClient.right &&
                        py >= itemClient.top && py < itemClient.bottom;
    const wchar_t* what = L"";
    UINT cmd = 0;
    if (which == 0 && inside) {
        what = L"左键点「关闭」这一条";
        cmd = kTrayMenuClose;
    } else if (which == 0) {
        // 左键落在面板里、但**不在**那一条上（内边距那一圈）：算取消，并且如实这么写。
        what = L"左键点面板的空白处（取消）";
    } else if (which == 1) {
        // 右键在面板上：菜单约定里右键是取消（系统菜单也是这个口径）。
        what = L"右键点面板（取消）";
    } else {
        what = L"面板以外的按下（取消）";
    }
    // 打开时量到的"那一条矩形"与**按下那一刻**再量一次：面板没动过它们就该相等。
    // 不相等说明面板在弹出之后被系统挪过（DPI 变化/多屏切换），那一下的落点判断就不可信 ——
    // 所以两个数都写进日志，而不是只写一个。
    // ★ "点在里面吗"报的是**真的比出来的那个结果**（inside），不是按 which 猜的。
    TrayLog(L"[tray] 菜单面板收到输入：%ls（消息坐标=(%d,%d)；那一条矩形 (客户区) "
            L"(%ld,%ld,%ld,%ld) = %ldx%ld，(屏幕) (%ld,%ld,%ld,%ld)；点在里面吗？%ls）"
            L" -> 投递 kMsgTrayMenu=%u 给属主 hwnd=%p",
            what, px, py, itemClient.left, itemClient.top, itemClient.right, itemClient.bottom,
            itemClient.right - itemClient.left, itemClient.bottom - itemClient.top, item.left,
            item.top, item.right, item.bottom, inside ? L"是，真的落在这一条上" : L"否",
            cmd, g_menuOwner);
    HWND owner = g_menuOwner;
    DrainMenuPanel();
    // ★ 属主必须是**活窗口**才投：面板活着而属主已经没了（主窗口正在销毁）时，
    //   PostMessage 到 NULL 是静默失败 —— 用户看到的就是"点了没反应"。
    if (cmd != 0 && owner && IsWindow(owner)) {
        PostMessageW(owner, kMsgTrayMenu, cmd, 0);
    } else if (cmd != 0) {
        TrayLog(L"[tray] ★ 菜单命令没能投出去：属主 hwnd=%p %ls（这一下不会有任何效果）", owner,
                (owner ? L"已经不是窗口了" : L"是 NULL"));
    }
}

// 面板的窗口过程。只做三件事：画那一项、把输入翻译成命令或取消、到点自己收回。
LRESULT CALLBACK MenuPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        // ★ 时限在**创建时**就挂上，而不是等第一次 WM_ACTIVATE：面板若根本没拿到激活
        //   （那正是旧实现收不到输入的形态），等激活的定时器就永远不会启动。
        SetTimer(hwnd, 1, kMenuPanelTimeoutMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == 1) CloseMenuPanelOnTimeout();
        return 0;
    case WM_ERASEBKGND:
        return 1;   // 背景在 WM_PAINT 里整块刷，先擦一遍是白闪
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        // 配色照系统菜单的口径（浅色主题）：底白、边框灰、悬停那条用浅灰。
        // 不追系统深色主题是**有意的取舍**：这一版要解决的是"点不动"，
        // 配色跟着主题走是另一件事，混进来会让这一轮的改动没法判断。
        HBRUSH bg = CreateSolidBrush(RGB(0xF7, 0xF7, 0xF7));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        POINT cur{};
        GetCursorPos(&cur);
        const RECT itemScreen = MenuItemRect(hwnd, RectSpace::Screen);
        const RECT itemClient = MenuItemRect(hwnd, RectSpace::Client);
        const bool hot = cur.x >= itemScreen.left && cur.x < itemScreen.right &&
                         cur.y >= itemScreen.top && cur.y < itemScreen.bottom;
        if (hot) {
            HBRUSH hover = CreateSolidBrush(RGB(0xE5, 0xE5, 0xE5));
            FillRect(dc, &itemClient, hover);
            DeleteObject(hover);
        }
        // 边框：贴着客户区最外一圈（面板没有非客户区，尺寸就是内容）。
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC0, 0xC0, 0xC0));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        // Rectangle 的右边/下边是开区间：往里收一像素，边框才在客户区之内。
        Rectangle(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x1A, 0x1A, 0x1A));
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT text = itemClient;
        // ★ DT_CENTER|DT_VCENTER 而不是手算坐标：文字尺寸随 DPI 与字体变，
        //   算出来的居中在别的 DPI 上就是歪的（本项目为"居中错位"踩过坑）。
        DrawTextW(dc, kTrayCloseText, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        // 悬停高亮要跟着光标走：不重画的话鼠标扫过面板时那一行永远不亮。
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        // 按下即生效（与系统菜单一致）。★ 用**消息里的坐标**而不是 GetCursorPos：
        //   注入的点击与真实点击在这一处的语义必须完全相同（探针就是靠这一条）；
        //   取当前光标位置会把"注入的坐标"和"光标实际在哪"混成一个数。
        {
            // ★ 用 LOWORD/HIWORD 拆坐标，与 main.cpp 里所有鼠标消息同一个写法
            //   （GET_X_LPARAM 要 windowsx.h，本文件不引它）。
            const int px = static_cast<int>(static_cast<short>(LOWORD(lp)));
            const int py = static_cast<int>(static_cast<short>(HIWORD(lp)));
            // which=0 表示"按的是左键"；**是否真的落在「关闭」那一条上**由
            // ActivateMenuPanel 拿同一个矩形（MenuItemRect(Client)）自己判 ——
            // 判定与日志都只有那一处，这里不替它下结论（第一版我在这里另判一次并且
            // 日志按 which 写死"是"，结果"什么都没点到"也被打成"是"，骗了下一个人一整轮）。
            ActivateMenuPanel(0, px, py);
        }
        return 0;
    case WM_RBUTTONDOWN:
        ActivateMenuPanel(1, static_cast<int>(static_cast<short>(LOWORD(lp))),
                          static_cast<int>(static_cast<short>(HIWORD(lp))));
        return 0;
    case WM_KEYDOWN:
        // Esc 关掉（不选任何东西）。★ 这一支**只关系到这个托盘面板自己**：弹出菜单按 Esc
        //   收起是 Windows 惯例，与挂件的生死无关。
        //   （挂件那边的全局 Esc 钩子已在 0.2 删除 —— 它会在**任何程序**里按 Esc 就关掉挂件。）
        if (wp == VK_ESCAPE) ActivateMenuPanel(2, -1, -1);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 日志与 main.cpp 的 SelfTestLog 同格式、同落点（exe 同目录的 selftest.log）。
// ★ 为什么这里自己写一份而不是共享 main.cpp 那个：main.cpp 的 SelfTestLog 在匿名
//   namespace 里，托盘模块（以及 tools/trayprobe.cpp）够不着它；而这个模块**必须**
//   自己能说话 —— "图标加上去了/摘掉了"这两行就是设计 §10.3 的验收依据。
//   两份的格式只有一处定义在这里（'[' 开头、UTF-8、一行一条），没有第二套约定。
void TrayLog(const wchar_t* fmt, ...) {
    static wchar_t path[MAX_PATH] = L"";
    if (path[0] == L'\0') {
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
        if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
        wcscat_s(path, L"selftest.log");
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        // ★ 毫秒时间戳：这次排查里最要紧的问题是"两条面板日志之间隔了多久" ——
        //   同一次右键里的重入是**毫秒级**的，而"用户点了两次"是秒级的。
        //   没有时间戳时这两种情形在日志里长得一模一样（所有者日志里成对的那几条就是），
        //   于是无法区分"重入"和"点两次"。自己取一次时钟，不依赖调用方传时间。
        SYSTEMTIME st{};
        GetLocalTime(&st);
        fwprintf(f, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfwprintf(f, fmt, args);
        va_end(args);
        fwprintf(f, L"\n");
        fclose(f);
    }
}

// 面板的窗口类只注册一次（进程级）。
void EnsureMenuPanelClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MenuPanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // ★ 光标：必须给，否则鼠标扫过面板时系统保留上一个窗口的光标（看起来像"没反应"）。
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMenuPanelClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        // 重复注册不是失败（类已经在了，能用）。其余失败是**真失败**：
        // 它的后果是"右键点托盘图标弹不出菜单"，所以要留痕。
        TrayLog(L"[tray] 菜单面板窗口类注册失败（err=%lu）：右键将弹不出菜单", GetLastError());
        return;
    }
    registered = true;
}

// WIC 工厂按需创建：它只在这三个入口上被用到（安装 / 图标矩形 / 弹菜单），而这三个
// 入口全部在主线程上（wWinMain、WndProc、以及和 WndProc 同线程的低级鼠标钩子），
// 所以不需要为它操心线程模型。谁创建谁释放。
IWICImagingFactory* CreateWic() {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return nullptr;
    }
    return wic;
}

bool IsPremultipliedFormat(const WICPixelFormatGUID& f) {
    return IsEqualGUID(f, GUID_WICPixelFormat32bppPBGRA) != FALSE;
}

// 把源位图缩到 (targetW, targetH)（按最长边取 targetPx，保持纵横比），
// 32bppBGRA（直通 alpha，未预乘）输出到 out，实际尺寸回填。
//
// ★ 为什么用 WICBitmapScaler 而不是 StretchBlt：StretchBlt 的颜色键/光栅操作会碰 alpha，
//   而这里的成败判据恰恰是 alpha（浅色底上有没有黑方块）。缩放器还给了明确的插值模式：
//   用 Fant（高保真重采样）而不是最近邻 —— 1141 -> 16 这种比例下最近邻会直接把边缘
//   采成锯齿，那就是验收里"边缘有硬锈"的样子。
// ★ 尺寸：icon.png 是 1141×1049（近方形，差 8%），所以按"最长边 = 目标边长"等比缩，
//   并把结果**居中**放进正方形画布，空出来的边用透明补齐。直接强行拉成正方形会把
//   圆角的曲率拉歪，而"正方形画布"是 HICON 的硬要求。
bool ScaleFrame(IWICImagingFactory* wic, IWICBitmapSource* src, UINT targetPx,
                std::vector<unsigned char>* out, unsigned int* outW, unsigned int* outH) {
    // ★ 源是**预乘**时先转成直通再缩放：把预乘数据当直通缩放，边缘会发黑/发白
    //   （renderer.cpp 导出 PNG 时踩过同一个坑，那里也有一行同样的声明）。
    //   转换器与缩放器都是 COM 对象，谁创建谁释放，两条路径上都不留泄漏。
    IWICBitmapSource* source = src;
    IWICFormatConverter* converter = nullptr;
    WICPixelFormatGUID srcFmt{};
    if (SUCCEEDED(src->GetPixelFormat(&srcFmt)) && IsPremultipliedFormat(srcFmt)) {
        if (SUCCEEDED(wic->CreateFormatConverter(&converter)) && converter) {
            if (FAILED(converter->Initialize(src, GUID_WICPixelFormat32bppBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom))) {
                converter->Release();
                converter = nullptr;
            } else {
                source = converter;
            }
        }
    }

    UINT srcW = 0, srcH = 0;
    source->GetSize(&srcW, &srcH);
    if (srcW == 0 || srcH == 0) {
        if (converter) converter->Release();
        return false;
    }
    // 最长边缩到 targetPx，另一边按比例（至少 1 像素）。
    UINT dstW = 0, dstH = 0;
    if (srcW >= srcH) {
        dstW = targetPx;
        dstH = static_cast<UINT>(static_cast<unsigned long long>(srcH) * targetPx / srcW);
    } else {
        dstH = targetPx;
        dstW = static_cast<UINT>(static_cast<unsigned long long>(srcW) * targetPx / srcH);
    }
    if (dstW == 0) dstW = 1;
    if (dstH == 0) dstH = 1;

    std::vector<unsigned char> scaled;
    IWICBitmapScaler* scaler = nullptr;
    bool ok = false;
    do {
        if (FAILED(wic->CreateBitmapScaler(&scaler)) || !scaler) break;
        // Fant = 高保真重采样。★ 不用最近邻的理由就是验收那条：1141 -> 16 时最近邻会把
        // 边缘直接采成锯齿（"边缘有硬锈"），而图标品质的判据只有像素。
        if (FAILED(scaler->Initialize(source, dstW, dstH, WICBitmapInterpolationModeFant))) break;
        scaled.assign(static_cast<std::size_t>(dstW) * dstH * 4, 0);
        if (FAILED(scaler->CopyPixels(nullptr, dstW * 4, static_cast<UINT>(scaled.size()),
                                      scaled.data()))) {
            break;
        }
        ok = true;
    } while (false);
    if (scaler) scaler->Release();
    if (converter) converter->Release();
    if (!ok) return false;

    // 居中放进正方形画布：空出来的边保持全 0（alpha=0），也就是透明。
    out->assign(static_cast<std::size_t>(targetPx) * targetPx * 4, 0);
    const UINT offX = (targetPx - dstW) / 2;
    const UINT offY = (targetPx - dstH) / 2;
    for (UINT y = 0; y < dstH; ++y) {
        std::memcpy(out->data() + (static_cast<std::size_t>(y + offY) * targetPx + offX) * 4,
                    scaled.data() + static_cast<std::size_t>(y) * dstW * 4,
                    static_cast<std::size_t>(dstW) * 4);
    }
    if (outW) *outW = dstW;
    if (outH) *outH = dstH;
    return true;
}

// 32bppBGRA 的像素 -> HICON。
//
// ★ 为什么要自己建 DIB 而不直接 CreateIconFromResourceEx：资源里那 1 MB 的 PNG 必须
//   先缩到 16/20/24/32 像素，而 CreateIconFromResourceEx 只认"它自己认得的那几种
//   原始格式"，喂给它非标准尺寸等于让系统自己拉伸 —— 那正是"大缩放下图标虚"的成因。
//   这里每一步都是我们算的：缩放归 WIC，掩码归 alpha。
//
// ★ AND 掩码那一位图必须全 0：32 位图标靠 alpha 通道做透明，掩码只是为了兼容旧路径。
//   掩码若全 1（"全透明"），在少数老外壳路径上整个图标会被掩掉。
HICON HiconFromBgra(UINT w, UINT h, const std::vector<unsigned char>& bgra) {
    if (w == 0 || h == 0 || bgra.size() < static_cast<std::size_t>(w) * h * 4) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = static_cast<LONG>(w);
    bi.bV5Height = -static_cast<LONG>(h);   // 负数 = 自上而下，与 WIC 的行序一致
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, bgra.data(), static_cast<std::size_t>(w) * h * 4);

    HBITMAP mask = CreateBitmap(static_cast<int>(w), static_cast<int>(h), 1, 1, nullptr);
    if (!mask) {
        DeleteObject(color);
        return nullptr;
    }
    // 全 0 掩码：CreateBitmap 刚创建时是未初始化的，必须显式清零（否则是随机透明）。
    {
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc) {
            HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, mask));
            RECT all{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SelectObject(dc, old);
            DeleteDC(dc);
        }
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// 当前系统在通知区域用的图标边长（像素）。
// ★ 双保险的理由：[tray] 日志要能看出"系统在这次 DPI 下要的是几像素"，而
//   GetSystemMetricsForDpi 在极老系统上不存在 —— 直接取 SM_CXSMICON 的分辨率版本。
int TrayIconSizePx(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    int px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (px <= 0) px = GetSystemMetrics(SM_CXSMICON);
    if (px <= 0) px = 16;
    return px;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

// ---------------------------------------------------------------------------
// 图标那一半的裸接口（tray.h 里写了为什么公开：探针要量的是**这一份**像素）
// ---------------------------------------------------------------------------
namespace detail {

bool LoadEmbeddedIconPng(const unsigned char** data, unsigned int* size) {
    *data = nullptr;
    *size = 0;
    HRSRC res = FindResourceW(nullptr, kIconResourceName, RT_RCDATA);
    if (!res) return false;
    const DWORD bytes = SizeofResource(nullptr, res);
    if (bytes == 0) return false;
    HGLOBAL loaded = LoadResource(nullptr, res);
    if (!loaded) return false;
    const void* locked = LockResource(loaded);
    if (!locked) return false;
    // 指进资源本身，不拷贝：资源映射在模块映像里，这个 exe 活着就一直有效。
    *data = static_cast<const unsigned char*>(locked);
    *size = bytes;
    return true;
}

void* OpenEmbeddedIconSource(void** outStream, void** outDecoder, void** outFactory,
                             unsigned int* outSrcW, unsigned int* outSrcH, GUID* outFormat) {
    if (outStream) *outStream = nullptr;
    if (outDecoder) *outDecoder = nullptr;
    if (outFactory) *outFactory = nullptr;
    if (outSrcW) *outSrcW = 0;
    if (outSrcH) *outSrcH = 0;
    if (outFormat) *outFormat = GUID{};

    const unsigned char* png = nullptr;
    unsigned int pngSize = 0;
    if (!LoadEmbeddedIconPng(&png, &pngSize)) return nullptr;

    IWICImagingFactory* wic = CreateWic();
    if (!wic) return nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    do {
        if (FAILED(wic->CreateStream(&stream))) break;
        // ★ 内存流是**独占**的：别的解码器打不开它 —— 我们的资源正好只此一家。
        if (FAILED(stream->InitializeFromMemory(const_cast<unsigned char*>(png),
                                                static_cast<DWORD>(pngSize)))) {
            break;
        }
        if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder))) {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) break;
    } while (false);

    if (!frame) {
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        wic->Release();   // 失败路径上工厂由这里收（成功路径上它跟着转换器一起被收）
        return nullptr;
    }

    // ★★ 必须**强制**转成直通 32bppBGRA，不能信解码器默认吐出来的那个格式。
    //   实测（2026-09-19，trayprobe 抓到）：这张 icon.png 解码后默认格式是 24bppBGR ——
    //   **alpha 通道被丢掉了**，透明底被拍成白色 (255,255,255)。后果正是验收里
    //   "浅色任务栏下不是黑方块"的反面：缩到 16 px 之后四边中点全是不透明的白，
    //   图标会带着一块白底出现在任务栏上。
    //   转换器同时负责预乘/直通的换算，所以这是一次真正的格式转换，不是一次拷贝。
    IWICFormatConverter* converter = nullptr;
    IWICBitmapSource* source = frame;
    if (SUCCEEDED(wic->CreateFormatConverter(&converter)) && converter) {
        if (SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, nullptr, 0.0,
                                            WICBitmapPaletteTypeCustom))) {
            source = converter;
        } else {
            converter->Release();
            converter = nullptr;
        }
    } else if (converter) {
        converter->Release();
        converter = nullptr;
    }

    UINT w = 0, h = 0;
    WICPixelFormatGUID fmt{};
    source->GetSize(&w, &h);
    source->GetPixelFormat(&fmt);
    if (outSrcW) *outSrcW = w;
    if (outSrcH) *outSrcH = h;
    if (outFormat) *outFormat = fmt;
    if (outStream) *outStream = stream;
    if (outDecoder) *outDecoder = decoder;
    // ★ 工厂一定要跟着源交出去：转换器要用它，"源还在用、工厂已经放了"是典型的悬空用法。
    if (outFactory) *outFactory = wic;
    if (source == frame) return frame;
    frame->Release();   // 转换器持有帧
    return converter;
}

void ReleaseIconSource(void* source, void* stream, void* decoder, void* factory) {
    // ★ 顺序不是随便的：源解码时还要回来读流（实测：先放流再 CopyPixels 会失败），
    //   所以按 源 -> 解码器 -> 流 -> 工厂 收（工厂最后放：源是用它做出来的）。
    if (source) static_cast<IWICBitmapSource*>(source)->Release();
    if (decoder) static_cast<IWICBitmapDecoder*>(decoder)->Release();
    if (stream) static_cast<IWICStream*>(stream)->Release();
    if (factory) static_cast<IWICImagingFactory*>(factory)->Release();
}

HICON CreateScaledIcon(void* wicSource, unsigned int sizePx, unsigned char* outBgra,
                       unsigned int* outActualW, unsigned int* outActualH) {
    if (!wicSource || sizePx == 0) return nullptr;
    // 复用 TrayIcon::Install 走的那两个函数：像素怎么来只有一处定义。
    IWICImagingFactory* wic = CreateWic();
    if (!wic) return nullptr;
    std::vector<unsigned char> scaled;
    unsigned int w = 0, h = 0;
    const bool ok = ScaleFrame(wic, static_cast<IWICBitmapSource*>(wicSource), sizePx, &scaled, &w,
                               &h);
    wic->Release();
    if (!ok) return nullptr;
    if (outBgra) {
        std::memcpy(outBgra, scaled.data(), scaled.size());
    }
    // 实际尺寸回给调用方（日志里要写"做出来的是几乘几"，而不是"我要的是几乘几"）。
    if (outActualW) *outActualW = w;
    if (outActualH) *outActualH = h;
    return HiconFromBgra(w, h, scaled);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// 命中测试（纯函数：全部输入显式传进来）
// ---------------------------------------------------------------------------
bool HitGeometry::InsideIcon(POINT p) const {
    // ★ 只有 System（Shell_NotifyIconGetRect 给的精确矩形）才算"我们自己的图标"。
    //   换句话说：**不知道图标在哪 = 这里不是我们的图标 = 不取消免谈**（一律取消）。
    //   这条守卫曾经只挡 None，而"取不到"那一档被塞进整块托盘区域的矩形 ——
    //   等于把"点别人的托盘图标不取消"放回来（见 RefreshIconRect 里那一段）。
    if (iconSource != IconRectSource::System) return false;
    return p.x >= icon.left && p.x < icon.right && p.y >= icon.top && p.y < icon.bottom;
}

bool HitGeometry::InsideMenu(POINT p) const {
    if (!menuValid) return false;
    return p.x >= menu.left && p.x < menu.right && p.y >= menu.top && p.y < menu.bottom;
}

OutsideHit ClassifyOutsideClick(POINT pt, const HitGeometry& g) {
    // 两条"什么都不做"的落点：我们自己的托盘图标、我们自己的菜单。
    // 其余**一切**取消 —— 包括别的程序的托盘图标、时钟、任务栏空白、桌面、别的窗口。
    // ★ 规格要的就是这个方向：宁可多取消（用户点了别处，进度归零是可恢复的），
    //   也不要误吞（点了别人的托盘图标却当作"点在自己图标上"，进度无声地留着）。
    if (g.InsideIcon(pt) || g.InsideMenu(pt)) return OutsideHit::Ignore;
    return OutsideHit::Cancel;
}

// ---------------------------------------------------------------------------
// 安装 / 摘除
// ---------------------------------------------------------------------------
bool TrayIcon::Install(HWND hwnd, UINT id) {
    if (added_) return true;   // 已经挂上：不重复 NIM_ADD（会多出一个图标）
    if (!hwnd) return false;
    hwnd_ = hwnd;
    id_ = id;

    unsigned int srcW = 0, srcH = 0;
    GUID srcFmt{};
    void* streamRaw = nullptr;
    void* decoderRaw = nullptr;
    void* factoryRaw = nullptr;
    void* frameRaw = detail::OpenEmbeddedIconSource(&streamRaw, &decoderRaw, &factoryRaw, &srcW,
                                                    &srcH, &srcFmt);
    if (!frameRaw) {
        TrayLog(L"[tray] 内嵌图标资源读不到或解不开（RCDATA \"DSHB_ICON_PNG\"）：不挂图标");
        detail::ReleaseIconSource(frameRaw, streamRaw, decoderRaw, factoryRaw);
        return false;
    }
    auto* frame = static_cast<IWICBitmapSource*>(frameRaw);

    bool ok = false;
    do {
        const int wantPx = TrayIconSizePx(hwnd);
        // 大图标取系统要的那一档，小图标多给一档（32）：通知区域在部分 DPI 下用小图标
        // 跟我们要，而"缺哪一档就让系统拉伸"正是 F2b 那条验收要防的（大缩放下图标虚）。
        const UINT bigPx = static_cast<UINT>(wantPx);
        const UINT smallPx = (bigPx < 32) ? 32u : bigPx;

        std::vector<unsigned char> big(static_cast<std::size_t>(bigPx) * bigPx * 4, 0);
        UINT bigW = 0, bigH = 0, smallW = 0, smallH = 0;
        hIcon_ = detail::CreateScaledIcon(frame, bigPx, big.data(), &bigW, &bigH);
        TrayLog(L"[tray] 图标源：%ux%u 内嵌 PNG（解码后强制转成 32bppBGRA 保住 alpha）-> "
                L"大 %ux%u 画布内容 %ux%u / 小 %ux%u，重采样=WIC Fant（非最近邻），"
                L"长边等比、居中、空边透明",
                srcW, srcH, bigPx, bigPx, bigW, bigH, smallPx, smallPx);
        if (!hIcon_) {
            TrayLog(L"[tray] 图标缩放或 CreateIconIndirect 失败（大图标）：不挂图标");
            break;
        }
        hIconSmall_ = (smallPx == bigPx)
                          ? nullptr
                          : detail::CreateScaledIcon(frame, smallPx, nullptr, &smallW, &smallH);
        iconPx_ = static_cast<int>(bigPx);
        iconContentW_ = static_cast<int>(bigW);
        iconContentH_ = static_cast<int>(bigH);

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = id_;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kMsgTrayIcon;
        nid.hIcon = hIcon_;
        wcscpy_s(nid.szTip, L"deepseek-balance");
        added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (!added_) {
            TrayLog(L"[tray] NIM_ADD 失败（err=%lu）：通知区域里不会有图标", GetLastError());
            break;
        }
        // uVersion 必须在**添加之后**设：NIM_SETVERSION 会重置提示与回调
        // （它之后 uTimeout/uVersion 才生效），所以在前面设等于白设。
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        TrayLog(L"[tray] NIM_ADD：图标已进通知区域（hWnd=0x%p uID=%u 尺寸=%dpx 内容=%dx%d "
                L"大=%ls 小=%ls）",
                hwnd, id_, iconPx_, iconContentW_, iconContentH_, hIcon_ ? L"有" : L"无",
                hIconSmall_ ? L"有" : L"无");
        ok = true;
    } while (false);

    detail::ReleaseIconSource(frameRaw, streamRaw, decoderRaw, factoryRaw);

    if (!ok) {
        // 半途失败也要收干净：留下一个 NIM_ADD 过的图标而返回 false 是最坏的结果
        // （调用方以为没图标，用户却看得见它，点它没反应）。
        if (hIcon_) { DestroyIcon(hIcon_); hIcon_ = nullptr; }
        if (hIconSmall_) { DestroyIcon(hIconSmall_); hIconSmall_ = nullptr; }
        if (added_) {
            NOTIFYICONDATAW del{};
            del.cbSize = sizeof(del);
            del.hWnd = hwnd;
            del.uID = id_;
            Shell_NotifyIconW(NIM_DELETE, &del);
            added_ = false;
        }
        return false;
    }

    RefreshIconRect();
    return true;
}

void TrayIcon::Remove() {
    if (hMenu_) DestroyMenu();
    if (added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = id_;
        const BOOL gone = Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
        // ★ 摘除之后立刻回读一次：返回 false 只说明"这个调用没成"，不说明图标已经没了。
        //   Shell_NotifyIconGetRect 在这里返回失败（E_FAIL）才是"它真的不在了"的证据，
        //   所以这两行一起看才有意义（验收第 2 条要的正是这个）。
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.hWnd = hwnd_;
        id.uID = id_;
        RECT probe{};
        const HRESULT hr = Shell_NotifyIconGetRect(&id, &probe);
        TrayLog(L"[tray] NIM_DELETE：图标已从通知区域摘掉（NIM_DELETE=%d "
                L"Shell_NotifyIconGetRect hr=0x%08lX -> %ls）",
                gone ? 1 : 0, static_cast<unsigned long>(hr),
                SUCCEEDED(hr) ? L"失败：图标居然还在" : L"失败=图标确实不在了");
    }
    if (hIcon_) { DestroyIcon(hIcon_); hIcon_ = nullptr; }
    if (hIconSmall_) { DestroyIcon(hIconSmall_); hIconSmall_ = nullptr; }
    hwnd_ = nullptr;
    geom_ = HitGeometry{};
    geomFresh_ = false;
    haveIconRect_ = false;
}

// ---------------------------------------------------------------------------
// 图标矩形
// ---------------------------------------------------------------------------
void TrayIcon::RefreshIconRect() {
    if (!added_ || !hwnd_) {
        geom_ = HitGeometry{};
        geomFresh_ = true;
        haveIconRect_ = false;
        iconRectAtMs_ = GetTickCount64();
        return;
    }

    NOTIFYICONIDENTIFIER ident{};
    ident.cbSize = sizeof(ident);
    ident.hWnd = hwnd_;
    ident.uID = id_;
    RECT r{};
    const HRESULT hr = Shell_NotifyIconGetRect(&ident, &r);

    IconRectSource source = IconRectSource::None;
    if (SUCCEEDED(hr)) {
        source = IconRectSource::System;
    } else {
        // ★★ 取不到就是取不到：**不拿整块托盘区域冒充我们的图标矩形**。
        //   失败最常见的原因是**图标被折进了溢出区**（"显示隐藏的图标"那个小三角里），
        //   此时它在屏幕上没有矩形。曾经这里把整块 TrayNotifyWnd 的矩形当成 Degraded 存下来，
        //   而命中测试只看矩形 —— 于是"点别人的托盘图标/时钟不取消"这个毛病从窗口类那条路
        //   原样搬到了矩形这条路上（同一类偏离，换了个判据）。
        //   现在的口径与规格一致：**取不到 = 我们不知道图标在哪 = 一律取消**（宁可多取消）。
        //   托盘区域那块矩形仍然量出来，但它只进日志（给"图标是不是被折起来了"留一条线索），
        //   不进 geom_。
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        HWND notifyWnd = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        RECT trayRect{};
        const bool haveTray = notifyWnd && GetWindowRect(notifyWnd, &trayRect);
        if (haveTray) {
            TrayLog(L"[tray] 图标矩形取不到（hr=0x%08lX，多半是折进了溢出区）："
                    L"**一律取消**（不拿整块托盘区域冒充我们的图标）。"
                    L"托盘区域只是线索=(%ld,%ld,%ld,%ld)，不参与命中测试",
                    static_cast<unsigned long>(hr), trayRect.left, trayRect.top, trayRect.right,
                    trayRect.bottom);
        } else {
            TrayLog(L"[tray] 图标矩形取不到（hr=0x%08lX），连托盘区域也没找到："
                    L"任何外部点击都取消",
                    static_cast<unsigned long>(hr));
        }
        geom_.iconSource = source;
        geom_.icon = RECT{};
        geomFresh_ = true;
        haveIconRect_ = true;
        iconRectAtMs_ = GetTickCount64();
        return;
    }

    const bool changed = (source != geom_.iconSource) || !SameRect(r, geom_.icon);
    geom_.iconSource = source;
    geom_.icon = r;
    geomFresh_ = true;
    haveIconRect_ = true;
    iconRectAtMs_ = GetTickCount64();

    // 只在**变化**时记一行：这一行一秒一次会被刷爆，而它要回答的只是
    // "系统给没给我们的图标矩形、什么时候给不出来"。
    if (changed) {
        TrayLog(L"[tray] 图标矩形（Shell_NotifyIconGetRect 精确值）：(%ld,%ld,%ld,%ld) "
                L"= %ldx%ld 像素",
                r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    }
}

RECT TrayIcon::IconRect(IconRectSource* source) {
    // 只在"上次失败了 / 还没问过"时才有必要重问：成功过就一直准（图标没有理由自己动）。
    // 250 ms 是给"刚被折进溢出区"留的重试节奏 —— 那时它可能马上就有矩形了。
    const ULONGLONG now = GetTickCount64();
    const bool stale = !geomFresh_ || geom_.iconSource != IconRectSource::System;
    if (stale && (now - iconRectAtMs_) >= 250) RefreshIconRect();
    if (source) *source = geom_.iconSource;
    return geom_.icon;
}

void TrayIcon::KeepGeometryFresh() {
    const ULONGLONG now = GetTickCount64();
    // 成功的矩形没有理由每帧重问（那是个跨进程调用）；失败的那一侧按 250 ms 重试。
    const bool needs = !geomFresh_ || geom_.iconSource != IconRectSource::System;
    if (needs && (now - iconRectAtMs_) >= 250) RefreshIconRect();
}

// 我们的菜单面板现在在屏幕上吗？在的话它的矩形是多少？
//
// ★ 为什么按"我们自己的线程 + 我们的类名"两个条件一起认：面板是自建窗口（kMenuPanelClass），
//   而不是系统那个所有弹出菜单共用的 "#32768"。于是"是不是我们的"这件事**类名就够**，
//   不必再去猜别人的菜单。这一条是这次换实现顺手修掉的：旧版认 "#32768" 时，别的程序的
//   弹出菜单与我们的同名，"点别人的菜单不取消"就是这么来的。
//
// ★★ 为什么是 EnumThreadWindows 而不是 FindWindowExW(..., ...)：
//   FindWindow / FindWindowEx **找不到调用线程自己创建的窗口**（这是它们的设计行为）。
//   面板恰好就是我们这个线程创建的，于是那一版永远拿不到菜单矩形 —— 矩形恒为空，
//   点我们自己的菜单也会被判成"点了别处"而取消关闭流程。
//   实测（2026-09-19）：换成 EnumThreadWindows 之前，"点菜单不取消"这一条**从来没生效过**，
//   而日志上看起来一切正常（只是从不打印菜单命中）。EnumThreadWindows 是本线程枚举，
//   不发跨线程消息，微秒级，远在 300 ms 的钩子预算之内。
//
// ★ 为什么在**钩子里**每次点击前调一遍，而不是每帧刷一次：菜单从弹开到用户点下去可能
//   一帧都没过去，缓存里那时还是"没有菜单"，那一下就判错了。
void TrayIcon::RefreshMenuRect() {
    geom_.menuValid = false;
    struct Ctx {
        HWND found;
    } ctx{nullptr};
    EnumThreadWindows(GetCurrentThreadId(),
                      [](HWND h, LPARAM p) -> BOOL {
                          auto* c = reinterpret_cast<Ctx*>(p);
                          wchar_t cls[64] = L"";
                          GetClassNameW(h, cls, 64);
                          if (wcscmp(cls, kMenuPanelClass) == 0) {
                              c->found = h;
                              return FALSE;   // 找到就停
                          }
                          return TRUE;
                      },
                      reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found) return;
    RECT r{};
    if (!GetWindowRect(ctx.found, &r)) return;
    geom_.menu = r;
    geom_.menuValid = true;
}

// ---------------------------------------------------------------------------
// 菜单
//
// ★★ 这一份里有两个"菜单"，它们的角色必须分清，否则下一个人会把其中一个当成死代码删掉：
//   · g_menuPanel（WM_CREATE 那个自建面板）—— **用户在屏幕上看见并点的**就是它；
//   · hMenu_（HMENU，AppendMenu 建出来的）—— **不在屏幕上**，它是"这一份菜单里有哪些项"
//     的唯一出处：id 与文字只在这里定义一次，面板画的是同一对常量
//     （kTrayMenuClose / kTrayCloseText），tools/trayprobe.cpp 与 --tray-probe 量它
//     （GetMenuItemCount/GetMenuStringW/GetMenuItemID）。
//   ★ HMENU 是**探针的边界**：探针要量"只有一项、文字=「关闭」、id=1"，而它只能从 HMENU
//     问出来。所以它不是"旧实现的残留"：删了它，探针就没有可量的对象了。
//     用户在屏幕上看到的那一项由面板自己画（MenuPanelProc 的 WM_PAINT），两者用的是
//     同一对常量，不会分叉。
// ---------------------------------------------------------------------------
HMENU TrayIcon::BuildMenu() {
    if (hMenu_) return hMenu_;
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        TrayLog(L"[tray] CreatePopupMenu 失败（err=%lu）：菜单弹不出来", GetLastError());
        return nullptr;
    }
    // 「关闭」是这一份菜单里**唯一**一项（所有者 2026-09-19）。
    // 设计 §10.3 明写没有「显示/隐藏窗口」（没有隐藏这回事）、也没有第二个「退出」
    // （「关闭」就是退出）。所以这里是 AppendMenu 而不是"再加几项"。
    if (!AppendMenuW(menu, MF_STRING, kTrayMenuClose, kTrayCloseText)) {
        TrayLog(L"[tray] AppendMenu 失败（err=%lu）", GetLastError());
        // ★ 这里必须写 ::DestroyMenu：成员函数恰好也叫 DestroyMenu，不加全局限定
        //   解析到的是**我们自己那个**（参数个数对不上，编译期就报 C2660）。
        ::DestroyMenu(menu);
        return nullptr;
    }
    hMenu_ = menu;
    return hMenu_;
}

void TrayIcon::DestroyMenu() {
    if (hMenu_) { ::DestroyMenu(hMenu_); hMenu_ = nullptr; }
}

// 弹出右键菜单面板（**非模态**），或把已经开着的那一个重新拉回来。
// 返回值是面板句柄；**选中的菜单项不走返回值**，走 kMsgTrayMenu（见 tray.h）。
//
// ★★ 为什么把 TrackPopupMenu 整条换掉（这是这一版的核心改动，理由不是"新写法更漂亮"）：
//   TrackPopupMenu 是**模态**等待 —— 它自己跑消息循环并一直阻塞到菜单被收起。实测
//   （2026-09-19，本机）菜单窗口出现了、但收不到任何输入，于是它永不返回：主线程停在
//   这个调用里，帧循环一行都不推进，托盘右键 = 挂件卡死。而卡死时进程还占着单实例，
//   用户既不能用它、也没法重新启动（设计 §10.3 那条"关闭后任务管理器里没有残留进程"
//   正好被顶掉）。上一任已排除四种解释（没设前台 / WS_EX_NOACTIVATE / 菜单没弹出来 /
//   往菜单窗口投键），那四种都不是原因，所以这一版不再猜下去：
//   **生产路径上不允许存在没有超时的模态等待**，这条与"换不换实现"无关。
//   换成自建面板之后，"等待"这件事根本不存在了：面板是一个普通窗口，帧循环照常跑。
//
// ★ 三个"上一任的坑"在这里的处理：
//   · AttachThreadInput + SetForegroundWindow：**保留**。它要解决的是真问题（托盘图标的
//     右键不是发给我们的窗口的，不设前台则点别处菜单不消失），而且实测过"只调
//     SetForegroundWindow 之后 GetForegroundWindow() 仍是 NULL"。日志里照旧打一份。
//   · 补 PostMessage(WM_NULL)：**删掉**。那是"菜单消失后偶发地卡在屏幕上"的老坑，
//     属于系统菜单窗口的行为；自建面板是 DestroyWindow，没有需要唤醒的消息泵。
//   · GetMenuItemRect 量第一项：**换成量面板里的那一条**（MenuItemRect(Screen)）。
//     HMENU 不在屏幕上，量它没有意义。
HWND TrayIcon::PopupMenu(HWND hwnd) {
    if (!hwnd) return nullptr;
    // ★★ 重入闸（最要紧的一行，理由见 MenuPanelBuildingSentinel() 的注释）：一进来就占住，
    //   因为下面 AttachThreadInput / SetForegroundWindow / CreateWindowExW 每一步都可能
    //   同步投递消息，把队列里的下一条托盘回调就地跑掉 —— PopupMenu 会在自己的肚子里
    //   再进来一次，而那时"面板已经建好"这件事第一趟还没记下来。
    //   哨兵值让第二趟在这里就掉头：不叠第二个面板，也不产生没人管的孤儿窗口。
    if (g_menuPanel == MenuPanelBuildingSentinel()) {
        TrayLog(L"[tray] ★ PopupMenu 重入：上一趟正在建面板（AttachThreadInput / "
                L"SetForegroundWindow / CreateWindowEx 期间消息被就地处理了），这一趟直接返回");
        return nullptr;
    }
    // 已经开着（而且是**活的**窗口）：把焦点拉回来就是"再点一次托盘图标"的全部效果。
    // ★ 这里用 MenuPanelAlive 而不是只判非空：死句柄被当成"还开着"的话，右击会永远
    //   拒绝建新面板 —— 那正是"右击没反应"。
    // ★ 日志里记一行"被挡住了"：这是"一个手势只弹一个面板"的**运行时证据**。
    //   没有它，"已经开着所以没建新的"与"压根没收到第二次调用"在日志上长得一模一样。
    if (MenuPanelAlive()) {
        TrayLog(L"[tray] PopupMenu：面板 %p 已经在屏幕上（IsWindow=是、当前个数=%d），"
                L"这一趟只把它提到前台、不再建第二个",
                g_menuPanel, CountMenuPanelsImpl());
        SetForegroundWindow(g_menuPanel);
        return g_menuPanel;
    }
    // 走到这里说明句柄是空的或已经不是窗口了：把它清掉，下面重建。
    if (g_menuPanel && g_menuPanel != MenuPanelBuildingSentinel()) {
        TrayLog(L"[tray] 记着的面板句柄 %p 已经不是窗口（IsWindow=否）：清掉它并重建",
                g_menuPanel);
    }
    g_menuPanel = MenuPanelBuildingSentinel();

    // 光标位置就是面板左上角（与系统菜单同一个口径：菜单出现在指针下方/右侧）。
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        g_menuPanel = nullptr;
        return nullptr;
    }

    const HWND fgBefore = GetForegroundWindow();
    const DWORD fgThread = fgBefore ? GetWindowThreadProcessId(fgBefore, nullptr) : 0;
    const DWORD myThread = GetCurrentThreadId();
    bool attached = false;
    if (fgThread != 0 && fgThread != myThread) {
        attached = AttachThreadInput(fgThread, myThread, TRUE) != FALSE;
    }
    SetForegroundWindow(hwnd);
    const HWND fgAfter = GetForegroundWindow();

    // ★★ DPI：**换算只有一个方向** —— 把 dip 换成物理像素，而且只用在"窗口该做多大"上。
    //   窗口尺寸（客户区宽/高）是 DIP 语义，所以按窗口 DPI 换算；换算出来的数交给
    //   AdjustWindowRectExForDpi 之后，want.top/left 就是边框的**物理像素**。
    //   面板自己的客户区坐标（WM_PAINT / WM_LBUTTONDOWN 收到的那个）本来就是物理像素，
    //   所以菜单那一条的矩形由 MenuItemRect() 直接从客户区推出来，**不再乘任何比例**。
    //   实测（2026-09-19）：第一版在两个地方各乘了一次，于是"那一条"被算成窗口外的坐标，
    //   一次**点中**被判成"点空白"，日志上一切正常、行为却是取消。两套坐标混用是本项目
    //   反复踩的坑，所以这里把口径写死：尺寸换算、命中不换算。
    const UINT dpi = GetDpiForWindow(hwnd);
    const UINT dpiUse = dpi ? dpi : 96u;
    auto dpiPx = [dpiUse](int v) { return MulDiv(v, static_cast<int>(dpiUse), 96); };
    const int clientW = dpiPx(56);   // 只放得下「关闭」两个字加一点余量
    const int clientH = dpiPx(kMenuItemHeightDip) + 2 * dpiPx(kMenuPaddingDip);

    EnsureMenuPanelClass();
    // WS_EX_TOPMOST：与挂件同一个层级口径（它也是 topmost），否则面板会被别的置顶窗口压住。
    // ★ 这里**不**加 WS_EX_NOACTIVATE：面板必须拿得到激活才会收到鼠标与键盘
    //   （上一任"临时摘掉 NOACTIVATE 症状不变"说的是**主窗口**那个扩展样式，不是这里）。
    const DWORD style = WS_POPUP | WS_BORDER;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    RECT want{0, 0, clientW, clientH};
    AdjustWindowRectExForDpi(&want, style, FALSE, exStyle, dpiUse);
    int winW = want.right - want.left;
    int winH = want.bottom - want.top;

    // 面板要放在光标那儿（与系统菜单同一个口径：菜单出现在指针处/下方）。
    // ★★ 但**必须钳进显示器的工作区**：光标可以在屏幕最右边或最下边（托盘图标就在那儿），
    //   而面板有 112x60 物理像素 —— 照抄光标位置就会把它大半放到屏幕外面（实测两次：
    //   面板被放在 (2879,1799)，屏幕只有 2880x1800，那一条"关闭"根本点不到）。
    //   系统菜单从不出屏，这里是同一个道理：钳住左上角，宁可让面板盖在指针左边/上面。
    int px = cursor.x;
    int py = cursor.y;
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const int maxX = mi.rcWork.right - winW;
            const int maxY = mi.rcWork.bottom - winH;
            if (px > maxX) px = maxX;
            if (py > maxY) py = maxY;
            if (px < mi.rcWork.left) px = mi.rcWork.left;
            if (py < mi.rcWork.top) py = mi.rcWork.top;
        } else {
            // 拿不到工作区（极罕见）：至少别把右/下边放到屏幕外。
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            if (px + winW > sw) px = sw - winW;
            if (py + winH > sh) py = sh - winH;
            if (px < 0) px = 0;
            if (py < 0) py = 0;
        }
    }

    g_menuOwner = hwnd;
    HWND panel = CreateWindowExW(exStyle, kMenuPanelClass, L"", style, px, py, winW, winH, hwnd,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!panel) {
        TrayLog(L"[tray] 菜单面板创建失败（err=%lu）：右键点托盘图标不会有菜单", GetLastError());
        g_menuOwner = nullptr;
        g_menuPanel = nullptr;   // 把哨兵撤掉，否则之后的右击会被"正在建"挡住
        if (attached) AttachThreadInput(fgThread, myThread, FALSE);
        return nullptr;
    }
    // ★ 真句柄在这里才写进去（之前是哨兵）。中间那几步如果发生了重入，第二趟看到的是
    //   哨兵、直接掉头 —— 这是"一次右键只能有一个面板"的全部保证。
    g_menuPanel = panel;
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    UpdateWindow(panel);   // 立刻画一遍：先出像素，再记日志

    // 打开时就量一次那一条的屏幕矩形：与"按下那一刻"再量的那一次对照，
    // 就能看出面板在两次之间有没有被系统挪过（见 ActivateMenuPanel 的日志）。
    const RECT item = MenuItemRect(panel, RectSpace::Screen);
    RECT wr{};
    GetWindowRect(panel, &wr);
    TrayLog(L"[tray] 菜单面板已弹出（**非模态**：帧循环照常跑）：面板=%p 窗口=(%ld,%ld,%ld,%ld) "
            L"「关闭」那一条=(%ld,%ld,%ld,%ld) 尺寸=%dx%d dpi=%u 光标=(%ld,%ld) 落点=(%d,%d) "
            L"AttachThreadInput=%ls 弹出时前台=%ls 时限=%u ms",
            panel, wr.left, wr.top, wr.right, wr.bottom, item.left, item.top, item.right,
            item.bottom, clientW, clientH, dpiUse, cursor.x, cursor.y, px, py,
            attached ? L"用过" : L"不需要/失败", (fgAfter == hwnd) ? L"我们（属主）" : L"别人",
            static_cast<unsigned>(kMenuPanelTimeoutMs));
    if (attached) AttachThreadInput(fgThread, myThread, FALSE);

    // 菜单展开期间缓存下来的菜单矩形要清掉：留着它会让"面板关掉之后落在旧面板区域里的
    // 一次点击"被当成"点在菜单上"而不取消。
    geom_.menuValid = false;
    return panel;
}

// tray.h 里那两个声明（detail 命名空间）的实现：转调文件内的那一个，这样"数面板"这件事
// 只有一处定义。trayprobe 与 main.cpp 都用它。
namespace detail {
int CountMenuPanels() { return CountMenuPanelsImpl(); }
bool MenuPanelsUnionRect(RECT* out) {
    const int n = CountMenuPanelsImpl(out);
    return n > 0 && out != nullptr;
}
}  // namespace detail

}  // namespace dshb
