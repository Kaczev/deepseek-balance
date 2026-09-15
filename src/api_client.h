// api_client.h -- DeepSeek balance endpoint: transport, response parsing, error routing.
//
// Plan of record (see the two documents under the not-committed folder):
//   * design 2.1 -- GET https://api.deepseek.com/user/balance with
//     "Authorization: Bearer $DEEPSEEK_API_KEY"; is_available is a boolean and is NOT the
//     same thing as "balance is 0"; balance_infos is an array (may be empty, may hold CNY
//     and USD); the three amounts are decimal strings.
//   * design 2.2 -- selection must not depend on the array order.
//   * design 3.1 -- amounts stay decimal strings, never binary floating point.
//   * design 4.4 -- error routing: 402 (out of money) must never be shown as 401 (bad key);
//     401/403/402 are not retried; 5xx is handled like a network failure; 429 honours
//     Retry-After and otherwise backs off from 60 s; network failures back off
//     10 -> 20 -> 40 -> 80 -> capped at 160 s.
//   * design 4.5 -- this file only decides *whether* and *how long* to wait. Sleeping,
//     locking, suspend and network-loss pausing belong to the sampling loop.
//   * design 10.5 / steps J6 -- the API key never reaches a log or any output, not even a
//     fragment. Nothing in this file formats or stores the key; `detail` and LogLine() are
//     built only from status codes, error codes and parsed fields.
//
// The testability seam: ParseBalanceBody(), ClassifyTransport(), NextRetry(),
// ParseRetryAfterSeconds(), IsDecimalAmount(), PreferredEntryIndex() and LogLine() are pure
// functions -- no network, no globals, deterministic. FetchBalance() is a thin WinHTTP
// wrapper that only obtains one status code plus one body and then hands both to those
// functions.
#pragma once

#include <string>
#include <vector>

namespace dshb::api {

enum class Status {
    Ok,              // 2xx and a body we understood
    NoKey,           // DEEPSEEK_API_KEY missing/empty -> steps J1, design 10.4
    Unauthorized,    // HTTP 401 -- bad key; never retried (design 4.4)
    Forbidden,       // HTTP 403 -- not in the official code table; logged as-is (design 4.4)
    PaymentRequired, // HTTP 402 -- out of money; never merged into 401 (design 4.4)
    RateLimited,     // HTTP 429 -- honour Retry-After, else back off from 60 s (design 4.4)
    ServerError,     // HTTP 5xx, plus 400/422 which design 4.4 says to treat like a 500
    TransportError,  // no HTTP response at all: DNS, connect, TLS, timeout, truncated read
    ParseError,      // we got a body but could not trust its contents (design 2.2, 3.2)
    NoEntries        // 200 with an empty balance_infos array (design 2.2 rule 3)
};

const char* StatusName(Status status);

struct BalanceEntry {
    std::string currency;         // verbatim from the response, e.g. "CNY" / "USD"
    std::string totalBalance;     // verbatim decimal string (design 3.1: no binary float)
    std::string grantedBalance;   // log only (design 2.3)
    std::string toppedUpBalance;  // log only (design 2.3)
    bool hasTotal = false;
    bool hasGranted = false;
    bool hasToppedUp = false;
};

struct BalanceResult {
    Status status = Status::ParseError;
    int httpStatus = 0;             // 0 when no HTTP response was obtained
    bool isAvailable = false;
    bool hasIsAvailable = false;    // false means the field was absent -- not "unavailable"
    std::vector<BalanceEntry> entries;
    int retryAfterSeconds = -1;     // from the Retry-After header; -1 when absent
    std::string detail;             // short, log only, never contains the key

    bool Ok() const { return status == Status::Ok; }
};

// --- pure functions: no network, no state -----------------------------------

// Maps "did the transport fail, and what status code came back" to the status enum.
// transportFailed wins over httpStatus (a failure means there was no response to read).
// httpStatus == 0 without transportFailed is also a transport error, defensively.
Status ClassifyTransport(bool transportFailed, int httpStatus);

// Parses a response BODY. Pure: the same bytes always give the same result.
//   * empty body, truncated body, non-JSON body      -> ParseError
//   * balance_infos missing / not an array / empty   -> ParseError / NoEntries
//   * is_available missing or not a boolean          -> ParseError (design 2.1: this flag
//     must not be silently defaulted, because it is not the same thing as a 0 balance)
//   * an entry without total_balance                 -> that entry keeps hasTotal == false;
//     if no CNY/USD entry has a usable total, the result is ParseError. It is never
//     reported as 0.00 (design 2.2 rule 3, 3.2).
//   * unknown currency                               -> entry kept verbatim; if neither CNY
//     nor USD is present the result is ParseError (design 2.2 rule 3)
BalanceResult ParseBalanceBody(const std::string& body);

// Decimal shape check only: optional sign, digits, optional single fraction. No conversion.
bool IsDecimalAmount(const std::string& text);

// A key we are willing to put on the wire: non-empty, no space and no control character.
// This is also what keeps a key from being able to inject a second HTTP header.
bool IsUsableApiKey(const std::wstring& apiKey);

// design 2.2 rules 1-2 with one addition: prefer CNY, then USD, and only accept an entry
// that actually carries total_balance. Returns -1 when nothing is displayable.
int PreferredEntryIndex(const std::vector<BalanceEntry>& entries);

// Retry-After in delta-seconds form. Returns false for the HTTP-date form and for junk;
// design 4.4 then falls back to its 60 s default.
bool ParseRetryAfterSeconds(const std::string& headerValue, int* outSeconds);

struct RetryPlan {
    bool retry = false;
    int delaySeconds = 0;
};

// design 4.4 routing. attemptIndex counts consecutive failures already seen (0 = first).
// retryAfterSeconds comes from the response header, -1 when absent.
//   TransportError / ServerError -> 10, 20, 40, 80, capped at 160
//   RateLimited                  -> Retry-After, else 60
//   NoKey / Unauthorized / Forbidden / PaymentRequired -> no retry, the user must act
//   NoEntries                    -> retry on the normal 10 s cadence (the request itself
//                                   succeeded, so escalating the backoff would be wrong)
//   ParseError                   -> treated like a transport failure. Design 4.4 does not
//                                   name this case; the next sample may well be fine.
RetryPlan NextRetry(Status status, int attemptIndex, int retryAfterSeconds = -1);

// One line for the rolling log (design 10.5, design 2.4): status, HTTP code, availability,
// currency and the three amounts as received. Never contains the key.
std::string LogLine(const BalanceResult& result);

// --- live fetch: a thin wrapper over the pure functions ----------------------

struct Endpoint {
    std::wstring host = L"api.deepseek.com";
    unsigned short port = 443;
    bool secure = true;             // false only for a local plain-HTTP test server
    std::wstring path = L"/user/balance";
    int timeoutMs = 5000;           // design 4.1 samples every 10 s; a request must fit inside
};

// Steps J3/J4 must be exercisable without waiting for a real 402/429/transport failure,
// so the endpoint (host, port, scheme, path, timeout) is a parameter.
BalanceResult FetchBalance(const std::wstring& apiKey, const Endpoint& endpoint);
BalanceResult FetchBalance(const std::wstring& apiKey);

} // namespace dshb::api
