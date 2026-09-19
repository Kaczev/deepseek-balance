// panelprobe -- the panel's curve DISPLAY SET, and why it is not the store's capacity.
//
//   panelprobe            run every check (the default)
//   panelprobe --verbose  also print the assembled point list
//
// The property under test, in one sentence: **the panel draws the newest 12 points of
// whatever the store holds, never more**, so raising CurveStore::kCapacity from 12 to 120
// changes the data layer and leaves the panel alone.
//
// Why this needs its own probe instead of one more case in storeprobe: storeprobe does
// not link widget_display.cpp (that layer pulls in Win32 and the ambience model), and the
// property under test is a *display* property that a *store* constant can break. The
// failure it guards is specific and was real: BuildFrameCurve used to feed the whole
// store into an 11-slot layout, which was correct only as long as the two numbers
// happened to be equal. With a 120-point ring and no slice, all 120 points get laid out
// on 11 slots and the curve collapses into a thread.
//
// ★ How it reaches the layer: through the real file path (Save -> SetCurveStorePath ->
//   Load), because that is the only door the display layer opens. A probe that reached
//   into an anonymous-namespace global would be testing a copy of the code rather than
//   the code.
// ★ What this probe does NOT do: render pixels. It checks the point list the display
//   layer hands to the renderer, which is the input to every pixel. The pixel comparison
//   across two capacities is a separate manual step (two builds of dshb.exe, one exported
//   PNG each) and is reported as such.
#include "widget_display.h"

#include "amount.h"
#include "curve_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

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
};

std::string Num(long long value) { return std::to_string(value); }

std::string F4(double value) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.4f", value);
    return buf;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string TempPath(const char* name) {
    wchar_t dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, dir) == 0) return std::string();
    const std::wstring wideName = Wide(name);
    const std::wstring path = std::wstring(dir) + L"dshb-panelprobe-" + wideName + L".json";
    const int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

// A curve store built through the real Append() path, so "only a change becomes a point"
// applies exactly as it does in production. Every value is distinct on purpose: a repeated
// value appends nothing and the point count would not be the count asked for.
void FeedStore(dshb::CurveStore* store, int count, double first, double step, int64_t firstAt,
               int64_t atStep) {
    for (int i = 0; i < count; ++i) {
        dshb::CurveObservation obs;
        obs.primaryCurrency = "CNY";
        dshb::CurveObservation::Item item;
        item.currency = "CNY";
        item.amountOk = true;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", first + step * i);
        item.text = buf;
        obs.observations.push_back(item);
        store->Append(obs, firstAt + atStep * i, std::string());
    }
}

// Writes the store to a temp file and lets the display layer load it -- the same door
// production uses. Returns the path, or "" on failure.
std::string Install(const dshb::CurveStore& store, const char* name) {
    const std::string path = TempPath(name);
    if (path.empty()) return std::string();
    if (!store.Save(path)) return std::string();
    dshb::SetCurveStorePath(Wide(path));
    return path;
}

// Feed ONE dated point. Same door as production (Append stamps the point with the time it is
// given), and the amount is passed as a decimal STRING so nothing here goes through a double.
void FeedPoint(dshb::CurveStore* store, const char* amount, int64_t at) {
    dshb::CurveObservation obs;
    obs.primaryCurrency = "CNY";
    dshb::CurveObservation::Item item;
    item.currency = "CNY";
    item.amountOk = true;
    item.text = amount;
    obs.observations.push_back(item);
    store->Append(obs, at, std::string());
}

// The text the panel would show for today's spend with the display layer's clock pinned at
// `nowSeconds` -- the whole point: the answer must not depend on when this probe runs.
std::string TodayLineAt(const dshb::CurveStore& store, const char* name, int64_t nowSeconds) {
    if (Install(store, name).empty()) return std::string("<install failed>");
    dshb::SetTodayUsageNowForProbe(nowSeconds);
    dshb::DisplayedAmount display;
    const dshb::WidgetFrame frame =
        dshb::BuildWidgetFrame(dshb::ConnState::Ok, display, true, L"\u00A5");
    return frame.todayUsageText;
}

