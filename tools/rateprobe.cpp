// rateprobe -- offline proof for the consumption-rate estimator and the
//             "time to zero" wording (src/rate_estimator.h/.cpp).
//
//   rateprobe            run every check (the default)
//   rateprobe --verbose  also print the per-case series and the spring's convergence
//
// No network, no external file, no shared state: the estimator is a pure function, so
// this tool needs nothing but synthetic series. It creates no file at all.
//
// Output contract (same shape as storeprobe): one line per acceptance item, each line
// starting with "PASS: " or "FAIL: ". The exit code is 0 only when every line is a PASS.
// ★ Every check prints the NUMBERS it judged -- the raw rate, the rounded rate, status,
//   point/step counts, time span, the drop sum and the wording produced -- so the report
//   can be read, not just trusted. A check that only printed PASS would be worth nothing.
//
// 所有者要求的验收项，逐条对应（编号就是下面 case 的编号）：
//   (1) 稳定消耗 -> 估计速率对上已知的元/分钟，且底部文案就是那一条外推
//   (2) 一次大额充值 -> 速率不为负、**仍然为正**、且接近真实消耗（充值自己不进分子）
//   (3) ★ 回归（这一轮的起点）：既有上升又有下降的序列 -> 一个真实的显著正速率，
//       而不是「暂无法预测」。旧口径在这个序列上会丢掉回升之后的每一个点，
//       剩下的下降段跨度 240 s < 300 s -> 「暂无法预测」；case 里把这两组数字都印出来
//   (4) 一个下降台阶都没有 -> 速率 0、status = NotConsuming、文案是破折号 U+2014
//   (5) 跨度不到 5 分钟、或下降台阶不到 3 个 -> 不显著（bar 就是 3：2 个不显著、3 个显著）
//   (6) 带"没有时间"的点的序列 -> 不显著，绝不假设点距
//   (7) 弹簧：给一个阶跃，收敛到目标且**从不过冲**
//   (8) §7.4 其余分支（已用尽 / 超过 7 天 / 外推 + 绝对时刻）与 10% 重绘规则
//   (9) 旧夹具那种形状（跳上去、随后掉回原来的水平）现在读数是**真的**一笔 50 元消费：
//       冻结这条期望，免得下一次有人照着旧探针"修回去"
//   (10) 平滑的边界：0 -> 正速率这一段**不做平滑**（否则屏幕上会先闪一下「超过 7 天」）
#include "../src/rate_estimator.h"

#include "amount.h"
#include "tuning.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

using dshb::Amount;
using dshb::RateEstimate;
using dshb::RateInputPoint;
using dshb::RateStatus;
using dshb::ZeroTimeKind;
using dshb::ZeroTimeRenderState;
using dshb::ZeroTimeText;

namespace {

// ===========================================================================
// A deliberately tiny harness (same contract as storeprobe's)
// ===========================================================================
struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }

    // Same bookkeeping, no output: used by the harness self-check so that proving
    // "a failing check fails" cannot itself print a FAIL line and sink the exit code.
    bool ReqSilent(bool ok) {
        if (ok) {
            ++passed;
            return true;
        }
        ++failed;
        failures.push_back("silent check failed");
        return false;
    }
};

// ===========================================================================
// Text helpers
// ===========================================================================
// The wording is Chinese. The console codepage of a redirected run is not UTF-8, so
// every string is printed twice: once as UTF-8 bytes (with the console set to 65001,
// so a terminal shows it) and once as bare hex, which cannot be mangled by any
// codepage and is what a failing check must be read from.
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::string Hex(const std::wstring& text) {
    std::string out;
    for (const wchar_t unit : text) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", static_cast<unsigned>(unit));
        if (!out.empty()) out += " ";
        out += buf;
    }
    return out;
}

// "text" (hex: ...) -- the quoted form for a human, the hex form for the judge.
std::string Show(const std::wstring& text) {
    return "\"" + Utf8(text) + "\" (hex: " + Hex(text) + ")";
}

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// Fixed 4-decimal formatting, so two rates can be compared by eye.
std::string F4(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", value);
    return buf;
}

std::string F6(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return buf;
}

std::string Num(long long value) { return std::to_string(value); }

