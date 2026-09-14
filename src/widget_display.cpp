#include "widget_display.h"

#include "amount.h"

#include <cmath>

namespace dshb {

void DisplayedAmount::OnSample(const Sample& s) {
    if (!s.amountsOk) return;                 // 读不到的样本不参与显示

    const double yuan = s.total.ToDouble();
    latest_ = yuan;

    // 余额为 0 需要连续两次采样确认：防止瞬时 0 把整个界面闪成灰白
    if (s.total.raw == 0) {
        if (zeroPending_) {
            zeroConfirmed_ = true;
        } else {
            zeroPending_ = true;
            return;                            // 这一次不采用，等下一次确认
        }
    } else {
        zeroPending_ = false;
        zeroConfirmed_ = false;
    }

    target_ = yuan;
    if (!hasValue_) {
        // 第一次拿到值就直接落位，不要从 0 滚上去——那会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
    }
}

double DisplayedAmount::Update(double dtSeconds) {
    if (!hasValue_) return 0.0;

    // 指数逼近：每帧走掉剩余差距的一部分。步长与 dt 挂钩，所以帧率变化不影响速度。
    const double diff = target_ - value_;
    if (std::fabs(diff) <= snapYuan) {
        value_ = target_;                      // 吸附：否则永远停在 99.9997
        return value_;
    }
    const double step = 1.0 - std::exp(-dtSeconds / tau);
    value_ += diff * step;
    return value_;
}

const wchar_t* StatusTextFor(ConnState state) {
    switch (state) {
    case ConnState::ColdStart: return L"正在读取";
    case ConnState::Ok: return L"deepseek 余额";
    case ConnState::NoKey: return L"未找到 DEEPSEEK_API_KEY";
    case ConnState::AuthFailed: return L"API Key 无效";
    case ConnState::Exhausted: return L"余额已耗尽，请充值";
    case ConnState::RateLimited: return L"请求过于频繁";
    case ConnState::NetworkError: return L"无法连接";
    case ConnState::Stale: return L"数据已过期";
    case ConnState::Unavailable: return L"账户不可用";
    default: return L"deepseek 余额";
    }
}

WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol) {
    WidgetFrame f{};
    f.state = state;
    f.statusText = StatusTextFor(state);

    // ★ 这里就是设计要防的第一个错：**"查不到"和"余额为 0"必须分开**。
    //   只有拿到了真实数值才显示数字；读不到时显示占位符，绝不显示 0.00。
    const bool haveNumber = amount.hasValue() && currencyKnown;
    f.showAmount = haveNumber;
    if (haveNumber) {
        Amount a{static_cast<AmountRaw>(std::llround(amount.value() * kUnitsPerYuan))};
        f.amountText = a.ToString2();
        f.currencySymbol = currencySymbol ? currencySymbol : L"";
    } else {
        f.amountText = "--.--";                // 占位符，不是 0.00
        f.currencySymbol = L"";
    }

    // 清零预估：C9 才实现。现在明确留空，不编一个假的。
    f.zeroTimeText.clear();
    return f;
}

}  // namespace dshb
