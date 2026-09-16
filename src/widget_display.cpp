#include "widget_display.h"

#include "amount.h"        // Amount / ParseAmount：纵坐标从十进制原文解析，不用二进制浮点
#include "curve_store.h"

#include <windows.h>   // WideCharToMultiByte（把宽路径转成数据层要的 UTF-8）   // CurveStore：12 点环形、只记变化、curve.json（规格 §2）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace dshb {
namespace {

// ===========================================================================
// 氛围曲线的显示状态（规格 §3）
// ===========================================================================
// 数据层（curve_store.h）已经定死了"记什么"：只有值变了才追加一个点、容量 12、
// 超过 86400 秒作废。这一层只管**画**：取最新 11 个点铺满恰好 10 段；新点进来时
// 整条在 10 秒内匀速左移一格；纵坐标按 P = L + (N − L)(1 − rate^k)^c 缓动。
//
// ★ 全部状态 = 存储内容 + 滚动计时器（"旧极值"本身也是存储内容的纯函数）。
//   没有任何"每帧自乘的增量状态"，所以第 k 帧可以单独构造、单独导出、单独量。
CurveStore g_curveStore;

// 滚动计时器（秒）：追加一个点就归零，之后由每帧的 dt 推进（DisplayedAmount::Update）。
double g_curveSeconds = 0.0;

// 是否有一个还没走完的滚动。走到 kCurveScrollSeconds **之后**（严格大于）才退休：
// 于是 elapsed == 10.000 s 那一帧仍是滚动的终点帧、10.001 s 那一帧是"没有滚动"的
// 静止帧——验收项 2 正是拿这两帧比像素（两条不同的代码路径必须给出同一张图）。
bool g_curveScrolling = false;

// --curve-frame=k：把计时器冻结在 k/60 秒（导帧口子，只影响导出路径的一帧）。
bool g_curveFrozen = false;

// L 用的"旧极值"（规格 §3 第 6 条）：追加发生**之前**屏幕上那 11 个点的极值。
// ★ 它其实也能从存储推出来（追加后环里最老的那些点就是刚才在看的点），但规格明确
//   要求"记住上一次的极值"；记住之后，滚动当中再来一次追加也不会把 L 算错。
double g_curveOldLo = 0.0;
double g_curveOldHi = 0.0;
bool g_curveOldValid = false;

// §3：显示最新 11 个点、恰好 10 段。
constexpr std::size_t kCurveDisplayPoints = 11;
constexpr std::size_t kCurveSegments = kCurveDisplayPoints - 1;

// 一个点对曲线的取值 = 这个点的**第一个可用条目**。
// ★ 为什么不是"当前显示的币种"：数据层判定"变没变"用的就是响应第一个条目
//   （primary）——一个点之所以存在，正是因为那个币种变了。让它与点一一对应，
//   曲线画的就是"数据层记下的那条序列"，与用户此刻点了哪个币种符号无关。
//   条目为 null（该次响应没有这个币种）时跳过，往后找第一个有值的。
bool CurveValueOf(const CurveStorePoint& point, double* out) {
    for (const CurveStorePoint::Entry& entry : point.entries) {
        if (entry.missing || entry.text.empty()) continue;
        Amount amount;
        if (!ParseAmount(entry.text, &amount)) continue;
        *out = amount.ToDouble();
        return true;
    }
    return false;
}

// 全部点的取值（oldest -> newest）。某个点读不出值时用相邻点的值补上，横向几何
// （一格一个点）才不会塌。数据层保证追加进来的点都有可用的第一个条目，所以这只有
// 在手工编辑过 curve.json 时才会发生。
bool CurveValues(const std::vector<CurveStorePoint>& points, std::vector<double>* out) {
    out->assign(points.size(), 0.0);
    std::vector<char> have(points.size(), 0);
    bool any = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        double v = 0.0;
        if (CurveValueOf(points[i], &v)) {
            (*out)[i] = v;
            have[i] = 1;
            any = true;
        }
    }
    if (!any) return false;
    double last = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {          // 向后填
        if (have[i]) last = (*out)[i];
        else (*out)[i] = last;
    }
    for (std::size_t i = points.size(); i-- > 0;) {            // 再向前填（头部空洞）
        if (have[i]) last = (*out)[i];
        else (*out)[i] = last;
    }
    return true;
}

