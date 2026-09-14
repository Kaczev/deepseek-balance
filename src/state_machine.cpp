#include "state_machine.h"

namespace dshb {

const wchar_t* ConnStateName(ConnState s) {
    switch (s) {
    case ConnState::ColdStart: return L"正在读取";
    case ConnState::Ok: return L"正常";
    case ConnState::NoKey: return L"未设置密钥";
    case ConnState::AuthFailed: return L"密钥无效";
    case ConnState::Exhausted: return L"余额已耗尽";
    case ConnState::RateLimited: return L"请求过于频繁";
    case ConnState::NetworkError: return L"无法连接";
    case ConnState::Stale: return L"数据已过期";
    case ConnState::Unavailable: return L"账户不可用";
    default: return L"?";
    }
}

void StateMachine::OnSample(const Sample& s, int64_t nowWallMs) {
    (void)nowWallMs;

    if (s.transportOk && s.httpStatus == 200) {
        // 拿到响应：先看账户是否可用，再看金额有没有解析出来
        if (!s.isAvailable) {
            lastOutcome_ = ConnState::Unavailable;
        } else if (!s.amountsOk) {
            // 响应来了但金额读不出：**不是 0**。按"读不到"处理，继续用上一条好数据。
            lastOutcome_ = ConnState::NetworkError;
        } else {
            lastOutcome_ = ConnState::Ok;
            lastGood_ = s;
            hasGood_ = true;
            samples_.push_back(s);
            if (samples_.size() > 512) samples_.erase(samples_.begin());
        }
        return;
    }

    switch (s.httpStatus) {
    case 401:
    case 403: lastOutcome_ = ConnState::AuthFailed; return;
    case 402: lastOutcome_ = ConnState::Exhausted; return;
    case 429: lastOutcome_ = ConnState::RateLimited; return;
    default:  lastOutcome_ = ConnState::NetworkError; return;
    }
}

ConnState StateMachine::Evaluate(int64_t nowWallMs) const {
    if (noKey_) return ConnState::NoKey;
    if (!hasGood_) {
        // 还没有好数据：把"读不到的原因"如实报出去，而不是笼统的"正在读取"
        if (lastOutcome_ == ConnState::ColdStart) return ConnState::ColdStart;
        return lastOutcome_;
    }

    // 有旧数据时：先看它是否过期（过期优先于"上次失败"——数据旧了，
    // 界面就该说"数据已过期"，而不是继续显示一个看不出是旧的数字）
    const int64_t age = nowWallMs - lastGood_.wallMs;
    if (age > kStaleAfterMs) return ConnState::Stale;

    if (lastOutcome_ == ConnState::Ok) return ConnState::Ok;
    return lastOutcome_;
}

}  // namespace dshb
