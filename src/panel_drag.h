// panel_drag.h —— 面板的自控拖动、边缘吸附、边界钳制、位置持久化
// （v0.2 设计 §10.1 / §10.1.1 / §11.2）
//
// 为什么单独一个模块：这几件事全是**纯几何 + 一个文件**，与渲染、取样、心跳都没有关系。
// 放在这里就能被离线探针逐条量（tools/dragprobe.cpp），而不必靠"人拖一下看看"——
// 1 像素的漂移、重开是不是落在同一个像素上，肉眼都判不了。
//
// 单位口径（这一条最容易错，写死在这里）：
//   · 所有坐标都是**屏幕像素**，都在同一个虚拟屏幕坐标系里（副屏在主屏左边时是负数）。
//   · 窗口位置 = 窗口矩形左上角，与 SetWindowPos / GetWindowRect 同一口径。
//   · 拖动锚点 = 光标屏幕坐标 − 窗口左上角屏幕坐标（按下那一刻固定）。
//   · 面板（实体视觉区）**在窗口里面**：画布 = 面板 + 四周各一份透明余量，所以
//     面板矩形 = 窗口左上角 + (m, m)，尺寸 (315s, 129s)，其中 m = 80s、s = 窗口像素宽 ÷ 475。
//     那圈余量是**透明且不拦鼠标**的，用户看到的"面板"就是实体区 —— 所以凡是"面板在
//     屏幕上怎么样"的判据（吸附、可见面积）都拿面板矩形算，不拿窗口外框算。
//   · s 从**窗口尺寸**推（不写死 80/315/129）：--ui-scale 与 --dpi 会改窗口像素尺寸。
//   这台机器缩放 200%，一旦混进"逻辑像素"（DIP），窗口就会以 2 倍的步子跑 —— 所以
//   本模块里没有 DIP、没有 GetDpiForWindow，只有"窗口像素 → 面板像素"这一次推导。
//
// ★ 本模块的位置数学**全是纯函数**：位置 = 光标 − 锚点，吸附 = 位置的函数，钳制 = 位置的
//   函数。没有任何"上一次吸住了没有"的状态 —— 所以"拖走立即脱开"是恒等式，不是调出来的。

#pragma once

#include <windows.h>

#include <string>

