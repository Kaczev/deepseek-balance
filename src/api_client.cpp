// api_client.cpp -- see api_client.h for the contract and the design references.
//
// Split in two: pure classification/parsing above, WinHTTP plumbing below. The plumbing
// returns nothing but a status code and a body, so every decision that can be wrong is
// reachable from the offline case runner without a socket.
#include "api_client.h"

#include "http_get.h"
#include "json_min.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace dshb::api {

const char* StatusName(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::NoKey: return "NoKey";
    case Status::Unauthorized: return "Unauthorized";
    case Status::Forbidden: return "Forbidden";
    case Status::PaymentRequired: return "PaymentRequired";
    case Status::RateLimited: return "RateLimited";
    case Status::ServerError: return "ServerError";
    case Status::TransportError: return "TransportError";
    case Status::ParseError: return "ParseError";
    case Status::NoEntries: return "NoEntries";
    }
    return "Unknown";
}

Status ClassifyTransport(bool transportFailed, int httpStatus) {
    if (transportFailed) return Status::TransportError;
    if (httpStatus == 0) return Status::TransportError; // defensive: no response to classify
    if (httpStatus == 401) return Status::Unauthorized;
    if (httpStatus == 402) return Status::PaymentRequired;
    if (httpStatus == 403) return Status::Forbidden;
    if (httpStatus == 429) return Status::RateLimited;
    if (httpStatus >= 200 && httpStatus <= 299) return Status::Ok;
    // design 4.4: 500/503 are handled like a network failure, and 400/422 are our own bug,
    // so they take the same path. Any other code is not in the official table either; it is
    // logged as received (httpStatus) and given the retryable path.
    return Status::ServerError;
}

bool IsDecimalAmount(const std::string& text) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    const std::size_t digitsStart = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == digitsStart) return false; // no integer part
    if (i < text.size() && text[i] == '.') {
        ++i;
        const std::size_t fracStart = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == fracStart) return false; // "1." is not an amount
    }
    return i == text.size();
}

bool IsUsableApiKey(const std::wstring& apiKey) {
    if (apiKey.empty()) return false;
    for (const wchar_t c : apiKey) {
        const unsigned int code = static_cast<unsigned int>(c);
        if (code < 0x21 || code == 0x7F) return false; // space or control character
    }
    return true;
}

bool ParseRetryAfterSeconds(const std::string& headerValue, int* outSeconds) {
    std::size_t begin = 0;
    std::size_t end = headerValue.size();
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && isSpace(headerValue[begin])) ++begin;
    while (end > begin && isSpace(headerValue[end - 1])) --end;
    if (begin == end) return false;
    long long value = 0;
    constexpr long long kCap = 86400; // a day is already absurd; clamp instead of overflowing
    for (std::size_t i = begin; i < end; ++i) {
        const char c = headerValue[i];
        if (c < '0' || c > '9') return false; // HTTP-date form, or junk: design 4.4 default applies
        if (value > kCap) continue;           // already past the cap: keep validating the digits
        value = value * 10 + (c - '0');
    }
    if (value > kCap) value = kCap;
    if (outSeconds) *outSeconds = static_cast<int>(value);
    return true;
}

RetryPlan NextRetry(Status status, int attemptIndex, int retryAfterSeconds) {
    RetryPlan plan;
    const int attempt = attemptIndex < 0 ? 0 : attemptIndex;
    auto networkBackoff = [attempt]() {
        const int shift = std::min(attempt, 4);
        return 10 << shift; // 10, 20, 40, 80, capped at 160 (design 4.4)
    };
    switch (status) {
    case Status::Ok:
        plan.retry = false;
        break;
    case Status::NoKey:
    case Status::Unauthorized:
    case Status::Forbidden:
    case Status::PaymentRequired:
        // design 4.4: do not retry; the user has to change something first.
        plan.retry = false;
        break;
    case Status::RateLimited:
        plan.retry = true;
        plan.delaySeconds = retryAfterSeconds >= 0 ? retryAfterSeconds : 60;
        break;
    case Status::TransportError:
    case Status::ServerError:
    case Status::ParseError:
        plan.retry = true;
        plan.delaySeconds = networkBackoff();
        break;
    case Status::NoEntries:
        // The request succeeded, so this is not a failure to back off from: keep sampling
        // on the normal cadence (design 4.1).
        plan.retry = true;
        plan.delaySeconds = 10;
        break;
    }
    return plan;
}

