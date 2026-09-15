// apiprobe -- offline proof for the balance API core, plus one optional real request.
//
//   apiprobe --cases                 recorded bodies and status codes, no network at all
//   apiprobe --live                  one real request using DEEPSEEK_API_KEY
//   apiprobe --live --host=127.0.0.1 --port=18080 --plain-http
//                                    point the client somewhere else (steps J3/J4 must be
//                                    exercisable without waiting for a real failure)
//
// --live prints exactly four things and nothing else: the status, the HTTP code, the entry
// count and the currencies. The key is never printed, not even a fragment, not even on error
// (design 10.5 / steps J6).
//
// Written for this probe only; the widget's own argument parsing is not touched.
#include "../src/api_client.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using dshb::api::BalanceEntry;
using dshb::api::BalanceResult;
using dshb::api::Endpoint;
using dshb::api::RetryPlan;
using dshb::api::Status;

namespace {

// --- recorded bodies ---------------------------------------------------------

// The documented shape (design 2.1), single CNY entry.
const char* kBodySingleCny = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"}]})";

// The same account carrying both currencies, and the same array with the two entries
// swapped. Design 2.2: the order must never decide which balance is used.
const char* kBodyCnyUsd = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"},{"currency":"USD","total_balance":"5.50","granted_balance":"0.00","topped_up_balance":"5.50"}]})";
const char* kBodyUsdCny = R"({"is_available":true,"balance_infos":[{"currency":"USD","total_balance":"5.50","granted_balance":"0.00","topped_up_balance":"5.50"},{"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"}]})";

const char* kBodyEmptyArray = R"({"is_available":true,"balance_infos":[]})";
const char* kBodyMissingTotal = R"({"is_available":true,"balance_infos":[{"currency":"CNY","granted_balance":"10.00","topped_up_balance":"100.00"}]})";
const char* kBodyTruncated = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_bal)";
const char* kBodyNotJson = R"(<html><head><title>502 Bad Gateway</title></head></html>)";

// Decimals must survive verbatim (steps J2: "the decimals being eaten" is a named failure).
const char* kBodyDecimals = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"0.10","granted_balance":"0.00","topped_up_balance":"1234.56"}]})";

// \u escapes must decode: "C\u004eY" is CNY.
const char* kBodyEscapedCurrency = R"({"is_available":true,"balance_infos":[{"currency":"C\u004eY","total_balance":"7.70","granted_balance":"0.00","topped_up_balance":"7.70"}]})";

const char* kBodyUnknownCurrency = R"({"is_available":true,"balance_infos":[{"currency":"EUR","total_balance":"9.99","granted_balance":"0.00","topped_up_balance":"9.99"}]})";

// First entry unusable, second fine: a readable balance must still come back.
const char* kBodyCnyNoTotalUsdTotal = R"({"is_available":true,"balance_infos":[{"currency":"CNY","granted_balance":"10.00","topped_up_balance":"100.00"},{"currency":"USD","total_balance":"5.50","granted_balance":"0.00","topped_up_balance":"5.50"}]})";

const char* kBodyIsAvailableMissing = R"({"balance_infos":[{"currency":"CNY","total_balance":"110.00"}]})";
const char* kBodyIsAvailableString = R"({"is_available":"true","balance_infos":[{"currency":"CNY","total_balance":"110.00"}]})";
const char* kBodyInfosMissing = R"({"is_available":true})";
const char* kBodyInfosObject = R"({"is_available":true,"balance_infos":{"currency":"CNY"}})";
const char* kBodyTotalNotDecimal = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"1.2.3"}]})";
const char* kBodyTotalNotString = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":110.00}]})";
const char* kBodyEntryNotObject = R"({"is_available":true,"balance_infos":["CNY"]})";
const char* kBodyTrailingGarbage = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"110.00"}]} extra)";
const char* kBodyDuplicateTotal = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"110.00","total_balance":"220.00"}]})";

