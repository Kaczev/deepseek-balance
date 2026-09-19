// ratebaseline -- OLD formula vs NEW formula, side by side, on real files.
//
//   ratebaseline --curve=<curve.json>     the owner's real curve store
//   ratebaseline --series=<a,b,c,...>     a synthetic balance series, one value per step
//   ratebaseline --step=<seconds>         seconds between the synthetic points (default 60)
//   ratebaseline --now=<epochSeconds>     the "now" the wording uses (default: newest point)
//
// Goes through the REAL data path: dshb::CurveStore loads the file (so the 86400 s
// invalidation rule and the undated-point rule apply exactly as in production),
// dshb::RateInputForCurrency's currency rule is reproduced verbatim from
// widget_display.cpp, and both estimators + ZeroTimeFor are the ones the widget links.
//
// Why this tool exists: the change from "Σdrops ÷ span" to "Theil–Sen median slope" is
// invisible in a probe that only checks one formula in isolation. What has to be seen is
// the two numbers next to each other on the SAME series, together with the sentence each
// one produces, so a reader can judge whether the change did what it was meant to do.
//
// Prints one block per series; every line starts with the case it belongs to.
#include "amount.h"
#include "curve_store.h"
#include "rate_estimator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string Num(long long value) { return std::to_string(value); }

std::string F6(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return buf;
}

std::string F9(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9f", value);
    return buf;
}

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

// The currency rule from widget_display.cpp's RateInputForCurrency, reproduced here
// verbatim (the display layer's copy is in an anonymous namespace and cannot be linked).
std::vector<dshb::RateInputPoint> RateInputForCurrency(const dshb::CurveStore& store,
                                                       const std::string& currency) {
    const std::vector<dshb::CurveStorePoint> points = store.Points();
    std::vector<dshb::RateInputPoint> out;
    out.reserve(points.size());
    for (const dshb::CurveStorePoint& point : points) {
        dshb::RateInputPoint p;
        p.at = point.at;
        p.atValid = point.atValid;
        const dshb::CurveStorePoint::Entry* entry = nullptr;
        if (currency.empty()) {
            for (const dshb::CurveStorePoint::Entry& e : point.entries) {
                if (!e.missing && !e.text.empty()) { entry = &e; break; }
            }
        } else {
            entry = point.Find(currency);
        }
        if (entry != nullptr && !entry->missing && !entry->text.empty()) {
            dshb::Amount amount;
            if (dshb::ParseAmount(entry->text, &amount)) {
                p.amountRaw = amount.raw;
                p.amountValid = true;
            }
        }
        out.push_back(p);
    }
    return out;
}

void Report(const std::string& label, const std::string& sourceLine,
            const std::vector<dshb::RateInputPoint>& series, int64_t balanceRaw, int64_t nowSeconds) {
    const dshb::RateEstimate oldEstimate = dshb::EstimateRate(series);
    const dshb::RateEstimate newEstimate = dshb::EstimateRateTheilSen(series);
    const dshb::ZeroTimeText oldText = dshb::ZeroTimeFor(oldEstimate, balanceRaw, nowSeconds);
    const dshb::ZeroTimeText newText = dshb::ZeroTimeFor(newEstimate, balanceRaw, nowSeconds);

    std::printf("\n=== %s ===\n", label.c_str());
    std::printf("  source      : %s\n", sourceLine.c_str());
    std::printf("  balance     : %s yuan (raw=%lld)\n",
                dshb::Amount{balanceRaw}.ToString2().c_str(), static_cast<long long>(balanceRaw));
    std::printf("  series      : %d point(s), %d usable\n", oldEstimate.pointsSeen,
                newEstimate.usablePoints);
    std::printf("  OLD formula : rate=%s yuan/min  status=%s  span=%llds  drops=%s yuan\n",
                F9(oldEstimate.rateYuanPerMinute).c_str(),
                dshb::RateStatusName(oldEstimate.status),
                static_cast<long long>(oldEstimate.spanSeconds),
                dshb::Amount{oldEstimate.dropSumRaw}.ToString2().c_str());
    std::printf("  OLD wording : \"%s\"  (kind=%s)\n", Utf8(oldText.text).c_str(),
                dshb::ZeroTimeKindName(oldText.kind));
    std::printf("  NEW formula : rate=%s yuan/min  status=%s  pairs=%lld  medianSlope=%s yuan/s\n",
                F9(newEstimate.rateYuanPerMinute).c_str(),
                dshb::RateStatusName(newEstimate.status),
                static_cast<long long>(newEstimate.pairCount),
                F9(newEstimate.medianSlopeYuanPerSecond).c_str());
    std::printf("  NEW wording : \"%s\"  (kind=%s)\n", Utf8(newText.text).c_str(),
                dshb::ZeroTimeKindName(newText.kind));
    if (oldEstimate.rateYuanPerMinute > 0.0 && newEstimate.rateYuanPerMinute > 0.0) {
        std::printf("  ratio       : NEW/OLD = %s\n",
                    F6(newEstimate.rateYuanPerMinute / oldEstimate.rateYuanPerMinute).c_str());
    }
    std::printf("  OLD note    : %s\n", oldEstimate.note.c_str());
    std::printf("  NEW note    : %s\n", newEstimate.note.c_str());
}

