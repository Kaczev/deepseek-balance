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
    rollStartValue_ = hasValue_ ? value_ : yuan;   // 本次行程的起点金额

    target_ = yuan;
    if (!hasValue_) {
        // 第一次拿到值就直接落位：从 0 滚上去会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
    }
}

double DisplayedAmount::UpdateValue(double dtSeconds) {
    if (!hasValue_) { trips_.clear(); places_.clear(); return 0.0; }
    if (frozen_) return value_;   // 手动冻结：显示值不推进

    const double diff = target_ - value_;
    if (diff == 0.0) return value_;

    // 截断：差到看不见就直接吸附。
    // ★ 指数逼近的尾巴在数值上永远不为 0，但在视觉上早就不动了。
    //   这一条把那段"看不见的尾巴"切掉，最后一位数字才能干脆落定；
    //   没有它，数字会在 99.997 这种地方磨蹭很久（实测过）。
    if (std::fabs(diff) <= kDisplaySnapYuan) {
        value_ = target_;
        return value_;
    }

    // 连续函数：每帧走掉剩余差距的一部分。步长与 dt 挂钩，所以帧率变化不影响手感。
    // 这是**唯一的**运动规律——没有"某次滚动的时长"这个参数。
    const double step = 1.0 - std::exp(-dtSeconds / kDisplayTauSeconds);
    value_ += diff * step;

    // 走完这一步可能刚好越过了截断线，顺手再判一次，避免多花一帧
    if (std::fabs(target_ - value_) <= kDisplaySnapYuan) value_ = target_;
    return value_;
}

// 每一位的纵坐标：目标是 floor(显示值 / 10^位次)，本帧朝目标追赶 dt 秒。
//
// 为什么不是直接令 coord = 显示值 / 10^位次：
//   那样静止时高位永远停在两格之间。实测 99.50 的十位会变成 9.95 -> frac 0.95，
//   整格几乎滚到 0，屏幕上显示成 "09.00"。所以静止必须落在整数上。
//   而"追赶一个只会 ±1 变的目标"既保住了整数落点，又让过程连续（滚动感）。
//
// 文本决定有哪些位次：高位是 0 时文本里根本没有那一位，于是它自动隐藏。
// 每一位自己的行程：轮子只有在自己这一位要变的时候才动。
//
// ★ 为什么不能直接用 S/n 当相位（所有者发现的错）：
//   S=99.10 时十位 S/10=9.91 -> 相位 0.91，看起来"9 快走完了、0 占满"。
//   可 99.10 跌到 99.00 时**十位根本不会变**，轮子就该稳稳停在 9 上。
//   所以相位来自"这一位从哪走到哪"，而不是相对整十的绝对位置。
//
// 行程的起止都取整数（floor），并且全体共用同一个进度 phase，所以：
//   · 静止时 phase==1，每位正好落在自己的数字上（读数清晰）
//   · 只有自己这一位要变的轮子才动，别的纹丝不动
//   · 该动的位同时开始、同时结束
void DisplayedAmount::AdvancePlaces(double dtSeconds, const std::string& amountText) {
    (void)dtSeconds;
    if (!hasValue_ || amountText.empty()) {
        trips_.clear();
        places_.clear();
        return;
    }

    // 全体共用的进度：从本次行程的起点金额走到目标金额，走了多少。
    // 用指数平滑的显示值来算，所以"值到哪儿、轮子就到哪儿"，两者永远同步。
    const double span = rollStartValue_ - target_;
    double phase = 1.0;
    if (std::fabs(span) > 1e-9) {
        phase = (rollStartValue_ - value_) / span;
        if (phase < 0.0) phase = 0.0;
        if (phase > 1.0) phase = 1.0;
    }
    if (phaseOverride_ >= 0.0) phase = phaseOverride_;
    if (value_ == target_ && phaseOverride_ < 0.0) phase = 1.0;   // 落定就是 1，保证对准数字
    const double eased = phase * phase * (3.0 - 2.0 * phase);      // smoothstep：两头慢中间快
    phaseNow_ = phase;

    // 有哪些位次由文本决定（高位是 0 时文本里没有它 -> 自动隐藏）。
    places_.clear();   // 每次重建都要清空：忘了它同一帧会重复累加（实测 places=12）
    std::vector<Trip> next;
    next.reserve(trips_.size());
    for (int slot = 0; slot < static_cast<int>(amountText.size()); ++slot) {
        const int place = axis::PlaceOfSlot(amountText, slot);
        if (place == axis::kNoPlace) continue;
        const double denom = std::pow(10.0, static_cast<double>(place) + 4.0);
        double from = 0.0;
        bool found = false;
        for (const Trip& t : trips_) {
            if (t.place == place) { from = t.from; found = true; break; }
        }
        if (!found) {
            // 这一位是新出现的：起点取"它上一次该在的位置"，即目标之前的那个金额对应的整数坐标。
            const double rawFrom = std::floor(rollStartValue_ * 100.0 + 0.5) * 100.0;
            from = std::floor(rawFrom / denom);
        }
        const double rawTo = std::floor(target_ * 100.0 + 0.5) * 100.0;
        const double to = std::floor(rawTo / denom);
        Trip t;
        t.place = place;
        t.from = from;
        t.to = to;
        next.push_back(t);

        // 这一位的当前坐标：只在自己 from != to 时才动。
        const double coord = from + (to - from) * eased;
        places_.push_back(axis::PlaceCoord{place, coord});
    }
    trips_ = next;
}

