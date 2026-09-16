// rate_estimator.cpp —— 见 rate_estimator.h（纯函数；没有时钟、没有文件、没有全局量）
#include "rate_estimator.h"

#include "amount.h"    // kUnitsPerYuan: the fixed-point unit this module reads and writes
#include "tuning.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace dshb {
namespace {

// 定点 -> 元。**只有这一个方向**：拟合用 double，但金额的存储与比较始终是定点。
constexpr double kUnitsPerYuanD = static_cast<double>(kUnitsPerYuan);

// 拟合前的斜率下限（元/分钟）。低于它就算"这条序列根本没在变"。
// 为什么需要它：半天掉一分钱时，OLS 给出的斜率是一个约 1e-6 元/分钟的极小值，
// 它不是"消耗速度"，只是首尾差被摊到时间上。报出去等于编了一个数字。
constexpr double kFlatRateCutoffYuanPerMinute = 1e-6;

// 十的幂，手写一份免得引入 <cmath> 之外的约定。
double Pow10(int digits) {
    double out = 1.0;
    for (int i = 0; i < digits; ++i) out *= 10.0;
    return out;
}

// 取整到 digits 位小数。用 llround：远离零方向取整，正负都不出现"取整后变号"。
double RoundToDigits(double value, int digits) {
    const double scale = Pow10(digits);
    return static_cast<double>(std::llround(value * scale)) / scale;
}

std::string Num(long long value) { return std::to_string(value); }
int64_t Abs64(int64_t value) { return value < 0 ? -value : value; }

bool MostlyFinite(double value) { return std::isfinite(value); }

}  // namespace

const char* RateStatusName(RateStatus status) {
    switch (status) {
        case RateStatus::Empty: return "Empty";
        case RateStatus::Insignificant: return "Insignificant";
        case RateStatus::Rising: return "Rising";
        case RateStatus::Significant: return "Significant";
    }
    return "?";
}

const char* ZeroTimeKindName(ZeroTimeKind kind) {
    switch (kind) {
        case ZeroTimeKind::None: return "None";
        case ZeroTimeKind::NoPrediction: return "NoPrediction";
        case ZeroTimeKind::Dash: return "Dash";
        case ZeroTimeKind::UsedUp: return "UsedUp";
        case ZeroTimeKind::OverSevenDays: return "OverSevenDays";
        case ZeroTimeKind::Extrapolated: return "Extrapolated";
    }
    return "?";
}

