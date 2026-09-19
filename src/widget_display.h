// 显示层：把"跳变的测量值"变成"连续变化的显示值"（C2/C3/C4）
//
// 设计里有一条规则必须保留：**测量值可以突变，显示值必须渐进跟随**。
// 充值会让余额瞬间从 3 元跳到 100 元；如果直接显示，数字会闪一下。
//
// ★ 两个容易做错的地方，这里都按实测定死了：
//   1. 时间常数与采样间隔**解耦**。采样是固定 10 秒，但滚动时长是 0.6 秒级别的
//      固定值——绑在一起会让数字永远滚不完就被下一个值打断。
//   2. 必须**吸附**。纯指数逼近永远到不了目标，于是数字会永远停在
//      "99.9997"这种地方，显示成 100.00 又和目标差一点，看起来像在抖。

#pragma once

#include "amount.h"           // Amount（AmountInYuan 的入参：主币种的一笔钱）
#include "heartbeat.h"        // 心跳位移（E 段）：纯函数，状态由本层保管
#include "rate_estimator.h"
#include "roll_axis.h"
#include "state_machine.h"
#include "tuning.h"

#include <cmath>     // std::fabs（rolling() 用）
#include <cstdint>
#include <string>
#include <vector>

namespace dshb {

// ===========================================================================
// 氛围：剧烈程度 R、死态程度 D、颜色管线（"E 重新设计.md"、"E 蒙光.md" §3）
// ===========================================================================
//  这一节全是**纯函数**：没有时钟、没有全局状态、没有文件。于是它们既能被
//  渲染层每帧调用，也能被探针离线逐值核对（这是本项目唯一的验证方式）。

// 一个 0..1 的 RGB 三元组（**直通**分量，不是预乘）。
struct AmbienceColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// ---- 剧烈程度 R ----
//   R = min{ max(每分钟步长, B*2), 0 } / (B*2)      B = kAmbienceBaselineYuanPerMinute
//  "每分钟步长"是**最近一步**的余额差 ÷ 该步自己的间隔秒数 × 60。
//  ★ 为什么是"最近一步"而不是那条平滑过的速率（§7.3 的 rate_display）：
//    R 要回答的是"刚刚有多陡"。弹簧平滑正是为了把单步噪声抹平，用它算 R
//    会把"刚刚发生的一次陡降"平均掉——而那恰恰是 R 唯一想表达的东西。
//  ★ 为什么分母是 B*2（施工单的修正一）：除以 B 时 R 能到 2，
//    会把心跳频率推过 F_max 上限。
//  结果夹在 [0,1]：余额回升（正步长）时 R = 0（回升不是"剧烈消耗"）。
//  stepPerMinute >= 0 或 B >= 0（基线不可信）时返回 0。
double SeverityRatio(double stepPerMinute);

// 把"两个相邻点的余额差 + 它们的间隔"折成每分钟步长（= R 的输入）。
// dtSeconds <= 0 或任一点不可用 -> 返回 0（"算不出来"，不是"没在消耗"）。
// ★ 只有**下降**才是消耗：回升（正差）原样返回正数，由 SeverityRatio 夹成 0 ——
//   这一层不做判断，免得两个地方各判一次、口径不一致。
double StepPerMinute(double previousYuan, double currentYuan, double dtSeconds);

// ---- 死态程度 D ----
//   D = min{ max(G - 余额, 0) / G , 1 }
//  ★ 修正（施工单的修正二）：他原来把**分子**夹成 1 元，于是 D 最大只有 0.1。
//    夹的应该是**结果**，不是分子。
//  G <= 0 -> 返回 0（不做除法，"E 蒙光.md" §3.3）。
//  余额为 0 或负 -> 饱和到 1。
double BalanceDepth(double balanceYuan);

// 一笔**主币种**的金额值多少元。
// ★★ 这是 D（危险度）与曲线颜色**共用的唯一换算口**：屏幕上显示哪个币种与它无关 ——
//    "这笔钱低不低"只跟钱有关，阈值统一是 kLowBalanceThresholdYuan = 10 元。
//      · "CNY"：恒等（1 元 = 1 元），**不看汇率**。大陆账号因此永远不需要汇率，
//        切到 USD 显示时 D / 颜色 / 心跳逐位不变（所有者 2026-09-19 的口径）。
//      · "USD"：× 启动时取到的那一次 USD->CNY 汇率（fx_rate.h 的 UsdAmountToYuan）。
//      · 其他币种 / USD 而没有汇率 / 文本读不出来 -> 返回 false，*outYuan 不动。
//        "不知道"绝不当成"安全"或"危险"中的哪一个 —— 由调用方各自决定（D 取 0，
//        理由写在 widget_display.cpp 的 AdvanceAmbience 里）。
bool AmountInYuan(const std::string& currency, const Amount& amount,
                  const std::string& usdToCnyText, bool usdToCnyOk, double* outYuan);

// ---- 颜色管线 ----
// 第一步：氛围初色 C_0 —— 两段 RGB 线性插值（不动饱和度）。
//   R <= 0.5: lerp(#6c89f6, #f6aa6c, R/0.5)
//   R >  0.5: lerp(#f6aa6c, #f66c6c, (R-0.5)/0.5)
// 这是"未降饱和"的那个颜色。**任何一帧的最终颜色都必须从它算起。**
AmbienceColor AmbienceBase(double ratio);

// 第二步（也是最后一步）：把 C_0 的**饱和度**乘 (1 - depth)，R=G=B 不变（色相与明度不动）。
// ★★ 饱和度必须**永远从本帧未降饱和的 C_0** 算：若从上一帧已经降过饱和的颜色再降一次，
//     D 会在每帧自我累积，几秒内整条曲线褪成灰色。
// depth >= 1 时得到的是 (v,v,v)，v = C_0 的明度；此时按 kGlowInD1Warm
//  朝基准蓝混一点点，免得纯中性灰在近黑底上读成"玻璃上的灰"（"E 蒙光.md" §5）。
AmbienceColor DesaturateTowards(const AmbienceColor& base, double depth);

// 合成：C = DesaturateTowards(AmbienceBase(ratio), depth)。
// ★ 这就是**最终要画的颜色本身**：调用方直接把它交给渲染层，之后不再有缓动。
//   平滑发生在 ratio 一侧 —— R(t) = kAmbienceDecayA^t 是连续衰减的（见 tuning.h 5.6）。
AmbienceColor AmbienceTargetColor(double ratio, double depth);

// 颜色管线的一句自检（不需要窗口、不需要渲染）：把 (ratio, depth) 算成颜色，
// 并断言两条不变式 —— (a) depth=0 时结果**恰好**等于 AmbienceBase(ratio)；
// (b) 反复调用同一个 (ratio, depth) 结果恒定（没有"自我累积"的可能）。
// 返回 true = 全部成立。细节写进 report（UTF-8，一行一条）。
bool AmbienceSelfTest(std::string* report);

// 余额数字的显示状态
//
// ★ 为什么是"速率 + 连续函数"而不是"定时长"（所有者纠正过一次，这里记清楚）：
//
//   设计里写的是"各状态的变化都用连续的方法去控制；变量可以突变，但显示变量
//   必须逐渐变化、跟着那个突变变量"。那就是**速率参数**驱动的连续函数。
//
//   我一度改成"固定时长 0.40 秒 + 缓动"，那是为了绕开指数逼近的尾巴而打的补丁，
//   副作用有两个：
//     1. 引入了设计里没有的东西（一次跳跃有自己的时长）——于是"0.4 秒是什么时长"
//        这个问题本身就说明它不对；
//     2. 它和采样间隔（10 秒）脱钩：数字 0.4 秒走完，然后 9.6 秒纹丝不动。
//        这和"绑在采样间隔上"是同一类毛病，只是换了个方向。
//
//   正确的形态：**目标值每 10 秒换一次**，所以"永远降不到 0"根本不发生——
//   显示值一直在追一个一直在动的目标。唯一的危害是"最后半格磨蹭"，
//   那由截断（吸附阈值）解决：差到看不见就直接贴上去。
//
//   于是只有一个参数 τ（速率），没有时长。
class DisplayedAmount {
public:
    // 速率：显示值每秒钟走掉剩余差距的 (1 - e^(-dt/τ))。
    //
    // ★ 默认 0.10 秒是**量出来的**，不是拍的（tools/tauprobe.cpp 可复现）：
    //     τ     截断(走完)   99% 到位
    //     0.30    2.90 s      1.37 s    太慢，对 10 秒采样明显滞后
    //     0.20    1.93 s      0.92 s    偏慢
    //     0.10    0.97 s      0.45 s    ← 正对初稿写的"1 秒恰好滚动完成"
    //     0.06    0.57 s      0.27 s    偏急
    //     0.04    0.38 s      0.18 s    像瞬跳，失去滚动感
    //   τ 仍远小于采样间隔（10 秒），所以每次采样后数字都早早到位、看起来始终新鲜。
    double tau = 0.10;

