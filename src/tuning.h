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
// 3c. 消耗速率与清零预估（设计 §7.2 / §7.4）
// ---------------------------------------------------------------------------
//  这里的数字都是**行为**参数，但它们放在这张表里，因为所有者要"看一眼觉得
//  不对就改"——速率估计的每一条阈值都在 src/rate_estimator.cpp 里按名字取用，
//  不散落在代码中间。单位与出处逐条写明。
//
//  ★ 估计口径（所有者 2026-09-16 定，别再改回"拟合斜率"）：
//      消耗 = Σ max(0, 前一点 − 后一点)   —— 只把**下降**的台阶加起来
//      速率 = 消耗 / (最老到最新一个可用点之间的跨度)   —— 元/分钟，**永远不为负**
//    回升（充值/赠金）**不作废窗口**：它只让**它自己那一个台阶**贡献 0，其余台阶
//    与时间照常参与。一个下降台阶都没有的窗口，速率是**恰好 0**（不是"不显著"）。

// ★ 弹簧时间常数（秒）：rate_display 追 rate 的时间常数。
//   几秒量级：太短等于没有平滑（每次采样数字跳一下），太长会把"刚充值"这类
//   真实变化也糊掉。6 秒 ≈ 半衰期 4.2 秒。
inline constexpr double kRateSpringTauSeconds = 6.0;

// ★ 弹簧每步的 dt 上限（秒）。设计 §9.6 的通病表：dt 不钳制，休眠唤醒后的
//   第一帧 dt 巨大，动画一步跳到位。50 ms 与 §9.6 里的 dt 钳制同一个值，
//   故意保持一致。
inline constexpr double kRateSpringMaxDtSeconds = 0.05;

// ★ 速率取整位数（元/分钟）。取整后为 0 的速率按"不显著"处理：报一个 1e-11 元/分钟
//   等于编了个数字。（真实数据到不了这一步：跨度上限 86400 s、最小下降 1 raw
//   也还有 6.9e-8 元/分钟。这一条只是把"不许报假数字"写死。）
//   10 位 = 1e-10，比任何真实消耗都细，所以取整只吃浮点噪声。
inline constexpr int kRateRoundDigitsYuanPerMinute = 10;

// ★ 显著的**最少**下降**台阶**数（设计 §7.3「少于 3 个下降样本 -> 不显著」）。
//   数的是 Σ max(0, 前−后) 里真正被加起来的那些台阶：一个台阶都没下降 -> 速率 0
//   （走破折号那一条，见 rate_estimator.cpp 的判定顺序），一个或两个下降台阶
//   不足以支撑一个速率。两个台阶可以"算出"任何东西，所以这条不能松。
inline constexpr int kRateMinDecreasingSamples = 3;

// ★ 显著的**最短**时间跨度（秒）= 5 分钟（设计 §7.3「窗口跨度 < 5 分钟 ->
//   不显著」）。注意这是**时间跨度**，不是点数：10 个点挤在 4 秒里同样不显著。
inline constexpr int64_t kRateMinSpanSeconds = 300;
// ★ 速率只看最近这一段（秒）。为什么需要：速率 = 下降量之和 ÷ 跨度，而跨度是
//   "最旧到最新"的时间差。余额在 ±0.01 之间抖动时，每个抖动点都算一个下降台阶
//   （分子只加 1 分钱）却把跨度拉长，于是速率被稀释、底部那行"按当前速度，约 X 小时后
//   归零"会自己往上爬（所有者实测：纯抖动、零真实消耗，7.4 小时 -> 31.4 小时 -> 超过 7 天）。
//   只取最近这一段，既堵住稀释，也让文案里的"当前速度"名副其实。
//   0 = 关闭（恢复"所有点都进窗口"的旧口径）。必须 >= kRateMinSpanSeconds 才有意义。
inline constexpr int64_t kRateWindowSeconds = 1800;

// ★ 清零预估的封顶（分钟）= 7 天（设计 §7.4「超过 7 天」）。超过就只说
//   "超过 7 天"，不给一个没人信的精确值。
inline constexpr double kZeroTimeSevenDayMinutes = 7.0 * 24.0 * 60.0;

// ★ 什么时候补绝对时刻（分钟）= 6 小时（设计 §7.4「剩余 ≤ 6 小时时补一个
//   绝对时刻」）。超过 6 小时只给相对时长。
inline constexpr double kZeroTimeAbsoluteMinutes = 6.0 * 60.0;

// ★ 底部文案的重绘阈值（设计 §7.4「数值变化 < 10% 时不重绘」）。
//   0.10 = 变化不到 10% 就沿用上一帧的字，避免数字乱跳。
inline constexpr double kZeroTimeRedrawFraction = 0.10;

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

