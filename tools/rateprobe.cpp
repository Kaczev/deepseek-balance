// rateprobe -- offline proof for the consumption-rate estimator and the
//             "time to zero" wording (src/rate_estimator.h/.cpp).
//
//   rateprobe            run every check (the default)
//   rateprobe --verbose  also print the per-case series and the spring's convergence
//
// No network, no external file, no shared state: the estimator is a pure function, so
// this tool needs nothing but synthetic series. It creates one temporary directory it
// removes again (only with --keep does it stay, which is only useful for debugging).
//
// Output contract (same shape as storeprobe): one line per acceptance item, each line
// starting with "PASS: " or "FAIL: ". The exit code is 0 only when every line is a PASS.
// ★ Every check prints the NUMBERS it judged -- raw slope, rounded rate, status, point
//   counts, time span, and the wording produced -- so the report can be read, not just
//   trusted. A check that only printed PASS would be worth nothing here.
//
// 所有者要求的 7 项验收，逐条对应（编号就是下面 case 的编号）：
//   (1) 稳定消耗 -> 估计速率对上已知的元/分钟，status = Significant
//   (2) 同一序列插入一次大额充值 -> 速率不许变负，且仍接近真实消耗
//   (3) 只有两个点 -> Insignificant，且文案说"暂无法预测"
//   (4) 平的序列 -> 速率 0、不是 Significant，文案是破折号
//   (5) 时间跨度不到 5 分钟 -> 不管几个点都 Insignificant
//   (6) 带"没有时间"的点的序列 -> Insignificant，而不是编一个数
//   (7) 弹簧：给一个阶跃，收敛到目标且**从不过冲**
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

// Fixed 4-decimal formatting, so two slopes can be compared by eye.
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

// One big top-up inserted into a series: the point at jumpIndex is the SAME point as in
// the plain series but with +50 yuan added, so it is a large RISE against the previous
// retained point. Every later point keeps its plain-series value, which therefore sits
// 50 yuan below the jump point -- this is the only shape the store can really write,
// because the store never records a rise: a top-up arrives as one high point followed by
// continuation of the old level (curve_store §2.1 records changes, and §2.3's comparison
// is against the newest point, so the high point IS the newest point when it arrives).
std::vector<RateInputPoint> WithTopUp(const std::vector<RateInputPoint>& base, int jumpIndex) {
    std::vector<RateInputPoint> out = base;
    if (jumpIndex < 0 || jumpIndex >= static_cast<int>(out.size())) return out;
    out[static_cast<std::size_t>(jumpIndex)].amountRaw += 50 * dshb::kUnitsPerYuan;
    return out;
}

// A series summary for the evidence line: "10.0000 -> 8.0000 over 600 s, 11 points".
std::string SeriesSummary(const std::vector<RateInputPoint>& points) {
    if (points.empty()) return "no points";
    std::string out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out += ", ";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(points[i].amountRaw) /
                                                    static_cast<double>(dshb::kUnitsPerYuan));
        if (!points[i].atValid) {
            out += std::string("undated@") + buf;
        } else {
            std::snprintf(buf, sizeof(buf), "%.4f@+%llds", static_cast<double>(points[i].amountRaw) /
                                                               static_cast<double>(dshb::kUnitsPerYuan),
                          static_cast<long long>(points[i].at - points.front().at));
            out += buf;
        }
    }
    return out;
}

// "raw=-0.020000 rounded=-0.0200000000 status=Significant usable=11/11 span=600s"
std::string EstimateSummary(const RateEstimate& e) {
    return "rawSlope=" + F6(e.rawSlopeYuanPerMinute) + " yuan/min, rate=" +
           F6(e.rateYuanPerMinute) + " yuan/min, status=" + dshb::RateStatusName(e.status) +
           ", usable=" + Num(e.usablePoints) + "/" + Num(e.pointsSeen) +
           ", undated=" + Num(e.undatedPoints) + ", unusableAmount=" + Num(e.unusablePoints) +
           ", span=" + Num(static_cast<long long>(e.spanSeconds)) + "s" +
           (e.note.empty() ? std::string() : ", note=" + Quote(e.note));
}