std::string Currencies(const BalanceResult& result) {
    std::string text;
    for (const BalanceEntry& entry : result.entries) {
        if (!text.empty()) text += ",";
        text += entry.currency.empty() ? "?" : entry.currency;
    }
    return text.empty() ? std::string("(none)") : text;
}

std::string AmountsByCurrency(const BalanceResult& result) {
    std::string text;
    for (const BalanceEntry& entry : result.entries) {
        if (!text.empty()) text += " ";
        text += entry.currency;
        text += "=";
        text += entry.hasTotal ? entry.totalBalance : std::string("(absent)");
    }
    return text.empty() ? std::string("(none)") : text;
}

std::string Summary(const BalanceResult& result) {
    std::string text = "status=";
    text += dshb::api::StatusName(result.status);
    text += " http=";
    text += std::to_string(result.httpStatus);
    text += " avail=";
    text += result.hasIsAvailable ? (result.isAvailable ? "true" : "false") : std::string("absent");
    text += " entries=";
    text += std::to_string(result.entries.size());
    text += " [";
    text += AmountsByCurrency(result);
    text += "]";
    return text;
}

// --- case running ------------------------------------------------------------

struct Expect {
    Status status = Status::Ok;
    int http = -1;          // -1: do not check
    int entries = -1;       // -1: do not check
    std::string currencies; // empty: do not check (joined with ',', entry order)
    int available = -1;     // -1: do not check; 0 false; 1 true
};

bool Check(const BalanceResult& result, const Expect& want, std::string* why) {
    if (result.status != want.status) {
        *why = std::string("status ") + dshb::api::StatusName(result.status) + " != " +
               dshb::api::StatusName(want.status);
        return false;
    }
    if (want.http >= 0 && result.httpStatus != want.http) {
        *why = "http " + std::to_string(result.httpStatus) + " != " + std::to_string(want.http);
        return false;
    }
    if (want.entries >= 0 && static_cast<int>(result.entries.size()) != want.entries) {
        *why = "entries " + std::to_string(result.entries.size()) + " != " + std::to_string(want.entries);
        return false;
    }
    if (!want.currencies.empty() && Currencies(result) != want.currencies) {
        *why = "currencies " + Currencies(result) + " != " + want.currencies;
        return false;
    }
    if (want.available >= 0 && static_cast<int>(result.isAvailable ? 1 : 0) != want.available) {
        *why = "is_available mismatch";
        return false;
    }
    return true;
}

struct Runner {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    // expectText is what the case asserts; it is printed whether or not the case passes, so
    // the output doubles as the acceptance record.
    void Case(int index, const std::string& name, const std::string& expectText, const Expect& want,
              const BalanceResult& result) {
        std::string why;
        bool ok = false;
        try {
            ok = Check(result, want, &why);
        } catch (...) {
            why = "case threw";
        }
        std::printf("%02d %-30s %-4s want[%s] got[%s]", index, name.c_str(), ok ? "PASS" : "FAIL",
                    expectText.c_str(), Summary(result).c_str());
        if (!ok) {
            std::printf("  <- %s", why.c_str());
            failures.push_back(name + ": " + why);
        }
        std::printf("\n");
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    }

    void Custom(int index, const std::string& name, const std::string& expectText, bool ok,
                const std::string& got) {
        std::printf("%02d %-30s %-4s want[%s] got[%s]\n", index, name.c_str(), ok ? "PASS" : "FAIL",
                    expectText.c_str(), got.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(name);
        }
    }
};

BalanceResult FromStatus(int httpStatus) {
    BalanceResult result;
    result.status = dshb::api::ClassifyTransport(false, httpStatus);
    result.httpStatus = httpStatus;
    return result;
}

std::string RetryText(Status status, int attempt, int retryAfter) {
    const RetryPlan plan = dshb::api::NextRetry(status, attempt, retryAfter);
    std::string text = plan.retry ? "retry=true delay=" : "retry=false delay=";
    text += std::to_string(plan.delaySeconds);
    return text;
}