// ===========================================================================
// §7.3 的估计本体
// ===========================================================================
RateEstimate EstimateRate(const std::vector<RateInputPoint>& pointsOldestFirst) {
    RateEstimate result;
    result.pointsSeen = static_cast<int>(pointsOldestFirst.size());
    if (pointsOldestFirst.empty()) {
        result.note = "no points at all";
        return result;   // status 默认是 Empty
    }

    // -- 1. 分类：诊断全部记上，"没有时间"是一条会毒掉整个结果的结论 ----------
    bool sawUndated = false;
    bool previousUndated = false;
    for (const RateInputPoint& point : pointsOldestFirst) {
        if (!point.amountValid) {
            ++result.unusablePoints;
            previousUndated = false;
            continue;
        }
        if (!point.atValid) {
            // 连续的一段"没有时间"只算一个：给了 10 个没有时间的点，说"10 个点
            // 没有时间"会让人以为它们是 10 次独立测量，其实一次都没有。
            if (!previousUndated) ++result.undatedPoints;
            previousUndated = true;
            sawUndated = true;
            continue;
        }
        previousUndated = false;
    }

    // -- 2. 只保留"金额和时间都可用"的点，它们按时间顺序进拟合 ----------------
    std::vector<RateInputPoint> raw;
    raw.reserve(pointsOldestFirst.size());
    for (const RateInputPoint& point : pointsOldestFirst) {
        if (point.amountValid && point.atValid) raw.push_back(point);
    }
    if (raw.empty()) {
        // ★ "Empty" means exactly this: not one point in the series carried a usable
        //   AMOUNT -- there is nothing to look at at all. Every other way of being
        //   inconclusive is "Insignificant": a series of undated points HAS a balance to
        //   look at, we simply cannot time it. Both print the same line today, but a
        //   caller that only reads the status must not be told "no data" about a file
        //   full of points. (The all-undated path below is the one that decides that.)
        if (sawUndated) {
            result.status = RateStatus::Insignificant;
            result.note = "no point carries both an amount and its own time: " +
                          Num(result.undatedPoints) + " undated run(s), " +
                          Num(result.unusablePoints) + " without an amount";
            return result;
        }
        result.status = RateStatus::Empty;
        result.note = "no point carries a usable amount";
        return result;
    }

    // -- 3. ★ 正跳变先剔除：**发现跳变的那一个点丢掉**，只丢它一个 --------------
    //   ★ 比的是**紧邻的前一个点**，不是"上一个保留点"。这个区别是探针 case9 逼出来的：
    //     拿"上一个保留点"当基准，充值之后每一个点都比那个低水位高，于是**充值之后的
    //     所有点全被当成跳变丢掉**——跨度塌掉、样本塌掉，一次充值就把整个估计器打成
    //     "不显著"。那既不是 §7.2 要的，也会让"充完接着花"这种最常见的场景读不出速率。
    //   §7.2 要丢的是**跳变那一刻**：余额涨了 = 这一刻不是消费，仅此而已。跳变之后的
    //   点每一个都仍然是"相对上一次的余额变化"，照常参与拟合。
    std::vector<RateInputPoint> usable;
    usable.reserve(raw.size());
    int jumps = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i > 0 && raw[i].amountRaw > raw[i - 1].amountRaw) {
            ++jumps;      // 充值 / 赠金：不是负消费，也永远不许进入拟合（§7.2）
            continue;
        }
        usable.push_back(raw[i]);
    }
    result.usablePoints = static_cast<int>(usable.size());
    // The span is reported even when the checks below refuse the series: a reader has to
    // be able to tell "not enough time" from "not enough points", and an evidence line
    // that says "span=0" for a series that plainly spans ten minutes is a lie.
    result.spanSeconds = usable.size() > 1 ? usable.back().at - usable.front().at : 0;
    // What was actually measured, kept alongside the reason for refusing (below): a
    // refusal that replaces the numbers would make every "no" look identical.
    const std::string note = "usable=" + Num(result.usablePoints) + ", span=" +
                             Num(static_cast<long long>(result.spanSeconds)) + " s, removed " +
                             Num(jumps) + " positive jump(s)";

    // -- 4. 不显著的两条硬判据（§7.3「样本不足」）----------------------------
    //   ★ "可用的点在减少的点少于 3 个" = 保留点少于 3 个：因为正跳变已经剔掉，
    //     相邻两个保留点之间不可能上升。所以这里数点就是数"在减少的样本"。
    //   ★ 5 分钟跨度同样要有：三点挤在 4 秒里，斜率是一个没有意义的数。
    //   ★ 从这一行往后所有"不给数字"的结论都是 Insignificant，不是 Empty。
    //     `refusal` 说的是"为什么不肯给数字"，`note` 是测量到的事实；两个都留着，
    //     所以一句 "the balance did not move" 不会把 "usable=10, span=540s" 挤掉。
    result.status = RateStatus::Insignificant;
    std::string refusal;
    if (sawUndated) {
        refusal = "a point has no time of its own (" + Num(result.undatedPoints) +
                  " undated run(s)): a rate would have to assume the spacing";
        result.note = refusal + "; " + note;
        return result;
    }
    if (result.usablePoints < kRateMinDecreasingSamples) {
        refusal = "only " + Num(result.usablePoints) +
                  " usable decreasing sample(s) after removing " + Num(jumps) +
                  " positive jump(s); " + Num(kRateMinDecreasingSamples) + " are required";
        result.note = refusal + "; " + note;
        return result;
    }
    if (result.spanSeconds < kRateMinSpanSeconds) {
        refusal = "time span " + Num(static_cast<long long>(result.spanSeconds)) + " s < " +
                  Num(static_cast<long long>(kRateMinSpanSeconds)) + " s";
        result.note = refusal + "; " + note;
        return result;
    }

    // -- 5. 普通最小二乘（OLS）：斜率 = Σ(t−t̄)(v−v̄) / Σ(t−t̄)² ---------------
    //   §7.3 文档写的是"稳健回归"，所有者明确换成 OLS（见头文件顶部第 2 条）。
    //   时间基点取第一个可用的点，免得大 epoch 数进平方和丢精度。
    const int64_t t0 = usable.front().at;
    double meanT = 0.0;
    double meanV = 0.0;
    for (const RateInputPoint& point : usable) {
        meanT += static_cast<double>(point.at - t0);
        meanV += static_cast<double>(point.amountRaw);
    }
    meanT /= static_cast<double>(usable.size());
    meanV /= static_cast<double>(usable.size());

    double cov = 0.0;
    double varT = 0.0;
    for (const RateInputPoint& point : usable) {
        const double dt = static_cast<double>(point.at - t0) - meanT;
        cov += dt * (static_cast<double>(point.amountRaw) - meanV);
        varT += dt * dt;
    }
    double rawSlope = 0.0;
    if (varT > 0.0) {
        // amountRaw 是 1/10000 元，时间是秒 -> 乘 60000 换成"元/分钟"。
        rawSlope = cov / varT / kUnitsPerYuanD * 60.0;
    }
    if (!MostlyFinite(rawSlope)) {
        result.note = "the fitted slope is not a finite number; " + note;
        return result;
    }
    result.rawSlopeYuanPerMinute = rawSlope;

    // 整条序列根本没动？那"速率 0"是事实本身，不是估计值。
    const bool noChangeAtAll = usable.front().amountRaw == usable.back().amountRaw;

    result.rateYuanPerMinute = RoundToDigits(rawSlope, kRateRoundDigitsYuanPerMinute);
    if (noChangeAtAll) {
        // The balance did not move at all. That is not "we could not tell" -- it is a
        // rate of exactly zero, which the wording turns into the dash ("不消耗"). Status
        // Rising carries "rate >= 0" (see the header), so this belongs here and not with
        // the refusals: reporting it as Insignificant would print "cannot predict yet"
        // for a balance that is simply sitting still.
        result.rateYuanPerMinute = 0.0;
        result.status = RateStatus::Rising;
        result.note = "the balance did not move at all over the samples; " + note;
        return result;
    }
    if (std::fabs(result.rateYuanPerMinute) < kFlatRateCutoffYuanPerMinute) {
        // There WAS movement, but the fitted slope is too small to mean anything (half a
        // cent over a day, where the "slope" is only the endpoint difference spread over
        // the time). Reporting 1e-6 yuan/min would be a fabricated number, so this one is
        // a refusal -- unlike the exactly-flat case above.
        result.rateYuanPerMinute = 0.0;
        result.status = RateStatus::Insignificant;
        refusal = "the fitted slope rounds to zero: no consumption to report";
        result.note = refusal + "; " + note;
        return result;
    }
    if (result.rateYuanPerMinute > 0.0) {
        // 走到这里说明上升**不是**正跳变造成的（正跳变已经剔除），是真的在回升。
        result.status = RateStatus::Rising;
        return result;
    }
    result.status = RateStatus::Significant;
    return result;
}