// "HH:MM" in LOCAL time, written here independently of the module under test (using
// localtime_s directly) rather than borrowed from it: an expectation computed by calling
// the code it judges would agree with that code by construction. Two independent
// implementations are the point.
std::wstring LocalClockFor(int64_t epochSeconds) {
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

// ===========================================================================
// Building synthetic series
// ===========================================================================
// Money never goes through a binary float on the way IN: the probe builds an Amount
// from the decimal string with dshb::ParseAmount (amount.h), the same converter the
// store and the API layer use. The estimator itself works on the fixed-point raw value.
const int64_t kAnchor = 1789561662;   // any epoch second; the estimator never reads a clock

// The wording's absolute-time branch does read a clock, but only to print local HH:MM,
// so the probe gives it a fixed instant and never depends on today's date.
const int64_t kTextNow = 1789561662;

bool YuanToRaw(const std::string& text, int64_t* out) {
    Amount amount;
    if (!dshb::ParseAmount(text, &amount)) return false;
    *out = amount.raw;
    return true;
}

// One dated, parsed point: the normal case.
RateInputPoint Dated(int64_t at, const std::string& yuan) {
    RateInputPoint point;
    point.at = at;
    point.atValid = true;
    point.amountValid = YuanToRaw(yuan, &point.amountRaw);
    return point;
}

// A point with no time of its own: what an old curve.json (no "at") reloads as.
RateInputPoint Undated(const std::string& yuan) {
    RateInputPoint point;
    point.at = 0;
    point.atValid = false;
    point.amountValid = YuanToRaw(yuan, &point.amountRaw);
    return point;
}

// A steady drain at exactly -drainPerMinute, one point every stepSeconds, starting at
// `first` yuan with `count` points. Built through the same decimal parser as everything
// else: the amounts are written with 4 decimals so a computed fixture cannot drift.
std::vector<RateInputPoint> SteadySeries(const std::string& first, double drainPerMinute,
                                        int count, int64_t stepSeconds) {
    double yuan = std::atof(first.c_str());
    std::vector<RateInputPoint> out;
    for (int i = 0; i < count; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", yuan);
        out.push_back(Dated(kAnchor + static_cast<int64_t>(i) * stepSeconds, buf));
        yuan -= drainPerMinute * (static_cast<double>(stepSeconds) / 60.0);
    }
    return out;
}

// A REAL top-up, in the only shape the store can write it: the rise arrives as ONE point
// that is higher than its predecessor, and every later point continues at the NEW level
// (curve_store §2.1 records a point only when the value changed, and §2.3 compares the
// incoming value against the newest point). The store never writes "one high point, then
// the old level" -- that would mean the balance fell back by the whole top-up, i.e. a real
// spend of that size, and case9 pins down what the estimator says about exactly that shape.
std::vector<RateInputPoint> WithRealTopUp(const std::vector<RateInputPoint>& base, int jumpIndex,
                                         const std::string& topUpYuan) {
    std::vector<RateInputPoint> out = base;
    Amount top;
    if (!dshb::ParseAmount(topUpYuan, &top)) return out;
    for (std::size_t i = static_cast<std::size_t>(jumpIndex); i < out.size(); ++i) {
        out[i].amountRaw += top.raw;
    }
    return out;
}

// ---------------------------------------------------------------------------
// An INDEPENDENT walk over a series (the expectation the estimator is judged against)
// ---------------------------------------------------------------------------
// ★ Written here from the definition -- "sum of the decreases, over the span between the
//   oldest and the newest usable point" -- and NOT by calling the estimator: an expectation
//   computed by the code under test would agree with it by construction.
struct DropWalk {
    bool comparable = true;    // every point carried both an amount and its own time
    int64_t dropSumRaw = 0;    // Σ max(0, previous − next)
    int steps = 0;             // how many steps fell
    int rises = 0;             // how many steps rose (they contribute 0, they do not discard)
    int64_t spanSeconds = 0;
};

DropWalk WalkDrops(const std::vector<RateInputPoint>& points) {
    DropWalk walk;
    std::vector<int64_t> amounts;
    std::vector<int64_t> times;
    for (const RateInputPoint& point : points) {
        if (!point.amountValid || !point.atValid) {
            walk.comparable = false;
            continue;
        }
        amounts.push_back(point.amountRaw);
        times.push_back(point.at);
    }
    if (amounts.size() < 2) return walk;
    walk.spanSeconds = times.back() - times.front();
    for (std::size_t i = 1; i < amounts.size(); ++i) {
        const int64_t drop = amounts[i - 1] - amounts[i];
        if (drop > 0) {
            walk.dropSumRaw += drop;
            ++walk.steps;
        } else if (drop < 0) {
            ++walk.rises;
        }
    }
    return walk;
}

// Yuan per minute implied by a walk: dropSum / span, in 元/分钟.
double WalkRate(const DropWalk& walk) {
    if (walk.spanSeconds <= 0) return 0.0;
    return static_cast<double>(walk.dropSumRaw) / static_cast<double>(dshb::kUnitsPerYuan) * 60.0 /
           static_cast<double>(walk.spanSeconds);
}

// What a reading that counted |change| instead (i.e. treated a top-up as consumption)
// would produce. Printed as "what we are NOT reporting" -- the number that makes the
// difference between "the rise contributed 0" and "the rise was counted" visible.
double AbsoluteChangeRate(const std::vector<RateInputPoint>& points) {
    std::vector<int64_t> amounts;
    std::vector<int64_t> times;
    for (const RateInputPoint& point : points) {
        if (!point.amountValid || !point.atValid) continue;
        amounts.push_back(point.amountRaw);
        times.push_back(point.at);
    }
    if (amounts.size() < 2 || times.back() - times.front() <= 0) return 0.0;
    int64_t sum = 0;
    for (std::size_t i = 1; i < amounts.size(); ++i) {
        sum += std::llabs(amounts[i] - amounts[i - 1]);
    }
    return static_cast<double>(sum) / static_cast<double>(dshb::kUnitsPerYuan) * 60.0 /
           static_cast<double>(times.back() - times.front());
}

// A series summary for the evidence line: "10.0000@+0s, 9.9800@+60s, ...".
std::string SeriesSummary(const std::vector<RateInputPoint>& points) {
    if (points.empty()) return "no points";
    std::string out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out += ", ";
        char buf[64];
        const double yuan = static_cast<double>(points[i].amountRaw) /
                            static_cast<double>(dshb::kUnitsPerYuan);
        if (!points[i].atValid) {
            std::snprintf(buf, sizeof(buf), "%.4f@undated", yuan);
        } else {
            std::snprintf(buf, sizeof(buf), "%.4f@+%llds", yuan,
                          static_cast<long long>(points[i].at - points.front().at));
        }
        out += buf;
    }
    return out;
}

// "raw=0.0200000000 rate=0.0200000000 status=Significant usable=11/11 steps=10/0 span=600s"
std::string EstimateSummary(const RateEstimate& e) {
    return "rawRate=" + F6(e.rawRateYuanPerMinute) + " rate=" + F6(e.rateYuanPerMinute) +
           " yuan/min, status=" + dshb::RateStatusName(e.status) + ", usable=" +
           Num(e.usablePoints) + "/" + Num(e.pointsSeen) + ", decreasingSteps=" +
           Num(e.decreasingSteps) + ", risingSteps=" + Num(e.risingSteps) + ", dropSum=" +
           F6(static_cast<double>(e.dropSumRaw) / static_cast<double>(dshb::kUnitsPerYuan)) +
           " yuan, undated=" + Num(e.undatedPoints) + ", unusableAmount=" + Num(e.unusablePoints) +
           ", span=" + Num(static_cast<long long>(e.spanSeconds)) + "s" +
           (e.note.empty() ? std::string() : ", note=" + Quote(e.note));
}

