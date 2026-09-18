// curve_store.cpp -- see curve_store.h for the model and for the spec references.
//
// No exceptions cross this file: every failure is a return value. No money is ever
// held as a binary float (`dshb::ParseAmount` is the only converter, amount.h), and
// no point is committed to the ring until the whole file has been validated.
#include "curve_store.h"

#include "amount.h"
#include "json_min.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace dshb {
namespace {

// --- small file helpers ------------------------------------------------------
// Paths are UTF-8 (the probe prints them; production builds them from
// %LOCALAPPDATA%), so both directions go through the wide API plus one conversion.

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string FormatMessageOf(DWORD code) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "windows error %lu", static_cast<unsigned long>(code));
    return buf;
}

bool ReadFileBytes(const std::string& path, std::string* out, std::string* error) {
    const std::wstring wide = Utf8ToWide(path);
    if (wide.empty()) {
        *error = "empty path";
        return false;
    }
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = FormatMessageOf(GetLastError());
        return false;
    }

    out->clear();
    char chunk[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, chunk, sizeof(chunk), &read, nullptr)) {
            *error = FormatMessageOf(GetLastError());
            CloseHandle(file);
            return false;
        }
        if (read == 0) break;
        out->append(chunk, read);
    }
    CloseHandle(file);
    return true;
}

bool WriteFileBytes(const std::string& path, const std::string& bytes, std::string* error) {
    const std::wstring wide = Utf8ToWide(path);
    if (wide.empty()) {
        *error = "empty path";
        return false;
    }
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = FormatMessageOf(GetLastError());
        return false;
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t left = bytes.size() - written;
        const DWORD want = static_cast<DWORD>(left > 0x10000000u ? 0x10000000u : left);
        DWORD done = 0;
        if (!WriteFile(file, bytes.data() + written, want, &done, nullptr) || done == 0) {
            *error = FormatMessageOf(GetLastError());
            CloseHandle(file);
            return false;
        }
        written += done;
    }
    // Flush before closing: a curve file left half on disk after a crash is exactly
    // the cross-file inconsistency §2.4 merged the two files to avoid.
    FlushFileBuffers(file);
    CloseHandle(file);
    return true;
}

// Creates one directory level, best effort. The caller names a path whose parent
// already exists (production: paths.cpp; the probe: a directory it made itself);
// deeper chains are not this helper's job and are not needed.
void TryMakeDirectory(const std::string& utf8Dir) {
    const std::wstring wide = Utf8ToWide(utf8Dir);
    if (wide.empty()) return;
    CreateDirectoryW(wide.c_str(), nullptr);
}

// Load-time clock, in seconds. §2.3 only talks about the elapsed time between two
// fetches, so a wrong system clock can only ever make the store *more* eager to
// discard -- it can never resurrect stale points -- and a future timestamp yields a
// negative elapsed time, which never expires.
int64_t NowSeconds() { return static_cast<int64_t>(::time(nullptr)); }

// --- JSON writing ------------------------------------------------------------
// json_min.h is a READER only (json_min.h: "minimal JSON reader"), so the writer for
// the single document this store owns lives here rather than in a second parser.

void AppendJsonString(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", byte);
                    out += esc;
                } else {
                    // Bytes >= 0x80 are copied verbatim: JSON strings are UTF-8 and
                    // nothing here transcodes.
                    out += c;
                }
                break;
        }
    }
    out += '"';
}