// 对外只有一个 Update：先推进显示值，再按行程刷新每一位的坐标。
// 这样"值"和"轮子"永远在同一帧里一起走，调用方不需要记得多调一次。
double DisplayedAmount::Update(double dtSeconds) {
    const double shown = UpdateValue(dtSeconds);
    AdvancePlaces(dtSeconds, TextToShow());
    return shown;
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

    // 每一位的纵坐标交给渲染层。空则渲染层退回整串绘制。
    f.places = amount.places();

    // 清零预估（占位）：**只放文案，不接速率计算**。
    //
    // 所有者当前要的是"把这行字摆上去、好调排版"，所以时间用 X 代替，
    // 不去算一个还没有依据的数字——编一个假的时长比空着更糟（设计 §7.4 的
    // 原则：预估必须带口径，不能给一个没人信的精确值）。
    // 真正的速率估计与四种分支（暂无法预测 / — / 已用尽 / 超过 7 天）等
    // 有了历史样本再按 §7.3 §7.4 接上；接的时候只改这里，渲染层不用动。
    //
    // 文案形态按 §7.4 的两种分支预留：相对时长在前，剩余较短时后面再补一个
    // 绝对时刻（设计原文示例「按当前速度，约 2 小时后归零」+「约 14:32」）。
    // 整行偏长，若排版放不下，先砍掉括号里的绝对时刻。
    f.zeroTimeText = haveNumber ? "按当前速度，约 X 小时后归零" : "";

    // ★ 逐位里程表：把"这一帧的连续金额"交给渲染层，**任何时刻都要给**。
    //
    //   为什么不再用 rolling() 做条件：轮子现在不只在滚动时用，它**就是**画数字的
    //   唯一路径（滚动结束不再切到另一条静止路径，那正是"结束时跳一行"的来源）。
    //   所以未滚动时也必须给值，否则轮子按 0 算、画面上会变成 00.00。
    //   轮子自然停在整行上，与静止状态逐像素一致。
    if (haveNumber) {
    } else {
    }
    return f;
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

std::string DisplayedAmount::PlaceReport() const {
    std::string out;
    char buf[240];
    std::snprintf(buf, sizeof(buf), "value=%.4f target=%.4f start=%.4f frozen=%d places=%d phase=%.3f\n", value_, target_, rollStartValue_,
                  frozen_ ? 1 : 0, static_cast<int>(places_.size()), phaseNow_);
    out += buf;
    out += " place  actualY=S/n      coord     digit  frac     from       to\n";
    for (const axis::PlaceCoord& pc : places_) {
        const double denom = std::pow(10.0, static_cast<double>(pc.place) + 4.0);
        const double actual = std::floor(value_ * 100.0 + 0.5) * 100.0 / denom;
        const int base = static_cast<int>(std::floor(pc.coord));
        const double frac = pc.coord - static_cast<double>(base);
        const int digit = ((base % 10) + 10) % 10;
        double tfrom = 0.0, tto = 0.0;
        bool found = false;
        for (const Trip& tt : trips_) {
            if (tt.place == pc.place) { tfrom = tt.from; tto = tt.to; found = true; break; }
        }
        (void)found;
        std::snprintf(buf, sizeof(buf), " %+4d  %11.4f  %10.4f    %d  %.4f  %8.2f %8.2f\n",
                      pc.place, actual, pc.coord, digit, frac, tfrom, tto);
        out += buf;
    }
    return out;
}

}  // namespace dshb

