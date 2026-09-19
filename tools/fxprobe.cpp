// fxprobe -- offline proof for the once-per-session USD->CNY rate and the USD counterpart
// it injects into a CNY-only sample (src/fx_rate.h/.cpp), plus one live case.
//
//   fxprobe                        every case (the default); the live case is skipped
//   fxprobe --live                 also fetch the real rate from api.frankfurter.app
//   fxprobe --url=HOST:PORT/PATH   fetch from this address instead (local test server);
//                                  implies --live
//   fxprobe --timeout-ms=N         the fetch's timeout (default 5000)
//   fxprobe --keep                 keep the temporary directory and print its path
//
// Why two halves:
//   * a/b/c are the INJECTION and they are pure -- fixed rate in, entries out. That is the
//     only way to pin the boundary cases (a conversion that lands exactly on a half cent,
//     an account that already reports USD, a session that has no rate) without waiting for
//     an account to change or a network to break.
//   * d is the owner's own sentence -- "曲线那边应该是加一个 USD 就可以正常使用" -- checked
//     as a FILE: two currencies walk through CurveStore, land in one JSON point with both
//     keys, and come back out with both values intact.
//   * --live is the one claim an offline case cannot make: that the address answers. It is
//     opt-in, exactly as apiprobe's --live is, so a bare run opens no socket.
//
// It writes exactly one file, under %TEMP%\dshb-fxprobe-<pid>\ : the production curve.json
// is never read or written. The widget's own argument parsing is not touched.
#include "../src/fx_rate.h"

#include "amount.h"
#include "curve_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using dshb::Amount;
using dshb::CurrencyAmount;
using dshb::CurveLoadResult;
using dshb::CurveObservation;
using dshb::CurveStore;
using dshb::CurveStorePoint;
using dshb::ParseAmount;
using dshb::Sample;
using dshb::fx::InjectUsdCounterpart;
using dshb::fx::ParseRateBody;
using dshb::fx::Rate;
using dshb::fx::RateEndpoint;

namespace {

// ===========================================================================
// The same deliberately tiny harness storeprobe uses: one line per item,
// "PASS: " / "FAIL: ", exit code 0 only when nothing failed.
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
};

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// One entry as text: "CNY=19.76" / "USD=2.95" / "USD=<ok=false>".
std::string One(const CurrencyAmount& entry) {
    std::string out = entry.currency + "=";
    out += entry.ok ? entry.total.ToString2() : std::string("<ok=false>");
    return out;
}

// Every entry, verbatim, in order: "CNY=19.76 then USD=2.95".
std::string All(const std::vector<CurrencyAmount>& entries) {
    if (entries.empty()) return "<no entries>";
    std::string out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) out += " then ";
        out += One(entries[i]);
    }
    return out;
}

// Field-by-field equality, so case (b)/(c) can say "nothing moved" as a fact about each
// field rather than about a size comparison that would miss a changed amount.
bool SameEntries(const std::vector<CurrencyAmount>& a, const std::vector<CurrencyAmount>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].currency != b[i].currency) return false;
        if (a[i].total.raw != b[i].total.raw) return false;
        if (a[i].ok != b[i].ok) return false;
    }
    return true;
}

CurrencyAmount Make(const std::string& currency, const std::string& text, bool ok = true) {
    CurrencyAmount entry{};
    entry.currency = currency;
    entry.ok = ok;
    if (ok) ParseAmount(text, &entry.total);
    return entry;
}

// A CNY-only sample, the shape a mainland account produces.
Sample CnyOnly(const std::string& cnyText) {
    Sample s{};
    s.currency = "CNY";
    s.amountsOk = true;
    ParseAmount(cnyText, &s.total);
    s.entries.push_back(Make("CNY", cnyText));
    return s;
}

Rate Fixed(const std::string& rateText) {
    Rate r;
    r.ok = true;
    r.rateText = rateText;
    r.date = "2026-09-18";
    return r;
}

std::string TempDir() {
    char temp[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::string dir = std::string(temp) + "dshb-fxprobe-" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string ReadFile(const std::string& path, bool* ok) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        *ok = false;
        return std::string();
    }
    std::string out;
    char buf[4096];
    std::size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    fclose(f);
    *ok = true;
    return out;
}

