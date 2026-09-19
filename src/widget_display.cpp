#include "widget_display.h"

#include "amount.h"        // Amount / ParseAmount：纵坐标从十进制原文解析，不用二进制浮点
#include "curve_store.h"

#include <windows.h>   // WideCharToMultiByte（把宽路径转成数据层要的 UTF-8）   // CurveStore：12 点环形、只记变化、curve.json（规格 §2）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace dshb {

// ===========================================================================
// 氛围的纯函数层（"E 重新设计.md"、"E 蒙光.md" §3、"E 施工单.md" 甲.2/3）
// ===========================================================================
//  这一节没有全局状态、没有时钟、没有文件 —— 所以它既能在每帧被调用，也能被
//  探针离线逐值核对。所有夹取（clamp）都写在这里，调用方一律不再夹一次。

double StepPerMinute(double previousYuan, double currentYuan, double dtSeconds) {
    if (!(dtSeconds > 0.0)) return 0.0;          // 没有间隔 = 算不出来（不是 0 消耗）
    return (currentYuan - previousYuan) / dtSeconds * 60.0;
}

double SeverityRatio(double stepPerMinute) {
    const double b = kAmbienceBaselineYuanPerMinute;
    // ★ 基线必须为负才有意义（"基线是消耗速率"）。基线不合法时 R 恒为 0，
    //   而不是产生一个随手的方向 —— 那会让心跳频率凭空跳起来。
    if (!(b < 0.0)) return 0.0;
    const double twice = b * 2.0;                // 施工单修正一：分母是 2*|基线|
    // min{ max(步长, 2B), 0 } / (2B)：步长 >= 0（余额没降）-> 0；
    // 步长 <= 2B（比两倍基线还陡）-> 1。
    const double capped = (stepPerMinute < twice) ? twice : stepPerMinute;
    const double limited = (capped > 0.0) ? 0.0 : capped;
    const double r = limited / twice;
    return (r < 0.0) ? 0.0 : ((r > 1.0) ? 1.0 : r);
}

double BalanceDepth(double balanceYuan) {
    const double g = kLowBalanceThresholdYuan;
    if (!(g > 0.0)) return 0.0;                  // G 未设 -> D = 0，不做除法
    const double over = g - balanceYuan;         // 超出 G 多少
    // ★ 施工单修正二：夹的是**结果**，不是分子。原来把分子夹成 1 元，
    //   于是 D 最大只有 1/G = 0.1，整条"枯竭"曲线被压扁。
    const double d = (over < 0.0) ? 0.0 : (over / g);
    return (d > 1.0) ? 1.0 : d;                  // 余额为 0 或负 -> 饱和到 1
}

AmbienceColor AmbienceBase(double ratio) {
    const double r = (ratio < 0.0) ? 0.0 : ((ratio > 1.0) ? 1.0 : ratio);
    const double mid = kAmbienceMidRatio;
    AmbienceColor out;
    if (r <= mid) {
        const double t = (mid > 0.0) ? (r / mid) : 0.0;
        out.r = static_cast<float>(kAmbienceAnchor0R + (kAmbienceAnchor1R - kAmbienceAnchor0R) * t);
        out.g = static_cast<float>(kAmbienceAnchor0G + (kAmbienceAnchor1G - kAmbienceAnchor0G) * t);
        out.b = static_cast<float>(kAmbienceAnchor0B + (kAmbienceAnchor1B - kAmbienceAnchor0B) * t);
    } else {
        const double t = (mid < 1.0) ? ((r - mid) / (1.0 - mid)) : 1.0;
        out.r = static_cast<float>(kAmbienceAnchor1R + (kAmbienceAnchor2R - kAmbienceAnchor1R) * t);
        out.g = static_cast<float>(kAmbienceAnchor1G + (kAmbienceAnchor2G - kAmbienceAnchor1G) * t);
        out.b = static_cast<float>(kAmbienceAnchor1B + (kAmbienceAnchor2B - kAmbienceAnchor1B) * t);
    }
    return out;
}

AmbienceColor DesaturateTowards(const AmbienceColor& base, double depth) {
    const double d = (depth < 0.0) ? 0.0 : ((depth > 1.0) ? 1.0 : depth);
    // 饱和度 x (1-D)：色相与明度（v = max 分量）不动，所以先把 v 记住。
    const float v = (base.r > base.g) ? ((base.r > base.b) ? base.r : base.b)
                                      : ((base.g > base.b) ? base.g : base.b);
    const double s = 1.0 - d;
    AmbienceColor out;
    out.r = static_cast<float>(v + (base.r - v) * s);
    out.g = static_cast<float>(v + (base.g - v) * s);
    out.b = static_cast<float>(v + (base.b - v) * s);
    if (d >= 1.0 && kGlowInD1Warm > 0.0f) {
        // 完全退饱和 = 一根中性灰。近黑底上的中性浅灰容易读成"玻璃上的灰"，
        // 所以朝基准蓝混一点点，让它读成"冷光"（"E 蒙光.md" §5）。
        const float w = kGlowInD1Warm;
        out.r += (kAmbienceAnchor0R - out.r) * w;
        out.g += (kAmbienceAnchor0G - out.g) * w;
        out.b += (kAmbienceAnchor0B - out.b) * w;
    }
    return out;
}

AmbienceColor AmbienceTargetColor(double ratio, double depth) {
    return DesaturateTowards(AmbienceBase(ratio), depth);
}

bool AmbienceSelfTest(std::string* report) {
    std::string text;
    bool ok = true;
    auto line = [&](const char* fmt, double a, double b) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), fmt, a, b);
        text += buf;
        text += '\n';
    };

    // (a) depth = 0 时，结果必须**恰好**等于未降饱和的 C_0（不是"接近"）。
    //     这一条正是"绝不从上一帧已降饱和的颜色算"的可执行形式。
    const double ratios[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    for (const double r : ratios) {
        const AmbienceColor base = AmbienceBase(r);
        const AmbienceColor got = AmbienceTargetColor(r, 0.0);
        const bool same = (got.r == base.r && got.g == base.g && got.b == base.b);
        line("  [%s] depth=0 时 C == C_0 恰好相等 (R=%.2f)", same ? 1.0 : 0.0, r);
        if (!same) ok = false;
    }

    // (b) 幂等：同一个 (R,D) 反复求值结果恒定 —— 于是"自我累积"在结构上不可能。
    for (const double r : ratios) {
        const AmbienceColor first = AmbienceTargetColor(r, 0.6);
        bool stable = true;
        for (int i = 0; i < 100; ++i) {
            const AmbienceColor again = AmbienceTargetColor(r, 0.6);
            if (again.r != first.r || again.g != first.g || again.b != first.b) stable = false;
        }
        line("  [%s] 同一 (R,D) 求值 101 次恒定 (R=%.2f)", stable ? 1.0 : 0.0, r);
        if (!stable) ok = false;
    }

    // (c) 两条修正的边界值：分母是 2B（所以"恰好两倍基线"= R=1），
    //     以及 D 能真的到 1（施工单修正二）。
    const double rAtTwoB = SeverityRatio(kAmbienceBaselineYuanPerMinute * 2.0);
    const double rAtB = SeverityRatio(kAmbienceBaselineYuanPerMinute);
    const double rRise = SeverityRatio(0.5);
    const double dZeroBal = BalanceDepth(0.0);
    line("  [%s] R(2B)=%.4f 必须 = 1.0", (std::fabs(rAtTwoB - 1.0) < 1e-9) ? 1.0 : 0.0, rAtTwoB);
    line("  [%s] R(B)=%.4f 必须 = 0.5（分母是 2B，不是 B）", (std::fabs(rAtB - 0.5) < 1e-9) ? 1.0 : 0.0, rAtB);
    line("  [%s] R(回升 +0.5 元/分)=%.4f 必须 = 0", (rRise == 0.0) ? 1.0 : 0.0, rRise);
    line("  [%s] D(余额 0)=%.4f 必须 = 1.0（分子不夹）", (std::fabs(dZeroBal - 1.0) < 1e-9) ? 1.0 : 0.0, dZeroBal);
    if (std::fabs(rAtTwoB - 1.0) >= 1e-9 || std::fabs(rAtB - 0.5) >= 1e-9 || rRise != 0.0 ||
        std::fabs(dZeroBal - 1.0) >= 1e-9) {
        ok = false;
    }

    // (d) 一条**故意失败**的检查：确认这套自检真的会报错（探针纪律）。
    {
        const bool deliberatelyWrong = (std::fabs(SeverityRatio(0.0) - 1.0) < 1e-9);
        line("  [%s] 故意失败的检查（R(0) 必须 != 1.0）—— 这一行应当是 0",
             deliberatelyWrong ? 1.0 : 0.0, 0.0);
        if (deliberatelyWrong) ok = false;
    }

    if (report) *report = text;
    return ok;
}

// ===========================================================================
// 氛围曲线的显示状态（规格 §3）
// ===========================================================================
// 数据层（curve_store.h）已经定死了"记什么"：只有值变了才追加一个点、容量 12、
// 超过 86400 秒作废。这一层只管**画**：取最新 11 个点铺满恰好 10 段；新点进来时
// 整条在 10 秒内匀速左移一格；纵坐标按 P = L + (N − L)(1 − rate^k)^c 缓动。
//
// ★ 全部状态 = 存储内容 + 滚动计时器（"旧极值"本身也是存储内容的纯函数）。
//   没有任何"每帧自乘的增量状态"，所以第 k 帧可以单独构造、单独导出、单独量。
namespace {

CurveStore g_curveStore;

// ---------------------------------------------------------------------------
// 余额 + 剧烈程度 -> 氛围颜色（"E 蒙光.md" §3）：两个输入都用**那一刻**的值
// ---------------------------------------------------------------------------
//  ★ 存进曲线点的颜色 = V(R_new, D)，其中：
//      D     = 那个点的**主币种余额**算出来的死态程度（纯函数，可精确复原）；
//      R_new = **追加这个点的那一刻新算出来的剧烈程度**（所有者 2026-09-18 的口径：
//              "R_new。只不过如果它没有原本的 R 大，就会被盖掉而已"）。
//    ★ 注意它**不是** R(t)：R(t) 是每帧衰减的实时量（内蒙光用那个），曲线点存的是
//      "那一刻有多陡"这个事件量。两者在刷新那一刻数值相同，之后 R(t) 会自己凉下去。
//  ★ 为什么调用方要把 R_new 传进来、而不是在这里自己算：它得读曲线存储（"这个点与
//    上一个点"），而这里只拿得到余额。调用方（FeedCurve）拿得到存储，所以由它算。
//  ★ `balanceYuan` 是**主币种那一笔**的数，不是屏幕上那个币种的数字：
//    同一个函数喂屏幕上的数进去，就是"$2.81 当成 ¥2.81"那个 bug（见 AdvanceAmbience 第 3 步）。
AmbienceColor BalanceColorAt(double balanceYuan, double ratio) {
    const double depth = BalanceDepth((balanceYuan < 0.0) ? 0.0 : balanceYuan);
    return AmbienceTargetColor(ratio, depth);
}


std::string HexOf(const AmbienceColor& c) {
    auto byte = [](float x) {
        const float v = (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
        return static_cast<int>(v * 255.0f + 0.5f);
    };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

// 把一个存储点带的 "#rrggbb" 解析回分量。形状只有这一种（curve_store 的
// IsHexColor 保证），这里仍然按"解析失败 = 没有颜色"处理，绝不猜。
bool ParseHexColor(const std::string& hex, AmbienceColor* out) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    int v[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        int byte = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = hex[1 + i * 2 + k];
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit < 0) return false;
            byte = byte * 16 + digit;
        }
        v[i] = byte;
    }
    out->r = static_cast<float>(v[0] / 255.0);
    out->g = static_cast<float>(v[1] / 255.0);
    out->b = static_cast<float>(v[2] / 255.0);
    return true;
}

// 滚动计时器（秒）：追加一个点就归零，之后由每帧的 dt 推进（DisplayedAmount::Update）。
double g_curveSeconds = 0.0;

// 是否有一个还没走完的滚动。走到 kCurveScrollSeconds **之后**（严格大于）才退休：
// 于是 elapsed == 10.000 s 那一帧仍是滚动的终点帧、10.001 s 那一帧是"没有滚动"的
// 静止帧——验收项 2 正是拿这两帧比像素（两条不同的代码路径必须给出同一张图）。
bool g_curveScrolling = false;

// --curve-frame=k：把计时器冻结在 k/60 秒（导帧口子，只影响导出路径的一帧）。
bool g_curveFrozen = false;

// --beat-trace：每一拍触发时往 stderr 打一行（触发时刻 / 这一拍的周期与幅度）。
//  ★ 存在的理由：计时器是**有状态**的，而导帧只看得到最后一帧的位移。要判断"这一帧的
//    位移是第几拍、那一拍是什么时候触发的"，必须有触发的留痕。默认关，生产路径零影响。
bool g_beatTrace = false;

// L 用的"旧极值"（规格 §3 第 6 条）：追加发生**之前**屏幕上那 11 个点的极值。
// ★ 它其实也能从存储推出来（追加后环里最老的那些点就是刚才在看的点），但规格明确
//   要求"记住上一次的极值"；记住之后，滚动当中再来一次追加也不会把 L 算错。
double g_curveOldLo = 0.0;
double g_curveOldHi = 0.0;
bool g_curveOldValid = false;

// §3：显示最新 11 个点、恰好 10 段。
// ★ kCurveDisplayPoints 现在在头文件里（widget_display.h）：验收探针要对着同一个数
//   断言"面板画几个点"。这里只剩"段数 = 点数 - 1"这一条内部关系。
constexpr std::size_t kCurveSegments = kCurveDisplayPoints - 1;

// 一个点对曲线的取值 = 这个点的**第一个可用条目**。
// ★ 为什么不是"当前显示的币种"：数据层判定"变没变"用的就是响应第一个条目
//   （primary）——一个点之所以存在，正是因为那个币种变了。让它与点一一对应，
//   曲线画的就是"数据层记下的那条序列"，与屏幕上此刻显示的是哪个币种无关。
//   条目为 null（该次响应没有这个币种）时跳过，往后找第一个有值的。
bool CurveValueOf(const CurveStorePoint& point, double* out) {
    for (const CurveStorePoint::Entry& entry : point.entries) {
        if (entry.missing || entry.text.empty()) continue;
        Amount amount;
        if (!ParseAmount(entry.text, &amount)) continue;
        *out = amount.ToDouble();
        return true;
    }
    return false;
}

// 全部点的取值（oldest -> newest）。某个点读不出值时用相邻点的值补上，横向几何
// （一格一个点）才不会塌。数据层保证追加进来的点都有可用的第一个条目，所以这只有
// 在手工编辑过 curve.json 时才会发生。
bool CurveValues(const std::vector<CurveStorePoint>& points, std::vector<double>* out) {
    out->assign(points.size(), 0.0);
    std::vector<char> have(points.size(), 0);
    bool any = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        double v = 0.0;
        if (CurveValueOf(points[i], &v)) {
            (*out)[i] = v;
            have[i] = 1;
            any = true;
        }
    }
    if (!any) return false;
    double last = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {          // 向后填
        if (have[i]) last = (*out)[i];
        else (*out)[i] = last;
    }
    for (std::size_t i = points.size(); i-- > 0;) {            // 再向前填（头部空洞）
        if (have[i]) last = (*out)[i];
        else (*out)[i] = last;
    }
    return true;
}

// 窗口内极值（规格 §3：刻度按极值铺满整条带子，不做最小跨度保护）。
struct CurveSpan {
    double lo = 0.0;
    double hi = 0.0;
    bool degenerate() const { return !(hi > lo); }
};