    // 截断阈值（元，= 半个最小显示单位）：差距小于这个就直接吸附。
    // 指数逼近的尾巴在数值上永远不为 0，但在视觉上早就没有移动了；
    // 这条截断把那截"看不见的尾巴"切掉，最后一位数字才能干脆落定。
    double snapYuan = 0.005;

    // ★ 每一位当前的纵坐标（连续）。渲染层按 place 取值来画数字。
    //   静止时每一位正好落在自己的数字上（整数坐标）；滚动中落在两格之间，
    //   于是显示位与它旁边那位数字一起画出来。
    const std::vector<axis::PlaceCoord>& places() const { return places_; }

    // ★ 手动设定三件参数并冻结。所有者定的参数就是这三个，没有"display"：
    //     R = 实际数字、L = 上次的实际数字、frames = 已经运算了多少帧 k。
    //   位置完全由它们决定：coord_n = floor(L/n) + D_n × (1 − rate^k)，K 就是 frames。
    void SetManual(double lastReal, double real, int frames) {
        hasValue_ = true;
        frozen_ = true;
        lastReal_ = lastReal;   // L
        target_ = real;         // R
        frames_ = frames;       // k
        animating_ = false;
        tripsDirty_ = true;
        SyncValueFromTrips();
        trips_.clear();
        places_.clear();
    }

    bool frozen() const { return frozen_; }
    void SetCrisp(bool on) { crisp_ = on; }
    // 强行指定相位 0..1（>=0 生效）：用来停在行程的任意一刻看效果。
    // 兼容旧工具：直接指定"已走完比例"（等价于给一个 rate^k）。
    void SetPhaseOverride(double p) { phaseOverride_ = p; }