// "a,b,c" -> the values. Whitespace around each item is ignored.
std::vector<std::string> SplitValues(const std::string& spec) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= spec.size()) {
        std::size_t comma = spec.find(',', begin);
        if (comma == std::string::npos) comma = spec.size();
        std::string item = spec.substr(begin, comma - begin);
        std::size_t b = 0;
        std::size_t e = item.size();
        while (b < e && (item[b] == ' ' || item[b] == '\t')) ++b;
        while (e > b && (item[e - 1] == ' ' || item[e - 1] == '\t')) --e;
        item = item.substr(b, e - b);
        if (!item.empty()) out.push_back(item);
        begin = comma + 1;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string curvePath;
    std::string seriesSpec;
    std::string currency;
    int64_t step = 60;
    int64_t nowSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--curve=", 8) == 0) {
            curvePath = arg.substr(8);
        } else if (arg.rfind("--series=", 9) == 0) {
            seriesSpec = arg.substr(9);
        } else if (arg.rfind("--currency=", 11) == 0) {
            currency = arg.substr(11);
        } else if (arg.rfind("--step=", 7) == 0) {
            step = std::atoll(arg.c_str() + 7);
        } else if (arg.rfind("--now=", 6) == 0) {
            nowSeconds = std::atoll(arg.c_str() + 6);
        } else {
            std::printf("ratebaseline: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: ratebaseline --curve=<curve.json> | --series=v0,v1,... "
                        "[--step=seconds] [--currency=CNY] [--now=epochSeconds]\n");
            return 64;
        }
    }
    if (curvePath.empty() && seriesSpec.empty()) {
        std::printf("ratebaseline: give --curve=<curve.json> or --series=v0,v1,...\n");
        return 64;
    }

    // ---- the real file, through the real store ----------------------------------
    if (!curvePath.empty()) {
        dshb::CurveStore store;
        const dshb::CurveLoadResult loaded = store.Load(curvePath);
        std::printf("curve.json: status=%s ok=%s pointsLoaded=%d discarded=%d detail=%s\n",
                    loaded.StatusName(), loaded.ok ? "yes" : "no", loaded.pointsLoaded,
                    loaded.pointsDiscarded, loaded.detail.c_str());
        if (!loaded.ok) {
            std::printf("ratebaseline: the file did not load; nothing to compare.\n");
            return 1;
        }
        const std::vector<dshb::CurveStorePoint> points = store.Points();
        std::printf("curve.json: %zu point(s) in the ring after Load (capacity %zu)\n",
                    points.size(), dshb::CurveStore::kCapacity);
        std::string spanLine = "            at:";
        for (const dshb::CurveStorePoint& point : points) {
            spanLine += " " + (point.atValid ? Num(static_cast<long long>(point.at)) : std::string("undated"));
        }
        std::printf("%s\n", spanLine.c_str());
        if (points.size() >= 2 && points.front().atValid && points.back().atValid) {
            std::printf("            first-to-newest span: %lld s (%s min)\n",
                        static_cast<long long>(points.back().at - points.front().at),
                        F6(static_cast<double>(points.back().at - points.front().at) / 60.0).c_str());
        }

        const std::vector<dshb::RateInputPoint> series = RateInputForCurrency(store, currency);
        // The balance the prediction divides by: the newest usable point's amount.
        int64_t balanceRaw = 0;
        for (const dshb::RateInputPoint& point : series) {
            if (point.amountValid) balanceRaw = point.amountRaw;
        }
        const int64_t now = (nowSeconds != 0) ? nowSeconds
                                             : (points.empty() ? 0 : points.back().at);
        Report("REAL curve.json" + (currency.empty() ? std::string() : " [" + currency + "]"),
               curvePath, series, balanceRaw, now);
    }

    // ---- a synthetic series given on the command line ---------------------------
    if (!seriesSpec.empty()) {
        const std::vector<std::string> values = SplitValues(seriesSpec);
        std::vector<dshb::RateInputPoint> series;
        // Synthetic time base: the points are spaced `step` seconds apart and end "now",
        // so that the wording's absolute clock lands on a plausible local time. The
        // estimators never read a clock; only ZeroTimeFor's HH:MM does.
        const int64_t endAt = (nowSeconds != 0) ? nowSeconds : 1789750101;
        const int64_t total = static_cast<int64_t>(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            dshb::Amount amount;
            if (!dshb::ParseAmount(values[i], &amount)) {
                std::printf("ratebaseline: \"%s\" is not a decimal amount\n", values[i].c_str());
                return 65;
            }
            dshb::RateInputPoint point;
            point.at = endAt - (total - 1 - static_cast<int64_t>(i)) * step;
            point.atValid = true;
            point.amountRaw = amount.raw;
            point.amountValid = true;
            series.push_back(point);
        }
        const int64_t balanceRaw = series.empty() ? 0 : series.back().amountRaw;
        Report("SYNTHETIC series (" + Num(static_cast<long long>(values.size())) + " points, step " +
                   Num(step) + " s)",
               "command line --series", series, balanceRaw, endAt);
    }

    return 0;
}
