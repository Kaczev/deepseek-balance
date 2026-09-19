// fx_rate.cpp -- see fx_rate.h for the contract and for why the rate is injected as a real
// currency entry instead of being applied in the display layer.
#include "fx_rate.h"

#include "amount.h"
#include "http_get.h"
#include "json_min.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>

namespace dshb::fx {

namespace {

// 定点标度：1/10000 CNY。和 amount.h 的 kUnitsPerYuan 同一个数量级，
// 但那一层是"元"，这里要的是"汇率的倒数再乘回去"，所以单独定一个。
constexpr int64_t kScale = 10000;

constexpr long long kSecondsPerDay = 86400;

// "6.6976" -> 66976。拒绝：空串、只有符号/小数点、任何非数字字符、超出 int64。
// ★ 不用 strtod：这里是"把一个十进制文本变成整数"，浮点只会引入一个不该有的舍入。
bool RateToScaled(const std::string& text, int64_t* out) {
    if (text.empty()) return false;
    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = (text[i] == '-');
        ++i;
    }
    int64_t whole = 0;
    int64_t frac = 0;
    int64_t scale = 1;
    bool anyDigit = false;
    bool seenDot = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (seenDot) return false;
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        anyDigit = true;
        if (!seenDot) {
            if (whole > (INT64_MAX - 9) / 10) return false;
            whole = whole * 10 + (c - '0');
        } else if (scale < kScale) {
            frac = frac * 10 + (c - '0');
            scale *= 10;
        }
        // 多余的小数位直接丢掉（ECB 给的是 4 位）
    }
    if (!anyDigit) return false;
    const int64_t value = whole * kScale + frac * (kScale / scale);
    *out = negative ? -value : value;
    return true;
}

// 1/10000 单位 -> "x.xx"（**截断**到分，绝不进位）。
// 与 amount.cpp 的 ToString2 是两件事：那个是"把已经存下来的值显示出来"，
// 这个是"把一次换算的结果落到两位"，方向（截断/进位）是这里自己定的，所以自己实现。
std::string ScaledToString2(int64_t raw) {
    const bool negative = raw < 0;
    const int64_t magnitude = negative ? -raw : raw;
    const int64_t cents = magnitude / 100;   // 截断：丢掉 1/10000 位
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%lld.%02lld", negative ? "-" : "",
                  static_cast<long long>(cents / 100), static_cast<long long>(cents % 100));
    return buf;
}

void SetError(Rate* rate, const std::string& why) {
    rate->ok = false;
    rate->rateText.clear();
    rate->date.clear();
    rate->error = why;
}

// ---- 缓存用的小工具 ---------------------------------------------------------

bool TryParseV(const std::string& text, long long* out) {
    if (text.empty()) return false;
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value < 0) return false;   // 溢出（不可能来自我们自己的写盘）
    }
    *out = value;
    return true;
}

std::string ReadAllBytes(const std::wstring& path, bool* ok) {
    *ok = false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return std::string();
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    *ok = true;
    return out;
}

// 只转义 JSON 字符串里必须转义的字符。缓存文件的成员都是我们自己写的
// （数字、日期、"YYYY-MM-DD HH:MM"），所以这里不是通用转义器，是"别写出非法 JSON"。
std::string EscapeJson(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// "2026-09-19 15:40"（本地时间）。人看的那一份；机器读的是 Unix 秒。
std::string FormatLocalMinutes(long long unixSeconds) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return std::string();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

}  // namespace

long long NowUnixSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    // 100 ns 单位；1601-01-01 到 1970-01-01 是 11644473600 秒。
    return static_cast<long long>(value.QuadPart / 10000000ULL) - 11644473600LL;
}

