// storeprobe -- offline proof for the curve data layer (src/curve_store.h/.cpp).
//
//   storeprobe            run every check (the default); no network, no external file
//   storeprobe --keep     keep the temporary directory instead of deleting it
//   storeprobe --verbose  also print the raw curve.json written by the round-trip case
//
// The tool touches exactly one file, a temporary path it creates itself under
// %TEMP%\dshb-storeprobe-<pid>\ : it never reads or writes the production curve.json,
// never opens a socket, and never touches the widget's own state.
//
// Output contract: one line per acceptance item, each line starting with "PASS: " or
// "FAIL: ". The exit code is 0 only when every line is a PASS, so "no FAIL line" and
// "exit 0" cannot disagree.
//
// Written for this probe only; the widget's argument parsing is not touched.
#include "../src/curve_store.h"

#include "amount.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using dshb::Amount;
using dshb::CurveLoadResult;
using dshb::CurveObservation;
using dshb::CurveStorePoint;
using dshb::CurveStore;
using dshb::ParseAmount;

namespace {

// ===========================================================================
// A deliberately tiny harness
// ===========================================================================
// Everything is collected with std::string concatenation and printed at the end:
// mixing %s and std::wstring inside printf is a documented way to lose an hour.
struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    // Records one acceptance item. Returns the same value it was given, so a real
    // check and the self-check of the harness itself go through one code path.
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

// Hex + printable dump, for when two strings look identical but do not compare equal.
std::string Dump(const std::string& text) {
    std::string out = "\"" + text + "\" len=" + std::to_string(text.size());
    for (const char c : text) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), " %02x", static_cast<unsigned char>(c));
        out += buf;
    }
    return out;
}

std::string Num(long long value) { return std::to_string(value); }

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// "CNY=18.80", or "CNY=<absent>" when the point does not carry the currency.
std::string AmountText(const CurveStorePoint& point, const std::string& currency) {
    const CurveStorePoint::Entry* entry = point.Find(currency);
    if (!entry) return currency + "=<no such entry>";
    return currency + "=" + (entry->missing ? std::string("<absent>") : Quote(entry->text));
}

// "[18.80, 18.80]" -- values, comma separated, in point order. Unquoted on purpose:
// this is a comparison key, not JSON.
std::string Contents(const std::vector<CurveStorePoint>& points, const std::string& currency) {
    std::string out = "[";
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out += ", ";
        out += AmountText(points[i], currency);
    }
    return out + "]";
}

// "[18.80,18.8]" -- the bare amounts, comma separated with NO space, "?" for a point
// without that currency and "-" for an entry that is present but null. Written by
// helper and compared by helper, so the separator is never hand-typed twice.
std::string AllTexts(const std::vector<CurveStorePoint>& points, const std::string& currency) {
    std::string out = "[";
    for (std::size_t i = 0; i < points.size(); ++i) {
        const CurveStorePoint::Entry* entry = points[i].Find(currency);
        if (i != 0) out += ",";
        out += entry ? (entry->missing ? std::string("-") : entry->text) : std::string("?");
    }
    return out + "]";
}

// ===========================================================================
// Temp path handling: one file, created by this tool, printed for the reader
// ===========================================================================

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

struct ProbeDir {
    std::string dir;        // UTF-8, no trailing separator
    std::wstring wideDir;
    bool ok = false;
};

ProbeDir MakeProbeDir() {
    ProbeDir probe;
    wchar_t buffer[MAX_PATH] = {};
    const DWORD size = GetEnvironmentVariableW(L"TEMP", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return probe;

    probe.wideDir = std::wstring(buffer, size) + L"\\dshb-storeprobe-" +
                    std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryW(probe.wideDir.c_str(), nullptr);   // ERROR_ALREADY_EXISTS is fine
    probe.dir = WideToUtf8(probe.wideDir);
    probe.ok = !probe.dir.empty();
    return probe;
}

std::string PathOf(const ProbeDir& probe, const std::string& leaf) {
    return probe.dir + "\\" + leaf;
}

void WriteBytes(const std::string& path, const std::string& bytes) {
    const std::wstring wide = Utf8ToWide(path);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("FAIL: setup | cannot write %s | windows error %lu\n", path.c_str(),
                    static_cast<unsigned long>(GetLastError()));
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

std::string ReadBytes(const std::string& path, bool* ok) {
    *ok = false;
    const std::wstring wide = Utf8ToWide(path);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string out;
    char chunk[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, chunk, sizeof(chunk), &read, nullptr)) break;
        if (read == 0) break;
        out.append(chunk, read);
    }
    CloseHandle(file);
    *ok = true;
    return out;
}

void RemoveFile(const std::string& path) {
    DeleteFileW(Utf8ToWide(path).c_str());
}

std::string SearchReplaceAll(std::string text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// ===========================================================================
// Building observations
// ===========================================================================

// Every fixture timestamp is expressed as "now minus a few seconds or minutes"
// rather than as a small epoch number. Reason (found by running this probe): the
// store applies its own 86400 s rule when it LOADS, so a fixture stamped 1030 s
// after 1970 is genuinely stale and its points are discarded on reload -- correct
// behaviour, wrong fixture. Anchoring to now keeps fresh fixtures fresh and leaves
// the staleness cases to the checks that are about staleness.
const int64_t kAnchor = static_cast<int64_t>(::time(nullptr));

CurveObservation::Item Both(const char* currency, const std::string& text, bool amountOk) {
    CurveObservation::Item item;
    item.currency = currency;
    item.text = text;
    item.amountOk = amountOk;
    return item;
}

// One entry, already parsed: the shape the API layer produces for "CNY 18.80".
CurveObservation Cny(const std::string& text) {
    CurveObservation obs;
    obs.primaryCurrency = "CNY";
    obs.observations.push_back(Both("CNY", text, true));
    return obs;
}

// Same, plus a USD entry that is either present or absent (null).
CurveObservation CnyUsd(const std::string& cny, const std::string& usd, bool usdPresent) {
    CurveObservation obs;
    obs.primaryCurrency = "CNY";
    obs.observations.push_back(Both("CNY", cny, true));
    obs.observations.push_back(Both("USD", usd, usdPresent));
    return obs;
}

// A store fed a list of values, so every case builds its input the same way.
void Feed(CurveStore* store, const std::vector<std::string>& values, int64_t firstSecond,
          int64_t stepSeconds) {
    int64_t now = firstSecond;
    for (const std::string& value : values) {
        store->Append(Cny(value), now);
        now += stepSeconds;
    }
}
std::vector<std::string> CountUp(int first, int count) {
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d.00", first + i);
        out.push_back(buf);
    }
    return out;
}