int RunCases() {
    Runner r;
    std::printf("apiprobe --cases : recorded bodies and status codes, no network is used\n");
    std::printf("%-3s %-30s %-4s %s\n", "#", "case", "", "want / got");
    std::printf("----------------------------------------------------------------------------------------------------------\n");

    try {
        // --- bodies (steps J2) -------------------------------------------------
        // A body-only parse has no HTTP code to report: the code is attached by the caller
        // (see the --live path and the status-code cases below), so http is not checked here.
        r.Case(1, "200 single CNY entry", "status=Ok entries=1 [CNY=110.00] avail=true",
               Expect{Status::Ok, -1, 1, "CNY", 1}, dshb::api::ParseBalanceBody(kBodySingleCny));

        r.Case(2, "200 CNY+USD", "status=Ok entries=2 [CNY,USD]",
               Expect{Status::Ok, -1, 2, "CNY,USD", 1}, dshb::api::ParseBalanceBody(kBodyCnyUsd));

        r.Case(3, "200 USD+CNY (swapped)", "status=Ok entries=2 [USD,CNY]",
               Expect{Status::Ok, -1, 2, "USD,CNY", 1}, dshb::api::ParseBalanceBody(kBodyUsdCny));

        {
            // The named J2 failure is "picking the wrong currency when the array order
            // changes". Both orders must give the same currency -> amount mapping, and the
            // preferred entry must be CNY in both (design 2.2 rule 2).
            const BalanceResult a = dshb::api::ParseBalanceBody(kBodyCnyUsd);
            const BalanceResult b = dshb::api::ParseBalanceBody(kBodyUsdCny);
            const std::string mapA = AmountsByCurrency(a);
            const std::string mapB = AmountsByCurrency(b);
            const int pickA = dshb::api::PreferredEntryIndex(a.entries);
            const int pickB = dshb::api::PreferredEntryIndex(b.entries);
            const std::string chosenA =
                pickA >= 0 ? a.entries[static_cast<std::size_t>(pickA)].currency : std::string("(none)");
            const std::string chosenB =
                pickB >= 0 ? b.entries[static_cast<std::size_t>(pickB)].currency : std::string("(none)");
            const bool sameAmounts = mapA == "CNY=110.00 USD=5.50" && mapB == "USD=5.50 CNY=110.00";
            const bool samePick = chosenA == "CNY" && chosenB == "CNY";
            r.Custom(4, "order independence", "same currency->amount map in both orders, CNY preferred",
                     sameAmounts && samePick, mapA + " / " + mapB + " preferred=" + chosenA + "," + chosenB);
        }

        r.Case(5, "200 empty array", "status=NoEntries entries=0 (never 0.00)",
               Expect{Status::NoEntries, -1, 0, "", 1}, dshb::api::ParseBalanceBody(kBodyEmptyArray));

        {
            // Design 2.2 rule 3 / 3.2: a missing amount is an error state, and specifically
            // must NOT be reported as 0.00.
            const BalanceResult result = dshb::api::ParseBalanceBody(kBodyMissingTotal);
            bool ok = result.status == Status::ParseError;
            for (const BalanceEntry& entry : result.entries) {
                if (entry.hasTotal) ok = false; // an amount was invented for a field that was absent
            }
            r.Custom(6, "200 missing total_balance", "status=ParseError, no amount invented for the absent field",
                     ok, Summary(result));
        }

        r.Case(7, "truncated mid-string", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyTruncated));
        r.Case(8, "empty body", "status=ParseError", Expect{Status::ParseError, -1, 0, "", -1},
               dshb::api::ParseBalanceBody(""));
        r.Case(9, "not JSON at all", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyNotJson));

        {
            // A hostile body must not be able to overflow the stack: the reader caps nesting
            // and returns an error rather than crashing the probe.
            std::string deep;
            deep.reserve(2000);
            for (int i = 0; i < 2000; ++i) deep += '[';
            r.Case(10, "2000-deep nesting", "status=ParseError, no crash", Expect{Status::ParseError, -1, 0, "", -1},
                   dshb::api::ParseBalanceBody(deep));
        }

        {
            // Steps J2: "the decimals being eaten" is a named failure. The strings must come
            // back byte for byte (design 3.1: no binary float anywhere in this path).
            const BalanceResult result = dshb::api::ParseBalanceBody(kBodyDecimals);
            const bool ok = result.status == Status::Ok && result.entries.size() == 1 &&
                            result.entries[0].totalBalance == "0.10" &&
                            result.entries[0].toppedUpBalance == "1234.56";
            r.Custom(11, "decimals kept verbatim", "total=\"0.10\" topped_up=\"1234.56\" exactly as sent", ok,
                     Summary(result));
        }

        r.Case(12, "escaped currency C\\u004eY", "status=Ok entries=1 [CNY]",
               Expect{Status::Ok, -1, 1, "CNY", 1}, dshb::api::ParseBalanceBody(kBodyEscapedCurrency));

        r.Case(13, "unknown currency only", "status=ParseError",
               Expect{Status::ParseError, -1, 1, "", -1}, dshb::api::ParseBalanceBody(kBodyUnknownCurrency));

        {
            const BalanceResult result = dshb::api::ParseBalanceBody(kBodyCnyNoTotalUsdTotal);
            const int pick = dshb::api::PreferredEntryIndex(result.entries);
            const bool ok = result.status == Status::Ok && pick >= 0 &&
                            result.entries[static_cast<std::size_t>(pick)].currency == "USD";
            r.Custom(14, "CNY without total, USD fine", "status=Ok, preferred entry = USD (the one with an amount)",
                     ok, Summary(result) + " preferred=" +
                             (pick >= 0 ? result.entries[static_cast<std::size_t>(pick)].currency : std::string("(none)")));
        }

        r.Case(15, "is_available missing", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyIsAvailableMissing));
        r.Case(16, "is_available not boolean", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyIsAvailableString));
        r.Case(17, "balance_infos missing", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyInfosMissing));
        r.Case(18, "balance_infos not array", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyInfosObject));
        r.Case(19, "total_balance not decimal", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyTotalNotDecimal));
        r.Case(20, "total_balance not string", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyTotalNotString));
        r.Case(21, "entry not an object", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyEntryNotObject));
        r.Case(22, "trailing garbage after JSON", "status=ParseError",
               Expect{Status::ParseError, -1, 0, "", -1}, dshb::api::ParseBalanceBody(kBodyTrailingGarbage));

        {
            const BalanceResult result = dshb::api::ParseBalanceBody(kBodyDuplicateTotal);
            const bool ok = result.status == Status::Ok && result.entries.size() == 1 &&
                            result.entries[0].totalBalance == "220.00";
            r.Custom(23, "duplicate total_balance", "status=Ok, last occurrence wins (220.00)", ok, Summary(result));
        }

        // --- status codes (steps J3, design 4.4) -------------------------------
        r.Case(24, "http 401", "status=Unauthorized", Expect{Status::Unauthorized, 401, 0, "", -1}, FromStatus(401));

        {
            // Named J3 failure mode: 402 (out of money) shown as 401 (bad key).
            const BalanceResult result = FromStatus(402);
            const RetryPlan plan = dshb::api::NextRetry(result.status, 0);
            const bool ok = result.status == Status::PaymentRequired && result.status != Status::Unauthorized &&
                            !plan.retry;
            r.Custom(25, "http 402 is not 401", "status=PaymentRequired, distinct from Unauthorized, no retry", ok,
                     Summary(result) + " " + RetryText(Status::PaymentRequired, 0, -1));
        }

        r.Case(26, "http 403", "status=Forbidden", Expect{Status::Forbidden, 403, 0, "", -1}, FromStatus(403));
        r.Case(27, "http 429", "status=RateLimited", Expect{Status::RateLimited, 429, 0, "", -1}, FromStatus(429));
        r.Case(28, "http 500", "status=ServerError", Expect{Status::ServerError, 500, 0, "", -1}, FromStatus(500));
        r.Case(29, "http 503", "status=ServerError", Expect{Status::ServerError, 503, 0, "", -1}, FromStatus(503));
        r.Case(30, "http 400 (design 4.4)", "status=ServerError", Expect{Status::ServerError, 400, 0, "", -1},
               FromStatus(400));
        r.Case(31, "http 422 (design 4.4)", "status=ServerError", Expect{Status::ServerError, 422, 0, "", -1},
               FromStatus(422));
        r.Case(32, "http 200", "status=Ok", Expect{Status::Ok, 200, -1, "", -1}, FromStatus(200));

        {
            BalanceResult result;
            result.status = dshb::api::ClassifyTransport(true, 0);
            r.Case(33, "transport failure", "status=TransportError",
                   Expect{Status::TransportError, -1, 0, "", -1}, result);
        }

        // --- retry routing (design 4.4) ----------------------------------------
        {
            const int want[6] = {10, 20, 40, 80, 160, 160};
            std::string got;
            bool ok = true;
            for (int attempt = 0; attempt < 6; ++attempt) {
                const RetryPlan plan = dshb::api::NextRetry(Status::TransportError, attempt);
                if (!got.empty()) got += " ";
                got += std::to_string(plan.delaySeconds);
                if (!plan.retry || plan.delaySeconds != want[attempt]) ok = false;
            }
            r.Custom(34, "network backoff ladder", "10 20 40 80 160 160", ok, got);
        }
        {
            const RetryPlan noHeader = dshb::api::NextRetry(Status::RateLimited, 0, -1);
            const RetryPlan withHeader = dshb::api::NextRetry(Status::RateLimited, 0, 7);
            const bool ok = noHeader.retry && noHeader.delaySeconds == 60 && withHeader.retry &&
                            withHeader.delaySeconds == 7;
            r.Custom(35, "429 honours Retry-After", "no header -> 60 s, Retry-After: 7 -> 7 s", ok,
                     RetryText(Status::RateLimited, 0, -1) + " / " + RetryText(Status::RateLimited, 0, 7));
        }
        {
            bool ok = true;
            std::string got;
            const struct {
                const char* value;
                bool valid;
                int seconds;
            } samples[] = {
                {"7", true, 7}, {"  7  ", true, 7}, {"0", true, 0}, {"", false, 0},
                {"-1", false, 0}, {"abc", false, 0}, {"Wed, 21 Oct 2015 07:28:00 GMT", false, 0},
                {"999999999", true, 86400},
            };
            for (const auto& sample : samples) {
                int seconds = -1;
                const bool valid = dshb::api::ParseRetryAfterSeconds(sample.value, &seconds);
                if (!got.empty()) got += " ";
                got += "\"";
                got += sample.value;
                got += "\"=";
                got += valid ? std::to_string(seconds) : std::string("invalid");
                if (valid != sample.valid || (valid && seconds != sample.seconds)) ok = false;
            }
            r.Custom(36, "Retry-After parsing", "delta-seconds only, HTTP-date and junk rejected", ok, got);
        }
        {
            const RetryPlan auth = dshb::api::NextRetry(Status::Unauthorized, 3);
            const RetryPlan forbidden = dshb::api::NextRetry(Status::Forbidden, 3);
            const RetryPlan nokey = dshb::api::NextRetry(Status::NoKey, 3);
            const RetryPlan server = dshb::api::NextRetry(Status::ServerError, 1);
            const bool ok = !auth.retry && !forbidden.retry && !nokey.retry && server.retry &&
                            server.delaySeconds == 20;
            r.Custom(37, "no retry on 401/403/NoKey", "retry=false, false, false; 5xx retries like a network failure",
                     ok, RetryText(Status::Unauthorized, 3, -1) + " / " + RetryText(Status::Forbidden, 3, -1) + " / " +
                             RetryText(Status::NoKey, 3, -1) + " / " + RetryText(Status::ServerError, 1, -1));
        }

        // --- key handling (steps J1, J6) ---------------------------------------
        {
            const bool ok = !dshb::api::IsUsableApiKey(L"") && !dshb::api::IsUsableApiKey(L"sk-a\r\nX: y") &&
                            !dshb::api::IsUsableApiKey(L"with space") && dshb::api::IsUsableApiKey(L"sk-abc123");
            r.Custom(38, "key usability", "empty/control/space rejected, normal key accepted", ok,
                     "empty=false crlf=false space=false normal=true");
        }
        {
            const BalanceResult result = dshb::api::FetchBalance(L"");
            const bool ok = result.status == Status::NoKey && result.httpStatus == 0;
            r.Custom(39, "no key -> NoKey, no request", "status=NoKey http=0 (design 10.4 / steps J1)", ok,
                     Summary(result));
        }
        {
            // J6: the log line must be reconstructable and must never contain the key.
            const std::string line = dshb::api::LogLine(dshb::api::ParseBalanceBody(kBodySingleCny));
            const bool ok = line.find("CNY") != std::string::npos && line.find("110.00") != std::string::npos &&
                            line.find("sk-") == std::string::npos;
            r.Custom(40, "log line (J6)", "contains status/currency/amount, contains no key material", ok, line);
        }

        // --- robustness: "no case may crash or throw" --------------------------
        {
            // Deterministic byte-flip sweep over the recorded bodies. Every result must come
            // back without throwing, and an Ok result must still carry a usable amount --
            // that is the invariant that keeps a wrong number off the widget.
            const char* bodies[] = {kBodySingleCny, kBodyCnyUsd, kBodyEmptyArray, kBodyMissingTotal,
                                    kBodyDecimals, kBodyEscapedCurrency};
            const char replacements[] = {'\0', '"', '{', '}', '9', '\\', 'e'};
            long long mutations = 0;
            long long okResults = 0;
            bool invariantHeld = true;
            bool threw = false;
            for (const char* raw : bodies) {
                const std::string base = raw;
                for (std::size_t i = 0; i < base.size(); ++i) {
                    for (const char replacement : replacements) {
                        std::string mutated = base;
                        mutated[i] = replacement;
                        ++mutations;
                        try {
                            const BalanceResult result = dshb::api::ParseBalanceBody(mutated);
                            if (result.status == Status::Ok) {
                                ++okResults;
                                bool usable = false;
                                for (const BalanceEntry& entry : result.entries) {
                                    if (entry.hasTotal && (entry.currency == "CNY" || entry.currency == "USD")) {
                                        usable = true;
                                    }
                                }
                                if (!usable) invariantHeld = false;
                            }
                        } catch (...) {
                            threw = true;
                        }
                    }
                }
            }
            r.Custom(41, "byte-flip fuzz", "no throw; Ok always carries a usable amount",
                     invariantHeld && !threw && mutations > 1000,
                     std::to_string(mutations) + " mutations, " + std::to_string(okResults) +
                         " still Ok, invariant=" + (invariantHeld ? "held" : "BROKEN") +
                         " threw=" + (threw ? "yes" : "no"));
        }
        {
            // Every prefix of every recorded body, plus truncation at every position of a
            // body that is being cut mid-string: never Ok, never a throw.
            const char* bodies[] = {kBodySingleCny, kBodyCnyUsd, kBodyMissingTotal, kBodyEscapedCurrency};
            long long prefixes = 0;
            bool anyOk = false;
            bool threw = false;
            for (const char* raw : bodies) {
                const std::string base = raw;
                for (std::size_t length = 0; length < base.size(); ++length) {
                    ++prefixes;
                    try {
                        const BalanceResult result = dshb::api::ParseBalanceBody(base.substr(0, length));
                        if (result.status == Status::Ok) anyOk = true;
                    } catch (...) {
                        threw = true;
                    }
                }
            }
            r.Custom(42, "truncation sweep", "no prefix of a valid body is ever accepted, no throw",
                     !anyOk && !threw && prefixes > 400,
                     std::to_string(prefixes) + " prefixes, accepted=" + (anyOk ? "yes" : "no") +
                         " threw=" + (threw ? "yes" : "no"));
        }
    } catch (const std::exception& e) {
        std::printf("case runner threw: %s\n", e.what());
        return 1;
    } catch (...) {
        std::printf("case runner threw an unknown exception\n");
        return 1;
    }

    std::printf("----------------------------------------------------------------------------------------------------------\n");
    std::printf("cases: %d passed, %d failed\n", r.passed, r.failed);
    for (const std::string& failure : r.failures) {
        std::printf("FAILED: %s\n", failure.c_str());
    }
    std::printf("RESULT: %s\n", r.failed == 0 ? "PASS" : "FAIL");
    return r.failed == 0 ? 0 : 1;
}

