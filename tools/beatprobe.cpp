// ===========================================================================
//  beatprobe.cpp —— 心跳位移波形、响应曲线与计时规则的离线证明（src/heartbeat.h/.cpp）
// ===========================================================================
//
//  怎么跑：
//      beatprobe             跑全部检查（默认，无参数）
//      beatprobe --verbose   另外打印一段采样波形（一拍之内每 16 个采样点一格）
//
//  输出契约（与 rateprobe / storeprobe 同形）：一行一个检查，
//      PASS: caseN | 标题 | 数字
//  任何一行 FAIL 都让退出码非 0；全过时最后两行是
//      N passed, 0 failed
//      beatprobe: ALL PASS
//  ★ 每条检查都把**判过的数字**印出来（峰位与极值个数、最坏跳变和它出现在哪个相位、
//    四个角、对 D 与对 e(R) 的线性偏差、e(R) 自己的性质、due 序列、触发时刻表、间隔区间、
//    残余量……）。只印 PASS 不印数字的检查，读报告的人只能信、不能读。
//
//  ---------------------------------------------------------------------------
//  ★ 本探针量得到什么
//  ---------------------------------------------------------------------------
//    · src/heartbeat.cpp 的**波形**：包络 Shape(τ)（一拍内恰好一个局部极大、零个局部极小）、
//      峰值恰好在 τ = μ 处为 1.0、τ ∉ [0, kBeatLen] 时**恰好 0**、kBeatLen 处的末端残留；
//    · **响应曲线** e(R) = 1 − (1 − R)^kBeatRPow：e(0) = 0、e(1) = 1、单调递增、
//      R ∈ (0,1) 时 e(R) > R（"同样的 R 比旧式更剧烈"就是这一条）；
//    · **两条律** A(R,D) 与 T(R,D)（R 经过 e(R) 进公式，且 R 那一项被 (1−D) 缩放）：
//      49 点网格对公式、四个角、对 D 线性、对 e(R) 仿射（e 的中点对应中点）、输入夹到 [0,1]；
//    · **计时规则**：把两条律接起来的那条触发判据（在探针里用夹具复刻，见 BeatFixture）。
//
//  ---------------------------------------------------------------------------
//  ★ 本探针量不到什么（别把这份证据读大了）
//  ---------------------------------------------------------------------------
//    · 生产层那个每帧真的在跑的计时循环：它在 src/widget_display.cpp 的
//      AdvanceBeat / TriggerBeatAt 里，而本目标只链 src/heartbeat.cpp。
//      这里只是把**同一条规则**复刻成夹具跑一遍（规则一致，代码不是同一份）；
//      生产循环由主代理用 `--beat-frame` 导帧 + [beat] 日志验证，那是另一条证据。
//      ★ 夹具把 due 锚在"上一次触发时刻 + T"上、**每一帧用当前的 (R,D) 重算**。
//        不能锚在 now 上：写成 due = now + T 时判据 `now >= due` 恒为假（T > 0），
//        第一拍之后永远不再触发。这是本语义唯一容易写反的一处，case5e 专门钉住它。
//    · 窗口位移、命中测试跟着 Δ 平移、SetWindowRgn 的行程 —— 都不在本文件里。
//    · (τ−μ) 到底是不是用 fma 算的：fma 与朴素减法的差别在本参数下只有 1e-16 量级，
//      对形状的影响低于任何可打印的位数（见 case7b 的说明）。探针量得出来的是**门闸**
//      有没有加 —— 那个能把 τ≈0 处整块砍成 0。
//
//  ★ 本探针不读时钟、不写文件、不碰 %LOCALAPPDATA%、不碰 src/ 的任何东西。
//    （更早的一版还读 psapi 的工作集，为的是量那张 0.19 MB 的 K 积分表；新模型里模块
//      连一个 static 都没有、那张表不存在了，工作集读数没有对象可量，所以连同 <psapi.h>
//      一起删掉 —— CMakeLists 不动，psapi 留着不链也不影响。）
//
//  ---------------------------------------------------------------------------
//  检查清单（编号就是下面 case 的编号）
//  ---------------------------------------------------------------------------
//    case0   自检：一条故意失败的检查真的会被格式化成 FAIL 行、真的被计数
//    case1   包络：1 个极大 / 0 个极小、峰值 1.0（τ=μ 处，A_max 下 5 px）、
//            区间外恰好 0（−1e-9 与 kBeatLen+1e-9 两点）、kBeatLen 处的残留
//    case2   单帧最大跳变：1200 起始相位 × 24 帧，占幅度的百分比 ≤ 35%（三种工况）
//    case3   幅度律 A(R,D)：49 点网格对公式、四个角 3/5/1/1（规格）、对 D 线性、
//            对 e(R) 仿射（e 的中点对应中点）、输入夹到 [0,1]
//    case3b  响应曲线 e(R)：e(0)=0、e(1)=1、单调递增、R∈(0,1) 时 e(R)>R、
//            与 kBeatRPow 重算的公式一致、输入夹到 [0,1]
//    case4   周期律 T(R,D)：同上，四个角 15/0.5/30/30（规格）
//    case5a  计时：固定 (R,D) 跑 N 秒，触发次数是**帧量化**的（与理想值最多差 1）
//    case5b  计时：一帧拉成 3T / 7T 只触发一次（不补拍）；时刻严格递增；τ 全程不为负
//    case5c  计时：T 变短 -> 下一拍**提前到**（一帧之内就到），旧语义的对照触发次数也打出来
//    case5d  计时：静默窗口里位移恰好 0；且 T_min > kBeatLen（两拍不叠的前提）
//    case5e  计时：due 每帧都在追 T（due == 触发时刻 + T；非触发帧上 due < now + T）
//    case6   确定性：同一 (now, bucket) 逐位相同、同一初始状态重建逐位相同、
//            位移与绝对时间原点无关
//    case7a  守门：两个中心都在 95 ms（把第一个中心放回 0 -> 首帧跳变 73.5% 的对照）
//    case7b  守门：脉冲不分段、无门闸（朴素门闸写法在 τ→0 返回 0，正确值 0.1645/0.2048）
#include "heartbeat.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using dshb::BeatBucket;
using dshb::BeatOnset;

// 帧长：从模块的 kBeatFrameHz 推，不要自己写 0.0166…（那是把常量抄一份到探针里）。
constexpr double kFrame = 1.0 / dshb::kBeatFrameHz;

// 合成时间原点。位移是 (now − trigger) 的纯函数，换个原点必须给同一个值 —— case6 量这条。
constexpr double kT0 = 1000.0;

// 包络采样率（Hz）。480 Hz 扫 [0, kBeatLen]：165 格、166 个点，最后一点在 343.75 ms，
// 全部落在区间**内部**；比 60 Hz 的渲染网格密 8 倍，足够把"一个极大"数清楚，
// 又不必用解析导数（更早的一版手写解析导数连错两次）。
constexpr double kEnvelopeHz = 480.0;

double Clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// ===========================================================================
// 一个很小的 harness（与 rateprobe 的同一份契约）
// ===========================================================================
struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    // 组装一行但不打印。自检要用它验证 FAIL 行**长什么样**，又不能真打印一行 FAIL：
    // 下游是按"行首是 FAIL"读这份输出的，探针自己印一行会被当成一条真失败。
    std::string Line(const char* id, const std::string& what, const std::string& evidence,
                     bool ok) const {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        return line;
    }

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        const std::string line = Line(id, what, evidence, ok);
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }

    // 同样的记账，不打印：自检用来证明"失败的检查会被计数"而不污染输出。
    bool ReqSilent(bool ok) {
        if (ok) {
            ++passed;
            return true;
        }
        ++failed;
        failures.push_back("silent check failed");
        return false;
    }
};

// ===========================================================================
// 数字与文本小工具
// ===========================================================================
std::string F(double v, int digits) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

std::string I(long long v) { return std::to_string(v); }

// ★ 必须返回 std::string，不能返回 const char*：它总和字符串字面量/其它 std::string 拼在
//   一起用，而 `"字面量" + (const char*)` 在 C++ 里是指针相加 —— 编译不过（更早的一版
//   第一次构建就栽在这里）。
std::string YN(bool v) { return v ? "yes" : "no"; }