// ===========================================================================
// 弹簧（rate -> rate_display）
// ===========================================================================
double RateSpringStep(double rateDisplay, double rateTarget, double dtSeconds) {
    // dt 钳制：休眠唤醒后的第一帧 dt 可能是几千秒，不钳的话一步跳到位，
    // 弹簧就成了摆设（设计 §9.6 的通病表：dt 不钳制 -> 一帧跳到目标）。
    double dt = dtSeconds;
    if (!(dt > 0.0)) return rateDisplay;   // 也挡住了 NaN
    if (dt > kRateSpringMaxDtSeconds) dt = kRateSpringMaxDtSeconds;

    const double tau = kRateSpringTauSeconds;
    if (!(tau > 0.0)) return rateTarget;   // 时间常数非正 = 立即跟随

    // 连续解的离散形式：它在大 dt 下也不过冲，且恰好在 dt->∞ 时落到目标。
    const double alpha = 1.0 - std::exp(-dt / tau);
    double next = rateDisplay + (rateTarget - rateDisplay) * alpha;

    // ★ 永不越过目标：exp 形式本身不过冲，但浮点误差可能让它在目标附近翻过去，
    //   而"过冲"正是验收项 7 要挡的东西。夹住就永远不会。
    const double lo = rateDisplay < rateTarget ? rateDisplay : rateTarget;
    const double hi = rateDisplay < rateTarget ? rateTarget : rateDisplay;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    return next;
}