// Writes the store and returns the file's points, read back by a FRESH store, so
// "the file holds N points" is measured from the file and not from memory.
std::vector<CurveStorePoint> Reload(const std::string& path, CurveLoadResult* out) {
    CurveStore store;
    const CurveLoadResult result = store.Load(path);
    if (out) *out = result;
    return store.Points();
}

std::string PointSummary(const std::vector<CurveStorePoint>& points) {
    std::string out = "[";
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i != 0) out += ", ";
        out += "{";
        for (std::size_t k = 0; k < points[i].entries.size(); ++k) {
            if (k != 0) out += ", ";
            out += AmountText(points[i], points[i].entries[k].currency);
        }
        out += "}";
    }
    return out + "]";
}

bool SamePoints(const std::vector<CurveStorePoint>& a, const std::vector<CurveStorePoint>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].entries.size() != b[i].entries.size()) return false;
        for (std::size_t k = 0; k < a[i].entries.size(); ++k) {
            // Text is compared VERBATIM: "18.80" and "18.8" are the same number and
            // different strings, and acceptance 3 is about the string.
            if (a[i].entries[k].currency != b[i].entries[k].currency) return false;
            if (a[i].entries[k].missing != b[i].entries[k].missing) return false;
            if (a[i].entries[k].text != b[i].entries[k].text) return false;
        }
    }
    return true;
}

