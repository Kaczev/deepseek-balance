// curve_store.h -- the curve data layer: 12-point ring, "only record changes",
//                  curve.json persistence, and the stale-data invalidation rule.
//
// SOURCE OF TRUTH: `不入库文件\曲线规格.md` (§2), the owner's own spec rewritten
// 2026-09-16. Everything below follows that document; the two places where this
// implementation has to make a choice the spec leaves open are marked 【判读】.
//
// ---------------------------------------------------------------------------
// §2.1 Only a CHANGE becomes a point (this is the core of the design)
// ---------------------------------------------------------------------------
//   * the newly fetched value == the last recorded point  -> record NO point,
//     only refresh `update_at`. A flat balance means the user simply was not
//     working, so the flat stretch is erased on purpose.
//   * the newly fetched value != the last recorded point  -> append a point
//     (proviso: the invalidation rule below).
// Consequences the spec spells out: the horizontal axis is SCREEN time, not real
// elapsed time, and the curve stands still while the value does not change.
//
// ★ WHICH currency decides "the value changed": the store judges the comparison
//   on the PRIMARY currency ONLY -- the currency of the newest response's FIRST
//   entry, which is CNY for this account. The file still stores every currency
//   that response carried (CNY and USD both live in one point). This is a
//   deliberate decision of ours, kept here so a later reader does not have to
//   re-derive it:
//     - the widget shows ONE currency at a time; a point appended because the
//       *invisible* currency moved would make the drawn curve change while the
//       displayed number stays put -- exactly the "user was not working" case
//       this design exists to erase;
//     - requiring all currencies to be equal would break the moment an account
//       starts reporting USD: a currency that appears or disappears is not a
//       balance change.
//   The primary currency is passed to Append() by the caller (in production: the
//   first balance_infos entry of the newest response) -- the store never hard-
//   codes CNY, because the account can be switched.
//
// ---------------------------------------------------------------------------
// §2.2 Ring of 12 points
// ---------------------------------------------------------------------------
//   Capacity 12: the 11 being displayed plus one incoming. Fewer than 12 points
//   is legal (the display layer flattens the left side; that is not this file's
//   job).
//
// ---------------------------------------------------------------------------
// §2.3 Timestamps and the invalidation rule
// ---------------------------------------------------------------------------
//   With elapsed D = now - update_at, when a value v arrives against last point l:
//     v == l                -> refresh update_at only, whatever D is
//     v != l and D > 86400  -> the STORED data is unusable: discard the points
//     v != l and D <= 86400 -> append the point, then refresh update_at
//   The same 86400 s rule is applied when the file is LOADED, because a file
//   written more than a day ago describes an account nobody was using. The owner
//   chose 86400 s explicitly and rejected a smaller threshold (spec §2.3: an hour
//   off means the user was away, the balance normally holds, and if it moved it
//   is the endpoint's own update lag worth a cent or two -- still valid). Do not
//   "improve" it.
//
// ---------------------------------------------------------------------------
// §2.4 One file, not two
// ---------------------------------------------------------------------------
//   `curve.json` holds the last-update timestamp AND the points, so no state can
//   be lost between two files:
//     { "update_at": 1789537189, "points": [ {"CNY": "18.80", "USD": null}, ... ] }
//   Currencies are the API's own codes (CNY / USD). A currency the response did
//   not carry is null. The newest point is the LAST element.
//   【判读】the spec's example prints 18.80 as a bare JSON number; this store
//   writes the amounts as JSON STRINGS. The project rule is that money is a
//   decimal string parsed with dshb::ParseAmount and is never a binary float
//   (amount.h, design §3.1); a bare 18.80 in a file would force every reader
//   through a double. The reader stays faithful: it accepts a string or a number
//   member and keeps the source digits verbatim either way.
//   The file keeps the digits it was given: "18.80" is stored back as "18.80",
//   never re-formatted to "18.8" or to a computed 4-decimal form (acceptance 3).
//
// ---------------------------------------------------------------------------
// Failure policy
// ---------------------------------------------------------------------------
//   Missing file, empty file, malformed JSON, wrong shape: start EMPTY and say
//   why through LoadResult. Nothing here throws, nothing here crashes, and a
//   half-read file is never half-loaded -- the points are only committed to the
//   ring after the whole file has been validated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dshb {

// ---------------------------------------------------------------------------
// One point of the curve
// ---------------------------------------------------------------------------
// ★ The amount keeps the exact decimal text it was read from ("18.80"), so no
//   precision is invented here. `missing` means "this response had no such
//   currency" -- that is NOT a zero balance, and the two must never be merged
//   (the same distinction the API layer refuses to blur).
struct CurvePoint {
    struct Entry {
        std::string currency;    // API code, e.g. "CNY" / "USD"
        std::string text;        // verbatim decimal string; empty when missing
        bool missing = true;     // true = the response carried no such currency
    };

    std::vector<Entry> entries;

    // nullptr when this point has no such currency.
    const Entry* Find(const std::string& currency) const;
};

