// http_get.h -- "one HTTPS GET, give me the status code and the body".
//
// ★ Why this file exists (2026-09-19): the project now talks to TWO hosts -- the balance
//   endpoint (api.deepseek.com) and the FX rate endpoint (api.frankfurter.app). Both need
//   exactly the same thirty lines of WinHTTP: session, timeouts, redirect policy, connect,
//   send, read with a size cap, and the "the server promised more bytes than it sent"
//   check. Copying those thirty lines into a second file would have produced two
//   transports that drift -- and this project has already paid for "the same thing
//   written twice" once (see widget_display.cpp's note about the three store scans).
//
// ★ What is deliberately NOT here: every decision that can be *wrong*. Whether a 402 means
//   "out of money", whether a body is a usable balance, whether a rate is plausible --
//   none of that is transport. api_client.cpp keeps its own classification
//   (ClassifyTransport), fx_rate.cpp keeps its own parsing, and both are reachable from an
//   offline case runner without a socket.
//
// ★ Redirects are pinned OFF, not followed. For the balance call that is a security rule
//   (the Authorization header must never be carried to another host); it stays the rule
//   here rather than becoming a per-caller option, because a caller that wants redirects
//   followed is a caller that has not thought about whose host it just trusted.
#pragma once

#include <string>

namespace dshb::http {

struct Request {
    std::wstring host;             // e.g. L"api.deepseek.com"
    unsigned short port = 443;
    bool secure = true;            // false only for a local plain-HTTP test server
    std::wstring path;             // e.g. L"/latest?from=USD&to=CNY"
    int timeoutMs = 5000;          // applied to resolve, connect, send and receive
    // Empty means "send no such header". Passing a request header is a data decision, so
    // whoever holds the secret decides (api_client builds its Bearer header, and wipes it).
    std::wstring authorization;    // full value, e.g. L"Bearer sk-..." ; never logged
    std::wstring userAgent;        // empty -> a version string of this project
};

struct Response {
    // false = there was no HTTP response at all (DNS, connect, TLS, timeout, truncated
    // read). `httpStatus` is then 0 and must not be read as a code.
    bool transportOk = false;
    int httpStatus = 0;
    std::string body;              // verbatim bytes as received, never truncated silently
    // Retry-After in delta-seconds form; -1 when the header is absent or unusable (the
    // HTTP-date form included -- see api_client's ParseRetryAfterSeconds, which is what
    // turns this into a backoff decision). Reading it is transport work; interpreting it
    // against a status code is not.
    int retryAfterSeconds = -1;
    // Non-empty means "this answer is not usable as an answer" -- a transport failure, a
    // transfer cut short, or a body past the size cap. `transportOk` stays true for the
    // latter two, because a status code and a partial body WERE received; the caller
    // decides which of its own statuses that maps to.
    std::string error;             // short, log-only, never contains a credential
    // True only for the size cap. A caller may want to call that a parse problem rather
    // than a transport one (api_client does) -- which is a decision about its own status
    // enum, so the transport reports the fact and not the verdict.
    bool bodyTooLarge = false;
};

// Performs exactly one GET. Bounded: the body is capped at 1 MiB, and a transfer that
// ends before the declared Content-Length is reported as a transport failure instead of
// being handed on as a short body.
Response Get(const Request& request);

}  // namespace dshb::http