    // 收到新样本。做一次确认，避免"瞬间 0"把界面闪成灰色。
    void OnSample(const Sample& s);

    // 每帧推进。返回当前应当显示的余额（元）。
    double Update(double dtSeconds);

    // ---- 消耗速率（设计 §7.3 的弹簧 / §7.4 的底部文案）----
    // ★ 速率在这里**每帧重算**：输入是 .cpp 里那份曲线存储的点，但**取的是每个点
    //   自己的第一个可用条目**（CurveValueOf 的口径，和曲线画的那条序列同源），
    //   不是"当前显示的币种"——"当前显示的币种"是另一个函数
    //   RateInputForCurrency 的口径，这几行说的不是它。
    //   每个点用它**自己的**时间戳（没有时间的点就是不显著，绝不拿全局 update_at 顶替）。
    //   结果是纯函数 EstimateRate 的输出，弹簧按帧的 dt 把它平滑成 rateDisplay，
    //   两者都留在成员里，BuildWidgetFrame 拿来算底部那行字。
    const RateEstimate& rateEstimate() const { return rateEstimate_; }
    double rateDisplay() const { return rateDisplay_; }

    // ---- 氛围（"E 蒙光.md" §3）：R / D / 最终颜色 ----
    // ★ R 不是纯函数：它是**时间**的纯函数 R(t) = kAmbienceDecayA^t（t 以分钟计，
    //   每帧 t += dt）。D 才是纯函数（BalanceDepth，余额的纯函数）。
    //   数据里算出来的那个值（= 刷新时 R 应当跳到的高度）由 ambienceRatioTarget() 给。
    // 本帧刷新的目标高度：clamp(min{max(s, 2B), 0} / 2B, 0, 1)，s = 最近一步的元/分钟。
    double ambienceRatioTarget() const { return ambienceRatioTarget_; }
    // 本帧的 D。★ 它的输入是**主币种余额折成元**（AmountInYuan），不是屏幕上那个币种的
    //   数字 —— 所以点击币种符号切换显示时它逐位不变（所有者 2026-09-19 的口径）；
    //   折不成元（海外账号 + 没有汇率）时它是 0（理由见 .cpp 的 AdvanceAmbience）。
    double ambienceDepth() const { return ambienceDepth_; }
    // 本帧**应当显示**的低余额程度（含"读不到余额按 D=1"这条状态规则）。
    double ambienceDepthShown() const { return depthShown_; }
    // 本帧**实际显示**的 R = R(t)。t 只由帧 dt 推进、由刷新抬升（见 .cpp 的模型说明）。
    // ★ 进程刚起、一步都还没量到时它是 **0**（不是 1）：t 只在第一次真的量出一步时开始走，
    //   见 ambienceSeconds_ 那一段的说明。
    double ambienceRatioShown() const { return ratioShown_; }
    // 本帧真正画上去的颜色（0..1 直通分量）。★ 它**就是**公式的直接输出，没有缓动。
    const AmbienceColor& ambienceColor() const { return ambienceColor_; }
    // 蒙光强度倍率 k(R,D)，范围 (0, 1.0]。
    float ambienceIntensity() const { return glowIntensity_; }
    // 本帧心跳位移（DIP，> 0 = 往下）。BeatOffsetFromBucket 的输出，本层不做任何平滑。
    double beatOffsetDip() const { return beatOffsetDip_; }
    // 心跳的仿真时刻（秒）。暂停冻结时**不推进**，于是位移自然停住。
    double beatSimSeconds() const { return beatSimSeconds_; }
    // 已经触发过几拍（诊断用）。第一帧就触发第一拍，所以正常从 1 起。
    std::size_t beatCount() const { return beatCount_; }
    // 导出夹具：把心跳的仿真时刻放到 k/60 秒，并按计时器重新走一遍（于是"第 k 帧的位移"
    // 可单独导出，与真跑 k 帧等价）。与 SetCurveScrollFrame 同一套惯例。
    void SetBeatSimFrame(int frame);
    // 余额读不到（= 按 D=1 且取"甲"）—— 供诊断与验收断言用。
    bool ambienceUnreadable() const { return ambientUnreadable_; }

    // ---- 关闭态的接口在文件末尾（自由函数）----

    // ★ 测试口子（导帧夹具）：**这一帧**用给定的 (R, D) 画，并跳过缓动与
    //   "读不到 -> D=1"那条状态规则。存在的理由只有一个：验收要求从真机导出
    //   (R,D) = (0,0)、(0.5,0)、(1,0)、(0,1) 四帧并量像素。R 由"最近一步 ÷ 基线"
    //   天然决定，靠夹具数据凑不出任意值；没有这个口子，"R=0.5 那一帧"就只能靠
    //   仿真数据碰巧落在 0.5，那是不可复现的证据。
    //   默认（没调用过）完全不生效。命令行接线见 widget_display.h 末尾的
    //   dshb::SetAmbienceGiven。
    void SetAmbienceGiven(double ratio, double depth);

    bool hasValue() const { return hasValue_; }
    // 连续失败到达阈值后调用：显示回到"没有值"（即 --.--）。
    // 下次成功样本会经 OnSample 自动恢复。
    void MarkUnreadable();