CurveSpan SpanOf(const std::vector<double>& values, std::size_t begin, std::size_t end) {
    CurveSpan span;
    span.lo = values[begin];
    span.hi = values[begin];
    for (std::size_t i = begin; i < end; ++i) {
        if (values[i] < span.lo) span.lo = values[i];
        if (values[i] > span.hi) span.hi = values[i];
    }
    return span;
}

// 归一化纵坐标：0 = 带子顶、1 = 带子底。极值相同（或只有一个点）-> 带子正中。
double NormY(double v, const CurveSpan& span) {
    if (span.degenerate()) return 0.5;
    return 1.0 - (v - span.lo) / (span.hi - span.lo);
}

// 槽位 -> 归一化横坐标。★ 两条路径（滚动 / 静止）必须用同一个式子，同一槽位要给出
// 逐位相同的 float，否则"滚动终点帧 == 静止帧"的逐像素比较会败在最后一位的舍入上。
float SlotX(double slot) { return static_cast<float>(slot * 0.1); }

// 一个存储点的**第一个可用条目**（与 CurveValueOf 同一口径，但返回 Amount）。
// 为什么与曲线取值用同一个口径：R 是"这条曲线刚刚有多陡"，
// 曲线画的是哪条序列，R 就必须量哪条序列 —— 否则屏幕上那条线和那个颜色
// 讲的是两件事。条目为 null / 解析失败时返回 false（不编造数值）。
bool CurveValueOfEntry(const CurveStorePoint& point, Amount* out) {
    for (const CurveStorePoint::Entry& entry : point.entries) {
        if (entry.missing || entry.text.empty()) continue;
        Amount amount;
        if (!ParseAmount(entry.text, &amount)) continue;
        *out = amount;
        return true;
    }
    return false;
}

// 一个存储点在**指定币种**下的金额：指定了就只认它，没指定（空串）时退回上面那条口径。
// ★ 为什么指定时**不回退**到别的币种：那是另一笔钱。回退等于把两条不同的曲线接起来，
//   与速率估计器同一条规矩（RateInputForCurrency 的注释）。
// ★ 为什么空串要回退：curve.json 在"第一次样本之前"就是这种形状（存储自己的
//   lastPrimaryCurrency 也是空的），那时每个点的第一个条目正是数据层记这个点时用的那个。
bool CurveValueOfEntryIn(const CurveStorePoint& point, const std::string& currency, Amount* out) {
    if (currency.empty()) return CurveValueOfEntry(point, out);
    const CurveStorePoint::Entry* entry = point.Find(currency);
    if (entry == nullptr || entry->missing || entry->text.empty()) return false;
    return ParseAmount(entry->text, out);
}

// "现在"：最近一次交付给显示层的样本（余额 + 它的整秒时间戳）。
//   ★ 存在的唯一理由就是 `StoreStepPerMinuteAt()` 那句"最新点 → 现在"——存储里没有
//     "现在"这个概念，它只记得变化发生的那一刻。**颜色不读它**（见下面那一段）。
bool g_currentStepFromValid = false;
int64_t g_currentStepFromAt = 0;
double g_currentStepFromBalance = 0.0;

// ===========================================================================
// **两个"这一步有多陡"，故意是两份实现**（所有者 2026-09-19 的拆分）
// ===========================================================================
//  同一个"元/分钟"在这里要回答两个**互相冲突**的问题，所以它们不能共用一个函数：
//
//   · 颜色要的是「**进入某个点**的那一步」——两个**相邻的存储点**之间、用它们
//     自己的时间戳。它回答的是"这一点相对上一点跌得有多陡"，与"现在几点"无关。
//     → `StepIntoNewestPointPerMinute()`
//
//   · R 的衰减要的是「最新那个点 **→ 现在**」——同一个下降量，余额不动时被越摊越薄，
//     于是平静期 R 会自己凉下来。它回答的是"刚刚有多陡"，必须读"现在"。
//     → `StoreStepPerMinute()`
//
//  ★ 为什么必须分开（实测，不是洁癖）：`FeedCurve` 是**先 Append 再量这一步**的，
//    而"现在"就是刚 Append 进去的那个样本 —— 两者时间戳逐位相同。走"最新点 → 现在"
//    的那条路来量颜色，Δt 恒被夹成 1、两个量又相等，于是**每个点的颜色都算成 0**
//    （= 基准蓝 #6c89f6）。所有者的原话是"后面线的颜色没有了"，像素证据是 11 个点
//    只导出 121 种颜色、且全挤在 (59..63, 71..77, 115..126) 这一个蓝上 —— 渐变没了。
//    这就是上一轮为了修 R 的衰减而把"现在"提前到量之前造成的逆带伤害。
//  ★ 分开之后两边各自单纯：颜色不看"现在"，衰减不看存储里两个点的间隔。

// 一个点**自己那一刻**的"进入它的那一步"（元/分钟）。口径 = 这个点与它前一个点。
// dt <= 0（同一秒内的两个点、或时间戳缺失）-> 返回 0：算不出来就是算不出来，
// 绝不拿 1 秒、也绝不拿"现在"来凑一个数。
// ★ 间隔不用 `kStepGapAwayThresholdSeconds` 那套上限：那一套是给**平静期摊薄**用的
//   （"最新点 → 现在"可以拉到几小时）。这里是两个**真实测量点**之间的间隔，它就是
//   那一段的真实时长，把它夹掉等于凭空说这一段更陡。
double StepIntoPointPerMinute(const CurveStorePoint& previous, const CurveStorePoint& current) {
    if (!previous.atValid || !current.atValid) return 0.0;
    const double dt = static_cast<double>(current.at - previous.at);
    if (!(dt > 0.0)) return 0.0;
    Amount prevAmount;
    Amount curAmount;
    if (!CurveValueOfEntry(previous, &prevAmount)) return 0.0;
    if (!CurveValueOfEntry(current, &curAmount)) return 0.0;
    // 参数顺序 = (上一个量, 这一个量, Δt)：`StepPerMinute` 算的是 (cur - prev)。
    // 写成 (cur, prev) 会把方向倒过来 —— 余额上涨会被算成剧烈下降，颜色恒红。
    return StepPerMinute(prevAmount.ToDouble(), curAmount.ToDouble(), dt);
}

// **进入存储里最新那个点**的那一步（元/分钟）。颜色通道的唯一取值口。
// 没有两个可用点（首次运行、只有一个点、时间戳缺失）-> 返回 0：
// 那时"这一步"根本不存在，颜色就是基准色（R = 0），不编造。
double StepIntoNewestPointPerMinute() {
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    if (points.size() < 2) return 0.0;
    return StepIntoPointPerMinute(points[points.size() - 2], points.back());
}

// 这一步有多陡（元/分钟）：**从最后一次变化，量到这一拍**。
//   ★ 口径（所有者 2026-09-18 / 2026-09-19）：`R_new` = 最后一次变化造成的下降量，
//     除以**从那次变化到这一拍的全部时间**。
//   ★★ 为什么必须摊到"现在"，不能只看存储里最后两个点：
//     存储**只在余额变化时记点**，所以余额一旦不动，最后两个点就是**几分钟/几小时前的
//     一对**。拿那一对的陡度当"现在的陡度"每一帧去抬 R（抬升规则：target > 当前值就抬），
//     R 就被**永久钉死**在那一刻的高度上，再也掉不下来。
//     所有者 2026-09-19 报的症状是"取消关闭态之后 R 好像不下降了"——实际与取消无关，
//     是这一条：余额不动之后 R 无论如何都不会掉。
//   ★ 余额不动得越久，**同样的下降量**被摊得越薄，R_new 自然趋近 0 —— 这正是
//     "平静下来 R 会自己凉"的物理来源，也是原始设计（按样本间隔算）里天然存在、
//     而"只看最后两个点"那次改动把它弄丢的那一条。
//   ★ Δt 仍然夹 `kStepGapCapSeconds`：那是所有者 2026-09-18 为"仪表关着两小时"定的上限
//     （否则停机期间的花费被摊成 0.0092 元/分、永远不红）。两件事共用同一个上限：
//     一次陡降之后约 100 秒内 R 平滑退下去，之后不再继续摊薄。
//   ★ "现在"还没有（进程刚起、一个样本都没交付）时，退回"存储里最后两个点"——那种时刻
//     两者本来就同值。
//   ★ 金额取每个点的**第一个有值条目**（CurveValueOfEntry），与会话里当前显示的币种无关。
//   ★ **颜色不要用这个函数**（见上面那段）：它量的是"现在"，而颜色要的是"进入这个点"。
//     两者在 FeedCurve 那一刻必然给出不同的答案。
//
// ★★ 派生的那一份实测（这一段让这个函数多了个参数，理由值得留着）：
//   `nowSeconds` **不是**原样用"这一个样本的时间戳"。样本按固定节奏到达（采样间隔 = Δ），
//   而"这一拍"该量的端点比它**早一个 Δ**：`FeedCurve` 是先把样本 Append 进去、再量这一步的
//   （回填颜色必须那个次序），所以这一拍测量时"最后一次变化"已经过去了**两个** Δ ——
//   一个是"变化 → 上一个样本"，一个是"上一个样本 → 这一个样本"。少减这一个 Δ 会让整条
//   R 的衰减曲线整体晚一个采样间隔（实测：陡降后平静 60 秒，R 量成 0.6227 而不是 0.4995，
//   因为 Δt 只算了 50 秒）。Δ 由调用方按"上一个样本的时间"传进来。
double StoreStepPerMinuteAt(int64_t nowSeconds) {
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    if (points.empty()) return 0.0;

    // ---- 首选：从"最后一次变化"量到"现在" ----
    //  ★ 无论这一拍**有没有落点**，量的都是同一段：最新那个点（= 最后一次变化）
    //    → 现在。落了点时"最新那个点"就是这一次变化本身，没落点时它还是上一次变化 ——
    //    两种情形下它都是"最后一次变化"，端点相同，所以不需要分两条路。
    //    ★ 这一点我一开始分成了两条分支，分别量"最后两个点之间"和"最新点 → 现在"，
    //      于是同一个 60 秒被量成 50 秒或 60 秒，取决于那一拍落没落点 —— 两条路必须
    //      给出同一个答案，而"同一个答案"只有把端点定死才能保证。
    if (points.size() >= 2 && nowSeconds > points.back().at) {
        Amount newestAmount;
        if (CurveValueOfEntry(points.back(), &newestAmount)) {
            double dt = static_cast<double>(nowSeconds - points.back().at);
            if (dt < 1.0) dt = 1.0;            // 同一秒、或时间戳没变：至少按 1 秒算
            if (dt >= kStepGapAwayThresholdSeconds) dt = kStepGapCapSeconds;
            // ★ 参数顺序 = (上一个量, 这一个量, Δt)：`StepPerMinute` 算的是 (cur - prev)。
            //   写成 (newest, prev) 会把方向倒过来 —— 余额**上涨**会被算成剧烈下降、
            //   R_new 恒 1、R 永久钉死（实测就是这么错了一版，closeprobe case6 抓出来的）。
            return StepPerMinute(newestAmount.ToDouble(), g_currentStepFromBalance, dt);
        }
    }
    if (points.size() < 2) return 0.0;             // 只有一个点且没有"现在"：无从判断

    // ---- 退回：存储里最后两个可用点（时间戳缺失的那几个点在这里被跳过）----
    for (std::size_t i = points.size(); i-- > 1;) {
        const CurveStorePoint& cur = points[i];
        const CurveStorePoint& prev = points[i - 1];
        if (!cur.atValid || !prev.atValid) continue;
        double dt = static_cast<double>(cur.at - prev.at);
        if (!(dt > 0.0)) continue;
        if (dt > kStepGapCapSeconds) dt = kStepGapCapSeconds;
        Amount curAmount;
        Amount prevAmount;
        if (!CurveValueOfEntry(cur, &curAmount)) continue;
        if (!CurveValueOfEntry(prev, &prevAmount)) continue;
        return StepPerMinute(prevAmount.ToDouble(), curAmount.ToDouble(), dt);
    }
    return 0.0;   // 没有可用的一步 = R = 0（"算不出来"，不是"在剧烈消耗"）
}

// 最近两次测出来的"这一步有多陡"（元/分钟），口径 = 存储里最后两个点。
//   ★ 为什么需要它：主循环**滞后一拍**（CommitDelayed 把上一个样本交给显示层，最新的那个
//     先压着）。`FeedCurve` 与 `AdvanceAmbience` 在**同一拍**里先后跑：
//         FeedCurve(s_N)      —— 为刚交付的 s_N 测出这一步（上一个点 → s_N），颜色用它
//         AdvanceAmbience     —— 同一拍稍后，此时屏幕上显示的**就是 s_N**
//   ★ 氛围读 `current`（屏幕上正在显示的那一步），不读存储：存储此刻已经有更新的点了，
//     直接读它会拿到"用户还看不到的那一步"，于是出现"数字几乎没动、面板却突然变红"
//     （所有者 2026-09-18 指出并选定 S2）。
struct StepMemory {
    double current = 0.0;    // 最近一次交付的样本所测的那一步（= 屏幕上正在显示的那一步）
    bool valid = false;
};
StepMemory g_stepMemory;

// 屏幕上这一步有多陡（元/分钟）。
//   ★ 只做一件事：把"最近交付的那一步"取出来。**不再抄一遍存储扫描** —— 这个项目已经
//     吃过"同一件事两份实现"的亏（审计员 2026-09-18 实测：那两份 store 扫描逐字相同，
//     而第三份的口径不同，差 2 倍）。
//   ★ 没有历史（刚启动、还没交付过任何样本）时返回 0；调用方在那种情况下用
//     `DisplayedAmount::LastStepPerMinute()`（那时两者本来就同值，见那里的说明）。
bool AmbientStepKnown() { return g_stepMemory.valid; }
double AmbientStepPerMinute() { return g_stepMemory.current; }


// 纵向缓动：P = L + (N − L) × (1 − rate^k)^c（规格 §3 第 6 条，k = 帧号）。
// ★ 走到终点（第 600 帧，整 10 秒）时直接取 N：公式在 k=600 处还剩 0.975^600 ≈ 2.5e-7
//   的残量（折算约 7e-5 像素），而"动画结束在数据自己给出的位置上"正是验收项 2 要逐
//   像素比的东西，所以终点取精确值。这不是改公式：10 秒之后本来就没有动画了。
double EasedY(double L, double N, double seconds) {
    if (seconds >= kCurveScrollSeconds) return N;
    const double k = seconds * kCurveFrameHz;
    const double decay = std::pow(static_cast<double>(kCurveRollRate), k);
    return L + (N - L) * std::pow(1.0 - decay, static_cast<double>(kCurveRollC));
}

// 环里两个快照是不是同一批点（逐条目比币种/缺失/原文）。
bool SamePoints(const std::vector<CurveStorePoint>& a, const std::vector<CurveStorePoint>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].entries.size() != b[i].entries.size()) return false;
        for (std::size_t j = 0; j < a[i].entries.size(); ++j) {
            if (a[i].entries[j].currency != b[i].entries[j].currency) return false;
            if (a[i].entries[j].missing != b[i].entries[j].missing) return false;
            if (a[i].entries[j].text != b[i].entries[j].text) return false;
        }
    }
    return true;
}

// 一条取样喂给数据层（§2.1 / §2.3 的规则全在那里），并观察"是不是追加了一个点"。
// 只有追加才启动滚动——值不变时曲线**静止**（规格 §2.1 的推论，这条是刻意的）。
// 记录文件的路径（main 启动时给一次）。为空 = 不落盘（导帧路径就是这样）。