namespace {

bool ReadAmountField(const json::Value& item, const char* field, const std::string& where,
                     std::string* outValue, bool* outHas, std::string* error) {
    const json::Value* value = item.Find(field);
    if (!value) {
        // Absent is reported as absent. It is never turned into a number (design 2.2, 3.2).
        outValue->clear();
        *outHas = false;
        return true;
    }
    if (!value->IsString()) {
        *error = where + "." + field + " is present but not a string";
        return false;
    }
    if (!IsDecimalAmount(value->text)) {
        *error = where + "." + field + " is not a decimal amount: \"" + value->text + "\"";
        return false;
    }
    *outValue = value->text;
    *outHas = true;
    return true;
}

bool IsKnownCurrency(const std::string& currency) { return currency == "CNY" || currency == "USD"; }

} // namespace

BalanceResult ParseBalanceBody(const std::string& body) {
    BalanceResult result;
    if (body.empty()) {
        result.status = Status::ParseError;
        result.detail = "empty response body";
        return result;
    }
    const json::Outcome parsed = json::Parse(body);
    if (!parsed.ok) {
        result.status = Status::ParseError;
        result.detail = "body is not usable JSON: " + parsed.error;
        return result;
    }
    const json::Value& root = parsed.root;
    if (!root.IsObject()) {
        result.status = Status::ParseError;
        result.detail = "top-level JSON value is not an object";
        return result;
    }
    const json::Value* available = root.Find("is_available");
    if (!available) {
        result.status = Status::ParseError;
        result.detail = "is_available is missing (design 2.1: it is not the same thing as a 0 balance)";
        return result;
    }
    if (!available->IsBool()) {
        result.status = Status::ParseError;
        result.detail = "is_available is not a boolean";
        return result;
    }
    result.isAvailable = available->boolean;
    result.hasIsAvailable = true;

    const json::Value* infos = root.Find("balance_infos");
    if (!infos) {
        result.status = Status::ParseError;
        result.detail = "balance_infos is missing";
        return result;
    }
    if (!infos->IsArray()) {
        result.status = Status::ParseError;
        result.detail = "balance_infos is not an array";
        return result;
    }
    if (infos->items.empty()) {
        // design 2.2 rule 3: an empty array is "cannot tell", not "the balance is 0".
        result.status = Status::NoEntries;
        result.detail = "balance_infos is an empty array (design 2.2 rule 3: error state, never 0.00)";
        return result;
    }

    for (std::size_t i = 0; i < infos->items.size(); ++i) {
        const json::Value& item = infos->items[i];
        const std::string where = "balance_infos[" + std::to_string(i) + "]";
        if (!item.IsObject()) {
            result.status = Status::ParseError;
            result.detail = where + " is not an object";
            return result;
        }
        BalanceEntry entry;
        const json::Value* currency = item.Find("currency");
        if (!currency || !currency->IsString() || currency->text.empty()) {
            result.status = Status::ParseError;
            result.detail = where + ".currency is missing or not a non-empty string";
            return result;
        }
        entry.currency = currency->text;

        std::string error;
        if (!ReadAmountField(item, "total_balance", where, &entry.totalBalance, &entry.hasTotal, &error) ||
            !ReadAmountField(item, "granted_balance", where, &entry.grantedBalance, &entry.hasGranted, &error) ||
            !ReadAmountField(item, "topped_up_balance", where, &entry.toppedUpBalance, &entry.hasToppedUp, &error)) {
            result.status = Status::ParseError;
            result.detail = error;
            return result;
        }
        result.entries.push_back(std::move(entry));
    }

    // design 2.2: selection must not depend on the array order, and an entry whose amount we
    // could not read must never be displayed as 0.00.
    bool anyDisplayable = false;
    for (const BalanceEntry& entry : result.entries) {
        if (IsKnownCurrency(entry.currency) && entry.hasTotal) anyDisplayable = true;
    }
    if (!anyDisplayable) {
        result.status = Status::ParseError;
        result.detail = "no CNY or USD entry carries a usable total_balance (design 2.2 rule 3)";
        return result;
    }

    result.status = Status::Ok;
    std::string skipped;
    for (const BalanceEntry& entry : result.entries) {
        if (IsKnownCurrency(entry.currency) && entry.hasTotal) continue;
        if (!skipped.empty()) skipped += ", ";
        skipped += entry.currency;
        skipped += entry.hasTotal ? " (unknown currency)" : " (no total_balance)";
    }
    if (!skipped.empty()) {
        result.detail = "kept but not displayable: " + skipped;
    }
    return result;
}

int PreferredEntryIndex(const std::vector<BalanceEntry>& entries) {
    // design 2.2 rule 2: CNY wins when both are present. Rule 1 covers the single-entry case.
    // Extension, because a wrong number is worse than no number: an entry without a readable
    // total_balance cannot be the preferred one.
    int usdIndex = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const BalanceEntry& entry = entries[i];
        if (!entry.hasTotal) continue;
        if (entry.currency == "CNY") return static_cast<int>(i);
        if (entry.currency == "USD" && usdIndex < 0) usdIndex = static_cast<int>(i);
    }
    return usdIndex;
}