// 逐位相同（含 NaN、±0）：确定性检查要求的是"逐位"，不是"差得很小"。
bool BitSame(double a, double b) {
    std::uint64_t ua = 0;
    std::uint64_t ub = 0;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

// 时刻表：少于 head+tail+1 个就全列，否则列头 head 个 + 尾 tail 个。
std::string Times(const std::vector<double>& v, std::size_t head, std::size_t tail) {
    const std::size_t n = v.size();
    const std::size_t h = n < head ? n : head;
    std::string out = "[";
    for (std::size_t i = 0; i < h; ++i) {
        if (i != 0) out += ", ";
        out += F(v[i], 6);
    }
    const bool truncated = n > head + tail + 1;
    const std::size_t t = truncated ? tail : (n - h);
    if (truncated) {
        out += ", ..., 共 " + I(static_cast<long long>(n)) + " 个, ..., ";
    } else if (t != 0) {
        out += ", ";
    }
    for (std::size_t i = n - t; i < n; ++i) {
        if (i != n - t) out += ", ";
        out += F(v[i], 6);
    }
    out += "]";
    return out;
}

// ===========================================================================
// 夹具：把 widget_display.cpp 的计时规则复刻一遍
// ===========================================================================
//  每帧的顺序与生产层（AdvanceBeat + TriggerBeatAt）逐条一致：
//      1. 推进仿真时钟：now += dt
//      2. T = BeatPeriodSeconds(本帧的 R, D)
//      3. due = 触发时刻 + T                       ← 每帧重算，T 一变 due 立刻跟着变
//      4. 第一帧：触发一次；之后：now >= due 就触发一次
//      5. y = BeatOffsetFromBucket(now, bucket)
//  触发时：triggerSeconds = now、amplitudePx = BeatAmplitudePx(本帧 R, D)、
//          dueSeconds = 这一拍采样到的 T（与生产层 TriggerBeatAt 写的是同一个量；
//          位移求值不读 dueSeconds，探针也不对它的取值下任何断言）。
//  ★ 一帧最多触发一次：迟到的帧**不补拍**（生产层也没有补齐的代码，case5b 量这条）。
//  ★ due 锚的是**触发时刻**，不是 now：锚在 now 时 due 永远领先一个 T，判据恒为假。
struct Tick {
    double t = 0.0;        // 这一帧的仿真时刻
    double tau = 0.0;      // 这一帧的 elapsed（now − 触发时刻）
    double y = 0.0;        // 这一帧的位移（px）
    double period = 0.0;   // 这一帧的 T（律给的值）
    double dueTime = 0.0;  // 这一帧判据用的 due 时刻（= 上一次触发时刻 + T）
    bool fired = false;    // 这一帧触发了没有
};

struct BeatFixture {
    BeatBucket bucket;
    double now = 0.0;
    long long triggers = 0;
    bool seeded = false;
    std::vector<double> triggerTimes;
    double minTau = 1e300;   // 全程出现过的最小 elapsed

    // 第一拍：生产层在第一帧（dt = 0）就跳一次，不等一个周期。
    void Seed(double R, double D) {
        bucket = BeatBucket{};
        now = 0.0;
        triggers = 0;
        seeded = false;
        triggerTimes.clear();
        minTau = 1e300;
        Step(0.0, R, D);
    }

    void Fire(double T, double R, double D) {
        bucket.triggerSeconds = now;
        bucket.amplitudePx = dshb::BeatAmplitudePx(R, D);
        bucket.dueSeconds = T;         // 这一拍自己的周期 T（t_j 每帧由 trigger + 它现算）
        ++triggers;
        triggerTimes.push_back(now);
    }

    Tick Step(double dt, double R, double D) {
        now += dt;
        Tick k;
        k.t = now;
        k.period = dshb::BeatPeriodSeconds(R, D);
        // ★ 这一行是整套新计时语义的要害（见文件头）：due = 上一次触发时刻 + T，
        //   每一帧都用**当前**的 (R, D) 重算 —— T 变短就把下一拍立刻拉近。
        k.dueTime = bucket.triggerSeconds + k.period;
        if (!seeded) {
            seeded = true;
            Fire(k.period, R, D);
            k.fired = true;
        } else if (now >= k.dueTime) {
            Fire(k.period, R, D);
            k.fired = true;
        }
        k.tau = now - bucket.triggerSeconds;
        k.y = dshb::BeatOffsetFromBucket(now, bucket);
        if (k.tau < minTau) minTau = k.tau;
        return k;
    }
};

// 固定 (R,D) 跑 frames 帧，返回逐帧位移。用于"同一初始状态重建"的逐位对照。
std::vector<double> FixtureSequence(double R, double D, int frames) {
    BeatFixture fx;
    fx.Seed(R, D);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) out.push_back(fx.Step(kFrame, R, D).y);
    return out;
}

// ===========================================================================
// 响应曲线与两条律的**公式**对照
// ===========================================================================
//  ★ 常量一个都不在这里写：e 的指数从 tuning.h 的 kBeatRPow 取，两条律的六个端点值
//    从 kBeatA* / kBeatT* 取。运算是同序同量：模块写的是
//        base + (min - base) * e * (1 - d) + (max - base) * d
//    这里逐字照抄，所以"网格对公式"这一条卡的是**结构**（多一项、少一项、(1-D) 挂错了
//    位置都会露出来），而不是浮点噪声。
//  ★ R 那一项被 (1-D) 缩放，所以 D=1 时 R 的两项都被乘掉：(R=1,D=1) 与 (R=0,D=1) 的
//    **周期**同值（30 s）；幅度则不然 —— 幅度的 D 项配的是 A_min，(1,1) 与 (0,1) 都是 3 px
//    而 (1,0) 才是 5 px。
//  ★ 幅度与周期的配对是**反的**（A_max 配 R、T_min 配 R）：剧烈 = 又快又猛，枯竭 = 又慢又弱。
double AmpByFormula(double R, double D) {
    const double e = dshb::BeatResponseCurve(R);
    const double d = Clamp01(D);
    return dshb::kBeatABase + (dshb::kBeatAMax - dshb::kBeatABase) * e * (1.0 - d) +
           (dshb::kBeatAMin - dshb::kBeatABase) * d;
}

double PeriodByFormula(double R, double D) {
    const double e = dshb::BeatResponseCurve(R);
    const double d = Clamp01(D);
    return dshb::kBeatTBase + (dshb::kBeatTMin - dshb::kBeatTBase) * e * (1.0 - d) +
           (dshb::kBeatTMax - dshb::kBeatTBase) * d;
}

// e(R) 的**定义式**重算：指数仍然是 tuning.h 的 kBeatRPow（探针里不写 1.5 这个字面量）。
double ResponseByFormula(double R) {
    const double r = Clamp01(R);
    return 1.0 - std::pow(1.0 - r, dshb::kBeatRPow);
}

// e(R) = 0.5 的那个 R：由定义反解 —— (1−R)^kBeatRPow = 0.5 -> R = 1 − 0.5^(1/kBeatRPow)。
// 用途是"e 的中点对应中点"这条：A/T 在它上面必须恰好等于两端点的平均。
double ResponseHalfR() { return 1.0 - std::pow(0.5, 1.0 / dshb::kBeatRPow); }

// ===========================================================================
// 探针内部的**对照实现**（不是模块的输出，只用来把"写错了会怎样"钉成数字）
// ===========================================================================
// 同一个包络，只把第一个脉冲的中心挪到别处：case7a 用它量"中心放在 0"的代价。
// 这里刻意用朴素减法 —— 它是一条**对照**曲线，不是被测代码。
double ShapeWithMu1(double tau, double mu1) {
    auto pulse = [](double x, double mu, double sigma) {
        const double u = x - mu;
        const double z = u / sigma;
        return std::exp(-0.5 * z * z);
    };
    return (pulse(tau, mu1, dshb::kBeatSigma1) +
            dshb::kBeatB2 * pulse(tau, dshb::kBeatMu2, dshb::kBeatSigma2)) /
           dshb::kBeatShapePeak;
}

// 朴素脉冲：先减再判门闸（"τ < μ 就不用算了，反正 exp 已经很小"）。
// 这是最自然的写法，也正是错的写法：μ=0.095 时 τ=0 落进门闸里，函数返回 0，
// 而正确值是 exp(-1.805)=0.164474（第二个脉冲）/ exp(-1.4917)=0.224941（第一个）。
double NaivePulse(double tau, double mu, double sigma) {
    if (tau < mu) return 0.0;
    const double u = tau - mu;
    const double z = u / sigma;
    return std::exp(-0.5 * z * z);
}

double NaiveShapeNorm(double tau) {
    if (tau < 0.0 || tau > dshb::kBeatLen) return 0.0;
    return (NaivePulse(tau, dshb::kBeatMu1, dshb::kBeatSigma1) +
            dshb::kBeatB2 * NaivePulse(tau, dshb::kBeatMu2, dshb::kBeatSigma2)) /
           dshb::kBeatShapePeak;
}

// 与模块同一顺序的 fma 重算：用来做逐位对照，证明"模块的值 == 不分段、无门闸的那个值"。
double FmaPulse(double tau, double mu, double sigma) {
    const double u = std::fma(tau, 1.0, -mu);
    const double z = u / sigma;
    const double e = -0.5 * z * z;
    return e > -700.0 ? std::exp(e) : 0.0;
}

double FmaShapeNorm(double tau) {
    if (tau < 0.0 || tau > dshb::kBeatLen) return 0.0;
    return (FmaPulse(tau, dshb::kBeatMu1, dshb::kBeatSigma1) +
            dshb::kBeatB2 * FmaPulse(tau, dshb::kBeatMu2, dshb::kBeatSigma2)) /
           dshb::kBeatShapePeak;
}

// ===========================================================================
// (0) 自检
// ===========================================================================
void RunSelfCheck(Harness* h) {
    Harness throwaway;
    const std::string passLine = throwaway.Line("case0:probe-true", "真检查", "预期 PASS", true);
    const std::string failLine = throwaway.Line("case0:probe-false", "假检查", "预期 FAIL", false);
    const bool good = throwaway.ReqSilent(true);
    const bool bad = throwaway.ReqSilent(false);
    // 这条自检在量两件事，都要有数字：
    //   ① 格式：假检查组装出来的那一行**行首是 "FAIL: "**（真检查是 "PASS: "）；
    //   ② 记账：丢弃用的 harness 里 passed 加了 1、failed 也加了 1，failures 里多一条。
    // 两件事合起来就是"它会报 FAIL"；那两行都没有打印，以免污染下游按行首读的证据。
    const bool formatOk = passLine.rfind("PASS: ", 0) == 0 && failLine.rfind("FAIL: ", 0) == 0;
    const bool countOk = throwaway.passed == 1 && throwaway.failed == 1 &&
                         throwaway.failures.size() == 1;
    const bool ok = good && !bad && formatOk && countOk;
    h->Req("case0", "自检：一条故意失败的检查真的被格式化成 FAIL 行、真的被计数",
           "假检查那一行的行首前缀 = \"" + failLine.substr(0, 6) + "\"（真检查 = \"" +
               passLine.substr(0, 6) + "\"）；丢弃用的 harness 计数 " + I(throwaway.passed) +
               " passed / " + I(throwaway.failed) + " failed，failures 里 " +
               I(static_cast<long long>(throwaway.failures.size())) +
               " 条；这两行都只组装、没有打印（打印会被下游当成一条真失败）",
           ok);
}

