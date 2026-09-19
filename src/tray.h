// 托盘图标与右键菜单（设计 §10.3、F4）。
//
// ★ 这个模块为什么单独一份：托盘图标是三件互相牵制的事 ——
//   ① 图标资源（PNG 内嵌进 exe -> 多尺寸 HICON，浅色任务栏下不能是黑方块）；
//   ② 通知区域的生命周期（NIM_ADD / NIM_DELETE，进程退出后图标必须消失）；
//   ③ 命中测试（"点的是不是我们自己的图标/菜单"）—— 关闭态那个分类器要问的正是这个。
//   第三件事单独放进 main.cpp 就等于把它焊死在钩子里，量不到；放进这里，
//   tools/trayprobe.cpp 能直接按坐标问它（见 tests/ 那条链）。
//
// ★ 图标数据**内嵌在 exe 里**（src/tray.rc 把 icon.png 作为 RCDATA 打进二进制）：
//   `icon.png` 只在编译期存在，运行期一个外部文件都不读。仓库里那份 1 MB 的 PNG
//   因此不是运行时依赖，用户拿到 exe 就能跑。
#pragma once

#include <windows.h>

namespace dshb {

// ---- 我们自己的托盘消息 ----
// 托盘图标上的鼠标事件（lParam 低位 = WM_LBUTTONUP / WM_RBUTTONUP 等）。
// ★ 编号从 WM_APP+2 起：WM_APP+1 已经是单实例的 kMsgActivate（src/single_instance.h），
//   同一个 WndProc 的 switch 里两个 case 撞号会直接编译不过 —— 这正是想要的失败方式。
inline constexpr UINT kMsgTrayIcon = WM_APP + 2;

// 菜单面板选中了一项 —— 由**面板自己**投给属主窗口，wParam 是选中的 id。
//
// ★ 为什么是**我们自己的消息**而不是 WM_COMMAND：WM_COMMAND 谁都能往窗口里 Post，
//   于是"点菜单「关闭」"就会多出一个能被假造的入口（这正是上一版选 TPM_RETURNCMD 的理由）。
//   这条消息只有一个投递点（tray.cpp 的 ActivateMenuPanel），处理它的地方也只有一处
//   （main.cpp 的 WndProc），所以"菜单命令只有一条真实路径"这条设计保住了。
// ★ 为什么不让面板直接调关闭流程：托盘模块不认识关闭态（那是 main.cpp 的事），
//   模块边界上只过"选中的 id"这一个数字。
inline constexpr UINT kMsgTrayMenu = WM_APP + 3;

// 菜单里唯一一项（所有者 2026-09-19：只有「关闭」，没有「显示/隐藏窗口」、没有第二个「退出」）。
inline constexpr UINT kTrayMenuClose = 1;
inline constexpr wchar_t kTrayCloseText[] = L"关闭";

// 我们这块图标矩形是哪来的 —— 分类器的取舍全部由它决定，所以必须能观察、能上报。
//
// ★★ 只有两档，而这件事本身就是口径：**取不到就是取不到**，不存在"退化成整块托盘区域"
//   的中间档。曾经有过那一档（图标折进溢出区时拿 TrayNotifyWnd 的矩形顶上），而命中测试
//   只看矩形 —— 于是"点别人的托盘图标/时钟不取消"这个毛病从窗口类那条路原样搬到了
//   矩形这条路上。所有权口径是"宁可多取消、不要误吞"，所以取不到 -> None -> 一律取消。
enum class IconRectSource {
    None,     // 还没测过，或 Shell_NotifyIconGetRect 失败（含"图标被折进溢出区"）
    System,   // Shell_NotifyIconGetRect 给的：精确到图标本身，只有这一档算"我们自己的图标"
};

// 一次点击要不要取消关闭流程。只有两个答案 —— 分类里那些"是托盘 / 是菜单 / 是别的程序"
// 的装饰性区分已经删掉：它们对行为没有任何影响（规格：落点落在我们自己的图标或菜单上就
// 什么都不做，其余一切取消）。返回值只用来写日志。
enum class OutsideHit {
    Ignore,   // 落在我们自己的托盘图标、或我们的菜单上 -> 什么都不做
    Cancel,   // 其余一切 -> 取消关闭流程（碰我们自己的窗口由调用方先拦掉）
};

// 命中测试的输入全都显式传进来：这样它是个纯函数，tools/trayprobe.cpp 可以按坐标问它，
// 不需要真的弹一个菜单、也不需要真的有一个托盘图标。
struct HitGeometry {
    RECT icon{};                 // 我们的图标矩形（屏幕坐标）
    IconRectSource iconSource = IconRectSource::None;
    RECT menu{};                 // 我们的菜单矩形（屏幕坐标；空 = 菜单没弹着）
    bool menuValid = false;