std::wstring EnvKey() {
#ifdef _WIN32
    wchar_t buffer[1024] = {};
    const DWORD size = GetEnvironmentVariableW(L"DEEPSEEK_API_KEY", buffer, 1024);
    if (size == 0 || size >= 1024) return std::wstring();
    return std::wstring(buffer, size);
#else
    return std::wstring();
#endif
}

int RunLive(const Endpoint& endpoint, bool diagnose) {
    const std::wstring key = EnvKey();
    const BalanceResult result = dshb::api::FetchBalance(key, endpoint);
    // Four fields, nothing else. No key, no fragment of a key, not even on failure.
    std::printf("live: status=%s http=%d entries=%d currencies=%s", dshb::api::StatusName(result.status),
                result.httpStatus, static_cast<int>(result.entries.size()), Currencies(result).c_str());
    if (diagnose) {
        std::printf(" | retryAfter=%d detail=\"%s\"", result.retryAfterSeconds, result.detail.c_str());
    }
    std::printf("\n");
    return result.status == Status::Ok ? 0 : 2;
}

void Usage() {
    std::printf("apiprobe -- offline proof for the balance API core\n");
    std::printf("  --cases                    run the recorded cases (default); no network\n");
    std::printf("  --live                     one real request with $DEEPSEEK_API_KEY\n");
    std::printf("  --host=HOST                endpoint host (default api.deepseek.com)\n");
    std::printf("  --port=N                   endpoint port (default 443)\n");
    std::printf("  --path=PATH                endpoint path (default /user/balance)\n");
    std::printf("  --plain-http               use http instead of https (local test servers)\n");
    std::printf("  --timeout-ms=N             request timeout (default 5000)\n");
    std::printf("  --diagnose                 append retryAfter and detail to the --live line\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Endpoint endpoint;
    bool live = false;
    bool diagnose = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cases") {
            live = false;
        } else if (arg == L"--live") {
            live = true;
        } else if (arg == L"--plain-http") {
            endpoint.secure = false;
        } else if (arg == L"--diagnose") {
            diagnose = true;
        } else if (arg.rfind(L"--host=", 0) == 0) {
            endpoint.host = arg.substr(7);
        } else if (arg.rfind(L"--path=", 0) == 0) {
            endpoint.path = arg.substr(7);
        } else if (arg.rfind(L"--port=", 0) == 0) {
            endpoint.port = static_cast<unsigned short>(std::wcstol(arg.c_str() + 7, nullptr, 10));
        } else if (arg.rfind(L"--timeout-ms=", 0) == 0) {
            endpoint.timeoutMs = static_cast<int>(std::wcstol(arg.c_str() + 13, nullptr, 10));
        } else if (arg == L"--help" || arg == L"-h") {
            Usage();
            return 0;
        } else {
            std::printf("apiprobe: unknown argument\n");
            Usage();
            return 64;
        }
    }
    return live ? RunLive(endpoint, diagnose) : RunCases();
}