// ===========================================================================
// (1) 一拍就是**一个**包络
// ===========================================================================
void RunEnvelope(Harness* h, bool verbose) {
    // 幅度取 A_max：这样"包络峰值 = 1.0"就直接读成 5 px。**哪个 (R,D) 给到 A_max** 由
    // case3 量（这一次的律里 A_max 出现在 D=1 那一侧，探针不在这里重复那个结论）。
    // 触发时刻取 0，是为了让 τ = now 精确 —— 边界点 kBeatLen±1e-9 与 kBeatLen 都能精确比较。
    BeatBucket b;
    b.triggerSeconds = 0.0;
    b.amplitudePx = dshb::kBeatAMax;   // dueSeconds 不参与位移求值，本 case 用不到

    const int n = static_cast<int>(std::floor(dshb::kBeatLen * kEnvelopeHz));
    std::vector<double> tau(static_cast<std::size_t>(n) + 1, 0.0);
    std::vector<double> y(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i <= n; ++i) {
        tau[i] = static_cast<double>(i) / kEnvelopeHz;
        y[i] = dshb::BeatOffsetFromBucket(tau[i], b);
    }

    // 局部极大 / 极小只数**严格内点**，且网格只覆盖 [0, kBeatLen] 内部：
    // 把静默段的 0 也采进来，第一个 0 会被当成一个局部极小（更早的一版就这么数出 minima=1）。
    int maxima = 0;
    int minima = 0;
    double gridMax = -1e300;
    double gridMaxAt = 0.0;
    double gridMin = 1e300;
    double gridMinAt = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (y[i] > gridMax) {
            gridMax = y[i];
            gridMaxAt = tau[i];
        }
        if (y[i] < gridMin) {
            gridMin = y[i];
            gridMinAt = tau[i];
        }
    }
    for (std::size_t i = 1; i + 1 < y.size(); ++i) {
        if (y[i] > y[i - 1] && y[i] >= y[i + 1]) {
            ++maxima;
        } else if (y[i] < y[i - 1] && y[i] <= y[i + 1]) {
            ++minima;
        }
    }

    // 峰值：两个中心相等 -> 连续域峰值恰好在 τ = μ，且恰好是 1.0（-> A_max px）。
    const double peakNorm = dshb::BeatShapeNorm(dshb::kBeatMu1);
    const double peakPx = dshb::BeatOffsetFromBucket(dshb::kBeatMu1, b);

    // 硬边界：τ 一旦出了 [0, kBeatLen]，函数必须返回**恰好 0**（不是"很近 0"）。
    // 这一条是"两次跳动之间回到 0"能被验收的前提，所以要求 == 0.0，不接受"接近"。
    const double normBefore = dshb::BeatShapeNorm(-1e-9);
    const double normAfter = dshb::BeatShapeNorm(dshb::kBeatLen + 1e-9);
    const double pxBefore = dshb::BeatOffsetFromBucket(-1e-9, b);
    const double pxAfter = dshb::BeatOffsetFromBucket(dshb::kBeatLen + 1e-9, b);
    const double pxFar = dshb::BeatOffsetFromBucket(kT0, b);   // τ = 1000 s，静默段深处
    // τ = kBeatLen 本身**仍在区间内**，所以那里是"末端残留"而不是 0：
    // 常数给的量级是 2.3e-5（相对峰值），A_max 下约 1.15e-4 px。按 1e-3 px 卡，
    // 并把实测值印出来 —— 卡紧到 1e-9 就成了在量浮点噪声。
    const double residualNorm = dshb::BeatShapeNorm(dshb::kBeatLen);
    const double residualPx = dshb::BeatOffsetFromBucket(dshb::kBeatLen, b);

    const bool peakOk = std::fabs(peakNorm - 1.0) < 1e-12 &&
                        std::fabs(peakPx - dshb::kBeatAMax) < 1e-9;
    const bool edgeOk = normBefore == 0.0 && normAfter == 0.0 && pxBefore == 0.0 &&
                        pxAfter == 0.0 && pxFar == 0.0;
    const bool ok = maxima == 1 && minima == 0 && peakOk && edgeOk &&
                    gridMax >= dshb::kBeatAMax - 0.01 && residualNorm > 0.0 &&
                    residualPx > 0.0 && residualPx < 1e-3;

    h->Req("case1",
           "一拍就是一个包络：恰好 1 个局部极大、0 个局部极小；峰值 1.0（τ=μ 处，A_max 下 5 px）；"
           "τ 在 [0, kBeatLen] 之外恰好 0",
           "在 [0, kBeatLen] 内按 " + F(kEnvelopeHz, 0) + " Hz 采 " + I(n + 1) +
               " 点：局部极大 " + I(maxima) + " 个、局部极小 " + I(minima) + " 个；网格最大值 " +
               F(gridMax, 9) + " px @ τ=" + F(gridMaxAt * 1000.0, 3) + " ms，最小值 " +
               F(gridMin, 9) + " px @ τ=" + F(gridMinAt * 1000.0, 3) +
               " ms（网格末端的残余，不是 0 —— τ=kBeatLen 仍在区间内）；解析峰位 τ=μ=" +
               F(dshb::kBeatMu1 * 1000.0, 1) +
               " ms 处 BeatShapeNorm=" + F(peakNorm, 15) + "（峰值 1.0）、位移 " + F(peakPx, 12) +
               " px（A_max=" + F(dshb::kBeatAMax, 1) + " px）；"
               "区间外恰好 0：BeatShapeNorm(-1e-9)=" + F(normBefore, 12) + "、BeatShapeNorm(" +
               F(dshb::kBeatLen, 3) + "+1e-9)=" + F(normAfter, 12) + "、同两点的位移 " +
               F(pxBefore, 12) + " / " + F(pxAfter, 12) + " px（都是字面 0.0）、τ=1000 s 处 " +
               F(pxFar, 12) + " px；末端残留：τ=kBeatLen=" + F(dshb::kBeatLen, 3) +
               " s 处包络 " + F(residualNorm, 12) + "（规格约 2.3e-5）、位移 " + F(residualPx, 12) +
               " px（A_max 下）",
           ok);

    if (verbose) {
        std::printf("  [case1] 一拍的位移（τ ms, px），%d 个采样点里每 16 个印一格:\n   ",
                    n + 1);
        for (std::size_t i = 0; i < y.size(); i += 16) {
            std::printf(" (%.1f,%.4f)", tau[i] * 1000.0, y[i]);
        }
        std::printf("\n");
    }
}

// ===========================================================================
// (2) 单帧最大跳变（扫遍起始相位）
// ===========================================================================
struct JumpSweep {
    double amp = 0.0;         // 这一档的幅度（px）
    double worst = 0.0;       // 最坏单帧跳变（px）
    double worstTau = 0.0;    // 它出现在触发之后多久（ms）
    double worstPhase = 0.0;  // 触发落在帧格内的位置（0 = 正好落在格上）
    double gridWorst = 0.0;   // 触发正好落在帧格上（生产里最常见的情形）
    double gridWorstAt = 0.0; // 上者在触发之后多久（ms）
    double firstFrame = 0.0;  // ph=0 时"触发那一瞬间"的位移（px）
};

//  几何（这条容易写反，所以写详细）：
//    帧网格的间距是 1/60 s。触发落在格内位置 ph ∈ [0,1) 处，于是触发之后的
//    第 k 个采样点相对触发的时刻是
//        τ = (ph + k)/60 ,  k = 0, 1, 2, ...
//    —— **触发之后的第一个采样点是 k = 0**，即 τ = ph/60 ∈ [0, 1/60)。
//    触发那一帧的位移是 0（静默段），所以首帧跳变 = τ 扫过 [0, 1/60) 时位移的最大值。
//    本参数下它出现在 τ→1/60：33.94% —— 这就是整个扫描的上界。
JumpSweep SweepJump(double R, double D, int phases, int frames) {
    JumpSweep out;
    BeatBucket b;
    b.triggerSeconds = 0.0;
    b.amplitudePx = dshb::BeatAmplitudePx(R, D);   // dueSeconds 不参与位移求值
    out.amp = b.amplitudePx;

    // 触发正好落在帧格上（ph = 0）：生产里帧步均匀时就是这个情形。
    double prev = 0.0;   // 触发之前的一帧：静默段，恰好 0
    for (int k = 0; k < frames; ++k) {
        const double t = static_cast<double>(k) * kFrame;
        const double yy = dshb::BeatOffsetFromBucket(t, b);
        const double jump = std::fabs(yy - prev);
        if (jump > out.gridWorst) {
            out.gridWorst = jump;
            out.gridWorstAt = t * 1000.0;
        }
        prev = yy;
    }
    out.firstFrame = dshb::BeatOffsetFromBucket(0.0, b);

    for (int q = 0; q < phases; ++q) {
        const double ph = (q + 0.5) / phases;
        prev = 0.0;
        for (int k = 0; k < frames; ++k) {
            const double tt = (ph + k) * kFrame;
            const double yy = dshb::BeatOffsetFromBucket(tt, b);
            const double jump = std::fabs(yy - prev);
            if (jump > out.worst) {
                out.worst = jump;
                out.worstTau = tt * 1000.0;
                out.worstPhase = ph;
            }
            prev = yy;
        }
    }
    return out;
}

void RunJumpSweeps(Harness* h) {
    // 三个 (R,D) 档：常态、两端各一半、最剧烈。A 从**模块**取（不写死），
    // 因为新律里 A(1,0)=5 px = kBeatAMax（最剧烈那一档的幅度最大 —— case3 的四个角钉的事）。
    struct Case {
        double R;
        double D;
    };
    const Case cases[3] = {{0.0, 0.0}, {0.5, 0.5}, {1.0, 0.0}};
    for (const Case& c : cases) {
        const JumpSweep s = SweepJump(c.R, c.D, 1200, 24);
        const double pct = 100.0 * s.worst / s.amp;
        const double gridPct = 100.0 * s.gridWorst / s.amp;
        h->Req("case2",
               "单帧最大跳变 <= 35% 幅度（R=" + F(c.R, 1) + "/D=" + F(c.D, 1) + "，A=" +
                   F(s.amp, 3) + " px）",
               "扇 1200 个起始相位 × 24 帧 = 28800 次求值：最坏跳变 " + F(s.worst, 6) + " px = " +
                   F(pct, 2) + "% of A=" + F(s.amp, 3) + " px，出现在触发后 τ=" +
                   F(s.worstTau, 3) + " ms（触发落在帧格内 " + F(s.worstPhase, 6) +
                   " 处；落在首帧=" + YN(s.worstTau <= kFrame * 1000.0 + 1e-9) +
                   "，也就是触发后第一个采样点那一格）；触发正好落在帧格上时最坏 " + F(s.gridWorst, 6) +
                   " px = " + F(gridPct, 2) + "% @ τ=" + F(s.gridWorstAt, 3) +
                   " ms；触发那一瞬间（τ=0）的位移 " + F(s.firstFrame, 6) + " px = " +
                   F(100.0 * s.firstFrame / s.amp, 2) + "%",
               pct <= 35.0);
    }
}

