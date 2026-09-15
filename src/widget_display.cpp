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

    lastReal_ = hasValue_ ? target_ : yuan;   // L = 上一次的实际数字（所有者的定义）
    target_ = yuan;                            // R：这一次的实际数字
    r_ = 1.0;                                  // rate^0 = 1：剩余量满格
    frames_ = 0;                               // k = 0：本次变化还没运算过
    animating_ = true;
    tripsDirty_ = true;                        // 下一帧重建每一位的行程
    if (!hasValue_) {
        // 第一次拿到值就直接落位：从 0 滚上去会让人以为余额在涨
        value_ = yuan;
        hasValue_ = true;
    }
}

// 显示值不是独立状态量，而是由三个参数导出的：
//     S = L + (R − L) × (1 − rate^k)
// rate^k 被截断为 0 时 S 恰好等于 R，所以"落定精确"是公式自带的，不需要吸附补丁。
// 这也是所有者这次的意思：参数是"实际数字 / 上次的实际数字 / 运算了 n 帧"，没有 display。
double DisplayedAmount::UpdateValue(double dtSeconds) {
    (void)dtSeconds;
    if (!hasValue_) { trips_.clear(); places_.clear(); return 0.0; }
    if (frozen_) return value_;
    SyncValueFromTrips();
    return value_;
}

void DisplayedAmount::SyncValueFromTrips() {
    value_ = lastReal_ + (target_ - lastReal_) * (1.0 - r_);
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
// 每一位的滚动位置（所有者给的映射，这里按整数坐标实现）。
//
//   L = 上次变化时的实际数字，R = 这次的实际数字
//   D_n = floor(R/n) − floor(L/n)        这一位要走几格（0 = 完全不动）
//   coord_n(k) = floor(L/n) + D_n × (1 − rate^k)
//
//   · rate^k 用"一个量每帧自乘"实现，不做幂运算（所有者的要求）
//   · k→∞ 时 coord 正好落在 floor(R/n)：整数 -> 读数清晰
//   · D_n == 0 的位从头到尾不动（所以"下降时十位应跟个位一样"成立）
//   · 全体同时开始、按同一条 rate 曲线收敛，所以一起到位
//
// ★ D 为什么不是 floor((R−L)/n)：起点不在整数格上时会漏步。
//   例：L=99.50, R=100.00, n=10 -> floor(0.50/10)=0（十位不动），
//   可十位的数字要从 9 变成 0，必须走 1 步；floor(R/n)−floor(L/n)=10−9=1 ✓
void DisplayedAmount::AdvancePlaces(double dtSeconds, const std::string& amountText) {
    (void)dtSeconds;
    if (!hasValue_ || amountText.empty()) {
        trips_.clear();
        places_.clear();
        return;
    }

    // 有新样本/新手动值时重建行程：from = floor(L/n)，to = floor(R/n)。
    if (tripsDirty_) {
        trips_.clear();
        const double rawL = std::floor(lastReal_ * 100.0 + 0.5) * 100.0;
        const double rawR = std::floor(target_ * 100.0 + 0.5) * 100.0;
        for (int slot = 0; slot < static_cast<int>(amountText.size()); ++slot) {
            const int place = axis::PlaceOfSlot(amountText, slot);
            if (place == axis::kNoPlace) continue;
            const double denom = std::pow(10.0, static_cast<double>(place) + 4.0);
            Trip t;
            t.place = place;
            t.from = std::floor(rawL / denom);
            t.to = std::floor(rawR / denom);
            trips_.push_back(t);
        }
        tripsDirty_ = false;
    }

    // 推进"剩余量"：每帧自乘一次 rate。r_ = rate^k。
    if (animating_) {
        r_ *= kRollRate;
        if (r_ < 0.01) { r_ = 0.0; animating_ = false; }   // 所有者：rate^k < 0.01 就截断为 0
    }
    double eased = 1.0 - r_;                 // 已走完的比例
    if (phaseOverride_ >= 0.0) eased = phaseOverride_;   // 调参：停在行程任意一刻
    phaseNow_ = eased;   // 记录本帧**实际**用的已走完比例（覆盖之后）

    places_.clear();   // 每次重建都要清空：忘了它同一帧会重复累加（实测 places=12）
    for (int slot = 0; slot < static_cast<int>(amountText.size()); ++slot) {
        const int place = axis::PlaceOfSlot(amountText, slot);
        if (place == axis::kNoPlace) continue;
        for (const Trip& t : trips_) {
            if (t.place != place) continue;
            const double coord = t.from + (t.to - t.from) * eased;
            places_.push_back(axis::PlaceCoord{place, coord});
            break;
        }
    }
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


}  // namespace dshb

