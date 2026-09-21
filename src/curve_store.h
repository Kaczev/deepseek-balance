// curve_store.h -- the curve data layer: 8641-point ring, "only record changes",
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
// §2.2 Ring of points
// ---------------------------------------------------------------------------
//   Capacity 8641. The number is derived, not chosen:
//     8640 = 24 x 60 x 60 / 10 s, where 10 s is tuning.h's kApiIntervalMs -- the fixed
//            poll rhythm (§2.3; the old 30/10/3 adaptive cadence is dead and no longer
//            exists to shorten it). One poll appends at most one point (§2.1), so 8640 is
//            the largest number of CHANGES one calendar day can produce.
//     1    = the day's BASELINE: the newest point from before midnight. "今日已 X.XX¥"
//            (widget_display.cpp's TodayUsageFromStore) needs that point to count the
//            day's first drop -- the one that spans midnight. A ring holding only today's
//            8640 points loses it the moment the 8640th change evicts it, and the whole
//            overnight drop leaves the total silently.
//   Every rule above (§2.1 "only a change becomes a point", §2.3 the invalidation rule,
//   §2.4 one file) is untouched.
//   ★ THE COST, stated so nobody has to rediscover it: the capacity equals that ceiling
//     exactly and has no headroom. If kApiIntervalMs ever drops below 10 s, a busy day
//     produces more changes than the ring holds and "今日已 X.XX¥" starts under-counting
//     again -- the oldest points of today are the first to be evicted. Lowering the poll
//     interval therefore REQUIRES raising the 8640 by the same factor. The 1 is not spare
//     capacity either: a day that reaches 8640 changes spends it on the baseline.
//   ★ The file format is unchanged: `curve.json` is the same JSON, it may just carry
//     up to 8641 points now, so a file written by an older build loads fine and keeps
//     its newest points.
//   ★ AND THE DISPLAY IS STILL 12: the panel draws the newest 12 of whatever the ring
//     holds -- that slice is the display layer's own constant (widget_display.h's
//     kCurveDisplayPoints), deliberately NOT kCapacity. The two numbers are decoupled
//     on purpose: a capacity that grows to cover a day must not change how many
//     points are laid out on 11 slots.
//   Fewer than 12 points is legal (the display layer flattens the left side; that
//   is not this file's job).
//
//   ★ THE RING LIVES ON THE HEAP, and that is a requirement, not a preference: at
//     sizeof(CurveStorePoint) = 72 B the ring is ~620 KB, while an MSVC frame gets
//     1 MB of stack. An inline array would put those 620 KB in every CurveStore's
//     frame -- and probes build several stores in one scope, so the inline array is an
//     immediate stack overflow (tools/panelprobe.cpp, case 2).
//     buf_ is a std::vector sized once at construction; buf_[i] still means "slot i".
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
// §2.3b ★ Each point carries its OWN timestamp ("at", epoch SECONDS)
// ---------------------------------------------------------------------------
//   `update_at` alone cannot time a point: these points are CHANGES, so two
//   consecutive ones can be 10 seconds apart or three hours apart -- when the
//   balance did not move, nothing was appended and only update_at crept forward.
//   Every calculation that divides an amount by a time (the consumption rate,
//   §7.2/§7.3 of the design) is a lie without the per-point time, so each point
//   stores the moment of ITS OWN append:
//     { "CNY": "15.43", "at": 1789561662 }
//
//   ★ NEVER assume the sampling interval. 10 s is the *poll* interval (§4.1),
//     not the point spacing: a point exists only where a change was seen.
//
//   ★ UNDATED POINTS ARE A REAL STATE, AND THEY STAY UNDATED. A `curve.json`
//     written before this field existed has no "at" on any point. Those points
//     must not silently inherit the file's global `update_at`: that would claim
//     every one of them was measured at the same instant, which is exactly the
//     fabrication this field removes -- and it would make a rate estimate out of
//     times nobody ever measured. They load as `atValid == false` ("undated"),
//     they are written back as `"at": null`, and the estimator refuses to fit
//     them (rate_estimator.cpp). The global `update_at` keeps its own job
//     (§2.3, fresh-or-stale) and is not a per-point time.
//   A point appended now always carries a usable `at`, so an all-undated file
//   heals itself as soon as the next change arrives (that point is dated; the
//   older ones stay undated, which is the honest record of them).
//
// ---------------------------------------------------------------------------
// §2.4 One file, not two
// ---------------------------------------------------------------------------
//   `curve.json` holds the last-update timestamp AND the points, so no state can
//   be lost between two files:
//     { "update_at": 1789537189,
//       "points": [ {"CNY": "18.80", "USD": null, "at": 1789537189}, ... ] }
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
//
// ★ WHY THE NAME IS `CurveStorePoint` and not `CurvePoint`: the display layer
//   (widget_display.h) already declares `dshb::CurvePoint` -- a normalised
//   screen point {x,y} for the renderer. Both live in namespace dshb, so the
//   two names collided the moment one translation unit needed both (the display
//   layer is exactly that unit: it reads this store and writes those points).
//   This one is the STORE's record (currencies and amounts), so it carries the
//   store's name; the display's name is unchanged.
struct CurveStorePoint {
    struct Entry {
        std::string currency;    // API code, e.g. "CNY" / "USD"
        std::string text;        // verbatim decimal string; empty when missing
        bool missing = true;     // true = the response carried no such currency
    };

    std::vector<Entry> entries;

    // ★ §2.3b: the moment THIS point was appended, in epoch seconds. `atValid ==
    //   false` means "undated" -- a point read from a file written before the field
    //   existed, or one whose "at" was null/unusable. `at` is 0 and meaningless then;
    //   read it only when `atValid` says it can be read. Nothing may substitute the
    //   store's global update_at for a missing per-point time.
    int64_t at = 0;
    bool atValid = false;