// 窗口内极值（规格 §3：刻度按极值铺满整条带子，不做最小跨度保护）。
struct CurveSpan {
    double lo = 0.0;
    double hi = 0.0;
    bool degenerate() const { return !(hi > lo); }
};

CurveSpan SpanOf(const std::vector<double>& values, std::size_t begin, std::size_t end) {
    CurveSpan span;
    span.lo = values[begin];
    span.hi = values[begin];
    for (std::size_t i = begin; i < end; ++i) {
        if (values[i] < span.lo) span.lo = values[i];
        if (values[i] > span.hi) span.hi = values[i];
    }
    return span;
}

// 归一化纵坐标：0 = 带子顶、1 = 带子底。极值相同（或只有一个点）-> 带子正中。
double NormY(double v, const CurveSpan& span) {
    if (span.degenerate()) return 0.5;
    return 1.0 - (v - span.lo) / (span.hi - span.lo);
}

// 槽位 -> 归一化横坐标。★ 两条路径（滚动 / 静止）必须用同一个式子，同一槽位要给出
// 逐位相同的 float，否则"滚动终点帧 == 静止帧"的逐像素比较会败在最后一位的舍入上。
float SlotX(double slot) { return static_cast<float>(slot * 0.1); }

// 纵向缓动：P = L + (N − L) × (1 − rate^k)^c（规格 §3 第 6 条，k = 帧号）。
// ★ 走到终点（第 600 帧，整 10 秒）时直接取 N：公式在 k=600 处还剩 0.975^600 ≈ 2.5e-7
//   的残量（折算约 7e-5 像素），而"动画结束在数据自己给出的位置上"正是验收项 2 要逐
//   像素比的东西，所以终点取精确值。这不是改公式：10 秒之后本来就没有动画了。
double EasedY(double L, double N, double seconds) {
    if (seconds >= kCurveScrollSeconds) return N;
    const double k = seconds * kCurveFrameHz;
    const double decay = std::pow(static_cast<double>(kCurveRollRate), k);
    return L + (N - L) * std::pow(1.0 - decay, static_cast<double>(kCurveRollC));
}

// 环里两个快照是不是同一批点（逐条目比币种/缺失/原文）。
bool SamePoints(const std::vector<CurveStorePoint>& a, const std::vector<CurveStorePoint>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].entries.size() != b[i].entries.size()) return false;
        for (std::size_t j = 0; j < a[i].entries.size(); ++j) {
            if (a[i].entries[j].currency != b[i].entries[j].currency) return false;
            if (a[i].entries[j].missing != b[i].entries[j].missing) return false;
            if (a[i].entries[j].text != b[i].entries[j].text) return false;
        }
    }
    return true;
}

// 一条取样喂给数据层（§2.1 / §2.3 的规则全在那里），并观察"是不是追加了一个点"。
// 只有追加才启动滚动——值不变时曲线**静止**（规格 §2.1 的推论，这条是刻意的）。
// 记录文件的路径（main 启动时给一次）。为空 = 不落盘（导帧路径就是这样）。