// ===========================================================================
// (3b) 响应曲线 e(R)
// ===========================================================================
//  这是本次修订的主角：R 不再线性地进公式，而是先过一道
//      e(R) = 1 − (1 − R)^kBeatRPow
//  它必须满足 e(0) = 0、e(1) = 1（两个端点是精确的：1 − 1 = 0、1 − 0 = 1）、单调递增，
//  以及 R ∈ (0,1) 上 **e(R) > R** —— 后者就是"同样的 R 现在更剧烈"的全部内容。
void RunResponseCurve(Harness* h) {
    const double e0 = dshb::BeatResponseCurve(0.0);
    const double e1 = dshb::BeatResponseCurve(1.0);

    // 在 [0,1] 上扫 101 个点：单调性、e(R) > R、与定义式的偏差。
    const int n = 100;
    bool monotone = true;
    long long strictSteps = 0;
    double minExcess = 1e300;
    double minExcessAt = 0.0;
    double worstFormula = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double r = static_cast<double>(i) / static_cast<double>(n);
        const double e = dshb::BeatResponseCurve(r);
        const double dev = std::fabs(e - ResponseByFormula(r));
        if (dev > worstFormula) worstFormula = dev;
        if (i > 0) {
            const double prev = dshb::BeatResponseCurve(static_cast<double>(i - 1) /
                                                        static_cast<double>(n));
            if (e < prev) monotone = false;
            if (e > prev) ++strictSteps;
        }
        if (i > 0 && i < n) {
            const double excess = e - r;
            if (excess < minExcess) {
                minExcess = excess;
                minExcessAt = r;
            }
        }
    }

    const double clampHigh = dshb::BeatResponseCurve(2.0);
    const double clampLow = dshb::BeatResponseCurve(-1.0);
    const double half = dshb::BeatResponseCurve(ResponseHalfR());

    const bool ok = e0 == 0.0 && e1 == 1.0 && monotone && strictSteps == n &&
                    minExcess > 0.0 && worstFormula < 1e-15 && clampHigh == e1 &&
                    clampLow == e0 && std::fabs(half - 0.5) < 1e-12;
    h->Req("case3b",
           "响应曲线 e(R) = 1 − (1 − R)^kBeatRPow：e(0)=0、e(1)=1（都精确）、[0,1] 上单调递增、"
           "R∈(0,1) 时 e(R) > R、与定义式重算一致、输入夹到 [0,1]",
           "e(0)=" + F(e0, 17) + "、e(1)=" + F(e1, 17) + "；[0,1] 上 101 个点：单调递增=" +
               YN(monotone) + "（严格上升 " + I(strictSteps) + "/100 段）；"
               "R∈(0,1) 上 e(R)−R 的最小值 " + F(minExcess, 9) + "（在 R=" +
               F(minExcessAt, 2) + "，> 0 = 同样的 R 比线性式更剧烈）；"
               "与定义式 1−(1−R)^kBeatRPow 的最大偏差 " + F(worstFormula, 18) +
               "（kBeatRPow=" + F(dshb::kBeatRPow, 2) + "）；夹取：e(2)=" + F(clampHigh, 17) +
               " == e(1)、e(-1)=" + F(clampLow, 17) + " == e(0)；"
               "几个规格点：e(0.25)=" + F(dshb::BeatResponseCurve(0.25), 6) + "（旧式 0.25）、e(0.5)=" +
               F(dshb::BeatResponseCurve(0.5), 6) + "（旧式 0.5）、e(0.9)=" +
               F(dshb::BeatResponseCurve(0.9), 6) + "（旧式 0.9）；e(R)=0.5 的那个 R=" +
               F(ResponseHalfR(), 9) + "（case3/case4 用它做「e 的中点对应中点」）",
           ok);
}

// ===========================================================================
// (3) 幅度律 A(R,D)
// ===========================================================================
void RunAmplitudeLaw(Harness* h) {
    const double grid[7] = {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0};

    double worst = 0.0;
    double worstAtR = 0.0;
    double worstAtD = 0.0;
    for (double r : grid) {
        for (double d : grid) {
            const double e = std::fabs(dshb::BeatAmplitudePx(r, d) - AmpByFormula(r, d));
            if (e > worst) {
                worst = e;
                worstAtR = r;
                worstAtD = d;
            }
        }
    }

    // 四个角：**所有者 2026-09-18 第二次修订给的规格值**（按实现里的式子实算）：
    //      A(0,0) = 3    A(1,0) = 5    A(0,1) = 1    A(1,1) = 1
//    （(1,1) 与 (0,1) 同值：D=1 时 (1-D)=0，R 那一项整个消失，剩下的就是 A_min）
    // ★ 这四行是本文件里**仅有的两处**规格字面量之一（另一处是 case4 的四角表）：
    //   它钉的是**规格**，不是复算波形。其余期望值都从 src/tuning.h 的常量推。
    // ★ (1,0) 与 (0,1) 是**两个相反的方向**：R 那一项被 (1-D) 缩放，D=1 时它整个消失，
    //   所以 (1,1) 与 (0,1) 同值。
    const double specR[4] = {0.0, 1.0, 0.0, 1.0};
    const double specD[4] = {0.0, 0.0, 1.0, 1.0};
    const double specWant[4] = {3.0, 5.0, 1.0, 1.0};
    double cornerErr = 0.0;
    std::string corners;
    for (int i = 0; i < 4; ++i) {
        const double got = dshb::BeatAmplitudePx(specR[i], specD[i]);
        const double e = std::fabs(got - specWant[i]);
        if (e > cornerErr) cornerErr = e;
        corners += " A(" + F(specR[i], 0) + "," + F(specD[i], 0) + ")=" + F(got, 12);
    }

    // 对 D 线性（固定 R）：中点 = 两端平均。D 只以一次项出现，所以这条必须精确成立。
    double worstMidD = 0.0;
    for (double r : grid) {
        const double mid = std::fabs(dshb::BeatAmplitudePx(r, 0.5) -
                                     0.5 * (dshb::BeatAmplitudePx(r, 0.0) +
                                            dshb::BeatAmplitudePx(r, 1.0)));
        if (mid > worstMidD) worstMidD = mid;
    }

    // 对 e(R) 仿射（固定 D）：R 只通过 e(R) 进来 —— A(R,D) = A(0,D) + (A(1,D) − A(0,D))·e(R)。
    // 对 R 本身**不再线性**，所以这里量的不再是"R 的中点"，而是 e(R) 的中点。
    const double rScan[4] = {0.25, 0.5, 0.75, ResponseHalfR()};
    double worstE = 0.0;
    double worstEAtR = 0.0;
    double worstEAtD = 0.0;
    for (double r : rScan) {
        const double e = dshb::BeatResponseCurve(r);
        for (double d : grid) {
            const double lo = dshb::BeatAmplitudePx(0.0, d);
            const double hi = dshb::BeatAmplitudePx(1.0, d);
            const double dev = std::fabs(dshb::BeatAmplitudePx(r, d) - (lo + (hi - lo) * e));
            if (dev > worstE) {
                worstE = dev;
                worstEAtR = r;
                worstEAtD = d;
            }
        }
    }
    // e = 0.5 处必须恰好等于两端点的平均（"e 的中点对应中点"）。
    double worstMidE = 0.0;
    for (double d : grid) {
        const double mid = std::fabs(dshb::BeatAmplitudePx(ResponseHalfR(), d) -
                                     0.5 * (dshb::BeatAmplitudePx(0.0, d) +
                                            dshb::BeatAmplitudePx(1.0, d)));
        if (mid > worstMidE) worstMidE = mid;
    }

    // 夹取：模块的头文件写着"R、D 会先夹到 [0,1]"。
    const double clampHigh = dshb::BeatAmplitudePx(2.0, 3.0);
    const double clampLow = dshb::BeatAmplitudePx(-1.0, -0.5);
    const double clampRefHigh = dshb::BeatAmplitudePx(1.0, 1.0);
    const double clampRefLow = dshb::BeatAmplitudePx(0.0, 0.0);

    // 方向（用模块自己的值算，只是印出来的证据；判据是上面那张四角表）：
    const double slopeR = dshb::BeatAmplitudePx(1.0, 0.0) - dshb::BeatAmplitudePx(0.0, 0.0);
    const double slopeD = dshb::BeatAmplitudePx(0.0, 1.0) - dshb::BeatAmplitudePx(0.0, 0.0);

    const bool ok = worst < 1e-12 && cornerErr < 1e-12 && worstMidD < 1e-12 &&
                    worstE < 1e-12 && worstMidE < 1e-12 && clampHigh == clampRefHigh &&
                    clampLow == clampRefLow;
    h->Req("case3",
           "幅度律 A(R,D)=A_base+(A_min-A_base)·e(R)·(1-D)+(A_max-A_base)·D：49 点网格对公式、"
           "四个角 3/5/1/1（规格）、对 D 线性、对 e(R) 仿射（e 的中点=中点）、输入夹到 [0,1]",
           "网格 R,D 各取 {0, 0.1, 0.25, 0.5, 0.75, 0.9, 1} 共 49 点：最大偏差 " + F(worst, 15) +
               " px（在 R=" + F(worstAtR, 2) + ", D=" + F(worstAtD, 2) + "）；四个角" + corners +
               "（规格 3/5/1/1，最大偏差 " + F(cornerErr, 15) + "）；对 D 线性的中点误差 " +
               F(worstMidD, 15) + " px；对 e(R) 仿射的最大偏差 " + F(worstE, 15) + " px（在 R=" +
               F(worstEAtR, 4) + ", D=" + F(worstEAtD, 2) + "），其中 e=0.5 那一点" +
               "（R=" + F(ResponseHalfR(), 6) + "）与两端平均的差 " + F(worstMidE, 15) +
               " px；斜率（用模块自己的值算）dA/dR=" + F(slopeR, 6) + " px（R 0->1，D=0）、"
               "dA/dD=" + F(slopeD, 6) + " px（D 0->1，R=0）；夹取：A(2,3)=" + F(clampHigh, 6) +
               " == A(1,1)=" + F(clampRefHigh, 6) + "，A(-1,-0.5)=" + F(clampLow, 6) +
               " == A(0,0)=" + F(clampRefLow, 6) + " px；常量 A_base=" + F(dshb::kBeatABase, 1) +
               " A_min=" + F(dshb::kBeatAMin, 1) + " A_max=" + F(dshb::kBeatAMax, 1) + " px",
           ok);
}

