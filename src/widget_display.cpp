#include "widget_display.h"

#include "sample_history.h"

#include <ctime>

namespace {
// 采样历史（D3）：显示层自己记——每一个帧组装点都自动带上曲线，
// 不会再犯"同形代码有两份、只改了一份"那类错。
dshb::SampleHistory g_hist;
}  // namespace

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

// 连续取不到值：回到"没有值"的状态（界面显示 --.--），但**历史不丢**：
// 下一次成功样本会重新落位。所有者：先当作没变，连续 5 次才显示 --.--。
void DisplayedAmount::MarkUnreadable() {
    hasValue_ = false;
    trips_.clear();
    places_.clear();
    animating_ = false;
    tripsDirty_ = true;
}

// 用户点了币种符号：换成清单里的下一个。
// 记住的是**名字**，所以下一次样本里顺序变了也切得对。
void DisplayedAmount::SelectCurrency(const std::string& code) {
    selectedCurrency_ = code;
    currencyShown_ = code;

    // ★ 立刻换掉**实际数字**，不等下一次样本。
    //   理由（实测踩过）：真实账户 10 秒才一个样本、夹具根本不发新样本，
    //   等下去符号和数字都不会动；而且符号只在这里更新才跟得上。
    //   金额取自最近一次样本里的同币种条目，按"新目标"处理 -> 轮子滚过去并停住。
    for (const CurrencyAmount& e : lastEntries_) {
        if (!e.ok || e.currency != code) continue;
        const double yuan = e.total.ToDouble();
        latest_ = yuan;
        if (e.total.raw != 0) {
            zeroPending_ = false;
            zeroConfirmed_ = false;
        }
        rollFromValue_ = hasValue_ ? value_ : yuan;
        lastReal_ = hasValue_ ? target_ : yuan;
        target_ = yuan;          // R：新的实际数字
        frames_ = 0;
        animating_ = true;
        tripsDirty_ = true;
        if (!hasValue_) {
            value_ = yuan;
            hasValue_ = true;
        }
        lastSwitchTarget_ = yuan;    // 回传给 main.cpp 记日志（这一层不能写日志）
        return;
    }
    // ★ 这个币种在最近的样本里**没有数据**：显示 --.--，但符号仍然换成它的
    //   （所有者：没有数据就显示 --.--，符号要变——否则看不出自己在看哪个币种）。
    hasValue_ = false;
    trips_.clear();
    places_.clear();
    animating_ = false;
    tripsDirty_ = true;
    lastSwitchTarget_ = -1.0;
}

std::string DisplayedAmount::NextCurrency() const {
    if (availableCurrencies_.size() < 2) return std::string();
    std::string current = selectedCurrency_.empty() ? currencyShown_ : selectedCurrency_;
    for (size_t i = 0; i < availableCurrencies_.size(); ++i) {
        if (availableCurrencies_[i] == current) {
            return availableCurrencies_[(i + 1) % availableCurrencies_.size()];
        }
    }
    return availableCurrencies_[0];
}