    bool InsideIcon(POINT p) const;
    bool InsideMenu(POINT p) const;
};

// pt / rects 都是屏幕坐标。
OutsideHit ClassifyOutsideClick(POINT pt, const HitGeometry& g);

class TrayIcon {
public:
    // 把图标挂到通知区域：读内嵌的 PNG -> 生成各档 HICON -> NIM_ADD -> 存下图标矩形。
    // 成功返回 true。失败不是崩溃点，但一定留痕（[tray] 那几行）。
    bool Install(HWND hwnd, UINT id);

    // 从通知区域摘掉并释放图标：NIM_DELETE + DestroyIcon。
    // ★ 必须走这一步（WM_DESTROY 与进程退出的每条路径上）：NIM_ADD 之后进程直接死掉
    //   会在任务栏上留一个**僵尸图标**——它悬停仍有提示、点它没有任何反应，只有鼠标
    //   扫过去才消失，而"用户看到的"就是"这个程序关了但图标还在"（设计 §10.3 明确否掉）。
    void Remove();

    bool installed() const { return added_; }
    HWND hwnd() const { return hwnd_; }
    UINT id() const { return id_; }
    int iconSizePx() const { return iconPx_; }

    // 我们这块图标矩形。系统那一刻没算出来（图标被折进溢出区 / 还没添加 / 系统忙）就
    // 退化为整块托盘区域，"是精算还是退化"一并报出来（见 KeepIconRectFresh 的注释）。
    // 调用方每秒只该问它几次：内部按 Shell_NotifyIconGetRect 是否***失败***设了 250 ms
    // 的重试间隔 —— 那正是"图标被折进溢出区之后立刻点它"的情形，晚了就判错了。
    RECT IconRect(IconRectSource* source);

    // 定期调用（主循环每帧一次）：刷新缓存下来的**图标矩形**，供钩子零成本读取。
    // 菜单矩形不在这里刷 —— 它只在"菜单正弹着"时需要，而那件事只有钩子判得准
    // （见 tray.cpp 里 RefreshMenuRect 的注释）。
    void KeepGeometryFresh();

    // 钩子回调里**只能**读这一份（不能在里面调 Shell_NotifyIconGetRect：那是跨进程的
    // 外壳调用，而 WH_MOUSE_LL 的回调超过约 300 ms 不返回会被系统静默摘除）。
    const HitGeometry& cachedGeometry() const { return geom_; }

    // ---- 菜单 ----
    // 建菜单（**里面只有一项**）。返回菜单句柄；调完就能用 GetMenuItemCount /
    // GetMenuStringW 量它 —— 那比截图硬。
    // ★ 这个 HMENU 是**探针的边界**，不是屏幕上那个面板：屏幕上的「关闭」由自建面板
    //   自己画（它不在屏幕上，也不参与命中测试）。两者用同一对常量
    //   （kTrayMenuClose / kTrayCloseText），所以"量到的"与"看到的"不会分叉。
    HMENU BuildMenu();
    void DestroyMenu();

    // 菜单句柄（没建过则 nullptr）：只给"这一份菜单里有哪些项"的**离线/日志**取证用
    // （tools/trayprobe.cpp 的 T11..T15、--tray-probe 的 [tray] 事实那一行）。
    HMENU menuHandle() const { return hMenu_; }

    // 内嵌 PNG -> 缩放 -> HICON 的实际尺寸（供日志与探针核对"出来的是几乘几"）。
    int iconContentWidth() const { return iconContentW_; }
    int iconContentHeight() const { return iconContentH_; }

    // ---- 菜单面板 ----
    // 弹出右键菜单面板（**非模态**）。返回面板的窗口句柄（0 = 没弹出来 / 没窗口）。
    // ★★ 选中的菜单项**不走返回值**，走 kMsgTrayMenu —— 所以这个调用**立刻返回**，
    //   没有任何等待。旧实现用 TrackPopupMenu（模态），菜单收不到输入时它永不返回，
    //   主线程就卡在那里 = 托盘右键把挂件弄死（tray.cpp 里写了为什么整条换掉）。
    // ★ 经典坑仍然处理：不先 SetForegroundWindow，面板在用户点别处时**不消失**
    //   （托盘图标的右键不是发给我们的窗口的，这是托盘的固有形状）。
    HWND PopupMenu(HWND hwnd);

private:
    void RefreshIconRect();
    void RefreshMenuRect();

