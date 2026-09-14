// 连接状态机（B8）
//
// ★ 两条必须分开的东西，混在一起就是设计里第一个要防的错：
//   - **连接状态**（离散、明确）：查到了 / 没网 / Key 不对 / 账户不可用 / 数据过期
//   - **余额数值**（连续）：可以是 0，可以是任意值
//   "余额为 0" 和 "查不到" 在界面上长得一样，是这套设计里最危险的一类 bug：
//   用户会以为没钱了跑去充值，充完发现还是灰的。

#pragma once

#include "sampling.h"

#include <cstdint>
#include <vector>

namespace dshb {

enum class ConnState {
    ColdStart,      // 还没有任何成功采样
    Ok,             // 最新采样成功且新鲜
    NoKey,          // 环境变量缺失
    AuthFailed,     // 401 / 403
    Exhausted,      // 402：官方语义就是"余额不足，请充值"
    RateLimited,    // 429
    NetworkError,   // 连不上/超时
    Stale,          // 有数据但已过期
    Unavailable,    // 接口说账户不可用（is_available=false）
};

const wchar_t* ConnStateName(ConnState s);

// 状态机 + 它依赖的"数据是否新鲜"判断都收在这里，避免各处各算一遍。
class StateMachine {
public:
    // 数据超过这个年龄就算过期：设计里是 2 × T_max，T_max=300s（§5.4）
    static constexpr int64_t kStaleAfterMs = 600 * 1000;

    void SetNoKey(bool noKey) { noKey_ = noKey; }
    bool NoKey() const { return noKey_; }
    void OnSample(const Sample& s, int64_t nowWallMs);

    // 当前状态。nowWallMs 用于判新鲜度。
    ConnState Evaluate(int64_t nowWallMs) const;

    // 最近一次成功的采样（状态机只读，不拥有）
    const Sample& lastGood() const { return lastGood_; }
    bool hasGood() const { return hasGood_; }

    // 已收集的样本（B1 的内存环形缓冲；落盘在 H 阶段接上）
    const std::vector<Sample>& samples() const { return samples_; }

private:
    bool noKey_ = false;
    bool hasGood_ = false;
    Sample lastGood_{};
    ConnState lastOutcome_ = ConnState::ColdStart;
    std::vector<Sample> samples_;
};

}  // namespace dshb