// ===========================================================================
// The acceptance checks
// ===========================================================================
void RunChecks(Harness* h, bool verbose) {
    // -----------------------------------------------------------------------
    // (1) A steady drain at a KNOWN rate: the estimate must match it, and be Significant.
    //     The wording of §7.4's extrapolating branch is checked on the same fixture.
    // -----------------------------------------------------------------------
    {
        const double trueDrain = 0.02;                       // 元/分钟
        const std::vector<RateInputPoint> series = SteadySeries("10.0000", trueDrain, 11, 60);
        const RateEstimate e = dshb::EstimateRate(series);
        const DropWalk walk = WalkDrops(series);
        const double expectedRate = WalkRate(walk);
        const double error = std::fabs(e.rateYuanPerMinute - expectedRate);
        // 10.00 元 / 0.02 元每分钟 = 500 分钟 = 8.3 小时（> 6 小时，所以 §7.4 不补绝对时刻）
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 10 * dshb::kUnitsPerYuan, kTextNow);
        const std::wstring wantText = L"按当前速度，约 8.3 小时后归零";
        const bool ok = e.status == RateStatus::Significant && error <= 1e-9 &&
                        std::fabs(e.rateYuanPerMinute - trueDrain) <= 1e-9 && e.rateYuanPerMinute > 0.0 &&
                        e.decreasingSteps == 10 && e.risingSteps == 0 && e.usablePoints == 11 &&
                        e.spanSeconds == 600 &&
                        e.dropSumRaw == 20 * (dshb::kUnitsPerYuan / 100) &&   // ten 2-cent drops
                        e.dropSumRaw == walk.dropSumRaw &&
                        text.kind == ZeroTimeKind::Extrapolated && text.text == wantText &&
                        text.minutesToZero == 500;
        h->Req("case1", "a steady synthetic drain (0.020000 yuan/min) is estimated as such",
               "independent walk says " + F6(expectedRate) + " yuan/min (" + F6(walk.dropSumRaw /
                   static_cast<double>(dshb::kUnitsPerYuan)) + " yuan over " +
                   Num(static_cast<long long>(walk.spanSeconds)) + "s in " + Num(walk.steps) +
                   " steps), " + EstimateSummary(e) + ", |error|=" + F6(error) +
                   " (tolerance 0.000000001); balance 10.00 -> " + Show(text.text) +
                   ", minutesToZero=" + Num(text.minutesToZero),
               ok);
        if (verbose) std::printf("  series: %s\n", SeriesSummary(series).c_str());
    }

    // -----------------------------------------------------------------------
    // (2) The same series with one large TOP-UP (+50.00 yuan) part-way through: the rate
    //     must stay POSITIVE and close to the true drain. 设计 §7.2：充值不是负消费。
    //     ★ The top-up's own step contributes 0 to the numerator, and the window keeps
    //     every point (that is the whole point of the new rule): the reported rate loses
    //     exactly the ONE step's drain that the rise replaced -- printed as a number.
    // -----------------------------------------------------------------------
    {
        const double trueDrain = 0.02;
        const std::vector<RateInputPoint> plain = SteadySeries("10.0000", trueDrain, 11, 60);
        const std::vector<RateInputPoint> topped = WithRealTopUp(plain, 7, "50.00");
        const RateEstimate plainEstimate = dshb::EstimateRate(plain);
        const RateEstimate e = dshb::EstimateRate(topped);
        const DropWalk walk = WalkDrops(topped);
        const double expectedRate = WalkRate(walk);
        const double error = std::fabs(e.rateYuanPerMinute - expectedRate);
        const double deficit = plainEstimate.rateYuanPerMinute - e.rateYuanPerMinute;
        const double oneStepShare = trueDrain * (1.0 / 10.0);   // 11 points -> 10 drop steps
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 5980 * (dshb::kUnitsPerYuan / 100), kTextNow);
        const bool ok = e.status == RateStatus::Significant && e.rateYuanPerMinute > 0.0 &&
                        error <= 1e-9 && e.usablePoints == 11 && e.spanSeconds == 600 &&
                        e.decreasingSteps == 9 && e.risingSteps == 1 &&
                        std::fabs(deficit - oneStepShare) <= 1e-12 &&
                        e.rateYuanPerMinute > 0.9 * plainEstimate.rateYuanPerMinute - 1e-12 &&
                        text.kind == ZeroTimeKind::Extrapolated;
        h->Req("case2", "one large TOP-UP: the rate stays positive and within one step of the true drain",
               "plain " + F6(plainEstimate.rateYuanPerMinute) + " yuan/min vs topped " +
                   F6(e.rateYuanPerMinute) + " yuan/min; the deficit " + F6(deficit) +
                   " is exactly the one step the rise replaced (" + F6(oneStepShare) +
                   ", i.e. 1 of 10 drop steps; its time still counts, which is why it is not 0); " +
                   EstimateSummary(e) + ", |error| vs independent walk=" + F6(error) +
                   "; the +50.00 rise contributed 0 (had a rise been counted as consumption, "
                   "the |change| sum would give " + F6(AbsoluteChangeRate(topped)) +
                   " yuan/min); balance 59.80 -> " + Show(text.text),
               ok);
        if (verbose) std::printf("  with top-up: %s\n", SeriesSummary(topped).c_str());
    }

    // -----------------------------------------------------------------------
    // (3) ★ THE REGRESSION THAT STARTED THIS: a series that both RISES and FALLS.
    //     Four falling steps (3 drops over 240 s), then a +50.00 top-up, then a balance
    //     that keeps RISING. Under the old rule (drop every rise, then fit) the only
    //     surviving points are the 4 falling ones -- span 240 s < the 5-minute bar --
    //     so the old code answered 「暂无法预测」. Under the owner's rule the window keeps
    //     every point, the three drops are real spending, and the rate is 0.075 yuan/min.
    // -----------------------------------------------------------------------
    {
        std::vector<RateInputPoint> series;
        series.push_back(Dated(kAnchor + 0, "10.0000"));
        series.push_back(Dated(kAnchor + 60, "9.7000"));    // drop 0.30
        series.push_back(Dated(kAnchor + 120, "9.4000"));   // drop 0.30
        series.push_back(Dated(kAnchor + 240, "9.1000"));   // drop 0.30
        series.push_back(Dated(kAnchor + 360, "59.1000"));  // +50.00 top-up: a rise
        series.push_back(Dated(kAnchor + 480, "59.6000"));  // rising (a rise, not a drop)
        series.push_back(Dated(kAnchor + 600, "60.1000"));  // rising
        series.push_back(Dated(kAnchor + 720, "60.6000"));  // rising
        const RateEstimate e = dshb::EstimateRate(series);
        const DropWalk walk = WalkDrops(series);
        const double expectedRate = WalkRate(walk);          // 0.90 yuan over 720 s = 0.075
        const double error = std::fabs(e.rateYuanPerMinute - expectedRate);
        // 60.60 元 / 0.075 元每分钟 = 808 分钟 = 13.5 小时（> 6 小时，所以不补绝对时刻）
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 6060 * (dshb::kUnitsPerYuan / 100), kTextNow);
        const std::wstring wantText = L"按当前速度，约 13.5 小时后归零";
        const std::wstring noPrediction = L"暂无法预测";
        const bool ok = e.status == RateStatus::Significant && e.rateYuanPerMinute > 0.0 &&
                        error <= 1e-9 && e.decreasingSteps == 3 && e.risingSteps == 4 &&
                        e.usablePoints == 8 && e.spanSeconds == 720 &&
                        text.kind == ZeroTimeKind::Extrapolated && text.text == wantText &&
                        text.text != noPrediction;
        h->Req("case3", "★ REGRESSION: a series that rises AND falls gives a real significant "
                        "positive rate, not 「暂无法预测」",
               "8 points, 3 drops (0.90 yuan) and 4 rises over 720 s -> " + EstimateSummary(e) +
                   " (independent walk: " + F6(expectedRate) + " yuan/min, |error|=" + F6(error) +
                   "); nothing was discarded (usable=" + Num(e.usablePoints) + "/" +
                   Num(e.pointsSeen) + "), so the span is the whole 720 s instead of the 240 s "
                   "the old rule was left with (240 s < " +
                   Num(static_cast<long long>(dshb::kRateMinSpanSeconds)) +
                   " s -> that is where 「暂无法预测」 came from); balance 60.60 -> " +
                   Show(text.text),
               ok);
        if (verbose) std::printf("  mixed rise/fall: %s\n", SeriesSummary(series).c_str());
    }

    // -----------------------------------------------------------------------
    // (4) NO DROPS AT ALL -> rate exactly zero, status NotConsuming, the wording is the
    //     dash. 设计 §7.4「速率 == 0 ->「—」」。
    //     ★ The dash is written as an ESCAPE (U+2014, em dash) on purpose: U+2014 and
    //     U+2015 look identical on screen, and comparing against the wrong one is a
    //     failure that reads like a pass. Written as a codepoint there is nothing to
    //     mistake.
    //     ★ Three shapes, because "no drop" is not only "flat": flat, rising-only, and a
    //     one-point window (which is what the store really holds when the balance never
    //     moved: a point exists only where the value changed).
    // -----------------------------------------------------------------------
    {
        const std::wstring expected = L"\u2014";   // em dash, U+2014 -- see the note above
        // (a) flat: 10 points over 9 minutes at one single value.
        std::vector<RateInputPoint> flat;
        for (int i = 0; i < 10; ++i) flat.push_back(Dated(kAnchor + i * 60, "10.0000"));
        const RateEstimate a = dshb::EstimateRate(flat);
        const ZeroTimeText textA = dshb::ZeroTimeFor(a, 10 * dshb::kUnitsPerYuan, kTextNow);

        // (b) rising only: a balance that does nothing but grow (a top-up that never stops).
        std::vector<RateInputPoint> rising;
        for (int i = 0; i < 6; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", 10.0 + 0.5 * i);
            rising.push_back(Dated(kAnchor + i * 120, buf));
        }
        const RateEstimate b = dshb::EstimateRate(rising);
        const ZeroTimeText textB = dshb::ZeroTimeFor(b, 1250 * (dshb::kUnitsPerYuan / 100), kTextNow);

        // (c) a single point: the store's own shape for a balance that never moved.
        std::vector<RateInputPoint> one;
        one.push_back(Dated(kAnchor, "10.0000"));
        const RateEstimate c = dshb::EstimateRate(one);
        const ZeroTimeText textC = dshb::ZeroTimeFor(c, 10 * dshb::kUnitsPerYuan, kTextNow);

        // (d) two flat points only 4 s apart: no significance bar applies to the
        //     "not consuming" conclusion (it is an observation, not an estimate).
        std::vector<RateInputPoint> brief;
        brief.push_back(Dated(kAnchor, "10.0000"));
        brief.push_back(Dated(kAnchor + 4, "10.0000"));
        const RateEstimate d = dshb::EstimateRate(brief);

        const bool ok = a.status == RateStatus::NotConsuming && a.rateYuanPerMinute == 0.0 &&
                        a.rawRateYuanPerMinute == 0.0 && a.decreasingSteps == 0 && a.risingSteps == 0 &&
                        a.spanSeconds == 540 && textA.text == expected &&
                        textA.kind == ZeroTimeKind::Dash && textA.minutesToZero < 0 &&
                        std::string(dshb::RateStatusName(a.status)) == "NotConsuming" &&
                        b.status == RateStatus::NotConsuming && b.rateYuanPerMinute == 0.0 &&
                        b.risingSteps == 5 && b.decreasingSteps == 0 && textB.text == expected &&
                        textB.kind == ZeroTimeKind::Dash &&
                        c.status == RateStatus::NotConsuming && c.rateYuanPerMinute == 0.0 &&
                        c.spanSeconds == 0 && textC.kind == ZeroTimeKind::Dash &&
                        d.status == RateStatus::NotConsuming && d.spanSeconds == 4 &&
                        d.rateYuanPerMinute == 0.0 &&
                        std::string(dshb::RateStatusName(RateStatus::NotConsuming)) != "Rising";
        h->Req("case4", "no decreasing step at all -> rate zero, NotConsuming, wording is the dash",
               "(a) flat, span 540 s: " + EstimateSummary(a) + ", text=" + Show(textA.text) +
                   ", minutesToZero=" + Num(textA.minutesToZero) + " (none claimed); " +
                   "(b) rising only, span 600 s: " + EstimateSummary(b) + ", text=" +
                   Show(textB.text) + "; (c) ONE point (span 0 s, the store's own shape for a "
                   "balance that never moved): " + EstimateSummary(c) + " -> " +
                   dshb::ZeroTimeKindName(textC.kind) + "; (d) two flat points 4 s apart (no "
                   "significance bar applies to an observation): status=" +
                   dshb::RateStatusName(d.status) + ", span=" +
                   Num(static_cast<long long>(d.spanSeconds)) + "s, rate=" +
                   F6(d.rateYuanPerMinute) + "; the old status name was \"Rising\" for all of "
                   "these, which is why it was renamed; expected=" + Show(expected),
               ok);
    }

    // -----------------------------------------------------------------------
    // (5) The two hard bars of §7.3: a span under 5 minutes -> Insignificant, and fewer
    //     than 3 decreasing steps -> Insignificant. Both with the numbers that were
    //     measured and refused (the raw rate is kept as a diagnostic on purpose).
    //     ★ The bar is exactly 3: 2 steps refuse, 3 steps pass (case3 is the 3).
    // -----------------------------------------------------------------------
    {
        // (a) 12 points, obvious drain, but only 110 seconds of time.
        const std::vector<RateInputPoint> shortSpan = SteadySeries("10.0000", 0.02, 12, 10);
        const RateEstimate a = dshb::EstimateRate(shortSpan);
        const ZeroTimeText textA = dshb::ZeroTimeFor(a, 9 * dshb::kUnitsPerYuan, kTextNow);

        // (b) one decreasing step over a perfectly good 10-minute span.
        std::vector<RateInputPoint> oneStep;
        oneStep.push_back(Dated(kAnchor, "10.0000"));
        oneStep.push_back(Dated(kAnchor + 150, "9.9800"));
        for (int i = 2; i < 5; ++i) oneStep.push_back(Dated(kAnchor + 150 * i, "9.9800"));
        const RateEstimate b = dshb::EstimateRate(oneStep);
        const ZeroTimeText textB = dshb::ZeroTimeFor(b, 998 * (dshb::kUnitsPerYuan / 100), kTextNow);

        // (c) two decreasing steps: still one short of the bar.
        std::vector<RateInputPoint> twoSteps;
        twoSteps.push_back(Dated(kAnchor, "10.0000"));
        twoSteps.push_back(Dated(kAnchor + 200, "9.8000"));
        twoSteps.push_back(Dated(kAnchor + 400, "9.6000"));
        twoSteps.push_back(Dated(kAnchor + 600, "11.0000"));   // a rise: no third drop
        const RateEstimate c = dshb::EstimateRate(twoSteps);

        const std::wstring wantNoPrediction = L"暂无法预测";
        const bool ok = a.status == RateStatus::Insignificant && a.rateYuanPerMinute == 0.0 &&
                        a.rawRateYuanPerMinute > 0.0 && a.decreasingSteps == 11 &&
                        a.spanSeconds == 110 && a.spanSeconds < dshb::kRateMinSpanSeconds &&
                        textA.kind == ZeroTimeKind::NoPrediction && textA.text == wantNoPrediction &&
                        b.status == RateStatus::Insignificant && b.decreasingSteps == 1 &&
                        b.spanSeconds == 600 && b.rateYuanPerMinute == 0.0 &&
                        textB.kind == ZeroTimeKind::NoPrediction &&
                        c.status == RateStatus::Insignificant && c.decreasingSteps == 2 &&
                        c.spanSeconds == 600 && c.risingSteps == 1 && c.rateYuanPerMinute == 0.0;
        h->Req("case5", "a span under 5 minutes, or fewer than 3 decreasing steps -> Insignificant",
               "(a) 12 points over " + Num(static_cast<long long>(a.spanSeconds)) + "s < " +
                   Num(static_cast<long long>(dshb::kRateMinSpanSeconds)) + "s: " +
                   EstimateSummary(a) + " (the raw rate " + F6(a.rawRateYuanPerMinute) +
                   " yuan/min was measured and deliberately NOT reported), text=" +
                   Show(textA.text) + "; (b) 1 decreasing step over 600 s: " + EstimateSummary(b) +
                   ", text=" + Show(textB.text) + "; (c) 2 decreasing steps over 600 s: " +
                   EstimateSummary(c) + " (the bar is " +
                   Num(dshb::kRateMinDecreasingSamples) + ", and case3 shows 3 passing)",
               ok);
    }

    // -----------------------------------------------------------------------
    // (6) A series with points that have no time of their own -> Insignificant rather
    //     than a fabricated rate. 老 curve.json（没有 "at"）读回来就是这样。
    //     ★ This is the case that forbids assuming the 10 s sampling interval.
    // -----------------------------------------------------------------------
    {
        // (a) nothing dated at all: an old file.
        std::vector<RateInputPoint> allUndated;
        for (int i = 0; i < 6; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", 10.0 - 0.1 * i);
            allUndated.push_back(Undated(buf));
        }
        const RateEstimate a = dshb::EstimateRate(allUndated);

        // (b) ten dated points in an obvious drain, plus ONE undated point in the middle:
        //     the dated subset would fit perfectly (9 decreasing steps over 540 s), which
        //     is exactly why it must not be used.
        std::vector<RateInputPoint> mixed = SteadySeries("10.0000", 0.02, 10, 60);
        mixed.insert(mixed.begin() + 5, Undated("9.5000"));
        const RateEstimate b = dshb::EstimateRate(mixed);
        const ZeroTimeText text = dshb::ZeroTimeFor(a, 10 * dshb::kUnitsPerYuan, kTextNow);

        const bool ok = a.status == RateStatus::Insignificant && a.usablePoints == 0 &&
                        a.rateYuanPerMinute == 0.0 && a.undatedPoints == 1 &&
                        b.status == RateStatus::Insignificant && b.undatedPoints == 1 &&
                        b.rateYuanPerMinute == 0.0 && b.decreasingSteps == 9 &&
                        b.spanSeconds == 540 &&
                        text.kind == ZeroTimeKind::NoPrediction;
        h->Req("case6", "points without their own time -> Insignificant, never a fabricated rate",
               "all-undated: " + EstimateSummary(a) + " (one undated RUN, 6 points); "
               "one undated point among 10 dated ones: " + EstimateSummary(b) +
                   " -- note decreasingSteps=9 and span=540 s: the DATED subset would have "
                   "produced a rate, and it is deliberately not used; text=" + Show(text.text),
               ok);
    }

    // -----------------------------------------------------------------------
    // (7) The spring: a step input converges towards the target and never overshoots.
    //     设计 §7.3 末尾（rate -> rate_display 的弹簧）+ §9.6（不许突变/不许过冲）。
    // -----------------------------------------------------------------------
    {
        const double target = 0.50;    // yuan/min, a step up from 0 (rates are never negative)
        const double dt = 1.0 / 60.0;
        double display = 0.0;
        bool monotone = true;
        bool overshoot = false;
        double previous = display;
        int framesTo63 = -1;
        int framesTo99 = -1;
        const int kFrames = 60 * 60;   // a full minute of 60 Hz frames
        std::vector<double> samples;
        for (int i = 0; i < kFrames; ++i) {
            const double next = dshb::RateSpringStep(display, target, dt);
            // Monotone towards the target (never falls back on a rising step)...
            if (next < previous - 1e-15) monotone = false;
            // ...and never past it.
            if (next > target + 1e-12) overshoot = true;
            previous = next;
            display = next;
            if (framesTo63 < 0 && display >= target * 0.63) framesTo63 = i + 1;
            if (framesTo99 < 0 && display >= target * 0.99) framesTo99 = i + 1;
            if (i < 5 || i % 60 == 0) samples.push_back(display);
        }

        // §9.6's acceptance shape: at one time constant the value is at 63% +/- 10%.
        const double tauFrames = dshb::kRateSpringTauSeconds / dt;
        const bool tauInRange = framesTo63 > 0 &&
                                std::fabs(static_cast<double>(framesTo63) - tauFrames) < 0.10 * tauFrames;
        // "Converged" is relative, not absolute: an exponential only approaches its
        // target, so demanding |residual| < 1e-6 would demand the impossible and would
        // fail a spring that is behaving perfectly. 1% of the step is the honest bar --
        // and 1e-6 absolute was tried first and failed at 2.3e-5, which is what a
        // 6-second time constant should still have left after 60 seconds.
        const double residual = std::fabs(display - target);
        const double settleBar = 0.01 * std::fabs(target);
        const bool settles = residual < settleBar;
        // dt clamping: one absurd step (as after a sleep) must not jump to the target.
        const double huge = dshb::RateSpringStep(0.0, target, 3600.0);
        const bool clamped = huge > 0.0 && huge < target;
        const bool backward = dshb::RateSpringStep(0.25, target, -1.0) == 0.25;

        const bool ok = monotone && !overshoot && settles && tauInRange && clamped && backward;
        h->Req("case7", "the spring converges to the target, monotonically and without overshoot",
               "target=" + F6(target) + ", after " + Num(kFrames) + " frames display=" +
                   F6(display) + " (|residual|=" + F6(residual) + " < bar " + F6(settleBar) +
                   " = 1% of the step), monotone=" +
                   std::string(monotone ? "yes" : "NO") + ", overshoot=" +
                   std::string(overshoot ? "YES" : "none") + ", 63% at frame " + Num(framesTo63) +
                   " (tau=" + Num(static_cast<long long>(dshb::kRateSpringTauSeconds)) + "s = " +
                   F4(tauFrames) + " frames, tolerance 10%), 99% at frame " + Num(framesTo99) +
                   ", dt=3600 s step -> " + F6(huge) + " (clamped, not 1 frame to target), "
                   "dt<0 -> unchanged=" + std::string(backward ? "yes" : "NO"),
               ok);
        if (verbose) {
            std::printf("  spring samples (every 60th frame):");
            for (const double value : samples) std::printf(" %s", F6(value).c_str());
            std::printf("\n");
        }
    }

    // -----------------------------------------------------------------------
    // (8) The other three branches of §7.4's mapping, and the documented format of the
    //     extrapolated line: 已用尽 / 超过 7 天 / 「按当前速度，约 X 小时后归零」+ 绝对时刻.
    //     (The task numbers 6 checks; this one exists because a wording that is never
    //     exercised is a wording nobody has read. It is the §7.4 table, end to end.)
    // -----------------------------------------------------------------------
    {
        // A significant drain of 2.00 yuan/min: 240 yuan left -> 120 minutes.
        const std::vector<RateInputPoint> series = SteadySeries("130.0000", 2.00, 11, 60);
        const RateEstimate e = dshb::EstimateRate(series);

        const ZeroTimeText usedUp = dshb::ZeroTimeFor(e, 0, kTextNow);
        const ZeroTimeText overWeek = dshb::ZeroTimeFor(e, 30000 * dshb::kUnitsPerYuan, kTextNow);
        const ZeroTimeText twoHours = dshb::ZeroTimeFor(e, 240 * dshb::kUnitsPerYuan, kTextNow);
        const ZeroTimeText shortRemainder = dshb::ZeroTimeFor(e, 6 * dshb::kUnitsPerYuan, kTextNow);

        const std::wstring wantUsedUp = L"已用尽";
        const std::wstring wantOver = L"超过 7 天";
        const bool usedUpOk = usedUp.kind == ZeroTimeKind::UsedUp && usedUp.text == wantUsedUp;
        const bool overOk = overWeek.kind == ZeroTimeKind::OverSevenDays && overWeek.text == wantOver;
        // 240 yuan / 2.00 per min = 120 min = 2 hours. ★ The expected line is built from
        // the two halves the design doc names, because at 2 hours the remainder is
        // UNDER the 6-hour bar, so §7.4 requires the absolute local time to be in the
        // same line: 「按当前速度，约 2 小时后归零」+「约 22:27」.
        const std::wstring wantRelative = L"按当前速度，约 2 小时后归零";
        const bool twoHoursOk = twoHours.kind == ZeroTimeKind::Extrapolated &&
                                twoHours.minutesToZero == 120 &&
                                twoHours.text == wantRelative + L" · 约 " +
                                                      LocalClockFor(kTextNow + 120 * 60);
        // 6 yuan / 2.00 per min = 3 min: under 6 hours, so an absolute local time follows.
        const std::string shortUtf8 = Utf8(shortRemainder.text);
        const bool hasPrefix = shortUtf8.rfind("按当前速度，约 ", 0) == 0;
        const bool hasClock = shortUtf8.find(" · 约 ") != std::string::npos;
        // "HH:MM" at the very end, and the two digits must be a real clock reading.
        bool clockShape = shortUtf8.size() >= 5;
        if (clockShape) {
            const std::string tail = shortUtf8.substr(shortUtf8.size() - 5);
            clockShape = tail[2] == ':' && tail[0] >= '0' && tail[0] <= '9' && tail[1] >= '0' &&
                         tail[1] <= '9' && tail[3] >= '0' && tail[3] <= '9' && tail[4] >= '0' &&
                         tail[4] <= '9';
        }
        const bool shortOk = shortRemainder.kind == ZeroTimeKind::Extrapolated &&
                             shortRemainder.minutesToZero == 3 && hasPrefix && hasClock && clockShape;

        // §7.4「数值变化 < 10% 时不重绘」: same branch, a minute count 3% lower -> the
        // PREVIOUS string is kept; 30% lower -> it is redrawn.
        ZeroTimeRenderState state;
        const std::wstring first = dshb::ZeroTimeTextForFrame(e, 240 * dshb::kUnitsPerYuan, kTextNow, &state);
        RateEstimate tiny = e;
        tiny.rateYuanPerMinute = e.rateYuanPerMinute * 1.03;   // 3% faster -> 116 min, ~3% less
        const std::wstring kept = dshb::ZeroTimeTextForFrame(tiny, 240 * dshb::kUnitsPerYuan, kTextNow, &state);
        RateEstimate big = e;
        big.rateYuanPerMinute = e.rateYuanPerMinute * 1.30;    // 30% faster -> 92 min
        const std::wstring redrawn = dshb::ZeroTimeTextForFrame(big, 240 * dshb::kUnitsPerYuan, kTextNow, &state);
        const bool hysteresis = first == wantRelative + L" · 约 " + LocalClockFor(kTextNow + 120 * 60) &&
                                kept == first && redrawn != kept;

        // A branch change always redraws: significant -> insignificant.
        RateEstimate none;
        none.status = RateStatus::Insignificant;
        const std::wstring branchChanged = dshb::ZeroTimeTextForFrame(none, 240 * dshb::kUnitsPerYuan, kTextNow, &state);
        const bool branchRedraws = branchChanged == L"暂无法预测";

        // A negative balance is NOT this module's line at all (欠款时说"几小时后归零"
        // 是废话)：the caller already suppresses it, this is the second guard.
        const ZeroTimeText owed = dshb::ZeroTimeFor(e, -1 * dshb::kUnitsPerYuan, kTextNow);
        const bool owedNone = owed.kind == ZeroTimeKind::None && owed.text.empty();

        const bool ok = usedUpOk && overOk && twoHoursOk && shortOk && hysteresis &&
                        branchRedraws && owedNone && e.status == RateStatus::Significant &&
                        e.rateYuanPerMinute > 0.0;
        h->Req("case8", "the remaining 7.4 branches and the 10% redraw rule",
               "estimate=" + EstimateSummary(e) + "; balance 0 -> " + Show(usedUp.text) +
                   " (" + dshb::ZeroTimeKindName(usedUp.kind) + "); 30000 yuan -> " +
                   Show(overWeek.text) + " (" + dshb::ZeroTimeKindName(overWeek.kind) +
                   "); 240 yuan -> " + Show(twoHours.text) + " (" +
                   dshb::ZeroTimeKindName(twoHours.kind) + ", minutesToZero=" +
                   Num(twoHours.minutesToZero) + "); 6 yuan -> " + Show(shortRemainder.text) +
                   " (minutesToZero=" + Num(shortRemainder.minutesToZero) + ", has HH:MM=" +
                   std::string(clockShape ? "yes" : "NO") + "); redraw: 3% faster kept the old " +
                   "line=" + std::string(kept == first ? "yes" : "NO") + ", 30% faster redrew=" +
                   std::string(redrawn != kept ? "yes" : "NO") + ", branch change redrew=" +
                   std::string(branchRedraws ? "yes" : "NO") + ", negative balance -> none=" +
                   std::string(owedNone ? "yes" : "NO"),
               ok);
    }

    // -----------------------------------------------------------------------
    // (9) The shape the OLD probe used as its top-up fixture: one point raised by +50.00
    //     while the points after it keep the OLD, lower level. That is NOT a top-up --
    //     it means the balance fell back by 50.02 yuan between two readings, i.e. a real
    //     50-yuan spend. The estimator now says so (5.018 yuan/min), and this check
    //     freezes that reading so nobody "restores" the old fixture and old expectation.
    //     ★ The old expectation (0.02 yuan/min, |error| <= 1e-6) was an artefact of OLS:
    //     the fitted line only saw the endpoints, and dropping the raised point hid the
    //     50-yuan step between the raised point and the next one.
    // -----------------------------------------------------------------------
    {
        const std::vector<RateInputPoint> plain = SteadySeries("10.0000", 0.02, 11, 60);
        std::vector<RateInputPoint> oldShape = plain;
        oldShape[7].amountRaw += 50 * dshb::kUnitsPerYuan;   // raise ONE point, keep the rest
        const RateEstimate e = dshb::EstimateRate(oldShape);
        const DropWalk walk = WalkDrops(oldShape);
        const double expectedRate = WalkRate(walk);
        const double error = std::fabs(e.rateYuanPerMinute - expectedRate);
        const bool ok = e.status == RateStatus::Significant && e.rateYuanPerMinute > 5.0 &&
                        error <= 1e-9 && e.decreasingSteps == 9 && e.risingSteps == 1 &&
                        e.usablePoints == 11 && e.spanSeconds == 600;
        h->Req("case9", "one point raised by +50.00 with the OLD level after it is a real 50.02 yuan "
                        "drop, and the rate says so",
               EstimateSummary(e) + " (independent walk: " + F6(expectedRate) +
                   " yuan/min, |error|=" + F6(error) + "; the biggest single step is 50.02 yuan, "
                   "not a top-up: 9 drops totalling " +
                   F6(static_cast<double>(walk.dropSumRaw) / static_cast<double>(dshb::kUnitsPerYuan)) +
                   " yuan over " + Num(static_cast<long long>(walk.spanSeconds)) + " s). "
                   "A REAL top-up is case2 (the new level continues afterwards)",
               ok);
        if (verbose) std::printf("  old fixture shape: %s\n", SeriesSummary(oldShape).c_str());
    }

    // -----------------------------------------------------------------------
    // (10) The smoothing boundary the window cares about: "0 -> a positive rate" must NOT
    //      be smoothed. It is a BRANCH change (§7.4: 分支变了 -> 一定重画), and smoothing
    //      it walks through rates so small that the extrapolation is "over 7 days" for a
    //      measurable number of frames -- printed below. Between two POSITIVE rates the
    //      spring is used, and then it never overshoots.
    // -----------------------------------------------------------------------
    {
        const double target = 0.02;        // yuan/min, the case1/2/3 kind of rate
        const double balance = 10.0;       // yuan
        const double dt = 1.0 / 60.0;
        int artifactFrames = 0;
        double display = 0.0;
        std::wstring firstArtifact;
        for (int i = 0; i < 600; ++i) {
            display = dshb::RateSpringStep(display, target, dt);
            RateEstimate e;
            e.status = RateStatus::Significant;
            e.rateYuanPerMinute = display;
            const ZeroTimeText text = dshb::ZeroTimeFor(
                e, static_cast<int64_t>(balance * dshb::kUnitsPerYuan), kTextNow);
            if (text.kind == ZeroTimeKind::OverSevenDays) {
                ++artifactFrames;
                if (firstArtifact.empty()) firstArtifact = text.text;
            }
        }
        // The seeded alternative (what the window does): land on the target at once.
        RateEstimate seeded;
        seeded.status = RateStatus::Significant;
        seeded.rateYuanPerMinute = target;
        const ZeroTimeText seededText = dshb::ZeroTimeFor(
            seeded, static_cast<int64_t>(balance * dshb::kUnitsPerYuan), kTextNow);
        // Between two positive rates the spring IS used, monotonically and without overshoot.
        const double mid = dshb::RateSpringStep(0.02, 0.05, dt);
        const bool steadySpring = mid > 0.02 && mid < 0.05;

        const bool ok = artifactFrames > 0 && artifactFrames < 60 &&
                        firstArtifact == L"超过 7 天" &&
                        seededText.kind == ZeroTimeKind::Extrapolated &&
                        seededText.minutesToZero == 500 && steadySpring;
        h->Req("case10", "smoothing a 0 -> positive rate would flash 「超过 7 天」: the window seeds it instead",
               "naive smoothing from 0 to " + F6(target) + " yuan/min: the extrapolation says " +
                   Show(firstArtifact) + " for the first " + Num(artifactFrames) +
                   " frame(s) (" + F4(artifactFrames / 60.0) + " s at 60 Hz) before it settles; "
                   "seeded (what display-layer AdvanceRate does): " + Show(seededText.text) +
                   " (minutesToZero=" + Num(seededText.minutesToZero) + ") immediately; "
                   "between two positive rates the spring runs: one frame 0.02 -> 0.05 gives " +
                   F6(mid) + " (strictly inside, never past the target)",
               ok);
    }
}