    // ---- 币种切换（点击币种符号）----
    // 按**名字**记住选中的币种，不记数组下标：接口不保证数组顺序（设计 §2.2）。
    void SelectCurrency(const std::string& code);
    const std::string& selectedCurrency() const { return selectedCurrency_; }
    // 当前**实际显示**的币种（选了哪个就显示哪个；没选则是接口的优先条目）
    const std::string& shownCurrency() const { return currencyShown_; }
    // 上一次切换币种实际换成的金额（-1 = 那次没有该币种）。给 main.cpp 记日志用。
    double lastSwitchTarget() const { return lastSwitchTarget_; }
    // 下一个币种（在当前清单里循环）。清单不足 2 个时返回空串。
    std::string NextCurrency() const;
    // 最近一次**已提交**采样的墙钟毫秒：曲线的横轴锚在它上面（右端 = 最近一次确认的值）。
    int64_t lastSampleWallMs() const { return lastSampleWallMs_; }

    double value() const { return value_; }
    double target() const { return target_; }

    // 是否正在追一个还没到位的目标（渲染层据此决定轮子要不要转）
    bool rolling() const { return hasValue_ && std::fabs(std::fabs(target_) - value_) > 0.0; }   // value_ 是幅值

    // 滚动期间应当显示哪一段文本。
    // ★ 数字文本只由**目标值**决定，不按每帧插值后的数值重算——那样每帧换一套
    //   数字，看起来就是一闪一闪（实测确认过）。竖直偏移（由当前值驱动）负责"动"，
    //   文本负责"内容"，两者不能同时变。
    std::string TextToShow() const;

private:
    bool hasValue_ = false;
    int64_t lastSampleWallMs_ = 0;   // 最近一次提交的采样时刻（曲线横轴锚点）
    double value_ = 0.0;      // 当前显示值：连续函数追着 target_ 走
    double target_ = 0.0;     // 目标值：每次采样直接改写（可以突变）
    bool zeroPending_ = false;
    bool zeroConfirmed_ = false;
    double latest_ = 0.0;
    std::string selectedCurrency_;                 // 空 = 用接口给的优先条目
    std::vector<std::string> availableCurrencies_;  // 最近一次样本里的币种清单
    std::string currencyShown_;                     // 当前显示的币种
    std::vector<CurrencyAmount> lastEntries_;       // 最近一次样本的条目（切换时要用金额）
    double lastSwitchTarget_ = -1.0;

    // ---- D 的输入：**本帧主币种余额折成元**（见 AmountInYuan 与 .cpp 的 AdvanceAmbience）----
    // ★ 它**不随显示币种变**：切币种改的只是屏幕上的数字与符号，同一笔钱只有一个 D。
    //   这一条就是所有者 2026-09-19 报的那个 bug 的反面：原来 D 吃的是 `value_`
    //   （显示币种的数），显示 USD 时 "$2.81" 被当成"¥2.81"去比 10 元阈值。
    // ★ 只有 OnSample 会写它（换算需要样本带来的汇率）；SelectCurrency **不许**碰它。
    // ★ primaryYuanOk_ = false 表示折不成元（海外账号 + 没有汇率，或主币种那一笔读不出来）：
    //   AdvanceAmbience 据此取 D = 0，理由写在那个函数里。
    double primaryYuan_ = 0.0;
    bool primaryYuanOk_ = false;

    // 这一段的起点值，用于自检报告"走了多少比例"
    double rollFromValue_ = 0.0;

    // ★ 每一位**自己管自己**：自己记着当前坐标、自己的目标、自己还剩多少要滚。
    //   全体共用一个 rate^k 是错的：中途来了新值就要重置那个共用状态，所有轮子被
    //   拽回起点——所有者看到的"突变"就是这么来的（他 rate=0.99 一轮约 7.6 秒，
    //   而序列每 3 秒换一次值，必然落在滚动中途）。
    //   分开之后：新值只改各自的 target，coord 从当前位置继续走，不会跳。
    struct Trip {
        int place = axis::kNoPlace;
        double from = 0.0;        // 起点坐标（格）
        double D = 0.0;           // 这一位要走多少格（带符号）
        double ratePower = 1.0;   // 这一位自己的 rate^k，每帧自乘一次
        double coord = 0.0;       // 当前坐标
    };
    std::vector<Trip> trips_;
    double lastReal_ = 0.0;      // L：上次变化时的实际数字
    bool tripsDirty_ = false;    // 有新样本/新手动值 -> 下一帧重建（并保留已有 coord）
    bool animating_ = false;     // 是否还有位在滚
    int frames_ = 0;             // k：手动模式下用来算 rate^k
    void SyncValueFromTrips();   // 显示值由最细那一位的坐标导出（保持一致）
    // 每一位当前的纵坐标（见 places()），渲染层按它画
    std::vector<axis::PlaceCoord> places_;
    double rollStartValue_ = 0.0;   // 本次行程的起点金额（用于算进度）
    double phaseOverride_ = -1.0;   // >=0 时强行指定相位（调参/排查用）
    double phaseNow_ = 0.0;         // 本帧实际用的相位（读数用）

    // 手动冻结：为真时不推进显示值
    bool frozen_ = false;
    // true = 坐标取 S/n 的整数部分（静止读数清晰）；false = 用连续坐标 S/n（有"两格之间"）
    bool crisp_ = false;