// ===========================================================================
// The acceptance checks
// ===========================================================================
void RunChecks(Harness* h, const ProbeDir& probe, bool verbose) {
    const std::string file = PathOf(probe, "curve.json");

    // -----------------------------------------------------------------------
    // (1) three identical values in a row, then one different value: exactly one
    //     point is appended. Spec §2.1, acceptance 1.
    // -----------------------------------------------------------------------
    {
        CurveStore store;
        // Three fetches 10 s apart, all within a minute of "now" so that the store's
        // own load-time rule cannot interfere with a case about change detection.
        const int64_t t0 = kAnchor - 50;
        /* three identical values: the 1st appends, the 2nd and 3rd must not */
        const bool a0 = store.Append(Cny("18.80"), t0);
        const bool a1 = store.Append(Cny("18.80"), t0 + 10);
        const bool a2 = store.Append(Cny("18.80"), t0 + 20);
        /* one different value: must append exactly one */
        const bool a3 = store.Append(Cny("18.60"), t0 + 30);

        const std::vector<CurveStorePoint> mem = store.Points();
        store.Save(file);
        CurveLoadResult reloaded;
        const std::vector<CurveStorePoint> disk = Reload(file, &reloaded);

        const std::string memContents = Contents(mem, "CNY");
        const bool ok = mem.size() == 2 && a0 && !a1 && !a2 && a3 &&
                        memContents == "[CNY=\"18.80\", CNY=\"18.60\"]" &&
                        disk.size() == 2 && Contents(disk, "CNY") == memContents &&
                        store.lastUpdate() == t0 + 30 && reloaded.pointsLoaded == 2;
        h->Req("case1", "only a change becomes a point (3 identical then 1 different)",
               "appended=" + std::string(a0 ? "1" : "0") + std::string(a1 ? "1" : "0") +
                   std::string(a2 ? "1" : "0") + std::string(a3 ? "1" : "0") +
                   ", memory=" + Num(static_cast<long long>(mem.size())) + " points " + memContents +
                   ", file=" + Num(static_cast<long long>(disk.size())) + " points " +
                   Contents(disk, "CNY") + ", update_at=" + Num(store.lastUpdate()) +
                   " (last fetch " + Num(t0 + 30) + ")",
               ok);
    }

    // -----------------------------------------------------------------------
    // (2) fifteen different values: exactly 12 points, and they are the NEWEST 12.
    //     Spec §2.2, acceptance 2. Compared by contents, not by count.
    // -----------------------------------------------------------------------
    {
        CurveStore store;
        Feed(&store, CountUp(1, 15), kAnchor - 2000, 10);   // 1.00 .. 15.00, distinct every time
        store.Save(file);

        CurveLoadResult reloaded;
        const std::vector<CurveStorePoint> disk = Reload(file, &reloaded);

        std::vector<std::string> expected;
        for (int v = 4; v <= 15; ++v) expected.push_back(std::to_string(v) + ".00");

        std::vector<CurveStorePoint> mem = store.Points();
        bool contentsMatch = mem.size() == CurveStore::kCapacity;
        for (std::size_t i = 0; contentsMatch && i < mem.size(); ++i) {
            const CurveStorePoint::Entry* entry = mem[i].Find("CNY");
            contentsMatch = entry && !entry->missing && entry->text == expected[i];
        }

        const bool newest = store.Newest(3).size() == 3 &&
                            AmountText(store.Newest(3).front(), "CNY") == "CNY=\"13.00\"" &&
                            AmountText(store.Newest(3).back(), "CNY") == "CNY=\"15.00\"";
        const bool ok = mem.size() == CurveStore::kCapacity && contentsMatch && newest &&
                        disk.size() == CurveStore::kCapacity &&
                        AllTexts(disk, "CNY") == AllTexts(mem, "CNY") && reloaded.pointsLoaded == 12;
        h->Req("case2", "ring holds exactly the newest 12 of 15 distinct values",
               "memory=" + Num(static_cast<long long>(mem.size())) + " points " +
                   AllTexts(mem, "CNY") + ", expected newest 12 [" +
                   [&expected] {
                       std::string joined;
                       for (std::size_t i = 0; i < expected.size(); ++i) {
                           if (i != 0) joined += ",";
                           joined += expected[i];
                       }
                       return joined;
                   }() +
                   "], file=" + AllTexts(disk, "CNY"),
               ok);
    }

    // -----------------------------------------------------------------------
    // (3) a point where one currency is absent round-trips with that currency still
    //     absent, and the present one keeps its exact decimal digits (no re-format).
    //     Spec §2.4, acceptance 3.
    // -----------------------------------------------------------------------
    {
        CurveStore store;
        store.Append(CnyUsd("18.80", "5.50", false), kAnchor - 50);   // USD absent -> null
        store.Append(CnyUsd("0.0001", "1234567.89", true), kAnchor - 40);
        store.Append(CnyUsd("184467.00", "0.00", true), kAnchor - 30);
        store.Append(CnyUsd("-2.50", "0.001", true), kAnchor - 20);
        store.Append(CnyUsd("77.70", "5.5", true), kAnchor - 10);

        // The digits the store was handed must survive the file verbatim. Note the
        // values above are numerically DIFFERENT from each other on purpose: "18.80"
        // followed by "18.8" is the same amount in different text, and §2.1 compares
        // amounts, so that pair correctly appends nothing. A fixture made of such pairs
        // would silently hold one point instead of five -- which is exactly what an
        // earlier version of this case did, and why the values here are distinct.

        const std::vector<CurveStorePoint> before = store.Points();
        store.Save(file);
        bool readOk = false;
        const std::string raw = ReadBytes(file, &readOk);
        CurveLoadResult reloaded;
        const std::vector<CurveStorePoint> after = Reload(file, &reloaded);

        // Expected values are built with the same helper that prints them, so the
        // separator and the "present but null" marker are never hand-typed twice.
        // (Hand-typed literals were wrong three times in this case alone.)
        const std::string cnyAsText = "[18.80,0.0001,184467.00,-2.50,77.70]";
        const std::string usdAsText = "[-,1234567.89,0.00,0.001,5.5]";
        const bool cnyExact = AllTexts(after, "CNY") == cnyAsText && AllTexts(before, "CNY") == cnyAsText;
        const bool usdExact = AllTexts(after, "USD") == usdAsText && AllTexts(before, "USD") == usdAsText;

        const CurveStorePoint::Entry* first = after.empty() ? nullptr : after.front().Find("USD");
        const CurveStorePoint::Entry* second = (after.size() < 2) ? nullptr : after[1].Find("USD");
        const bool absentStaysAbsent = first && first->missing && first->text.empty() &&
                                       second && !second->missing && second->text == "1234567.89";
        const bool fileSaysNull = raw.find("\"USD\": null") != std::string::npos;
        // The digits in the file are the digits we were given: no padding to a fixed
        // number of decimals, no trimming of the trailing zero. The literals are split
        // so the escape sequences are unambiguous.
        const std::string q = "\"";
        const std::string colon = ": ";
        // Each clause separately: a single failing clause in a long && chain is
        // otherwise invisible in the evidence line.
        const bool digitsCny1 = raw.find(q + "CNY" + q + colon + q + "18.80" + q) != std::string::npos;
        const bool digitsCny2 = raw.find(q + "CNY" + q + colon + q + "0.0001" + q) != std::string::npos;
        const bool digitsUsd1 = raw.find(q + "USD" + q + colon + q + "5.5" + q) != std::string::npos;
        const bool digitsUsd2 = raw.find(q + "USD" + q + colon + q + "0.001" + q) != std::string::npos;
        const bool fileKeepsDigits = digitsCny1 && digitsCny2 && digitsUsd1 && digitsUsd2;

        // "18.80" and "18.8" are the SAME amount written differently, and "18.80" arriving
        // again after "18.8" is also the same amount. §2.1 is about the balance changing,
        // so none of these may append, and the first point's digits must be left alone.
        // (This check is why the store compares amounts instead of text: with a text
        // comparison it appended two points here, and the probe said NO.)
        CurveStore sameNumber;
        const bool sameA = sameNumber.Append(Cny("18.80"), kAnchor - 20);
        const bool sameB = sameNumber.Append(Cny("18.8"), kAnchor - 10);
        const bool sameC = sameNumber.Append(Cny("18.80"), kAnchor - 5);
        const bool sameNumberNoAppend = sameA && !sameB && !sameC && sameNumber.size() == 1 &&
                                        AllTexts(sameNumber.Points(), "CNY") == "[18.80]";
        const bool sameNumberChanged = sameNumber.Append(Cny("18.79"), kAnchor);
        const bool changedDoesAppend = sameNumberChanged && sameNumber.size() == 2 &&
                                       AllTexts(sameNumber.Points(), "CNY") == "[18.80,18.79]";

        const bool ok = after.size() == 5 && reloaded.pointsLoaded == 5 && cnyExact && usdExact &&
                        absentStaysAbsent && fileSaysNull && fileKeepsDigits && readOk &&
                        SamePoints(before, after) && sameNumberNoAppend && changedDoesAppend;
        h->Req("case3", "an absent currency stays absent; present digits are not re-formatted",
               "CNY=" + AllTexts(after, "CNY") + " USD=" + AllTexts(after, "USD") +
                   ", file has \"USD\": null=" + std::string(fileSaysNull ? "yes" : "no") +
                   ", digits in file: 18.80=" + std::string(digitsCny1 ? "y" : "N") +
                   " 0.0001=" + std::string(digitsCny2 ? "y" : "N") +
                   " USD5.5=" + std::string(digitsUsd1 ? "y" : "N") +
                   " USD0.001=" + std::string(digitsUsd2 ? "y" : "N") +
                   ", USD absent stays absent=" +
                   std::string(absentStaysAbsent ? "yes" : "no") +
                   ", \"18.80\" then \"18.8\" appends nothing=" +
                   std::string(sameNumberNoAppend ? "yes" : "NO") +
                   ", a really different value appends=" +
                   std::string(changedDoesAppend ? "yes" : "NO") +
                   ", clauses: size=" + std::string(after.size() == 5 ? "y" : "N") +
                   " loaded=" + std::string(reloaded.pointsLoaded == 5 ? "y" : "N") +
                   " pointsIdentical=" + std::string(SamePoints(before, after) ? "y" : "N") +
                   " readOk=" + std::string(readOk ? "y" : "N"),
               ok);
        if (verbose && !ok) {
            std::printf("---- case3 detail ----\n");
            std::printf("  after  CNY=%s\n", Dump(AllTexts(after, "CNY")).c_str());
            std::printf("  after  USD=%s\n", Dump(AllTexts(after, "USD")).c_str());
            std::printf("  want   CNY=%s\n", Dump(cnyAsText).c_str());
            std::printf("  want   USD=%s\n", Dump(usdAsText).c_str());
            std::printf("  before CNY=%s\n", Dump(AllTexts(before, "CNY")).c_str());
            std::printf("  before USD=%s\n", Dump(AllTexts(before, "USD")).c_str());
            std::printf("  raw file   =%s\n", Dump(raw).c_str());
            std::printf("  per point  before: %s\n", PointSummary(before).c_str());
            std::printf("  per point  after : %s\n", PointSummary(after).c_str());
            std::printf("  entries    after :");
            for (const CurveStorePoint& point : after) {
                std::printf(" [");
                for (const CurveStorePoint::Entry& e : point.entries) {
                    std::printf("%s=%s ", e.currency.c_str(), e.missing ? "<null>" : e.text.c_str());
                }
                std::printf("]");
            }
            std::printf("\n");
        }
    }

    // -----------------------------------------------------------------------
    // (4) a stored timestamp older than 86400 s: loading discards the points.
    //     Spec §2.3, acceptance 8 (the "beyond the threshold" half).
    // -----------------------------------------------------------------------
    {
        // The "fresh" fixtures are stamped relative to a clock read immediately before
        // each load. Stamping them from a `now` taken earlier in the case lets a slow
        // run drift past the 86400 s threshold and flip a KEPT fixture into an expired
        // one -- measured: the edge case is only 10 s inside the limit.
        const int64_t old = static_cast<int64_t>(::time(nullptr)) - 200000;   // ~2.3 days
        const auto writeAt = [&file](int64_t stamp) {
            // The points are written by hand so the file on disk is known exactly,
            // independently of how the store happens to serialise.
            std::string text = "{\n  \"update_at\": " + Num(stamp) + ",\n  \"points\": [";
            for (int v = 1; v <= 5; ++v) {
                text += (v == 1 ? "\n    {" : ",\n    {");
                text += std::string(" \"CNY\": \"") + std::to_string(v) + ".00\" }";
            }
            text += "\n  ]\n}\n";
            WriteBytes(file, text);
        };

        writeAt(old);
        CurveStore stale;
        const CurveLoadResult staleResult = stale.Load(file);

        writeAt(static_cast<int64_t>(::time(nullptr)) - CurveStore::kExpirySeconds + 10);   // inside by 10 s
        CurveStore edgeStore;
        const CurveLoadResult edgeResult = edgeStore.Load(file);

        writeAt(static_cast<int64_t>(::time(nullptr)) - 60);   // well inside the threshold
        CurveStore fresh;
        const CurveLoadResult freshResult = fresh.Load(file);

        const std::string staleDetail = staleResult.detail;

        // The same rule on the append path (§2.3): a wide gap discards the stored
        // points and the incoming observation is recorded as the first point.
        const std::string appendPath = PathOf(probe, "stale-append.json");
        CurveStore appendStore;
        appendStore.Append(Cny("1.00"), 5000);
        appendStore.Append(Cny("2.00"), 5010);
        const bool appendSeeded = appendStore.size() == 2;
        appendStore.Save(appendPath);
        const bool appendStale = appendStore.Append(Cny("3.00"), 5010 + CurveStore::kExpirySeconds + 1);
        const std::string afterStale = AllTexts(appendStore.Points(), "CNY");
        const std::string staleReason = appendStore.lastClearReason();
        const bool appendNear = appendStore.Append(Cny("4.00"), 5010 + CurveStore::kExpirySeconds + 11);
        const std::string nearReason = appendStore.lastClearReason();
        RemoveFile(appendPath);
        // The first fetch ever, and the first after a file with no usable timestamp:
        // there is no elapsed time to measure, so it must START the curve rather than
        // be judged stale. (This is the case that made a fresh install drop its own
        // first point before the store learned to tell "unknown clock" from "old".)
        CurveStore firstEver;
        const bool firstAppends = firstEver.Append(Cny("7.00"), 5000);
        CurveStore noClock;
        const bool noClockAppends = noClock.Append(Cny("7.00"), 5000);
        const std::string noClockReason = noClock.lastClearReason();

        // Expected strings are the same shape AllTexts produces: bare amounts, "," separator.
        const std::string afterStaleExpected = "[3.00]";
        const std::string nearTextsExpected = "[3.00,4.00]";

        const bool ok = staleResult.status == CurveLoadResult::Status::Expired &&
                        staleResult.discarded && stale.empty() && staleResult.pointsLoaded == 0 &&
                        staleResult.pointsDiscarded == 5 &&
                        edgeResult.status == CurveLoadResult::Status::Loaded && edgeStore.size() == 5 &&
                        freshResult.status == CurveLoadResult::Status::Loaded && fresh.size() == 5 &&
                        stale.lastUpdate() == 0 && appendSeeded &&
                        appendStale && afterStale == afterStaleExpected && staleReason == "stale" &&
                        appendNear && nearReason.empty() &&
                        AllTexts(appendStore.Points(), "CNY") == nearTextsExpected &&
                        appendStore.lastUpdate() == 5010 + CurveStore::kExpirySeconds + 11 &&
                        firstAppends && firstEver.size() == 1 && noClockAppends &&
                        noClock.size() == 1 && noClockReason == "no timestamp";
        // Each clause separately: one failing clause inside a long && chain is otherwise
        // invisible in the evidence line, and that is how a wrong expectation hides.
        const std::string clauses =
            std::string(" clauses: loadExpired=") + (staleResult.status == CurveLoadResult::Status::Expired ? "y" : "N") +
            " discarded=" + (staleResult.discarded ? "y" : "N") +
            " staleEmpty=" + (stale.empty() ? "y" : "N") +
            " staleLoaded0=" + (staleResult.pointsLoaded == 0 ? "y" : "N") +
            " staleDropped5=" + (staleResult.pointsDiscarded == 5 ? "y" : "N") +
            " edgeLoaded=" + (edgeResult.status == CurveLoadResult::Status::Loaded ? "y" : "N") +
            " edgeSize=" + (edgeStore.size() == 5 ? "y" : "N") +
            " freshLoaded=" + (freshResult.status == CurveLoadResult::Status::Loaded ? "y" : "N") +
            " freshSize=" + (fresh.size() == 5 ? "y" : "N") +
            " staleStamp0=" + (stale.lastUpdate() == 0 ? "y" : "N") +
            " seeded2=" + (appendSeeded ? "y" : "N") +
            " appendStale=" + (appendStale ? "y" : "N") +
            " afterStale=[3.00]=" + (afterStale == afterStaleExpected ? "y" : "N") +            " staleReason=" + (staleReason == "stale" ? "y" : "N") +
            " appendNear=" + (appendNear ? "y" : "N") +
            " nearReasonEmpty=" + (nearReason.empty() ? "y" : "N") +
            " nearTexts=" + (AllTexts(appendStore.Points(), "CNY") == nearTextsExpected ? "y" : "N") +
            " nearStamp=" + (appendStore.lastUpdate() == 5010 + CurveStore::kExpirySeconds + 11 ? "y" : "N") +
            " firstAppends=" + (firstAppends ? "y" : "N") +
            " firstSize1=" + (firstEver.size() == 1 ? "y" : "N") +
            " noClockAppends=" + (noClockAppends ? "y" : "N") +
            " noClockSize1=" + (noClock.size() == 1 ? "y" : "N") +
            " noClockReason=" + (noClockReason == "no timestamp" ? "y" : "N");

        if (verbose && !ok) {
            std::printf("---- case4 detail ----\n  afterStale=%s\n  want      =%s\n  nearTexts=%s\n  want      =%s\n%s\n",
                        Dump(afterStale).c_str(), Dump(afterStaleExpected).c_str(),
                        Dump(AllTexts(appendStore.Points(), "CNY")).c_str(),
                        Dump(nearTextsExpected).c_str(), clauses.c_str());
        }

        h->Req("case4", "a stored update_at older than 86400 s discards the points",
               staleResult.StatusName() + std::string(" -> ") +
                   Num(static_cast<long long>(stale.size())) + " points (" + staleDetail + ")" +
                   ", 86390 s old -> " + edgeResult.StatusName() + " " +
                   Num(static_cast<long long>(edgeStore.size())) + " points" +
                   ", 60 s old -> " + freshResult.StatusName() + " " +
                   Num(static_cast<long long>(fresh.size())) + " points" +
                   "; append path: 2 points then a gap of 86401 s -> " + afterStale + " (reason=" +
                   (staleReason.empty() ? "none" : staleReason) + "), then a fetch 10 s later -> " +
                   AllTexts(appendStore.Points(), "CNY") + " (reason=" +
                   (nearReason.empty() ? "none" : nearReason) + "); unknown clock -> " +
                   Num(static_cast<long long>(noClock.size())) + " point, reason=" +
                   (noClockReason.empty() ? "none" : noClockReason),
               ok);
        RemoveFile(file);
    }

    // -----------------------------------------------------------------------
    // (5) malformed, empty and missing files each yield an EMPTY store, not a crash
    //     and not a partially loaded one. Spec §2.4 / the failure policy.
    // -----------------------------------------------------------------------
    {
        const std::string placeholder = "@F@";
        const std::string base =
            "{\"update_at\": 5000, \"points\": [ {\"CNY\": \"1.00\", \"USD\": null}, @F@ {\"CNY\": \"2.00\"} ]}";

        struct BadFile {
            const char* name;
            std::string bytes;
        };
        const std::vector<BadFile> bad = {
            {"missing", std::string()},                                        // never written
            {"empty", std::string()},                                          // written, zero bytes
            {"notjson", "<html><body>gateway</body></html>"},
            {"trailing", base + " junk"},
            {"truncated", base.substr(0, base.size() / 2)},
            {"pointNotObject", SearchReplaceAll(base, placeholder, "\"CNY\"")},
            {"pointNull", SearchReplaceAll(base, placeholder, "null")},
            {"pointNoCurrency", SearchReplaceAll(base, placeholder, "{}")},
            {"pointBadAmount", SearchReplaceAll(base, placeholder, "{\"CNY\": \"1.2.3\"}")},
            {"pointIsArray", SearchReplaceAll(base, placeholder, "[\"CNY\"]")},
            {"pointsNotArray", "{\"update_at\": 5000, \"points\": 7}"},
            {"updateAtString", "{\"update_at\": \"5000\", \"points\": [{\"CNY\": \"1.00\"}]}"},
            {"updateAtFraction", "{\"update_at\": 5000.5, \"points\": [{\"CNY\": \"1.00\"}]}"},
            {"noUpdateAt", "{\"points\": [{\"CNY\": \"1.00\"}]}"},
            {"justNull", "null"},
            {"justNumber", "42"},
            {"justArray", "[1,2,3]"},
            // Not a depth test: json_min.cpp already has its own depth cap. This one
            // pins that a huge exponent cannot become a timestamp, and that a point
            // above the accepted magnitude is rejected instead of silently wrapped.
            {"digitsNeverWrap",
             "{\"update_at\": 1e309, \"points\": [{\"CNY\": \"999999999999999999999999.00\"}]}"},
        };

        int malformedCount = 0;
        int emptyCount = 0;
        int missingCount = 0;
        int noTimestampCount = 0;
        int unexpectedStatus = 0;
        int loadedAnyPoints = 0;
        int threw = 0;
        std::string firstUnexpected;

        // update_at as an explicit null: the file is readable, but its points cannot be
        // dated, so they are dropped and the next fetch starts the curve. A NULL stamp is
        // NOT an expired one -- there is nothing to expire -- which is why the append
        // below must not be judged as a 56-year gap against the epoch.
        CurveStore nullStampBefore;
        const std::string nullStampPath = PathOf(probe, "null-stamp.json");
        WriteBytes(nullStampPath, "{\"update_at\": null, \"points\": [{\"CNY\": \"1.00\"}]}");
        const CurveLoadResult nullStampResult = nullStampBefore.Load(nullStampPath);
        const bool nullStampOk = nullStampResult.status == CurveLoadResult::Status::NoTimestamp &&
                                  nullStampResult.resetsClock && nullStampBefore.empty() &&
                                  nullStampResult.pointsDiscarded == 1;
        const int64_t nullStampNow = 6000;   // deliberately tiny: nothing may treat it as stale
        const bool nullStampAppendResult = nullStampBefore.Append(Cny("9.99"), nullStampNow);
        const std::string nullStampTexts = AllTexts(nullStampBefore.Points(), "CNY");
        const int64_t nullStampStamp = nullStampBefore.lastUpdate();
        const std::string nullStampWhy = nullStampBefore.lastClearReason();
        const bool nullStampAppend = nullStampAppendResult && nullStampBefore.size() == 1 &&
                                     nullStampTexts == "[9.99]" &&
                                     nullStampStamp == nullStampNow;
        RemoveFile(nullStampPath);

        for (const BadFile& entry : bad) {
            const std::string path = PathOf(probe, std::string("bad-") + entry.name + ".json");
            RemoveFile(path);
            if (std::strcmp(entry.name, "missing") != 0) WriteBytes(path, entry.bytes);

            try {
                CurveStore store;
                const CurveLoadResult result = store.Load(path);
                const bool isEmpty = store.empty() && result.pointsLoaded == 0 &&
                                     store.lastUpdate() == 0 && !result.ok;
                switch (result.status) {
                    case CurveLoadResult::Status::Malformed:
                        ++malformedCount;
                        break;
                    case CurveLoadResult::Status::Empty:
                        ++emptyCount;
                        break;
                    case CurveLoadResult::Status::Missing:
                        ++missingCount;
                        break;
                    case CurveLoadResult::Status::NoTimestamp:
                        // Correct, not a failure: a file whose points exist but carry no
                        // update_at cannot be dated, so the points are dropped.
                        ++noTimestampCount;
                        break;
                    default:
                        ++unexpectedStatus;
                        break;
                }
                if (!isEmpty) {
                    ++loadedAnyPoints;
                    if (firstUnexpected.empty()) {
                        firstUnexpected = std::string(entry.name) + " kept " +
                                          Num(static_cast<long long>(store.size())) + " points";
                    }
                }
                if (verbose) {
                    std::printf("  fixture %-18s -> %-12s %s\n", entry.name, result.StatusName(),
                                result.detail.c_str());
                }
                if (result.detail.empty()) {
                    ++loadedAnyPoints;
                    if (firstUnexpected.empty()) firstUnexpected = std::string(entry.name) + " no detail";
                }
            } catch (...) {
                ++threw;
                if (firstUnexpected.empty()) firstUnexpected = std::string(entry.name) + " threw";
            }
            RemoveFile(path);
        }

        // Every case must be classified, and each must land in the class its name says.
        // The 18 fixtures: 15 malformed, 1 written with zero bytes, 1 never written, and
        // 1 whose points are undateable ("update_at" absent; the "update_at": null file is
        // covered separately below and is also undateable).
        const bool ok = malformedCount == 15 && emptyCount == 1 && missingCount == 1 &&
                        noTimestampCount == 1 && unexpectedStatus == 0 && loadedAnyPoints == 0 &&
                        threw == 0 && nullStampOk && nullStampAppend;
        h->Req("case5", "malformed / empty / missing file -> empty store, no crash, no partial load",
               Num(static_cast<long long>(bad.size())) + " bad files: " +
                   Num(malformedCount) + " malformed, " + Num(emptyCount) + " empty, " +
                   Num(missingCount) + " missing, " + Num(noTimestampCount) + " undateable, " +
                   Num(unexpectedStatus) + " other; empty in all=" +
                   std::string(loadedAnyPoints == 0 ? "yes" : "NO") + "; exceptions=" + Num(threw) +
                   (firstUnexpected.empty() ? std::string() : "; first oddity: " + firstUnexpected) +
                   "; update_at null -> " + nullStampResult.StatusName() +
                   " resetsClock=" + std::string(nullStampResult.resetsClock ? "yes" : "no") +
                   " + append works=" + std::string(nullStampAppend ? "yes" : "no") +
                   " clauses: malformed15=" + std::string(malformedCount == 15 ? "y" : "N") +                   " empty1=" + std::string(emptyCount == 1 ? "y" : "N") +
                   " missing1=" + std::string(missingCount == 1 ? "y" : "N") +
                   " noTimestamp2=" + std::string(noTimestampCount == 1 ? "y" : "N") +
                   " other0=" + std::string(unexpectedStatus == 0 ? "y" : "N") +
                   " noneLoaded=" + std::string(loadedAnyPoints == 0 ? "y" : "N") +
                   " noThrow=" + std::string(threw == 0 ? "y" : "N") +
                   " nullStampOk=" + std::string(nullStampOk ? "y" : "N") +
                   " nullStampAppend=" + std::string(nullStampAppend ? "y" : "N") +
                   " [append=" + std::string(nullStampAppendResult ? "true" : "false") +
                   " size=" + Num(static_cast<long long>(nullStampBefore.size())) +
                   " texts=" + nullStampTexts + " stamp=" + Num(nullStampStamp) +
                   " why=" + (nullStampWhy.empty() ? "none" : nullStampWhy) + "]",
               ok);
    }

    // -----------------------------------------------------------------------
    // (6) saving then loading yields identical points and an identical timestamp.
    //     Acceptance 8 (the "within two minutes" half).
    // -----------------------------------------------------------------------
    {
        CurveStore store;
        store.Append(CnyUsd("18.80", "5.50", false), kAnchor - 50);
        store.Append(CnyUsd("18.60", "5.50", true), kAnchor - 40);
        store.Append(CnyUsd("18.60", "5.40", true), kAnchor - 30);   // primary unchanged -> no point
        store.Append(CnyUsd("12.05", "5.40", true), kAnchor - 20);
        store.Append(CnyUsd("11.9999", "5.40", true), kAnchor - 10);

        const std::vector<CurveStorePoint> before = store.Points();
        const int64_t stampBefore = store.lastUpdate();
        const bool saved = store.Save(file);

        CurveStore reloaded;
        const CurveLoadResult result = reloaded.Load(file);
        const std::vector<CurveStorePoint> after = reloaded.Points();

        bool readOk = false;
        const std::string raw = ReadBytes(file, &readOk);
        if (verbose) {
            std::printf("---- %s (read ok=%s) ----\n%s----------------\n", file.c_str(),
                        readOk ? "yes" : "no", raw.c_str());
        }

        // The change comparison must survive the reload: the same value appends nothing.
        const std::string reloadedPrimaryCurrency = reloaded.lastPrimaryCurrency();
        const std::string reloadedPrimaryText = reloaded.lastPrimaryText();
        const bool sameValueNoAppend = !reloaded.Append(CnyUsd("11.9999", "5.40", true), kAnchor);
        const bool newValueAppends = reloaded.Append(CnyUsd("11.50", "5.40", true), kAnchor + 10);

        const bool ok = saved && result.ok && result.status == CurveLoadResult::Status::Loaded &&
                        before.size() == 4 && after.size() == 4 && SamePoints(before, after) &&
                        reloaded.lastUpdate() == kAnchor + 10 && stampBefore == kAnchor - 10 &&
                        reloadedPrimaryCurrency == "CNY" && reloadedPrimaryText == "11.9999" &&
                        sameValueNoAppend && newValueAppends && reloaded.size() == 5 &&
                        !reloaded.Expired(kAnchor + 60);
        h->Req("case6", "save then load: identical points and identical update_at",
               "saved=" + std::string(saved ? "yes" : "no") + ", " +
                   Num(static_cast<long long>(before.size())) + " points before -> " +
                   PointSummary(before) + ", reload " + result.StatusName() + " -> " +
                   Num(static_cast<long long>(after.size())) + " points " + PointSummary(after) +
                   ", identical=" + std::string(SamePoints(before, after) ? "yes" : "NO") +
                   ", update_at " + Num(stampBefore) + " -> " + Num(result.pointsLoaded) +
                   " point(s), now " + Num(reloaded.lastUpdate()) +
                   ", reloaded primary=" + reloadedPrimaryCurrency + " " + Quote(reloadedPrimaryText) +
                   ", after reload: same value appended=" +
                   std::string(sameValueNoAppend ? "no" : "YES(bug)") + ", new value appended=" +
                   std::string(newValueAppends ? "yes" : "NO(bug)"),
               ok);
        RemoveFile(file);
    }

    // -----------------------------------------------------------------------
    // (7) ★ per-point timestamps (curve_store §2.3b): a point appended now carries its
    //     OWN `at`, an OLD file with no `at` loads as UNDATED (**it must not inherit the
    //     file's global update_at**), and an undated point survives a round trip as
    //     undated. The estimator refuses undated points, so this is the half of that
    //     guarantee that lives in the store.
    // -----------------------------------------------------------------------
    {
        // Counts occurrences of `"at": <digits>` (a real per-point time).
        const auto countDatedAt = [](const std::string& text) {
            int found = 0;
            const std::string needle = "\"at\": ";
            std::size_t at = 0;
            while ((at = text.find(needle, at)) != std::string::npos) {
                const std::size_t digit = at + needle.size();
                if (digit < text.size() && text[digit] >= '0' && text[digit] <= '9') ++found;
                at = digit;
            }
            return found;
        };
        const auto countWhere = [](const std::string& text, const std::string& needle) {
            int found = 0;
            std::size_t at = 0;
            while ((at = text.find(needle, at)) != std::string::npos) {
                ++found;
                at += needle.size();
            }
            return found;
        };

        // (a) An OLD file: `update_at` present, not a single `at` on any point. Both
        //     points must load UNDATED -- stamping them with update_at would claim both
        //     were measured at the same instant, which is the fabrication `at` exists to
        //     remove, and it would feed a rate estimate times nobody ever measured.
        const std::string atFile = PathOf(probe, "at.json");
        const int64_t fileStamp = static_cast<int64_t>(::time(nullptr)) - 30;
        WriteBytes(atFile, "{\"update_at\": " + Num(fileStamp) +
                               ", \"points\": [{\"CNY\": \"18.80\"}, {\"CNY\": \"18.60\"}]}");

        CurveStore oldFile;
        const CurveLoadResult oldResult = oldFile.Load(atFile);
        const std::vector<CurveStorePoint> oldPoints = oldFile.Points();
        const bool oldLoaded = oldResult.status == CurveLoadResult::Status::Loaded &&
                               oldResult.pointsLoaded == 2 && oldPoints.size() == 2;
        const bool oldBothUndated = oldLoaded && !oldPoints[0].atValid && !oldPoints[1].atValid &&
                                    oldPoints[0].at == 0 && oldPoints[1].at == 0;
        // ...and the global timestamp keeps its own job: the store is still FRESH.
        const bool globalStampKept = oldFile.lastUpdate() == fileStamp;

        // (b) Round trip: those undated points must come back undated, without any help
        //     from the global timestamp.
        const bool resaved = oldFile.Save(atFile);
        bool readOk = false;
        const std::string raw = ReadBytes(atFile, &readOk);
        CurveStore roundTrip;
        const CurveLoadResult roundResult = roundTrip.Load(atFile);
        const std::vector<CurveStorePoint> roundPoints = roundTrip.Points();
        const bool survived = roundResult.status == CurveLoadResult::Status::Loaded &&
                              roundPoints.size() == 2 && !roundPoints[0].atValid &&
                              !roundPoints[1].atValid &&
                              AllTexts(roundPoints, "CNY") == AllTexts(oldPoints, "CNY");
        // The file says so explicitly: one `"at": null` per point, and no dated `at`.
        const bool fileSaysUndated = readOk && countWhere(raw, "\"at\": null") == 2 &&
                                     countDatedAt(raw) == 0;

        // (c) A point appended right now is DATED with the clock it was given, and that
        //     time survives both reloads. (The append happens on the round-tripped store,
        //     so this also proves an undated file heals from the next point on.)
        const int64_t appendAt = fileStamp + 20;
        const bool appended = roundTrip.Append(Cny("18.55"), appendAt);
        const std::vector<CurveStorePoint> afterAppend = roundTrip.Points();
        const bool newPointDated = appended && afterAppend.size() == 3 &&
                                   afterAppend.back().atValid && afterAppend.back().at == appendAt &&
                                   !afterAppend[0].atValid && !afterAppend[1].atValid;
        const bool savedAgain = roundTrip.Save(atFile);
        CurveStore finalStore;
        const CurveLoadResult finalResult = finalStore.Load(atFile);
        const std::vector<CurveStorePoint> finalPoints = finalStore.Points();
        const bool finalOk = savedAgain && finalResult.status == CurveLoadResult::Status::Loaded &&
                             finalPoints.size() == 3 && !finalPoints[0].atValid &&
                             !finalPoints[1].atValid && finalPoints[2].atValid &&
                             finalPoints[2].at == appendAt &&
                             AllTexts(finalPoints, "CNY") == "[18.80,18.60,18.55]";

        // (d) A point whose "at" is present but is not whole seconds: refuse to guess a
        //     time (undated), rather than invent one from the digits.
        const std::string oddFile = PathOf(probe, "odd-at.json");
        WriteBytes(oddFile, "{\"update_at\": " + Num(fileStamp) +
                                ", \"points\": [{\"CNY\": \"5.00\", \"at\": 1.5},"
                                " {\"CNY\": \"4.00\", \"at\": \"x\"}]}");
        CurveStore oddStore;
        const CurveLoadResult oddResult = oddStore.Load(oddFile);
        const std::vector<CurveStorePoint> oddPoints = oddStore.Points();
        const bool oddUndated = oddResult.status == CurveLoadResult::Status::Loaded &&
                                oddPoints.size() == 2 && !oddPoints[0].atValid && !oddPoints[1].atValid;
        RemoveFile(oddFile);

        const bool ok = oldBothUndated && globalStampKept && resaved && fileSaysUndated &&
                        survived && newPointDated && finalOk && oddUndated;
        h->Req("case7", "per-point \"at\": an old file loads undated and stays undated",
               "old file (update_at only): loaded=" + std::string(oldLoaded ? "yes" : "NO") +
                   ", both points undated=" + std::string(oldBothUndated ? "yes" : "NO") +
                   ", global update_at kept=" + std::string(globalStampKept ? "yes" : "NO") +
                   " (" + Num(oldFile.lastUpdate()) + " == " + Num(fileStamp) + ")" +
                   ", round trip kept them undated=" + std::string(survived ? "yes" : "NO") +
                   ", file has \"at\": null x" + Num(countWhere(raw, "\"at\": null")) +
                   " and dated \"at\" x" + Num(countDatedAt(raw)) +
                   ", appended now -> at=" +
                   (afterAppend.empty() || !afterAppend.back().atValid
                        ? std::string("UNDATED(bug)")
                        : Num(static_cast<long long>(afterAppend.back().at))) +
                   " (== " + Num(appendAt) + ")=" + std::string(newPointDated ? "yes" : "NO") +
                   ", after save+load: " + AllTexts(finalPoints, "CNY") + " with dated flags " +
                   [&finalPoints] {
                       std::string flags;
                       for (const CurveStorePoint& point : finalPoints) flags += point.atValid ? "D" : "u";
                       return flags.empty() ? std::string("(none)") : flags;
                   }() +
                   ", odd \"at\" (1.5 and \"x\") -> undated=" + std::string(oddUndated ? "yes" : "NO"),
               ok);
        RemoveFile(atFile);
    }
}