std::string LogLine(const BalanceResult& result) {
    // design 10.5 / steps J6: enough to reconstruct why a sample failed, and never the key.
    std::string line;
    line += "status=";
    line += StatusName(result.status);
    line += " http=";
    line += std::to_string(result.httpStatus);
    line += " available=";
    line += result.hasIsAvailable ? (result.isAvailable ? "true" : "false") : "absent";
    line += " entries=";
    line += std::to_string(result.entries.size());
    line += " retryAfter=";
    line += std::to_string(result.retryAfterSeconds);
    for (const BalanceEntry& entry : result.entries) {
        line += " | ";
        line += entry.currency.empty() ? "?" : entry.currency;
        line += " total=";
        line += entry.hasTotal ? entry.totalBalance : "(absent)";
        line += " granted=";
        line += entry.hasGranted ? entry.grantedBalance : "(absent)";
        line += " topped_up=";
        line += entry.hasToppedUp ? entry.toppedUpBalance : "(absent)";
    }
    if (!result.detail.empty()) {
        line += " detail=\"";
        line += result.detail;
        line += "\"";
    }
    return line;
}

// --- live fetch --------------------------------------------------------------
//
// ★ 2026-09-19: the WinHTTP plumbing that used to live here moved to src/http_get.cpp,
//   because a second host (the FX rate endpoint) needed the same thirty lines. What stays
//   here is everything that is a *decision*: the endpoint check, the key check, and the
//   routing of a status code + body through ClassifyTransport / ParseBalanceBody. Every
//   message and every status this function produced before the move it still produces;
//   the ["api t=..."] log lines of an existing run are unchanged.

namespace {

BalanceResult TransportFailure(const char* what, unsigned long code) {
    BalanceResult result;
    result.status = Status::TransportError;
    result.detail = std::string(what) + " failed, error=" + std::to_string(code);
    return result;
}

} // namespace

BalanceResult FetchBalance(const std::wstring& apiKey, const Endpoint& endpoint) {
    BalanceResult result;
    if (!IsUsableApiKey(apiKey)) {
        result.status = Status::NoKey;
        result.detail = apiKey.empty() ? "DEEPSEEK_API_KEY is not set"
                                       : "the api key contains a space or a control character";
        return result;
    }
    if (endpoint.host.empty() || endpoint.path.empty() || endpoint.timeoutMs <= 0) {
        result.status = Status::TransportError;
        result.detail = "endpoint is not usable (empty host/path, or a non-positive timeout)";
        return result;
    }

    http::Request request;
    request.host = endpoint.host;
    request.port = endpoint.port;
    request.secure = endpoint.secure;
    request.path = endpoint.path;
    request.timeoutMs = endpoint.timeoutMs;
    request.authorization = L"Bearer " + apiKey;

    const http::Response response = http::Get(request);

    // The status code travels on every failure path that had one, exactly as before: the
    // sampling loop's backoff and the log both read it.
    if (!response.transportOk) {
        BalanceResult failure;
        failure.status = Status::TransportError;
        failure.httpStatus = response.httpStatus;
        failure.retryAfterSeconds = response.retryAfterSeconds;
        failure.detail = response.error;
        return failure;
    }

    // A body that is not usable as a body: a transfer cut short by the server's own
    // Content-Length, or one past the size cap. The HTTP code did arrive, so it is kept.
    // The two map to different statuses here on purpose: "the answer never finished
    // arriving" is a transport problem, while "the answer was told to be enormous" is a
    // problem with the body we are looking at (design 4.4 leaves both to this layer).
    if (!response.error.empty()) {
        BalanceResult failure;
        failure.status = response.bodyTooLarge ? Status::ParseError : Status::TransportError;
        failure.httpStatus = response.httpStatus;
        failure.retryAfterSeconds = response.retryAfterSeconds;
        failure.detail = response.error;
        return failure;
    }

    const Status classified = ClassifyTransport(false, response.httpStatus);
    if (classified != Status::Ok) {
        result.status = classified;
        result.httpStatus = response.httpStatus;
        result.retryAfterSeconds = response.retryAfterSeconds;
        result.detail = "HTTP " + std::to_string(response.httpStatus);
        return result;
    }

    result = ParseBalanceBody(response.body);
    result.httpStatus = response.httpStatus;
    result.retryAfterSeconds = response.retryAfterSeconds;
    return result;
}

BalanceResult FetchBalance(const std::wstring& apiKey) {
    return FetchBalance(apiKey, Endpoint{});
}

} // namespace dshb::api