std::string Serialize(int64_t updateAt, bool updateAtValid, const std::vector<CurveStorePoint>& points) {
    std::string out = "{\n  \"update_at\": ";
    if (updateAtValid) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(updateAt));
        out += buf;
    } else {
        // Nothing has been timestamped yet; null is honest and reloads as "start over".
        out += "null";
    }
    out += ",\n  \"points\": [";
    for (std::size_t i = 0; i < points.size(); ++i) {
        out += (i == 0) ? "\n    {" : ",\n    {";
        for (std::size_t k = 0; k < points[i].entries.size(); ++k) {
            const CurveStorePoint::Entry& entry = points[i].entries[k];
            out += (k == 0) ? " " : ", ";
            AppendJsonString(out, entry.currency);
            out += ": ";
            if (entry.missing) {
                out += "null";
            } else {
                // The digits as recorded -- "18.80" stays "18.80" (acceptance 3).
                AppendJsonString(out, entry.text);
            }
        }
        // ★ §2.3b: the point's OWN time, in epoch seconds. An UNDATED point is written
        //   as an explicit null rather than being stamped with the global update_at:
        //   the round trip has to remember "nobody measured when this happened", and
        //   a bare omission could not be told apart from a point whose sole currency
        //   failed to parse. Null is also what makes an all-undated file reload as
        //   undated instead of as a set of points that all share one instant.
        // The separator before the timestamp member is unconditional: an object always
        // carries "at", so the member before it is never the last one.
        out += ", ";
        if (points[i].atValid) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(points[i].at));
            out += "\"at\": ";
            out += buf;
        } else {
            out += "\"at\": null";
        }
        // ★ The ambience colour of that instant. Written ONLY when the point actually
        //   carries one: a point loaded from a pre-colour file must come back out of
        //   Save() byte-identical, and a caller that recorded no colour never gets one
        //   invented for it. The member name is "color" (American) to match the JSON
        //   the rest of the file already speaks ("update_at", "points").
        if (!points[i].color.empty()) {
            out += ", ";
            out += "\"color\": ";
            AppendJsonString(out, points[i].color);
        }
        if (!points[i].entries.empty()) out += " ";
        out += "}";
    }
    out += points.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

// --- JSON reading ------------------------------------------------------------

// True for exactly "#rrggbb" (lower or upper case hex). This is the only shape the
// writer ever emits, so anything else is treated as "no colour" rather than guessed at.
bool IsHexColor(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// Converts a verbatim JSON number token into whole seconds.
// Rejects anything that is not a plain integer in range, so "1789537189.5" or "1e3"
// can never be silently turned into a timestamp pointing at the wrong time.
bool ParseWholeSeconds(const std::string& text, int64_t* out) {    if (text.empty() || text.size() > 19) return false;
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        i = 1;
    } else if (text[0] == '+') {
        i = 1;
    }
    if (i >= text.size()) return false;

    int64_t value = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    *out = negative ? -value : value;
    return true;
}

}  // namespace

const char* CurveLoadResult::StatusName() const {
    switch (status) {
        case Status::Loaded: return "loaded";
        case Status::Missing: return "missing";
        case Status::Empty: return "empty";
        case Status::Malformed: return "malformed";
        case Status::NoTimestamp: return "no-timestamp";
        case Status::Expired: return "expired";
    }
    return "?";
}