void FeedCurve(const Sample& s) {
    std::vector<CurveStorePoint> before = g_curveStore.Points();
    std::vector<double> beforeValues;
    const bool beforeOk = CurveValues(before, &beforeValues);

    CurveObservation obs;
    // §2.1：判定"变没变"用**响应第一个条目**那个币种。
    obs.primaryCurrency = !s.entries.empty() ? s.entries.front().currency : s.currency;
    for (const CurrencyAmount& e : s.entries) {
        obs.observations.push_back(CurveObservation::Item{
            e.currency, e.ok ? e.total.ToString2() : std::string(), e.ok});
    }
    if (obs.observations.empty()) {
        if (!s.amountsOk || obs.primaryCurrency.empty()) return;   // 没有币种信息，没什么可记
        obs.observations.push_back(CurveObservation::Item{
            obs.primaryCurrency, s.total.ToString2(), true});
    }
    // 样本的**墙钟秒**就是数据层的时间戳（规格 §2.3）。
    // ★ "现在"**只服务 R 的衰减**（`StoreStepPerMinuteAt()`），颜色走另一条路
    //   （`StepIntoNewestPointPerMinute()`，见下面那一大段）。
    //   ★ 它在 `Append()` **之前**更新，所以量 R 那一步时它就是"这一个样本"。
    //   ★ 顶部那两处 `return`（没有币种信息）在这个更新之前：那种样本连"现在"都不该占 ——
    //     它没有可读的余额，会把"现在"推到一个不存在的时间点上。
    g_currentStepFromValid = true;
    g_currentStepFromAt = s.wallMs / 1000;
    g_currentStepFromBalance = s.total.ToDouble();

    // ★ 氛围颜色的口径（所有者 2026-09-18）：**段 P_N→P_(N+1) 的颜色 = 终止于 P_(N+1)
    //   那一步的 R_new**，也就是"进入 P_(N+1) 的那一步"。
    //   ★ 于是它只能**回填**：写 P_N 的时候"到 P_(N+1) 那一步"还不存在（要等 P_(N+1) 到达
    //     才知道）。顺序是：
    //       1. 这一拍先把新点追加进去（暂时不带颜色）
    //       2. 量出"上一个点 → 这个新点"这一步（`StoreStepPerMinute()`，此刻存储的最后
    //          两个点正好是它们）
    //       3. 用这一步的颜色覆盖**上一个点**的颜色 —— 它右边那一段就是这一步
    //   ★ 用"样本间隔"代替"存储里两点间隔"会错：余额没变的采样不成点，两者会越差越多
    //     （审计员实测 10 s 采样、20 s 变化一次时差 2 倍）。
    // ★ 氛围颜色的口径（所有者 2026-09-18）：**段 P_N→P_(N+1) 的颜色 = 终止于 P_(N+1)
    //   那一步的 R_new**，也就是"进入 P_(N+1) 的那一步"。
    //   ★ 于是它只能**回填**：写 P_N 的时候"到 P_(N+1) 那一步"还不存在（要等 P_(N+1) 到达
    //     才知道）。顺序是：
    //       1. 这一拍先把新点追加进去（暂时不带颜色）
    //       2. 量出"上一个点 → 这个新点"这一步（`StepIntoNewestPointPerMinute()`，此刻
    //          存储的最后两个点正好是它们）
    //       3. 用这一步的颜色覆盖**上一个点**的颜色 —— 它右边那一段就是这一步
    //   ★ 用"样本间隔"代替"存储里两点间隔"会错：余额没变的采样不成点，两者会越差越多
    //     （审计员实测 10 s 采样、20 s 变化一次时差 2 倍）。
    //   ★★ 这里的量法**不能**换成 `StoreStepPerMinute()`（"最新点 → 现在"）：那一刻
    //     "现在"就是刚追加进去的这个点，Δt 恒被夹成 1、两个量又相等 —— 每个点都算成
    //     0、每个点都被涂成同一个基准蓝。所有者说的"后面线的颜色没有了"就是这一条，
    //     像素证据见文件上方"两个『这一步有多陡』"那一段。颜色走这一条，衰减走那一条。
    g_curveStore.Append(obs, s.wallMs / 1000, std::string());
    // ★ 判定"追加了一个点"看的是**存储自己的状态**，不是 Append() 返回值的含义：
    //   点数变了，或者环里的内容变了（容量到顶时 size() 不动，但最老的点会被挤掉）。
    const std::vector<CurveStorePoint> after = g_curveStore.Points();
    const bool appended = !SamePoints(before, after);
    const double stepIntoNewest = StepIntoNewestPointPerMinute();
    // 回填：把"终止于最新那个点的那一步"的颜色写到**它的前一个点**上 —— 那一段就是这一步。
    // 颜色里的余额用前一个点自己的值（D 要按它自己的余额算，不能用最新那个点的）。
    // ★★ 余额取**主币种那一笔**，与 D 用的是同一个数（同一个币种、同一把"钱"的尺子）：
    //    以前这里取的是"第一个可用条目"，主币种恰好写在第一条时它碰巧是对的 ——
    //    而海外账号的第一条是 USD，那个美元数字会被当成元去比 10 元阈值，
    //    于是曲线的颜色与"危险度"讲的是两件事（同一笔钱两个结论）。
    //    现在按**名字**取（名字取自本帧样本的主币种，与数据层判定"变没变"用的是同一个），
    //    取不到就**不上色**（点保持"没有颜色"，渲染层用它右边那个点的颜色画），绝不编造。
    //    ★ 为什么用本帧样本的主币种而不是"那个点自己的主币种"：存储点里**没有**记这个
    //      名字（curve.json 只存条目），而一个会话里所有点的主币种都是同一个 ——
    //      接口换主币种时，上一个点与这一个点之间本来就没有可比性。
    {
        const std::vector<CurveStorePoint> pts = g_curveStore.Points();
        if (pts.size() >= 2) {
            const CurveStorePoint& prevPoint = pts[pts.size() - 2];
            const CurveStorePoint::Entry* primary = prevPoint.Find(obs.primaryCurrency);
            Amount prevAmount{};
            if (primary != nullptr && !primary->missing && !primary->text.empty() &&
                ParseAmount(primary->text, &prevAmount)) {
                g_curveStore.SetColorOfPrevNewest(
                    HexOf(BalanceColorAt(prevAmount.ToDouble(), SeverityRatio(stepIntoNewest))));
            }
        }
    }
    // ★ R 的衰减要的是**另一件事**：从最后一次变化，摊到"现在"。它必须在这里再量一次
    //   （见上面那一段）—— 余额没变的采样不成点，只有时间在往前走，平静期才摊得薄。
    //   这一步刚测出来，留给**下一次**用：主循环滞后一拍，所以下一次刷新时屏幕上
    //   正在显示的就是这一步，氛围要用它（见 StepMemory 的说明）。
    g_stepMemory.current = StoreStepPerMinuteAt(g_currentStepFromAt);   // 原始量（元/分钟）
    g_stepMemory.valid = true;

    // ★ 判定"追加了一个点"看的是**存储自己的状态**，不是 Append() 返回值的含义：
    //   点数变了，或者环里的内容变了（容量到顶时 size() 不动，但最老的点会被挤掉）。
    //   只比 size() 会在环满之后永远看不到新点（那时曲线会在生产里停住不滚）。
    if (!appended) return;

    // 落盘：只在"真的追加了一个点"之后写（12 个点，代价极小；崩溃最多丢最后一次）
    const std::string& cp = dshb::CurveStorePath();
    if (!cp.empty()) (void)g_curveStore.Save(cp);

    // 追加了：计时器归零，并记下"旧极值"= 刚才屏幕上那 11 个点的极值（规格 §3 第 6 条）。
    // 新点入场时也按这套旧极值算 L，于是它不会凭空跳进来，而是从旧刻度滑过去。
    const std::size_t keep =
        (before.size() < kCurveDisplayPoints) ? before.size() : kCurveDisplayPoints;
    g_curveOldValid = beforeOk && keep > 0;
    if (g_curveOldValid) {
        const CurveSpan old = SpanOf(beforeValues, before.size() - keep, before.size());
        g_curveOldLo = old.lo;
        g_curveOldHi = old.hi;
    }
    g_curveSeconds = 0.0;
    g_curveScrolling = true;
}

// 每帧推进滚动计时器（规格 §3：计时器由帧 dt 推进，追加时归零）。
void AdvanceCurve(double dtSeconds) {
    if (g_curveFrozen || !g_curveScrolling) return;
    g_curveSeconds += dtSeconds;
    if (g_curveSeconds > kCurveScrollSeconds) {
        g_curveSeconds = kCurveScrollSeconds;
        g_curveScrolling = false;   // 走完了：显示集就是最新 11 个，不再有"第 12 个"
    }
}

// 组装一帧的曲线点（规格 §3）。全部是"存储内容 + 计时器"的纯函数。
void BuildFrameCurve(WidgetFrame* f) {
    f->curve.clear();
    f->curveHasData = false;

    // ★ 面板的显示集与存储容量**解耦**（所有者 2026-09-19：容量 12 -> 120）。
    //   这一层要的永远是"最新 12 个点"（11 个在看 + 1 个进场），那是一个**显示**常量
    //   （kCurveDisplayPoints），不是存储的容量。存储从 12 涨到 120 之后，直接拿全部
    //   存储点往下算会让滚动那一瞬间（first = 0）把 120 个点全塞进 11 个槽位——
    //   间距变成 1/119，曲线被压成一条细丝再滑过去，面板就坏了。
    //   所以入口先切片，后面对 n 的一切（槽位基准、span、裁剪）都基于切片自己的 size；
    //   容量是 12 还是 120，画出来的东西逐像素相同。
    //   代价：每次多一次 vector 拷贝（最多 12 个点，一帧一次，可以忽略）。
    const std::vector<CurveStorePoint> allPoints = g_curveStore.Points();
    const std::size_t keep = (allPoints.size() > kCurveDisplayPoints + 1)
                                 ? (kCurveDisplayPoints + 1)
                                 : allPoints.size();
    const std::vector<CurveStorePoint> points(allPoints.end() - static_cast<std::ptrdiff_t>(keep),
                                              allPoints.end());
    const std::size_t n = points.size();
    if (n == 0) return;                         // 没有数据：渲染层画带子正中的平线

    std::vector<double> values;
    if (!CurveValues(points, &values)) return;

    const bool scrolling = g_curveScrolling;
    const double seconds = g_curveSeconds;
    const double progress =
        (seconds >= kCurveScrollSeconds) ? 1.0 : (seconds / kCurveScrollSeconds);

    // 滚动结束后留下的那些点（规格 §3 第 6 条：N 用"留下的点"的极值）——静止时
    // 它就是显示集本身（最新 11 个）。
    const std::size_t shownBegin = (n > kCurveDisplayPoints) ? (n - kCurveDisplayPoints) : 0;
    const CurveSpan newSpan = SpanOf(values, shownBegin, n);
    const CurveSpan oldSpan =
        (scrolling && g_curveOldValid) ? CurveSpan{g_curveOldLo, g_curveOldHi} : newSpan;

    // 画哪些点、各自在哪个槽位：
    //   静止：最新 11 个，槽位 0..10（最老在左、最新在右边缘）。
    //   滚动：切片里全部点（最多 12 个 —— 切片在上面的入口就做完了，所以存储里
    //         有 120 个点也一样），槽位再右移 0.1×(1−进度)——于是"第 12 个"
    //         从 x=1.1 进来、整条以**线性**进度左移一格，走完时正好落在"最新 11 个"。
    const double slotBase = static_cast<double>(n) - 1.0 - static_cast<double>(kCurveSegments);
    const std::size_t first = scrolling ? 0 : shownBegin;

    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(n + 1);
    ys.reserve(n + 1);
    // ★ 与 ys 一一对应的"该点自己那一刻的颜色"（"E 蒙光.md" §3.1 末句）。
    //   点没有颜色（老文件）时填 hasColor = false —— 不补、不猜。
    std::vector<AmbienceColor> colors;
    std::vector<bool> hasColors;
    colors.reserve(n + 1);
    hasColors.reserve(n + 1);
    for (std::size_t p = first; p < n; ++p) {
        const double slot = static_cast<double>(p) - slotBase;
        double x = SlotX(slot);
        if (scrolling) x += 0.1 * (1.0 - progress);
        const double y = scrolling ? EasedY(NormY(values[p], oldSpan), NormY(values[p], newSpan),
                                            seconds)
                                   : NormY(values[p], newSpan);
        xs.push_back(x);
        ys.push_back(y);
        AmbienceColor stored{};
        const bool have = ParseHexColor(points[p].color, &stored);
        colors.push_back(stored);
        hasColors.push_back(have);
    }
    if (xs.size() < 2) return;                  // 一个点：渲染层画平线（curveHasData = false）

    // 横向裁剪到 [0,1]（规格 §3）：两端的点只要"还有一段在画面里"就留着——单调三次只在
    // [0,1] 上取值，越界的部分自然画不出来（渲染层按 u∈[0,1] 采样）。完全在画面外、
    // 连相邻那一段都挤不进来的点直接丢掉：否则它会通过切线影响画面内那一段的形状，
    // 于是"滚动终点帧"与"静止帧"就不再逐像素相同了。
    std::size_t from = 0;
    while (from + 1 < xs.size() && xs[from + 1] <= 0.0) ++from;
    std::size_t to = xs.size();
    while (to > 1 && xs[to - 2] >= 1.0) --to;
    if (to <= from) return;

    // 左侧拉平（规格 §2.2、验收 3）：显示的点还不够 11 个时，最早那个点左边的区间
    // "看做与它同值"。滚动中同理——最左那个点滑出画面后，左端由次左点拉平。
    // ★ 这个补位点不对应任何存储点，所以它**没有**自己的颜色（hasColor = false）：
    //   渲染层会沿用它右边那个真点的颜色（见 renderer.cpp 的 PaintAmbientCurve）——
    //   那段本来就没有独立的测量值，硬套一个颜色等于发明一个测量值。
    if (xs[from] > 0.0) {
        CurvePoint lead{};
        lead.x = 0.0f;
        lead.y = static_cast<float>(ys[from]);
        lead.hasColor = false;
        f->curve.push_back(lead);
    }
    for (std::size_t i = from; i < to; ++i) {
        CurvePoint cp{};
        cp.x = static_cast<float>(xs[i]);
        cp.y = static_cast<float>(ys[i]);
        cp.hasColor = hasColors[i];
        cp.cr = colors[i].r;
        cp.cg = colors[i].g;
        cp.cb = colors[i].b;
        f->curve.push_back(cp);
    }
    // 极值两边都退化（没有数据 / 只有一个点 / 全都一样）= 平线，按老规矩交给渲染层画。
    f->curveHasData = (f->curve.size() >= 2) && !(newSpan.degenerate() && oldSpan.degenerate());
}

// ---------------------------------------------------------------------------
// 消耗速率：把**本文件自己的曲线存储**变成估计器的输入（设计 §7.2 §7.3）
// ---------------------------------------------------------------------------
//  ★ 只取"当前显示的那个币种"，和曲线画的那条序列**故意不同**：曲线永远画数据层
//    记下的那条（primary 条目），而速率是给用户看的——他要的是"我现在看的这个
//    数字还剩多久"，所以必须用他正在看的币种。两个口径各自都要成立。
//  ★ 每个点用它**自己的**时间戳（curve_store §2.3b）：`atValid == false` 的点就是
//    "没有时间"，照样传成 atValid=false 交给估计器（它会因此报"不显著"），
//    **绝不**拿存储的全局 update_at 顶替——那会把"没人测量过"变成一个时间。
std::vector<RateInputPoint> RateInputForCurrency(const std::string& currency) {
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    std::vector<RateInputPoint> out;
    out.reserve(points.size());
    for (const CurveStorePoint& point : points) {
        RateInputPoint p;
        p.at = point.at;
        p.atValid = point.atValid;

        const CurveStorePoint::Entry* entry = nullptr;
        if (currency.empty()) {
            // 还没有显示中的币种（第一次样本之前）：退回这个点的第一个有值的条目，
            // 与曲线取值的口径一致。这不是"随便挑一个"——它就是数据层记这个点时
            // 用的那个币种（primary）。
            for (const CurveStorePoint::Entry& e : point.entries) {
                if (!e.missing && !e.text.empty()) { entry = &e; break; }
            }
        } else {
            // ★ 有显示中的币种时**不回退**：这个点没有该币种就是"这个点没有可用金额"。
            //   回退到别的币种等于把两条不同的曲线接在一起，比不显著更糟。
            entry = point.Find(currency);
        }
        if (entry != nullptr && !entry->missing && !entry->text.empty()) {
            Amount amount;
            if (ParseAmount(entry->text, &amount)) {
                p.amountRaw = amount.raw;
                p.amountValid = true;
            }
        }
        out.push_back(p);
    }
    return out;
}

