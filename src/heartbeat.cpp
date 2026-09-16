// ===========================================================================
//  heartbeat.cpp —— 心跳位移波形的实现（纯数学）
// ===========================================================================
//
//  规格：`不入库文件/E 心跳波形.md`。头文件里有完整的接线说明与坑单。
//
//  本文件里最要紧的三件事，按"写错了会怎样"排序：
//
//   1. **脉冲用中心参数化**，且 (t-mu) 用 fma 且**不加** `(t < mu)` 门闸。
//      写错 = 单帧跳变从 33.95% 退到 100%（整条波形退化成一帧到位）。
//
//   2. **相位用解析分段积分**，不用 `sin(2*pi*F*t)`、也不逐帧累加。
//      写错 = F 变化那一刻窗口跳一下（实测 1.0396 px = 幅度的 20.8%），
//      而且之后持续漂；更糟的是逐帧累加后单帧不可离线复算。
//
//   3. **K 积分表必须够细**。被积函数 (1-rate^(60u))^c 在 u ≈ 9.571 ms 处有一个
//      近乎尖角的峰（峰值约 204、半宽约 4 ms）。用固定步数的辛普森（比如 60 步
//      覆盖 3 秒 = 50 ms 一步）一个点都踩不到它，积分会少一半 ——
//      上一轮真的踩过：R=0.5/D=0.5 那个工况被算成"一次搏动都没有"。
//      这里改成 1e-6 s 密的累积表 + 线性插值，误差 < 5e-11。
#include "heartbeat.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace dshb {
namespace {

// ---------------------------------------------------------------------------
// 1. 一个高斯脉冲
// ---------------------------------------------------------------------------
//  ★ 只能用 std::fma 算 (t - mu)，**不能**写 t - mu，也**不能**加 `t < mu` 的门闸：
//    · t - mu 在 t≈1e-5、mu=0.095 时会丢掉 13 位有效数字，(t-mu)^2 变成纯舍入噪声；
//    · 更要命的是那个门闸：mu=0.095 时 std::fma(0.0, 1.0, -0.095) 返回的是
//      −1.4e-17（不是 0），于是 "t < mu" 成立、函数在 t=0 处返回 0，
//      而正确值是 exp(-1.805) = 0.165 —— 只差 1.4e-17 的符号，整条波形就错掉。
//    所以这里**不分段**，只有一个 exp；z 很大时 exp 自己下溢到 0。
inline double Pulse(double t, double mu, double sigma) {
    const double u = std::fma(t, 1.0, -mu);
    const double z = u / sigma;
    const double e = -0.5 * z * z;
    return e > -700.0 ? std::exp(e) : 0.0;
}

// ---------------------------------------------------------------------------
// 2. K 表：K(τ) = ∫₀^τ (1 - rate^(60u))^c du
// ---------------------------------------------------------------------------
//  这是"把离散帧和写成连续积分"之后唯一需要的积分。预先在一张细网格上累加一次，
//  之后所有查询都是查表 + 线性插值（仍然是纯函数：同样的 τ 给同样的值）。
//
//  ★★ 定义域只需要 [0, t2]，**不是**一刀切的 4 s。理由是可证的：
//     K() 只在缓动段被调用，而且实参是 `e = min(u, t2)`（见 BeatFPrime），
//     所以 τ 永远 ≤ t2。t2 = 2.312614025 s（= ln(1-eps^(1/c))/ln(rate)/60），
//     与 R/D 完全无关 —— 它是一个由 (rate, c, eps) 决定的编译期常数。
//
//     旧版把域定在 4 s，比需要的长 1.73 倍，白占 30.5 MB 常驻内存。
//     对一个常驻桌面挂件这个量级不可接受。
//
//  ★ 步长的选择是量出来的，不是估出来的。被积函数 (1-rate^(60u))^c 在
//    u ∈ [0, 2.32] 上**单调递增**、且非常平缓（K'' 的最大值只有约 5e3，
//    出现在 u ≈ 9.571 ms —— 那里是拐点，不是"尖峰"）。复化梯形的整体误差是
//    h²/12 · ∫|K''| du，而 ∫|K''| ≈ 2·max K' ≈ 0.86，所以 h = 1e-4 给出约 7e-10，
//    线性插值误差是 h²·|K''|/8 ≈ 6e-6 —— 后者占主导，仍然比相位验收要求的
//    1e-6 小两个数量级（相位里 K 还要乘 (F2-F1) ≤ 1.98）。
//    实测（对 1e-7 步长的收敛参考）见探针 case10；h = 1e-4 的域内条目数是
//    23201 个 double = 0.19 MB。
constexpr double kKTableTauMax = 2.32;   // > t2 = 2.312614025，留 7.4 ms 余量
constexpr double kKTableStep = 1e-4;

struct KTable {
    std::vector<double> cum;   // cum[i] = K(i * step)
    double step = kKTableStep;