namespace dshb {

// ---------------------------------------------------------------------------
// 面板几何：把"窗口像素"翻译成"面板实体在屏幕上的矩形"
// ---------------------------------------------------------------------------

// 面板（实体视觉区）与它在窗口里的偏移，单位都是**窗口像素**（与屏幕像素 1:1）。
// 由窗口尺寸推出，见 PanelForWindow。默认值就是 s=1（窗口 475x289）那一种。
struct PanelGeometry {
    int margin = 80;      // 窗口左上角 → 面板左上角的偏移（两轴相同）
    int width = 315;      // 面板宽
    int height = 129;     // 面板高
};

// 由窗口像素尺寸推面板几何：s = 窗口宽 ÷ (315 + 80×2)，面板 = 设计尺寸 × s。
// 为什么用窗口尺寸推而不是读 --ui-scale/--dpi：那两个开关只改窗口像素尺寸，模块拿不到它们；
// 而窗口尺寸是这两条路的**共同结果**，推出来的东西与渲染器画出来的必然一致
// （renderer.cpp: m = kMarginDip×scale + 0.5，实体 = kEntityWidthDip×scale + 0.5，同一个 s）。
// 尺寸离谱（0 或负）时退回 1:1：宁可算错一个像素，也不能拿负的宽高去算面积。
PanelGeometry PanelForWindow(SIZE windowSize);

// 窗口左上角 + 几何 → 面板在屏幕上的矩形。吸附与"面板可见多少"都用它。
RECT PanelRectForWindow(POINT windowTopLeft, const PanelGeometry& panel);

// ---------------------------------------------------------------------------
// 吸附（所有者定案：面板实体贴工作区的边）
// ---------------------------------------------------------------------------

// 吸附距离：面板最近的那条边离工作区对应的边 <= 这么多像素，就吸上去。
// 取 24 的理由：面板宽 315，24 大约是它的 1/13 —— 手觉得"靠近了"，但光标离屏边还有
// 一截时不会误吸（面板比光标小得多，拿光标当判据时这个距离要换算成 ~157 才能生效）。
// 单位是屏幕像素，与 --ui-scale 无关（一个开关不该改变"多近算近"）。
inline constexpr int kSnapDistancePx = 24;

// 吸附的纯几何：**未吸附时**的面板矩形，哪条边距工作区对应边 <= kSnapDistancePx 就吸哪条。
// 两块轴各判各的，所以四个角也能吸（两轴同时命中）。
//   · 判据是**面板的边**距屏边的距离，不是光标的 —— 光标拖在面板中央时离屏边还有 ~157px，
//     拿光标当判据永远吸不上（这是这一步最容易写错的地方）。
//   · 返回值只改命中的那个轴；没有命中就原样返回 desiredTopLeft（一个像素都不动）。
//   · 不设"光标必须还在面板里"的上限：右/下两条边要贴住屏边时它与吸附目标互斥
//     （见 .cpp 里的算式），所有者定的目标是"面板贴住屏边"，所以按几何算。
POINT SnapTopLeftIn(POINT desiredTopLeft, const PanelGeometry& panel, const RECT& workArea);

// 实际用的入口：工作区取**光标所在**那块（与钳制同一条路，多显示器才有一致的答案）。
POINT SnapTopLeft(POINT desiredTopLeft, const PanelGeometry& panel, POINT cursor);

// ---------------------------------------------------------------------------
// 边界钳制
// ---------------------------------------------------------------------------

// 钳制规则（设计 §10.1「可见性」、验收 A15）：**面板**任何时刻都必须有
// >= kMinVisibleAreaFraction 的**面积**落在某块显示器的工作区内。
//
// ★ 为什么是面板而不是窗口（口径变更的**唯一**理由，别再按"换了个量法"理解）：
//   分母必须是用户看得见的东西。窗口外框里有 80px 透明余量，用户看不见它，
//   而外框比面板大得多（475 对 315），所以按窗口算面积**比按面板算宽松得多**：
//     · 面板贴 80px 余量（面板左 = 工作区左 + 80）：面板可见 100%、窗口可见 83% —— 两个口径都放行；
//     · 面板一半悬在屏外：**面板可见 38%、窗口可见 52%** —— 窗口口径放行，面板口径拒绝。
//   也就是说这次是**真的收紧了约束**（面板最多悬出去 50%），不是换单位。
//   代价：以前存在 config.json 里的一批"面板只露一点"的位置，现在会被 LoadWindowPos 判非法。
// 分母是面板自己的面积；"50%" 是面积比，不是每个轴各一半（两轴各剩一半时面积只有 25%）。
inline constexpr double kMinVisibleAreaFraction = 0.5;

// 没有 config.json 可用时的起点（也是本步之前那个写死的值）。
inline constexpr int kWindowXDefault = 240;
inline constexpr int kWindowYDefault = 240;

// 某一点所在（最近）显示器的工作区。rcWork，不是 rcMonitor —— 任务栏不算可用区。
// 点落在所有显示器之外时取**最近**的那块：钳制和"存下来的位置还算不算数"都必须有一个
// 确定的答案，返回"没有"会让调用方各写一套兜底。
struct WorkArea {
    RECT rect{};
    bool valid = false;
};

WorkArea WorkAreaForPoint(POINT point);

// rect 落在 workArea 里的面积 ÷ rect 自己的面积。空交集返回 0。
// 分母由调用方决定传的是哪个矩形 —— 本模块的判据一律传**面板**矩形（见上）。
double VisibleAreaFraction(const RECT& rect, const RECT& workArea);

// 钳制的核心：把想要的窗口左上角收进给定的工作区，保证**面板** >= kMinVisibleAreaFraction
// 的面积在里面。与下面那个只差"工作区从哪来"，拆开是为了让钳制规则本身可以用合成的显示器量
// （真实机器上只有一块 2880×1800 的屏，量不到"窗口比屏还大"这种输入）。
POINT ClampWindowTopLeftIn(POINT desiredTopLeft, const PanelGeometry& panel, const RECT& workArea);

// 实际用的入口：工作区取**光标所在**显示器的那一块（拖动中光标就是"你想把它放哪"）。
POINT ClampWindowTopLeft(POINT desiredTopLeft, const PanelGeometry& panel, POINT cursor);

// ---------------------------------------------------------------------------
// 拖动算术与位置持久化
// ---------------------------------------------------------------------------

// 拖动锚点（口径见文件头）。
POINT DragOffsetFor(const RECT& window, POINT cursor);
POINT DragTopLeft(POINT dragOffset, POINT cursor);

// config.json 里的窗口位置。字段名 windowX / windowY（整数，屏幕像素）。
//
// 读的容错（三条都要求）：文件不存在 / JSON 损坏 / 坐标不在任何显示器内 -> 返回 false，
// 由调用方退回 (kWindowXDefault, kWindowYDefault)。窗口尺寸要传进来，因为"还算不算数"
// 用的就是钳制那一条规则（**面板** >= 50% 面积在某块显示器工作区内）—— 见 .cpp 里的说明。
bool LoadWindowPos(const std::wstring& path, SIZE windowSize, POINT* outTopLeft,
                   std::wstring* why);

// 写的容错：文件里**别的字段原样保留**（它是用户的设置文件，以后会加字段，这个版本不能
// 把别人的字段吃掉）；写失败返回 false 并把原因写进 why，不抛、不崩、不弹窗。
bool SaveWindowPos(const std::wstring& path, POINT topLeft, std::wstring* why);

}  // namespace dshb