std::string Rate::ConvertAmountText(const std::string& cnyText) const {
    if (!ok) return std::string();
    Amount cny{};
    if (!ParseAmount(cnyText, &cny)) return std::string();
    int64_t scaled = 0;
    if (!RateToScaled(rateText, &scaled)) return std::string();
    if (scaled <= 0) return std::string();
    if (cny.raw == 0) return "0.00";
    // USD_raw = CNY_raw * kScale / rate_scaled，以 1/10000 USD 为单位。
    // 溢出保护：cny.raw 是 1/10000 元，现实中不会接近这个量级（这里是 1e15 元），
    // 但接口给的是字符串，所以还是挡一下，返回空串表示"算不出来"而不是给一个绕回来的数。
    const int64_t numerator = cny.raw <= INT64_MAX / kScale ? cny.raw * kScale : 0;
    if (numerator == 0) return std::string();
    return ScaledToString2(numerator / scaled);
}

std::string Rate::LogLine() const {
    if (!ok) {
        // 走到了这里就是**连缓存都没有**（ResolveRate 只有在三步都失败时才给 ok=false），
        // 所以这一行说的是"从没有过汇率"，不是"这一次没有"。
        std::string line = "[fx] 本会话没有可用汇率（从没有过汇率）：";
        line += error.empty() ? "原因未知" : error;
        line += "（不补 USD，切到 USD 仍显示 --.--）";
        return line;
    }
    if (cached) {
        // 所有者的措辞：本次失败的原因、用的是哪一天的值、那个值是什么时候存下来的。
        std::string line = "[fx] 本次取汇失败（" + fallbackNote + "），沿用 ";
        line += date.empty() ? std::string("（无牌价日期）") : date;
        line += " 的 " + rateText + "（存于 " + fetchedAt + "，已 ";
        line += std::to_string(ageDays) + " 天）";
        return line;
    }
    std::string line = "[fx] 1 USD = " + rateText + " CNY（frankfurter/ECB";
    if (!date.empty()) line += " " + date;
    line += "，本会话只取这一次）";
    return line;
}

Rate ParseRateBody(const std::string& body) {
    Rate rate;
    // 有些服务器会带 UTF-8 BOM，json_min 的第一个字节就必须是 '{'，所以先剥掉。
    // 这不是猜：BOM 是编码标记，不是正文的一部分。
    std::size_t begin = 0;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        begin = 3;
    }
    const json::Outcome parsed = json::Parse(body.substr(begin));
    if (!parsed.ok) {
        SetError(&rate, "响应不是合法 JSON：" + parsed.error);
        return rate;
    }
    if (!parsed.root.IsObject()) {
        SetError(&rate, "响应不是一个 JSON 对象");
        return rate;
    }
    const json::Value* base = parsed.root.Find("base");
    if (!base || !base->IsString()) {
        SetError(&rate, "响应里没有 base");
        return rate;
    }
    if (base->text != "USD") {
        SetError(&rate, "base 不是 USD（是 \"" + base->text + "\"）");
        return rate;
    }
    const json::Value* rates = parsed.root.Find("rates");
    if (!rates || !rates->IsObject()) {
        SetError(&rate, "响应里没有 rates 对象");
        return rate;
    }
    const json::Value* cny = rates->Find("CNY");
    if (!cny || !cny->IsNumber()) {
        SetError(&rate, "rates 里没有 CNY");
        return rate;
    }
    int64_t scaled = 0;
    if (!RateToScaled(cny->text, &scaled)) {
        SetError(&rate, "CNY 不是一个可用的数（\"" + cny->text + "\"）");
        return rate;
    }
    if (scaled <= 0) {
        SetError(&rate, "CNY 不是正数（\"" + cny->text + "\"）");
        return rate;
    }
    rate.ok = true;
    rate.rateText = cny->text;
    if (const json::Value* date = parsed.root.Find("date"); date && date->IsString()) {
        rate.date = date->text;
    }
    return rate;
}

Rate FetchRate(const RateEndpoint& endpoint) {
    http::Request request;
    request.host = endpoint.host;
    request.port = endpoint.port;
    request.secure = endpoint.secure;
    request.path = endpoint.path;
    request.timeoutMs = endpoint.timeoutMs;

    const http::Response response = http::Get(request);
    if (!response.transportOk) {
        Rate rate;
        SetError(&rate, "请求没到（" + response.error + "）");
        return rate;
    }
    if (response.httpStatus != 200) {
        Rate rate;
        SetError(&rate, "HTTP " + std::to_string(response.httpStatus));
        return rate;
    }
    return ParseRateBody(response.body);
}