// ===========================================================================
// (4) 周期律 T(R,D)
// ===========================================================================
void RunPeriodLaw(Harness* h) {
    const double grid[7] = {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0};

    double worst = 0.0;
    double worstAtR = 0.0;
    double worstAtD = 0.0;
    for (double r : grid) {
        for (double d : grid) {
            const double e = std::fabs(dshb::BeatPeriodSeconds(r, d) - PeriodByFormula(r, d));
            if (e > worst) {
                worst = e;
                worstAtR = r;
                worstAtD = d;
            }
        }
    }

    // 四个角：**所有者 2026-09-18 第二次修订给的规格值**：
    //      T(0,0) = 15   T(1,0) = 0.5   T(0,1) = 30   T(1,1) = 30
    // 与 case3 同一句话：这四行也是**规格字面量**（本文件只有这两张四角表写死了两律数值）。
    const double specR[4] = {0.0, 1.0, 0.0, 1.0};
    const double specD[4] = {0.0, 0.0, 1.0, 1.0};
    const double specWant[4] = {15.0, 0.5, 30.0, 30.0};
    double cornerErr = 0.0;
    std::string corners;
    for (int i = 0; i < 4; ++i) {
        const double got = dshb::BeatPeriodSeconds(specR[i], specD[i]);
        const double e = std::fabs(got - specWant[i]);
        if (e > cornerErr) cornerErr = e;
        corners += " T(" + F(specR[i], 0) + "," + F(specD[i], 0) + ")=" + F(got, 12);
    }

    // 对 D 线性（固定 R）。
    double worstMidD = 0.0;
    for (double r : grid) {
        const double mid = std::fabs(dshb::BeatPeriodSeconds(r, 0.5) -
                                     0.5 * (dshb::BeatPeriodSeconds(r, 0.0) +
                                            dshb::BeatPeriodSeconds(r, 1.0)));
        if (mid > worstMidD) worstMidD = mid;
    }

    // 对 e(R) 仿射（固定 D）：T(R,D) = T(0,D) + (T(1,D) − T(0,D))·e(R)。
    const double rScan[4] = {0.25, 0.5, 0.75, ResponseHalfR()};
    double worstE = 0.0;
    double worstEAtR = 0.0;
    double worstEAtD = 0.0;
    for (double r : rScan) {
        const double e = dshb::BeatResponseCurve(r);
        for (double d : grid) {
            const double lo = dshb::BeatPeriodSeconds(0.0, d);
            const double hi = dshb::BeatPeriodSeconds(1.0, d);
            const double dev = std::fabs(dshb::BeatPeriodSeconds(r, d) - (lo + (hi - lo) * e));
            if (dev > worstE) {
                worstE = dev;
                worstEAtR = r;
                worstEAtD = d;
            }
        }
    }
    double worstMidE = 0.0;
    for (double d : grid) {
        const double mid = std::fabs(dshb::BeatPeriodSeconds(ResponseHalfR(), d) -
                                     0.5 * (dshb::BeatPeriodSeconds(0.0, d) +
                                            dshb::BeatPeriodSeconds(1.0, d)));
        if (mid > worstMidE) worstMidE = mid;
    }

    const double clampHigh = dshb::BeatPeriodSeconds(2.0, 3.0);
    const double clampLow = dshb::BeatPeriodSeconds(-1.0, -0.5);
    const double clampRefHigh = dshb::BeatPeriodSeconds(1.0, 1.0);
    const double clampRefLow = dshb::BeatPeriodSeconds(0.0, 0.0);

    const double slopeR = dshb::BeatPeriodSeconds(1.0, 0.0) - dshb::BeatPeriodSeconds(0.0, 0.0);
    const double slopeD = dshb::BeatPeriodSeconds(0.0, 1.0) - dshb::BeatPeriodSeconds(0.0, 0.0);

    // 用模块自己的律取最快/最慢，不抄常量。
    const double fastest = dshb::BeatPeriodSeconds(1.0, 0.0);
    const double slowest = dshb::BeatPeriodSeconds(0.0, 1.0);

    const bool ok = worst < 1e-9 && cornerErr < 1e-12 && worstMidD < 1e-9 &&
                    worstE < 1e-9 && worstMidE < 1e-9 && clampHigh == clampRefHigh &&
                    clampLow == clampRefLow && fastest > dshb::kBeatLen;
    h->Req("case4",
           "周期律 T(R,D)=T_base+(T_min-T_base)·e(R)·(1-D)+(T_max-T_base)·D：49 点网格对公式、"
           "四个角 15/0.5/30/30（规格）、对 D 线性、对 e(R) 仿射（e 的中点=中点）、输入夹到 [0,1]",
           "网格 R,D 各取 {0, 0.1, 0.25, 0.5, 0.75, 0.9, 1} 共 49 点：最大偏差 " + F(worst, 15) +
               " s（在 R=" + F(worstAtR, 2) + ", D=" + F(worstAtD, 2) + "）；四个角" + corners +
               "（规格 15/0.5/30/30，最大偏差 " + F(cornerErr, 15) + "）；对 D 线性的中点误差 " +
               F(worstMidD, 15) + " s；对 e(R) 仿射的最大偏差 " + F(worstE, 15) + " s（在 R=" +
               F(worstEAtR, 4) + ", D=" + F(worstEAtD, 2) + "），其中 e=0.5 那一点" +
               "（R=" + F(ResponseHalfR(), 6) + "）与两端平均的差 " + F(worstMidE, 15) +
               " s；斜率 dT/dR=" + F(slopeR, 6) + " s（R 0->1，D=0）、dT/dD=" + F(slopeD, 6) +
               " s（D 0->1，R=0）；夹取：T(2,3)=" + F(clampHigh, 6) + " == T(1,1)=" +
               F(clampRefHigh, 6) + "，T(-1,-0.5)=" + F(clampLow, 6) + " == T(0,0)=" +
               F(clampRefLow, 6) + " s；常量 T_base=" + F(dshb::kBeatTBase, 1) + " T_min=" +
               F(dshb::kBeatTMin, 1) + " T_max=" + F(dshb::kBeatTMax, 1) + " s；律给的最快 " +
               F(fastest, 4) + " s、最慢 " + F(slowest, 4) + " s",
           ok);
}

// ===========================================================================
// (5a) 计时：触发次数是帧量化的
// ===========================================================================
void RunTimerCount(Harness* h) {
    struct Case {
        const char* name;
        double R;
        double D;
        double seconds;
    };
    const Case cases[2] = {{"常态 R=0/D=0", 0.0, 0.0, 60.0},
                           {"最快 R=1/D=0", 1.0, 0.0, 10.0}};
    for (const Case& c : cases) {
        const double T = dshb::BeatPeriodSeconds(c.R, c.D);
        const int frames = static_cast<int>(std::llround(c.seconds * dshb::kBeatFrameHz));
        BeatFixture fx;
        fx.Seed(c.R, c.D);
        for (int i = 0; i < frames; ++i) fx.Step(kFrame, c.R, c.D);

        // 触发次数的两种算法：
        //   · 帧量化（这一次计时语义的真实答案）：判据只在**整帧**上比较，所以每拍固定占
        //     ceil(T×60) 帧，一拍也不多。这是生产层真正会给出的次数。
        //   · 理想值 1 + floor(N/T)：连续时间的答案。两者最多差 1 —— 也就是"每拍固定慢
        //     不到一帧"，所以计数本身不能拿来当"漏拍"的证据，interval 那一列才是。
        const long long perBeatFrames =
            static_cast<long long>(std::ceil(T * dshb::kBeatFrameHz - 1e-9));
        const long long expectQuant = 1 + frames / perBeatFrames;
        const long long expectIdeal = 1 + static_cast<long long>(std::floor(c.seconds / T));
        const long long got = fx.triggers;
        const long long diffQuant = got - expectQuant;
        const long long diffIdeal = got - expectIdeal;

        // 每个间隔必须落在 [T, T + 一帧]：触发发生在"跨过 due 的第一个采样格"上。
        double minIv = 1e300;
        double maxIv = 0.0;
        for (std::size_t i = 1; i < fx.triggerTimes.size(); ++i) {
            const double iv = fx.triggerTimes[i] - fx.triggerTimes[i - 1];
            if (iv < minIv) minIv = iv;
            if (iv > maxIv) maxIv = iv;
        }
        const bool haveTwo = fx.triggerTimes.size() >= 2;
        const bool countsOk = diffQuant <= 1 && diffQuant >= -1 && diffIdeal <= 1 &&
                              diffIdeal >= -1;
        const bool ok = countsOk && !fx.triggerTimes.empty() && fx.triggerTimes[0] == 0.0 &&
                        haveTwo && minIv >= T - 1e-9 && maxIv <= T + kFrame + 1e-9 &&
                        fx.minTau >= 0.0;

        const std::string gap =
            haveTwo ? "；间隔 ∈ [" + F(minIv, 9) + ", " + F(maxIv, 9) + "] s（T 到 T+一帧 = " +
                          F(T + kFrame, 9) + " s）"
                    : std::string("；只有一个触发时刻，间隔无从谈起");

        h->Req("case5a",
               std::string("计时：固定 (R,D) 跑 N 秒，触发次数是帧量化的（") + c.name + "）",
               "T=" + F(T, 4) + " s，跑 " + F(c.seconds, 1) + " s = " + I(frames) +
                   " 帧 @60 Hz（每拍 " + I(perBeatFrames) + " 帧 = ceil(T×60)）：触发 " + I(got) +
                   " 次，帧量化期望 1 + floor(" + I(frames) + "/" + I(perBeatFrames) + ") = " +
                   I(expectQuant) + "（差 " + I(diffQuant) + "），理想值 1 + floor(" +
                   F(c.seconds, 1) + "/" + F(T, 4) + ") = " + I(expectIdeal) + "（差 " +
                   I(diffIdeal) + "）—— 两者最多差 1，即每拍固定慢不到一帧；节拍时刻（s）=" +
                   Times(fx.triggerTimes, 5, 3) + gap + "；第一拍在 t=" +
                   F(fx.triggerTimes[0], 6) + " s 触发，全程最小 elapsed τ=" + F(fx.minTau, 12) + " s",
               ok);
    }
}