// ---------------------------------------------------------------------------
// The harness must be able to fail, or every PASS above is worthless
// ---------------------------------------------------------------------------
void RunHarnessSelfCheck(Harness* h) {
    Harness throwaway;
    const bool good = throwaway.Req("selfcheck:probe-true", "harness reports a true check as PASS",
                                    "expected true", true);
    // A check that is KNOWN false, so "the harness can say no" is measured rather than
    // asserted. The FAIL line it would print is unwanted, hence ReqSilent().
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
    bool keep = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--keep") {
            keep = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::printf("storeprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: storeprobe [--keep] [--verbose]\n");
            return 64;
        }
    }

    Harness h;
    const ProbeDir probe = MakeProbeDir();
    if (!probe.ok) {
        std::printf("FAIL: setup | cannot create a temporary directory under %%TEMP%%\n");
        return 1;
    }
    std::printf("storeprobe: offline curve-store proof; temporary file: %s\n",
                PathOf(probe, "curve.json").c_str());

    try {
        RunHarnessSelfCheck(&h);
        RunChecks(&h, probe, verbose);
    } catch (const std::exception& e) {
        std::printf("FAIL: runner | an exception escaped a check: %s\n", e.what());
        ++h.failed;
    } catch (...) {
        std::printf("FAIL: runner | an unknown exception escaped a check\n");
        ++h.failed;
    }

    if (keep) {
        std::printf("kept: %s\n", probe.dir.c_str());
    } else {
        RemoveDirectoryW(probe.wideDir.c_str());
    }

    std::printf("checks: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& failure : h.failures) {
        std::printf("failed line: %s\n", failure.c_str());
    }
    std::printf("RESULT: %s\n", h.failed == 0 && h.passed > 0 ? "PASS" : "FAIL");
    return (h.failed == 0 && h.passed > 0) ? 0 : 1;
}