// UTF-16（文案在估计器里就是宽字符）-> UTF-8（WidgetFrame 里的字段是窄字符，
// 渲染层自己再 Widen 回去）。失败时给空串：宁可这一行不画，也不画半截乱码。
std::string Utf8FromWide(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// 数字那一侧用的小工具
// ---------------------------------------------------------------------------

// 找出两段文本第一个与最后一个不同的位置。长度不等时退化为"整段都变"。
void DiffSpan(const std::string& a, const std::string& b, int* from, int* to) {
    *from = 0;
    *to = static_cast<int>(b.size()) - 1;
    if (a.size() != b.size()) return;
    int i = 0;
    while (i < static_cast<int>(b.size()) && a[i] == b[i]) ++i;
    if (i == static_cast<int>(b.size())) {   // 完全相同
        *from = 0;
        *to = -1;
        return;
    }
    int j = static_cast<int>(b.size()) - 1;
    while (j >= 0 && a[j] == b[j]) --j;
    *from = i;
    *to = j;
}

}  // namespace

// 曲线存储里最新那个点的余额。
// ★ 给启动过渡用（main.cpp 的 CommitDelayed）：起点就取它 —— "上次关掉前显示的余额"
//   就是存储里最新那个点，不必在别处再存一份。取不到（存储为空）时返回 false。
// ★ 不判它多老：超过 86400 秒的存储在加载时已被整份丢弃（curve_store 的 §2.3 规则），
//   所以这里拿到的一定在一天之内。
// ★ 返回 Amount 而不是"元"的 double：调用方要拿它和**当前采样**判"变没变"，而两个
//   整数（1/10000 元的 raw）比两个 double 更靠得住 —— 余额本来就是整数量，走一趟
//   浮点再比等于把"相同"也交给舍入去裁决（设计 §3.1 就是为此定下的整数存储）。
bool CurveStartBalance(Amount* outAmount) {
    if (outAmount == nullptr) return false;
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    for (std::size_t i = points.size(); i-- > 0;) {
        Amount amount;
        if (CurveValueOfEntry(points[i], &amount)) {
            *outAmount = amount;
            return true;
        }
    }
    return false;
}

// 连续取不到值：回到"没有值"的状态（界面显示 --.--），但**历史不丢**：
// 下一次成功样本会重新落位。所有者：先当作没变，连续 5 次才显示 --.--。
void DisplayedAmount::MarkUnreadable() {
    hasValue_ = false;
    trips_.clear();
    places_.clear();
    animating_ = false;
    tripsDirty_ = true;
}

void DisplayedAmount::OnSample(const Sample& s) {
    if (!s.amountsOk) return;                 // 读不到的样本不参与显示
    lastSampleWallMs_ = s.wallMs;   // 曲线横轴锚点（见 widget_display.h）

    // ---- 显示哪个币种：现在只剩**一条**规则 —— 接口给的那条优先条目 ----
    // 优先条目 = balance_source 按 api::PreferredEntryIndex 挑出来的那一条，就是 s.currency。
    // ★ 为什么只剩一条：币种符号曾经可点、双击能在币种之间切换，于是还有一条"上次选中的
    //   币种优先"；那个入口在 97179eb 被删之后，记"选中了哪个"的字段再没有写方、恒为空，
    //   那条分支结构上永远不成立 —— 规则与状态一起删了。
    // ★ 按**名字**比对而不是按下标：接口不保证数组顺序（设计 §2.2）。
    Amount picked = s.total;
    std::string pickedCode = s.currency;
    if (!s.entries.empty()) {
        const CurrencyAmount* hit = nullptr;
        for (const CurrencyAmount& e : s.entries) {
            if (e.ok && e.currency == s.currency) { hit = &e; break; }
        }
        if (!hit) {
            // 优先条目这一笔没有数：退回本次样本里第一个有数据的条目。
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok) { hit = &e; break; }
            }
        }
        if (hit) {
            picked = hit->total;
            pickedCode = hit->currency;
        }
    }
    currencyShown_ = pickedCode;

    const double yuan = picked.ToDouble();
    latest_ = yuan;

    // 余额为 0 需要连续两次采样确认：防止瞬时 0 把整个界面闪成灰白
    if (picked.raw == 0) {
        if (zeroPending_) {
            zeroConfirmed_ = true;
        } else {
            zeroPending_ = true;
            return;                            // 这一次不采用，等下一次确认
        }
    } else {
        zeroPending_ = false;
        zeroConfirmed_ = false;
    }

    // ---- D 的输入：**主币种余额**（见 AdvanceAmbience 第 3 步）----
    // ★ 主币种 = 接口的**第一个条目**（数据层判定"变没变"用的就是它，见 curve_store.h）；
    //   没有条目时退回 Sample::currency/total（模拟数据源就是这种形状，币种是 CNY）。
    //   ★ 它**不一定**等于屏幕上显示的那个币种：币种选择只改显示，不该改危险度 ——
    //     所以这一段算完就放着，币种选择那一段一个字节都不碰它。
    // ★ 主币种那一笔读不出来时不回落到别的条目：那是另一个数字（这里要的是"这一笔钱
    //   多少"，不是"随便找得到的某个数"）—— 读不出来就是 primaryBalanceOk_ = false，D 取 0。
    {
        Amount primaryAmount = s.total;
        bool haveAmount = s.amountsOk;
        if (!s.entries.empty()) {
            haveAmount = s.entries.front().ok;
            primaryAmount = s.entries.front().total;
        }
        primaryBalanceOk_ = haveAmount;
        primaryBalance_ = haveAmount ? primaryAmount.ToDouble() : 0.0;
    }

    // ★ 目标值直接改写（可以突变），显示值照旧慢慢追——这就是设计里那句
    //   "变量可以突变，但要套一个显示变量，那个显示变量是逐渐变化、跟着那个突变变量的"。
    //   起点值只记来给自检报告"走了多少比例"，不参与计算。
    rollFromValue_ = hasValue_ ? value_ : yuan;

    lastReal_ = hasValue_ ? target_ : yuan;   // L = 上一次的实际数字（所有者的定义）
    target_ = yuan;                            // R：这一次的实际数字
    frames_ = 0;                               // k = 0：本次变化还没运算过
    animating_ = true;
    tripsDirty_ = true;                        // 下一帧重建每一位的行程
    // 氛围曲线（规格 §2/§3）：每次取到样本都喂给曲线存储；只有**追加了点**才滚动。
    FeedCurve(s);
    if (!hasValue_) {
        // 第一次拿到值就直接落位：从 0 滚上去会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
    }
}

// 显示值不是独立状态量，而是由三个参数导出的：
//     S = L + (R − L) × (1 − rate^k)
// rate^k 被截断为 0 时 S 恰好等于 R，所以"落定精确"是公式自带的，不需要吸附补丁。
// 这也是所有者这次的意思：参数是"实际数字 / 上次的实际数字 / 运算了 n 帧"，没有 display。
double DisplayedAmount::UpdateValue(double dtSeconds) {
    (void)dtSeconds;
    if (!hasValue_) { trips_.clear(); places_.clear(); return 0.0; }
    if (frozen_) return value_;
    SyncValueFromTrips();
    return value_;
}


// 每一位的纵坐标：目标是 floor(显示值 / 10^位次)，本帧朝目标追赶 dt 秒。
//
// 为什么不是直接令 coord = 显示值 / 10^位次：
//   那样静止时高位永远停在两格之间。实测 99.50 的十位会变成 9.95 -> frac 0.95，
//   整格几乎滚到 0，屏幕上显示成 "09.00"。所以静止必须落在整数上。
//   而"追赶一个只会 ±1 变的目标"既保住了整数落点，又让过程连续（滚动感）。
//
// 文本决定有哪些位次：高位是 0 时文本里根本没有那一位，于是它自动隐藏。
// 每一位自己的行程：轮子只有在自己这一位要变的时候才动。
//
// ★ 为什么不能直接用 S/n 当相位（所有者发现的错）：
//   S=99.10 时十位 S/10=9.91 -> 相位 0.91，看起来"9 快走完了、0 占满"。
//   可 99.10 跌到 99.00 时**十位根本不会变**，轮子就该稳稳停在 9 上。
//   所以相位来自"这一位从哪走到哪"，而不是相对整十的绝对位置。
//
// 行程的起止都取整数（floor），并且**每一位自己管自己地滚**。
//
//   L = 上次变化时的实际数字，R = 这次的实际数字，n = 这一位的位权
//   起点格 = floor(L/n)（取整数：否则静止时轮子停在两个数字之间，实测踩过）
//   终点格 = kRollDDiffFloor ? floor(L/n) + floor((R − L)/n) : floor(R/n)
//   D_n    = 终点格 − 起点格                    这一位要走几格（0 = 完全不动）
//   coord_n(k) = 起点格 + D_n × (1 − rate^k)^c
//
//   · rate^k 用"一个量每帧自乘"实现，不做幂运算（所有者的要求）；c 是 kRollCurveC
//   · k→∞ 时 coord 正好落在终点格：整数 -> 读数清晰
//   · D_n == 0 的位从头到尾不动（所以"下降时十位应跟个位一样"成立）
//
// ★ D 的取整口径由 kRollDDiffFloor（tuning.h）选，默认是后者：
//   floor(R/n) − floor(L/n) 而不是 floor((R−L)/n)。差别在"起点不在整数格上"时——
//   例：L=99.50, R=100.00, n=10 -> floor(0.50/10)=0（十位不动），
//   可十位的数字要从 9 变成 0，必须走 1 步；floor(R/n)−floor(L/n)=10−9=1 ✓
//
//   每位记着：当前坐标 coord、起点格 from、这一位要走几格 D、自己的 rate^k。
//   每帧：
//       剩余 = (from + D) − coord              // 这一位还差多少格
//       这一位的 rate^k 自乘一次 -> coord = from + D × (1 − rate^k)^c
//       |剩余| < kRollSnapGrid  -> coord = from + D（**这一位**自己收尾）
//
//   · 收尾是**每位独立判断**的：某一位先到位就先停，不被别的位拖住
//     （所以起点格也取 floor：吸附之后位次落在整数上，"正好是自己的数字"成立）
//   · 新值到来时只改各自的 from / D，coord 从当前位置继续走 -> 不会跳
//     （全体共用一个 rate^k 时，中途来新值要重置共用状态，所有轮子被拽回起点，
//       所有者看到的"突变"正是如此；他 rate=0.99 一轮要 7.6 秒，而序列每 3 秒换值）
void DisplayedAmount::AdvancePlaces(double dtSeconds, const std::string& amountText) {
    (void)dtSeconds;
    if (!hasValue_ || amountText.empty()) {
        trips_.clear();
        places_.clear();
        return;
    }

    // ★ 欠款（负余额）：动画一律用**绝对值**。不用负值参与动画的原因很实在：
    //   "变小就往下滚"在 0 处会走到 9（磁带环绕），0.00 -> -1.00 会显示成 9.00。
    //   取绝对值后，-1.00 与 +1.00 的轮子行为完全一样，符号交给文本层。
    const double rawL = std::fabs(std::floor(lastReal_ * 100.0 + 0.5) * 100.0);
    const double rawR = std::fabs(std::floor(target_ * 100.0 + 0.5) * 100.0);

    // 重建"有哪些位次"：终点按公式算，起点取这一位的当前位置（连续性）。
    if (tripsDirty_) {
        std::vector<Trip> next;
        // ★ 行程覆盖**全部位次**（+6..−2），不再只覆盖目标文本里那几列。
        //   原因：100.00 -> 0.33 时十位/百位在目标文本里已经不存在了，只按文本建
        //   行程它们就当场消失（所有者要的是"滚到低于 1 才消失"）。
        //   超出的高位坐标为 0 -> 渲染层按"整数位坐标 ≥ 1 才画"隐藏。
        for (int place = 6; place >= -2; --place) {
            const double denom = std::pow(10.0, static_cast<double>(place) + 4.0);
            const double Lg = rawL / denom;   // 起点格（不提前取整）
            // 终点：所有者口径 L + floor((R−L)/n)，或 floor(R/n)
            double endGrid;
            if (kRollDDiffFloor) {
                endGrid = Lg + std::floor((rawR - rawL) / denom);
            } else {
                endGrid = std::floor(rawR / denom);
            }
            Trip t;
            t.place = place;
            bool kept = false;
            for (const Trip& old : trips_) {
                if (old.place == place) {
                    t.from = old.coord;        // 从当前位置继续，不跳
                    // 缓动不沿用：上一次那位已经吸附（ratePower=0），沿用会让它当场跳到新终点
                    t.ratePower = 1.0;
                    kept = true;
                    break;
                }
            }
            if (!kept) {
                t.from = std::floor(Lg);   // ★ 必须是整数格：否则静止时轮子停在两个数字之间（实测踩过）
                t.ratePower = 1.0;
            }
            t.D = endGrid - t.from;
            t.coord = t.from + t.D * std::pow(1.0 - t.ratePower, kRollCurveC);
            next.push_back(t);
        }
        trips_ = next;
        tripsDirty_ = false;
        animating_ = true;
    }

    // 手动模式：按 k 帧直接摆到那一帧的位置（不推进）。
    if (frozen_) {
        places_.clear();
        for (Trip& t : trips_) {
            t.ratePower = 1.0;
            for (int i = 0; i < frames_; ++i) t.ratePower *= kRollRate;
            const double eased = 1.0 - t.ratePower;
            t.coord = t.from + t.D * std::pow(eased, kRollCurveC);
            if (std::fabs(t.D - (t.coord - t.from)) < kRollSnapGrid) t.coord = t.from + t.D;
            places_.push_back(axis::PlaceCoord{t.place, t.coord});
        }
        SyncValueFromTrips();
        return;
    }

    // 正常推进：每位各自收敛、各自截断。
    bool anyMoving = false;
    places_.clear();
    for (Trip& t : trips_) {
        t.ratePower *= kRollRate;                       // rate^k 自乘，避免幂运算
        const double eased = 1.0 - t.ratePower;
        t.coord = t.from + t.D * std::pow(eased, kRollCurveC);
        const double remaining = (t.from + t.D) - t.coord;   // 这一位还差多少格
        if (std::fabs(remaining) < kRollSnapGrid) {
            t.coord = t.from + t.D;                    // 这一位自己到位了
            t.ratePower = 0.0;
        } else {
            anyMoving = true;
        }
        places_.push_back(axis::PlaceCoord{t.place, t.coord});
    }
    animating_ = anyMoving;
    SyncValueFromTrips();
}

// 显示值由**最细那一位**的坐标导出，保证"文本/状态"与"轮子位置"永远一致。
// 没有细位（比如只有整数位）时退回 L + (R−L) 的粗略值。
void DisplayedAmount::SyncValueFromTrips() {
    for (const Trip& t : trips_) {
        if (t.place == -2) { value_ = t.coord / 100.0; return; }
    }
    value_ = target_;
}