// ---------------------------------------------------------------------------
// The harness must be able to fail, or every PASS above is worthless
// ---------------------------------------------------------------------------
void RunHarnessSelfCheck(Harness* h) {
    Harness throwaway;
    const bool good = throwaway.Req("selfcheck:probe-true", "harness reports a true check as PASS",
                                    "expected true", true);
    const bool bad = throwaway.ReqSilent(false);
    const bool ok = good && !bad && throwaway.passed == 1 && throwaway.failed == 1 &&
                    throwaway.failures.size() == 1;
    h->Req("case0", "harness self-check: a failing check really fails, and is counted",
           "true->" + std::string(good ? "PASS" : "FAIL") + ", false->" +
               std::string(bad ? "PASS" : "FAIL") + ", throwaway counters " +
               Num(throwaway.passed) + " passed / " + Num(throwaway.failed) + " failed",
           ok);
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose") {
            verbose = true;
        } else {
            std::printf("rateprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: rateprobe [--verbose]\n");
            return 64;
        }
    }

    // The wording is Chinese; a console (or a redirected file) is not UTF-8 by default.
    // Best effort only -- the hex printing below is the form that cannot be mangled.
    SetConsoleOutputCP(CP_UTF8);

    Harness h;
    std::printf("rateprobe: offline rate-estimator proof (pure function, no clock, no file)\n");
    std::printf("tuning: kRateSpringTauSeconds=%.1f kRateMinDecreasingSamples=%d "
                "kRateMinSpanSeconds=%lld\n",
                dshb::kRateSpringTauSeconds, dshb::kRateMinDecreasingSamples,
                static_cast<long long>(dshb::kRateMinSpanSeconds));

    try {
        RunHarnessSelfCheck(&h);
        RunChecks(&h, verbose);
    } catch (const std::exception& e) {
        std::printf("FAIL: runner | an exception escaped a check: %s\n", e.what());
        ++h.failed;
    } catch (...) {
        std::printf("FAIL: runner | an unknown exception escaped a check\n");
        ++h.failed;
    }

    std::printf("checks: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& failure : h.failures) {
        std::printf("failed line: %s\n", failure.c_str());
    }
    std::printf("RESULT: %s\n", h.failed == 0 && h.passed > 0 ? "PASS" : "FAIL");
    return (h.failed == 0 && h.passed > 0) ? 0 : 1;
}