    // 按当前显示值刷新每位坐标：目标是 floor(显示值 / 10^位次)，本帧朝它追赶 dt 秒。
    // "有哪些位次"由文本决定——高位是 0 时文本里没有这一位，于是自动隐藏。
    void AdvancePlaces(double dtSeconds, const std::string& amountText);
    double UpdateValue(double dtSeconds);   // Update 的内核（只推进显示值）

    // 速率：每帧从曲线存储重算一次，再按帧的 dt 走一步弹簧（见头文件上面那一段）。
    void AdvanceRate(double dtSeconds);

    // 氛围：推进 R 的衰减时间、按状态规则决定 (R, D)、算出颜色与强度（见 .cpp 的模型说明）。
    void AdvanceAmbience(double dtSeconds);

    // 曲线存储里"最近那一步"有多陡（元/分钟）= `StoreStepPerMinute()` 的转发。
    // ★ 它量的是**存储里最新那个点 → 现在**（不看选中的币种，取每个点的第一个有值条目）。
    // ★ 它**只服务 R 的衰减**；曲线点的颜色走另一条路
    //   （`StepIntoNewestPointPerMinute()`，"进入这个点的那一步"）。
    //   两件事不能共用一个函数，理由写在 .cpp 那一段的实测里。
    double LastStepPerMinute() const;

    // 测试口子（SetAmbienceGiven 的实现体）：given_ 为真时 (R,D) 直接用给定值。
    bool ambienceGiven_ = false;
    double ambienceGivenRatio_ = 0.0;
    double ambienceGivenDepth_ = 0.0;

    // --- 氛围的逐帧状态 ---
    // ambienceRatioTarget_ 是"数据这一次给出的高度"（纯函数输出，只读）；
    // ambienceRatio_ 是本帧实际的 R = R(t)（由 ambienceSeconds_ 导出）。
    // ratioShown_ / depthShown_ 是"按状态规则该显示的"（读不到 -> R=0, D=1）。
    double ambienceRatioTarget_ = 0.0;
    double ambienceDepth_ = 0.0;
    double ambienceRatio_ = 0.0;
    double ratioShown_ = 0.0;
    double depthShown_ = 0.0;
    bool ambientUnreadable_ = false;
    AmbienceColor ambienceColor_{};     // 本帧的颜色 = 公式的直接输出
    float glowIntensity_ = kGlowInK0;   // k(R,D)
    bool glowSeeded_ = false;           // 强度有没有起点（没有时第一次直接落位）
    // R 的衰减时间（分钟）：R(t) = kAmbienceDecayA ^ ambienceSeconds_。
    // ★ 唯一的递推状态就是它，而且只加不减（刷新时是**抬高** t 让 R 跳上去）。
    //   它在"颜色链路"上扮演的角色，等于 kRollRate 那条自乘残差在数字滚动里的角色。
    // ★★ 它有一个"还没开始走"的状态（`ambienceClockValid_ == false`），而且**必须**有：
    //    t = 0 的含义是"刚刚发生了一次剧烈消耗"（R = a^0 = 1），而一个刚启动、还什么
    //    都没测到的进程没有资格这么说。少了这个状态，冷启动的头几帧就是 R = 1，
    //    第一个样本一到（`hasValue_` 变真、`unreadable` 那条"R = 0"的规则让位）
    //    屏幕直接血红 —— 所有者报的"每次点开都是红的"就是这么来的，实测见
    //    .dsh/scratch/bug2_probe.cpp（R_new = 0 而 R 一步跳到 0.9749）。
    //    计时器只在**第一次真的量出一步**（`raiseTo > 0`）时才开始走。
    double ambienceSeconds_ = 0.0;
    bool ambienceClockValid_ = false;

    // --- 心跳（模块本身无状态，状态全在这里）---
    //  模型（2026-09-18）：**一个计时器 + 一拍包络**。没有历史、没有相位、没有缓动。
    //   beatBucket_ 存的就是"这一拍的触发时刻 / 周期 / 幅度"这三个数；
    //   周期与幅度都在**触发那一刻**由当时的 (R, D) 采样，整拍不变。
    //   beatSeeded_ 只表示"第一拍已经触发过"，于是第一帧就跳一次（而不是等一个周期）。
    BeatBucket beatBucket_{};
    bool beatSeeded_ = false;
    double beatSimSeconds_ = 0.0;
    double beatOffsetDip_ = 0.0;
    std::size_t beatCount_ = 0;
    // 每帧推进心跳：推进仿真时间（冻结时不动）、必要时触发下一拍、再求这一拍的位移。
    void AdvanceBeat(double dtSeconds);
    // 触发一拍（把触发时刻记为 at，并按当时的 R/D 采样这一拍的周期与幅度）。
    // ★ 只写状态、不算位移：位移一律由 AdvanceBeat 在每帧末尾算一次（单一来源）。
    void TriggerBeatAt(double atSeconds);
    // 本帧该显示的 (R, D)：状态规则 + 两个夹具的覆盖，**只在这里**决定（见 .cpp 的说明）。
    void ApplyShownState();