    HWND hwnd_ = nullptr;
    UINT id_ = 0;
    HICON hIcon_ = nullptr;        // 大图标：取系统给托盘用的那一档
    HICON hIconSmall_ = nullptr;   // 小图标（通知区域在部分 DPI 下要这一档）
    HMENU hMenu_ = nullptr;
    bool added_ = false;
    int iconPx_ = 0;               // 大图标边长（像素）
    int iconContentW_ = 0;         // 画布里真的有内容的宽度（等比缩放后可能不是正方形）
    int iconContentH_ = 0;

    HitGeometry geom_{};
    bool geomFresh_ = false;       // 这一帧是否已经问过系统
    bool haveIconRect_ = false;
    ULONGLONG iconRectAtMs_ = 0;   // 上次真的调 Shell_NotifyIconGetRect 的时刻
};

// ---- 图标那一半的"裸"接口：只做"内嵌 PNG -> 某档尺寸的 HICON"，不碰通知区域 ----
//
// ★ 为什么公开出来：tools/trayprobe.cpp 要量的就是**这一份**像素（各档 HICON 的四角
//   alpha、边缘斜坡、浅/深底对照图）。探针里再抄一遍缩放与建图标的代码，量到的就是
//   抄件，而不是托盘里那个东西 —— 那正是"资源管线"这一层最容易骗过自己的地方。
//   IWICBitmapSource 是故意的不透明指针：调用方不需要认识 WIC 就能把源传给下面两个。
namespace detail {

// 下面每一个 void* 都是一个 COM 接口，名字写在注释里；调用方只需要**成对**地用
// ReleaseIconSource() 把它们放掉，不必认识 WIC 的类型。

// 内嵌 PNG -> 可直接缩放的解码源（IWICBitmapSource*），已**强制**转成直通 32bppBGRA。
// ★ 解码 + 格式转换只此一处：托盘安装与探针都从它拿源，所以"量到的像素"就是"托盘里的像素"。
// ★ 为什么必须强制转格式：实测这张 icon.png 解码后默认是 24bppBGR —— alpha 被丢掉、
//   透明底被拍成白色，缩到 16 px 就是"任务栏上带一块白底"。转换器负责这一层。
// ★ 它把流与解码器一起返回：源解码时还要回来读流（实测：先放流再 CopyPixels 会失败），
//   所以这几件的生命周期是**一起**的，由 ReleaseIconSource 统一收。
// outFormat 回填的是**转换之后**的格式（调用方据此确认 alpha 真的还在）。
void* OpenEmbeddedIconSource(void** outStream, void** outDecoder, void** outFactory,
                             unsigned int* outSrcW, unsigned int* outSrcH, GUID* outFormat);

// 上面那几件一起放掉（源是用哪个工厂做出来的，工厂就得活到源用完：转换器要用它）。
void ReleaseIconSource(void* source, void* stream, void* decoder, void* factory);

// 把缩放源（OpenEmbeddedIconSource 给的那个 void*）缩到 sizePx 见方的画布，并建成带
// alpha 的 HICON。**最长边**缩到 sizePx、另一边等比、结果居中，空边保持透明。
// 未预乘的 32bppBGRA 像素同时拷进 outBgra（sizePx*sizePx*4 字节，自上而下），
// 供调用方直接量像素。outBgra 可以传 nullptr（只要图标、不要像素）；
// outActualW/outActualH 回填"画布里真的有内容的那块是几乘几"。失败返回 nullptr。
HICON CreateScaledIcon(void* wicSource, unsigned int sizePx, unsigned char* outBgra,
                       unsigned int* outActualW, unsigned int* outActualH);

// 屏幕上**现在**有几个我们的菜单面板窗口（本进程、类名 DshbTrayMenu）。
//
// ★ 为什么需要它：这次修的是"一次右键叠出两个面板、第一个永远不消失"，而这件事只有
//   **数窗口**能证伪 —— 日志里"已弹出"出现两次既可能是重入（bug），也可能是用户点了
//   两次（正常）。夹具（main.cpp 的 --tray-menu-test）用它把"屏幕上到底有几个"直接量
//   出来，而不是从日志行数去猜。
// ★ 用 EnumWindows（枚举全部顶层窗口）再按类名+进程号过滤，因为"屏幕上真的有几个"
//   问的就是这个；EnumThreadWindows 只看得见某个线程自己建的窗口。
int CountMenuPanels();

// 屏幕上所有面板窗口并起来占的那块矩形（物理像素）。没有面板时返回 false、不动 out。
// ★ 为什么需要：所有者说按钮"太大了" —— 而"一个面板真有这么大"与"几个面板叠在一起"
//   在屏幕上看起来一样。个数 + 并集矩形这两个数一起看，才能把那两种情况分开。
bool MenuPanelsUnionRect(RECT* out);

}  // namespace detail

}  // namespace dshb
