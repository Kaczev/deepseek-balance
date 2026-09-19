// http_get.cpp -- see http_get.h for why this is one shared transport and not two.
//
// Lifted verbatim in behaviour from api_client.cpp's FetchBalance (the WinHTTP half only).
// Every message this file produces is the message that file produced, so the [api t=...]
// log lines of an existing run stay byte-identical apart from which call site they name.
#include "http_get.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <cwchar>
#include <string>
#include <utility>

namespace dshb::http {

namespace {

// A balance answer is a few hundred bytes and a rate answer is under a hundred; a
// megabyte is the point at which the answer is not an answer any more.
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

struct InternetHandle {
    HINTERNET handle = nullptr;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET h) : handle(h) {}
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    explicit operator bool() const { return handle != nullptr; }
};

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

Response Failure(const char* what, unsigned long code) {
    Response result;
    result.transportOk = false;
    result.error = std::string(what) + " failed, error=" + std::to_string(code);
    return result;
}

}  // namespace

Response Get(const Request& request) {
    if (request.host.empty() || request.path.empty() || request.timeoutMs <= 0) {
        Response bad;
        bad.error = "endpoint is not usable (empty host/path, or a non-positive timeout)";
        return bad;
    }

    const std::wstring agent = request.userAgent.empty() ? L"deepseek-balance/0.2.1"
                                                         : request.userAgent;
    InternetHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return Failure("WinHttpOpen", GetLastError());

    WinHttpSetTimeouts(session.handle, request.timeoutMs, request.timeoutMs, request.timeoutMs,
                       request.timeoutMs);

    // A request must go exactly where it was addressed. Never let a redirect carry the
    // request (and any header we put on it) somewhere else.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));

    InternetHandle connection(WinHttpConnect(session.handle, request.host.c_str(), request.port, 0));
    if (!connection) return Failure("WinHttpConnect", GetLastError());

    const DWORD flags = request.secure ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle handle(WinHttpOpenRequest(connection.handle, L"GET", request.path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             flags));
    if (!handle) return Failure("WinHttpOpenRequest", GetLastError());

    if (!request.authorization.empty()) {
        std::wstring header = L"Authorization: ";
        header += request.authorization;
        if (!WinHttpAddRequestHeaders(handle.handle, header.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            const unsigned long code = GetLastError();
            // Wipe before the string dies: this is the only copy outside the caller's.
            SecureZeroMemory(header.data(), header.size() * sizeof(wchar_t));
            return Failure("WinHttpAddRequestHeaders", code);
        }
        SecureZeroMemory(header.data(), header.size() * sizeof(wchar_t));
    }

    if (!WinHttpSendRequest(handle.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0)) {
        return Failure("WinHttpSendRequest", GetLastError());
    }
    if (!WinHttpReceiveResponse(handle.handle, nullptr)) {
        return Failure("WinHttpReceiveResponse", GetLastError());
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    const bool haveStatus =
        WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                            WINHTTP_NO_HEADER_INDEX) != FALSE;

    // A cut-off transfer looks like a clean end-of-body to WinHTTP, so remember what the
    // server promised. Without this, "the connection died mid-body" is indistinguishable
    // from "the server sent malformed JSON" in the log (design 10.5 / steps J6).
    // The same buffer also carries Retry-After, which is a wire-level quantity: seconds
    // as the server wrote them, or -1 when the header is absent or is the HTTP-date form.
    // What a particular status code then does with it is the caller's business
    // (api_client's NextRetry), and that decision stays testable without a socket.
    int retryAfter = -1;
    {
        wchar_t buffer[256] = {};
        DWORD size = sizeof(buffer);
        if (WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_RETRY_AFTER,
                                WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            // Delta-seconds only; the date form deliberately falls through to -1, exactly
            // as ParseRetryAfterSeconds does for it (design 4.4 then uses its 60 s default).
            wchar_t* end = nullptr;
            const long parsed = std::wcstol(buffer, &end, 10);
            if (end != buffer && parsed >= 0 && parsed <= 86400) retryAfter = static_cast<int>(parsed);
        }
    }

    long long declaredLength = -1;
    {
        wchar_t buffer[64] = {};
        DWORD size = sizeof(buffer);
        if (WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            wchar_t* end = nullptr;
            const long long parsed = std::wcstoll(buffer, &end, 10);
            if (end != buffer && parsed >= 0) declaredLength = parsed;
        }
    }

    std::string body;
    bool readFailed = false;
    bool tooLarge = false;
    unsigned long readError = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.handle, &available)) {
            readFailed = true;
            readError = GetLastError();
            break;
        }
        if (available == 0) break;
        if (body.size() + available > kMaxBodyBytes) {
            tooLarge = true;
            break;
        }
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle.handle, body.data() + offset, available, &read)) {
            readFailed = true;
            readError = GetLastError();
            body.resize(offset);
            break;
        }
        body.resize(offset + read);
        if (read == 0) break;
    }

    if (!haveStatus) return Failure("WinHttpQueryHeaders(status code)", GetLastError());

    if (!readFailed && declaredLength >= 0 && static_cast<long long>(body.size()) < declaredLength) {
        Response cut;
        cut.httpStatus = static_cast<int>(statusCode);
        cut.retryAfterSeconds = retryAfter;
        cut.error = "body cut short: got " + std::to_string(body.size()) + " of " +
                    std::to_string(declaredLength) + " declared bytes";
        return cut;
    }

    if (readFailed) {
        Response failed = Failure("WinHttpReadData", readError);
        failed.httpStatus = static_cast<int>(statusCode);
        failed.retryAfterSeconds = retryAfter;
        return failed;
    }
    if (tooLarge) {
        Response large;
        large.httpStatus = static_cast<int>(statusCode);
        large.retryAfterSeconds = retryAfter;
        large.bodyTooLarge = true;
        large.error = "response body is larger than " + std::to_string(kMaxBodyBytes) + " bytes";
        return large;
    }

    Response ok;
    ok.transportOk = true;
    ok.httpStatus = static_cast<int>(statusCode);
    ok.retryAfterSeconds = retryAfter;
    ok.body = std::move(body);
    return ok;
}

}  // namespace dshb::http
