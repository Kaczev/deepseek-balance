// ===========================================================================
//  tuning.h —— 外观调参表（唯一出处）
// ===========================================================================
//
//  这里放所有"看一眼觉得不对就改"的数字：字号、面板尺寸、圆角、颜色、
//  数字的落点与间距、各处透明度。改完重跑 build.bat 就生效。
//
//  约定：
//    · 单位一律是 **DIP**（与显示器 DPI 无关）；实际像素 = DIP × scale，
//      scale 由 --dpi / --ui-scale 或窗口所在显示器的 DPI 决定。
//    · 只放**外观**常量。逻辑用的量（取样间隔、时间常数、吸附阈值、
//      各种阈值判定）不在这里，它们属于行为，不属于外观。
//    · 这里没有"魔法数字"以外的含义：每一项上面一行都写明它影响什么。
//
//  改完之后怎么看效果：
//    双击 build.bat 编译 → build\dshb.exe --roll=loop      看真机效果
//    想看某个具体金额：build\dshb.exe --roll=0 --roll-to=1234.56 --export-frame=1 --out=x.png
//
//  ⚠ 构建时不要让 dshb.exe 正在运行，否则链接会报 LNK1104。

#pragma once

#include <cstdint>

namespace dshb {

// ---------------------------------------------------------------------------
// 1. 画布与面板几何
// ---------------------------------------------------------------------------

// 面板（实体视觉区）的尺寸。改变它会影响整个版面：数字的居中、标题的位置
// 都相对它计算。
inline constexpr int kEntityWidthDip = 315;
inline constexpr int kEntityHeightDip = 129;

// 面板四周向外的透明余量（给投影/抗锯齿留白，不显示任何内容）。
// 画布 = (实体区 + 2 × margin)，所以 315x129 + 80×2 = 475x289。
inline constexpr int kMarginDip = 80;

// 面板圆角半径。
inline constexpr int kCornerRadiusDip = 12;

// ---------------------------------------------------------------------------
// 2. 字体大小（DIP）
// ---------------------------------------------------------------------------

// 数字字号。**所有金额用同一个字号**（所有者的决定，不再按位数分档）。
//
// 曾经是按位数分档的（位数越多字号越小），但那是没有根据的谨慎：实测在 315 DIP
// 宽的面板里，即使用到 7 位（99999.99），数字两侧仍各空 70 px 以上，根本不会挤到边。
// 所以现在只有一个值；觉得数字偏大或偏小，就改这一个数。
inline constexpr float kNumberFixedSizeDip = 41.0f;

// 右上角那行："deepseek 余额"（同时兼状态文案）。
inline constexpr float kTitleSizeDip = 15.0f;

// 币种符号（¥）的字号。它只在"币种已知"时画。
inline constexpr float kCurrencySizeDip = 30.0f;

// 面板底部那行预估文案（例如"约可用 X 天"）。
inline constexpr float kEstimateSizeDip = 15.0f;

// 调试浮层的字号（等宽字体 Consolas）。
inline constexpr float kDebugSizeDip = 13.0f;

// ---------------------------------------------------------------------------
// 3. 版面里的位置与间距（DIP）
// ---------------------------------------------------------------------------

// 标题相对面板左上角的内缩。
inline constexpr float kTitleInsetXDip = 12.0f;
inline constexpr float kTitleInsetYDip = 8.0f;

// 数字与币种符号之间的空档。
inline constexpr float kAmountSymbolGapDip = 2.0f;

// ★ 数字的**垂直锚点**：数字排版原点在面板中线下方多少 DIP。
//   数字墨迹从原点下方约 9.9 DIP 开始、高约 33 DIP（40 DIP 字号实测），
//   所以墨迹中心在原点下方约 26.5 DIP —— 取 26.5 时墨迹正好居中于面板中线。
//   字号变小（位数变多）时这个偏移量应跟着变小，否则数字会显得偏上；
//   要精确居中就把它按当前字号换算（当前是固定值，已知残留：7 位时偏上约 8 px）。
inline constexpr float kNumberTopOffsetDip = 26.5f;

// 面板底部预估文案距面板底边的内缩。
inline constexpr float kEstimateInsetDip = 26.0f;

// ---------------------------------------------------------------------------
// 3b. 显示数字跟随实际数字的三个常数（所有者指定）
// ---------------------------------------------------------------------------


// ★ 滚动速度：每帧把"剩余量"乘上这个系数（所有者给的做法：记一个量每帧自乘，不做幂运算）。
//   剩余 = D × rate^k，所以 rate 越小滚得越快、越早到位；越接近 1 越慢越长。
//   ★ 这是**每帧**系数（窗口是垂直同步的，约 60 帧/秒），不是每秒系数：
//     改成按秒算就要用 pow(rate, dt×60)，而所有者要的正是避免幂运算。
inline constexpr float kRollRate = 0.975f;

// ★ 每位自己的截断阈值（单位：格）。**每一位分开判断**：
//   当"这一位自己的剩余移动距离" < 这个值时，直接把这一位放到位（坐标 = 目标整数）。
//   注意不是全体共用一个截止时刻——每位按自己还剩多少来收尾，
//   这样某一位先到位就先停下来，不会被其他位的进度拖住。
//   一位的"格"就是它自己的一个数字：距离 1.0 格 = 正好走到下一个数字。
inline constexpr float kRollSnapGrid = 0.01f;

// ★ 所有者定的规则：取不到值时**先当作没变**（显示保持原样），
//   连续失败到第 5 次才显示 --.--。
inline constexpr int kUnreadableAfterFailures = 5;

// ★ 所有者定的自适应节奏：取值**有变化**就把间隔缩短 1 秒（最快 3 秒），
//   **没变化**就延长 1 秒（最慢 10 秒）。
inline constexpr int kApiIntervalMaxMs = 10000;
inline constexpr int kApiIntervalMinMs = 3000;
inline constexpr int kApiIntervalStepMs = 1000;

inline constexpr float kNumberShiftRate = 0.90f;   // 整块数字横向滑动：每帧把残差乘上它（同 kRollRate 的风格）
inline constexpr float kNumberShiftSnapDip = 0.05f;   // 横向残差 < 它就直接吸附（免得永远差一点点）

// ★ 缓动曲线指数 c：位置 = L + D × (1 − rate^k)^c。
//   c = 1 就是原来的 (1 − rate^k)；c > 1 起步更慢、尾段更有"收"的感觉；
//   c < 1 起步更快。这是外观旋钮，由所有者调。
inline constexpr float kRollCurveC = 10.0f;

// ★ 步长 D 的取整口径（两种都留着，便于对比）：
//   true  = floor((R − L)/n)          —— 所有者指定的写法
//   false = floor(R/n) − floor(L/n)   —— 起点不在整数格上时不会多走一步
//   两者只在"起点不是 n 的整数倍"时不同。
inline constexpr bool kRollDDiffFloor = false;  // 默认 B：floor(R/n) − floor(L/n)

// ---------------------------------------------------------------------------
// 4. 颜色与透明度
// ---------------------------------------------------------------------------

// 面板底色 #6c89f6（"充足"档的基准色），分量为 0..1。
// ---- 面板与文字（所有者 2026-09-16 定）----
// 背景 #1b1b1c、文字 #f9fafb
inline constexpr float kPanelColorR = 0x1b / 255.0f;
inline constexpr float kPanelColorG = 0x1b / 255.0f;
inline constexpr float kPanelColorB = 0x1c / 255.0f;
inline constexpr float kTextColorR = 0xf9 / 255.0f;
inline constexpr float kTextColorG = 0xfa / 255.0f;
inline constexpr float kTextColorB = 0xfb / 255.0f;

// 边缘（标题 / 倒计时 / 底部文案）文字色 #afb2b7
inline constexpr float kEdgeTextColorR = 0xaf / 255.0f;
inline constexpr float kEdgeTextColorG = 0xb2 / 255.0f;
inline constexpr float kEdgeTextColorB = 0xb7 / 255.0f;

// 面板边框：颜色 #afb2b7，线宽 4 DIP
inline constexpr float kBorderColorR = 0xaf / 255.0f;
inline constexpr float kBorderColorG = 0xb2 / 255.0f;
inline constexpr float kBorderColorB = 0xb7 / 255.0f;
inline constexpr float kBorderWidthDip = 4.0f;

// ---- 氛围曲线（D 段）----
// D1：先画一条与数据无关的正弦线，只证明"能画出平滑曲线"。
// 颜色取边缘文字那个中性色，透明度先给氛围档；D2 会专门调透明度。
inline constexpr float kCurveColorR = 0xaf / 255.0f;
inline constexpr float kCurveColorG = 0xb2 / 255.0f;
inline constexpr float kCurveColorB = 0xb7 / 255.0f;
inline constexpr float kCurveAlpha = 0.35f;
inline constexpr float kCurveWidthDip = 2.0f;
inline constexpr float kCurveAmplitudeDip = 14.0f;
inline constexpr float kCurveCenterYDip = 65.0f;   // 实体区内的中线 y（65 = 绝对 145 = 数字墨迹中线）
inline constexpr float kCurvePeriods = 2.5f;

// ★ 状态主色：原来那支蓝 #6C89F6。**保留**，后面演示各种状态时用；
//   面板与文字已改成上面的中性色，所以它现在不参与面板底色。
inline constexpr float kStatePrimaryR = 108.0f / 255.0f;
inline constexpr float kStatePrimaryG = 137.0f / 255.0f;
inline constexpr float kStatePrimaryB = 246.0f / 255.0f;

// 面板整体不透明度（0 = 全透明，1 = 不透明）。
inline constexpr float kPanelOpacity = 0.95f;


// 各处文字的透明度（文字一律白色，只有透明度不同）。
inline constexpr float kTitleAlpha = 1.00f;      // 边缘文字：设为 1.0 才能渲染成 #afb2b7 本身
inline constexpr float kAmountAlpha = 1.00f;     // 数字本身
inline constexpr float kCurrencyAlpha = 1.00f;   // ¥ 符号：要精确等于 #f9fafb
inline constexpr float kEstimateAlpha = 1.00f;   // 边缘文字：同标题，渲染成 #afb2b7

// 调试浮层里用的几个颜色（正常运行时看不到）。
inline constexpr float kProbeRedAlpha = 0.50f;   // 预乘自检用的纯红方块
inline constexpr float kWarnR = 1.00f;           // 警告色（偏黄）
inline constexpr float kWarnG = 0.94f;
inline constexpr float kWarnB = 0.60f;
inline constexpr float kWarnAlpha = 0.95f;

}  // namespace dshb
