// capacityprobe -- the measured cost of the curve ring's size, and the per-frame paths that
// must not copy it.
//
//   capacityprobe            run every measurement (the default)
//   capacityprobe --repeat=N multiply every timed loop N times (default 1)
//
// Why this is a target of its own: "the store now holds a whole day of changes" is a claim about TIME
// as well as about space, and the two numbers that matter -- the size of a store on the
// stack, and what a full-ring copy costs per frame -- belong to the store and to the display
// layer at once. Neither storeprobe (no display layer) nor panelprobe (checks points, not
// time) can answer them.
//
// ★ It measures the REAL paths: the store it times is the one widget_display.cpp holds
//   (loaded through the same file door production uses), and the frame it times is built by
//   BuildWidgetFrame, the function the widget calls every frame.
// ★ What it does NOT do: guess an "old" implementation to compare against. The one number
//   that needed the old shape -- a whole-ring copy -- is measured directly through
//   Points(), and the per-frame paths are measured as they are now.
#include "widget_display.h"

#include "amount.h"
#include "curve_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

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

std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string TempPath() {
    wchar_t dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, dir) == 0) return std::string();
    const std::wstring path =
        std::wstring(dir) + L"dshb-capacityprobe-" +
        std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L".json";
    const int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

// A full ring, through the real Append() path: distinct values 1 cent apart, 10 s apart, all
// inside one local day -- exactly the shape panelprobe's case 5e uses as the day's ceiling.
void FillRing(dshb::CurveStore* store) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t start = now - static_cast<int64_t>(dshb::CurveStore::kCapacity) * 10;
    int64_t cent = 100000;
    for (std::size_t i = 0; i < dshb::CurveStore::kCapacity; ++i) {
        dshb::CurveObservation obs;
        obs.primaryCurrency = "CNY";
        dshb::CurveObservation::Item item;
        item.currency = "CNY";
        item.amountOk = true;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(cent / 100),
                      static_cast<long long>(cent % 100));
        item.text = buf;
        obs.observations.push_back(item);
        store->Append(obs, start + static_cast<int64_t>(i) * 10, std::string());
        --cent;
    }
}

double MsSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

volatile std::size_t g_sink = 0;          // keeps the optimiser from deleting the loops
volatile double g_sinkDouble = 0.0;

}  // namespace

