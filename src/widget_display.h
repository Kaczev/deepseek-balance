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

// ---- 颜色管线 ----
// 第一步：氛围初色 C_0 —— 两段 RGB 线性插值（不动饱和度）。
//   R <= 0.5: lerp(#6c89f6, #f6aa6c, R/0.5)
//   R >  0.5: lerp(#f6aa6c, #f66c6c, (R-0.5)/0.5)
// 这是"未降饱和"的那个颜色。**任何一帧的最终颜色都必须从它算起。**
AmbienceColor AmbienceBase(double ratio);

// 第二步 + 第三步：把 C_0 的**饱和度**乘 (1 - depth)，R=G=B 不变（色相与明度不动）。
// ★★ 这是本模块最容易做错的一处：饱和度必须**永远从本帧未降饱和的 C_0** 算。
//     若从上一帧已经降过饱和的颜色再降一次，D 会在每帧自我累积，
//     几秒内整条曲线褪成灰色（施工单特别点名）。
//  depth >= 1 时得到的是 (v,v,v)，v = C_0 的明度；此时按 kGlowInD1Warm
//  朝基准蓝混一点点，免得纯中性灰在近黑底上读成"玻璃上的灰"（"E 蒙光.md" §5）。
AmbienceColor DesaturateTowards(const AmbienceColor& base, double depth);

// 两步合起来：目标颜色 = DesaturateTowards(AmbienceBase(ratio), depth)。
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
    // ★ R 与 D 是**纯函数**（SeverityRatio / BalanceDepth）的逐帧求值结果：
    //   输入是曲线存储里"当前显示币种"的最近两个点、以及当前显示的余额。
    //   它们每帧重算，不缓存、不递推 —— 于是不存在"脏了忘了刷"的状态。
    double ambienceRatio() const { return ambienceRatio_; }
    double ambienceDepth() const { return ambienceDepth_; }
    // 本帧**应当显示**的低余额程度（含"读不到余额按 D=1"这条状态规则）。
    double ambienceDepthShown() const { return depthShown_; }
    // 本帧**应当显示**的 R（读不到余额时为 0，"E 蒙光.md" §3.3）。
    double ambienceRatioShown() const { return ratioShown_; }
    // 逐通道缓动之后、真正画上去的颜色（0..1 直通分量）。
    const AmbienceColor& ambienceColor() const { return ambienceColor_; }
    // 本帧的颜色目标（未缓动）。导出/探针要对账时用它。
    const AmbienceColor& ambienceTargetColor() const { return ambienceTarget_; }
    // 蒙光强度倍率 k(R,D)，范围 (0, 1.0]。
    float ambienceIntensity() const { return glowIntensity_; }
    // 本帧心跳位移（DIP，> 0 = 往下）。纯函数 BeatOffsetPx 的输出，本层不做任何平滑。
    double beatOffsetDip() const { return beatOffsetDip_; }
    // 心跳的仿真时刻（秒）。暂停冻结时**不推进**，于是位移自然停住。
    double beatSimSeconds() const { return beatSimSeconds_; }
    // 已经记下几次"余额变化"（诊断用：正常只会用到最后一条）。
    std::size_t beatChangeCount() const { return beatHistory_.size(); }
    // 导出夹具：把心跳的仿真时刻直接放到 k/60 秒，于是"第 k 帧的位移"可单独导出。
    //  与 SetCurveScrollFrame 同一套惯例（帧状态是 k 的纯函数，不必连跑 k 帧）。
    void SetBeatSimFrame(int frame);
    // 余额读不到（= 按 D=1 且取"甲"）—— 供诊断与验收断言用。
    bool ambienceUnreadable() const { return ambientUnreadable_; }

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

    // 氛围：每帧重算 (R, D) 与目标颜色，再逐通道缓动一步（见下面的 ★ 段）。
    void AdvanceAmbience(double dtSeconds);

    // 曲线存储里"当前显示币种"的最近一步（元/分钟）。没有可用的两步时返回 0。
    double LastStepPerMinute() const;

    // 测试口子（SetAmbienceGiven 的实现体）：given_ 为真时 (R,D) 直接用给定值。
    bool ambienceGiven_ = false;
    double ambienceGivenRatio_ = 0.0;
    double ambienceGivenDepth_ = 0.0;

    // --- 氛围的逐帧状态 ---
    // ambienceRatio_ / ambienceDepth_ 是"从数据算出来的"（纯函数输出，只读）；
    // ratioShown_ / depthShown_ 是"按状态规则该显示的"（读不到 -> R=0, D=1）。
    double ambienceRatio_ = 0.0;
    double ambienceDepth_ = 0.0;
    double ratioShown_ = 0.0;
    double depthShown_ = 0.0;
    bool ambientUnreadable_ = false;
    AmbienceColor ambienceTarget_{};    // 本帧目标（未缓动）
    AmbienceColor ambienceColor_{};     // 本帧实际画上去的
    bool ambienceSeeded_ = false;       // 有没有起点（没有时第一次直接落位到目标）
    float glowIntensity_ = kGlowInK0;   // k(R,D)
    bool glowSeeded_ = false;           // 强度有没有起点（同上）
    // 每个通道一条"每帧自乘的残差"：与自己上一帧的值相乘 = rate^k。
    // ★ 与数字滚动同一套手感（kRollRate / kRollCurveC），但它只属于颜色。
    float ambDcR_ = 0.0f;               // 逐通道颜色残差（**故意不叫 D**：
    float ambDcG_ = 0.0f;               // 全局的 D 是死态程度，两者绝不能混）
    float ambDcB_ = 0.0f;
    int ambienceFrames_ = 0;            // k：颜色缓动的帧号（幂的指数）

    // --- 心跳的逐帧状态（模块本身无状态，状态全在这里）---
    //  simSeconds：单调仿真时间（暂停时不推进）；history：每次余额变化追加一条。
    std::vector<BeatChange> beatHistory_;
    double beatSimSeconds_ = 0.0;
    double beatOffsetDip_ = 0.0;
    bool beatSeeded_ = false;
    double beatLastRatio_ = 0.0;    // 上次追加时的 R/D，用来判断"又变化了一次"
    double beatLastDepth_ = 0.0;
    // 每帧推进心跳：推进仿真时间（冻结时不动）、必要时追加一条变化、再求位移。
    void AdvanceBeat(double dtSeconds);

    RateEstimate rateEstimate_;   // 最近一次 EstimateRate 的结果（纯函数输出）
    double rateDisplay_ = 0.0;    // rate_display：被弹簧平滑过的速率，元/分钟
    bool rateSeeded_ = false;     // 弹簧有没有一个起点（没有时第一次直接落位）
};


// 曲线上的一个点：x、y 都归一化到实体区 [0,1]。
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
void PrimeHistoryForDemo(int points);

// 曲线记录文件路径（main 启动时给一次）。空 = 不落盘；给了就顺手加载一次。
void SetCurveStorePath(const std::wstring& path);
const std::string& CurveStorePath();
const wchar_t* CountdownText();

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
