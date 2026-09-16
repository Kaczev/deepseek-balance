// 采样历史（B1，D3 依赖）
//
// 为什么要有它：曲线画的是"过去 10 分钟的余额"，所以必须留下历史。
// 计划把 B1 排在 B 段，但实现里一直没做（D1 之前只有"当前值"），
// D3 要用，就在这里补上。
//
// 设计取舍：
//   * 定容环形缓冲，不动态增长——曲线只要 10 分钟窗口，堆增长没有意义
//     （长时间运行也不会越占越多）。
//   * 存的是**按币种摊平**的点和金额的十进制原值（raw），不存二进制浮点。
//   * 查询按币种过滤：界面一次只显示一个币种，曲线必须跟着选中的那个走。

#pragma once

#include "amount.h"
#include "sampling.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dshb {

struct HistoryPoint {
    int64_t wallMs = 0;
    Amount total{};
};

class SampleHistory {
public:
    // 一条采样进来：把它的每个币种条目各存一个点。
    void Add(const Sample& s);

    // 取 [nowMs - windowMs, nowMs] 内该币种的点，按时间升序。
    // maxPoints 用来限制数量：超过就等间隔抽取（曲线不需要每个点都画）。
    std::vector<HistoryPoint> Window(const std::string& currency, int64_t nowMs,
                                     int64_t windowMs, size_t maxPoints) const;

    size_t size() const { return count_; }

private:
    static constexpr size_t kCapacity = 4096;

    struct Entry {
        int64_t wallMs = 0;
        Amount total{};
        std::string currency;
    };

    Entry buf_[kCapacity]{};
    size_t count_ = 0;   // 已写入的总数（可能大于容量）
    size_t next_ = 0;    // 下一个写入位置
};

}  // namespace dshb