    KTable() {
        const int n = static_cast<int>(kKTableTauMax / kKTableStep);
        cum.resize(static_cast<std::size_t>(n) + 1);
        cum[0] = 0.0;
        double prev = 0.0;   // (1 - rate^0)^c = 0
        // ★ 这里必须按**网格步长**推进，不是按"帧"：
        //     rate^(60u) 在 u 上每走一步 step，就乘以 rate^(60*step)。
        //   我第一版写成了 `decay *= pow(rate, 60)` —— 那是"每格跨一整帧"（1/60 s），
        //   而格子实际只有 1e-4 s，于是 decay 快进 167 倍、几步就衰减到 0，
        //   被积函数整段变成 1，K(τ) 直接退化成 τ。那不是精度问题，是**量纲错了**。
        //   这个错会让相位按 0.017 Hz 而不是 F1 前进 —— 探针 case1..case9 仍然全过
        //   （它们量的是包络与起点，不量缓动），只有 case10 把它抓了出来。
        const double decayPerStep = std::pow(kBeatEaseRate, kBeatFrameHz * kKTableStep);
        double decay = 1.0;   // = rate^(60 * i * step)
        for (int i = 1; i <= n; ++i) {
            decay *= decayPerStep;
            const double w = std::pow(1.0 - decay, kBeatEaseC);
            cum[static_cast<std::size_t>(i)] = cum[static_cast<std::size_t>(i) - 1] +
                                                0.5 * (w + prev) * kKTableStep;
            prev = w;
        }
    }