    // ★ 归零预测的速率口径是 **Theil–Sen 中位斜率**（EstimateRateTheilSen）：窗口内
    //   所有点对斜率的中位数。旧的 EstimateRate（下降量之和 ÷ 跨度）**还在**，但只有
    //   曲线/氛围那条链用它——见 rate_estimator.h 的"哪个是哪个、为什么两个共存"。
    RateEstimate rateEstimate_;   // 最近一次 Theil–Sen 估计的结果（纯函数输出）
    double rateDisplay_ = 0.0;    // rate_display：被弹簧平滑过的速率，元/分钟
    bool rateSeeded_ = false;     // 弹簧有没有一个起点（没有时第一次直接落位）
};


// 曲线上的一个点：x、y 都归一化到实体区 [0,1]。
// ★ 2026-09-19：`kCapacity` 与"面板画几个点"**解耦**了。存储的容量在
//   curve_store.h（120），面板的**显示宽**是下面这个常量（11 个在看 + 1 个进场）——
//   BuildFrameCurve 在入口把存储切成"最新 12 个"，之后所有的槽位与极值都基于切片。
//   把它挪到头文件是为了让验收探针（tools/panelprobe.cpp）能对着**同一个数**断言，
//   而不是在探针里再抄一遍数字。
inline constexpr std::size_t kCurveDisplayPoints = 11;   // 可见点数；段数 = 它 - 1
// ★ 规格 §3 之后，这个点由**显示层**（本文件所在的这一层）算好最终位置再塞进帧里：
//   横向位置、纵向缓动都已经算完，渲染层只负责把点连成线（连法用单调三次，见 curve.h）。
//   渲染层不再做"逐帧逼近"——那正是规格 §4 要删掉的旧实现。
struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;   // 0 = 带子顶部，1 = 底部

    // ★ 这个点**自己那一刻**的氛围颜色（"E 蒙光.md" §3.1 末句：节点恰好就是那个
    //   时刻的氛围颜色，节点之间渐变过去）。由显示层从存储点里的颜色字段算出来，
    //   渲染层只负责在相邻两点之间插值。
    // ★ hasColor == false 表示"这个点没有颜色"——来源有两种，两种都不许编造：
    //     1) 老文件里的点（写颜色字段之前存的）：没有就是没有；
    //     2) 老点的**补位点**（把最左那段拉平的那个），它不对应任何存储点。
    //   渲染层见到 false 时用**当前 C** 画那一段（见 renderer.cpp 的注释）。
    float cr = 0.0f;
    float cg = 0.0f;
    float cb = 0.0f;
    bool hasColor = false;
};

struct WidgetFrame {
    ConnState state = ConnState::ColdStart;
    bool showAmount = false;        // 数字该不该显示（无数据时显示占位符）
    std::string amountText;         // 已格式化的两位小数
    const wchar_t* currencySymbol = L"";   // 空串 = 币种未知，**不默认 ¥**
    const wchar_t* statusText = L"";       // 标题行/状态文案
    const wchar_t* countdownText = L"";    // 右上角刷新倒计时（空串 = 不画）

    // 氛围曲线（D3）：空 = 没有数据（渲染层画平线）；1 个点 = 平线；
    // >=2 个点 = 用单调三次连成曲线（curve.h）。x、y 都归一化到实体区 [0,1]。
    std::vector<CurvePoint> curve;
    bool curveHasData = false;   // false 时一律按平线画
    std::string zeroTimeText;       // 清零预估（C9）：由估算速率与当前余额算出的那行字（UTF-8）
    // 每一位当前的纵坐标（渲染层按它画数字）。空 = 渲染层退回整串绘制。
    std::vector<axis::PlaceCoord> places;

    // ---- 氛围蒙光（"E 蒙光.md" §3、§6）----
    // 渲染层只认这两个数：颜色（已逐通道缓动）与强度倍率 k。它不知道 R、D、
    // 余额、基线、阈值 —— 那些是显示层的事（和"余额为 0 与查不到有什么区别"
    // 同一条分工）。剖面（内唇/底部/底噪的形状）属于渲染层，在 tuning.h 里。
    AmbienceColor ambientColor{};      // 直通分量 0..1
    float ambientIntensity = 0.0f;     // k(R,D)，0 = 不画蒙光

    // ---- 心跳位移（"E 心跳波形.md"）----
    //  ★ 这是"窗口真实位置"之外**唯一**会让画面动的东西：它只平移画布内部内容，
    //    窗口自身位置一帧都不改 —— 所以"移动窗口触发系统贴边吸附"结构上不可能发生。
    //  ★ 单位是 DIP；> 0 = 往下。渲染层乘上 scale 之后必须**同时**用在绘制变换与
    //    命中矩形上，否则点击会差这么多像素。
    float beatOffsetDip = 0.0f;
};

// 把状态 + 显示值组装成一帧。放在这里而不是渲染层，是为了让"显示什么"
// 和"怎么画"分开——渲染层不该知道余额为 0 和查不到有什么区别。
WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol);

// 状态文案行（C6）。标题兼状态行：紧急状态除了颜色变化，文字也要跟着换，
// 否则只靠颜色编码状态——色觉障碍、屏幕反光、截图转述三种情况下都会失效。
const wchar_t* StatusTextFor(ConnState state);

// 右上角的刷新倒计时：一个**纯数字**，每秒变一次，不做滚动动画。
// 用模块级的设置/读取，是为了让渲染层与导出路径都能拿到同一个值
// （导出路径不取样，所以它靠 --countdown=N 夹具提供）。
void SetCountdownText(const wchar_t* text);

