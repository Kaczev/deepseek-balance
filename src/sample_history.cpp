#include "sample_history.h"

namespace dshb {

void SampleHistory::Add(const Sample& s) {
    if (s.wallMs <= 0) return;

    // 同一时刻同一币种只留一条：单调插值要求 x 严格递增，
    // 重复时间戳会让整条曲线被拒绝（宁可这里挡住，也不要画不出来）。
    if (count_ > 0) {
        const size_t lastIdx = (next_ + kCapacity - 1) % kCapacity;
        if (buf_[lastIdx].wallMs == s.wallMs) return;
    }

    // 一条采样可能有多个币种条目（CNY + USD 并列）；全都留下，
    // 查询时再按币种过滤——这样切换币种也能看到那个币种自己的历史。
    if (!s.entries.empty()) {
        for (const CurrencyAmount& e : s.entries) {
            if (!e.ok) continue;                 // 没解析出来的条目不是 0，绝不能记成 0
            Entry& slot = buf_[next_];
            slot.wallMs = s.wallMs;
            slot.total = e.total;
            slot.currency = e.currency;
            next_ = (next_ + 1) % kCapacity;
            ++count_;
        }
        return;
    }

    // 没有 entries 的老式采样：只记 currency/total 这一条。
    if (!s.amountsOk) return;
    Entry& slot = buf_[next_];
    slot.wallMs = s.wallMs;
    slot.total = s.total;
    slot.currency = s.currency;
    next_ = (next_ + 1) % kCapacity;
    ++count_;
}

std::vector<HistoryPoint> SampleHistory::Window(const std::string& currency, int64_t nowMs,
                                                int64_t windowMs, size_t maxPoints) const {
    std::vector<HistoryPoint> all;

    const size_t live = (count_ < kCapacity) ? count_ : kCapacity;
    const size_t start = (count_ < kCapacity) ? 0 : next_;   // 环形的话从最旧一条开始
    const int64_t from = nowMs - windowMs;

    for (size_t i = 0; i < live; ++i) {
        const Entry& e = buf_[(start + i) % kCapacity];
        if (e.currency != currency) continue;
        if (e.wallMs < from || e.wallMs > nowMs) continue;
        all.push_back(HistoryPoint{e.wallMs, e.total});
    }

    if (maxPoints == 0 || all.size() <= maxPoints) return all;

    // 等间隔抽取：保留首尾，中间均匀取点（曲线看不出差别，但点数可控）。
    std::vector<HistoryPoint> out;
    out.reserve(maxPoints);
    const double step = static_cast<double>(all.size() - 1) / static_cast<double>(maxPoints - 1);
    for (size_t i = 0; i < maxPoints; ++i) {
        const size_t idx = static_cast<size_t>(i * step + 0.5);
        out.push_back(all[idx < all.size() ? idx : all.size() - 1]);
    }
    return out;
}

}  // namespace dshb