void DisplayedAmount::OnSample(const Sample& s) {
    if (!s.amountsOk) return;                 // 读不到的样本不参与显示

    // ---- 币种选择 ----
    // 清单来自样本；没选中（或选中的这个币种这次没出现）就用接口给的优先条目。
    // 按名字找而不是按下标：接口不保证数组顺序（设计 §2.2）。
    lastEntries_ = s.entries;   // 留着给"点符号切换"用
    availableCurrencies_.clear();
    for (const CurrencyAmount& e : s.entries) {
        if (e.ok) availableCurrencies_.push_back(e.currency);
    }
    // ★ 可切换的币种 = 本次样本里**有数据的** + 我们**知道怎么显示的**（CNY/USD）。
    //   所有者：切到没有数据的币种就显示 --.--，但符号要变。
    //   所以即使账户只有 CNY，也必须能切到 USD（显示 --.-- 加 $）——
    //   否则这个功能在单币种账户上根本看不出效果。
    for (const char* known : {"CNY", "USD"}) {
        bool has = false;
        for (const std::string& c : availableCurrencies_) {
            if (c == known) { has = true; break; }
        }
        if (!has) availableCurrencies_.push_back(known);
    }
    Amount picked = s.total;
    std::string pickedCode = s.currency;
    if (!s.entries.empty()) {
        const CurrencyAmount* hit = nullptr;
        if (!selectedCurrency_.empty()) {
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok && e.currency == selectedCurrency_) { hit = &e; break; }
            }
        }
        if (!hit) {
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok && e.currency == s.currency) { hit = &e; break; }
            }
        }
        if (!hit) {
            for (const CurrencyAmount& e : s.entries) {
                if (e.ok) { hit = &e; break; }
            }
        }
        if (hit) {
            picked = hit->total;
            pickedCode = hit->currency;
        }
    }
    currencyShown_ = pickedCode;

    const double yuan = picked.ToDouble();
    latest_ = yuan;

    // 余额为 0 需要连续两次采样确认：防止瞬时 0 把整个界面闪成灰白
    if (picked.raw == 0) {
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
    frames_ = 0;                               // k = 0：本次变化还没运算过
    animating_ = true;
    tripsDirty_ = true;                        // 下一帧重建每一位的行程
        g_hist.Add(s);   // 历史：氛围曲线要用（本文件上方的 g_hist）
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
// 每一位**自己管自己**地滚。
//
//   每位记着：当前坐标 coord、目标坐标 target（整数）。每帧：
//       剩余 = (target − coord) × rate     // 剩余量每帧乘一次 rate，避免幂运算
//       |剩余| < kRollSnapGrid  -> coord = target（**这一位**自己收尾）
//       否则                    -> coord = target − 剩余
//
//   · 截断是**每位独立判断**的：某一位先到位就先停，不被别的位拖住
//   · 新值到来时只改 target，coord 从当前位置继续走 -> 不会跳
//     （全体共用一个 rate^k 时，中途来新值要重置共用状态，所有轮子被拽回起点，
//       所有者看到的"突变"正是如此；他 rate=0.99 一轮要 7.6 秒，而序列每 3 秒换值）
// 每一位自己管自己地滚。位置公式（所有者给的）：
//
//     coord_n(k) = L + D_n × (1 − rate^k)^c        D_n = floor((R − L)/n)
//
// 实现要点：
//   · 每位自带 ratePower = rate^k，每帧自乘一次（不做幂运算）
//   · 截断**每位独立**：这一位自己的剩余距离 < kRollSnapGrid 就放到位
//   · 新值到来只改终点，起点取"这一位当前坐标" -> 不会跳
//     （全体共用一个 rate^k 时，中途来新值要重置共用状态、所有轮子被拽回起点，
//       所有者看到的"突变"就是这样来的）
//   · D 的取整口径由 kRollDDiffFloor 选：所有者的 floor((R−L)/n)，
//     或 floor(R/n) − floor(L/n)（起点不在整数格上时不会多走一格）
void DisplayedAmount::AdvancePlaces(double dtSeconds, const std::string& amountText) {
    (void)dtSeconds;
    if (!hasValue_ || amountText.empty()) {
        trips_.clear();
        places_.clear();
        return;
    }

    // ★ 欠款（负余额）：动画一律用**绝对值**。不用负值参与动画的原因很实在：
    //   "变小就往下滚"在 0 处会走到 9（磁带环绕），0.00 -> -1.00 会显示成 9.00。
    //   取绝对值后，-1.00 与 +1.00 的轮子行为完全一样，符号交给文本层。
    const double rawL = std::fabs(std::floor(lastReal_ * 100.0 + 0.5) * 100.0);
    const double rawR = std::fabs(std::floor(target_ * 100.0 + 0.5) * 100.0);

    // 重建"有哪些位次"：终点按公式算，起点取这一位的当前位置（连续性）。
    if (tripsDirty_) {
        std::vector<Trip> next;
        // ★ 行程覆盖**全部位次**（+6..−2），不再只覆盖目标文本里那几列。
        //   原因：100.00 -> 0.33 时十位/百位在目标文本里已经不存在了，只按文本建
        //   行程它们就当场消失（所有者要的是"滚到低于 1 才消失"）。
        //   超出的高位坐标为 0 -> 渲染层按"整数位坐标 ≥ 1 才画"隐藏。
        for (int place = 6; place >= -2; --place) {
            const double denom = std::pow(10.0, static_cast<double>(place) + 4.0);
            const double Lg = rawL / denom;   // 起点格（不提前取整）
            // 终点：所有者口径 L + floor((R−L)/n)，或 floor(R/n)
            double endGrid;
            if (kRollDDiffFloor) {
                endGrid = Lg + std::floor((rawR - rawL) / denom);
            } else {
                endGrid = std::floor(rawR / denom);
            }
            Trip t;
            t.place = place;
            bool kept = false;
            for (const Trip& old : trips_) {
                if (old.place == place) {
                    t.from = old.coord;        // 从当前位置继续，不跳
                    // 缓动不沿用：上一次那位已经吸附（ratePower=0），沿用会让它当场跳到新终点
                    t.ratePower = 1.0;
                    kept = true;
                    break;
                }
            }
            if (!kept) {
                t.from = std::floor(Lg);   // ★ 必须是整数格：否则静止时轮子停在两个数字之间（实测踩过）
                t.ratePower = 1.0;
            }
            t.D = endGrid - t.from;
            t.coord = t.from + t.D * std::pow(1.0 - t.ratePower, kRollCurveC);
            next.push_back(t);
        }
        trips_ = next;
        tripsDirty_ = false;
        animating_ = true;
    }

    // 手动模式：按 k 帧直接摆到那一帧的位置（不推进）。
    if (frozen_) {
        places_.clear();
        for (Trip& t : trips_) {
            t.ratePower = 1.0;
            for (int i = 0; i < frames_; ++i) t.ratePower *= kRollRate;
            const double eased = 1.0 - t.ratePower;
            t.coord = t.from + t.D * std::pow(eased, kRollCurveC);
            if (std::fabs(t.D - (t.coord - t.from)) < kRollSnapGrid) t.coord = t.from + t.D;
            places_.push_back(axis::PlaceCoord{t.place, t.coord});
        }
        SyncValueFromTrips();
        return;
    }

    // 正常推进：每位各自收敛、各自截断。
    bool anyMoving = false;
    places_.clear();
    for (Trip& t : trips_) {
        t.ratePower *= kRollRate;                       // rate^k 自乘，避免幂运算
        const double eased = 1.0 - t.ratePower;
        t.coord = t.from + t.D * std::pow(eased, kRollCurveC);
        const double remaining = (t.from + t.D) - t.coord;   // 这一位还差多少格
        if (std::fabs(remaining) < kRollSnapGrid) {
            t.coord = t.from + t.D;                    // 这一位自己到位了
            t.ratePower = 0.0;
        } else {
            anyMoving = true;
        }
        places_.push_back(axis::PlaceCoord{t.place, t.coord});
    }
    animating_ = anyMoving;
    SyncValueFromTrips();
}

// 显示值由**最细那一位**的坐标导出，保证"文本/状态"与"轮子位置"永远一致。
// 没有细位（比如只有整数位）时退回 L + (R−L) 的粗略值。
void DisplayedAmount::SyncValueFromTrips() {
    for (const Trip& t : trips_) {
        if (t.place == -2) { value_ = t.coord / 100.0; return; }
    }
    value_ = target_;
}

// 对外只有一个 Update：先推进显示值，再按行程刷新每一位的坐标。
// 这样"值"和"轮子"永远在同一帧里一起走，调用方不需要记得多调一次。
double DisplayedAmount::Update(double dtSeconds) {
    const double shown = UpdateValue(dtSeconds);
    AdvancePlaces(dtSeconds, TextToShow());
    return shown;
}


// 右上角倒计时：模块级文本。**不做任何动画**——它就是一个每秒变一次的数字。
namespace {
std::wstring& CountdownStorage() {
    static std::wstring s;
    return s;
}
}  // namespace

void SetCountdownText(const wchar_t* text) {
    CountdownStorage() = (text ? text : L"");
}

const wchar_t* CountdownText() { return CountdownStorage().c_str(); }

std::string DisplayedAmount::TextToShow() const {
    if (!hasValue_) return "--.--";
    // 文本只由目标值决定：滚动期间冻结在目标上，落位后 value_ == target_，
    // 两种情形其实是同一个式子。这样"内容"与"竖直偏移"永远不同时变。
    // ★ 文本用**当前动画值**（不再冻结在目标上）：列数随滚动增减，于是高位列
    //   会随滚动出现/消失。字形是按每位坐标画的，文本只负责布局，所以安全。
    const double shown = value_;   // value_ 是**幅值**（动画走绝对值）
    std::string s = Amount{static_cast<AmountRaw>(std::llround(shown * kUnitsPerYuan))}.ToString2();
    // 负号来自**目标值**而不是动画值：否则滚动值穿过 0 的一瞬负号会闪。
    // 幅值本来就为 0 时不加（不出现 "-0.00"）。负号是布局字符，不参与滚动。
    if (target_ < 0.0 && s != "0.00") s.insert(s.begin(), '-');
    return s;
}


// --history-demo=N：合成一段历史（从 20.00 缓慢降到 19.50，最后四分之一不动），
// 供导帧验证。真实历史在导帧路径里不存在（一个进程只画一帧），所以必须能合成。
void PrimeHistoryForDemo(int points) {
    if (points < 2) return;
    const int64_t now = static_cast<int64_t>(std::time(nullptr)) * 1000;
    const int64_t windowMs = 10 * 60 * 1000;
    for (int i = 0; i < points; ++i) {
        dshb::Sample s{};
        s.wallMs = now - windowMs + (windowMs * i) / (points - 1);
        s.amountsOk = true;
        s.currency = "CNY";
        const double u = static_cast<double>(i) / static_cast<double>(points - 1);
        const double v = (u < 0.75) ? (20.00 - 0.50 * (u / 0.75)) : 19.50;
        s.total = dshb::Amount::FromYuanDouble(v);
        dshb::CurrencyAmount e{};
        e.currency = "CNY";
        e.total = s.total;
        e.ok = true;
        s.entries.push_back(e);
        g_hist.Add(s);
    }
}

WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol) {
    WidgetFrame f{};
    f.state = state;
    f.statusText = StatusTextFor(state);
    f.countdownText = CountdownText();

    // ★ 这里就是设计要防的第一个错：**"查不到"和"余额为 0"必须分开**。
    //   只有拿到了真实数值才显示数字；读不到时显示占位符，绝不显示 0.00。
    const bool haveNumber = amount.hasValue() && currencyKnown;
    f.showAmount = haveNumber;
    if (haveNumber) {
        f.amountText = amount.TextToShow();
    } else {
        f.amountText = "--.--";                // 占位符，不是 0.00
    }
    // 氛围曲线（D3）：从采样历史装配归一化点。规则：
    //   * 横向 10 分钟窗口，铺满实体区
    //   * 纵向按**该币种**本窗口内的极值铺满带子 -> 换币种带子位置不变
    //   * 没有数据 / 只有 1 点 / 极值相同 -> 平线（curveHasData = false）
    //   * 两端补"保持点"（值等于端点值）-> 单调三次在等值相邻点切线为 0，
    //     于是曲线**平滑收平成水平**，不会在数据边界出现折角
    {
        const std::string& cur = amount.shownCurrency();
        const int64_t now = static_cast<int64_t>(std::time(nullptr)) * 1000;
        const int64_t winMs = 10 * 60 * 1000;
        std::vector<dshb::HistoryPoint> hp =
            (cur.empty() || !haveNumber) ? std::vector<dshb::HistoryPoint>{}
                                            : g_hist.Window(cur, now, winMs, 120);
        if (hp.size() >= 2) {
            double lo = hp.front().total.ToDouble();
            double hi = lo;
            for (const dshb::HistoryPoint& p : hp) {
                const double v = p.total.ToDouble();
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            const double span = hi - lo;
            if (span > 0.0) {
                f.curve.clear();
                // ★ 保持点只在**严格早于**第一个样本时才补：
                //   两者 x 相同会让单调插值拒绝整条曲线（x 必须严格递增），
                //   表现为曲线变成带子顶部的一条平线（实测踩过）。
                const double xFirst =
                    static_cast<double>(hp.front().wallMs - (now - winMs)) / static_cast<double>(winMs);
                const double xLast =
                    static_cast<double>(hp.back().wallMs - (now - winMs)) / static_cast<double>(winMs);
                if (xFirst > 0.002) {
                    f.curve.push_back({0.0f, static_cast<float>(1.0 - (hp.front().total.ToDouble() - lo) / span)});
                }
                for (const dshb::HistoryPoint& p : hp) {
                    double x = static_cast<double>(p.wallMs - (now - winMs)) / static_cast<double>(winMs);
                    if (x < 0.0) x = 0.0;
                    if (x > 1.0) x = 1.0;
                    const double yn = 1.0 - (p.total.ToDouble() - lo) / span;
                    f.curve.push_back({static_cast<float>(x), static_cast<float>(yn)});
                }
                if (xLast < 0.998) {
                    f.curve.push_back({1.0f, static_cast<float>(1.0 - (hp.back().total.ToDouble() - lo) / span)});
                }
                f.curveHasData = true;
            }
        }
    }
    // ★ 符号与数字**分开决定**（所有者）：只要币种是确定的，即使没有数字也要显示符号，
    //   否则切到没数据的币种时看不出自己在看哪个币种。
    f.currencySymbol = currencyKnown ? (currencySymbol ? currencySymbol : L"") : L"";

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
    // ★ 所有者：欠款与为 0 时不显示这一行。
    //   欠款时说"约 X 小时后归零"是废话（已经欠了），余额为 0 时更荒谬。
    //   判定用**目标值**而不是动画值：动画途中穿过 0 不该让这行字闪。
    const bool canEstimate = haveNumber && amount.target() > 0.0;
    f.zeroTimeText = canEstimate ? "按当前速度，约 X 小时后归零" : "";

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

