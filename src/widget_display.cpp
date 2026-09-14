#include "widget_display.h"

#include "amount.h"

#include <cmath>

namespace dshb {

namespace {

// 找出两段文本第一个与最后一个不同的位置。长度不等时退化为"整段都变"。
void DiffSpan(const std::string& a, const std::string& b, int* from, int* to) {
    *from = 0;
    *to = static_cast<int>(b.size()) - 1;
    if (a.size() != b.size()) return;
    int i = 0;
    while (i < static_cast<int>(b.size()) && a[i] == b[i]) ++i;
    if (i == static_cast<int>(b.size())) {   // 完全相同
        *from = 0;
        *to = -1;
        return;
    }
    int j = static_cast<int>(b.size()) - 1;
    while (j >= 0 && a[j] == b[j]) --j;
    *from = i;
    *to = j;
}

}  // namespace

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

    // ★ 逐位滚动：记下起点值/起点文本、目标文本，并重新开始计时。
    //   起点取"当前显示值"，所以动画中途来新样本不会跳——从当前位置接着滚。
    const std::string newText =
        Amount{static_cast<AmountRaw>(std::llround(yuan * kUnitsPerYuan))}.ToString2();

    if (hasValue_) {
        const std::string oldText =
            Amount{static_cast<AmountRaw>(std::llround(value_ * kUnitsPerYuan))}.ToString2();
        rollOldText_ = oldText;
        rollNewText_ = newText;
        rollFromValue_ = value_;
        rollElapsed_ = 0.0;
        rollFraction_ = 0.0;
    } else {
        // 第一次拿到值就直接落位，不要从 0 滚上去——那会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
        rollOldText_.clear();
        rollNewText_.clear();
        rollFraction_ = 1.0;
        rollElapsed_ = rollSeconds;
    }

    target_ = yuan;
}

double DisplayedAmount::Update(double dtSeconds) {
    if (!hasValue_) return 0.0;

    // 固定时长 + 缓动。到点就是到点，不留尾巴，也不再"永远差一点"。
    rollElapsed_ += dtSeconds;
    double t = (rollSeconds > 0.0) ? (rollElapsed_ / rollSeconds) : 1.0;
    if (t >= 1.0) t = 1.0;
    rollFraction_ = t;

    // 缓动：两端慢、中间快。线性会显得发闷，这条像齿轮拨过一格。
    const double e = t * t * (3.0 - 2.0 * t);
    value_ = rollFromValue_ + (target_ - rollFromValue_) * e;

    if (t >= 1.0) value_ = target_;            // 精确落位，不留 99.9997
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

    // 逐位滚动：把"变化的区间"和进度交给渲染层。
    // 只有变化的那几位会滚动，高位不动——这是里程表的样子，也是省掉
    // "整段数字都在动"那种廉价感的关键。
    //
    // ★ 长度不同的两段文本**不能逐位滚**：19.90 -> 100.00 是 5 位对 6 位，
    //   格位对不上。第一版就在这里错了——按长度配对读到了错位的数据，
    //   画面上一直显示旧值（实测就是"数字根本不动"）。长度不同时直接显示新值。
    if (haveNumber && amount.rolling() &&
        amount.rollOldText().size() == amount.rollNewText().size()) {
        f.roll.active = true;
        f.roll.oldText = amount.rollOldText();
        f.roll.newText = amount.rollNewText();
        f.roll.fraction = amount.rollFraction();
        DiffSpan(f.roll.oldText, f.roll.newText, &f.roll.changeFrom, &f.roll.changeTo);
        if (f.roll.changeTo < f.roll.changeFrom) f.roll.active = false;   // 没有实际变化
    }
    return f;
}

}  // namespace dshb