// ---------------------------------------------------------------------------
// 导帧用的氛围夹具：把 (R, D) 直接钉住
// ---------------------------------------------------------------------------
//  ★ 为什么需要它：验收要求从真机导出 (R,D) = (0,0)、(0.5,0)、(1,0)、(0,1) 四帧并
//    量像素。R 由"最近一步 ÷ 基线"天然决定，靠仿真数据**凑不出**任意值
//    （--history-demo 那串演示数据的最后一次跳变恰好让 R=1），所以没有这个口子，
//    "R=0.5 那一帧"就只能靠数据碰巧落在 0.5 —— 那不是可复现的证据。
//
//  ★ 走命令行开关 `--ambience=R,D`（所有者 2026-09-17 的要求）：环境变量在日志里
//    看不见，而本项目的验证全靠日志与导帧留痕。main.cpp 里两行接线：
//        else if (wcsncmp(argv[i], L"--ambience=", 11) == 0) {
//            if (!dshb::SetAmbienceGiven(<utf8 of argv[i]+11>)) { 记一条日志 }
//        }
//    这一层只提供 dshb::SetAmbienceGiven(text)（头文件里已声明）。
//  ★ 默认不生效，对生产路径零影响。
bool& AmbienceFrozenFlag() {
    // 默认 false：正常运行时氛围照走。--pause-ambience 是测试口子。
    static bool frozen = false;
    return frozen;
}

bool& TextLayerEnabled() {
    // 默认 true：正常运行时正文照画。--no-text 是测试口子（量底色用的）。
    // 渲染层通过 dshb::TextEnabled() 读它（renderer.cpp 与这里必须看同一个标志）。
    static bool enabled = true;
    return enabled;
}

AmbienceOverrideState& AmbienceOverride() {
    // 默认 given = false：正常运行时用算出来的 R/D，不用夹具。
    static AmbienceOverrideState state;
    return state;
}

// 对外只有一个 Update：先推进显示值，再按行程刷新每一位的坐标。
// 这样"值"和"轮子"永远在同一帧里一起走，调用方不需要记得多调一次。
// 曲线的滚动计时器也在这里推进（规格 §3：由帧 dt 推进，追加时归零）。
double DisplayedAmount::Update(double dtSeconds) {
    ShutdownAdvanceFrame();       // 帧号先走：它下面每一步（R 的下限、抖动）都要读它
    AdvanceCurve(dtSeconds);
    AdvanceRate(dtSeconds);       // 消耗速率（§7.3）：每帧重算 + 走一步弹簧
    const double shown = UpdateValue(dtSeconds);
    AdvancePlaces(dtSeconds, TextToShow());
    // 氛围放在**显示值之后**：R 那一半要读本帧刚推完的状态（心跳、蒙光都接在它后面），
    // 而且"这一帧的 R/D/颜色"必须是在文字与轮子都落位之后才定下来 —— 先算会用上一帧的
    // 状态，屏幕上就会出现"数字已经变了、颜色还差一拍"。
    // ★ D 自己**不**读屏幕上的数字（它读主币种折元后的余额，见 AdvanceAmbience 第 3 步）：
    //   所以它在切币种时不跟着符号走 —— 那正是所有者 2026-09-19 报的那个 bug。
    AdvanceAmbience(dtSeconds);
    return shown;
}

// 氛围：R 的衰减模型、D 与颜色
// ---------------------------------------------------------------------------
//                 R(t) = a^t          a = kAmbienceDecayA ∈ (0,1)，t 的单位是**分钟**
//                 每帧   t += dt       （冻结时不推进，于是 R 停住）
//   数据刷新时     R_new = clamp( min{ max(s, 2B), 0 } / (2B), 0, 1 )
//                  若 R_new > R(t)  ->  t = ln(R_new) / ln(a)
//   画到屏幕上     C = DesaturateTowards( AmbienceBase(R_display), D )     ← 没有缓动
//                  D = clamp( (G − 主币种余额) / G, 0, 1 )                    ← 瞬时量
//                  ★ 阈值 G 统一是 10（kLowBalanceThresholdYuan，元），喂进去的是主币种的
//                    数：切显示币种不改 D。主币种那一笔读不出来时 D = 0，理由见下面第 3 步。
//   t 的起点        进程刚起、一步都还没量到时 **R = 0**（不是 a^0 = 1）：
//                  t 只在第一次真的量出一步（R_new > 0）时才开始走。
//
//  三个后果，都是这个模型故意的：
//   1. R 只会被**抬高**：数据说"这次不那么陡"时，R 交给时间自己凉，不许被硬拉下去。
//      （R 低过当前值时连 t 都不动一下 —— 见下面 currentRatio 那一行的条件。）
//   2. 颜色**不需要也不许**再有缓动：平滑全部发生在 R 一侧，颜色是 R 与 D 的纯函数。
//      这同时消掉了旧实现里"从上一帧已降饱和的颜色再降一次"那条自我累积的路径。
//   3. 一次冲高之后，屏幕是连续地退回平静（t 是连续量），但 D 会在余额刷新那一瞬间跳
//      —— 余额就是余额，它没有"刚刚"这一说。
//
//  ★ 为什么用帧 dt 累加、而不是读墙钟：dt 由主循环给出并被 kAmbienceDtMaxSeconds 夹住，
//    所以休眠唤醒后的第一帧只推进 50 ms，不会让 R 一帧之内凉透（与速率弹簧、心跳同一
//    条纪律）。代价是"久挂之后 R 的衰减会比墙上时间慢"，那是刻意的：显示量的连续性
//    比它与墙钟的严格一致更重要。
//
//  ★ 只有"抬升"会改写 t（R_new > R(t) 才抬），所以 t 是单调不减的，不存在负时间。
//  ★ 这个成员函数现在**只是转发** StoreStepPerMinuteAt()（文件上方那个自由函数）：
//    同一件事只允许一份实现，否则迟早出现"两个地方各算一次、结果不一样"。
//    ★ 注意它量的是"最新那个点 → 现在"，**颜色不走这一条**（见文件上方那一段）。
double DisplayedAmount::LastStepPerMinute() const { return StoreStepPerMinuteAt(g_currentStepFromAt); }

void DisplayedAmount::AdvanceAmbience(double dtSeconds) {
    // ---- 0) 暂停 = 冻结 ----
    //  ★ 冻结的含义是"**不再推进**"，不是"不画"：R 的时间不走、光强也不走，于是两者
    //    都停在当前值上，颜色按当前值照常画。所以第一帧仍然要**落位**（`glowSeeded_`
    //    为假时下面第 6 步会落位）—— 否则暂停期间启动会画出一个没有蒙光的面板
    //    （实测：彩色层颜色为 (0,0,0)，32% 的黑盖在 #1b1b1c 上，整个光晕等于不存在）。
    const bool frozen = AmbienceFrozenFlag();
    if (frozen && glowSeeded_) return;

    // ---- 1) 数据这一次给出的高度：R_new = clamp(min{max(s, 2B), 0} / 2B, 0, 1) ----
    //  ★ 这里只算"高度"，不把它直接当成 R：R 由时间决定（见文件上面那一段模型说明）。
    //  ★ s 取的是**屏幕上正在显示的那一步**（主循环滞后一拍），不是存储里最新那对点：
    //    否则会出现"数字几乎没动、面板却突然变红"（所有者 2026-09-18 指出并选定 S2）。
    //    刚启动还没交付过样本时退回 `LastStepPerMinute()`（那时两种取法同值）。
    ambienceRatioTarget_ =
        SeverityRatio(AmbientStepKnown() ? AmbientStepPerMinute() : LastStepPerMinute());

    // ---- 2) R(t) = a^t：先让时间走这一步，再让刷新去抬高它 ----
    const double dt = (dtSeconds < 0.0) ? 0.0
                     : ((dtSeconds > kAmbienceDtMaxSeconds) ? kAmbienceDtMaxSeconds : dtSeconds);
    if (!frozen && ambienceClockValid_) ambienceSeconds_ += dt / 60.0;   // t 的单位是分钟
    //  ★ 抬高规则（第一帧、或长时间没有刷新之后）：只要当前高度**低于**要抬到的高度，
    //    就把时间退回去，使 R(t) 恰好等于那个高度。R_new 为 0 时"退回"没有意义（R 恒为 0），
    //    所以只在 > 0 时才动 t；于是"数据说这次不那么陡"永远不会把 R 拉下去。
    //  ★ 关闭态的两个下限（规格 §10.2）**就走这一条**：把要抬到的高度从 R_new 换成
    //    `max(R_new, R_d)`。一次 max、一次抬升 —— 与"数据刷新"是同一个式子（所有者要求的
    //    "同口径运算"因此不是另写一遍，而是同一个 max 换了个右操作数）。
    //    两次点击之后 R_d 停在那一档上，于是每一帧的 max 都把它重新抬回 R_d：
    //    下限把 R **钉住**，衰减穿不过去。
    //    （这也是"光晕与心跳同时拿到下限"的全部实现：心跳按 R 取律，而 R 只有这一个来源。）
    //  ★★ 计时器"还没开始走"时当前高度是 **0**，不是 1：t = 0 代表"刚刚剧烈消耗过"，
    //    而冷进程一次测量都还没有。少了这一条，第一个样本一到屏幕就血红（实测：R_new = 0
    //    而 R = 0.9749），所有者报的"每次点开都是红的"就是它。计时器由**第一次真的量出
    //    一步**启动 —— 那一刻它才开始代表"距离那次消耗过了多久"。
    const double currentRatio = ambienceClockValid_ ? std::pow(kAmbienceDecayA, ambienceSeconds_) : 0.0;
    const double raiseTo = std::max(ambienceRatioTarget_, ShutdownFloorRatio());
    if (raiseTo > 0.0 && raiseTo > currentRatio) {
        ambienceSeconds_ = std::log(raiseTo) / std::log(kAmbienceDecayA);
        ambienceClockValid_ = true;
    }
    // ★ 这里**没有**再写一句 `if (R < R_d) R = R_d`：上面那条条件已经保证了它 ——
    //   没抬升就说明 currentRatio >= raiseTo >= R_d。写下来就是一句永远不执行的代码，
    //   而"下限没生效"会被它掩盖成一个看起来正确的数（本项目删掉过两次这种补丁）。
    ambienceRatio_ = ambienceClockValid_ ? std::pow(kAmbienceDecayA, ambienceSeconds_) : 0.0;

    // ---- 3) D：**主币种余额**的纯函数，与屏幕上显示哪个币种无关 ----
    // ★★ 这是所有者 2026-09-19 报的那个 bug 的根：这一行原来喂的是 `value_`
    //    （屏幕上那个币种的数），于是显示 USD 时危险度被按"10 美元"这条线重算：
    //    $2.81 给出 0.72（同一天、同一笔钱在 CNY 下是 0），再低一点就满值 ——
    //    颜色、内蒙光、心跳跟着一起变。D 锚在"钱"上：同一笔钱只有一个 D，
    //    切币种只换数字与符号，阈值统一 10（kLowBalanceThresholdYuan）。
    // ★ 数的来源是 OnSample 存下的 `primaryBalance_`（主币种那一笔），**不是** value_：
    //    value_ 是显示币种的数，还跟着轮子滚。
    // ★ 主币种那一笔读不出来时取 **D = 0**：这是一个"不知道"，而不知道不该被假装成
    //    "安全"或"危险"中的哪一个。选 0 而不是 1：D 唯一的作用是**宣告低余额**
    //    （降饱和 + D = 1 的冷白光 + 让心跳变慢），而我们没有任何依据说这个账户余额低 ——
    //    凭"这一笔读不出来"去点亮一个危险信号，会让真正的危险信号贬值。
    //    这一条与"余额读不到"（那条仍是 D = 1，见 ApplyShownState 与 §7.1）不同：
    //    这里余额看得见，只是主币种那一笔没有数。
    // ★ 与"低余额有多低"的连续性无关：D 本来就是瞬时量（余额一刷新就到位）。
    ambienceDepth_ = primaryBalanceOk_ ? BalanceDepth(primaryBalance_) : 0.0;

    // ---- 4) 状态规则 ----
    //  读不到余额（数字显示 --.--）：R 与 D 都喂极端值，而不是"保留上一帧"。
    //  保留上一帧会让"网络断了"冻在血红色上 —— 那是对用户撒谎（§7.1）。
    //  ★ 读不到时 D = 1 且观感取**甲**：C 退饱和后的那道冷白光**仍在**。
    //    取"乙"（连光一起褪尽）会让面板变成一块没有任何光的死板子，
    //    而"死板子"本身就是"出事了"的信号 —— 正是 §7.1 禁止的那件事。
    //    "甲"由 DesaturateTowards 在 depth >= 1 时按 kGlowInD1Warm 保温实现；该常量现在
    //    等于 0，于是 D = 1 时得到的是纯中性灰 —— 这是代码现状，不是笔误。
    ambientUnreadable_ = !hasValue_;
    ApplyShownState();

    // ---- 5) 颜色：公式的直接输出，**没有缓动** ----
    //  平滑全部发生在 R 那一侧（R(t) 自己衰减），所以这里不需要、也不许再叠一层动画。
    //  ★ D 仍是瞬时量：余额一刷新，颜色里 D 那一半当场到位；R 那一半照样自己凉下来。
    ambienceColor_ = AmbienceTargetColor(ratioShown_, depthShown_);
    // 夹回合法范围：公式的定义域已经保证在 0..1，越界只可能来自浮点噪声。
    auto clamp01 = [](float x) { return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x); };
    ambienceColor_.r = clamp01(ambienceColor_.r);
    ambienceColor_.g = clamp01(ambienceColor_.g);
    ambienceColor_.b = clamp01(ambienceColor_.b);

    // ---- 心跳位移：放在这里是因为它要读上面刚算好的 ratioShown_/depthShown_ ----
    //  ★ 与氛围一样在冻结时"停住"，但含义略有不同：氛围是停在当前值，心跳是**时间不走**，
    //    于是位移作为一个纯函数自然保持不变（不需要额外的"记住上一帧"状态）。
    AdvanceBeat(dtSeconds);

    // ---- 6) 蒙光强度倍率 k(R,D)：与颜色分开缓动（连续解，与帧率无关）----
    //  ★ 关闭态的**亮度倍率不在这里乘**：它乘在 BuildWidgetFrame 那个两条路共用的落点上
    //    （导帧路径从不跑 Update，在这里乘的话导出来的帧亮度不变、量不到）。理由写在那边。
    const float kTarget = static_cast<float>((kGlowInK0 + kGlowInK1 * ratioShown_) *
                                             (1.0 - kGlowInD * depthShown_));
    if (!glowSeeded_ || frozen) {
        glowIntensity_ = kTarget;      // 起点落位；冻结时停在当前目标上不再缓动
        glowSeeded_ = true;
    } else if (dt > 0.0) {
        const double a = 1.0 - std::exp(-dt / static_cast<double>(kGlowInTauSeconds));
        glowIntensity_ += static_cast<float>((kTarget - glowIntensity_) * a);
    }
    if (glowIntensity_ < 0.0f) glowIntensity_ = 0.0f;
    // ★ 只夹下界（所有者 2026-09-18 的公式 max{0, ...}）：**上界不夹**。
    //   原来这里还写着 `if (> 1.0f) = 1.0f`，于是 k₀ = 1.00 时 (k₀ + k₁·R) 那一项永远
    //   加不上去 —— "剧烈时更亮"在数值上根本表达不出来。剖面峰值覆盖率只有 0.60，
    //   所以 k 在 1.0 到约 1.67 之间都还在图层通道的范围内，不需要靠夹上界来防溢出。

}

// 导帧夹具：这一帧用给定的 (R, D) 画。见头文件里的理由（验收要求四帧可复现）。
void DisplayedAmount::SetAmbienceGiven(double ratio, double depth) {
    ambienceGiven_ = true;
    ambienceGivenRatio_ = (ratio < 0.0) ? 0.0 : ((ratio > 1.0) ? 1.0 : ratio);
    ambienceGivenDepth_ = (depth < 0.0) ? 0.0 : ((depth > 1.0) ? 1.0 : depth);
}