// The same line in UTF-8, for evidence strings that are read by a person.
std::string Encode(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

// The point list the panel would draw for whatever the display layer currently holds.
std::vector<dshb::CurvePoint> PanelPoints() {
    dshb::DisplayedAmount display;
    const dshb::WidgetFrame frame = dshb::BuildWidgetFrame(
        dshb::ConnState::ColdStart, display, false, L"");
    return frame.curve;
}

// The width of the DISPLAY SET: the newest kCurveDisplayPoints+1 points are taken from the
// store, of which kCurveDisplayPoints are ON the panel -- the extra one is the incoming
// point at x = 1.1, just off the right edge, which slides in on the next append.
// Both numbers are asked of the display layer rather than restated here.
constexpr std::size_t kDisplaySet = dshb::kCurveDisplayPoints + 1;   // 12
constexpr std::size_t kVisible = dshb::kCurveDisplayPoints;          // 11

// The slots the layout puts the display set on, and therefore the x values a correct frame
// carries: the LAST kVisible of them are the ones on the panel ([0,1]), the extra one sits
// at 1.1. Spacing is one slot = 0.1 in normalized units (widget_display.cpp's SlotX).
constexpr double kSlotStep = 0.1;

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose") {
            verbose = true;
        } else {
            std::printf("panelprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: panelprobe [--verbose]\n");
            return 64;
        }
    }
    SetConsoleOutputCP(CP_UTF8);

    Harness h;
    std::printf("panelprobe: the panel's curve display set vs CurveStore::kCapacity\n");
    std::printf("tuning: CurveStore::kCapacity=%zu, kCurveDisplayPoints=%zu (visible), display "
                "set=%zu\n",
                dshb::CurveStore::kCapacity, kVisible, kDisplaySet);
    if (dshb::CurveStore::kCapacity <= kDisplaySet) {
        std::printf("FAIL: setup | the store capacity (%zu) must EXCEED the display set (%zu), "
                    "or this probe cannot tell a slice from a pass-through\n",
                    dshb::CurveStore::kCapacity, kDisplaySet);
        return 1;
    }

    const int64_t now = static_cast<int64_t>(::time(nullptr));

    // -----------------------------------------------------------------------
    // (1) A store holding more points than the panel can show. The frame must carry exactly
    //     the 11 visible points, evenly one slot apart from x=0 to x=1.
    //     ★ The x values are the discriminator: with the whole store laid out instead of
    //     the newest 12, the spacing becomes 1/(n-1) and the panel's curve collapses into a
    //     thread. The oldest 13 points sit at 1000.xx and the newest 12 at 20.xx, so a
    //     pass-through would also move the y values.
    // -----------------------------------------------------------------------
    {
        dshb::CurveStore store;
        FeedStore(&store, 13, 1000.00, 0.01, now - 2000, 60);
        FeedStore(&store, 12, 20.00, -0.01, now - 720, 60);
        const std::string path = Install(store, "big");
        const std::vector<dshb::CurvePoint> pts = PanelPoints();

        const bool count = pts.size() == kVisible;
        bool slots = count;
        for (std::size_t i = 0; slots && i < pts.size(); ++i) {
            const double want = static_cast<double>(i) * kSlotStep;
            if (pts[i].x < want - 1e-5 || pts[i].x > want + 1e-5) slots = false;
        }
        // ★ y: 0 = top of the band, 1 = bottom (widget_display.cpp's NormY puts the MAXIMUM
        //   at the top). The drawn span is the newest 12 values, 20.00 .. 19.89, so the
        //   staircase runs x=0 -> y=0.0 (the oldest visible sample, the maximum) down to
        //   x=1 -> y=1.0 (the newest, the minimum). That staircase is the second signature
        //   of "only the newest 12 are here": a pass-through would pull 1000.xx into the
        //   span and flatten all eleven visible y values onto the bottom sliver of the band.
        const bool extremes = count && pts.front().y == 0.0f && pts.back().y == 1.0f;
        bool staircase = extremes;
        for (std::size_t i = 0; staircase && i < pts.size(); ++i) {
            const double want = static_cast<double>(i) * kSlotStep;
            if (pts[i].y < want - 1e-5 || pts[i].y > want + 1e-5) staircase = false;
        }
        const bool span = count && store.size() == 25;

        const bool ok = count && slots && staircase && span && !path.empty();
        std::string xs;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (i != 0) xs += ",";
            xs += F4(pts[i].x);
        }
        std::string ys;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (i != 0) ys += ",";
            ys += F4(pts[i].y);
        }
        h.Req("case1", "a store with 25 points still yields exactly the 11 visible points, one "
                       "slot apart",
               "store=25 points (capacity " +
                   Num(static_cast<long long>(dshb::CurveStore::kCapacity)) +
                   "), frame curve=" + Num(static_cast<long long>(pts.size())) + " points; x=" +
                   xs + "; y=" + ys +
                   " (a pass-through would space 25 points by 1/24 and put 1000.xx at y=0)",
               ok);
        if (verbose) {
            for (std::size_t i = 0; i < pts.size(); ++i) {
                std::printf("    pt[%zu] x=%s y=%s\n", i, F4(pts[i].x).c_str(), F4(pts[i].y).c_str());
            }
        }
    }

    // -----------------------------------------------------------------------
    // (2) The SAME newest 12 points, held in a store that holds exactly 12: the frame must
    //     be identical point for point. This is the property that makes the capacity change
    //     invisible on screen.
    // -----------------------------------------------------------------------
    {
        dshb::CurveStore big;
        FeedStore(&big, 13, 1000.00, 0.01, now - 2000, 60);
        FeedStore(&big, 12, 20.00, -0.01, now - 720, 60);
        (void)Install(big, "big2");
        const std::vector<dshb::CurvePoint> a = PanelPoints();

        dshb::CurveStore small;
        FeedStore(&small, 12, 20.00, -0.01, now - 720, 60);
        (void)Install(small, "small");
        const std::vector<dshb::CurvePoint> b = PanelPoints();

        bool identical = a.size() == b.size() && !a.empty();
        for (std::size_t i = 0; identical && i < a.size(); ++i) {
            identical = a[i].x == b[i].x && a[i].y == b[i].y && a[i].hasColor == b[i].hasColor;
        }
        const bool ok = identical && small.size() == 12;
        h.Req("case2", "the frame is identical whether the store holds 12 points or 25",
               "store sizes " + Num(static_cast<long long>(big.size())) + " vs " +
                   Num(static_cast<long long>(small.size())) + " -> frame sizes " +
                   Num(static_cast<long long>(a.size())) + " vs " +
                   Num(static_cast<long long>(b.size())) + " (" + Num(kVisible) +
                   " visible each), identical=" +
                   std::string(identical ? "yes (x, y and hasColor all equal)" : "NO"),
               ok);
    }

    // -----------------------------------------------------------------------
    // (3) A store holding fewer points than the panel can show: the frame carries what there
    //     is, plus the left-edge fill point the layout prepends while the oldest point is
    //     not yet at x = 0. Without this, the slice could be written as "always take 12" and
    //     a short series would silently draw nothing.
    // -----------------------------------------------------------------------
    {
        dshb::CurveStore store;
        FeedStore(&store, 5, 20.00, -0.01, now - 300, 60);
        (void)Install(store, "small5");
        const std::vector<dshb::CurvePoint> pts = PanelPoints();
        // 5 measured points sit in slots 6..10 -> x = 0.6..1.0, and the layout flattens the
        // stretch to their left with one fill point at x = 0.
        const bool tail = pts.size() == 6 && pts.size() >= store.size();
        const bool fill = tail && pts[0].x == 0.0f && !pts[0].hasColor &&
                          pts[0].y == pts[1].y;
        const bool kept = tail && pts[1].x == 0.6f && pts.back().x == 1.0f;
        const bool ok = store.size() == 5 && tail && fill && kept;
        h.Req("case3", "a store with 5 points keeps all 5 and pads only the left edge",
               "store=5 points -> frame curve=" + Num(static_cast<long long>(pts.size())) +
                   " entries (5 measured + 1 left-edge fill), x=" +
                   (pts.size() >= 2 ? F4(pts[1].x) + ".." + F4(pts.back().x) : std::string("-")) +
                   ", fill at x=" + (pts.empty() ? std::string("-") : F4(pts[0].x)) +
                   " hasColor=" + (pts.empty() ? "-" : (pts[0].hasColor ? "yes" : "no")),
               ok);
    }

    // -----------------------------------------------------------------------
    // (4) The restart transition's START POINT (CurveStartBalance): what it must hand back,
    //     and what it must hand back when there is nothing to hand back.
    //
    //     ★ Why this belongs here rather than in storeprobe: the function lives in
    //     widget_display.cpp, and the failure it guards is a *restart* failure. The store
    //     records a point ONLY when the value changes, so the newest point and the live
    //     balance are the same number whenever nothing happened while the app was closed --
    //     and getting that case wrong made the widget perform a fall it had not had.
    //     The whole fix rests on this function returning an amount that can be compared
    //     EXACTLY (1/10000 yuan, Amount::raw) against the incoming sample, so a lossy
    //     round trip through "yuan as a double" would put the bug back.
    //
    //     A single-point store is the resting shape of a real curve.json: quit after one
    //     recorded change, or quit after a quiet hour. The value read back out of the FILE
    //     must equal the value that went in, or "the balance did not change" cannot be
    //     recognized as equality.
    //
    //     What this case does NOT cover: whether main.cpp's CommitDelayed actually treats
    //     equality as "do not play". That is one `!=` on these two values, and the
    //     end-to-end evidence for it is the widget's own `[commit]` log line.
    // -----------------------------------------------------------------------
    {
        dshb::CurveStore store;
        FeedStore(&store, 1, 48.80, 0.0, now - 600, 60);
        (void)Install(store, "single48");
        dshb::Amount start{};
        const bool have = dshb::CurveStartBalance(&start);
        // The same number the API would hand the widget, parsed the same way.
        dshb::Amount current{};
        const bool parsed = dshb::ParseAmount("48.80", &current);
        const bool equal = have && parsed && start == current;
        h.Req("case4a", "a one-point store hands back that point's amount, exactly "
                        "(Amount::raw), so \"unchanged\" is recognizable as equality",
               "stored \"48.80\" + one token -> CurveStartBalance=" +
                   std::string(have ? start.ToString2() : "(none)") + " raw=" +
                   Num(have ? static_cast<long long>(start.raw) : -1) + "; incoming \"48.80\" raw=" +
                   Num(parsed ? static_cast<long long>(current.raw) : -1) +
                   " -> equality=" + (equal ? "yes" : "NO"),
               equal);

        // Same store, and the one case that must still play: the balance really moved.
        dshb::Amount lower{};
        const bool parsedLower = dshb::ParseAmount("47.72", &lower);
        const bool differs = have && parsedLower && start != lower;
        h.Req("case4b", "the same start point is NOT equal to a lower balance, so the restart "
                        "transition still has both endpoints and still plays",
               "start \"48.80\" raw=" + Num(have ? static_cast<long long>(start.raw) : -1) +
                   " vs \"47.72\" raw=" +
                   Num(parsedLower ? static_cast<long long>(lower.raw) : -1) +
                   " -> different=" + (differs ? "yes" : "NO"),
               differs);

        // No store at all: nothing to hand back, and the caller must fall back to "show the
        // first sample directly" rather than invent a start value.
        dshb::CurveStore empty;
        (void)Install(empty, "none");
        dshb::Amount fromEmpty{};
        const bool got = dshb::CurveStartBalance(&fromEmpty);
        h.Req("case4c", "an empty store reports \"no start point\" instead of inventing one",
               std::string("0 points -> CurveStartBalance returned ") +
                   (got ? "true (WRONG)" : "false"),
               !got);
    }

    // -----------------------------------------------------------------------
    // (5) "今日已X.XX¥" -- today's spend, on the title row.
    //
    //     The rule is the owner's: a CALENDAR DAY in local time, and only the DECREASES
    //     count (a top-up must not shrink the number and it must never go negative).
    //
    //     The clock is pinned (SetTodayUsageNowForProbe) and the fixture is built from
    //     LOCAL MIDNIGHT of that pinned instant, so this case gives the same answer today,
    //     tomorrow, and in any timezone. That is why it is a probe case rather than a
    //     screenshot: an exported frame would only be reproducible on one day.
    //
    //     Every sub-case is an exact string comparison, so a wrong sum, a missing "0.00",
    //     a value that has a top-up subtracted, or "--.--" where a number belongs all fail.
    // -----------------------------------------------------------------------
    {
        // 2026-09-19 12:00 local. Any instant works; this one is fixed so the evidence is
        // readable and the arithmetic below (midnight, +5 min, +2 h, ...) can be checked.
        const int64_t kNow = 1789790400;   // 2026-09-19T12:00:00+08:00
        const int64_t midnight = dshb::TodayStartSeconds(kNow);
        const std::string localDay = [] {
            const std::time_t t = static_cast<std::time_t>(1789790400);
            std::tm local{};
            char buf[48];
            if (localtime_s(&local, &t) != 0) return std::string("(localtime_s failed)");
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                          local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                          local.tm_min, local.tm_sec);
            return std::string(buf);
        }();

        // (5a) Three drops and one top-up inside the day. Drops: 0.50 + 1.00 + 0.20 + 0.30 =
        //      2.00; the 5.00 top-up in the middle must contribute nothing.
        {
            dshb::CurveStore store;
            FeedPoint(&store, "20.00", midnight - 180);   // yesterday 23:57 -- the baseline
            FeedPoint(&store, "19.50", midnight + 300);
            FeedPoint(&store, "18.50", midnight + 900);
            FeedPoint(&store, "23.50", midnight + 1200);  // top-up: +5.00
            FeedPoint(&store, "23.30", midnight + 1800);
            FeedPoint(&store, "23.00", midnight + 2400);
            const std::string got = TodayLineAt(store, "today-drops", kNow);
            // A SPACE between 已 and the number (owner, 2026-09-19: first asked to drop it, then
            // asked for it back: "我感觉没有空格好奇怪"). The string is built by concatenation
            // because "\xB22" would lex as ONE hex escape -- and that matters here: the space has
            // to sit inside its own literal, never right after a hex escape.
            const std::string want = "\xE4\xBB\x8A\xE6\x97\xA5\xE5\xB7\xB2 " "2.00\xC2\xA5";
            std::printf("    today fixture (local day %s, midnight=%lld): 20.00@-3min, 19.50@+5min, "
                        "18.50@+15min, 23.50@+20min (top-up), 23.30@+30min, 23.00@+40min\n",
                        localDay.c_str(), static_cast<long long>(midnight));
            h.Req("case5a", "today's spend is the sum of the day's DROPS only (a top-up adds 0)",
                   "drops 0.50+1.00+0.20+0.30 = 2.00, top-up 5.00 ignored -> \"" + got + "\"",
                   got == want);
        }

        // (5b) Only increases today: the answer is a NUMBER (0.00), not the placeholder.
        //      "--.--" means "cannot be computed" and the store here can compute it.
        {
            dshb::CurveStore store;
            FeedPoint(&store, "10.00", midnight - 60);
            FeedPoint(&store, "12.00", midnight + 600);
            FeedPoint(&store, "15.00", midnight + 1200);
            const std::string got = TodayLineAt(store, "today-up", kNow);
            const std::string want = "\xE4\xBB\x8A\xE6\x97\xA5\xE5\xB7\xB2 " "0.00\xC2\xA5";
            h.Req("case5b", "a day with no drops shows 0.00, not the --.-- placeholder",
                   "one baseline + two top-ups (no drop) -> \"" + got + "\"", got == want);
        }

        // (5c) Nothing dated today at all: the value cannot be computed, and the owner's own
        //      placeholder shape is what must appear.
        {
            dshb::CurveStore store;
            FeedPoint(&store, "20.00", midnight - 7200);
            FeedPoint(&store, "19.00", midnight - 3600);
            const std::string got = TodayLineAt(store, "today-none", kNow);
            const std::string want = "\xE4\xBB\x8A\xE6\x97\xA5\xE5\xB7\xB2 --.--\xC2\xA5";
            h.Req("case5c", "a store with no dated point for today shows --.--",
                   "both points are yesterday -> \"" + got + "\"", got == want);
        }

        // (5d) The midnight boundary itself, with the two points that bracket it. Both are
        //      DROPS (1.00 before midnight, 2.00 after), and only the second one is today --
        //      so a baseline taken from the wrong side of midnight gives 1.00 or 3.00.
        {
            dshb::CurveStore store;
            FeedPoint(&store, "20.00", midnight - 1800);
            FeedPoint(&store, "19.00", midnight - 60);    // yesterday's drop: NOT today
            FeedPoint(&store, "17.00", midnight + 600);   // today's drop from 19.00: 2.00
            const std::string got = TodayLineAt(store, "today-midnight", kNow);
            const std::string want = "\xE4\xBB\x8A\xE6\x97\xA5\xE5\xB7\xB2 " "2.00\xC2\xA5";
            h.Req("case5d", "the day boundary is local midnight: a drop before it is not counted, "
                            "and the balance it left is today's baseline",
                   "20.00@-30min, 19.00@-1min, 17.00@+10min; only 19.00->17.00 is today -> \"" +
                       got + "\"",
                   got == want);
        }

        // Restore the real clock for anything that runs after this case.
        dshb::SetTodayUsageNowForProbe(0);
    }

    std::printf("checks: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& failure : h.failures) std::printf("failed line: %s\n", failure.c_str());
    std::printf("RESULT: %s\n", h.failed == 0 && h.passed > 0 ? "PASS" : "FAIL");
    return (h.failed == 0 && h.passed > 0) ? 0 : 1;
}