// ===========================================================================
// (5b) 计时：不补拍、不回退
// ===========================================================================
void RunTimerNoCatchUp(Harness* h) {
    const double R = 1.0;
    const double D = 0.0;
    BeatFixture fx;
    fx.Seed(R, D);
    const double T = dshb::BeatPeriodSeconds(R, D);

    const long long before = fx.triggers;
    const Tick a = fx.Step(3.0 * T, R, D);      // 一帧拉成 3T
    const long long afterA = fx.triggers;
    const Tick b = fx.Step(7.0 * T, R, D);      // 再跳一大帧
    const long long afterB = fx.triggers;

    // "补齐"的写法会按 elapsed/T 补：那是这两个数。
    const long long naiveA = static_cast<long long>(std::floor((3.0 * T) / T));
    const long long naiveB = static_cast<long long>(std::floor((7.0 * T) / T));

    bool increasing = true;
    for (std::size_t i = 1; i < fx.triggerTimes.size(); ++i) {
        if (!(fx.triggerTimes[i] > fx.triggerTimes[i - 1])) increasing = false;
    }

    const bool ok = (afterA - before) == 1 && (afterB - afterA) == 1 && a.tau == 0.0 &&
                    b.tau == 0.0 && a.t == 3.0 * T && increasing && fx.minTau >= 0.0 &&
                    fx.triggers == 3;
    h->Req("case5b",
           "计时：一帧拉成 3T / 7T 只触发一次，不补拍（触发时刻记在 now，不倒推回去）；"
           "时刻严格递增；now − triggerSeconds 全程不为负",
           "T=" + F(T, 4) + " s；第 1 帧 dt=3T=" + F(3.0 * T, 4) + " s：触发 " +
               I(afterA - before) + " 次（按 elapsed/T 补齐会触发 " + I(naiveA) +
               " 次），触发时刻 t=" + F(a.t, 6) + " s = now，触发后 τ=" + F(a.tau, 9) +
               " s、位移 " + F(a.y, 6) + " px（= A·Shape(0)，触发瞬间不是 0）；第 2 帧 dt=7T=" +
               F(7.0 * T, 4) + " s：触发 " + I(afterB - afterA) + " 次（补齐会 " + I(naiveB) +
               " 次），t=" + F(b.t, 6) + " s、τ=" + F(b.tau, 9) + " s；两帧共 " + I(fx.triggers) +
               " 次触发（含 t=0 的第一拍），帧数 2；触发时刻严格递增=" + YN(increasing) +
               "（" + Times(fx.triggerTimes, 4, 2) + "）；全程最小 τ=" + F(fx.minTau, 12) + " s",
           ok);
}

// ===========================================================================
// (5c) 计时：T 变短 -> 下一拍**提前到**
// ===========================================================================
//  旧语义的对照（只用来把"提前到"钉成两个数字）：已过时间 >= 这一拍**采样到的**周期，
//  周期整拍不变。它不是被测代码，判据也不依赖它 —— 它只是同一条律在旧计时下的次数。
long long LegacyTriggerCount(double seconds, double tChange, double RFast) {
    double now = 0.0;
    double trigger = 0.0;
    double period = 0.0;
    long long n = 0;
    bool seeded = false;
    const int frames = static_cast<int>(std::llround(seconds * dshb::kBeatFrameHz));
    for (int i = 0; i <= frames; ++i) {
        if (i != 0) now += kFrame;
        const double r = (now >= tChange) ? RFast : 0.0;
        if (!seeded) {
            seeded = true;
            trigger = now;
            period = dshb::BeatPeriodSeconds(r, 0.0);
            ++n;
        } else if (now - trigger >= period) {
            trigger = now;
            period = dshb::BeatPeriodSeconds(r, 0.0);
            ++n;
        }
    }
    return n;
}

void RunTimerFasterWhenShorter(Harness* h) {
    // 脚本：t ∈ [0,1) 用 R=0（律给 T=15 s），t >= 1.0 起用 R=1（律给 T=0.5 s）。
    //   新语义：due = 触发时刻 + T(现在的 R,D)，每帧重算。换律那一帧 due 变成 0 + 0.5 ——
    //           **早就过去了**，所以一帧之内就跳下一拍，随后每 0.5 s 一跳。
    //   旧语义：0 s 采样到的 T=15 s 把下一拍钉在 15 s（变红了也要等满旧拍）。
    const double tChange = 1.0;
    const double seconds = 5.0;
    const int frames = static_cast<int>(std::llround(seconds * dshb::kBeatFrameHz));

    BeatFixture fx;
    fx.Seed(0.0, 0.0);
    const double tSlow = dshb::BeatPeriodSeconds(0.0, 0.0);
    const double tFast = dshb::BeatPeriodSeconds(1.0, 0.0);

    double dueLastSlow = 0.0;    // 换律之前 due 一直是它（= 触发时刻 0 + 15）
    double dueAtFlip = 0.0;      // 换律那一帧判据用的 due
    double tFlip = 0.0;          // 第一次用 R=1 的那一帧
    double firstAfterFlip = 0.0; // 换律之后的第一次触发时刻
    for (int i = 0; i < frames; ++i) {
        const double t = fx.now + kFrame;
        const double r = (t >= tChange) ? 1.0 : 0.0;
        const Tick k = fx.Step(kFrame, r, 0.0);
        if (r == 0.0) {
            dueLastSlow = k.dueTime;
        } else {
            if (tFlip == 0.0) {
                tFlip = k.t;
                dueAtFlip = k.dueTime;
            }
            if (firstAfterFlip == 0.0 && k.fired && k.t > 0.0) firstAfterFlip = k.t;
        }
    }

    // 换律之后每一拍的间隔（第一段是"从 0 到第一次提前到"的那一跳，单列出来）。
    std::vector<double> ivFast;
    for (std::size_t i = 1; i < fx.triggerTimes.size(); ++i) {
        if (fx.triggerTimes[i - 1] >= tChange) {
            ivFast.push_back(fx.triggerTimes[i] - fx.triggerTimes[i - 1]);
        }
    }
    double worstFastIv = 0.0;
    for (double iv : ivFast) {
        const double dev = std::fabs(iv - tFast);
        if (dev > worstFastIv) worstFastIv = dev;
    }

    const long long legacy = LegacyTriggerCount(seconds, tChange, 1.0);
    const long long got = fx.triggers;
    const double gapFrames = (firstAfterFlip - tFlip) / kFrame;   // 换律到下一拍隔了几帧

    const bool ok = tFlip > 0.0 && firstAfterFlip > 0.0 && gapFrames <= 1.0 + 1e-9 &&
                    firstAfterFlip < tSlow && std::fabs(dueLastSlow - tSlow) < 1e-12 &&
                    std::fabs(dueAtFlip - tFast) < 1e-12 && !ivFast.empty() &&
                    worstFastIv <= kFrame + 1e-9 && got > legacy && fx.minTau >= 0.0;
    h->Req("case5c",
           "计时：T 变短 -> 下一拍提前到（due 每帧 = 触发时刻 + T，换律后一帧之内就跳，之后每 "
           "T_fast 一跳）；旧语义（整拍采样不变）的触发次数做对照",
           "脚本：t∈[0,1) R=0（律给 T=" + F(tSlow, 1) + " s），t>=1.0 R=1（律给 T=" +
               F(tFast, 1) + " s）；跑 " + F(seconds, 0) + " s = " + I(frames) +
               " 帧；换律前 due 一直是 " + F(dueLastSlow, 9) + " s（= 第一拍触发时刻 0 + T_slow）；"
               "第一次用 R=1 在 t=" + F(tFlip, 6) + " s，那一帧判据用的 due=" + F(dueAtFlip, 9) +
               " s（= 0 + T_fast，早就过去了）-> 下一拍在 t=" + F(firstAfterFlip, 6) +
               " s，与换律只隔 " + F(gapFrames, 2) + " 帧；此后每 " + F(tFast, 4) +
               " s 一跳（与 T_fast 的最大偏差 " + F(worstFastIv, 9) + " s，<= 一帧 " +
               F(kFrame, 9) + " s）；触发时刻（s）=" + Times(fx.triggerTimes, 4, 3) + "；"
               "同一脚本、同一律：新语义共 " + I(got) + " 拍，旧语义（整拍采样不变、等满旧拍）" +
               I(legacy) + " 拍；全程最小 τ=" + F(fx.minTau, 12) + " s",
           ok);
}

// ===========================================================================
// (5d) 计时：静默窗口恰好 0，且两拍不叠
// ===========================================================================
void RunSilenceWindow(Harness* h) {
    // 最短周期那一档就是最坏情形：静默窗口最短（T - kBeatLen 最小），
    // 所以只跑这一档；T 从**律**里取，不抄常量。
    const double R = 1.0;
    const double D = 0.0;
    const double T = dshb::BeatPeriodSeconds(R, D);
    BeatFixture fx;
    fx.Seed(R, D);
    const int frames = static_cast<int>(std::llround(10.0 * dshb::kBeatFrameHz));

    int silent = 0;
    int silentBad = 0;
    int inside = 0;
    int insideBad = 0;
    double lastInsideTau = 0.0;
    double firstSilentTau = 0.0;
    for (int i = 0; i < frames; ++i) {
        const Tick k = fx.Step(kFrame, R, D);
        if (k.tau > dshb::kBeatLen) {
            ++silent;
            if (k.y != 0.0) ++silentBad;
            if (silent == 1) firstSilentTau = k.tau;   // 第一个静默帧的 τ
        } else {
            ++inside;
            if (!(k.y > 0.0)) ++insideBad;
            if (k.tau > lastInsideTau) lastInsideTau = k.tau;
        }
    }

    const double margin = T - dshb::kBeatLen;
    const bool ok = silentBad == 0 && insideBad == 0 && silent > 0 && inside > 0 &&
                    margin > 0.0 && dshb::kBeatTMin > dshb::kBeatLen && fx.minTau >= 0.0 &&
                    fx.triggers > 1;
    h->Req("case5d",
           "计时：静默窗口（τ > kBeatLen）里位移恰好 0；且最短周期 T_min > kBeatLen（两拍不叠的前提）",
           "最快档 R=1/D=0：T=" + F(T, 4) + " s、一拍长 kBeatLen=" + F(dshb::kBeatLen, 3) +
               " s，余量 " + F(margin, 3) + " s = " + F(margin * dshb::kBeatFrameHz, 1) + " 帧；跑 " +
               F(10.0, 0) + " s = " + I(frames) + " 帧（触发 " + I(fx.triggers) + " 拍）：静默帧 " +
               I(silent) + "/" + I(frames) + "（位移恰好 0.0 的 " + I(silent - silentBad) +
               " 帧，不等于 0 的 " + I(silentBad) + " 帧），信封内帧 " + I(inside) + "（全部 > 0 的 " +
               I(inside - insideBad) + " 帧，坏的 " + I(insideBad) + " 帧）；信封内最大 τ=" +
               F(lastInsideTau, 6) + " s，第一个静默帧 τ=" + F(firstSilentTau, 6) +
               " s；常数一侧：kBeatTMin=" + F(dshb::kBeatTMin, 3) + " s > kBeatLen=" +
               F(dshb::kBeatLen, 3) + " s；全程最小 elapsed τ=" + F(fx.minTau, 12) + " s",
           ok);
}