// 合成一段历史（--history-demo=N）：导帧时没有真实历史，用它验证曲线画得对。
// ★ 规格 §2 之后它喂的是**曲线存储**（Append，只记变化那一条规则照旧生效），
//   不是旧的 SampleHistory；N=12 时正好得到"11 个点在看 + 第 12 个刚进来"的滚动起点。
// ★ 这里说的 12 是**显示宽**（kCurveDisplayPoints），它和存储容量（curve_store.h 的
//   kCapacity，2026-09-19 起是 120）从那一刻起就是两个数：喂 N > 12 也能用，
//   面板只会画出最新 12 个（BuildFrameCurve 入口切片）。
void PrimeHistoryForDemo(int points);

// 曲线记录文件路径（main 启动时给一次）。空 = 不落盘；给了就顺手加载一次。
void SetCurveStorePath(const std::wstring& path);
const std::string& CurveStorePath();
const wchar_t* CountdownText();

// 曲线存储里**最新那个点**的余额（元）。没有可用点（首次运行、存储为空/size<2 之外的
// 任何情况）时返回 false，`*outYuan` 不动。
// ★ 用途只有一个：启动过渡时给显示值一个"上次关掉前"的起点（main.cpp 的 CommitDelayed）。
//   存储在这里是权威来源 —— "上次关机前显示的余额"就是它最新那个点，不用另存一份状态。
bool CurveStartBalance(double* outYuan);

// ---- 曲线的滚动计时（规格 §3，导帧口子）----
// --curve-frame=k：把滚动计时器**冻结**在 k/60 秒，于是"滚动中的第 k 帧"可以用
// --export-frame=1 单独导出（帧状态是 k 的纯函数，不必连画 k 帧）。
// 必须在任何推进过计时器的循环**之后**调用；不带这个参数时导出路径照旧（一帧一个进程）。
void SetCurveScrollFrame(int frame);

// 一行诊断：存储里有几个点、滚动计时器停在哪一帧、进度多少。
// 这一层不写日志（约定：显示层只返回文本，写文件由 main 做），所以返回字符串给 main。
std::string CurveStateLine();

// ---- 命令行接线（--ambience=R,D / --no-text / --pause-ambience）----
// ★ 为什么是这几个自由函数而不是环境变量（所有者 2026-09-17 的要求）：
//   环境变量在日志里看不见，而本项目的全部验证都靠日志与导帧留痕。
//   命令行参数会出现在 SelfTestLog 的 `[argv]` 与自检输出里，事后可复查。
//
// --ambience=R,D：把这一帧的氛围钉在给定的 (R, D) 上（测试专用）。
//   R、D 都会被夹到 [0,1]。返回 false = 文本格式不对（main 应当记一条日志）。
//   默认（没调用过）完全不生效，生产路径不受影响。
bool SetAmbienceGiven(const char* text);

// ===========================================================================
// 关闭态（设计 §10.2）：右键进入 → 三次点击即关 → 只有 `Esc`（或点到面板之外）能取消
// ===========================================================================
//  ★ 状态机为什么在显示层而不在 main：R 的下限（R_d）必须与 R(t) 的抬升走**同一条**
//    代码路径 —— `AdvanceAmbience` 一处改动，光晕与心跳一起拿到（心跳按 R 取 §6 的两条律，
//    而 R 只有这一个来源）。main 只把输入翻译成 Enter/Click/Cancel 三个调用。
//  ★ 状态是**整个挂件只有一份**的（不是每个显示层实例一份），所以放模块级并按
//    AmbienceOverrideState / CountdownStorage 同一套写法暴露成自由函数。
//  ★ 四个硬性质（逐条有验收，全部由这里的纯函数决定）：
//      · 三次之间**没有时间窗**：第 2 击之后停 10 分钟，第 3 击照样关（一个时间戳都不存，
//        存了早晚有人顺手加一句"太久就清零"）；
//      · 进入关闭态之后**左键与右键都算一次**（所有者定）—— 所以 main 必须按
//        "当前是否关闭态"分派右键的两种含义（进入 / 计数），不能写成一个；
//      · 点满三次 = 发一次"该放粒子了"的信号，此后**不再计数、不再受理任何输入**；
//      · 取消（`Esc` 或点到别处）之后进度归零、文字恢复、R_d 归零（R 交回时间自己衰减）。
//  ★ 抖动与进入段是**帧号的纯函数**（下面两个自由函数）：生产路径每帧给一个帧号、
//    导帧路径把帧号钉在第 k 帧、探针能离线复算 —— 三条加起来才让"抖动"量得出来。
enum class ShutdownPhase { Off, Armed, Fired };