    // ★ The ambience colour of the moment this point was appended ("E 蒙光.md" §3.1:
    //   a node *is* the ambience colour of that instant, and the segments between
    //   nodes interpolate). Kept as the SAME "#rrggbb" text the writer emitted, for
    //   the same reason the amounts are text: nothing here invents precision.
    //
    //   EMPTY means "this point has no colour" -- either a point read from a file
    //   written before the field existed, or a caller that passed none. An empty
    //   colour is never written back, so a legacy file round-trips byte-identically
    //   instead of growing a field nobody measured.
    std::string color;

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
    // §2.2: the ring size -- one day of changes plus the day's baseline point (see the
    // §2.2 note above for the derivation and the cost of having no headroom). NOT the
    // display width -- the panel slices the newest 12 out of this.
    static constexpr std::size_t kCapacity = 8641;
    // §2.3: the owner's chosen threshold, in seconds. Not tunable on purpose.
    static constexpr int64_t kExpirySeconds = 86400;

    // The ring is sized once, here, and never resized: every slot is addressable from
    // the moment the store exists, which is the invariant the rest of this file relies
    // on. The allocation is ~620 KB (see §2.2) -- paid once per store, not per frame.
    CurveStore() : buf_(kCapacity) {}

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
    //
    // An appended point records the same `nowSeconds` as its own `at` (§2.3b), which
    // is why a point only ever has one time and the two can never disagree.
    //
    // `colorHex` is the ambience colour to store with the point, as "#rrggbb"; an
    // empty string stores no colour at all (the point then draws with the current
    // colour, see widget_display.h's CurvePoint::hasColor). The two-argument form
    // stays for the probes and for any caller that has no colour to record -- it is
    // exactly `Append(obs, nowSeconds, "")`.
    bool Append(const CurveObservation& obs, int64_t nowSeconds, const std::string& colorHex);
    bool Append(const CurveObservation& obs, int64_t nowSeconds) {
        return Append(obs, nowSeconds, std::string());
    }

    void Clear();

    // Oldest -> newest (the newest point is the last element).
    // ★ COPIES THE WHOLE RING, so it is for the probes and for one-off paths only.
    //   Inside a frame loop or a per-poll path use size() + At(i) instead: at the
    //   capacity above this call is ~620 KB of string copies per call.
    std::vector<CurveStorePoint> Points() const;

    // The i-th point, oldest = 0, in the SAME order Points() returns: At(0) is the
    // oldest, At(size() - 1) the newest. Reads in place -- no copy, no allocation --
    // which is why the per-frame paths use it. The caller guarantees i < size().
    // ★ The modulo is the ring: the slots are cyclic, so start_ + i can run past the end
    //   of the buffer once the ring is full (the oldest slot rotates back to slot 0).
    const CurveStorePoint& At(std::size_t i) const { return buf_[(start_ + i) % kCapacity]; }

    // Overwrite the colour of the point BEFORE the newest one (value, time and entries
    // untouched). Used to stamp "the step whose END is the newest point" onto the point it
    // starts from, which is only known once that next point arrives. False when size < 2.
    bool SetColorOfPrevNewest(const std::string& colorHex);

    // Overwrite the colour of the NEWEST point (value, time and entries untouched).
    // ★ Why this exists (owner, 2026-09-19: "氛围红了，下一段冒出来的曲线也应该红"): the
    //   newest point's segment runs to the NEXT point, which does not exist yet -- so under
    //   pure backfill that segment stays colourless and the renderer borrows the colour of
    //   the point to its RIGHT (renderer.cpp), i.e. an OLD colour. The ambience, meanwhile,
    //   turns red the same instant the step is measured. Result: red glow, blue curve.
    //   Writing the newest point's colour with the step we just measured makes the newest
    //   segment speak in the same instant as the glow; the next point arriving overwrites
    //   it with the real step's colour (SetColorOfPrevNewest), so the estimate never sticks.
    // False when the store is empty.
    bool SetColorOfNewest(const std::string& colorHex);

    // The newest n points, oldest -> newest; fewer when the store holds fewer.
    // ★ COPIES n points, not the whole ring: this is what the 12-point display slice
    //   asks for.
    std::vector<CurveStorePoint> Newest(std::size_t n) const;

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
    // On the heap: at kCapacity the slots are ~620 KB and a stack frame is 1 MB, so an
    // inline array overflows the stack as soon as two stores share a scope (see §2.2).
    std::vector<CurveStorePoint> buf_;
    std::size_t start_ = 0;   // slot holding the oldest live point, ALWAYS < kCapacity
    std::size_t count_ = 0;   // live points (never exceeds kCapacity)
    int64_t updateAt_ = 0;    // §2.4 "update_at": the last update, in seconds
    bool updateAtValid_ = false;
    std::string lastPrimaryText_;      // verbatim text of the newest point's value
    std::string lastPrimaryCurrency_;  // which currency that value belongs to
    std::string clearReason_;          // "stale" / "no timestamp" / empty
};

// 探针时钟夹具（见 curve_store.cpp 里的说明）：把"现在"钉在一个固定值。
// ★ 为什么需要它：这台时钟直接决定一个存储**算不算过期**（kExpirySeconds = 24 h）。
//   夹具若把点和时间钉死在某个绝对值，真实时钟走过 24 小时之后**整个存储过期、
//   点全被丢掉**，于是"今天没有带时间的点"→ 断言从某一刻起永远为红，代码却一个字没改。
//   0 = 用真实时钟（默认）；**只有探针会调它**，生产路径不调。
void SetCurveStoreNowForProbe(int64_t nowSeconds);

}  // namespace dshb