// ===========================================================================
// §7.4 底部文案
// ===========================================================================
namespace {

// 相对时长用哪种单位说。文档原文是「约 2 小时后归零」；到不了 1 小时的时候，
// "约 0.4 小时后归零"读起来很别扭，所以一分钟到一小时之间改用分钟。
std::wstring DurationPhrase(double minutes) {
    wchar_t buf[64];
    if (minutes < 60.0) {
        int whole = static_cast<int>(minutes + 0.5);
        if (whole < 1) whole = 1;   // 归零前一刻：说"约 1 分钟"比"约 0 分钟"诚实
        std::swprintf(buf, 64, L"%d 分钟", whole);
    } else {
        double hours = minutes / 60.0;
        // 取一位小数，去掉末尾的 .0：2 小时 / 2.5 小时都自然。
        double rounded = std::floor(hours * 10.0 + 0.5) / 10.0;
        if (std::fabs(rounded - std::floor(rounded + 0.5)) < 1e-9) {
            std::swprintf(buf, 64, L"%d 小时", static_cast<int>(rounded + 0.5));
        } else {
            std::swprintf(buf, 64, L"%.1f 小时", rounded);
        }
    }
    return buf;
}

// 本地时刻 HH:MM（§7.4：绝对时刻以本地时钟为准，必须和相对时长在同一段文案里）。
std::wstring LocalClockText(int64_t epochSeconds) {
    const std::time_t when = static_cast<std::time_t>(epochSeconds);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &when) != 0) return std::wstring();
#else
    if (localtime_r(&when, &local) == nullptr) return std::wstring();
#endif
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

bool SameKind(ZeroTimeKind a, ZeroTimeKind b) { return a == b; }

}  // namespace

ZeroTimeText ZeroTimeFor(const RateEstimate& estimate, int64_t balancedRaw, int64_t nowEpochSeconds) {
    ZeroTimeText out;

    // 欠款：余额已经是负的，"几小时后归零"是废话。窗口那边本来也不显示这一行
    // （widget_display.cpp 的 canEstimate），这里再挡一次，免得以后有人漏判。
    if (balancedRaw < 0) {
        out.kind = ZeroTimeKind::None;
        return out;
    }
    // ★ 判定顺序就是规格，别调换：
    //   1) 余额为 0 ->「已用尽」。余额真用尽了和"算不出速率"是两件事，
    //      先判它，否则一个刚好读不到速率的 0 余额会被说成"暂无法预测"。
    if (balancedRaw == 0) {
        out.kind = ZeroTimeKind::UsedUp;
        out.text = L"已用尽";
        return out;
    }
    // ★ 这两条的**顺序**是规格的一部分，而且很容易写反：
    //   「速率不显著」和「速率 ≤ 0」互相不含对方，但一个平的序列同时满足"算不出
    //   消耗"和"速率恰好是 0"。文档给它的是破折号（"不消耗"），不是"暂无法预测"。
    //   所以先判"我们手上有没有一个真正算出来的、非正的速率"：有就破折号。
    //   Insignificant / Empty 都**没有**可用速率（rate 被清成 0 只是"没数字"，
    //   不是"数字是 0"），所以它们必须走在后面，否则平的序列会说错话。
    //   （这个顺序也真的写错过一次，探针的 case4 就是为它留的。）
    //   3) 速率 ≤ 0（不消耗 / 在回升）->「—」
    //      ★ 这个破折号是 U+2014（em dash），与设计文档 §7.4 表格里那个字符逐字节
    //        相同——不是 U+2015（horizontal bar），两者在屏幕上几乎一样，
    //        拿前者去比后者会一直"看起来一样但断言失败"。探针按字节比，就是为了
    //        让这种错当场暴露（这个错真的发生过）。
    const bool rateReported = estimate.status == RateStatus::Significant ||
                              estimate.status == RateStatus::Rising;
    // ★ 破折号到底给谁：**给"没在消耗"的那一种**，也就是 Rising（平的，或真的在
    //   回升）。不是给"速率为负"——**消耗的速率本来就是负的**。
    //   写成 `estimate.rateYuanPerMinute <= 0.0` 是这个模块最贵的一个错：那个条件对
    //   每一个正常消耗都成立，于是每一个正常消耗都拿到破折号、永远走不到外推。
    //   case1-7 全绿也照样带着它（它们只看斜率，不看文案），是 case8 抓出来的。
    //   文档的「速率 ≤ 0」之所以能写成 ≤，是因为它指的是**余额的趋势**：趋势在涨或
    //   不动 -> 破折号；趋势在掉 -> 外推。状态已经把这件事说清楚了，就别再看符号。
    if (rateReported && estimate.status == RateStatus::Rising) {
        out.kind = ZeroTimeKind::Dash;
        out.text = L"\u2014";
        return out;
    }
    //   2) 速率不显著 ->「暂无法预测」。Empty 也走这条：没有点，同样无法预测。
    if (estimate.status == RateStatus::Insignificant || estimate.status == RateStatus::Empty) {
        out.kind = ZeroTimeKind::NoPrediction;
        out.text = L"暂无法预测";
        return out;
    }

    //   4) 剩余时长。
    //      ★ 用速率的**大小**（fabs），不是它本身。消耗的速率是负数（余额在掉），
    //        拿负数去除余额会得到负的分钟数，然后被下面的保护吞成"暂无法预测"——
    //        那等于**永远不会给出一条外推**。这个错真的发生过，是探针的 case8 抓出来的
    //        （case1-7 全绿也照样带着它，因为它们只看斜率不看文案）。
    //        raw 是 1/10000 元，速率是元/分钟。
    const double balanceYuan = static_cast<double>(balancedRaw) / kUnitsPerYuanD;
    const double speedYuanPerMinute = std::fabs(estimate.rateYuanPerMinute);
    const double minutes = balanceYuan / speedYuanPerMinute;
    if (!MostlyFinite(minutes) || speedYuanPerMinute <= 0.0) {
        // 没有可用的速率大小 -> 宁可说"无法预测"，也不编一个数。
        out.kind = ZeroTimeKind::NoPrediction;
        out.text = L"暂无法预测";
        return out;
    }
    out.minutesToZero = static_cast<int>(minutes + 0.5);
    if (minutes > kZeroTimeSevenDayMinutes) {
        out.kind = ZeroTimeKind::OverSevenDays;
        out.text = L"超过 7 天";   // 封顶：不给一个没人信的精确值
        return out;
    }

    //   5) 其它 ->「按当前速度，约 X 小时后归零」，剩余 ≤ 6 小时时补绝对时刻。
    out.kind = ZeroTimeKind::Extrapolated;
    std::wstring text = L"按当前速度，约 " + DurationPhrase(minutes) + L"后归零";
    if (minutes <= kZeroTimeAbsoluteMinutes) {
        const std::wstring clock = LocalClockText(nowEpochSeconds + static_cast<int64_t>(minutes + 0.5) * 60);
        if (!clock.empty()) text += L" · 约 " + clock;
    }
    out.text = text;
    return out;
}

