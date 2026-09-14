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

    // ★ 目标值直接改写（可以突变），显示值照旧慢慢追——这就是设计里那句
    //   "变量可以突变，但要套一个显示变量，那个显示变量是逐渐变化、跟着那个突变变量的"。
    //   起点值只记来给自检报告"走了多少比例"，不参与计算。
    rollFromValue_ = hasValue_ ? value_ : yuan;

    target_ = yuan;
    if (!hasValue_) {
        // 第一次拿到值就直接落位：从 0 滚上去会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
    }
}

double DisplayedAmount::Update(double dtSeconds) {
    if (!hasValue_) return 0.0;

    const double diff = target_ - value_;
    if (diff == 0.0) return value_;

    // 截断：差到看不见就直接吸附。
    // ★ 指数逼近的尾巴在数值上永远不为 0，但在视觉上早就不动了。
    //   这一条把那段"看不见的尾巴"切掉，最后一位数字才能干脆落定；
    //   没有它，数字会在 99.997 这种地方磨蹭很久（实测过）。
    if (std::fabs(diff) <= snapYuan) {
        value_ = target_;
        return value_;
    }

    // 连续函数：每帧走掉剩余差距的一部分。步长与 dt 挂钩，所以帧率变化不影响手感。
    // 这是**唯一的**运动规律——没有"某次滚动的时长"这个参数。
    const double step = 1.0 - std::exp(-dtSeconds / tau);
    value_ += diff * step;

    // 走完这一步可能刚好越过了截断线，顺手再判一次，避免多花一帧
    if (std::fabs(target_ - value_) <= snapYuan) value_ = target_;
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

std::string DisplayedAmount::TextToShow() const {
    if (!hasValue_) return "--.--";
    // 文本只由目标值决定：滚动期间冻结在目标上，落位后 value_ == target_，
    // 两种情形其实是同一个式子。这样"内容"与"竖直偏移"永远不同时变。
    const double shown = rolling() ? target_ : value_;
    return Amount{static_cast<AmountRaw>(std::llround(shown * kUnitsPerYuan))}.ToString2();
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
        f.amountText = amount.TextToShow();     // 滚动期间是冻结的目标文本
        f.currencySymbol = currencySymbol ? currencySymbol : L"";
    } else {
        f.amountText = "--.--";                // 占位符，不是 0.00
        f.currencySymbol = L"";
    }

    // 清零预估：C9 才实现。现在明确留空，不编一个假的。
    f.zeroTimeText.clear();

    // ★ 逐位里程表：把"这一帧的连续金额"交给渲染层，**任何时刻都要给**。
    //
    //   为什么不再用 rolling() 做条件：轮子现在不只在滚动时用，它**就是**画数字的
    //   唯一路径（滚动结束不再切到另一条静止路径，那正是"结束时跳一行"的来源）。
    //   所以未滚动时也必须给值，否则轮子按 0 算、画面上会变成 00.00。
    //   f.roll.active 保留给"轮子要不要按小数部分偏移"用——落定后它是 0，
    //   轮子自然停在整行上，与静止状态逐像素一致。
    if (haveNumber) {
        f.roll.active = amount.rolling();
        f.roll.amount = amount.value();
    } else {
        f.roll.active = false;
        f.roll.amount = 0.0;
    }
    return f;
}

}  // namespace dshb