// ===========================================================================
// (5e) 计时：due 每帧都在追 T
// ===========================================================================
//  这条钉的是**锚点**：固定 (R,D) 时 due 每帧都等于"触发时刻 + T"，
//  于是非触发帧上 due 一定**小于** now + T（严格）。如果把 due 写成 now + T，
//  那么每个非触发帧上 due == now + T，判据 now >= due 恒为假 —— 第一拍之后永不再跳。
//  所以"非触发帧上 due < now + T"不是废话，它是这套计时唯一的死锁写法在这一层留下的指纹。
void RunDueTracking(Harness* h) {
    const double R = 0.0;
    const double D = 0.0;
    const double seconds = 60.0;
    const int frames = static_cast<int>(std::llround(seconds * dshb::kBeatFrameHz));
    const double T = dshb::BeatPeriodSeconds(R, D);

    BeatFixture fx;
    fx.Seed(R, D);

    double worstDue = 0.0;          // |due - (触发时刻 + T)|（应恰好 0）
    double minIdleGap = 1e300;      // 非触发帧上 (now + T) - due 的最小值（锚在 now 上是 0）
    double minDueMinusNow = 1e300;  // **非触发帧**上 due - now 的最小值（> 0 = 还没到点）
    double maxDueMinusNow = -1e300; // 同上取最大（<= T = due 最多领先一个周期）
    double worstLate = 0.0;         // 触发比 due 晚了多少（∈ [0, 一帧)）
    double worstNextDue = 0.0;      // 触发后下一帧的 due 与"触发时刻 + T"的最大偏差
    long long fired = 0;
    long long idle = 0;
    long long dueChanges = 0;
    double prevDue = 0.0;
    bool pendingFireCheck = false;
    double lastFireT = 0.0;
    for (int i = 0; i < frames; ++i) {
        const Tick k = fx.Step(kFrame, R, D);
        // ★ 这里**不能**在 Step 之后再读 fx.bucket.triggerSeconds 去拼期望值：触发那一帧它
        //   刚被改写，期望值会凭空老整整一个周期（曾在这里量出 15.0167 的"偏差"）。锚点由
        //   worstNextDue 钉（"触发后的下一帧 due == 触发时刻 + T"，恰好 0），那才是判据。
        if (!k.fired) {
            const double dn = k.dueTime - k.t;
            if (dn < minDueMinusNow) minDueMinusNow = dn;
            if (dn > maxDueMinusNow) maxDueMinusNow = dn;
        }

        // 触发后的**下一帧**：due 必须恰好是"触发时刻 + T"。
        if (pendingFireCheck) {
            const double nd = std::fabs(k.dueTime - (lastFireT + T));
            if (nd > worstNextDue) worstNextDue = nd;
            pendingFireCheck = false;
        }
        if (k.fired) {
            ++fired;
            const double late = k.t - k.dueTime;
            if (late > worstLate) worstLate = late;
            pendingFireCheck = true;
            lastFireT = k.t;
        } else {
            ++idle;
            const double gap = (k.t + k.period) - k.dueTime;
            if (gap < minIdleGap) minIdleGap = gap;
        }
        if (i == 0 || !BitSame(k.dueTime, prevDue)) ++dueChanges;
        prevDue = k.dueTime;
    }

    const bool ok = worstDue <= 1e-15 && idle > 0 && minIdleGap > 0.0 && fired > 1 &&
                    dueChanges > 1 && worstNextDue <= 1e-15 && worstLate < kFrame &&
                    minDueMinusNow > 0.0 && maxDueMinusNow <= T + 1e-9;
    h->Req("case5e",
           "计时：due 每帧都在追 T —— due == 触发时刻 + T（每帧用当前的 R/D 重算）；"
           "非触发帧上 due < now + T（锚在 now 上时这里恰好是 0，判据就永不成立）",
           "固定 R=" + F(R, 1) + "/D=" + F(D, 1) + "（T=" + F(T, 1) + " s）跑 " + F(seconds, 0) +
               " s = " + I(frames) + " 帧：触发 " + I(fired) + " 帧、非触发 " + I(idle) +
               " 帧；|due - (触发时刻 + T)| 的最大值 " + F(worstDue, 18) + "（应恰好 0）；"
               "非触发帧上 (now + T) - due 的最小值 " + F(minIdleGap, 9) + " s（> 0 = 没有锚在 now 上）；"
               "非触发帧上 due - now ∈ [" + F(minDueMinusNow, 9) + ", " + F(maxDueMinusNow, 9) +
               "] s（下界 > 0 = 还没到点，上界 <= T = due 最多领先一个周期）；触发比 due 最晚晚 " +
               F(worstLate, 9) + " s（< 一帧 " + F(kFrame, 9) + " s，所以过期就跳、不拖）；"
               "触发后下一帧的 due 与（触发时刻 + T）的最大偏差 " +
               F(worstNextDue, 18) + " s；due 在全程变过 " + I(dueChanges - 1) + " 次（每拍一次）",
           ok);
}

// ===========================================================================
// (6) 确定性
// ===========================================================================
void RunDeterminism(Harness* h) {
    // (a) 同一 (now, bucket) 反复求值逐位相同
    BeatBucket b;
    b.triggerSeconds = kT0;
    b.dueSeconds = dshb::BeatPeriodSeconds(0.7, 0.3);
    b.amplitudePx = dshb::BeatAmplitudePx(0.7, 0.3);
    const double now = kT0 + 0.077;
    const double first = dshb::BeatOffsetFromBucket(now, b);
    bool same = true;
    for (int i = 0; i < 2000; ++i) {
        if (!BitSame(first, dshb::BeatOffsetFromBucket(now, b))) {
            same = false;
            break;
        }
    }

    // (b) 从同一初始状态重建：整段序列逐位相同
    const std::vector<double> seqA = FixtureSequence(1.0, 0.0, 600);
    const std::vector<double> seqB = FixtureSequence(1.0, 0.0, 600);
    bool rebuild = seqA.size() == seqB.size();
    for (std::size_t i = 0; rebuild && i < seqA.size(); ++i) rebuild = BitSame(seqA[i], seqB[i]);
    // 这条不能是空检查：序列里必须真的有很多不同的值，而且换一拍必须变。
    long long distinct = 0;
    for (std::size_t i = 0; i < seqA.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i && !seen; ++j) seen = BitSame(seqA[i], seqA[j]);
        if (!seen) ++distinct;
    }
    BeatBucket other = b;
    other.triggerSeconds = kT0 - 0.02;   // 同样的 now，但这一拍不同 -> 位移必须不同
    const double otherY = dshb::BeatOffsetFromBucket(now, other);

    // (c) 位移与**绝对时间原点**无关：同一个 τ，触发时刻取 0 与取 1000 s 必须一致。
    //     τ 只扫到 kBeatLen − 1e-6：正好压在 kBeatLen 上时，(1000+τ)−1000 的最后一位
    //     可能把 τ 推到边界**外侧**（那里模块按约定返回恰好 0），差的就不是浮点噪声
    //     而是一个真实的跳变（1.15e-4 px）。那是边界语义，不是原点依赖，不该混进来。
    BeatBucket b0 = b;
    b0.triggerSeconds = 0.0;
    double worstOrigin = 0.0;
    const double originSpan = dshb::kBeatLen - 1e-6;
    for (int i = 0; i <= 200; ++i) {
        const double tau = originSpan * static_cast<double>(i) / 200.0;
        const double d = std::fabs(dshb::BeatOffsetFromBucket(tau, b0) -
                                   dshb::BeatOffsetFromBucket(kT0 + tau, b));
        if (d > worstOrigin) worstOrigin = d;
    }

    // (d) 诊断接口 CurrentBeatOnset：渲染循环不用它，但它是探针/自检的入口，
    //     所以它的三个字段必须和夹具自己的记账一致。
    const BeatOnset o = dshb::CurrentBeatOnset(now, b);
    BeatBucket zero = b;
    zero.amplitudePx = 0.0;
    const BeatOnset oz = dshb::CurrentBeatOnset(now, zero);
    const double yz = dshb::BeatOffsetFromBucket(now, zero);

    const bool diagOk = o.valid && o.atSeconds == b.triggerSeconds &&
                        o.tauSeconds == now - b.triggerSeconds && !oz.valid && yz == 0.0;
    const bool ok = same && rebuild && distinct > 10 && otherY != first && worstOrigin < 1e-9 &&
                    diagOk;
    h->Req("case6",
           "确定性：同一 (now, bucket) 逐位相同、同一初始状态重建逐位相同、位移与绝对时间原点无关",
           "同一点求值 2000 次逐位相同=" + YN(same) + "（样本 " + F(first, 17) + " px @ now=1000.077 s）；"
           "600 帧序列与重建序列逐位相同=" + YN(rebuild) + "（其中相异值 " + I(distinct) +
           " 个；同一 now 换一拍给 " + F(otherY, 12) + " px != " + F(first, 12) +
           " px，所以这不是一条恒真的空检查）；同一 τ∈[0, kBeatLen−1e-6] 在触发时刻 0 与 1000 s 上的最大差 " +
           F(worstOrigin, 15) + " px；诊断接口：valid=" + YN(o.valid) + "、atSeconds-trigger=" +
           F(o.atSeconds - b.triggerSeconds, 12) + " s、tauSeconds-(now-trigger)=" +
           F(o.tauSeconds - (now - b.triggerSeconds), 12) + " s；幅度 0 的拍 valid=" + YN(oz.valid) +
           " 且位移 " + F(yz, 12) + " px",
           ok);
}