int main(int argc, char** argv) {
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--repeat=", 0) == 0) {
            repeat = std::atoi(arg.c_str() + 9);
            if (repeat < 1) repeat = 1;
        } else {
            std::printf("capacityprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: capacityprobe [--repeat=N]\n");
            return 64;
        }
    }

    // ---- the sizes, straight from the compiler ----
    std::printf("sizes: sizeof(CurveStorePoint)=%zu B, sizeof(CurveStore)=%zu B, kCapacity=%zu\n",
                sizeof(dshb::CurveStorePoint), sizeof(dshb::CurveStore),
                dshb::CurveStore::kCapacity);
    std::printf("       ring slots = %zu B = %.1f KB (heap, allocated once per store)\n",
                sizeof(dshb::CurveStorePoint) * dshb::CurveStore::kCapacity,
                sizeof(dshb::CurveStorePoint) * dshb::CurveStore::kCapacity / 1024.0);
    std::printf("       display set = %zu visible + 1 incoming = %zu points (unchanged)\n",
                dshb::kCurveDisplayPoints, dshb::kCurveDisplayPoints + 1);

    // ---- the store the display layer will actually read ----
    dshb::CurveStore store;
    FillRing(&store);
    const std::string path = TempPath();
    if (!store.Save(path)) {
        std::printf("FAIL: cannot write the fixture file %s\n", path.c_str());
        return 1;
    }
    dshb::SetCurveStorePath(Wide(path));
    std::printf("fixture: %zu points, oldest->newest %s -> %s, file %s\n", store.size(),
                store.At(0).entries.front().text.c_str(),
                store.At(store.size() - 1).entries.front().text.c_str(), path.c_str());
    bool ok = true;

    // ---- (1) a WHOLE-RING copy: what the per-frame paths used to do once per call ----
    {
        double best = 1e9;
        double total = 0.0;
        for (int r = 0; r < repeat; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            const std::vector<dshb::CurveStorePoint> all = store.Points();
            const double ms = MsSince(t0);
            g_sink += all.size();
            if (ms < best) best = ms;
            total += ms;
        }
        std::printf("Points() full-ring copy      : %8.3f ms/call  (best of %d, avg %.3f)\n", best,
                    repeat, total / repeat);
    }

    // ---- (2) the same points read in place: what the per-frame paths do now ----
    {
        double best = 1e9;
        double total = 0.0;
        for (int r = 0; r < repeat; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            std::size_t sum = 0;
            for (std::size_t i = 0; i < store.size(); ++i) {
                sum += store.At(i).entries.size();
            }
            const double ms = MsSince(t0);
            g_sink += sum;
            if (ms < best) best = ms;
            total += ms;
        }
        std::printf("At(i) walk, whole ring       : %8.3f ms/call  (best of %d, avg %.3f)\n", best,
                    repeat, total / repeat);
    }

    // ---- (3) the display slice the frame asks for: 12 points, not the whole ring ----
    {
        double best = 1e9;
        double total = 0.0;
        for (int r = 0; r < repeat; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            const std::vector<dshb::CurveStorePoint> slice =
                store.Newest(dshb::kCurveDisplayPoints + 1);
            const double ms = MsSince(t0);
            g_sink += slice.size();
            if (ms < best) best = ms;
            total += ms;
        }
        std::printf("Newest(12) display slice     : %8.3f ms/call  (best of %d, avg %.3f)\n", best,
                    repeat, total / repeat);
    }

    // ---- (4) one real frame, split into its parts: BuildWidgetFrame does BuildFrameCurve
    //      (the display slice) + TodayUsageFromStore (the title line). CurveStateLine is a
    //      diagnostic string, and it is measured on its own because on a full ring it is the
    //      one part of the frame that is NOT bounded by the display width. ----
    {
        dshb::DisplayedAmount display;
        dshb::Sample sample{};
        sample.wallMs = static_cast<int64_t>(std::time(nullptr)) * 1000;
        sample.amountsOk = true;
        sample.currency = "CNY";
        sample.total = dshb::Amount::FromYuanDouble(913.60);
        dshb::CurrencyAmount entry{};
        entry.currency = "CNY";
        entry.total = sample.total;
        entry.ok = true;
        sample.entries.push_back(entry);
        display.OnSample(sample);

        const double todayMs = [&] {
            // Warm first: the first call pays for the vector/string allocations the following
            // calls reuse, and reporting that one would overstate a per-frame cost by ~10x.
            for (int r = 0; r < 3; ++r) {
                g_sinkDouble += static_cast<double>(dshb::TodayUsageFromStore("CNY").raw);
            }
            double best = 1e9;
            for (int r = 0; r < repeat; ++r) {
                const auto t0 = std::chrono::steady_clock::now();
                const dshb::TodayUsage usage = dshb::TodayUsageFromStore("CNY");
                const double ms = MsSince(t0);
                g_sinkDouble += static_cast<double>(usage.raw);
                if (ms < best) best = ms;
            }
            return best;
        }();

        // ★ The title line ("今日已 X.XX¥") is built by BuildWidgetFrame ONLY while the widget
        //   is not shutting down (TodayUsageText returns early otherwise), and it is the one
        //   part of the frame that reads today's whole day of points. So the shutdown state is
        //   cleared first: measuring a frame that skips it would report a cost that production
        //   does not have.
        (void)dshb::ShutdownActive();
        dshb::ShutdownCancel();

        double bestFrame = 1e9;
        std::size_t curveSize = 0;
        std::size_t titleChars = 0;
        for (int r = 0; r < repeat; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            const dshb::WidgetFrame frame =
                dshb::BuildWidgetFrame(dshb::ConnState::Ok, display, true, L"\u00A5");
            const double ms = MsSince(t0);
            curveSize = frame.curve.size();
            titleChars = frame.todayUsageText.size();
            g_sink += frame.todayUsageText.size();
            g_sinkDouble += frame.ambientIntensity;
            if (ms < bestFrame) bestFrame = ms;
        }

        double bestLine = 1e9;
        std::size_t lineSize = 0;
        for (int r = 0; r < repeat; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            const std::string line = dshb::CurveStateLine();
            const double ms = MsSince(t0);
            lineSize = line.size();
            g_sink += line.size();
            if (ms < bestLine) bestLine = ms;
        }
        std::printf("one frame: BuildWidgetFrame : %8.3f ms/frame (best of %d; the slice it takes "
                    "from the ring is %zu points -- %zu visible + %zu incoming -- and the frame "
                    "draws %zu of them; title line=%zu chars)\n",
                    bestFrame, repeat, dshb::kCurveDisplayPoints + 1, dshb::kCurveDisplayPoints, 1,
                    curveSize, titleChars);
        std::printf("           TodayUsageFromStore: %8.3f ms/call  (whole day's points, in place)\n",
                    todayMs);
        std::printf("           CurveStateLine     : %8.3f ms/call  (diagnostic; %zu chars)\n",
                    bestLine, lineSize);
    }

    // ---- (6) what this target ASSERTS. Everything above is a measurement; these are the
    //      claims the numbers stand for, and the exit code follows them. ----
    //  1) One point is 72 B. The ring therefore CANNOT live inside the object: at kCapacity it
    //     is ~620 KB, and panelprobe builds two stores in one scope (1.24 MB) against an MSVC
    //     stack limit of 1 MB.
    //  2) sizeof(CurveStore) is a vector header, not the storage. This is the check that the
    //     ring really is on the heap: if someone puts the array back inline, this number
    //     becomes ~622 KB and this line fails rather than the program crashing later.
    //  3) The display set is still 11 visible + 1 incoming.
    //  4) A one-point store round-trips through the file (the shape a first run produces).
    {
        const std::size_t pointsBytes =
            sizeof(dshb::CurveStorePoint) * dshb::CurveStore::kCapacity;
        std::printf("assert: sizeof(CurveStorePoint)=%zu B (always 72), ring=%zu B, "
                    "sizeof(CurveStore)=%zu B\n",
                    sizeof(dshb::CurveStorePoint), pointsBytes, sizeof(dshb::CurveStore));
        ok = (sizeof(dshb::CurveStorePoint) == 72) && ok;
        ok = (sizeof(dshb::CurveStore) < pointsBytes / 100) && ok;
        ok = (dshb::kCurveDisplayPoints == 11) && ok;

        dshb::CurveStore one;
        dshb::CurveObservation obs;
        obs.primaryCurrency = "CNY";
        dshb::CurveObservation::Item item;
        item.currency = "CNY";
        item.text = "48.80";
        item.amountOk = true;
        obs.observations.push_back(item);
        one.Append(obs, static_cast<int64_t>(std::time(nullptr)), std::string());
        const std::string onePath = path + ".one";
        dshb::CurveStore back;
        ok = one.Save(onePath) && ok;
        const dshb::CurveLoadResult loaded = back.Load(onePath);
        ok = loaded.ok && back.size() == 1 && ok;
        std::printf("assert: one-point store round-trip -> ok=%s points=%d, size=%zu (%s)\n",
                    loaded.ok ? "yes" : "NO", loaded.pointsLoaded, back.size(),
                    loaded.detail.c_str());
    }

    std::printf("sink=%zu %.6f\n", static_cast<std::size_t>(g_sink), static_cast<double>(g_sinkDouble));
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