// ---- 氛围曲线（规格 §3 的显示层）----
// 颜色仍是边缘文字那个中性色，透明度仍是氛围档（规格 §3 没让改，保持原样）。
inline constexpr float kCurveColorR = 0xaf / 255.0f;
inline constexpr float kCurveColorG = 0xb2 / 255.0f;
inline constexpr float kCurveColorB = 0xb7 / 255.0f;
inline constexpr float kCurveAlpha = 0.35f;
inline constexpr float kCurveWidthDip = 8.0f;
// 曲线的上下界（实体区内坐标，0 = 面板顶）。曲线在这两条线之间铺满。
// 参考：数字墨迹占 48..81；标题在 8 上下；底部预估文案在 103 上下。
// 想更宽就把两个数字拉开（例如 40 / 90）；想更窄就收拢（例如 55 / 75）。
inline constexpr float kCurveBandTopDip = 30.0f;
inline constexpr float kCurveBandBottomDip = 100.0f;


// ---- 曲线的滚动动画（规格 §3；所有者给的公式）----
// 第 k 帧每点纵坐标：P = L + (N − L) × (1 − rate^k)^c
//   L = 用旧极值算出的实际纵坐标，N = 用新极值算出的实际纵坐标
// 横向同理：每点横坐标 = 基准横坐标 − 进度 × 一格，进度同样是帧号 k 的纯函数。
// 初始值与数字滚动一致（同一套手感），之后你可以分别调。
// ★ 必须是帧号 k 的纯函数（不要改成"每帧自乘的增量状态"）——否则导帧逐帧量不了。
// 刷新节奏：固定 10 秒一个点（所有者规格 §2.3：不做自适应；自适应那三个常量已删）
inline constexpr int kApiIntervalMs = 10000;

inline constexpr float kCurveRollRate = 0.975f;
inline constexpr float kCurveRollC = 10.0f;

// 滚动一格的时间与帧率（规格 §3：整体**10 秒内匀速**移动到下一格；帧号 k = 秒数 × 60）。
// 这两个不是外观旋钮，是规格写死的时钟：改它们等于改"10 秒一格"这条验收项。
inline constexpr double kCurveScrollSeconds = 10.0;
inline constexpr double kCurveFrameHz = 60.0;

// （所有者决定保留"极值铺满"：微小变化被放大是可以接受的，因此**不做**最小跨度保护。）

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
// 鼠标悬停在币种符号上时的不透明度（= 所有者说的"变深一点"：深色面板上变暗一档表示可按）。
inline constexpr float kCurrencyHoverAlpha = 0.72f;
inline constexpr float kEstimateAlpha = 1.00f;   // 边缘文字：同标题，渲染成 #afb2b7

// 调试浮层里用的几个颜色（正常运行时看不到）。
inline constexpr float kProbeRedAlpha = 0.50f;   // 预乘自检用的纯红方块
inline constexpr float kWarnR = 1.00f;           // 警告色（偏黄）
inline constexpr float kWarnG = 0.94f;
inline constexpr float kWarnB = 0.60f;
inline constexpr float kWarnAlpha = 0.95f;

// ---------------------------------------------------------------------------
// 5. 氛围：内蒙光 + 颜色管线（"E 蒙光.md" 第 1-10 章、"E 施工单.md" 甲）
// ---------------------------------------------------------------------------
//  这一段里每个数都是**外观**：所有者"看一眼觉得不对就改"的就是这里。
//  改完重跑 build.bat 就生效（不要只改渲染代码里的字面量）。

// ---- 5.1 三个颜色锚点（"E 重新设计.md" 第 9-11 行）----
// 氛围初色 C_0：R=0 是"充足"的蓝，R=0.5 是橙，R=1 是血色。两段线性插值。
// ★ 这是**氛围自己的**色标，与上面那个 kStatePrimary*（旧的"状态主色"）是两回事：
//   后者已不参与面板底色，只留给演示用。不要把两者合并。
inline constexpr float kAmbienceAnchor0R = 0x6c / 255.0f;   // R = 0.0  充足
inline constexpr float kAmbienceAnchor0G = 0x89 / 255.0f;
inline constexpr float kAmbienceAnchor0B = 0xf6 / 255.0f;
inline constexpr float kAmbienceAnchor1R = 0xf6 / 255.0f;   // R = 0.5  橙
inline constexpr float kAmbienceAnchor1G = 0xaa / 255.0f;
inline constexpr float kAmbienceAnchor1B = 0x6c / 255.0f;
inline constexpr float kAmbienceAnchor2R = 0xf6 / 255.0f;   // R = 1.0  血色
inline constexpr float kAmbienceAnchor2G = 0x6c / 255.0f;
inline constexpr float kAmbienceAnchor2B = 0x6c / 255.0f;
// R 的分段点：0 → 0.5 走第一段，0.5 → 1.0 走第二段。
inline constexpr float kAmbienceMidRatio = 0.5f;