// ===========================================================================
// The acceptance checks
// ===========================================================================
void RunChecks(Harness* h, bool verbose) {
    // -----------------------------------------------------------------------
    // (1) A steady drain at a KNOWN rate: the estimate must match, and be Significant.
    //     设计 §7.3 的方法（OLS）在一条规则序列上的正确性。
    // -----------------------------------------------------------------------
    {
        const double trueDrain = 0.02;                       // 元/分钟
        const std::vector<RateInputPoint> series = SteadySeries("10.0000", trueDrain, 11, 60);
        const RateEstimate e = dshb::EstimateRate(series);
        const double error = std::fabs(e.rateYuanPerMinute - (-trueDrain));
        const bool ok = e.status == RateStatus::Significant && error <= 1e-6 &&   // 1e-6 元/分钟容差
                        e.usablePoints == 11 && e.spanSeconds == 600 &&
                        e.rateYuanPerMinute < 0.0;
        h->Req("case1", "a steady synthetic drain (0.020000 yuan/min) is estimated as such",
               "expected=" + F6(-trueDrain) + " yuan/min, " + EstimateSummary(e) +
                   ", |error|=" + F6(error) + " (tolerance 0.000001)",
               ok);
        if (verbose) std::printf("  series: %s\n", SeriesSummary(series).c_str());
    }

    // -----------------------------------------------------------------------
    // (2) ★ The same series with one large TOP-UP inserted: the rate must not become
    //     negative-positive (i.e. must not read as "recovering") and must stay close to
    //     the true drain. 设计 §7.2：正跳变先剔除，充值不许读成负消费。
    //     The top-up fixture is +50.00 yuan -- if the jump were NOT removed, the fitted
    //     slope over that series would be wildly positive, so the near-exact result
    //     below is itself the evidence that the jump was removed.
    // -----------------------------------------------------------------------
    {
        const double trueDrain = 0.02;
        const std::vector<RateInputPoint> plain = SteadySeries("10.0000", trueDrain, 11, 60);
        const std::vector<RateInputPoint> topped = WithTopUp(plain, 7);   // big rise at point 7
        const RateEstimate e = dshb::EstimateRate(topped);
        const double error = std::fabs(e.rateYuanPerMinute - (-trueDrain));
        // The decisive assertions: NOT positive (no "recovering" reading), still
        // negative, and the true drain is recovered.
        // ★ Only the ONE point where the rise was detected comes out of the fit. The
        // samples after it are ordinary "balance changed" readings and stay in -- if the
        // rule dropped every later point instead, the span would collapse and one top-up
        // would make the estimator useless (that was the first version; case9 caught it).
        const bool notPositive = e.rateYuanPerMinute <= 0.0;
        const bool ok = e.status == RateStatus::Significant && notPositive && error <= 1e-6 &&
                        e.usablePoints == 10 && e.spanSeconds == 600;
        h->Req("case2", "one large TOP-UP inserted: the rate stays negative and near the true drain",
               "trueDrain=" + F6(-trueDrain) + ", " + EstimateSummary(e) +
                   ", usable=" + Num(e.usablePoints) + " of " + Num(e.pointsSeen) +
                   " (the single +50.00 yuan jump point was removed, the rest stayed), "
                   "|error|=" + F6(error),
               ok);
        if (verbose) std::printf("  with top-up: %s\n", SeriesSummary(topped).c_str());
    }

    // -----------------------------------------------------------------------
    // (3) Two points only -> Insignificant, and the wording says it cannot predict.
    //     设计 §7.3「少于 3 个下降样本」；§7.4「暂无法预测」。
    // -----------------------------------------------------------------------
    {
        std::vector<RateInputPoint> series;
        series.push_back(Dated(kAnchor, "10.0000"));
        series.push_back(Dated(kAnchor + 600, "9.0000"));   // 600 s apart: the SPAN is fine
        const RateEstimate e = dshb::EstimateRate(series);
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 900 * dshb::kUnitsPerYuan, kTextNow);
        const std::wstring expected = L"暂无法预测";
        const bool ok = e.status == RateStatus::Insignificant && e.usablePoints == 2 &&
                        e.spanSeconds == 600 && text.text == expected &&
                        text.kind == ZeroTimeKind::NoPrediction;
        h->Req("case3", "two points only -> Insignificant, and the wording cannot predict",
               "points=2 spanning " + Num(static_cast<long long>(e.spanSeconds)) +
                   "s (the SPAN alone would have been enough, so the refusal is about the "
                   "sample count), " + EstimateSummary(e) + ", text=" +
                   Show(text.text) + ", expected=" + Show(expected),
               ok);
    }

    // -----------------------------------------------------------------------
    // (4) A flat series -> rate zero, status not Significant, the wording is the dash.
    //     ★ This is the case that must never become "zero in 3 years": 10 points over
    //     10 minutes at one single value. 设计 §7.4「速率 ≤ 0 ->「—」」。
    //     ★ The expected dash is written as an ESCAPE (U+2014, em dash) on purpose: the
    //     characters U+2014 and U+2015 look identical on screen, and comparing against
    //     the wrong one is a failure that reads like a pass. Written as a codepoint,
    //     there is nothing to mistake.
    // -----------------------------------------------------------------------
    {
        const std::wstring expected = L"\u2014";   // em dash, U+2014 -- see the note above
        std::vector<RateInputPoint> series;
        for (int i = 0; i < 10; ++i) series.push_back(Dated(kAnchor + i * 60, "10.0000"));
        const RateEstimate e = dshb::EstimateRate(series);
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 10 * dshb::kUnitsPerYuan, kTextNow);
        const bool ok = e.status != RateStatus::Significant && e.rateYuanPerMinute == 0.0 &&
                        e.rawSlopeYuanPerMinute == 0.0 && e.usablePoints == 10 &&
                        text.text == expected && text.kind == ZeroTimeKind::Dash &&
                        text.minutesToZero < 0;
        h->Req("case4", "a flat series -> rate zero, not Significant, wording is the dash",
               EstimateSummary(e) + ", text=" + Show(text.text) +
                   ", expected=U+2014 " + Show(expected) + ", minutesToZero=" +
                   Num(text.minutesToZero) + " (none claimed)",
               ok);
    }

    // -----------------------------------------------------------------------
    // (5) A time span under 5 minutes -> Insignificant however many points there are.
    //     设计 §7.3「窗口跨度 < 5 分钟」。
    // -----------------------------------------------------------------------
    {
        // 12 points, obvious drain, but only 110 seconds of time: a beautiful slope that
        // must be refused. (12 points is more than the store even holds.)
        const std::vector<RateInputPoint> series = SteadySeries("10.0000", 0.02, 12, 10);
        const RateEstimate e = dshb::EstimateRate(series);
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 9 * dshb::kUnitsPerYuan, kTextNow);
        const bool ok = e.status == RateStatus::Insignificant && e.usablePoints == 12 &&
                        e.spanSeconds == 110 && e.spanSeconds < dshb::kRateMinSpanSeconds &&
                        text.kind == ZeroTimeKind::NoPrediction;
        h->Req("case5", "a span under 5 minutes -> Insignificant no matter how many points",
               "points=" + Num(e.pointsSeen) + ", span=" +
                   Num(static_cast<long long>(e.spanSeconds)) + "s < " +
                   Num(static_cast<long long>(dshb::kRateMinSpanSeconds)) + "s, " +
                   EstimateSummary(e) + ", text=" + Show(text.text),
               ok);
    }

    // -----------------------------------------------------------------------
    // (6) A series with points that have no time of their own -> Insignificant rather
    //     than a fabricated number. 老 curve.json（没有 "at"）读回来就是这样。
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
        //     a subset would still "work", which is exactly why it must not be used.
        std::vector<RateInputPoint> mixed = SteadySeries("10.0000", 0.02, 10, 60);
        mixed.insert(mixed.begin() + 5, Undated("9.5000"));
        const RateEstimate b = dshb::EstimateRate(mixed);
        const ZeroTimeText text = dshb::ZeroTimeFor(a, 10 * dshb::kUnitsPerYuan, kTextNow);

        const bool ok = a.status == RateStatus::Insignificant && a.usablePoints == 0 &&
                        a.rateYuanPerMinute == 0.0 && a.undatedPoints == 1 &&
                        b.status == RateStatus::Insignificant && b.undatedPoints == 1 &&
                        b.rateYuanPerMinute == 0.0 &&
                        text.kind == ZeroTimeKind::NoPrediction;
        h->Req("case6", "points without their own time -> Insignificant, never a fabricated rate",
               "all-undated: " + EstimateSummary(a) + " (one undated RUN, 6 points); "
               "one undated point among 10 dated ones: " + EstimateSummary(b) +
                   " -- the dated subset would have fitted fine and is deliberately not used; "
                   "text=" + Show(text.text),
               ok);
    }

    // -----------------------------------------------------------------------
    // (7) The spring: a step input converges towards the target and never overshoots.
    //     设计 §7.3 末尾（rate -> rate_display 的弹簧）+ §9.6（不许突变/不许过冲）。
    // -----------------------------------------------------------------------
    {
        const double target = -0.50;   // yuan/min, a step from 0
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
            // Monotone towards the target (never increases a negative-going step)...
            if (next > previous + 1e-15) monotone = false;
            // ...and never past it.
            if (next < target - 1e-12) overshoot = true;
            previous = next;
            display = next;
            if (framesTo63 < 0 && display <= target * 0.63) framesTo63 = i + 1;
            if (framesTo99 < 0 && display <= target * 0.99) framesTo99 = i + 1;
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
        const bool clamped = huge > target && huge < 0.0;
        const bool backward = dshb::RateSpringStep(-0.25, target, -1.0) == -0.25;

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
    //     (The task numbers 7 checks; this one exists because a wording that is never
    //     exercised is a wording nobody has read. It is the §7.4 table, end to end.)
    // -----------------------------------------------------------------------
    {
        // A significant drain of 2.00 yuan/min: 120 yuan left -> 60 minutes.
        const std::vector<RateInputPoint> series = SteadySeries("130.0000", 2.00, 11, 60);
        const RateEstimate e = dshb::EstimateRate(series);

        const ZeroTimeText usedUp = dshb::ZeroTimeFor(e, 0, kTextNow);
        const ZeroTimeText overWeek = dshb::ZeroTimeFor(e, 30000 * dshb::kUnitsPerYuan, kTextNow);
        const ZeroTimeText twoHours = dshb::ZeroTimeFor(e, 240 * dshb::kUnitsPerYuan, kTextNow);
        const ZeroTimeText shortRemainder = dshb::ZeroTimeFor(e, 6 * dshb::kUnitsPerYuan, kTextNow);

        const std::wstring wantUsedUp = L"已用尽";
        const std::wstring wantOver = L"超过 7 天";
        const std::wstring wantDash = L"\u2014";   // em dash U+2014 (never U+2015)
        const bool usedUpOk = usedUp.kind == ZeroTimeKind::UsedUp && usedUp.text == wantUsedUp;
        const bool overOk = overWeek.kind == ZeroTimeKind::OverSevenDays && overWeek.text == wantOver;
        // 240 yuan / 2.00 per min = 120 min = 2 hours. ★ The expected line is built from
        // the two halves the design doc names, because at 2 hours the remainder is
        // UNDER the 6-hour bar, so §7.4 requires the absolute local time to be in the
        // same line: 「按当前速度，约 2 小时后归零」+「约 22:27」. An earlier version of this
        // check demanded the bare relative string and failed a correct implementation --
        // the fixture was wrong, not the code. The relative half stays spelled out in
        // full (the exact Chinese wording of §7.4); only the clock half is computed.
        const std::wstring wantRelative = L"按当前速度，约 2 小时后归零";
        const bool twoHoursOk = twoHours.kind == ZeroTimeKind::Extrapolated &&
                                twoHours.minutesToZero == 120 &&
                                twoHours.text.rfind(wantRelative, 0) == 0 &&
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
                        branchRedraws && owedNone && e.status == RateStatus::Significant;
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
    // (9) ★ A balance that is genuinely RISING (§7.2's recovery): every point of the rise
    //     is a positive jump, so after the removal the surviving samples are the earlier
    //     DECLINE only -- 3 points over 240 s, which is under the 5-minute bar.
    //     ★ THIS CHECK RECORDS A CONFLICT rather than a happy path, and it is written to
    //     assert what the code REALLY does, because that is what a probe is for:
    //       - the owner's rules are "positive jumps are removed first and must never
    //         contribute" + "insignificant when the span is under 5 minutes";
    //       - together they mean a rising balance can never produce a rate, so §7.4's
    //         「速率 ≤ 0 ->「—」」 branch is unreachable via a POSITIVE rate: the only route
    //         to the dash is a FLAT balance (rate exactly 0), which case4 covers.
    //     So this case asserts Insignificant / 「暂无法预测」, NOT the dash. If the owner
    //     wants a rising balance to read as the dash instead, the rule to change is the
    //     span bar (or jump handling for a wholly-rising window) -- see the report.
    // -----------------------------------------------------------------------
    {
        std::vector<RateInputPoint> series;
        series.push_back(Dated(kAnchor, "10.0000"));
        series.push_back(Dated(kAnchor + 120, "9.8000"));
        series.push_back(Dated(kAnchor + 240, "9.6000"));
        series.push_back(Dated(kAnchor + 360, "11.0000"));   // +1.40 top-up: a jump, removed
        series.push_back(Dated(kAnchor + 480, "11.2000"));   // rising: also a jump, removed
        series.push_back(Dated(kAnchor + 600, "11.4000"));   // rising: also a jump, removed
        const RateEstimate e = dshb::EstimateRate(series);
        const ZeroTimeText text = dshb::ZeroTimeFor(e, 11 * dshb::kUnitsPerYuan, kTextNow);
        const std::wstring wantInsignificant = L"暂无法预测";
        const bool ok = e.status == RateStatus::Insignificant && e.rateYuanPerMinute == 0.0 &&
                        e.usablePoints == 3 && e.spanSeconds == 240 &&
                        text.kind == ZeroTimeKind::NoPrediction && text.text == wantInsignificant;
        h->Req("case9", "a rising balance: every rise is a jump, so the survivor is under the bar",
               "series 10.00, 9.80, 9.60, 11.00, 11.20, 11.40 over 600s -> " + EstimateSummary(e) +
                   ", text=" + Show(text.text) +
                   ". CONFLICT RECORDED: §7.4's dash (rate <= 0) cannot be reached by a "
                   "positive rate under these rules; only a flat balance reaches it (case4)",
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