std::wstring ZeroTimeTextForFrame(const RateEstimate& estimate, int64_t balancedRaw,
                                  int64_t nowEpochSeconds, ZeroTimeRenderState* state) {
    const ZeroTimeText fresh = ZeroTimeFor(estimate, balancedRaw, nowEpochSeconds);
    if (state == nullptr) return fresh.text;

    if (state->have && SameKind(fresh.kind, state->kind)) {
        // §7.4「数值变化 < 10% 时不重绘」：这是"别让数字乱跳"，不是"少算一次"。
        // 两种"没变"：取整到分钟没变，或变了但不到 10%。两者都保留上一帧的字。
        const bool minutesSame = fresh.minutesToZero == state->minutesToZero;
        bool withinTenPercent = false;
        if (!minutesSame && state->minutesToZero > 0 && fresh.minutesToZero >= 0) {
            const double previous = static_cast<double>(state->minutesToZero);
            const double delta = std::fabs(static_cast<double>(fresh.minutesToZero) - previous);
            withinTenPercent = delta < kZeroTimeRedrawFraction * previous;
        }
        // 但有例外：速率明显变了（>10%）就必须重画——"2.0 小时"和"1.05 小时"
        // 都取整成同一个分钟数附近时，光看分钟会把这个变化吞掉。
        // ★ 比的是**绝对值**：本分支里速率的符号是固定的（是一个正速率），只看
        //   大小才比得出来。写成 `ratePrevious > 0.0` 是这个模块真的犯过的错——
        //   消耗的速率是负的，那个判断永远不成立，这一整条例外就成了死代码。
        const double rateBefore = std::fabs(state->rateYuanPerMinute);
        const bool rateMoved = rateBefore > 1e-12 &&
                               std::fabs(std::fabs(estimate.rateYuanPerMinute) - rateBefore) >=
                                   kZeroTimeRedrawFraction * rateBefore;
        if ((minutesSame || withinTenPercent) && !rateMoved) {
            return state->text;
        }
    }

    state->have = true;
    state->kind = fresh.kind;
    state->minutesToZero = fresh.minutesToZero;
    state->rateYuanPerMinute = estimate.rateYuanPerMinute;
    state->text = fresh.text;
    return fresh.text;
}

}  // namespace dshb