void FeedCurve(const Sample& s) {
    std::vector<CurveStorePoint> before = g_curveStore.Points();
    std::vector<double> beforeValues;
    const bool beforeOk = CurveValues(before, &beforeValues);

    CurveObservation obs;
    // §2.1：判定"变没变"用**响应第一个条目**那个币种。
    obs.primaryCurrency = !s.entries.empty() ? s.entries.front().currency : s.currency;
    for (const CurrencyAmount& e : s.entries) {
        obs.observations.push_back(CurveObservation::Item{
            e.currency, e.ok ? e.total.ToString2() : std::string(), e.ok});
    }
    if (obs.observations.empty()) {
        if (!s.amountsOk || obs.primaryCurrency.empty()) return;   // 没有币种信息，没什么可记
        obs.observations.push_back(CurveObservation::Item{
            obs.primaryCurrency, s.total.ToString2(), true});
    }
    // 样本的**墙钟秒**就是数据层的时间戳（规格 §2.3）。
    g_curveStore.Append(obs, s.wallMs / 1000);

    // ★ 判定"追加了一个点"看的是**存储自己的状态**，不是 Append() 返回值的含义：
    //   点数变了，或者环里的内容变了（容量 12 到顶时 size() 不动，但最老的点会被挤掉）。
    //   只比 size() 会在环满之后永远看不到新点（那时曲线会在生产里停住不滚）。
    const std::vector<CurveStorePoint> after = g_curveStore.Points();
    const bool appended = !SamePoints(before, after);
    if (!appended) return;

    // 落盘：只在"真的追加了一个点"之后写（12 个点，代价极小；崩溃最多丢最后一次）
    const std::string& cp = dshb::CurveStorePath();
    if (!cp.empty()) (void)g_curveStore.Save(cp);

    // 追加了：计时器归零，并记下"旧极值"= 刚才屏幕上那 11 个点的极值（规格 §3 第 6 条）。
    // 新点入场时也按这套旧极值算 L，于是它不会凭空跳进来，而是从旧刻度滑过去。
    const std::size_t keep =
        (before.size() < kCurveDisplayPoints) ? before.size() : kCurveDisplayPoints;
    g_curveOldValid = beforeOk && keep > 0;
    if (g_curveOldValid) {
        const CurveSpan old = SpanOf(beforeValues, before.size() - keep, before.size());
        g_curveOldLo = old.lo;
        g_curveOldHi = old.hi;
    }
    g_curveSeconds = 0.0;
    g_curveScrolling = true;
}

// 每帧推进滚动计时器（规格 §3：计时器由帧 dt 推进，追加时归零）。
void AdvanceCurve(double dtSeconds) {
    if (g_curveFrozen || !g_curveScrolling) return;
    g_curveSeconds += dtSeconds;
    if (g_curveSeconds > kCurveScrollSeconds) {
        g_curveSeconds = kCurveScrollSeconds;
        g_curveScrolling = false;   // 走完了：显示集就是最新 11 个，不再有"第 12 个"
    }
}