// 每帧一次：从曲线存储重算估计，再按这一帧的 dt 走一步弹簧。
// dt 是调用方给的帧 dt（休眠唤醒后的巨型 dt 已被 Clock::Tick 钳到 50 ms；
// RateSpringStep 自己还会再钳一次，见 rate_estimator.cpp）。
void DisplayedAmount::AdvanceRate(double dtSeconds) {
    // ★ 口径：**Theil–Sen 中位斜率**（EstimateRateTheilSen），不是旧的"下降量之和 ÷ 跨度"。
    //   为什么换：旧公式把停机空档算进了分母（余额没变的那段时间没有点、没有下降量，
    //   却只把分母拉长），所有者实测 12 个点跨 137.9 分钟 -> 预测 42.4 小时。
    //   输入还是曲线存储的点，只是**换了公式**：这一轮没有新增采样文件，也没有改
    //   "只在余额变化时记点"那条规则（曲线规格 §2.1）。见 rate_estimator.h 的"哪个是哪个"。
    //   仍然是每帧重算：输入（存储的点）只在采样到达时变，但窗口滑动是**时间**的函数，
    //   所以"每帧算一次"正是让窗口跟着墙钟走的那一步（一次 7140 个点对是微秒级）。
    rateEstimate_ = EstimateRateTheilSen(RateInputForCurrency(currencyShown_));

    // 目标：只有显著（余额在掉）时才是正数；不显著 / 空 / 不消耗都以 0 为目标。
    const double target = (rateEstimate_.status == RateStatus::Significant)
                              ? rateEstimate_.rateYuanPerMinute
                              : 0.0;

    // ★ 什么时候**不做平滑、直接落位**：这三种都是"分支切换"，不是数字在动。
    //   1) 第一次算出来：弹簧还没有起点；
    //   2) 上一帧没有可用速率（显示 0）、这一帧有了：中间那几帧的极小速率换算成
    //      "归零时间"是几十万分钟，屏幕上会先闪一下「超过 7 天」才变成正常值
    //      （tau=6 s、60 Hz 下要 4 秒才收敛到 1% 以内，实测）；
    //   3) 这一帧没有可用速率：那行字要么是「—」要么是「暂无法预测」，都不是数字，
    //      平滑一个看不见的数没有意义。
    //   §7.4 的重绘规则本来就写着"分支变了 -> 一定重画"，这里是同一个道理。
    if (!rateSeeded_ || rateDisplay_ <= 0.0 || target <= 0.0) {
        rateDisplay_ = target;
        rateSeeded_ = true;
        return;
    }
    rateDisplay_ = RateSpringStep(rateDisplay_, target, dtSeconds);
}


// 右上角倒计时：模块级文本。**不做任何动画**——它就是一个每秒变一次的数字。
namespace {
std::wstring& CountdownStorage() {
    static std::wstring s;
    return s;
}
}  // namespace

void SetCountdownText(const wchar_t* text) {
    CountdownStorage() = (text ? text : L"");
}

const wchar_t* CountdownText() { return CountdownStorage().c_str(); }

std::string DisplayedAmount::TextToShow() const {
    if (!hasValue_) return "--.--";
    // 文本只由目标值决定：滚动期间冻结在目标上，落位后 value_ == target_，
    // 两种情形其实是同一个式子。这样"内容"与"竖直偏移"永远不同时变。
    // ★ 文本用**当前动画值**（不再冻结在目标上）：列数随滚动增减，于是高位列
    //   会随滚动出现/消失。字形是按每位坐标画的，文本只负责布局，所以安全。
    const double shown = value_;   // value_ 是**幅值**（动画走绝对值）
    std::string s = Amount{static_cast<AmountRaw>(std::llround(shown * kUnitsPerYuan))}.ToString2();
    // 负号来自**目标值**而不是动画值：否则滚动值穿过 0 的一瞬负号会闪。
    // 幅值本来就为 0 时不加（不出现 "-0.00"）。负号是布局字符，不参与滚动。
    if (target_ < 0.0 && s != "0.00") s.insert(s.begin(), '-');
    return s;
}


// --history-demo=N：合成 N 条样本喂进**曲线存储**（一个进程只画一帧，导帧路径里没有
// 真实历史，所以历史必须能合成）。走的是真实那条路（Sample -> FeedCurve -> Append），
// 所以"只记变化"照旧生效：值必须两两不同才会真的留下 N 个点。
//
// ★ 这 12 个值是为验收测量定的形状（不是随便一条线）：
//   * p0 = 21.00 是"追加前那 11 个点"的上极值，滚动一格后它被挤出缓冲
//     -> 旧极值 [19.90,21.00]、新极值 [19.90,20.20]，极值真的变了，纵向缓动有活干；
//   * p9 = 19.90 是**两套极值下的同一个最小值**，所以它自己的 y 从头到尾不动，
//     是量像素时可跟踪的特征；它的横坐标是 363.5 -> 332 px（在数字右侧那块
//     **没有文字压着**的区域里：数字盖住了带子中间一段，这是画法本身决定的）；
//   * 与它相邻的 p8/p10 都比它高 5 px 以上，所以"最低那一行墨"只属于它一个点；
//   * 其余各点都落在两套极值之内，没有谁会飞出带子（p0 例外：它是被删掉的那个
//     极值，它的纵向目标是"新极值之外"，按规格第 6 条本来就会飞出带子；它只在前
//     0.6 秒里还留在画面边缘，整段行程里它只动了 0.6 px）。
//   （--fixed-amount=20.12 配上它，屏幕上的数字正好等于最新那个点。）
void PrimeHistoryForDemo(int points) {
    if (points < 1) return;
    static const double kDemo[12] = {21.00, 20.20, 20.19, 20.18, 20.17, 20.16,
                                     20.15, 20.14, 20.13, 19.90, 20.10, 20.12};
    const int want = points;
    const int table = (want < 12) ? want : 12;   // <12 时取表尾那几个
    // ★ >12 时多喂的那些点是**填充**，它们的值不重要；2026-09-19（kCapacity 12 -> 120）
    //   之后它们**留在环里**（以前会被 12 格的环挤掉），所以 --history-demo=20 现在
    //   画出来的是"最新 12 个"这些话里的第 8..12 个 + 填充点 —— 面板照旧只取最新 12 个
    //   （BuildFrameCurve 入口切片），所以画面仍然只受最后 12 个点影响。
    const int prefix = want - table;
    // ★ 固定基准时刻 + 固定步长（所有者 2026-09-17 的要求）。
    //   原来是 `std::time(nullptr)`：同一个 --history-demo=N 在不同时刻产生**不同的
    //   时间戳串**，于是同一个导出命令两次跑出来的帧不逐位相同 —— 而"导帧可复现"
    //   是这个项目唯一的验证方式。现在锚点是常量、步长是常量，任何时刻都是同一串点。
    //   锚点取值只为可读（2026-09-17 00:00:00Z 附近），它不参与任何画面计算：
    //   曲线横轴是**槽位**、纵轴是极值归一化，唯一用到时间是速率估计（本串只有 1 秒
    //   跨度，本来就"不显著"）。
    //   注：这一串点比"现在"早，所以它**只能**喂进内存里的存储；任何落盘路径都必须
    //   配 --curve-store 指向夹具（生产路径的漏洞由 main 侧堵，不在这里补）。
    constexpr int64_t kDemoAnchorSec = 1789574400;
    constexpr int64_t kDemoStepSec = 1;

    for (int i = 0; i < want; ++i) {
        const double v = (i < prefix) ? (21.00 + 0.01 * i) : kDemo[12 - table + (i - prefix)];
        dshb::Sample s{};
        s.wallMs = (kDemoAnchorSec + static_cast<int64_t>(i) * kDemoStepSec) * 1000;
        s.amountsOk = true;
        s.currency = "CNY";
        s.total = dshb::Amount::FromYuanDouble(v);
        dshb::CurrencyAmount e{};
        e.currency = "CNY";
        e.total = s.total;
        e.ok = true;
        s.entries.push_back(e);
        FeedCurve(s);
    }
}

// 本帧该显示的 (R, D)：状态规则 + 两个夹具的覆盖，**只在这里**决定。
//  ★ 为什么单独抽出来：心跳的导帧夹具（--beat-frame）要把中间每一帧重放一遍，而它重放时
//    必须用**同一套** R/D 规则。上一版这里散在两处，夹具重放时读到的是夹具生效之前的
//    R/D（R=0），于是"夹具设 R=1、T=0.5 s"被常态的 15 s 钉住——实测踩过。
//    一个概念只有一个来源。
void DisplayedAmount::ApplyShownState() {
    ratioShown_ = ambientUnreadable_ ? 0.0 : ambienceRatio_;
    depthShown_ = ambientUnreadable_ ? 1.0 : ambienceDepth_;
    // ★ 关闭态的下限加在**状态规则之后、夹具之前**，两个理由都是硬的：
    //   1. 读不到余额时（§7.1 规定 R = 0）也要紧张起来 —— 关闭态是人手动进入的，
    //      与"余额读不读得到"无关；而"读不到 → R = 0"那条规则的含义是"没有剧烈消耗"，
    //      不是"没有情绪"。常态下它是 0，这里把关闭态的下限叠上去。
    //   2. 放在夹具**之前**：`--ambience=R,D` 是量光晕用的夹具，它必须仍然能把 R
    //      钉在任意值（包括 0）—— 排在它后面就会把夹具推翻，整个光晕验收作废。
    //   可读余额时这一句是**空操作**（AdvanceAmbience 的抬升已经保证 R >= R_d），
    //   所以它不是"多一道保险"，而是专门管读不到那一条路径。
    const double floor = ShutdownFloorRatio();
    if (ratioShown_ < floor) ratioShown_ = floor;
    if (ambienceGiven_) {            // 程序内夹具（DisplayedAmount::SetAmbienceGiven）
        ratioShown_ = ambienceGivenRatio_;
        depthShown_ = ambienceGivenDepth_;
    // 命令行夹具（--ambience=R,D）：跳过上面两条状态规则，只影响导出/验收。
    } else if (AmbienceOverride().given) {
        ratioShown_ = AmbienceOverride().ratio;
        depthShown_ = AmbienceOverride().depth;
    }
}

// ---------------------------------------------------------------------------
// 关闭态（设计 §10.2）：状态机 + 抖动 + 进入段
// ---------------------------------------------------------------------------
//  模型一句话：**右键进入 → 三次点击即关 → 只有「点到面板之外」能取消**。
//  ★ 0.2 起 `Esc` 不再是取消路（见头文件里那段说明）。
//
//  为什么状态机这么小却值得单独写一段注释（三条都是踩过或将要踩的）：
//   1. **没有时间窗**。第 2 击之后停 10 秒还是 10 分钟，第 3 击照样关。所以这里
//      一个时间戳都不存：存了就会有人顺手加一个"太久就清零"的判定。
//   2. **右键在关闭态里是"点击"，不是"再次进入"**。两件事都必须由**当前相位**分派，
//      写成一个就会让"右键进入 → 右键"变成"重置进度"而不是"第 1 击"。
//   3. **点满三次是单向的**：Fired 之后不再计数、不再受理输入（粒子期间）。
//      "再次触发销毁不产生第二个实例"因此不需要调用方记得先判断。
//
//  抖动与进入段是**帧号的纯函数**（下面两个自由函数），所以：
//    · 生产路径每帧给一个帧号；
//    · 导帧路径可以把帧号直接钉在第 k 帧（--shutdown-frame=k），不必连跑 k 帧；
//    · 探针能离线把同一串 k 复算一遍，与屏幕上的位移对得上。
//  这三条正是本项目唯一的验证方式。
namespace {

// 帧号的确定性噪声，落在 [-1, 1)。整数混合（splitmix64 的前三步），
//  **不引随机数、不引状态**：抖动必须是帧号的纯函数，否则同一个 k 导出两次会得到
//  两个不同的位移，"量出来的抖动幅度"里就混着随机数（那就不是证据了）。
double FrameNoise(int frame) {
    uint32_t h = static_cast<uint32_t>(frame) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return static_cast<double>(h) / 2147483647.5 - 1.0;
}

constexpr double kTwoPi = 6.283185307179586;

}  // namespace

// 进入段进度：0 = 刚进入那一刻，1 = 进入段走完（之后恒为 1）。
double ShutdownEntryProgress(int frame) {
    if (frame <= 0) return 0.0;
    if (frame >= kShutdownEntryFrames) return 1.0;
    return static_cast<double>(frame) / static_cast<double>(kShutdownEntryFrames);
}

// 抖动位移（DIP）。档位 = 已计数到第几击。
//  ★ 为什么是两个不同频率的正弦 + 逐帧噪声（所有者点名的"不要单一正弦"）：
//    单一正弦是**周期运动**，看起来像机械摆动，不像发抖。7.0 Hz 与 11.7 Hz 不可通约，
//    合成波形的重复周期很长，再叠一点逐帧噪声就是"抖"而不是"摇"。
//  ★ 为什么进入段是"从猛收到常态"（settle > 1）：这是"砸进来"在位移上的落点 ——
//    右键那一刻最猛，0.117 s 之内收到这一档的常态值。
double ShutdownJitterAt(int frame, int clicks) {
    const int tier = (clicks < 1) ? 0 : ((clicks > 3) ? 2 : clicks - 1);
    const double amp = kShutdownJitterDip[tier];
    const double freqMul = kShutdownJitterFreqMul[tier];
    // 帧 -> 秒用**心跳那根 60 Hz 标尺**（kBeatFrameHz）：位移与心跳是同一个通道，
    // 两套标尺只会让"这一帧该抖多少"在别处对不上。
    const double seconds = static_cast<double>(frame) / kBeatFrameHz * freqMul;
    const double wave = 0.62 * std::sin(kTwoPi * 7.0 * seconds) +
                        0.26 * std::sin(kTwoPi * 11.7 * seconds + 1.7);
    const double jitter = wave + 0.34 * FrameNoise(frame);
    const double u = ShutdownEntryProgress(frame);
    const double settle = 1.0 + kShutdownJitterSlam * (1.0 - u) * (1.0 - u);
    return amp * settle * jitter;
}

// 关闭态的状态：**整个挂件只有一份**，所以放模块级（不是显示层实例的成员）。
//  ★ 为什么不放 DisplayedAmount 里：R 的下限要在 AdvanceAmbience（成员函数）里读，
//    但夹具与探针要从**没有窗口、也没有实例**的地方驱动它（--shutdown-frame 是命令行
//    夹具）。模块级 + 自由函数是这一层既有的写法（AmbienceOverrideState、
//    CountdownStorage、AmbienceFrozenFlag 全都是这个形状）。
struct ShutdownState {
    ShutdownPhase phase = ShutdownPhase::Off;
    int clicks = 0;         // 已计数的点击（0..3）
    int frame = -1;         // 进入那一帧 = 0（ShutdownAdvanceFrame 每帧 +1）
    bool fixture = false;   // 导帧夹具：帧号是给进来的，不推进
};

ShutdownState& Shutdown() {
    static ShutdownState state;
    return state;
}

bool ShutdownActive() { return Shutdown().phase != ShutdownPhase::Off; }
bool ShutdownFired() { return Shutdown().phase == ShutdownPhase::Fired; }
int ShutdownClicks() { return Shutdown().clicks; }
int ShutdownFrame() { return Shutdown().frame; }

