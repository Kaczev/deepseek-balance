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
// 4. 颜色与透明度
// ---------------------------------------------------------------------------

// 面板底色 #6c89f6（"充足"档的基准色），分量为 0..1。
inline constexpr float kPanelColorR = 108.0f / 255.0f;
inline constexpr float kPanelColorG = 137.0f / 255.0f;
inline constexpr float kPanelColorB = 246.0f / 255.0f;

// 面板整体不透明度（0 = 全透明，1 = 不透明）。
inline constexpr float kPanelOpacity = 0.95f;

// 同一支底色的 32 位写法（窗口创建/清屏路径用，格式 0xAABBGGRR 的 RGB 部分）。
// 改底色时**两处都要改**：上面的三个分量，和这里的整数。
inline constexpr uint32_t kBaseColorBgra = 0xF6896C;

// 各处文字的透明度（文字一律白色，只有透明度不同）。
inline constexpr float kTitleAlpha = 0.85f;      // 右上角标题
inline constexpr float kAmountAlpha = 1.00f;     // 数字本身
inline constexpr float kCurrencyAlpha = 0.90f;   // ¥ 符号
inline constexpr float kEstimateAlpha = 0.75f;   // 底部预估文案

// 调试浮层里用的几个颜色（正常运行时看不到）。
inline constexpr float kProbeRedAlpha = 0.50f;   // 预乘自检用的纯红方块
inline constexpr float kRingAlpha = 0.95f;       // 调试用的光环
inline constexpr float kWarnR = 1.00f;           // 警告色（偏黄）
inline constexpr float kWarnG = 0.94f;
inline constexpr float kWarnB = 0.60f;
inline constexpr float kWarnAlpha = 0.95f;

}  // namespace dshb