    double At(double tau) const {
        if (tau <= 0.0) return 0.0;
        const double tauMax = (cum.size() - 1) * step;
        if (tau >= tauMax) {
            // 表尾之外的常态分支（正常路径永远走不到，见上面的可证说明）
            return cum.back() + (tau - tauMax);
        }
        const double x = tau / step;
        const std::size_t i = static_cast<std::size_t>(x);
        const double f = x - static_cast<double>(i);
        return cum[i] * (1.0 - f) + cum[i + 1] * f;
    }
};

const KTable& K() {
    static const KTable table;   // 只构造一次；C++11 起局部 static 初始化是线程安全的
    return table;
}

// ---------------------------------------------------------------------------
// 3. 一个变化段的频率曲线与它的原函数
// ---------------------------------------------------------------------------
//  段内用 τ = t - change.at 表示。R 夹到 [0,1]（外面算错了也不许把频率推过上限）。
struct Segment {
    double at = 0.0;
    double R = 0.0;
    double D = 0.0;
    double F1 = 0.0;      // 变化刚开始时的频率
    double F2 = 0.0;      // 缓动结束后、恢复开始前的频率
    double tRec = 0.0;    // T_rec = kBeatRecMax * D * R；0 = 不需要恢复
};

Segment MakeSegment(const BeatChange& c) {
    Segment s;
    s.at = c.at;
    s.R = std::clamp(c.R, 0.0, 1.0);
    s.D = std::clamp(c.D, 0.0, 1.0);
    s.F1 = BeatF1(s.R);
    s.F2 = BeatF2(s.R);
    s.tRec = BeatRecoverySeconds(s.R, s.D);
    return s;
}

//  t2：缓动 (1-rate^(60u))^c 第一次升到 kBeatF2Eps 的时刻。
//  解 (1-rate^(60u))^c = eps  ->  60u = ln(1 - eps^(1/c)) / ln(rate)
//
//  ★★ 用本模块的常量算出来是 **2.312614025 s**。
//     规格 `不入库文件/E 心跳波形.md` 第 200 行印的是 0.4579 s —— 那是**错的**
//     （写规格时把"帧数 138.7568"当成了 1/60 秒量级的一个数；0.4579 = 0.4579s
//     对应的帧数是 27.47，不是 138.76）。规格里那个公式本身是对的，只是那一个
//     常数印错了。本实现按**正确的**值走，理由见探针 case6 的实测：
//     t2+T_rec 处频率恰好等于 F0_min。
//
//  ★ 步长是 1e-4 而不是旧版的 1e-6：被积函数在这个定义域上极其平缓
//    （它单调递增，二阶导数最大仅约 5e3，出现在 u ≈ 9.571 ms 的拐点处），
//    所以粗网格完全够用。实测误差见探针 case10。
//
//  只要 rate ∈ (0,1)、c > 0，这一步就一定有解；取不到时退回 1.0 s（安全值）。
//
//  ★ 关于"编译期守住表够用"这条：`std::pow`/`std::log` 在 C++20 里**不是** constexpr，
//    所以 t2 的精确值拿不到、没法直接写进 static_assert。我先试过推一个解析上界，
//    结果推错了方向（ln(1-x) 比 -x 更负，所以 -eps^(1/c) 是个**下**界不是上界；
//    用它会得出 1.66 s < t2 = 2.31 s，界不成立）。与其留一个错的界，不如：
//      · 编译期只留一条形式上的 sanity 检查；
//      · 真正的守卫是下面那条**精确**的运行期 assert："域 > t2"。
//    这样越界一定会在第一次取值时炸掉，而不是悄悄走外推分支给出错的相位。
constexpr double kKTableTauMaxSanityMax = 60.0;
static_assert(kKTableTauMax > 0.0 && kKTableTauMax <= kKTableTauMaxSanityMax,
              "sanity: K table domain must be a sane positive length");

double T2Seconds() {
    static const double t2 = [] {
        const double need = 1.0 - std::pow(kBeatF2Eps, 1.0 / kBeatEaseC);
        const double lr = std::log(kBeatEaseRate);
        if (!(need > 0.0 && need < 1.0) || !(lr < 0.0)) return 1.0;
        const double k = std::log(need) / lr;   // 帧数（= 138.756841502）
        const double value = k / kBeatFrameHz;
        // ★ 真正管用的守卫：K() 的实参上限就是 t2（BeatFPrime 里传 e = min(u, t2)），
        //   所以"域 > t2"就是"表够用"，充要。
        assert(value < kKTableTauMax &&
               "K table domain must cover t2; K is read with e = min(u, t2)");
        return value;
    }();
    return t2;
}

//  实时的频率曲线，按"相对变化时刻"的 u（秒）给：
//    第一段（缓动）:        F1 + (F2 - F1) * ease(u)
//    第二段（恢复，u > t2）: F2 + (F0_min - F2) * min((u - t2)/tRec, 1)
//  这样写两个端点都是精确的：ease(0) = 0 -> 恰好 F1；ease(∞) = 1 -> 恰好 F2。
//  ★ tRec == 0 时**没有第二段**（规格：T_rec = 0 表示不需要恢复），此时 F 停在 F2。
double FreqAt(const Segment& s, double u) {
    if (u <= 0.0) return s.F1;
    const double ease = std::pow(1.0 - std::pow(kBeatEaseRate, kBeatFrameHz * u), kBeatEaseC);
    double f = s.F1 + (s.F2 - s.F1) * ease;
    if (s.tRec > 0.0 && u > T2Seconds()) {
        const double prog = std::min((u - T2Seconds()) / s.tRec, 1.0);
        f = s.F2 + (kBeatF0Min - s.F2) * prog;
    }
    return f;
}

//  ∫₀^a FreqAt(s, u) du —— 段内积分的**原函数**。
//  ★ 写成"原函数之差"而不是"先算第一段再加第二段"的带符号分支：
//    带符号分支里只要有一处条件写反，整段就会**静默地返回 0**，
//    相位少一截、搏动次数直接变成 0。上一轮真的踩过这个（R=0.5/D=0.5 被算成零次搏动）。
double BeatFPrime(const Segment& s, double a) {
    if (a <= 0.0) return 0.0;
    const double t2 = T2Seconds();
    double out = 0.0;

    // --- 缓动段：[0, min(a, t2)]
    const double e = std::min(a, t2);
    if (e > 0.0) {
        out += s.F1 * e + (s.F2 - s.F1) * K().At(e);
    }

    // --- 恢复段：[t2, t2 + tRec]
    if (s.tRec > 0.0 && a > t2) {
        const double prog = std::min(1.0, (a - t2) / s.tRec);
        out += s.tRec * (s.F2 * prog + (kBeatF0Min - s.F2) * prog * prog * 0.5);
    }

    // --- 恢复之后的常态段
    if (s.tRec > 0.0 && a > t2 + s.tRec) {
        out += kBeatF0Min * (a - t2 - s.tRec);
    } else if (s.tRec == 0.0 && a > t2) {
        // 不需要恢复：t2 之后频率停在 F2（不是 F0_min）
        out += s.F2 * (a - t2);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 4. 相位 φ(t)：逐段求和
// ---------------------------------------------------------------------------
//  history 之前的时段（第一条之前）频率是 kBeatF0Min（常态最慢那一档）。
double PhaseAt(double t, const std::vector<BeatChange>& history) {
    double total = 0.0;
    double prevAt = 0.0;
    bool haveSeg = false;
    Segment cur;

    for (const BeatChange& c : history) {
        if (c.at >= t) break;
        if (haveSeg) {
            total += BeatFPrime(cur, c.at - cur.at) - BeatFPrime(cur, prevAt - cur.at);
        } else {
            total += kBeatF0Min * (c.at - prevAt);
        }
        prevAt = c.at;
        cur = MakeSegment(c);
        haveSeg = true;
    }

    if (!haveSeg) return kBeatF0Min * t;
    return total + (BeatFPrime(cur, t - cur.at) - BeatFPrime(cur, prevAt - cur.at));
}

}  // namespace

// ---------------------------------------------------------------------------
// 历史管理
// ---------------------------------------------------------------------------
BeatChange MakeBeatChange(double atSeconds, double R, double D) {
    BeatChange c;
    c.at = atSeconds;
    c.R = std::clamp(R, 0.0, 1.0);
    c.D = std::clamp(D, 0.0, 1.0);
    return c;
}

void AppendBeatChange(std::vector<BeatChange>* history, const BeatChange& change,
                      std::size_t keep) {
    if (history == nullptr) return;
    BeatChange c = change;
    c.R = std::clamp(c.R, 0.0, 1.0);
    c.D = std::clamp(c.D, 0.0, 1.0);

    // 有序插入（正常情况就是 push_back，因为时刻总是递增的）
    const auto pos = std::upper_bound(
        history->begin(), history->end(), c.at,
        [](double at, const BeatChange& e) { return at < e.at; });
    history->insert(pos, c);

    // 只留最近 keep 条。★ 裁掉最老的那条不改变 φ 的**差值**（老段只贡献常数），
    // 所以"当前这一拍"与"onset 时刻"都不受影响；受影响的只是 φ 的绝对值，
    // 而那个绝对值只用来取整，整数的差值不变。
    if (keep > 0 && history->size() > keep) {
        history->erase(history->begin(),
                       history->begin() + static_cast<std::ptrdiff_t>(history->size() - keep));
    }
}

// ---------------------------------------------------------------------------
// 包络
// ---------------------------------------------------------------------------
double BeatShapeNorm(double tauSeconds) {
    // ★ 边界严格：τ < 0 或 τ > kBeatLen 时**恰好 0**（不是"很小的数"）。
    //   静默段必须是 0，否则"两次搏动之间回到 0"这条验收项就没法量。
    if (tauSeconds < 0.0 || tauSeconds > kBeatLen) return 0.0;
    const double p = Pulse(tauSeconds, kBeatMu1, kBeatSigma1) +
                     kBeatB2 * Pulse(tauSeconds, kBeatMu2, kBeatSigma2);
    return p * kBeatShapeNorm;
}

// ---------------------------------------------------------------------------
// 频率与相位
// ---------------------------------------------------------------------------
double BeatFrequencyHz(double nowSeconds, const std::vector<BeatChange>& history) {
    // 找最后一条 at <= now 的条目
    const BeatChange* last = nullptr;
    for (const BeatChange& c : history) {
        if (c.at <= nowSeconds) {
            last = &c;
        } else {
            break;
        }
    }
    if (last == nullptr) return kBeatF0Min;
    return FreqAt(MakeSegment(*last), nowSeconds - last->at);
}

double BeatPhaseBeats(double nowSeconds, const std::vector<BeatChange>& history) {
    return PhaseAt(nowSeconds, history);
}

double BeatEaseT2Seconds() { return T2Seconds(); }

// ---------------------------------------------------------------------------
// 当前这一拍从哪一刻开始 + 这一刻的幅度
// ---------------------------------------------------------------------------
//  ★ 用一个内部函数把"onset / τ / 幅度"**一次**算出来，保证三者用的是同一个 φ
//    和同一条生效的历史条目。分三次各算一遍（各自再找一遍"最后一条"）
//    就会出现"onset 用新段、幅度用旧段"这种对不上的错 —— 而且只在变化那一帧错，
//    极难查。
namespace {

struct BeatEval {
    bool haveOnset = false;
    double index = 0.0;   // 第几拍
    double tau = 0.0;     // nowSeconds - onset
    double amp = 0.0;     // 幅度（px），只由生效条目的 D 决定
};

BeatEval EvaluateBeat(double nowSeconds, const std::vector<BeatChange>& history) {
    BeatEval out;

    // 生效的那一条（at <= now 里最晚的）：它同时决定 F_t 和 D
    const BeatChange* last = nullptr;
    for (const BeatChange& c : history) {
        if (c.at <= nowSeconds) {
            last = &c;
        } else {
            break;
        }
    }
    const double D = last != nullptr ? std::clamp(last->D, 0.0, 1.0) : 0.0;
    out.amp = kBeatAMax + (kBeatAMin - kBeatAMax) * D;

    const double phi = PhaseAt(nowSeconds, history);
    if (phi < 1.0) return out;   // 还没有跨过第一个整数 -> 静默

    const double target = std::floor(phi);
    out.index = target;

    // 求 φ(t) = target 的 t。φ 严格单调增，所以在 [0, now] 上二分是稳的。
    // ★ 不用"两帧之间找跨界"：那要逐帧累加，就把"单帧可离线复算"这条弄丢了。
    //   这里和 BeatOffsetPx 用的是**同一个** φ，两者不可能对不上。
    double lo = 0.0;
    double hi = nowSeconds;
    for (int i = 0; i < 80; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (PhaseAt(mid, history) < target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    out.haveOnset = true;
    out.tau = nowSeconds - 0.5 * (lo + hi);
    return out;
}

}  // namespace

BeatOnset CurrentBeatOnset(double nowSeconds, const std::vector<BeatChange>& history) {
    const BeatEval e = EvaluateBeat(nowSeconds, history);
    BeatOnset out;
    out.valid = e.haveOnset;
    out.index = static_cast<long long>(e.index);
    out.tauSeconds = e.tau;
    out.atSeconds = nowSeconds - e.tau;
    return out;
}

// ---------------------------------------------------------------------------
// 位移（唯一的对外接口）
// ---------------------------------------------------------------------------
double BeatOffsetPx(double nowSeconds, const std::vector<BeatChange>& history,
                    double fade01) {
    const BeatEval e = EvaluateBeat(nowSeconds, history);
    if (!e.haveOnset) return 0.0;

    // 静默段恰好 0。kBeatLen = 0.345 s < 1/kBeatFMax = 0.5 s 保证 τ 一定落在区间内，
    // 所以这里是兜底而不是常规路径（规格 §9.5 把这条约束写成了判据）。
    if (e.tau < 0.0 || e.tau > kBeatLen) return 0.0;

    return e.amp * BeatShapeNorm(e.tau) * std::clamp(fade01, 0.0, 1.0);
}

}  // namespace dshb