// 组装一帧的曲线点（规格 §3）。全部是"存储内容 + 计时器"的纯函数。
void BuildFrameCurve(WidgetFrame* f) {
    f->curve.clear();
    f->curveHasData = false;

    const std::vector<CurveStorePoint> points = g_curveStore.Points();
    const std::size_t n = points.size();
    if (n == 0) return;                         // 没有数据：渲染层画带子正中的平线

    std::vector<double> values;
    if (!CurveValues(points, &values)) return;

    const bool scrolling = g_curveScrolling;
    const double seconds = g_curveSeconds;
    const double progress =
        (seconds >= kCurveScrollSeconds) ? 1.0 : (seconds / kCurveScrollSeconds);

    // 滚动结束后留下的那些点（规格 §3 第 6 条：N 用"留下的点"的极值）——静止时
    // 它就是显示集本身（最新 11 个）。
    const std::size_t shownBegin = (n > kCurveDisplayPoints) ? (n - kCurveDisplayPoints) : 0;
    const CurveSpan newSpan = SpanOf(values, shownBegin, n);
    const CurveSpan oldSpan =
        (scrolling && g_curveOldValid) ? CurveSpan{g_curveOldLo, g_curveOldHi} : newSpan;

    // 画哪些点、各自在哪个槽位：
    //   静止：最新 11 个，槽位 0..10（最老在左、最新在右边缘）。
    //   滚动：环里全部点（最多 12 个），槽位再右移 0.1×(1−进度)——于是"第 12 个"
    //         从 x=1.1 进来、整条以**线性**进度左移一格，走完时正好落在"最新 11 个"。
    const double slotBase = static_cast<double>(n) - 1.0 - static_cast<double>(kCurveSegments);
    const std::size_t first = scrolling ? 0 : shownBegin;

    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(n + 1);
    ys.reserve(n + 1);
    for (std::size_t p = first; p < n; ++p) {
        const double slot = static_cast<double>(p) - slotBase;
        double x = SlotX(slot);
        if (scrolling) x += 0.1 * (1.0 - progress);
        const double y = scrolling ? EasedY(NormY(values[p], oldSpan), NormY(values[p], newSpan),
                                            seconds)
                                   : NormY(values[p], newSpan);
        xs.push_back(x);
        ys.push_back(y);
    }
    if (xs.size() < 2) return;                  // 一个点：渲染层画平线（curveHasData = false）

    // 横向裁剪到 [0,1]（规格 §3）：两端的点只要"还有一段在画面里"就留着——单调三次只在
    // [0,1] 上取值，越界的部分自然画不出来（渲染层按 u∈[0,1] 采样）。完全在画面外、
    // 连相邻那一段都挤不进来的点直接丢掉：否则它会通过切线影响画面内那一段的形状，
    // 于是"滚动终点帧"与"静止帧"就不再逐像素相同了。
    std::size_t from = 0;
    while (from + 1 < xs.size() && xs[from + 1] <= 0.0) ++from;
    std::size_t to = xs.size();
    while (to > 1 && xs[to - 2] >= 1.0) --to;
    if (to <= from) return;

    // 左侧拉平（规格 §2.2、验收 3）：显示的点还不够 11 个时，最早那个点左边的区间
    // "看做与它同值"。滚动中同理——最左那个点滑出画面后，左端由次左点拉平。
    if (xs[from] > 0.0) f->curve.push_back({0.0f, static_cast<float>(ys[from])});
    for (std::size_t i = from; i < to; ++i) {
        f->curve.push_back({static_cast<float>(xs[i]), static_cast<float>(ys[i])});
    }
    // 极值两边都退化（没有数据 / 只有一个点 / 全都一样）= 平线，按老规矩交给渲染层画。
    f->curveHasData = (f->curve.size() >= 2) && !(newSpan.degenerate() && oldSpan.degenerate());
}

// ---------------------------------------------------------------------------
// 数字那一侧用的小工具
// ---------------------------------------------------------------------------

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
    lastSampleWallMs_ = s.wallMs;   // 曲线横轴锚点（见 widget_display.h）

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
    // 氛围曲线（规格 §2/§3）：每次取到样本都喂给曲线存储；只有**追加了点**才滚动。
    FeedCurve(s);
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
// 曲线的滚动计时器也在这里推进（规格 §3：由帧 dt 推进，追加时归零）。
double DisplayedAmount::Update(double dtSeconds) {
    AdvanceCurve(dtSeconds);
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


// --history-demo=N：合成 N 条样本喂进**曲线存储**（一个进程只画一帧，导帧路径里没有
// 真实历史，所以历史必须能合成）。走的是真实那条路（Sample -> FeedCurve -> Append），
// 所以"只记变化"照旧生效：值必须两两不同才会真的留下 N 个点。
//
// ★ 这 12 个值是为验收测量定的形状（不是随便一条线）：
//   * p0 = 21.00 是"追加前那 11 个点"的上极值，滚动一格后它被挤出缓冲
//     -> 旧极值 [19.90,21.00]、新极值 [19.90,20.20]，极值真的变了，纵向缓动有活干；
//   * p9 = 19.90 是**两套极值下的同一个最小值**，所以它自己的 y 从头到尾不动，
//     是量像素时可跟踪的特征；它的横坐标是 363.5 -> 332 px（在数字右侧那块
//     **没有文字压着**的区域里：数字盖住了带子中间一段，这是画法本身决定的）；
//   * 与它相邻的 p8/p10 都比它高 5 px 以上，所以"最低那一行墨"只属于它一个点；
//   * 其余各点都落在两套极值之内，没有谁会飞出带子（p0 例外：它是被删掉的那个
//     极值，它的纵向目标是"新极值之外"，按规格第 6 条本来就会飞出带子；它只在前
//     0.6 秒里还留在画面边缘，整段行程里它只动了 0.6 px）。
//   （--fixed-amount=20.12 配上它，屏幕上的数字正好等于最新那个点。）
void PrimeHistoryForDemo(int points) {
    if (points < 1) return;
    static const double kDemo[12] = {21.00, 20.20, 20.19, 20.18, 20.17, 20.16,
                                     20.15, 20.14, 20.13, 19.90, 20.10, 20.12};
    const int want = points;
    const int table = (want < 12) ? want : 12;   // <12 时取表尾那几个
    const int prefix = want - table;             // >12 时多喂的填充点，会被环挤掉
    const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));

    for (int i = 0; i < want; ++i) {
        const double v = (i < prefix) ? (21.00 + 0.01 * i) : kDemo[12 - table + (i - prefix)];
        dshb::Sample s{};
        s.wallMs = (nowSec - (want - 1 - i)) * 1000;   // 一点一秒，最老的最早
        s.amountsOk = true;
        s.currency = "CNY";
        s.total = dshb::Amount::FromYuanDouble(v);
        dshb::CurrencyAmount e{};
        e.currency = "CNY";
        e.total = s.total;
        e.ok = true;
        s.entries.push_back(e);
        FeedCurve(s);
    }
}