namespace {

// 进入关闭态那一刻的共同起点：clicks 归零、帧号 -1（下一次 ShutdownAdvanceFrame 立刻把它
// 推到 0 = 进入那一帧）。两条入口（窗口上右键、托盘菜单「关闭」）共用这一句，于是
// "进入段从这一刻数起"只有一处能写坏。
void ResetShutdownProgress(ShutdownState& s) {
    s.phase = ShutdownPhase::Armed;
    s.clicks = 0;
    s.frame = -1;
    s.fixture = false;
}

// 把状态钉在"已发粒子信号"。两条入口（窗口上点满第三下、托盘菜单「关闭」）共用这一句，
// 于是"Fired 时 clicks 必为 3"这条不变量只有一处能写坏 —— 它是 R 的下限（ShutdownFloorRatio）
// 与两档亮度（ShutdownGlowLevel）的输入。
void LatchShutdownFired(ShutdownState& s) {
    s.clicks = 3;
    s.phase = ShutdownPhase::Fired;
}

}  // namespace

bool ShutdownEnter(bool cancelPathReady) {
    ShutdownState& s = Shutdown();
    if (s.phase != ShutdownPhase::Off) return false;   // 已在关闭态：不重置、不重复计时
    // 没有出去的路就**不进去**（头文件里写了这条为什么在状态机里）：关闭态唯一的取消路是
    // 宿主那把全局鼠标钩子，装不上时进去只能靠三击关掉、点面板之外毫无反应。
    if (!cancelPathReady) return false;
    ResetShutdownProgress(s);
    return true;
}

bool ShutdownClick() {
    ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return false;     // 非关闭态：调用方走错了
    if (s.phase == ShutdownPhase::Fired) return false;   // 粒子期间不重复触发
    ++s.clicks;
    if (s.clicks < 3) return false;
    LatchShutdownFired(s);
    return true;                                    // 调用方据此去发"该放粒子了"的信号
}

// 托盘菜单「关闭」那一下（见头文件里它为什么不是"连调三次 ShutdownClick"的马甲）。
//  ★ 这里一个帧号都不碰：clicks/phase 之外不动任何字段，于是"进入段照跑"是结构上的 ——
//    帧号与"先右键、再点三下"一样从 -1 走到 0（进入段的抖动与两档亮度照跑），
//    粒子信号的时机也一样。为了"立刻炸"而跳过进入段会让两条入口的画面不一样。
bool ShutdownFireNow() {
    ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Fired) return false;   // 粒子期间不重复触发
    // ★ 这一条**不走 ShutdownEnter**：那一个要"取消路已备好"，而这里一步就到 Fired，
    //    Fired 期间取消本来就不受理（ShutdownCancel 那道闸）—— 取消路在这条路上没有落点，
    //    所以不该拿一个假的"已备好"去换取进入。起点字段仍与右键那条路逐位相同
    //    （同一个 ResetShutdownProgress）。
    if (s.phase == ShutdownPhase::Off) ResetShutdownProgress(s);  // 先进入：clicks=0、帧号从头数
    LatchShutdownFired(s);
    return true;
}

bool ShutdownCancel() {
    ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return false;   // 日志要能分辨"取消了"与"关窗口"
    // ★ 点满三下之后**不受理取消**：粒子已经在画，规格要的是"粒子期间不响应任何输入"。
    //   少了这一句，一次"点到别处"就能把正在播的关闭流程撤销掉 —— 而窗口已经
    //   该关了，撤销等于让一个"关到一半"的挂件留在屏幕上。closeprobe 的 case5 就是这条。
    if (s.phase == ShutdownPhase::Fired) return false;
    s.phase = ShutdownPhase::Off;
    s.clicks = 0;
    s.frame = -1;      // 进度归零：再进来是全新的一轮
    s.fixture = false;
    return true;
}

// 本帧的 R 下限：第 1 击 -> kShutdownRd1，第 2 击（含已经点满的粒子期间）-> kShutdownRd2。
// 刚进入还没点、以及非关闭态一律 0（下限还没抬起来，R 照旧交给时间自己衰减）。
double ShutdownFloorRatio() {
    const ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return 0.0;
    if (s.clicks >= 2) return kShutdownRd2;
    if (s.clicks == 1) return kShutdownRd1;
    return 0.0;
}

double ShutdownJitterDip() {
    const ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return 0.0;
    return ShutdownJitterAt(s.frame < 0 ? 0 : s.frame, s.clicks);
}

// 内蒙光的亮度倍率（乘在 k(R,D) 上）。
//  ★ 所有者 2026-09-19 要的：关闭态**只有变红**时反馈不够 —— 余额已经红着的时候，
//    右键之后除了抖动几乎看不出发生了什么。亮度是**与色相正交**的通道：不管当时面板是
//    蓝还是红，一暗就看得见。所以关闭态不是换个颜色，而是**把光压下去**。
//  ★ 两档的亮度是**各自相对原始亮度**的比例（不是逐档相乘）：
//      第 1 击 -> ×kShutdownGlowLevel1   第 2 击及以后 -> ×kShutdownGlowLevel2
//    两次点击各把光压下去一档（取值见 tuning.h 那一段），是**单向变暗**。
//  ★ 乘的位置：**只在这一处算倍率，乘法落在 `BuildWidgetFrame` 那个两条路共用的落点上**
//    （`f.ambientIntensity = amount.ambienceIntensity() * ShutdownGlowLevel()`）。
//    为什么不在这里乘进 `glowIntensity_`：导帧路径从不跑 `Update`，在这里乘的话**导出来的
//    帧亮度不变、量不到**，整套像素验收就作废了。
//    代价（已知、可接受）：亮度是**一帧到位**，不走 `glowIntensity_` 那条缓动 —— 而这与
//    "点击带来一次突变"的意图一致（要的就是"啪地暗下去"）。
double ShutdownGlowLevel() {
    const ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return 1.0;
    if (s.clicks >= 2) return kShutdownGlowLevel2;
    if (s.clicks == 1) return kShutdownGlowLevel1;
    return 1.0;   // 刚进入还没点：亮度不动，反馈只在点击之后出现
}

bool ShutdownCurveLayerVisible() {
    return !ShutdownActive() || ShutdownFrame() < kShutdownEntryFrames;
}

void ShutdownAdvanceFrame() {
    ShutdownState& s = Shutdown();
    if (s.phase == ShutdownPhase::Off) return;
    if (s.fixture) return;   // 导帧夹具：帧号是给进来的，推进就把它推跑了
    ++s.frame;
}

// 导帧夹具：把关闭态直接钉在"已点 N 下、进入以来第 k 帧"。见头文件里的说明。
void SetShutdownFixture(int clicks, int frame) {
    ShutdownState& s = Shutdown();
    const int c = (clicks < 1) ? 1 : ((clicks > 3) ? 3 : clicks);
    s.clicks = c;
    s.phase = (c >= 3) ? ShutdownPhase::Fired : ShutdownPhase::Armed;
    s.frame = (frame < 0) ? 0 : frame;
    s.fixture = true;
}

// --beat-frame=k：把心跳的仿真时刻放到 k/60 秒，并按计时器重新走一遍（导出夹具）。
//  ★ 为什么是"重新走一遍"而不是"直接摆到那一帧"：位移不只取决于时刻，还取决于**这一拍
//    是什么时候触发的**，而那由"每 T 秒触发一次"决定。照同一条规则把触发次数数出来，
//    导出的才是真的"第 k 帧"。
//  ★ 它只重置状态，然后**逐帧走一遍**（不自己算位移）：位移与触发都由 `AdvanceBeat`
//    一个入口负责（生产路径同一个入口）。若这里另写一遍触发/求值，就有两个地方各算
//    同一个东西——本项目的"一个概念两个来源"已经犯过三次。
//  ★ 为什么不能只把 `beatSimSeconds_` 一摆就走：计时器是**有状态**的，"现在这一拍是哪
//    一刻触发的"取决于中间每一帧。直接摆过去会让 90 帧只跳一次（实测：夹具写 T=0.5 s，
//    屏幕上却像 15 s 的节拍），导出出来的不是"第 k 帧"。
//  ★ 第一拍必须由第 0 帧那次 `AdvanceBeat` 触发（所以这里清 `beatSeeded_`）：窗口刚建好
//    时 `--ambience=R,D` 还没生效，那时候采样的周期是**常态的 15 s**；夹具设了 R=1
//    （T=0.5 s）而第一拍被 15 s 钉住，整套夹具就白设了（实测踩过）。
void DisplayedAmount::SetBeatSimFrame(int frame) {
    ApplyShownState();               // 先按同一套规则定下这一段的 (R, D)
    beatSimSeconds_ = 0.0;
    beatBucket_ = BeatBucket{};
    beatCount_ = 0;
    beatSeeded_ = false;
    const int frames = (frame > 0) ? frame : 0;
    for (int i = 0; i <= frames; ++i) {
        // ★ 每步用 `beatSimSeconds_ += dt` 累加，而不是把 `i/60` 直接赋给它：浮点累加的
        //   次序必须与生产路径**逐位相同**，否则卡在整数边界的那一帧会数出不同的拍数
        //   （实测：`i/60` 与 `+= 1/60` 在同一位置差整整一拍）。
        AdvanceBeat((i == 0) ? 0.0 : 1.0 / kBeatFrameHz);
    }
}

// 每帧推进心跳位移。
//  ★ 冻结（暂停）时**不推进时间**：位移是 (时间, 这一拍) 的纯函数，时间不走位移就不变，
//    这比"每帧记住一个值再锁住"更难写错。
//  ★ 计时就是你给的那一条：**due = now + T，now >= due 就跳一次**。T 每帧重算，所以
//    R 一变短（余额突然掉一截）下一拍**立刻**被拉近——旧写法是"整拍采样、不变"，
//    于是变红之后要等满旧拍才快起来（所有者实测到的问题）。
//  ★ 已经跳完的那一拍不受影响：触发时刻只在真的跳了那一下才被改写，due 只往前拉不往后推
//    （T 变大时 due 变远，那一拍就等久一点——这也是要的：不剧烈了就慢下来）。
//  ★ 第一拍锚在**第一次调用的时候**（而不是"仿真时刻 0"）：调用它的那一刻 R/D 才是
//    有意义的——`--ambience=R,D` 这类夹具在窗口建好之后才生效，若第一拍锚在 0、用夹具
//    生效前的 R/D（R=0 ⇒ 周期 15 s）算，夹具就算白设了（这个坑真的踩过一次）。
void DisplayedAmount::AdvanceBeat(double dtSeconds) {
    if (!AmbienceFrozenFlag()) beatSimSeconds_ += dtSeconds;
    if (!beatSeeded_) {
        beatSeeded_ = true;
        TriggerBeatAt(beatSimSeconds_);      // 第一帧就跳一次，而不是先干等一个周期
        ++beatCount_;
    } else {
        // ★ 记的是"下一次应当跳动的时刻"，但**不把它存下来**：due = 上次触发 + 当前的 T。
        //   T 变短就把 due 拉近 —— 变红之后下一拍立刻提前（这正是这次修订要的）。
        //   ★ 因果别搞反：把 due 真的存下来、每帧写成 `now + T`，`now >= due` 就变成
        //     "一帧要跨过 T"，而 T ≥ kBeatTMin = 0.5 s、一帧只有 1/60 s —— 第一拍之后再
        //     也不跳（实测踩过，探针 case5e 专门钉这条）。所以 due 必须锚在**上次触发**上。
        const double due = beatBucket_.triggerSeconds +
                           BeatPeriodSeconds(ratioShown_, depthShown_);
        if (beatSimSeconds_ >= due) {
            TriggerBeatAt(beatSimSeconds_);
            ++beatCount_;
        }
    }
    beatOffsetDip_ = BeatOffsetFromBucket(beatSimSeconds_, beatBucket_);
}

// 触发一拍：记下触发时刻、采样这一拍的幅度与它的周期 T（t_j 每帧由这两者现算，不存 t_j）。
void DisplayedAmount::TriggerBeatAt(double atSeconds) {
    beatBucket_.triggerSeconds = atSeconds;
    beatBucket_.amplitudePx = BeatAmplitudePx(ratioShown_, depthShown_);
    beatBucket_.dueSeconds = BeatPeriodSeconds(ratioShown_, depthShown_);
    if (g_beatTrace) {
        std::fprintf(stderr, "[trace] trigger t=%.6f T=%.6f due_at=%.6f amp=%.6f R=%.6f D=%.6f\n",
                     atSeconds, beatBucket_.dueSeconds,
                     beatBucket_.triggerSeconds + beatBucket_.dueSeconds, beatBucket_.amplitudePx,
                     ratioShown_, depthShown_);
    }
}

void SetCurveScrollFrame(int frame) {
    g_curveFrozen = true;
    g_curveSeconds = (frame > 0) ? (static_cast<double>(frame) / kCurveFrameHz) : 0.0;
}

// 一行诊断给 main 记日志（这一层自己不写文件）。
std::string CurveStateLine() {
    std::string values;
    const std::vector<CurveStorePoint> points = g_curveStore.Points();
    std::vector<double> v;
    if (CurveValues(points, &v)) {
        char one[24];
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::snprintf(one, sizeof(one), "%s%.2f", (i == 0) ? "" : ",", v[i]);
            values += one;
        }
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "points=%zu lastUpdate=%lld scrollFrame=%.0f progress=%.4f scrolling=%s frozen=%s old=[%.2f,%.2f] values=[%s]",
                  points.size(), static_cast<long long>(g_curveStore.lastUpdate()),
                  g_curveSeconds * kCurveFrameHz, g_curveSeconds / kCurveScrollSeconds,
                  g_curveScrolling ? "yes" : "no", g_curveFrozen ? "yes" : "no", g_curveOldLo,
                  g_curveOldHi, values.c_str());
    return buf;
}


// 记录文件路径（函数内 static，避免模块级变量与匿名命名空间的可见性纠缠）
static std::string s_path;   // 记录文件路径（UTF-8）；空 = 不落盘

const std::string& dshb::CurveStorePath() { return s_path; }