Rate FetchRate() { return FetchRate(RateEndpoint{}); }

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------

Rate LoadCachedRate(const std::wstring& path, long long nowUnixSeconds) {
    Rate rate;
    if (path.empty()) {
        rate.error = "没有缓存文件路径";
        return rate;
    }
    bool readOk = false;
    const std::string text = ReadAllBytes(path, &readOk);
    if (!readOk) {
        rate.error = "读不到缓存文件（不存在或不可读）";
        return rate;
    }
    const json::Outcome parsed = json::Parse(text);
    if (!parsed.ok || !parsed.root.IsObject()) {
        rate.error = "缓存文件不是合法 JSON";
        return rate;
    }
    const json::Value* value = parsed.root.Find("rate");
    const json::Value* date = parsed.root.Find("date");
    const json::Value* at = parsed.root.Find("fetched_at");
    if (!value || !value->IsString() || !date || !date->IsString() || !at || !at->IsNumber()) {
        rate.error = "缓存文件形状不对（rate / date / fetched_at 缺一个）";
        return rate;
    }
    int64_t scaled = 0;
    if (!RateToScaled(value->text, &scaled) || scaled <= 0) {
        rate.error = "缓存里的汇率不是一个正数（\"" + value->text + "\"）";
        return rate;
    }
    long long fetchedAt = 0;
    if (!TryParseV(at->text, &fetchedAt) || fetchedAt <= 0) {
        rate.error = "缓存里的 fetched_at 读不出来（\"" + at->text + "\"）";
        return rate;
    }
    // ★ 年龄算不出来的缓存 = 不能用的缓存：看不出多旧的值和一个 30 天旧的值一样不能信。
    const long long age = (nowUnixSeconds - fetchedAt) / kSecondsPerDay;
    if (age < 0) {
        // 时钟被往回调过。不当作"新"，也不当作"没有"以外的任何东西：这一条只能诚实地说
        // 不知道，而不知道就不能信。
        rate.error = "缓存的取到时间在未来（本机时钟被改过？），年龄无从判断";
        return rate;
    }
    if (age > kCacheMaxAgeDays) {
        rate.error = "缓存已过期：存于 " + FormatLocalMinutes(fetchedAt) + "，已 " +
                     std::to_string(age) + " 天（上限 " + std::to_string(kCacheMaxAgeDays) +
                     " 天）—— 丢弃，不用旧值静默换算";
        return rate;
    }
    rate.ok = true;
    rate.cached = true;
    rate.rateText = value->text;
    rate.date = date->text;
    rate.fetchedAtUnix = fetchedAt;
    rate.fetchedAt = FormatLocalMinutes(fetchedAt);
    rate.ageDays = age;
    return rate;
}