// --curve-frame=k：把滚动计时器冻结在 k/60 秒（导帧口子）。
void SetCurveScrollFrame(int frame) {
    g_curveFrozen = true;
    g_curveSeconds = (frame > 0) ? (static_cast<double>(frame) / kCurveFrameHz) : 0.0;
}

// 一行诊断给 main 记日志（这一层自己不写文件）。
std::string CurveStateLine() {
    std::string values;
    const std::vector<CurveStorePoint> points = g_curveStore.Points();
    std::vector<double> v;
    if (CurveValues(points, &v)) {
        char one[24];
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::snprintf(one, sizeof(one), "%s%.2f", (i == 0) ? "" : ",", v[i]);
            values += one;
        }
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "points=%zu lastUpdate=%lld scrollFrame=%.0f progress=%.4f scrolling=%s frozen=%s old=[%.2f,%.2f] values=[%s]",
                  points.size(), static_cast<long long>(g_curveStore.lastUpdate()),
                  g_curveSeconds * kCurveFrameHz, g_curveSeconds / kCurveScrollSeconds,
                  g_curveScrolling ? "yes" : "no", g_curveFrozen ? "yes" : "no", g_curveOldLo,
                  g_curveOldHi, values.c_str());
    return buf;
}


// 记录文件路径（函数内 static，避免模块级变量与匿名命名空间的可见性纠缠）
static std::string s_path;   // 记录文件路径（UTF-8）；空 = 不落盘

const std::string& dshb::CurveStorePath() { return s_path; }


void dshb::SetCurveStorePath(const std::wstring& path) {
    s_path.clear();
    if (path.empty()) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string utf8(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    s_path = utf8;
    // 启动即加载：加载失败/文件不存在/数据过期都由数据层自己决定（规格 §2.4 / 验收 8）
    (void)g_curveStore.Load(s_path);
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
    // 氛围曲线（规格 §3）：点由**显示层**算好（横向位置 + 纵向缓动都算完），渲染层
    // 只负责连线。规则（都在本文件上方的 BuildFrameCurve 里）：
    //   * 显示集 = 记录里最新 11 个点铺满恰好 10 段（左侧不足时按最早的点拉平）
    //   * 只有**追加了一个点**才滚动：整条在 10 秒内匀速左移一格，新点从 x=1.1 进来
    //   * 纵向按 P = L + (N − L)(1 − rate^k)^c 缓动，极值仍铺满整条带子
    //   * 值不变 -> 没有新点 -> 曲线静止（规格 §2.1 的推论，故意的）
    //   * 没有数据 / 只有一个点 / 全都一样 -> 平线（curveHasData = false）
    BuildFrameCurve(&f);
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