const CurveStorePoint::Entry* CurveStorePoint::Find(const std::string& currency) const {
    for (const Entry& entry : entries) {
        if (entry.currency == currency) return &entry;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

CurveLoadResult CurveStore::Load(const std::string& path) {
    CurveLoadResult result;
    Clear();

    std::string bytes;
    std::string ioError;
    if (!ReadFileBytes(path, &bytes, &ioError)) {
        result.status = CurveLoadResult::Status::Missing;
        result.resetsClock = true;
        result.detail = "no readable file (start empty): " + ioError;
        return result;   // an absent file is an ordinary first run, not a failure
    }
    if (bytes.empty()) {
        result.status = CurveLoadResult::Status::Empty;
        result.resetsClock = true;
        result.detail = "file is empty (start empty)";
        return result;
    }

    const json::Outcome parsed = json::Parse(bytes);
    if (!parsed.ok || !parsed.root.IsObject()) {
        result.status = CurveLoadResult::Status::Malformed;
        result.resetsClock = true;
        result.detail = parsed.ok ? "root is not a JSON object (start empty)"
                                  : "not JSON (start empty): " + parsed.error;
        return result;
    }

    // Everything is collected in locals first: a half-valid file must never leave
    // half a curve in the store.
    std::vector<CurveStorePoint> loaded;
    int64_t updateAt = 0;
    bool sawUpdateAt = false;
    bool ok = true;
    std::string problem;

    const json::Value* points = parsed.root.Find("points");
    if (points) {
        if (!points->IsArray()) {
            ok = false;
            problem = "\"points\" is not an array";
        } else {
            for (const json::Value& element : points->items) {
                if (!element.IsObject()) {
                    ok = false;
                    problem = "\"points\" holds a non-object element";
                    break;
                }
                CurveStorePoint point;
                for (const auto& member : element.members) {
                    // ★ §2.3b: "at" is the point's OWN time, not a currency. It is read
                    //   before the currency loop's checks can misread it as one.
                    if (member.first == "at") {
                        if (member.second.IsNumber() && ParseWholeSeconds(member.second.text, &point.at)) {
                            // A usable instant is a POSITIVE epoch second. 0 / negative is
                            // not a time anyone measured at, and treating it as a real one
                            // would put 1970 into a rate calculation.
                            point.atValid = point.at > 0;
                        } else if (member.second.IsNull()) {
                            point.atValid = false;   // explicitly undated -- stays undated
                        } else {
                            // "at" present but not whole seconds: never guess a time for a
                            // point. It loads as UNDATED, which the estimator already
                            // refuses to use, rather than as a fabricated timestamp.
                            point.atValid = false;
                        }
                        continue;
                    }
                    if (member.first.empty()) {
                        ok = false;
                        problem = "a point has an empty currency name";
                        break;
                    }
                    // ★ The ambience colour ("E 蒙光.md" §3.1). It is a plain "#rrggbb"
                    //   string. A point that carries none -- an older file, or one that
                    //   simply never had a colour -- loads with an EMPTY colour and is
                    //   then drawn with the current colour (the display layer's choice,
                    //   see CurvePoint::hasColor). A malformed colour is NOT a load
                    //   failure: the colour is decoration, and throwing away the whole
                    //   curve because one hex digit is wrong would lose real money data.
                    //   It is dropped instead (left empty = "no colour"), which is the
                    //   same state an older file loads in.
                    if (member.first == "color") {
                        if (member.second.IsString() && IsHexColor(member.second.text)) {
                            point.color = member.second.text;
                        }
                        continue;
                    }
                    if (member.second.IsNull()) {
                        // §2.4: "a currency may be absent from that response".
                        point.entries.push_back(CurveStorePoint::Entry{member.first, std::string(), true});
                        continue;
                    }
                    if (!member.second.IsString() && !member.second.IsNumber()) {
                        ok = false;
                        problem = "a point value is neither a decimal string nor null";
                        break;
                    }
                    // Checked with dshb::ParseAmount and then stored as TEXT: the digits
                    // are never re-formatted, and no binary float is produced anywhere
                    // (amount.h; acceptance 3).
                    Amount amount;
                    if (!ParseAmount(member.second.text, &amount)) {
                        ok = false;
                        problem = "a point value is not a decimal amount";
                        break;
                    }
                    point.entries.push_back(CurveStorePoint::Entry{member.first, member.second.text, false});
                }
                if (!ok) break;
                if (point.entries.empty()) {
                    ok = false;
                    problem = "a point carries no currency";
                    break;
                }
                loaded.push_back(std::move(point));
            }
        }
    }

    bool sawUpdateAtField = false;
    if (const json::Value* update = parsed.root.Find("update_at")) {
        sawUpdateAtField = true;
        if (update->IsNumber() && ParseWholeSeconds(update->text, &updateAt)) {
            sawUpdateAt = true;
        } else if (update->IsNull()) {
            // Explicitly "no timestamp": readable, but the points cannot be trusted and
            // the next append starts the curve over. Handled below.
        } else {
            ok = false;
            problem = "\"update_at\" is not whole seconds";
        }
    } else {
        sawUpdateAtField = true;   // missing and null are the same situation here
    }

    if (!ok) {
        result.status = CurveLoadResult::Status::Malformed;
        result.resetsClock = true;
        result.detail = problem + " (start empty)";
        return result;
    }

    if (!sawUpdateAt) {
        // No usable timestamp: the points in the file cannot be dated, so a point from
        // last year looks exactly like one from a minute ago. The task's rule is to
        // start empty in this situation, and the next append begins the curve.
        result.status = CurveLoadResult::Status::NoTimestamp;
        result.resetsClock = true;
        result.pointsLoaded = 0;
        result.pointsDiscarded = static_cast<int>(loaded.size());
        result.detail = std::string(sawUpdateAtField ? "\"update_at\" is null or missing"
                                                     : "\"update_at\" is missing") +
                        ": stored points cannot be dated, start empty";
        return result;
    }

    // §2.3 applied at load time: the same 86400 s rule as on append. A file written
    // more than a day ago describes an account nobody was using, so its points are
    // discarded rather than continued.
    const int64_t now = NowSeconds();
    if (updateAt > 0 && now - updateAt > kExpirySeconds) {
        result.status = CurveLoadResult::Status::Expired;
        result.discarded = true;
        result.resetsClock = true;
        result.pointsLoaded = 0;
        result.pointsDiscarded = static_cast<int>(loaded.size());
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "stored update_at is %lld s old (> %lld s): %d point(s) discarded, start empty",
                      static_cast<long long>(now - updateAt), static_cast<long long>(kExpirySeconds),
                      static_cast<int>(loaded.size()));
        result.detail = buf;
        return result;   // the store was cleared at entry and stays empty
    }

    updateAt_ = updateAt;
    updateAtValid_ = true;
    count_ = 0;
    next_ = 0;
    const std::size_t total = loaded.size();
    const std::size_t keep = std::min(total, kCapacity);
    for (std::size_t i = total - keep; i < total; ++i) {
        buf_[next_] = loaded[i];   // the ring keeps the newest 12 in order
        next_ = (next_ + 1) % kCapacity;
        ++count_;
    }
    if (count_ > 0) {
        // The newest point's first entry is the primary currency again: the file keeps
        // the response's own order, so the change comparison survives the reload.
        const CurveStorePoint& newest = buf_[(next_ + kCapacity - 1) % kCapacity];
        lastPrimaryCurrency_ = newest.entries.front().currency;
        lastPrimaryText_ = newest.entries.front().missing ? std::string() : newest.entries.front().text;
    }

    result.status = CurveLoadResult::Status::Loaded;
    result.ok = true;
    result.pointsLoaded = static_cast<int>(keep);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d point(s) loaded, update_at is %lld s old",
                      static_cast<int>(keep), static_cast<long long>(now - updateAt));
        result.detail = buf;
    }
    return result;
}