bool SaveCachedRate(const std::wstring& path, const Rate& rate, long long nowUnixSeconds,
                    std::wstring* why) {
    if (path.empty() || !rate.ok || rate.rateText.empty()) {
        if (why) *why = L"没有可写的路径或汇率本身不可用";
        return false;
    }

    std::string text = "{\n  \"rate\": \"";
    text += EscapeJson(rate.rateText);
    text += "\",\n  \"date\": \"";
    text += EscapeJson(rate.date);
    text += "\",\n  \"fetched_at\": ";
    text += std::to_string(nowUnixSeconds);
    text += "\n}\n";

    // ★ 先写临时文件、再原子替换（照 panel_drag.cpp 的 SaveWindowPos）。这个文件是
    //   **跨进程**的边界：下一次启动要读它。写一半被杀掉会在盘上留半个 JSON，
    //   而下一次启动会把它当成"缓存损坏"丢掉 —— 用户看到的现象是"上次明明取到了，
    //   这次却说没有"，原因是自己踩到的。
    const std::wstring tempPath = path + L".tmp";
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, tempPath.c_str(), L"wb") != 0 || !f) {
            if (why) *why = L"建不了临时文件（目录不存在或不可写）";
            return false;
        }
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        if (written != text.size()) {
            DeleteFileW(tempPath.c_str());
            if (why) *why = L"写临时文件不完整";
            return false;
        }
    }
    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        DeleteFileW(tempPath.c_str());
        if (why) {
            wchar_t buf[96];
            swprintf_s(buf, L"替换目标文件失败 err=%lu", err);
            *why = buf;
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 一次完整的决定：本次取汇 -> 磁盘缓存 -> 没有汇率
// ---------------------------------------------------------------------------

Rate ResolveRate(const RateEndpoint& endpoint, const std::wstring& cachePath,
                 long long nowUnixSeconds) {
    Rate fresh = FetchRate(endpoint);
    if (fresh.ok) {
        std::wstring why;
        if (!SaveCachedRate(cachePath, fresh, nowUnixSeconds, &why)) {
            // 取到了但没存下来：**照常用它**，只是记一句。这不是错误路径 ——
            // 这个会话有汇率，功能是好的，坏掉的只有"下一次开机能不能少一次请求"。
            const int need = WideCharToMultiByte(CP_UTF8, 0, why.c_str(), static_cast<int>(why.size()),
                                                 nullptr, 0, nullptr, nullptr);
            if (need > 0) {
                std::string narrow(static_cast<std::size_t>(need), '\0');
                WideCharToMultiByte(CP_UTF8, 0, why.c_str(), static_cast<int>(why.size()),
                                    narrow.data(), need, nullptr, nullptr);
                fresh.cacheSaveError = narrow;
            } else {
                fresh.cacheSaveError = "(写失败，原因无法转换)";
            }
        }
        return fresh;
    }

    // 本次取不到 -> 回退到缓存。所有者的理由："反正汇率变化不是很大"，
    // 而且他"不一定一直开着这个软件"，所以开机那一刻的磁盘缓存常常是唯一能用的那个。
    Rate cached = LoadCachedRate(cachePath, nowUnixSeconds);
    if (cached.ok) {
        cached.fallbackNote = fresh.error;
        return cached;
    }
    Rate none;
    none.error = fresh.error + "；缓存也不可用：" + cached.error;
    return none;
}

Rate ResolveRate(const RateEndpoint& endpoint, const std::wstring& cachePath) {
    return ResolveRate(endpoint, cachePath, NowUnixSeconds());
}

bool InjectUsdCounterpart(Sample* sample, const Rate& rate) {
    if (sample == nullptr || !rate.ok) return false;

    // ★ 接口已经给了 USD（海外账号）：一个字节都不改。这条判断必须在最前面，
    //   否则会在一个真 USD 条目之外再造一个换算出来的。
    bool haveUsd = false;
    for (const CurrencyAmount& e : sample->entries) {
        if (e.currency == "USD") {
            haveUsd = true;
            break;
        }
    }
    if (haveUsd) return false;

    // 换算的来源是 entries 里那条 CNY。**不用 sample->total**：entries 是接口原样给的
    // 全部条目，而 total 可能来自另一个币种的优先条目（设计 §2.2 的选择规则）。
    for (const CurrencyAmount& e : sample->entries) {
        if (e.currency != "CNY" || !e.ok) continue;
        const std::string usd = rate.ConvertAmountText(e.total.ToString2());
        if (usd.empty()) return false;   // 算不出来（汇率不可用/金额解析不了）：不补
        CurrencyAmount injected{};
        injected.currency = "USD";
        injected.total = Amount{};
        // 走和接口条目同一条解析路径，所以下游拿到的就是一个普通条目。
        if (!ParseAmount(usd, &injected.total)) return false;
        injected.ok = true;
        sample->entries.push_back(injected);
        return true;
    }
    return false;
}

}  // namespace dshb::fx