// ---------------------------------------------------------------------------
// What one fetch produced
// ---------------------------------------------------------------------------
// `primaryCurrency` is the currency whose change decides whether a point is
// appended (see the ★ note at the top). `observations` are all currencies the
// response carried, in the response's own order.
//
// An observation with `amountOk == false` is stored as null (absent): either the
// response did not carry the currency, or the text could not be parsed as money.
// Guessing a value for either case would put a number on the curve that the
// account never reported.
struct CurveObservation {
    struct Item {
        std::string currency;
        std::string text;        // verbatim decimal string from the response
        bool amountOk = false;   // false = absent (not 0), never appended as a value
    };

    std::string primaryCurrency;   // currency of the newest response's FIRST entry
    std::vector<Item> observations;
};

// ---------------------------------------------------------------------------
// Load outcome
// ---------------------------------------------------------------------------
// A failed load still leaves the store usable and empty; no exception is thrown.
struct CurveLoadResult {
    enum class Status {
        Loaded,      // the file existed, parsed, and was fresh
        Missing,     // no file yet -- an ordinary first run, not an error
        Empty,       // the file existed but carried no bytes
        Malformed,   // not JSON, or JSON of the wrong shape
        NoTimestamp, // readable the file's points are dropped: update_at is absent or null,
                     // so there is no way to tell fresh points from year-old ones
        Expired      // parsed, but update_at was more than 86400 s old: points discarded
    };

    Status status = Status::Missing;
    bool ok = false;         // true only for Loaded
    bool discarded = false;  // true when points were thrown away (Expired)
    bool resetsClock = false;// true when update_at is unusable and the next append must start over
    int pointsLoaded = 0;
    int pointsDiscarded = 0;
    std::string detail;      // one short human-readable line, never empty on failure

    const char* StatusName() const;
};

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------
class CurveStore {
public:
    // §2.2: 11 displayed + 1 incoming.
    static constexpr std::size_t kCapacity = 12;
    // §2.3: the owner's chosen threshold, in seconds. Not tunable on purpose.
    static constexpr int64_t kExpirySeconds = 86400;

    // -- persistence: one file, `curve.json` --------------------------------

    // Reads path if it exists. Always leaves the store in a defined state: on
    // missing/empty/malformed/expired it is EMPTY (no exception, no partial load).
    CurveLoadResult Load(const std::string& path);

    // Writes `curve.json` atomically enough for this purpose: one write, flushed,
    // no file left behind on failure. Returns false on any I/O error.
    bool Save(const std::string& path) const;

    // -- data ---------------------------------------------------------------

    // Feeds one fetch (§2.1/§2.3). Returns true when a point was appended.
    //
    //   nowSeconds <= 0, no primary currency, or an unusable primary amount
    //        -> nothing is appended and update_at is refreshed.
    //           【判读】the spec's table has only two rows (v == l / v != l); a
    //           response with no usable primary value is neither, and appending a
    //           point whose value we could not read would fabricate data. The
    //           timestamp is still refreshed because the fetch did happen.
    //   v == l (primary text, compare-as-number)      -> no point, refresh time
    //   v != l and D >  86400                         -> stored data discarded
    //   v != l and D <= 86400                         -> point appended, time set
    bool Append(const CurveObservation& obs, int64_t nowSeconds);

    void Clear();

    // Oldest -> newest (the newest point is the last element).
    std::vector<CurvePoint> Points() const;

    // The newest n points, oldest -> newest; fewer when the store holds fewer.
    std::vector<CurvePoint> Newest(std::size_t n) const;

    // §2.2: the display layer takes the newest 11 of these.
    std::vector<CurvePoint> DisplayPoints() const { return Newest(kCapacity - 1); }

    std::size_t size() const { return count_ < kCapacity ? count_ : kCapacity; }
    bool empty() const { return size() == 0; }
    int64_t lastUpdate() const { return updateAt_; }

    // Why the points were thrown away by the most recent Append(), when they were:
    // "stale" (§2.3, the elapsed time exceeded the threshold) or "no timestamp"
    // (nothing was stored yet, so there was no elapsed time to measure). Empty when
    // nothing was discarded. Diagnostics only -- nothing depends on it.
    const std::string& lastClearReason() const { return clearReason_; }

    // §2.3 applied to "now": would the stored data be discarded right now?
    // True when update_at is unknown, since nothing usable is stored then.
    bool Expired(int64_t nowSeconds) const;

    // The newest point's value for the PRIMARY currency, or "" when there is none.
    // The value the next Append() compares against (§2.1).
    const std::string& lastPrimaryText() const { return lastPrimaryText_; }
    const std::string& lastPrimaryCurrency() const { return lastPrimaryCurrency_; }

private:
    CurvePoint buf_[kCapacity]{};
    std::size_t count_ = 0;   // points written in total (may exceed the capacity)
    std::size_t next_ = 0;    // next slot to write
    int64_t updateAt_ = 0;    // §2.4 "update_at": the last update, in seconds
    bool updateAtValid_ = false;
    std::string lastPrimaryText_;      // verbatim text of the newest point's value
    std::string lastPrimaryCurrency_;  // which currency that value belongs to
    std::string clearReason_;          // "stale" / "no timestamp" / empty
};

}  // namespace dshb