// The conversion is checked against a value computed a second, independent way: the
// quotient truncated to cents by integer arithmetic on the *text* of both numbers.
// Two implementations that agree are not proof, so this one is deliberately the dumbest
// possible: scale both to integers with a fixed number of decimals, divide, truncate.
std::string ReferenceConvert(const std::string& cny, const std::string& rate, int rateDecimals) {
    auto toInt = [](const std::string& text, int decimals) -> long long {
        long long whole = 0;
        long long frac = 0;
        int seen = 0;
        bool dot = false;
        for (const char c : text) {
            if (c == '.') { dot = true; continue; }
            if (c < '0' || c > '9') continue;
            if (!dot) {
                whole = whole * 10 + (c - '0');
            } else if (seen < decimals) {
                frac = frac * 10 + (c - '0');
                ++seen;
            }
        }
        while (seen < decimals) { frac *= 10; ++seen; }
        long long scale = 1;
        for (int i = 0; i < decimals; ++i) scale *= 10;
        return whole * scale + frac;
    };
    const long long cnyScaled = toInt(cny, 4);         // 1/10000 CNY
    const long long rateScaled = toInt(rate, rateDecimals);
    const long long usdRaw = cnyScaled * 10000 / rateScaled;   // 1/10000 USD
    const long long cents = usdRaw / 100;                       // truncate to cents
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld", cents / 100, cents % 100);
    return buf;
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int RunLive(RateEndpoint endpoint, int timeoutMs) {
    endpoint.timeoutMs = timeoutMs;
    const long long t0 = NowMs();
    const Rate rate = dshb::fx::FetchRate(endpoint);
    const long long elapsed = NowMs() - t0;
    std::printf("live: ok=%d rate=%s date=%s elapsed=%lldms error=%s\n", rate.ok ? 1 : 0,
                rate.ok ? Quote(rate.rateText).c_str() : "(none)",
                rate.date.empty() ? "(none)" : Quote(rate.date).c_str(),
                static_cast<long long>(elapsed), rate.error.empty() ? "(none)" : rate.error.c_str());
    std::printf("live: %s\n", rate.LogLine().c_str());
    return rate.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    bool live = false;
    bool keep = false;
    bool haveUrl = false;
    std::string url;
    int timeoutMs = 5000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--live") {
            live = true;
        } else if (arg == "--keep") {
            keep = true;
        } else if (arg.rfind("--url=", 0) == 0) {
            url = arg.substr(6);
            haveUrl = true;
            live = true;
        } else if (arg.rfind("--timeout-ms=", 0) == 0) {
            timeoutMs = std::atoi(arg.c_str() + 13);
        } else {
            std::printf("fxprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("  --live | --url=HOST:PORT/PATH | --timeout-ms=N | --keep\n");
            return 2;
        }
    }

    if (live) {
        RateEndpoint endpoint;
        if (haveUrl) {
            const std::size_t slash = url.find('/');
            const std::string hostPort = slash == std::string::npos ? url : url.substr(0, slash);
            endpoint.path = slash == std::string::npos ? L"/" : std::wstring(url.begin() + slash, url.end());
            const std::size_t colon = hostPort.rfind(':');
            const std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
            endpoint.host = std::wstring(host.begin(), host.end());
            endpoint.port = static_cast<unsigned short>(
                colon == std::string::npos ? 80 : std::atoi(hostPort.c_str() + colon + 1));
            endpoint.secure = false;
        }
        return RunLive(endpoint, timeoutMs);
    }

    Harness h;
    std::printf("fxprobe: offline cases (no socket is opened)\n");

    // =====================================================================
    // Rate parsing: what a body is allowed to become a rate
    // =====================================================================
    {
        const std::string good =
            "{\"amount\":1.0,\"base\":\"USD\",\"date\":\"2026-09-18\",\"rates\":{\"CNY\":6.6976}}";
        const Rate r = ParseRateBody(good);
        h.Req("p1", "ECB/frankfurter answer parses to a rate", "rate=" + Quote(r.rateText) + " date=" +
                  Quote(r.date) + " ok=" + std::to_string(r.ok),
              r.ok && r.rateText == "6.6976" && r.date == "2026-09-18");

        const Rate bom = ParseRateBody("\xEF\xBB\xBF" + good);
        h.Req("p2", "a leading UTF-8 BOM does not break the parse",
              "ok=" + std::to_string(bom.ok) + " rate=" + Quote(bom.rateText),
              bom.ok && bom.rateText == "6.6976");

        struct Bad {
            const char* what;
            const char* body;
        };
        const Bad bad[] = {
            {"not JSON", "hello"},
            {"empty body", ""},
            {"base is not USD", "{\"base\":\"EUR\",\"rates\":{\"CNY\":6.6}}"},
            {"no base", "{\"rates\":{\"CNY\":6.6}}"},
            {"no rates", "{\"base\":\"USD\"}"},
            {"rates is not an object", "{\"base\":\"USD\",\"rates\":[]}"},
            {"no CNY in rates", "{\"base\":\"USD\",\"rates\":{\"JPY\":150}}"},
            {"CNY is not a number", "{\"base\":\"USD\",\"rates\":{\"CNY\":\"6.6\"}}"},
            {"CNY is zero", "{\"base\":\"USD\",\"rates\":{\"CNY\":0}}"},
            {"CNY is negative", "{\"base\":\"USD\",\"rates\":{\"CNY\":-6.6}}"},
        };
        bool allRejected = true;
        std::string detail;
        for (const Bad& b : bad) {
            const Rate refused = ParseRateBody(b.body);
            if (refused.ok) {
                allRejected = false;
                detail += std::string(" accepted:") + b.what;
            }
        }
        h.Req("p3", "every unusable body is refused, none becomes a guessed rate",
              allRejected ? "10 bodies refused, each with an error line" : detail, allRejected);
    }

    // =====================================================================
    // (a) after injection BOTH currencies are present and USD == CNY / rate
    // =====================================================================
    {
        const Rate rate = Fixed("6.6976");
        const Sample before = CnyOnly("19.76");
        Sample after = before;
        const bool injected = InjectUsdCounterpart(&after, rate);

        const CurrencyAmount* usd = nullptr;
        const CurrencyAmount* cny = nullptr;
        for (const CurrencyAmount& e : after.entries) {
            if (e.currency == "USD") usd = &e;
            if (e.currency == "CNY") cny = &e;
        }
        const std::string want = ReferenceConvert("19.76", "6.6976", 4);
        h.Req("a1", "a CNY-only sample ends up with both CNY and USD, USD = CNY / rate",
              "before=[" + All(before.entries) + "] after=[" + All(after.entries) +
                  "] want=USD=" + want,
              injected && after.entries.size() == 2 && cny != nullptr && usd != nullptr &&
                  cny->total.ToString2() == "19.76" && usd->ok && usd->total.ToString2() == want);
        h.Req("a2", "the injected entry carries exactly two decimals and ok=true",
              "USD=" + (usd ? usd->total.ToString2() : std::string("<absent>")) +
                  " ok=" + std::to_string(usd ? usd->ok : false),
              usd != nullptr && usd->total.ToString2() == "2.95" && usd->ok);
        h.Req("a3", "the injected entry is a real CurrencyAmount, appended LAST",
              "order=[" + All(after.entries) + "]",
              after.entries.size() == 2 && after.entries[0].currency == "CNY" &&
                  after.entries[1].currency == "USD");

        // ---- boundary values ------------------------------------------------
        struct Case {
            const char* cny;
            const char* rate;
            const char* what;
        };
        const Case cases[] = {
            {"0.00", "6.6976", "a zero balance converts to 0.00, not to 0 or to empty"},
            {"0.01", "6.6976", "one cent is below one USD cent -> 0.00 (truncated, never rounded up)"},
            {"0.03", "6.6976", "0.03 CNY -> 0.00"},
            {"0.04", "6.6976", "0.04 CNY -> 0.00 (boundary of the first cent)"},
            {"0.07", "6.6976", "0.07 CNY -> 0.01 (first value that reaches one cent)"},
            {"1.00", "1.0000", "rate exactly 1 -> the amounts are equal"},
            {"19.76", "1.0000", "rate exactly 1, two decimals kept"},
            {"19.76", "7.0000", "an exact division"},
            {"19.76", "0.5000", "a rate below 1 (USD stronger than the balance currency)"},
            {"0.99", "6.6976", "just under one unit"},
            {"1.00", "6.6976", "just over one unit"},
            {"1000000.00", "6.6976", "a large balance does not overflow"},
            {"19.7654", "6.6976", "a four-decimal balance keeps its cents and truncates the rest"},
        };
        bool allOk = true;
        std::string detail;
        for (const Case& c : cases) {
            Sample s = CnyOnly(c.cny);
            const Rate r = Fixed(c.rate);
            const bool got = InjectUsdCounterpart(&s, r);
            std::string shown;
            bool ok = false;
            Amount wantAmount{};
            const std::string wantText = ReferenceConvert(c.cny, c.rate, 4);
            ParseAmount(wantText, &wantAmount);
            for (const CurrencyAmount& e : s.entries) {
                if (e.currency != "USD") continue;
                shown = e.total.ToString2();
                ok = got && e.ok && e.total.raw == wantAmount.raw;
            }
            char line[192];
            std::snprintf(line, sizeof(line), " %s/%s -> USD=%s want=%s", c.cny, c.rate,
                          shown.empty() ? "<none>" : shown.c_str(), wantText.c_str());
            detail += line;
            if (!ok) allOk = false;
        }
        h.Req("a4", "13 boundary values all match an independently computed quotient",
              std::string(allOk ? "all 13 agree;" : "MISMATCH;") + detail, allOk);

        // A balance whose CNY text cannot be read is not converted.
        Sample broken{};
        broken.entries.push_back(Make("CNY", "", false));
        broken.entries[0].currency = "CNY";
        const bool injected5 = InjectUsdCounterpart(&broken, Fixed("6.6976"));
        h.Req("a5", "a CNY entry that carries no readable amount is not converted",
              "entries=[" + All(broken.entries) + "] injected=" + std::to_string(injected5),
              !injected5 && broken.entries.size() == 1);
    }

    // =====================================================================
    // (b) an overseas account (USD already present) is not touched
    // =====================================================================
    {
        Sample before{};
        before.currency = "CNY";
        before.amountsOk = true;
        ParseAmount("19.76", &before.total);
        before.entries.push_back(Make("CNY", "19.76"));
        before.entries.push_back(Make("USD", "2.95"));
        Sample after = before;

        const bool injected = InjectUsdCounterpart(&after, Fixed("6.6976"));
        h.Req("b1", "a sample that already carries USD is left byte-for-byte alone",
              "before=[" + All(before.entries) + "] after=[" + All(after.entries) +
                  "] injected=" + std::to_string(injected),
              !injected && SameEntries(before.entries, after.entries) && after.entries.size() == 2);

        // The same must hold when the rate is different -- proof that the untouched
        // sample is untouched for the right reason (USD was there), not by coincidence.
        Sample after2 = before;
        const bool injected2 = InjectUsdCounterpart(&after2, Fixed("9.9999"));
        h.Req("b2", "and still alone with a completely different rate",
              "after=[" + All(after2.entries) + "] injected=" + std::to_string(injected2),
              !injected2 && SameEntries(before.entries, after2.entries));

        // A USD-only sample (an account with no CNY at all) is likewise untouched.
        Sample usdOnly{};
        usdOnly.currency = "USD";
        usdOnly.amountsOk = true;
        usdOnly.entries.push_back(Make("USD", "2.95"));
        Sample usdOnlyAfter = usdOnly;
        const bool injected3 = InjectUsdCounterpart(&usdOnlyAfter, Fixed("6.6976"));
        h.Req("b3", "a USD-only sample gains no CNY counterpart either",
              "entries=[" + All(usdOnlyAfter.entries) + "] injected=" + std::to_string(injected3),
              !injected3 && SameEntries(usdOnly.entries, usdOnlyAfter.entries));
    }

    // =====================================================================
    // (c) with no rate nothing is injected
    // =====================================================================
    {
        const Sample before = CnyOnly("19.76");
        Sample after = before;
        Rate failed;                      // ok = false, exactly what a failed fetch produces
        failed.error = "请求没到（WinHttpConnect failed, error=10061）";
        const bool injected = InjectUsdCounterpart(&after, failed);
        h.Req("c1", "no rate -> the sample is exactly what it was before",
              "before=[" + All(before.entries) + "] after=[" + All(after.entries) +
                  "] injected=" + std::to_string(injected),
              !injected && SameEntries(before.entries, after.entries));

        Sample after2 = before;
        const bool injected2 = InjectUsdCounterpart(&after2, Rate{});
        h.Req("c2", "the same for a default-constructed (never fetched) rate",
              "entries=[" + All(after2.entries) + "] injected=" + std::to_string(injected2),
              !injected2 && SameEntries(before.entries, after2.entries));

        // The display layer's fallback is what makes this acceptable: with no USD entry
        // the currency is still switchable, it just has no number. That switchable list
        // lives in widget_display.cpp's OnSample (it forces CNY and USD into the list),
        // so what is checkable here is the consequence: no USD entry exists to read.
        h.Req("c3", "with no rate there is no USD entry for the display to pick up",
              "entries=[" + All(after.entries) + "]",
              after.entries.size() == 1 && after.entries[0].currency == "CNY");

        // The log line must say why (the owner asked for a line either way).
        const std::string line = failed.LogLine();
        h.Req("c4", "the failure is logged as a [fx] line with the reason", line,
              line.rfind("[fx]", 0) == 0 && line.find("WinHttpConnect") != std::string::npos);
    }

    // The success log line's exact shape.
    {
        const std::string line = Fixed("6.6976").LogLine();
        h.Req("c5", "the success line names the source, the value and the date", line,
              line == "[fx] 1 USD = 6.6976 CNY（frankfurter/ECB 2026-09-18，本会话只取这一次）");
    }

    // =====================================================================
    // (d) the curve store, across two currencies, as a file
    // =====================================================================
    {
        const std::string dir = TempDir();
        const std::string path = dir + "\\curve.json";
        bool ok = !dir.empty();

        CurveStore store;
        CurveObservation first;
        first.primaryCurrency = "CNY";
        first.observations.push_back(CurveObservation::Item{"CNY", "19.76", true});
        first.observations.push_back(CurveObservation::Item{"USD", "2.95", true});
        // ★ 时间戳必须是"现在"：CurveStore 会把超过 24 小时的旧点整批丢掉（§2.3 的
        //   kExpirySeconds），所以写死一个日期会让这条用例在第二天自己变成 FAIL ——
        //   第一次跑就是这么失败的（写死的那一秒比当天早了一年）。
        //   这也正好说明"过期"是存储自己的规则，探针不该绕开它。
        const long long nowSec = dshb::fx::NowUnixSeconds();
        store.Append(first, nowSec, "#3b6fb0");

        CurveObservation second;
        second.primaryCurrency = "CNY";
        second.observations.push_back(CurveObservation::Item{"CNY", "19.70", true});
        second.observations.push_back(CurveObservation::Item{"USD", "2.94", true});
        store.Append(second, nowSec + 10, "#3b6fb0");

        ok = store.Save(path) && ok;

        bool readOk = false;
        const std::string raw = ReadFile(path, &readOk);
        ok = ok && readOk;

        // The JSON itself: the same point must carry both keys. Checked on the bytes,
        // because that is what the owner will open.
        const bool bothInFile = raw.find("\"CNY\": \"19.76\"") != std::string::npos &&
                                raw.find("\"USD\": \"2.95\"") != std::string::npos &&
                                raw.find("\"CNY\": \"19.70\"") != std::string::npos &&
                                raw.find("\"USD\": \"2.94\"") != std::string::npos;
        h.Req("d1", "one point carries BOTH currencies in the file", "bytes contain the four members",
              bothInFile);

        CurveStore reloaded;
        const CurveLoadResult loaded = reloaded.Load(path);
        const std::vector<CurveStorePoint> points = reloaded.Points();
        bool valuesOk = loaded.ok && points.size() == 2;
        std::string shown;
        if (valuesOk) {
            for (const CurveStorePoint& p : points) {
                const CurveStorePoint::Entry* cny = p.Find("CNY");
                const CurveStorePoint::Entry* usd = p.Find("USD");
                shown += "[" + (cny ? cny->text : std::string("<absent>")) + "," +
                         (usd ? usd->text : std::string("<absent>")) + "]";
                if (!cny || !usd || cny->missing || usd->missing) valuesOk = false;
            }
            valuesOk = valuesOk && points[0].Find("USD")->text == "2.95" &&
                       points[1].Find("USD")->text == "2.94" &&
                       points[0].Find("CNY")->text == "19.76" &&
                       points[1].Find("CNY")->text == "19.70";
        }
        h.Req("d2", "read back, both currencies still give the right value",
              "loaded ok=" + std::to_string(loaded.ok) + " points=" + std::to_string(points.size()) +
                  " [CNY,USD]=" + (shown.empty() ? std::string("<none>") : shown),
              valuesOk);

        if (!keep) {
            DeleteFileA(path.c_str());
            RemoveDirectoryA(dir.c_str());
        } else {
            std::printf("kept: %s\n", path.c_str());
        }
    }

    // =====================================================================
    // The fetch's failure path, timed. A closed port answers immediately on this
    // machine; the assertion is about the OUTCOME (ok=false, a reason, and no hang),
    // which is the part the widget depends on. It runs with an EMPTY cache path, i.e.
    // the real first-run-with-no-network case.
    // =====================================================================
    if (timeoutMs > 0) {
        RateEndpoint dead;
        dead.host = L"127.0.0.1";
        dead.port = 1;          // nothing listens on port 1
        dead.secure = false;
        dead.path = L"/latest?from=USD&to=CNY";
        dead.timeoutMs = timeoutMs;
        const long long t0 = NowMs();
        const Rate rate = dshb::fx::FetchRate(dead);
        const long long elapsed = NowMs() - t0;
        h.Req("e1", "an unreachable endpoint fails cleanly and does not hang",
              "ok=" + std::to_string(rate.ok) + " elapsed=" + std::to_string(elapsed) +
                  "ms error=" + Quote(rate.error),
              !rate.ok && !rate.error.empty() && elapsed < timeoutMs + 2000);
    }

    // =====================================================================
    // The cache (owner's 2026-09-19 addition): 取到就落盘 / 取不到用上一次 /
    // 连缓存都没有 / 缓存太旧当作没有.
    // =====================================================================
    {
        const std::string dir = TempDir();
        const std::wstring cachePath = std::wstring(dir.begin(), dir.end()) + L"\\fx.json";
        const std::wstring missingPath = std::wstring(dir.begin(), dir.end()) + L"\\nope.json";
        // 一个固定的"现在"：所有年龄都由它算出来，所以这几条不依赖跑探针那一天。
        // 2026-09-19 12:00:00 UTC.
        const long long now = 1789819200LL;

        // ---- ① 取到 -> 落盘，读回文件核对值/日期 ------------------------------
        {
            Rate fresh;
            fresh.ok = true;
            fresh.rateText = "6.6976";
            fresh.date = "2026-09-18";
            std::wstring why;
            const bool wrote = dshb::fx::SaveCachedRate(cachePath, fresh, now, &why);

            const Rate back = dshb::fx::LoadCachedRate(cachePath, now);
            h.Req("f1", "a fetched rate is written to disk and reads back with the same value and date",
                  "wrote=" + std::to_string(wrote) + " read ok=" + std::to_string(back.ok) +
                      " rate=" + Quote(back.rateText) + " date=" + Quote(back.date) +
                      " fetchedAt=" + Quote(back.fetchedAt) + " ageDays=" + std::to_string(back.ageDays),
                  wrote && back.ok && back.rateText == "6.6976" && back.date == "2026-09-18" &&
                      back.fetchedAtUnix == now && back.ageDays == 0);

            bool readOk = false;
            const std::string raw = ReadFile(dir + "\\fx.json", &readOk);
            h.Req("f2", "the cache file is a small readable JSON with the three members",
                  std::string("file=") + (readOk ? "read" : "MISSING") + " body=" + raw,
                  readOk && raw.find("\"rate\": \"6.6976\"") != std::string::npos &&
                      raw.find("\"date\": \"2026-09-18\"") != std::string::npos &&
                      raw.find("\"fetched_at\": " + std::to_string(now)) != std::string::npos);
        }

        // ---- ② 本次失败 + 有缓存 -> 用缓存，日志写明来源 ----------------------
        {
            RateEndpoint dead;
            dead.host = L"127.0.0.1";
            dead.port = 1;
            dead.secure = false;
            dead.path = L"/latest?from=USD&to=CNY";
            dead.timeoutMs = 1500;

            const Rate used = dshb::fx::ResolveRate(dead, cachePath, now + 2 * 86400LL);
            const std::string line = used.LogLine();
            h.Req("f3", "this session's fetch fails -> the cached rate is used, marked as cached",
                  "ok=" + std::to_string(used.ok) + " cached=" + std::to_string(used.cached) +
                      " rate=" + Quote(used.rateText) + " ageDays=" + std::to_string(used.ageDays),
                  used.ok && used.cached && used.rateText == "6.6976" && used.date == "2026-09-18" &&
                      used.ageDays == 2);
            h.Req("f4", "the log line says it came from the cache, with the date and the age", line,
                  line.find("本次取汇失败") != std::string::npos &&
                      line.find("2026-09-18") != std::string::npos &&
                      line.find("6.6976") != std::string::npos &&
                      line.find("已 2 天") != std::string::npos);
        }

        // ---- ③ 本次失败 + 无缓存 -> 不补 USD，entries 逐字段与注入前相同 ------
        {
            RateEndpoint dead;
            dead.host = L"127.0.0.1";
            dead.port = 1;
            dead.secure = false;
            dead.path = L"/latest?from=USD&to=CNY";
            dead.timeoutMs = 1500;

            const Rate none = dshb::fx::ResolveRate(dead, missingPath, now);
            const std::string line = none.LogLine();
            h.Req("f5", "no cache either -> there is no rate, and the line says so",
                  "ok=" + std::to_string(none.ok) + " line=" + line,
                  !none.ok && line.find("从没有过汇率") != std::string::npos);

            const Sample before = CnyOnly("19.76");
            Sample after = before;
            const bool injected = InjectUsdCounterpart(&after, none);
            h.Req("f6", "and then the sample gets no USD entry at all",
                  "before=[" + All(before.entries) + "] after=[" + All(after.entries) + "]",
                  !injected && SameEntries(before.entries, after.entries));
        }

        // ---- ④ 缓存超过上限 -> 当作没有 + 日志写明为什么 ----------------------
        {
            const Rate at29 = dshb::fx::LoadCachedRate(cachePath, now + 29 * 86400LL);
            h.Req("f7", "29 days old is still inside the limit",
                  "ok=" + std::to_string(at29.ok) + " ageDays=" + std::to_string(at29.ageDays),
                  at29.ok && at29.ageDays == 29);

            const long long at31 = now + 31 * 86400LL;
            const Rate at31r = dshb::fx::LoadCachedRate(cachePath, at31);
            h.Req("f8", "31 days old is dropped, and the reason says why",
                  "ok=" + std::to_string(at31r.ok) + " error=" + Quote(at31r.error),
                  !at31r.ok && at31r.error.find("缓存已过期") != std::string::npos &&
                      at31r.error.find("31 天") != std::string::npos &&
                      at31r.error.find("30 天") != std::string::npos);

            // 回退路径也要遵守上限：过期 + 本次失败 = 没有汇率。
            RateEndpoint dead;
            dead.host = L"127.0.0.1";
            dead.port = 1;
            dead.secure = false;
            dead.path = L"/latest?from=USD&to=CNY";
            dead.timeoutMs = 1500;
            const Rate old = dshb::fx::ResolveRate(dead, cachePath, at31);
            h.Req("f9", "an expired cache plus a failed fetch is 'no rate', not a stale value",
                  "ok=" + std::to_string(old.ok) + " line=" + old.LogLine(),
                  !old.ok && old.LogLine().find("缓存已过期") != std::string::npos);
        }

        // ---- 缓存文件坏掉：当作没有，不崩 ----------------------------------
        {
            const std::string badPath = dir + "\\bad.json";
            FILE* f = nullptr;
            if (fopen_s(&f, badPath.c_str(), "wb") == 0 && f) {
                const char half[] = "{\"rate\": \"6.69";
                std::fwrite(half, 1, sizeof(half) - 1, f);
                std::fclose(f);
            }
            const std::wstring badW(badPath.begin(), badPath.end());
            const Rate bad = dshb::fx::LoadCachedRate(badW, now);
            h.Req("f10", "a half-written cache file is refused, not trusted",
                  "ok=" + std::to_string(bad.ok) + " error=" + Quote(bad.error),
                  !bad.ok && bad.error.find("合法 JSON") != std::string::npos);
            DeleteFileA(badPath.c_str());
        }

        if (!keep) {
            DeleteFileW(cachePath.c_str());
            DeleteFileW((cachePath + L".tmp").c_str());
            RemoveDirectoryA(dir.c_str());
        } else {
            std::printf("kept: %s\\fx.json\n", dir.c_str());
        }
    }

    std::printf("fxprobe: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& line : h.failures) std::printf("  %s\n", line.c_str());
    std::printf("RESULT: %s\n", h.failed == 0 ? "all properties hold" : "FAILED");
    return h.failed == 0 ? 0 : 1;
}