// ---- 5.2 剧烈程度 R 的基线 ----
// 消耗基线（元/分钟）。估计不出来的时候就是它 —— 所有者定的 -0.25。
// ★ R 用的**不是** kRateSpringTauSeconds 那条平滑后的速率，而是"最近一步"的
//   每分钟步长：R 描述的是"刚刚有多陡"，平滑过的值恰恰把"刚刚"抹掉了
//   （"E 施工单.md"：两者都是每分钟量，每步余额差 ÷ 该步自己的间隔秒数 × 60）。
// ★ 分母是 2*|基线|，不是 |基线|（施工单的修正一）：除以 |基线| 时 R 能到 2，
//   会把心跳频率推过上限。
inline constexpr double kAmbienceBaselineYuanPerMinute = -0.25;

// ---- 5.3 死态程度 D 的低余额阈值 G ----
// 余额低于 G 元算"不足"。
// ★ 这是**本地常量**：DeepSeek 账号里没有这个设置，不要去接口里找（施工单写明）。
//   G <= 0 时 D 恒为 0（不做除法，"E 蒙光.md" §3.3 第 2 条）。
inline constexpr double kLowBalanceThresholdYuan = 10.0;

// ---- 5.4 内蒙光的剖面（alpha 形状，与亮度无关）----
// 面板内部贴边的一圈"内唇"：从边框内沿向心衰减到 0。
// 它碰不到任何元素（文字最短内缩 12 DIP > 5 DIP），所以它可以做到最亮。
inline constexpr float kGlowInLipDip = 5.0f;      // 衰减距离（DIP）
inline constexpr float kGlowInLipAlpha = 0.280f;  // 轮廓内沿处的 alpha（满强度）
// 底部透光：从面板底边往上的一条竖向渐变（下亮上暗），17 DIP 内衰减到 0。
inline constexpr float kGlowInVertDip = 17.0f;
inline constexpr float kGlowInVertAlpha = 0.150f;
// 整板底噪：面板内处处一层极淡的 alpha，作用是"整体被染了一点"，不提供亮度。
inline constexpr float kGlowInFloorAlpha = 0.060f;   // 整板底噪：这是"被照亮的表面"而不是"一条边"的关键

// ---- 5.5 内蒙光的强度倍率 k(R,D) —— 单独暴露，改它不用动剖面 ----
//   k(R,D) = (kGlowInK0 + kGlowInK1 * R) * (1 - kGlowInD * D)
//   R 越大越亮（最多 +67%），D 越大越暗（最多 -60%）。
// ★ 这是所有者唯一需要动的"亮度"旋钮：剖面（5.4）一个数都不用改。
//   它乘在**整条剖面**上，所以形状不随状态变化（形状变了 = 换了一种状态语言）。
inline constexpr float kGlowInK0 = 1.00f;   // R = 0 时的基准强度（所有者 2026-09-17：先加大看看）
inline constexpr float kGlowInK1 = 0.40f;   // R = 1 时额外加多少
inline constexpr float kGlowInD = 0.60f;    // D = 1 时暗掉的比例
// 读不到余额（按 D = 1 处理）时观感取"甲"：冷白光**仍在**，不是"褪尽"
// （一块死板子本身就是"出事了"的信号，v0.2 设计 §7.1 禁止）。
// 想要"连光一起褪尽"就把 kGlowInD 改成 0.88（D=1 时 k 从 0.36 降到 0.088），
// 剖面与几何一个数都不用动。
// D = 1 时 C 是饱和度为 0 的近白；纯中性灰在近黑底上容易读成"玻璃上的灰"，
// 所以朝基准蓝混一点点，让它读成"冷光"。
inline constexpr float kGlowInD1Warm = 0.12f;

// ---- 5.6 颜色与强度的缓动 ----
// 颜色逐通道缓动：外观旋钮，与数字滚动共用同一套"帧号 k 的纯函数"手感。
// ★ rate 是**每帧**系数（窗口垂直同步，约 60 帧/秒），与 kRollRate 同风格。
//   c > 1 起步更快（颜色比数字更早看得出来）。
inline constexpr float kAmbienceColorRate = 0.975f;
inline constexpr float kAmbienceColorC = 10.0f;
// 某个通道的残差小于它就吸附到目标（免得末位永远差一点点）。单位 0..1。
inline constexpr float kAmbienceColorSnap = 0.002f;
// 蒙光强度的缓动时间常数（秒）。与设计 §9.6「光晕透明度 1.5 s」同一个数。
// ★ 强度与颜色分开：颜色逐帧自乘（纯帧号函数），强度用连续解 exp(-dt/τ)。
inline constexpr float kGlowInTauSeconds = 1.5f;
// 缓动每步的 dt 上限（秒）。与 kRateSpringMaxDtSeconds 同一个值、同一个理由：
// 休眠唤醒后第一帧 dt 巨大，动画会一步跳到位。
inline constexpr double kAmbienceMaxDtSeconds = 0.05;

}  // namespace dshb
