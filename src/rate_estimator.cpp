// rate_estimator.cpp —— 见 rate_estimator.h（纯函数；没有时钟、没有文件、没有全局量）
#include "rate_estimator.h"

#include "amount.h"    // kUnitsPerYuan: the fixed-point unit this module reads and writes
#include "tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dshb {
namespace {

// 定点 -> 元。**只有这一个方向**：速率用 double，但金额的存储与比较始终是定点。
constexpr double kUnitsPerYuanD = static_cast<double>(kUnitsPerYuan);

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

// 定点金额 -> 四位小数的"元"，只用在诊断文本里（note）。
std::string Yuan(int64_t raw) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(raw) / kUnitsPerYuanD);
    return buf;
}

// 速率 -> 文本（只用在诊断里）。速率的单位是元/分钟，取四位小数：真实速率在
// 0.001~0.05 元/分钟这个量级，四位足够看出两条口径的差别。
std::string RateText(double yuanPerMinute) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.4f", yuanPerMinute);
    return buf;
}

bool MostlyFinite(double value) { return std::isfinite(value); }

}  // namespace

const char* RateStatusName(RateStatus status) {
    switch (status) {
        case RateStatus::Empty: return "Empty";
        case RateStatus::Insignificant: return "Insignificant";
        case RateStatus::NotConsuming: return "NotConsuming";
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
// §7.2 §7.3 的估计本体
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

    // -- 2. 窗口 = "金额和时间都可用"的点，**一个都不剔除** --------------------
    //   ★ 这里不再剔除回升的点（旧口径丢的是"发现跳变的那一个点"）。回升只让**它自己
    //     那一个台阶**贡献 0（分子只加下降量），它的时间照样留在跨度里——那段时间确实
    //     过去了。把回升的点丢出窗口正是那次回归的病根：充值之后每个点都比前一个低水位
    //     高、全被判成跳变丢掉，一次充值就把跨度和样本一起打没。
    std::vector<RateInputPoint> usable;
    usable.reserve(pointsOldestFirst.size());
    for (const RateInputPoint& point : pointsOldestFirst) {
        if (point.amountValid && point.atValid) usable.push_back(point);
    }
    if (usable.empty()) {
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
    // ★ 时间上界的窗口（tuning.h 的 kRateWindowSeconds）：只保留最近这一段内的点。
    //   没有它，"下降量之和 ÷ 跨度"会被抖动点稀释 —— 每个抖动点只贡献 1 分钱，
    //   却把跨度拉长，于是速率一路衰减、预估值一路攀升（所有者实测到"超过 7 天"）。
    //   至少保留一个点；一个都不满足时不动（不让"算不出来"凭空出现）。
    //   0 = 关闭。
    if (kRateWindowSeconds > 0 && usable.size() > 1) {
        const int64_t newestAt = usable.back().at;
        std::size_t firstKept = 0;
        while (firstKept + 1 < usable.size() &&
               newestAt - usable[firstKept].at > kRateWindowSeconds) {
            ++firstKept;
        }
        if (firstKept > 0) {
            usable.erase(usable.begin(),
                         usable.begin() + static_cast<std::ptrdiff_t>(firstKept));
        }
    }
    result.usablePoints = static_cast<int>(usable.size());
    result.spanSeconds = usable.size() > 1 ? usable.back().at - usable.front().at : 0;

    // -- 3. ★ 分子：只把**下降**的台阶加起来（所有者的口径）--------------------
    //   drop = 前一点 − 后一点；drop > 0 -> 真的花了这么多，进分子；drop <= 0 -> 不进分子，
    //   但**绝不作废窗口**（回升时 drop < 0 只记诊断，它的时间照样留在跨度里）。
    int64_t dropSumRaw = 0;
    for (std::size_t i = 1; i < usable.size(); ++i) {
        const int64_t drop = usable[i - 1].amountRaw - usable[i].amountRaw;
        if (drop > 0) {
            dropSumRaw += drop;
            ++result.decreasingSteps;
        } else if (drop < 0) {
            ++result.risingSteps;
        }
    }
    result.dropSumRaw = dropSumRaw;

    // What was actually measured, kept alongside the reason for refusing (below): a
    // refusal that replaces the numbers would make every "no" look identical.
    const std::string note = "usable=" + Num(result.usablePoints) + ", span=" +
                             Num(static_cast<long long>(result.spanSeconds)) + " s, decreasing steps=" +
                             Num(result.decreasingSteps) + ", drops=" + Yuan(dropSumRaw) +
                             " yuan, rising steps=" + Num(result.risingSteps) +
                             " (a rise contributes 0 and never discards the window), undated=" +
                             Num(result.undatedPoints) + ", no amount=" + Num(result.unusablePoints);

    // 取整之前的原始速率（元/分钟）。**只看不判**：不显著的结论里也照记——"测到了多少"
    // 和"肯不肯报"是两件事。跨度 <= 0（点的时间不是递增的）时不编一个数：留 0。
    if (result.spanSeconds > 0) {
        result.rawRateYuanPerMinute = static_cast<double>(dropSumRaw) / kUnitsPerYuanD * 60.0 /
                                      static_cast<double>(result.spanSeconds);
    }
    if (!MostlyFinite(result.rawRateYuanPerMinute)) {
        result.note = "the computed rate is not a finite number; " + note;
        return result;
    }

    // -- 4. 判定（顺序就是规格，别调换）--------------------------------------
    //   从这一行往后所有"不给数字"的结论都是 Insignificant，不是 Empty。
    //   `refusal` 说的是"为什么不肯给数字"，`note` 是测量到的事实；两个都留着。
    result.status = RateStatus::Insignificant;
    std::string refusal;

    // (a) 有点没有自己的时间 -> 不显著。绝不假设点距（曲线规格 §2.3b）。
    if (sawUndated) {
        refusal = "a point has no time of its own (" + Num(result.undatedPoints) +
                  " undated run(s)): a rate would have to assume the spacing";
        result.note = refusal + "; " + note;
        return result;
    }
    // (b) ★ 一个下降台阶都没有 -> 速率**恰好 0**，status = NotConsuming。
    //     这一条**故意排在两条显著性判据之前**：显著性是"够不够格给一个速率"，
    //     而"这段没在花钱"本身就是个结论，不该被"样本不够"顶掉。
    //     两个已知后果，探针 case4/case5 都把它们印出来：
    //       · 一个点（或跨度 0）的窗口也算"没有下降" -> 破折号。曲线存储"只记变化"，
    //         所以余额一直不动时环里本来就只有一个点——那正是"不消耗"。
    //       · 只有回升（充值）的窗口同样 -> 破折号，而不是"暂无法预测"。
    if (result.decreasingSteps == 0) {
        result.rateYuanPerMinute = 0.0;
        result.status = RateStatus::NotConsuming;
        result.note = "no decreasing step in the window (flat, or rising only); " + note;
        return result;
    }
    // (c) 下降的台阶少于 kRateMinDecreasingSamples 个 -> 不显著。两个台阶可以
    //     "算出"任何东西，所以这条不能松（所有者的旧决定）。
    if (result.decreasingSteps < kRateMinDecreasingSamples) {
        refusal = "only " + Num(result.decreasingSteps) + " decreasing step(s); " +
                  Num(kRateMinDecreasingSamples) + " are required";
        result.note = refusal + "; " + note;
        return result;
    }
    // (d) 跨度不到 kRateMinSpanSeconds -> 不显著。10 个台阶挤在 4 秒里，速率是没有意义的数。
    if (result.spanSeconds < kRateMinSpanSeconds) {
        refusal = "time span " + Num(static_cast<long long>(result.spanSeconds)) + " s < " +
                  Num(static_cast<long long>(kRateMinSpanSeconds)) + " s";
        result.note = refusal + "; " + note;
        return result;
    }
    // (e) 速率 = 分子 / 跨度，取整到 kRateRoundDigitsYuanPerMinute 位。
    //     分子是下降量之和、跨度为正 -> 取整前的速率**永远 > 0**，所以速率永不为负。
    result.rateYuanPerMinute = RoundToDigits(result.rawRateYuanPerMinute, kRateRoundDigitsYuanPerMinute);
    if (!(result.rateYuanPerMinute > 0.0)) {
        // There WAS a drop, but it is so small that it rounds to zero at the reporting
        // resolution. Reporting "0" here would be a lie (we measured a drop) and reporting
        // the unrounded value would be a fabricated number, so this one is a refusal.
        // Unreachable with real data: the store's spans are at most 86400 s and one raw
        // unit of drop over that span is still 6.9e-8 yuan/min.
        result.rateYuanPerMinute = 0.0;
        refusal = "the computed rate rounds to zero at " + Num(kRateRoundDigitsYuanPerMinute) +
                  " digits; nothing to report";
        result.note = refusal + "; " + note;
        return result;
    }
    result.status = RateStatus::Significant;
    result.note = "consumption is the sum of the decreases only; " + note;
    return result;
}

// ===========================================================================
// ★ 新口径（所有者 2026-09-19）：Theil–Sen 中位斜率
// ===========================================================================
// 口径与"哪个是哪个、为什么两个共存"写在 rate_estimator.h，这里只说实现上的取舍。
RateEstimate EstimateRateTheilSen(const std::vector<RateInputPoint>& pointsOldestFirst) {
    RateEstimate result;
    result.pointsSeen = static_cast<int>(pointsOldestFirst.size());

    // -- 1. 分类：诊断全部记上 ----------------------------------------------
    // 与旧口径同一套习惯："没有时间"的点**不算**（绝不假设点距，见 rate_estimator.h），
    // 没有金额的点不算。两者都只记诊断，不进窗口。
    bool sawUndated = false;
    bool previousUndated = false;
    for (const RateInputPoint& point : pointsOldestFirst) {
        if (!point.amountValid) {
            ++result.unusablePoints;
            previousUndated = false;
            continue;
        }
        if (!point.atValid) {
            // 连续的一段"没有时间"只算一段：10 个没有时间的点不是 10 次测量。
            if (!previousUndated) ++result.undatedPoints;
            previousUndated = true;
            sawUndated = true;
            continue;
        }
        previousUndated = false;
    }

    std::vector<RateInputPoint> usable;
    usable.reserve(pointsOldestFirst.size());
    for (const RateInputPoint& point : pointsOldestFirst) {
        if (point.amountValid && point.atValid) usable.push_back(point);
    }
    if (usable.empty()) {
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

    // -- 2. 窗口：最新点往前 kRateWindowSeconds ------------------------------
    //   ★ 窗口的锚是**序列里最新那个点**，不是调用方的"现在"。理由是这段数据本来就
    //     是最近的记录，而"现在"可能离最后一次采样很久（网络断了、界面挂着没人看），
    //     拿"现在"当锚会把全部点判出窗口。锚在点上，窗口说的一律是"最近这段记录"。
    //   与旧口径共用同一个常量。0 = 关闭窗口（所有点都进）。
    const int64_t newestAt = usable.back().at;
    std::size_t first = 0;
    if (kRateWindowSeconds > 0) {
        // 至少留一个点：一个都不满足时不动（不让"算不出来"凭空出现）。
        while (first + 1 < usable.size() && newestAt - usable[first].at > kRateWindowSeconds) {
            ++first;
        }
    }
    result.usablePoints = static_cast<int>(usable.size() - first);
    result.spanSeconds = (result.usablePoints > 1) ? (newestAt - usable[first].at) : 0;

    // -- 3. 点对斜率 ---------------------------------------------------------
    //   斜率 = (amount_i − amount_j) / (at_i − at_j)，i 比 j 新。按时间排序之后 i > j，
    //   所以分母恒正，不存在"除以 0"或"除以负数"的编数路径。时间相同的两个点对
    //   （同一秒内的两次采样）跳过：dt == 0 的点对没有斜率可言。
    //   金额是定点，只在最后一步才转成 double。点对数 = C(n,2)，量级见 tuning.h 的 3d。
    std::vector<double> slopes;   // 元/秒（先别换成分钟：中位数要在同一个尺度上取）
    if (result.usablePoints >= 2) {
        const long long n = result.usablePoints;
        result.pairCount = n * (n - 1) / 2;
        const std::size_t count = static_cast<std::size_t>(result.usablePoints);
        slopes.reserve(static_cast<std::size_t>(result.pairCount));
        for (std::size_t i = first + 1; i < first + count; ++i) {
            for (std::size_t j = first; j < i; ++j) {
                const int64_t dt = usable[i].at - usable[j].at;
                if (dt <= 0) continue;
                const int64_t delta = usable[i].amountRaw - usable[j].amountRaw;
                slopes.push_back(static_cast<double>(delta) / kUnitsPerYuanD /
                                 static_cast<double>(dt));
            }
        }
        // 相邻台阶只作诊断：新口径**不用**它们做判定（中位数里每个点对都是一票）。
        for (std::size_t i = first + 1; i < first + count; ++i) {
            const int64_t dt = usable[i].at - usable[i - 1].at;
            const int64_t drop = usable[i - 1].amountRaw - usable[i].amountRaw;
            if (dt <= 0) continue;
            if (drop > 0) {
                ++result.decreasingSteps;
                result.dropSumRaw += drop;
            } else if (drop < 0) {
                ++result.risingSteps;
            }
        }
    }

    const std::string note = "window=" + Num(static_cast<long long>(kRateWindowSeconds)) +
                             " s, points in window=" + Num(result.usablePoints) + "/" +
                             Num(result.pointsSeen) + ", span=" +
                             Num(static_cast<long long>(result.spanSeconds)) + " s, pairs=" +
                             Num(result.pairCount) + ", decreasing steps=" +
                             Num(result.decreasingSteps) + ", rising steps=" +
                             Num(result.risingSteps) + ", drops=" + Yuan(result.dropSumRaw) +
                             " yuan, undated=" + Num(result.undatedPoints) + ", no amount=" +
                             Num(result.unusablePoints);

    // -- 4. 判定（顺序就是规格，别调换）--------------------------------------
    //   (a) 窗口里不到 2 个点 -> 不能预测。这是唯一一条"读不出来"。
    if (result.usablePoints < 2 || slopes.empty()) {
        result.status = RateStatus::Insignificant;
        result.note = "fewer than 2 usable points in the window (" +
                      Num(static_cast<int>(slopes.size())) + " pair(s)); " + note;
        return result;
    }

    //   (b) 中位数。偶数个取中间两个的平均（这是"中位数"的定义，不是新口径）。
    std::sort(slopes.begin(), slopes.end());
    const std::size_t n = slopes.size();
    const double medianYuanPerSecond =
        (n % 2 == 1) ? slopes[n / 2] : 0.5 * (slopes[n / 2 - 1] + slopes[n / 2]);
    result.medianSlopeYuanPerSecond = medianYuanPerSecond;

    //   (c) ★ 消耗为正：中位数斜率**取负号**就是 RateEstimate::rateYuanPerMinute 的方向
    //       （余额在掉 = 正速率）。中位数 >= 0 表示"这段里余额整体没往下走"——
    //       平的，或者只在回升（充值）。那是一个**结论**：不消耗，速率恰好 0。
    //       ★ 故意不做旧口径那套"取整到 N 位、取整后为 0 就当不显著"：这里的中位数是
    //         一个真实测量到的斜率，而定点金额 + 整秒时间戳能表达的最小非零斜率是
    //         1e-4 元/秒（= 0.006 元/分钟），远在任何浮点噪声之上。把"确实量到了一点
    //         消耗"取整成"读不出来"，正是旧口径那条纪律要防的反面。
    const double rateYuanPerMinute = -medianYuanPerSecond * 60.0;
    if (!(rateYuanPerMinute > 0.0)) {
        // 0 或负（回升）：不消耗。`!(x > 0)` 顺手挡住 NaN——不能把"读不出来"说成
        // "不消耗"，所以 NaN 走下面那条不显著。
        if (MostlyFinite(rateYuanPerMinute)) {
            result.rateYuanPerMinute = 0.0;
            result.rawRateYuanPerMinute = 0.0;
            result.status = RateStatus::NotConsuming;
            result.note = "the median pairwise slope is zero or rising (" +
                          RateText(medianYuanPerSecond) + " yuan/s) -> not consuming; " + note;
            return result;
        }
        result.status = RateStatus::Insignificant;
        result.note = "the median pairwise slope is not a finite number; " + note;
        return result;
    }

    result.status = RateStatus::Significant;
    result.rateYuanPerMinute = rateYuanPerMinute;
    result.rawRateYuanPerMinute = rateYuanPerMinute;
    result.note = "rate is the Theil-Sen median of all " + Num(result.pairCount) +
                  " pairwise slopes, negated (consumption is positive); median slope=" +
                  RateText(medianYuanPerSecond) + " yuan/s -> " + RateText(rateYuanPerMinute) +
                  " yuan/min; " + note;
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

// 相对时长怎么说（所有者 2026-09-19 定案）。
//  ★ 单位：不到 60 分钟说分钟，到 60 分钟及以上说小时。这是原来的口径，没改。
//  ★ 小数位：**永远一位小数，`.0` 不许省**（这次的定案）。原来写的是整数分钟
//    （`int whole = minutes + 0.5`），于是 2.5 分钟被说成"3 分钟"、2.0 小时被说成
//    "2 小时"。所有者要的是"一位小数始终在"，因为这一位就是这条预测的分辨率——
//    省掉它，用户分不出"2 分钟"和"2.4 分钟"。
//  ★★ 这一条**推翻**了 `不入库文件\t.md:118` 的"去掉末尾的 .0"（那份文档是那条旧
//     口径的出处）。以所有者 2026-09-19 的定案为准：`2.0 分钟`、`2.5 分钟`、
//     `2.0 小时`都要写出那一位。下一个读到这里的人不该以为这行写错了。
//  ★ 就低不就高（floor 到 0.1）：预测归零时刻**宁早不宁晚**。说"2.4 分钟"而实际是
//    2.49，用户不会因此误事；说"2.5 分钟"而实际是 2.41，就是把他说晚了。
//  ★ 仍有一个下限：不足 0.1 分钟（6 秒）时说"0.1 分钟"而不是"0.0 分钟"——
//    "约 0.0 分钟后归零"读起来像已经归零了。归零前一刻这句话必须还是"还没到"。
std::wstring DurationPhrase(double minutes) {
    wchar_t buf[64];
    if (minutes < 60.0) {
        // floor 到一位小数：先变成"十分之一分钟"，取整后再除以 10 打出来。
        double tenths = std::floor(minutes * 10.0);
        if (!(tenths >= 1.0)) tenths = 1.0;   // 也挡住了 NaN
        std::swprintf(buf, 64, L"%.1f 分钟", tenths / 10.0);
    } else {
        double tenths = std::floor(minutes / 60.0 * 10.0);
        if (!(tenths >= 1.0)) tenths = 1.0;
        std::swprintf(buf, 64, L"%.1f 小时", tenths / 10.0);
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
    //      先判它，否则一个刚好读不到速率的 0 余额会被说成"暂时无法预测何时归零"。
    if (balancedRaw == 0) {
        out.kind = ZeroTimeKind::UsedUp;
        out.text = L"已用尽";
        return out;
    }
    // ★ 两种"给不出时刻"合成同一句话（所有者 2026-09-19 定案）：
    //   · NotConsuming（速率恰好 0 = 这段没在花钱）
    //   · Insignificant / Empty（没算出来）
    //   原来两种字不一样（「—」和「暂无法预测」）。合并的理由：对用户它们是**同一个
    //   事实**——这一行给不出一个时刻；而一条光秃秃的破折号不解释为什么，用户只会
    //   以为界面坏了。`ZeroTimeKind` 两个值都留着：诊断（探针、日志）仍然要能分开
    //   "没在消耗"和"读不出来"，只是画出来一样。
    //   ★ 顺序仍然是规格：先判"我们手上有没有一个真正算出来的、等于 0 的速率"，
    //     再判"没算出来"。两者今天字一样，但 kind 不同，而 kind 决定
    //     ZeroTimeTextForFrame 的重绘判断（分支变了必须重画）。
    const bool rateReported = estimate.status == RateStatus::Significant ||
                              estimate.status == RateStatus::NotConsuming;
    if (rateReported && estimate.rateYuanPerMinute == 0.0) {
        out.kind = ZeroTimeKind::Dash;
        out.text = L"暂时无法预测何时归零";
        return out;
    }
    if (estimate.status == RateStatus::Insignificant || estimate.status == RateStatus::Empty) {
        out.kind = ZeroTimeKind::NoPrediction;
        out.text = L"暂时无法预测何时归零";
        return out;
    }

    //   4) 剩余时长。
    //      ★ 用速率的**大小**（fabs）。新口径下速率永远是正的，所以 fabs 只是
    //        一层保险；它的来历值得留着：旧口径里消耗的速率是**负数**，拿负数去除
    //        余额会得到负的分钟数，然后被下面的保护吞成"暂无法预测"——那等于
    //        **永远不会给出一条外推**（case8 抓出来的）。
    //        raw 是 1/10000 元，速率是元/分钟。
    const double balanceYuan = static_cast<double>(balancedRaw) / kUnitsPerYuanD;
    const double speedYuanPerMinute = std::fabs(estimate.rateYuanPerMinute);
    const double minutes = balanceYuan / speedYuanPerMinute;
    if (!MostlyFinite(minutes) || speedYuanPerMinute <= 0.0) {
        // 没有可用的速率大小 -> 宁可说"无法预测"，也不编一个数。
        out.kind = ZeroTimeKind::NoPrediction;
        out.text = L"暂时无法预测何时归零";
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
        // ★ 比的是**绝对值**：本分支里速率是正的（新口径下速率永不为负），只看
        //   大小才比得出来。写成 `ratePrevious > 0.0` 是这个模块真的犯过的错——
        //   旧口径里消耗的速率是负的，那个判断永远不成立，这一整条例外就成了死代码。
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