// ===========================================================================
// (7a) 守门：两个中心都在 95 ms
// ===========================================================================
//  把第一个中心放到 0 时，触发那一刻位移就已经是 3.61 px（A_max 的 72%），
//  首帧落点最坏到 73.5% —— 上升段根本没被 60 Hz 采到。这条检查把那个对照数钉住，
//  防止以后有人"为了好看"把中心挪回 0。对照曲线是探针内部重算的，不是模块的输出。
struct FirstFrame {
    double fraction = 0.0;   // 占幅度的比例
    double at = 0.0;         // 出现在 τ（ms）
};

FirstFrame SweepFirstFrame(double mu1) {
    FirstFrame out;
    for (int q = 0; q < 1200; ++q) {
        const double ph = (q + 0.5) / 1200.0;
        const double tau = ph * kFrame;   // 触发之后第一个采样点
        const double f = ShapeWithMu1(tau, mu1);
        if (f > out.fraction) {
            out.fraction = f;
            out.at = tau * 1000.0;
        }
    }
    return out;
}

void RunCentersGate(Harness* h) {
    const bool equal = dshb::kBeatMu1 == dshb::kBeatMu2;
    const bool at95 = dshb::kBeatMu1 >= 0.090 && dshb::kBeatMu1 <= 0.100;

    const FirstFrame nowFirst = SweepFirstFrame(dshb::kBeatMu1);   // 现状：中心在 μ
    const FirstFrame zeroFirst = SweepFirstFrame(0.0);             // 对照：第一个中心在 0
    const double nowPct = 100.0 * nowFirst.fraction;
    const double zeroPct = 100.0 * zeroFirst.fraction;
    const double onsetNow = dshb::BeatShapeNorm(0.0);
    const double onsetZero = ShapeWithMu1(0.0, 0.0);

    const bool ok = equal && at95 && zeroPct > 70.0 && nowPct <= 35.0;
    h->Req("case7a", "守门：两个脉冲中心必须都在 95 ms（把第一个中心放回 0 -> 首帧跳变 73.5% 的对照）",
           "常量 mu1=" + F(dshb::kBeatMu1 * 1000.0, 1) + " ms、mu2=" +
               F(dshb::kBeatMu2 * 1000.0, 1) + " ms（相等=" + YN(equal) + "，规格 95 ms）"
               "；对照（探针内部重算，只挪第一个中心）：中心在 0 时 τ=0 的位移占幅度 " +
               F(100.0 * onsetZero, 2) + "%（A_max 下 " + F(onsetZero * dshb::kBeatAMax, 4) +
               " px），首帧最坏 " + F(100.0 * zeroFirst.fraction, 2) + "% @ τ=" + F(zeroFirst.at, 2) +
               " ms；现状（两个中心都在 " + F(dshb::kBeatMu1 * 1000.0, 0) + " ms）τ=0 的位移占幅度 " +
               F(100.0 * onsetNow, 2) + "%（" + F(onsetNow * dshb::kBeatAMax, 4) + " px），首帧最坏 " +
               F(nowPct, 2) + "% @ τ=" + F(nowFirst.at, 2) + " ms；倍数 " + F(zeroPct / nowPct, 2) + "x",
           ok);
}

// ===========================================================================
// (7b) 守门：脉冲不分段、无门闸
// ===========================================================================
//  ★ 老实说清楚本探针**量得出来**和**量不出来**的：
//    · fma 与朴素减法的差别在本参数下只有 1e-16 量级（u 是小数减大数，差在最后一位），
//      对形状的影响远低于任何可打印的位数 —— 探针**分不出**模块用的是哪一种。
//    · 量得出来的是那个**门闸**（`if (τ < μ) return 0;`）：μ=0.095 时 τ→0 落进门里，
//      函数返回 0，而正确值是 exp(-(0.095/σ)²/2)：第二个脉冲 0.164474、第一个 0.224941，
//      整个包络在 τ=0 处是 0.204785（峰值的 20.48%）—— 一个 20% 到 67% 的洞。
void RunPulseGate(Harness* h) {
    const double taus[8] = {0.0,
                            1e-18,
                            1e-12,
                            1e-9,
                            dshb::kBeatMu1 * 0.5,
                            dshb::kBeatMu1,
                            dshb::kBeatLen * 0.5,
                            dshb::kBeatLen};
    int naiveZeros = 0;
    int bitwiseAgree = 0;
    double worstHole = 0.0;
    double worstHoleAt = 0.0;
    double worstFmaDiff = 0.0;
    double atZeroModule = 0.0;
    double atZeroNaive = 0.0;
    for (double tau : taus) {
        const double got = dshb::BeatShapeNorm(tau);
        const double fma = FmaShapeNorm(tau);
        const double naive = NaiveShapeNorm(tau);
        const double fmaDiff = std::fabs(got - fma);
        if (fmaDiff > worstFmaDiff) worstFmaDiff = fmaDiff;
        if (BitSame(got, fma)) ++bitwiseAgree;
        if (naive == 0.0) ++naiveZeros;
        const double hole = std::fabs(got - naive);
        if (hole > worstHole) {
            worstHole = hole;
            worstHoleAt = tau;
        }
        if (tau == 0.0) {
            atZeroModule = got;
            atZeroNaive = naive;
        }
    }
    const double pulse1Zero = FmaPulse(0.0, dshb::kBeatMu1, dshb::kBeatSigma1);
    const double pulse2Zero = FmaPulse(0.0, dshb::kBeatMu2, dshb::kBeatSigma2);
    const double naivePulse2Zero = NaivePulse(0.0, dshb::kBeatMu2, dshb::kBeatSigma2);

    // 断言的是"模块 == 不分段无门闸的 fma 重算"这件事本身（差 < 1e-15），
    // 逐位相同的点数只作为证据印出来：fma 与朴素减法在最后一位上的差别取决于
    // 编译器怎么排布这两份同源代码，拿逐位相等去卡会卡到编译器头上。
    const bool ok = atZeroModule > 0.2 && atZeroNaive == 0.0 && naiveZeros == 5 &&
                    worstHole > 0.6 && worstFmaDiff < 1e-15 && pulse2Zero > 0.16 &&
                    naivePulse2Zero == 0.0;
    h->Req("case7b",
           "守门：脉冲不分段、无门闸 —— τ→0 时朴素写法（if (τ<μ) return 0）给 0，正确值 0.2048（包络）"
           "/ 0.1645（第二个脉冲）",
           "8 个 τ 点（0、1e-18、1e-12、1e-9、μ/2、μ、kBeatLen/2、kBeatLen）：模块与 fma 重算逐位相同 " +
               I(bitwiseAgree) + "/8，最大差 " + F(worstFmaDiff, 18) + "；朴素门闸写法在其中 " +
               I(naiveZeros) + " 个点上返回 0（全在 τ<μ 一侧），最大洞 " + F(worstHole, 9) +
               " @ τ=" + F(worstHoleAt * 1000.0, 3) + " ms（= 峰值的 " + F(100.0 * worstHole, 2) +
               "%，A_max 下 " + F(worstHole * dshb::kBeatAMax, 4) + " px）；τ=0 处：模块包络 " +
               F(atZeroModule, 9) + "、朴素 " + F(atZeroNaive, 9) + "，逐个脉冲 pulse2(0)=" +
               F(pulse2Zero, 9) + "（朴素 " + F(naivePulse2Zero, 9) + "，头文件写的 ≈0.165）、pulse1(0)=" +
               F(pulse1Zero, 9) + "；位移换算：" + F(atZeroModule * dshb::kBeatAMax, 4) + " px vs " +
               F(atZeroNaive * dshb::kBeatAMax, 4) + " px",
           ok);
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose") {
            verbose = true;
        } else {
            std::printf("beatprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: beatprobe [--verbose]\n");
            return 64;
        }
    }

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // 标题是中文；所有数字都是 ASCII
#endif

    Harness h;
    std::printf("beatprobe: 心跳位移波形、响应曲线与计时规则的离线证明（纯函数：无时钟、无文件、无全局状态）\n");
    std::printf("tuning: mu1=%.1f ms mu2=%.1f ms sigma1=%.1f ms sigma2=%.1f ms b2=%.2f "
                "shapePeak=%.3f beatLen=%.3f s\n",
                dshb::kBeatMu1 * 1000.0, dshb::kBeatMu2 * 1000.0, dshb::kBeatSigma1 * 1000.0,
                dshb::kBeatSigma2 * 1000.0, dshb::kBeatB2, dshb::kBeatShapePeak, dshb::kBeatLen);
    std::printf("        e(R)=1-(1-R)^%.2f | A: base=%.1f min=%.1f max=%.1f px | "
                "T: base=%.1f min=%.1f max=%.1f s | frame=%.0f Hz\n",
                dshb::kBeatRPow, dshb::kBeatABase, dshb::kBeatAMin, dshb::kBeatAMax,
                dshb::kBeatTBase, dshb::kBeatTMin, dshb::kBeatTMax, dshb::kBeatFrameHz);
    std::printf("scope: 量 src/heartbeat.cpp 的波形、响应曲线与两条律；生产层的计时循环在 "
                "src/widget_display.cpp 的 AdvanceBeat / TriggerBeatAt 里，本探针只把同一条规则当夹具"
                "复刻（due = 触发时刻 + T，每帧用当前的 R/D 重算），那个循环由主代理用 --beat-frame "
                "导帧验证\n");

    RunSelfCheck(&h);
    RunEnvelope(&h, verbose);
    RunJumpSweeps(&h);
    RunAmplitudeLaw(&h);
    RunResponseCurve(&h);
    RunPeriodLaw(&h);
    RunTimerCount(&h);
    RunTimerNoCatchUp(&h);
    RunTimerFasterWhenShorter(&h);
    RunSilenceWindow(&h);
    RunDueTracking(&h);
    RunDeterminism(&h);
    RunCentersGate(&h);
    RunPulseGate(&h);

    std::printf("\n%d passed, %d failed\n", h.passed, h.failed);
    if (!h.failures.empty()) {
        std::printf("failures:\n");
        for (const std::string& f : h.failures) {
            std::printf("  %s\n", f.c_str());
        }
    }
    std::printf(h.failed == 0 ? "beatprobe: ALL PASS\n" : "beatprobe: THERE ARE FAILURES\n");
    return h.failed == 0 ? 0 : 1;
}