bool CurveStore::Save(const std::string& path) const {
    // Written oldest -> newest, so the newest point is the last array element (§2.4).
    const std::string bytes = Serialize(updateAt_, updateAtValid_, Points());

    const std::size_t cut = path.find_last_of("\\/");
    if (cut != std::string::npos && cut > 0) {
        TryMakeDirectory(path.substr(0, cut));
    }

    std::string error;
    return WriteFileBytes(path, bytes, &error);
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void CurveStore::Clear() {
    for (CurveStorePoint& point : buf_) point = CurveStorePoint{};
    count_ = 0;
    next_ = 0;
    updateAt_ = 0;
    updateAtValid_ = false;
    lastPrimaryText_.clear();
    lastPrimaryCurrency_.clear();
    clearReason_.clear();
}

std::vector<CurveStorePoint> CurveStore::Points() const {
    std::vector<CurveStorePoint> out;
    const std::size_t live = size();
    out.reserve(live);
    const std::size_t start = (count_ < kCapacity) ? 0 : next_;   // oldest first
    for (std::size_t i = 0; i < live; ++i) {
        out.push_back(buf_[(start + i) % kCapacity]);
    }
    return out;
}

std::vector<CurveStorePoint> CurveStore::Newest(std::size_t n) const {
    std::vector<CurveStorePoint> all = Points();
    if (n == 0 || all.size() <= n) return (n == 0) ? std::vector<CurveStorePoint>() : all;
    return std::vector<CurveStorePoint>(all.end() - static_cast<std::ptrdiff_t>(n), all.end());
}

bool CurveStore::Expired(int64_t nowSeconds) const {
    if (!updateAtValid_ || updateAt_ <= 0) return true;   // nothing usable is stored
    return nowSeconds - updateAt_ > kExpirySeconds;
}

bool CurveStore::Append(const CurveObservation& obs, int64_t nowSeconds,
                        const std::string& colorHex) {
    if (nowSeconds <= 0) return false;   // no trustworthy clock -> never invent a timestamp

    // Reset the diagnostic first; it is set again below when this call discards points.
    clearReason_.clear();

    // Which observation decides "the value changed" (see the ★ note in the header).
    const CurveObservation::Item* primary = nullptr;
    for (const CurveObservation::Item& item : obs.observations) {
        if (item.currency == obs.primaryCurrency) {
            primary = &item;
            break;
        }
    }
    if (obs.observations.empty() || primary == nullptr || !primary->amountOk) {
        // 【判读】neither row of §2.1's table: no readable primary value. Refresh the
        // timestamp (the fetch did happen) but record nothing -- a point whose value we
        // could not read would be fabricated data. The next readable fetch compares
        // against the newest point as usual.
        if (updateAtValid_) updateAt_ = nowSeconds;
        return false;
    }

    if (updateAtValid_ && size() > 0) {
        // The last recorded value of the PRIMARY currency. When the newest point does
        // not carry it, there is nothing to compare against and the value counts as
        // changed (the same answer as "we could not read the old value").
        const CurveStorePoint& newest = buf_[(next_ + kCapacity - 1) % kCapacity];
        const CurveStorePoint::Entry* lastPrimary = newest.Find(lastPrimaryCurrency_);

        // ★ Compare AMOUNTS, not text. "18.80" and "18.8" are the same balance written
        //   differently, and §2.1 is about the balance changing, not about the endpoint
        //   re-formatting its decimals -- a text comparison would append a point every
        //   time the API trimmed a trailing zero, which is precisely the "user was not
        //   working" case this design erases. (Found by the probe: a fixture feeding
        //   "18.80" then "18.8" appended two points instead of one.)
        //   Both sides are parsed with dshb::ParseAmount; the stored text is untouched,
        //   so the file still keeps the digits it was given.
        Amount previousAmount;
        Amount incomingAmount;
        const bool havePrevious = lastPrimaryCurrency_ == obs.primaryCurrency && lastPrimary &&
                                  !lastPrimary->missing &&
                                  ParseAmount(lastPrimary->text, &previousAmount);
        const bool haveIncoming = ParseAmount(primary->text, &incomingAmount);

        // §2.1: the value equals the last recorded point -> no point, timestamp only.
        if (havePrevious && haveIncoming && previousAmount == incomingAmount) {
            updateAt_ = nowSeconds;
            return false;
        }

        // §2.3: a gap wider than the threshold makes the stored curve unusable. The
        // incoming observation is still recorded -- it is fresh data, and the app is
        // running now.
        if (nowSeconds - updateAt_ > kExpirySeconds) {
            Clear();
            clearReason_ = "stale";
        }
    } else if (!updateAtValid_) {
        // ★ An UNKNOWN clock: nothing was ever stored, or the file carried no usable
        // update_at (first run, or `"update_at": null`). That is not the same thing as
        // stale data -- there is no elapsed time to measure -- so this fetch simply
        // starts the curve. Without this branch the first point would be appended and
        // then immediately discarded as "1080000000 seconds old", i.e. a fresh install
        // would lose its very first measurement.
        Clear();
        clearReason_ = "no timestamp";
    }

    CurveStorePoint point;
    // §2.3b: the point's own time. `nowSeconds` is trustworthy here -- the guard at the
    // top of this function returns before reaching this line when it is not.
    point.at = nowSeconds;
    point.atValid = true;
    // ★ The ambience colour of this instant ("E 蒙光.md" §3.1). Only a well-formed
    //   "#rrggbb" is recorded; anything else is stored as "no colour" rather than
    //   written into the file and then refused by the reader (one shape, one meaning).
    if (IsHexColor(colorHex)) point.color = colorHex;
    for (const CurveObservation::Item& item : obs.observations) {
        if (item.currency.empty()) continue;   // a nameless entry cannot be written down
        point.entries.push_back(CurveStorePoint::Entry{
            item.currency,
            item.amountOk ? item.text : std::string(),
            !item.amountOk});
    }
    if (point.entries.empty()) {
        // No writable currency at all: fall back to the primary alone, which we know
        // parsed, so the point is never an empty object.
        point.entries.push_back(CurveStorePoint::Entry{obs.primaryCurrency, primary->text, false});
    }

    buf_[next_] = std::move(point);
    next_ = (next_ + 1) % kCapacity;
    ++count_;
    lastPrimaryText_ = primary->text;
    lastPrimaryCurrency_ = obs.primaryCurrency;
    updateAt_ = nowSeconds;     // §2.3: "仅更新 update_at"
    updateAtValid_ = true;
    return true;
}

// 给"最新那个点的**前一个**点"写颜色。
// ★ 为什么需要这个口子：段 P_N→P_(N+1) 的颜色 = **终止于 P_(N+1) 那一步**的 R_new
//   （所有者 2026-09-18 的口径）。而写 P_N 的时候那一步还不存在 —— 它要等 P_(N+1) 到达
//   才知道。所以顺序是"先把新点落进去、再量出那一步、回头给前一个点上色"，回填的就是它。
// ★ 最新那个点自己在被回填之前**没有颜色**：它右边那一段暂时是空白的，等下一个点到达时
//   才上色 —— 这与主循环滞后一拍是同一个节奏。
// ★ 这不是"改历史数据"：值、时间、条目一个都不动，只写颜色这一个字段。
// ★ 点数不足 2 时返回 false（没有"前一个点"可写）。
bool CurveStore::SetColorOfPrevNewest(const std::string& colorHex) {
    if (count_ < 2) return false;
    const std::size_t prev = (next_ + kCapacity - 2) % kCapacity;
    buf_[prev].color = IsHexColor(colorHex) ? colorHex : std::string();
    return true;
}

}  // namespace dshb