bool ShutdownActive();             // Off 之外都算（Fired = 粒子期间）
bool ShutdownFired();              // 已经点满三下：宿主据此去发粒子信号
int ShutdownClicks();              // 已计数的点击（0..3）
int ShutdownFrame();               // 进入以来第几帧（进入那一帧 = 0）
bool ShutdownEnter();              // 右键（非关闭态）→ 进入；已在关闭态返回 false
bool ShutdownClick();              // 关闭态内的一次点击（左/右键都算）；true = 这一下点满
// 托盘菜单的「关闭」：**直接进第三击**（所有者 2026-09-19）—— 需要时先进入（帧号与进入段
// 都从"进入那一刻"算起，与右键进入完全一样），然后一步把状态推到"已发粒子信号"。
// true = 宿主该去发粒子了，与 ShutdownClick 第三次的返回值同一个含义。
//  ★ 它不是"连调三次 ShutdownClick"的马甲：那会绕过次序约束（每一次点击都要写日志、抬 R_d、
//    推帧号），以后每加一条点击规则就得多改一处调用点。它就是"把 clicks 补齐到 3 再走第三次
//    点击那一步"，终点状态与在面板上点满三下**逐位相同**（clicks=3 + Fired）。
//  ★ 只有 Fired 时返回 false 且一位都不改（粒子期间不重复触发，与 ShutdownClick 同一道闸）；
//    已经在关闭态（Armed）时照样一次到 Fired —— 用户的意思是"我改主意了，现在就关"。
bool ShutdownFireNow();
bool ShutdownCancel();             // 取消；true = 刚才确实在关闭态里（日志要分得清）
double ShutdownFloorRatio();       // 本帧的 R 下限（0 / kShutdownRd1 / kShutdownRd2）
// 本帧内蒙光的**亮度倍率**（乘在 5.5 的 k(R,D) 上）：非关闭态 1.0，
// 第 1 击 kShutdownGlowLevel1、第 2 击及以后 kShutdownGlowLevel2。
//  ★ 关闭态的反馈不能只靠"变红"——余额已经红着时那就等于没有反馈；亮度与色相正交。
double ShutdownGlowLevel();
double ShutdownJitterDip();        // 本帧的抖动（DIP，与心跳相加进 beatOffsetDip）
bool ShutdownCurveLayerVisible();  // 进入段里曲线还画（它在收缩），之后不画
// 每帧由 DisplayedAmount::Update 调一次（宿主不要调）：推进帧号。导帧夹具下不推进。
void ShutdownAdvanceFrame();

// 入口进度 0..1：`kShutdownEntryFrames` 帧之后恒为 1（"约 0.12 s"在这里是帧数）。
double ShutdownEntryProgress(int frame);
// 第 k 帧、已计数到第 `clicks` 击时的抖动位移（DIP，> 0 = 往下）。
//  ★ 必须是**帧号的纯函数**（不能是逐帧累加的状态），否则"第 k 帧的抖动"没法单独量。
double ShutdownJitterAt(int frame, int clicks);

// 导帧夹具（`--shutdown-frame=k --shutdown-clicks=N`）：把关闭态**直接钉在**
// "已点 N 下、进入以来第 k 帧"。走的是显示层自己那套状态，而不是模拟点击 ——
// 模拟点击会在第三次时真的发粒子信号（那是生产行为，不是夹具该做的事）。
// 与 --curve-frame / --beat-frame 同一套惯例：帧状态是纯函数，一帧一个进程。
void SetShutdownFixture(int clicks, int frame);

// 夹具的存放处（定义在 widget_display.cpp 的匿名 namespace 里，通过函数取用）。
// 放在头文件是为了让两个自由函数与 AdvanceAmbience 用同一份状态。
struct AmbienceOverrideState {
    bool given = false;
    double ratio = 0.0;
    double depth = 0.0;
};

// --no-text：正文（标题/数字/预估）一层都不画，其余（面板、边框、蒙光、曲线）照旧。
//   存在的理由：要量"白字对它自己那层底色的对比度"，必须先知道底色是多少，
//   而整帧里文字墨迹正好盖在要量的那些像素上（第一次量就量到了字形本身，
//   得到 1.00:1 —— 那是"白字对白字"，不是对比度）。
void SetTextEnabled(bool on);
bool TextEnabled();

// --pause-ambience：暂停期间氛围**冻结**（颜色与光强一步都不推进）。
//   与显式传入的 --pause-ambience=0/1 配对，用来证明"暂停中连导两帧逐位相同、
//   恢复后再导一帧不同"。
void SetAmbienceFrozen(bool on);
bool AmbienceFrozen();

// --beat-trace：每一拍触发时往 stderr 打一行（触发时刻 / 这一拍的周期与幅度）。
//   计时器是有状态的，而一帧的位移看不出"这是第几拍"；要留痕就得在触发处打。
void SetBeatTrace(bool on);

// --ambience-glide=N：跑 N 帧**真实的**氛围推进（每帧 1/60 秒），其间 R 走一条
//   真实会发生的轨迹：0 -> 1（余额突然掉一截，剧烈程度拉满）-> 再回落到 0。
//   D 固定为 0。
//  ★ 为什么需要它：光强是烘进彩色层的，所以 k 每跨过 1/255 就可能重烘一整张
//    475x289 的位图。要量"一次颜色滑行到底重烘几次、最坏单帧多贵"，就必须真的把
//    那一串帧跑出来 —— 单帧导出看不出这件事。
//  ★ 它只喂氛围，不动余额：所以与 --fixed-amount 并存时也不会改变画面上的数字。
//  ★ 需要先把 main 那个显示层实例挂进来（见下面的 g_ambienceGlideTarget）。
void RunAmbienceGlide(int frames);

// 滑行要推的那个显示层实例。main 在拿到自己的 g_display 之后立即赋值一次：
//     dshb::g_ambienceGlideTarget = &g_display;
// 为什么不是引用来引用去：显示层实例属于 main（它在那里是全局的），这一层不该
// 反过来持有它；一个显式的测试用指针最省事，也最容易被看见和删掉。
extern DisplayedAmount* g_ambienceGlideTarget;

}  // namespace dshb
