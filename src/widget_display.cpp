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
// 余额 -> 氛围颜色（"E 蒙光.md" §3）：D 用**当时**的余额算
// ---------------------------------------------------------------------------
//  ★ 为什么"存点的颜色"在这里自己算一次、而不是直接读 ambienceColor_：
//    接口**只有余额**，而 D 是余额的纯函数，所以"那一刻的颜色"可以精确复原 ——
//    存点时把余额喂进来就得到那一刻的颜色，不依赖任何渲染状态。
//    R 那一半是"当时刚刚有多陡"，它不在余额里，所以这里取 R = 0；
//    也就是说存下来的颜色是"D 的精确值 + R 取常态"。这个取舍写进了报告。
//  ★ 声明必须在使用它的 FeedCurve 之前（C++ 的名字要先声明后使用）。
AmbienceColor BalanceColorAt(double balanceYuan) {
    const double depth = BalanceDepth((balanceYuan < 0.0) ? 0.0 : balanceYuan);
    return AmbienceTargetColor(0.0, depth);
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
constexpr std::size_t kCurveDisplayPoints = 11;
constexpr std::size_t kCurveSegments = kCurveDisplayPoints - 1;

// 一个点对曲线的取值 = 这个点的**第一个可用条目**。
// ★ 为什么不是"当前显示的币种"：数据层判定"变没变"用的就是响应第一个条目
//   （primary）——一个点之所以存在，正是因为那个币种变了。让它与点一一对应，
//   曲线画的就是"数据层记下的那条序列"，与用户此刻点了哪个币种符号无关。
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
    // ★ 同时把这个点**自己那一刻**的氛围颜色存下去（"E 蒙光.md" §3.1）：
    //   颜色由余额的纯函数给出（BalanceColorAt），所以"那一刻的颜色"是可复原的
    //   —— 存下来的不是"当前渲染状态"，而是"这条数据在那一刻对应的颜色"。
    g_curveStore.Append(obs, s.wallMs / 1000, HexOf(BalanceColorAt(s.total.ToDouble())));

    // ★ 判定"追加了一个点"看的是**存储自己的状态**，不是 Append() 返回值的含义：
    //   点数变了，或者环里的内容变了（容量 12 到顶时 size() 不动，但最老的点会被挤掉）。
    //   只比 size() 会在环满之后永远看不到新点（那时曲线会在生产里停住不滚）。
    const std::vector<CurveStorePoint> after = g_curveStore.Points();
    const bool appended = !SamePoints(before, after);
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

    const std::vector<CurveStorePoint> points = g_curveStore.Points();
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
    //   滚动：环里全部点（最多 12 个），槽位再右移 0.1×(1−进度)——于是"第 12 个"
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
    //   渲染层会用当前 C 画它左边那一段。给它硬套一个颜色等于发明一个测量值。
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
            // 还没选过币种（第一次样本之前）：退回这个点的第一个有值的条目，
            // 与曲线取值的口径一致。这不是"随便挑一个"——它就是数据层记这个点时
            // 用的那个币种（primary）。
            for (const CurveStorePoint::Entry& e : point.entries) {
                if (!e.missing && !e.text.empty()) { entry = &e; break; }
            }
        } else {
            // ★ 有选中的币种时**不回退**：这个点没有该币种就是"这个点没有可用金额"。
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

// 连续取不到值：回到"没有值"的状态（界面显示 --.--），但**历史不丢**：
// 下一次成功样本会重新落位。所有者：先当作没变，连续 5 次才显示 --.--。
void DisplayedAmount::MarkUnreadable() {
    hasValue_ = false;
    trips_.clear();
    places_.clear();
    animating_ = false;
    tripsDirty_ = true;
}

// 用户点了币种符号：换成清单里的下一个。
// 记住的是**名字**，所以下一次样本里顺序变了也切得对。
void DisplayedAmount::SelectCurrency(const std::string& code) {
    selectedCurrency_ = code;
    currencyShown_ = code;

    // ★ 立刻换掉**实际数字**，不等下一次样本。
    //   理由（实测踩过）：真实账户 10 秒才一个样本、夹具根本不发新样本，
    //   等下去符号和数字都不会动；而且符号只在这里更新才跟得上。
    //   金额取自最近一次样本里的同币种条目，按"新目标"处理 -> 轮子滚过去并停住。
    for (const CurrencyAmount& e : lastEntries_) {
        if (!e.ok || e.currency != code) continue;
        const double yuan = e.total.ToDouble();
        latest_ = yuan;
        if (e.total.raw != 0) {
            zeroPending_ = false;
            zeroConfirmed_ = false;
        }
        rollFromValue_ = hasValue_ ? value_ : yuan;
        lastReal_ = hasValue_ ? target_ : yuan;
        target_ = yuan;          // R：新的实际数字
        frames_ = 0;
        animating_ = true;
        tripsDirty_ = true;
        if (!hasValue_) {
            value_ = yuan;
            hasValue_ = true;
        }
        lastSwitchTarget_ = yuan;    // 回传给 main.cpp 记日志（这一层不能写日志）
        return;
    }
    // ★ 这个币种在最近的样本里**没有数据**：显示 --.--，但符号仍然换成它的
    //   （所有者：没有数据就显示 --.--，符号要变——否则看不出自己在看哪个币种）。
    hasValue_ = false;
    trips_.clear();
    places_.clear();
    animating_ = false;
    tripsDirty_ = true;
    lastSwitchTarget_ = -1.0;
}

std::string DisplayedAmount::NextCurrency() const {
    if (availableCurrencies_.size() < 2) return std::string();
    std::string current = selectedCurrency_.empty() ? currencyShown_ : selectedCurrency_;
    for (size_t i = 0; i < availableCurrencies_.size(); ++i) {
        if (availableCurrencies_[i] == current) {
            return availableCurrencies_[(i + 1) % availableCurrencies_.size()];
        }
    }
    return availableCurrencies_[0];
}

void DisplayedAmount::OnSample(const Sample& s) {
    if (!s.amountsOk) return;                 // 读不到的样本不参与显示
    lastSampleWallMs_ = s.wallMs;   // 曲线横轴锚点（见 widget_display.h）

    // ---- 币种选择 ----
    // 清单来自样本；没选中（或选中的这个币种这次没出现）就用接口给的优先条目。
    // 按名字找而不是按下标：接口不保证数组顺序（设计 §2.2）。
    lastEntries_ = s.entries;   // 留着给"点符号切换"用
    availableCurrencies_.clear();
    for (const CurrencyAmount& e : s.entries) {
        if (e.ok) availableCurrencies_.push_back(e.currency);
    }
    // ★ 可切换的币种 = 本次样本里**有数据的** + 我们**知道怎么显示的**（CNY/USD）。
    //   所有者：切到没有数据的币种就显示 --.--，但符号要变。
    //   所以即使账户只有 CNY，也必须能切到 USD（显示 --.-- 加 $）——
    //   否则这个功能在单币种账户上根本看不出效果。
    for (const char* known : {"CNY", "USD"}) {
        bool has = false;
        for (const std::string& c : availableCurrencies_) {
            if (c == known) { has = true; break; }
        }
        if (!has) availableCurrencies_.push_back(known);
    }
    Amount picked = s.total;
    std::string pickedCode = s.currency;
    if (!s.entries.empty()) {
        const CurrencyAmount* hit = nullptr;
        if (!selectedCurrency_.empty()) {
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok && e.currency == selectedCurrency_) { hit = &e; break; }
            }
        }
        if (!hit) {
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok && e.currency == s.currency) { hit = &e; break; }
            }
        }
        if (!hit) {
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
    AdvanceCurve(dtSeconds);
    AdvanceRate(dtSeconds);       // 消耗速率（§7.3）：每帧重算 + 走一步弹簧
    const double shown = UpdateValue(dtSeconds);
    AdvancePlaces(dtSeconds, TextToShow());
    // 氛围放在**显示值之后**：D 用的是"这一刻屏幕上那个数字"，而文字/轮子
    // 刚刚在本帧落位；先算氛围会用上一帧的余额，D 就慢半拍。
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
//                  D = clamp( (G − 余额) / G, 0, 1 )                        ← 瞬时量
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
double DisplayedAmount::LastStepPerMinute() const {
    const std::vector<CurveStorePoint> points = g_curveStore.Points();   // 旧 -> 新
    if (points.size() < 2) return 0.0;

    // 从最新往回找一对"两个点都有值、都有时间、时间递增"的相邻点。
    // 中间夹着没有时间的老点（老文件）是常态，所以这里要往后走而不是直接放弃。
    for (std::size_t i = points.size(); i-- > 1;) {
        const CurveStorePoint& cur = points[i];
        const CurveStorePoint& prev = points[i - 1];
        if (!cur.atValid || !prev.atValid) continue;
        const double dt = static_cast<double>(cur.at - prev.at);
        if (!(dt > 0.0)) continue;

        Amount curAmount;
        Amount prevAmount;
        if (!CurveValueOfEntry(cur, &curAmount)) continue;
        if (!CurveValueOfEntry(prev, &prevAmount)) continue;
        return StepPerMinute(prevAmount.ToDouble(), curAmount.ToDouble(), dt);
    }
    return 0.0;   // 没有可用的一步 = R = 0（"算不出来"，不是"在剧烈消耗"）
}

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
    ambienceRatioTarget_ = SeverityRatio(LastStepPerMinute());

    // ---- 2) R(t) = a^t：先让时间走这一步，再让刷新去抬高它 ----
    const double dt = (dtSeconds < 0.0) ? 0.0
                     : ((dtSeconds > kAmbienceDtMaxSeconds) ? kAmbienceDtMaxSeconds : dtSeconds);
    if (!frozen) ambienceSeconds_ += dt / 60.0;   // t 的单位是分钟
    //  ★ 抬高规则（第一帧、或长时间没有刷新之后）：只要当前高度**低于**数据给的高度，
    //    就把时间退回去，使 R(t) 恰好等于 R_new。R_new 为 0 时"退回"没有意义（R 恒为 0），
    //    所以只在 R_new > 0 时才动 t；于是"数据说这次不那么陡"永远不会把 R 拉下去。
    const double currentRatio = std::pow(kAmbienceDecayA, ambienceSeconds_);
    if (ambienceRatioTarget_ > 0.0 && ambienceRatioTarget_ > currentRatio) {
        ambienceSeconds_ = std::log(ambienceRatioTarget_) / std::log(kAmbienceDecayA);
    }
    ambienceRatio_ = std::pow(kAmbienceDecayA, ambienceSeconds_);

    // ---- 3) D：余额的纯函数，**不加时间平滑** ----
    //  ★ 用**本帧屏幕上那个数字**（value_ 由本帧的 UpdateValue/AdvancePlaces 刚推完）：
    //    D 说的是"现在还看得见的那个数有多低"。目标值只在清零预估那一行用。
    const double shownYuan = (value_ < 0.0) ? 0.0 : value_;
    ambienceDepth_ = BalanceDepth(shownYuan);

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
    if (glowIntensity_ > 1.0f) glowIntensity_ = 1.0f;
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
    rateEstimate_ = EstimateRate(RateInputForCurrency(currencyShown_));

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
    const int prefix = want - table;             // >12 时多喂的填充点，会被环挤掉
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
    if (ambienceGiven_) {            // 程序内夹具（DisplayedAmount::SetAmbienceGiven）
        ratioShown_ = ambienceGivenRatio_;
        depthShown_ = ambienceGivenDepth_;
    // 命令行夹具（--ambience=R,D）：跳过上面两条状态规则，只影响导出/验收。
    } else if (AmbienceOverride().given) {
        ratioShown_ = AmbienceOverride().ratio;
        depthShown_ = AmbienceOverride().depth;
    }
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
//  ★ 触发条件就是你给的计时：t = now − 上次触发，t >= T 就跳一次。**T 与 A 在触发那一刻
//    采样、整拍不变**：R 现在每帧都在衰减（R(t) = kAmbienceDecayA^t），若每帧重算 T，
//    那么"已经等了多久"和"要等多久"会同时变，参照系自己会动。
//  ★ 第一拍锚在**第一次调用的时候**（而不是"仿真时刻 0"）：调用它的那一刻 R/D 才是
//    有意义的——`--ambience=R,D` 这类夹具在窗口建好之后才生效，若第一拍锚在 0、用夹具
//    生效前的 R/D（R=0 ⇒ 周期 15 s）采样，夹具就算白设了（这个坑真的踩过一次：夹具写
//    T=0.5 s，屏幕上却是 15 s 的节拍）。
void DisplayedAmount::AdvanceBeat(double dtSeconds) {
    if (!AmbienceFrozenFlag()) beatSimSeconds_ += dtSeconds;
    if (!beatSeeded_) {
        beatSeeded_ = true;
        TriggerBeatAt(beatSimSeconds_);
        ++beatCount_;
    } else if (beatSimSeconds_ - beatBucket_.triggerSeconds >= beatBucket_.periodSeconds) {
        TriggerBeatAt(beatSimSeconds_);
        ++beatCount_;
    }
    beatOffsetDip_ = BeatOffsetFromBucket(beatSimSeconds_, beatBucket_);
}

// 触发一拍：采样这一拍的周期与幅度，并把触发时刻记为 at。
void DisplayedAmount::TriggerBeatAt(double atSeconds) {
    beatBucket_.triggerSeconds = atSeconds;
    beatBucket_.periodSeconds = BeatPeriodSeconds(ratioShown_, depthShown_);
    beatBucket_.amplitudePx = BeatAmplitudePx(ratioShown_, depthShown_);
    if (g_beatTrace) {
        std::fprintf(stderr, "[trace] trigger t=%.6f period=%.6f amp=%.6f R=%.6f D=%.6f\n",
                     atSeconds, beatBucket_.periodSeconds, beatBucket_.amplitudePx, ratioShown_,
                     depthShown_);
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

WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol) {
    WidgetFrame f{};
    f.state = state;
    f.statusText = StatusTextFor(state);
    f.countdownText = CountdownText();

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
    f.ambientIntensity = amount.ambienceIntensity();
    f.beatOffsetDip = static_cast<float>(amount.beatOffsetDip());

    // ★ 符号与数字**分开决定**（所有者）：只要币种是确定的，即使没有数字也要显示符号，
    //   否则切到没数据的币种时看不出自己在看哪个币种。
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

