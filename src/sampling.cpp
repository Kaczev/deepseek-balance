#include "sampling.h"

#include <windows.h>

namespace dshb {

namespace {

// 首次调用时记录一次，之后返回稳定值：单调时钟基准（毫秒，不含休眠）
int64_t MonotonicMs() {
    return static_cast<int64_t>(GetTickCount64());
}

}  // namespace

const wchar_t* ScenarioName(Scenario s) {
    switch (s) {
    case Scenario::Steady: return L"1 平稳消耗";
    case Scenario::FastDrain: return L"2 快速消耗";
    case Scenario::LowBalance: return L"3 余额偏低";
    case Scenario::Recharge: return L"4 充值跳变";
    case Scenario::Zero: return L"5 余额归零";
    case Scenario::Unavailable: return L"6 账户不可用";
    case Scenario::NoNetwork: return L"7 连不上";
    case Scenario::StaleData: return L"8 旧数据";
    case Scenario::ClockJump: return L"9 时钟跳变";
    default: return L"?";
    }
}

void FakeSource::Select(Scenario s) {
    scenario_ = s;
    stepIndex_ = 0;
    produced_ = 0;
    if (s == Scenario::LowBalance) balance_ = Amount::FromYuan(30);
    else if (s == Scenario::Zero) balance_ = Amount::FromYuan(0);
    else if (s == Scenario::Unavailable) balance_ = Amount::FromYuan(50);
    else if (s == Scenario::Recharge) balance_ = Amount::FromYuan(20);
    else balance_ = Amount::FromYuan(100);
    rechargePending_ = false;
    clockJumped_ = false;
}

void FakeSource::TriggerRecharge(double jumpToYuan) {
    rechargePending_ = true;
    rechargeTo_ = jumpToYuan;
}

void FakeSource::TriggerClockJump() {
    clockJumped_ = true;
    // 时钟跳变情形也允许直接按热键触发
    if (scenario_ != Scenario::ClockJump) scenario_ = Scenario::ClockJump;
}

Sample FakeSource::NextIfDue(double nowSeconds) {
    const double now = nowSeconds * speed_;   // speed>1 时内部时间跑得更快
    if (produced_ > 0 && now < nextAt_) return Sample{};

    // 第一拍立刻产生，让"冷启动"只持续一瞬；之后每 10 秒一拍
    nextAt_ = (produced_ == 0) ? (now + 1.0) : (nextAt_ + StepSeconds());
    ++produced_;
    ++stepIndex_;

    Sample s{};
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME 是 1601 年起的 100 纳秒；转成 Unix 毫秒
    s.wallMs = static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
    s.monotonicMs = MonotonicMs();
    // 开机标识：取开机时长的"天"级别的粗值即可，同一开机内恒定
    s.bootId = s.monotonicMs / (24LL * 3600 * 1000);

    if (clockJumped_) {
        // 时钟跳变：墙钟整体前进 8 小时，而单调时钟不动——这正是"时序断层"
        s.wallMs += 8LL * 3600 * 1000;
    }

    switch (scenario_) {
    case Scenario::Steady:
        balance_.raw -= 2000;                        // 慢：每拍约 0.20 元
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;
        break;

    case Scenario::FastDrain:
        balance_.raw -= 15000;                       // 每拍 1.50 元
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;
        break;

    case Scenario::LowBalance:
        // 从 30 元一路降到 5 元以下，然后停在低位
        if (balance_ > Amount::FromYuan(2)) balance_.raw -= 4000;
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;
        break;

    case Scenario::Recharge:
        balance_.raw -= 1000;
        if (rechargePending_) {
            // 跳到指定金额（默认 100）。测试时需要控制"位数是否变化"，
            // 所以这个目标值是可指定的。
            const double cents = rechargeTo_ * 100.0;
            balance_.raw = static_cast<AmountRaw>(cents) * 100;
            rechargePending_ = false;
        }
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;
        break;

    case Scenario::Zero:
        balance_ = Amount::FromYuan(0);
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;                        // 可用但为 0 —— 与"不可用"是两回事
        break;

    case Scenario::Unavailable:
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = false;                       // 权威判据：账户不可用
        break;

    case Scenario::NoNetwork:
        s.transportOk = false;
        s.httpStatus = 0;
        s.note = "transport";
        break;

    case Scenario::StaleData: {
        // 前两拍正常，之后连不上；并把墙钟往回推，制造"数据已过期"
        if (produced_ <= 2) {
            balance_.raw -= 3000;
            s.transportOk = true;
            s.httpStatus = 200;
            s.isAvailable = true;
        } else {
            s.transportOk = false;
            s.httpStatus = 0;
            s.note = "transport";
            s.wallMs -= 8LL * 3600 * 1000;           // 旧数据
            s.monotonicMs -= 8LL * 3600 * 1000;
        }
        break;
    }

    case Scenario::ClockJump:
        balance_.raw -= 500;
        s.transportOk = true;
        s.httpStatus = 200;
        s.isAvailable = true;
        break;

    default:
        break;
    }

    if (balance_.raw < 0) balance_.raw = 0;

    if (s.transportOk && s.httpStatus == 200) {
        s.total = balance_;
        s.granted = Amount::FromYuan(0);
        s.toppedUp = balance_;
        s.amountsOk = true;
        s.currency = "CNY";
    }

    last_ = s;
    return s;
}

}  // namespace dshb