void dshb::SetCurveStorePath(const std::wstring& path) {
    s_path.clear();
    if (path.empty()) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string utf8(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    s_path = utf8;
    // 启动即加载：加载失败/文件不存在/数据过期都由数据层自己决定（规格 §2.4 / 验收 8）
    (void)g_curveStore.Load(s_path);
}

// ---------------------------------------------------------------------------
// "今日已 X.XX¥"（所有者 2026-09-19）—— 口径与两个边界都在 widget_display.h 里
// ---------------------------------------------------------------------------
//  ★ 这里只说两件代码里看不出来的事：
//
//  1) **基线**（"今天零点那一刻的余额"）取的是"今天之前最后一个有时间的点"，不是
//     存储里假装补一个零点。理由：存储只记变化（规格 §2.1），零点那一刻**通常没有点**，
//     余额没动的时候更是完全没有；而"只记变化"是这条曲线的核心，为了这一行去追写
//     一个零点采样会让那条规矩破洞。所以：
//       · 有今天之前的点  -> 基线取它（余额在零点前后没动时，它逐位等于零点那一刻）；
//       · 没有（首次运行 / 应用是在零点之后才启动 / 存储被 86400 秒规则清过）-> **不**
//         编一个起点，直接从今天第一个可用点开始量。代价写在这里：零点到第一次变化
//         之间那段消耗会漏掉，而漏掉多少**没有任何可用的数据能说明**（那一段没有点）。
//         这是**选项 B**：所有者列的 A（内存里记跨午夜那一刻的余额）能补上这一段，
//         代价是多一份状态，且只在"应用一直开着跨过零点"时才有用。
//  2) 上涨**不抵消**：`if (after < before)` 才加。所以充值那一步只贡献 0 —— 累加值
//     只增不减，永远非负（所有者选的规则）。
// 这一层眼里的"现在"（epoch 秒）。0 = 没设过夹具 -> 真实墙钟。
// ★ 时钟夹具（SetTodayUsageNowForProbe）只为探针存在：这一行的结果依赖"今天"，
//   一个按真实时钟跑的命令明天会给出另一个数字，而"导帧可复现"是本项目唯一的验证方式。
static int64_t g_todayNowOverride = 0;

int64_t TodayUsageNowSeconds() {
    if (g_todayNowOverride > 0) return g_todayNowOverride;
    return static_cast<int64_t>(std::time(nullptr));
}

void SetTodayUsageNowForProbe(int64_t nowSeconds) { g_todayNowOverride = nowSeconds; }

int64_t TodayStartSeconds(int64_t nowSeconds) {
    const std::time_t t = static_cast<std::time_t>(nowSeconds);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return nowSeconds;   // 读不出本地日期：不退，当"今天刚开始"
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;   // 让 CRT 按那个日期的本地时区规则重算（夏令时地区才看得出差别）
    const std::time_t start = std::mktime(&local);
    if (start == static_cast<std::time_t>(-1)) return nowSeconds;
    return static_cast<int64_t>(start);
}

TodayUsage TodayUsageFromStore(const std::string& currency) {
    TodayUsage usage;
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    const int64_t todayStart = TodayStartSeconds(TodayUsageNowSeconds());

    // 基线：今天之前**最后一个**有时间、有金额的点。找不到就是"零点到第一次变化之间
    // 没有办法量的那一段"（见上面 1）。
    bool haveBaseline = false;
    AmountRaw baselineRaw = 0;
    for (const CurveStorePoint& point : points) {
        if (!point.atValid || point.at >= todayStart) break;
        Amount amount;
        if (CurveValueOfEntryIn(point, currency, &amount)) {
            baselineRaw = amount.raw;
            haveBaseline = true;
        }
    }

    // 今天：`beforeRaw` 是"上一个可用余额"，它可能来自基线（零点之前）或者今天更早的点。
    // 上一个可用点就是基线时，今天的**第一个**点相对它那一步照样被累计 —— 那一步是
    // 真实的下降。若基线正好等于今天第一个点的余额，那一步贡献 0（余额没动）。
    bool haveBefore = haveBaseline;
    AmountRaw beforeRaw = baselineRaw;
    bool anyToday = false;
    for (const CurveStorePoint& point : points) {
        if (!point.atValid || point.at < todayStart) continue;
        Amount amount;
        if (!CurveValueOfEntryIn(point, currency, &amount)) continue;   // 这个点没有可用金额
        anyToday = true;
        if (haveBefore && amount.raw < beforeRaw) usage.raw += beforeRaw - amount.raw;
        beforeRaw = amount.raw;
        haveBefore = true;
    }
    // 今天一个可用的点都没有 -> 算不出来（显示 --.--），不是 0。
    usage.ok = anyToday;
    return usage;
}

// ---- 时钟夹具（panelprobe 的 case5）----
//  0 = 没设过 -> 用真实墙钟。"现在"只在这一行的计算里用一次，就是上面那次调用。

std::string TodayUsageText() {
    // 关闭态里标题、倒计时、数字都由 BuildWidgetFrame 收起来了；这一行没有理由还留着
    // （它和那几个是同一行上的东西，只留一个字串在屏幕上是"关闭态里还留着某某东西"）。
    if (ShutdownActive()) return std::string();
    const TodayUsage usage = TodayUsageFromStore(g_curveStore.lastPrimaryCurrency());
    // 算不出来 -> 所有者原话里那个形状（--.--），不是 0.00。两者的区别是
    // "算出来是零"与"算不出来"，与余额那一行 0.00 / --.-- 同一条规矩。
    // ★ ToString2 给的是 ASCII（只有数字、'-'、'.'），所以这一个字一个字地转就够，
    //   不需要再走一次多字节转换（那正是"图省事的转换在中文上出错"的那个口子）。
    std::wstring amount;
    if (usage.ok) {
        for (char c : Amount{usage.raw}.ToString2()) amount.push_back(static_cast<wchar_t>(c));
    } else {
        amount = L"--.--";
    }
    return Utf8FromWide(L"今日已 " + amount + L"\u00A5");
}

WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol) {
    WidgetFrame f{};
    f.state = state;
    f.statusText = StatusTextFor(state);
    f.countdownText = CountdownText();
    // "今日已 X.XX¥"（所有者 2026-09-19）：标题那一行后面的灰色小字。
    // ★ 它与状态无关，只与曲线存储和"现在"有关 —— 所以状态是"读不到余额"时它照样在
    //   （今天花了多少与"这一秒能不能读到余额"是两件事）。关闭态是唯一例外，见
    //   TodayUsageText 的第一行。
    f.todayUsageText = TodayUsageText();

    // ★ 这里就是设计要防的第一个错：**"查不到"和"余额为 0"必须分开**。
    //   只有拿到了真实数值才显示数字；读不到时显示占位符，绝不显示 0.00。
    const bool haveNumber = amount.hasValue() && currencyKnown;
    f.showAmount = haveNumber;
    if (haveNumber) {
        f.amountText = amount.TextToShow();
    } else {
        f.amountText = "--.--";                // 占位符，不是 0.00
    }
    // 氛围曲线（规格 §3）：点由**显示层**算好（横向位置 + 纵向缓动都算完），渲染层
    // 只负责连线。规则（都在本文件上方的 BuildFrameCurve 里）：
    //   * 显示集 = 记录里最新 11 个点铺满恰好 10 段（左侧不足时按最早的点拉平）
    //   * 只有**追加了一个点**才滚动：整条在 10 秒内匀速左移一格，新点从 x=1.1 进来
    //   * 纵向按 P = L + (N − L)(1 − rate^k)^c 缓动，极值仍铺满整条带子
    //   * 值不变 -> 没有新点 -> 曲线静止（规格 §2.1 的推论，故意的）
    //   * 没有数据 / 只有一个点 / 全都一样 -> 平线（curveHasData = false）
    BuildFrameCurve(&f);

    // ---- 氛围蒙光（"E 蒙光.md" §3、§6）----
    // ★ 渲染层只拿到两样东西：**颜色**（已逐通道缓动）与**强度倍率 k**。
    //   它不知道余额、G、基线、R、D —— 那一整套留在显示层（和"余额为 0 与
    //   查不到有什么区别"同一条分工）。剖面的形状属于渲染层（tuning.h 5.4）。
    //   ★ 读不到余额时 R/D 的取值、以及"取甲不取乙"，都已经在 AdvanceAmbience 里
    //     按状态规则算完，这里只是把它搬进帧。
    f.ambientColor = amount.ambienceColor();
    // ★ 关闭态的**亮度倍率在这里乘，且只在这里乘一次**（这是导帧与实时两条路共用的落点）。
    //   为什么不在 AdvanceAmbience 里乘：导帧路径**从不跑 Update()**（它只跑
    //   BuildWidgetFrame 然后画一帧），在那边乘的话导出来的帧亮度不会变，等于量不到；
    //   而两处都乘会变成平方。所以唯一的落点就是这里。
    //   ★ 代价（如实记着）：实时的亮度**没有缓动**了 —— 它随档位一帧到位。若以后要让它
    //     滑过去，得把倍率挪回 AdvanceAmbience 并给导帧路径单独补一次，那时要小心别乘两遍。
    f.ambientIntensity = amount.ambienceIntensity() * static_cast<float>(ShutdownGlowLevel());
    f.beatOffsetDip = static_cast<float>(amount.beatOffsetDip());

    // ★ 符号与数字**分开决定**（所有者）：只要币种是确定的，即使没有数字也要显示符号，
    //   否则余额读不出来、界面只剩 --.-- 的时候看不出自己在看哪个币种。
    f.currencySymbol = currencyKnown ? (currencySymbol ? currencySymbol : L"") : L"";

    // 每一位的纵坐标交给渲染层。空则渲染层退回整串绘制。
    f.places = amount.places();

    // 清零预估（C9）：**接上了**估计器（原来这里是一句占位文案「按当前速度，约 X
    // 小时后归零」，所有者等了它三轮）。
    //
    // 口径：
    //   * 速率 = 这一层每帧从曲线存储重算、并用弹簧平滑过的那个值（amount.rateDisplay()），
    //     状态来自估计器本身（Significant / NotConsuming / Insignificant / Empty）。
    //   * 余额用**目标值**（amount.target()）而不是动画值：动画途中穿过 0 不该让这行字闪。
    //   * 时间用当前墙钟：§7.4 要求"剩余 ≤ 6 小时时补一个绝对时刻"，那需要本地时钟。
    //     估计器自己是纯函数（没有时钟），时钟只在文案这一层用。
    //   * 五个分支与中文逐字由 ZeroTimeFor 决定（暂无法预测 / — / 已用尽 / 超过 7 天 /
    //     按当前速度，约 …后归零），这里不改写一个字。
    //
    // ★ 门（所有者 2026-09-16 改）：**只有欠款（余额为负）才藏这一行**。
    //   原来写的是 `amount.target() > 0.0`，那会把「已用尽」（余额恰好 0）和
    //   「—」（不消耗）一起藏掉——而这两条正是估计器算出来的结论，藏了就等于
    //   余额归零时那行字凭空消失。
    const bool canEstimate = haveNumber && amount.target() >= 0.0;
    if (canEstimate) {
        RateEstimate smoothed = amount.rateEstimate();
        smoothed.rateYuanPerMinute = amount.rateDisplay();
        const int64_t balanceRaw = static_cast<int64_t>(std::llround(amount.target() * kUnitsPerYuan));
        const ZeroTimeText line = ZeroTimeFor(smoothed, balanceRaw, static_cast<int64_t>(std::time(nullptr)));
        f.zeroTimeText = Utf8FromWide(line.text);   // UTF-16 -> UTF-8（渲染层自己再 Widen 回去）
    }

    // ★ 逐位里程表：把"这一帧的连续金额"交给渲染层，**任何时刻都要给**。
    //
    //   为什么不再用 rolling() 做条件：轮子现在不只在滚动时用，它**就是**画数字的
    //   唯一路径（滚动结束不再切到另一条静止路径，那正是"结束时跳一行"的来源）。
    //   所以未滚动时也必须给值，否则轮子按 0 算、画面上会变成 00.00。
    //   轮子自然停在整行上，与静止状态逐像素一致。
    //
    // ---- 关闭态（设计 §10.2）：只留"关闭"二字 ----
    //  ★ 为什么收口放在**最后一行**而不是开头：上面那些字段是一个一个赋上去的
    //    （币种符号、places、清零预估都在 BuildFrameCurve 之后才写），在开头清会被后面
    //    重新填回来。那种 bug 在屏幕上就是"关闭态里还留着某某东西"，而每一处单独看都对。
    //  ★ 两个字放 `amountText`：金额那条绘制路径本来就**按墨迹居中**（数字与"关闭"走
    //    同一段代码），所以渲染层一行都不用改 —— 它看到的仍然只是"这一帧要显示的文字"。
    if (ShutdownActive()) {
        f.statusText = L"";
        f.countdownText = L"";
        f.showAmount = false;
        f.currencySymbol = L"";
        f.zeroTimeText.clear();
        f.places.clear();
        f.amountText = "关闭";
        const double entry = ShutdownEntryProgress(ShutdownFrame());
        if (entry < 1.0) {
            // 进入段：曲线**绕实体区中心向内收缩**（规格：旧内容不是淡出，是被抽走）。
            //  它是唯一可收缩的元素：点列的横纵坐标都是显示层的归一化值。文字的坐标、
            //  字号、alpha 都是渲染层的常量（那正是"变大到位/呼吸/边缘散"做不了的地方，
            //  理由与需要的接缝写在 widget_display.h 的关闭态那一段）。
            const double shrink = 1.0 - entry;
            for (CurvePoint& p : f.curve) {
                p.x = static_cast<float>(0.5 + (static_cast<double>(p.x) - 0.5) * shrink);
                p.y = static_cast<float>(0.5 + (static_cast<double>(p.y) - 0.5) * shrink);
            }
        } else {
            f.curve.clear();
            f.curveHasData = false;
        }
    }
    // 抖动与心跳**相加**送进同一个位移通道（§9.4：关闭态里心跳不停）。
    // 非关闭态这一项恰好是 0（ShutdownJitterDip 在 Off 相位返回 0.0）。
    f.beatOffsetDip += static_cast<float>(ShutdownJitterDip());
    return f;
}

const wchar_t* StatusTextFor(ConnState state) {
    switch (state) {
    case ConnState::ColdStart: return L"正在读取";
    case ConnState::Ok: return L"deepseek 余额";
    case ConnState::NoKey: return L"未找到 DEEPSEEK_API_KEY";
    case ConnState::AuthFailed: return L"API Key 无效";
    case ConnState::Exhausted: return L"余额已耗尽，请充值";
    case ConnState::RateLimited: return L"请求过于频繁";
    case ConnState::NetworkError: return L"无法连接";
    case ConnState::Stale: return L"数据已过期";
    case ConnState::Unavailable: return L"账户不可用";
    default: return L"deepseek 余额";
    }
}


// ---------------------------------------------------------------------------
// 命令行接线（--ambience=R,D / --no-text / --pause-ambience）—— 见头文件的说明
// ---------------------------------------------------------------------------
//  ★ 状态存放处（AmbienceOverride / TextLayerEnabled / AmbienceFrozenFlag）定义在
//    本文件开头的匿名 namespace 里：DisplayedAmount::AdvanceAmbience 与渲染层都要
//    读它们，而真正的命令行入口就是下面这三个函数。
bool SetAmbienceGiven(const char* text) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const double first = std::strtod(text, &end);
    if (end == text) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != ',' && *end != ' ') return false;   // "R,D"（也容忍空格分隔）
    ++end;
    const char* second = end;
    const double value = std::strtod(second, &end);
    if (end == second) return false;
    AmbienceOverride().given = true;
    AmbienceOverride().ratio = (first < 0.0) ? 0.0 : ((first > 1.0) ? 1.0 : first);
    AmbienceOverride().depth = (value < 0.0) ? 0.0 : ((value > 1.0) ? 1.0 : value);
    return true;
}

void SetTextEnabled(bool on) { TextLayerEnabled() = on; }
bool TextEnabled() { return TextLayerEnabled(); }

void SetAmbienceFrozen(bool on) { AmbienceFrozenFlag() = on; }
bool AmbienceFrozen() { return AmbienceFrozenFlag(); }

void SetBeatTrace(bool on) { g_beatTrace = on; }

// --ambience-glide=N：跑 N 帧真实的氛围推进。见头文件里的理由（量重烘代价）。
// ★ 轨迹：前 25% 帧 R 从 0 爬到 1（余额刚掉一截），余下 75% 帧 R 从 1 落回 0
//   （不再剧烈变化，颜色自己慢慢回到常态）。D 固定 0。
// ★ 每帧 1/60 秒、用**真实**的 Update 路径推进：所以量到的是生产里那条代码，
//   不是另写一套模拟。冻结标志在这里同样生效（于是它也能用来验证暂停）。
void RunAmbienceGlide(int frames) {
    if (frames < 1) return;
    if (!g_ambienceGlideTarget) return;   // main 没挂实例 = 这一层不猜、不建第二个
    const int rampUp = (frames + 3) / 4;
    for (int i = 0; i < frames; ++i) {
        const double r = (i < rampUp)
                             ? (static_cast<double>(i) / static_cast<double>(rampUp))
                             : (1.0 - static_cast<double>(i - rampUp) /
                                            static_cast<double>(frames - rampUp > 0
                                                                    ? frames - rampUp
                                                                    : 1));
        AmbienceOverride().given = true;
        AmbienceOverride().ratio = (r < 0.0) ? 0.0 : ((r > 1.0) ? 1.0 : r);
        AmbienceOverride().depth = 0.0;
        g_ambienceGlideTarget->Update(1.0 / 60.0);
    }
    // 滑行结束后把夹具撤掉：后续帧回到"用真实算出来的 R/D"，与生产一致。
    AmbienceOverride().given = false;
}

DisplayedAmount* g_ambienceGlideTarget = nullptr;

}  // namespace dshb

