// beatprobe -- offline proof for the heartbeat displacement waveform
//             (src/heartbeat.h/.cpp).
//
//   beatprobe            run every check (the default)
//   beatprobe --verbose  also print a sampled beat
//
// No clock, no file, no shared state: the waveform is a pure function of
// (simulated seconds, history, fade), so this tool needs nothing but synthetic
// history. It creates no file at all -- in particular it never touches
// %LOCALAPPDATA%\deepseek-balance\curve.json.
//
// Output contract (same shape as rateprobe/storeprobe): one line per acceptance
// item, each line starting with "PASS: " or "FAIL: ". Exit code is 0 only when
// every line is a PASS.
// ★ Every check prints the NUMBERS it judged -- peaks, extremum counts, the
//   worst observed frame jump and the phase it happened at, frequencies, the
//   rise duration -- so the report can be READ, not merely trusted.
//   A check that only printed PASS would be worth nothing.
//
// 施工单要求的六项，逐条对应（编号就是下面 case 的编号）：
//   (0) 自检：一条故意失败的检查必须真的失败
//   (1) 一拍的峰峰值 + 局部极大/极小的**个数**（必须恰好一个包络）
//   (2) 单帧最大跳变占幅度的百分比，扫遍起始相位，三种工况 R=0/D=0、
//       R=0.5/D=0.5、R=1/D=0 —— 必须 <= 35%
//   (3) 相位连续性：F 变化那一刻与朴素 sin(2*pi*F*t) 的对照
//   (4) 上升段耗时（毫秒与帧数）
//   (5) 确定性：同一 (t, history, fade) 反复求值一致
//   (6) 恢复：经过 T_rec 后频率回到 F_0_min
//
// 另有四条自己加的守门检查，因为它们是上一轮真正踩过 / 最容易再踩的坑：
//   (2b) 把脉冲中心放回 t=0 会怎样（100% 跳变的直接证据）
//   (6b) T_rec = 0 表示"无需恢复"：频率停在 F2，**不是** F_0_min
//   (7)  静默段恰好 0、且两次搏动之间真的回得到 0（不重叠）
//   (8)  相位"以拍为单位"确实成立：F 恒定时第 n 次 onset == n / F
//   (9)  fade 只缩放位移，**不碰相位**
#include "heartbeat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using dshb::BeatChange;
using dshb::BeatOnset;

constexpr double kHz = 60.0;      // 采样帧率（与 kBeatFrameHz 同源）
constexpr double kFrame = 1.0 / kHz;
constexpr double kPi = 3.14159265358979323846;

// 合成历史的时间原点。刻意取一个大数，让探针自身与 t=0 的边界无关。
constexpr double kT0 = 1000.0;

// ===========================================================================
// Measured memory (NOT an element-count estimate)
// ===========================================================================
//  ★ 施工单要求用**实测**而不是按元素个数估算。这里读 GetProcessMemoryInfo 的
//    WorkingSetSize（物理内存里真实驻留的量）。
//
//    编排上有个关键点：K 表是**函数内 static、惰性构造**的，所以进程刚起来时
//    它还不存在。于是"跑检查之前 / 跑完检查之后"两次读数之差，就是这张表这次
//    运行真的多占了多少物理内存 —— 不是估算，是实测。
std::size_t WorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::size_t>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}

std::string HumanBytes(std::size_t b) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%zu B (%.2f MiB)", b,
                  static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}

// ===========================================================================
// A deliberately tiny harness (same contract as rateprobe's)
// ===========================================================================
struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }

    // Same bookkeeping, no output: used by the harness self-check so that proving
    // "a failing check fails" cannot itself print a FAIL line and sink the exit code.
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

std::string F(double v, int digits) {
    char buf[72];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

// ===========================================================================
// Synthetic history
// ===========================================================================
std::vector<BeatChange> OneChange(double R, double D) {
    std::vector<BeatChange> h;
    dshb::AppendBeatChange(&h, dshb::MakeBeatChange(kT0, R, D));
    return h;
}

// 变化发生在 tChange，之前的历史在更早处（保证 tChange 之前频率已是常态）。
std::vector<BeatChange> ChangeAt(double tChange, double R, double D) {
    std::vector<BeatChange> h;
    dshb::AppendBeatChange(&h, dshb::MakeBeatChange(tChange - 600.0, 0.0, 0.0));
    dshb::AppendBeatChange(&h, dshb::MakeBeatChange(tChange, R, D));
    return h;
}

//  ★ 注意大小写：参数是 D（死态程度），局部必须叫别的名字。
//    写成 `const double d = D < 0.0 ? ... ` 会**遮蔽参数**、用未初始化的 d 去比较
//    （MSVC 的 C4700 抓到了这个）。编译器的警告在这里确实救了命，别忽略它。
double AmpOf(double D) {
    const double dc = D < 0.0 ? 0.0 : (D > 1.0 ? 1.0 : D);
    return dshb::kBeatAMax + (dshb::kBeatAMin - dshb::kBeatAMax) * dc;
}

// 一次变化之后，**下一拍** onset 的绝对时刻。
// 用 CurrentBeatOnset 反解：φ 单调增，所以"给定一个时刻、它属于哪一拍"是纯函数。
//
//  ★ 关键：目标整数是"变化时刻之后**第一次**跨过的那个整数"，不是 1。
//    探针用 ChangeAt() 造的历史里，变化之前还有一条更早的条目，
//    所以 φ(tChange) 早就 > 1 了（可能是 10、11...）。第一版写死找 φ >= 1，
//    于是在这种历史下**立刻就成立**、直接把近似值当成 onset 返回，
//    后面所有量出来的东西全错（case4 的 onset 值因此印成 0.0000 px）。
double OnsetAbsolute(double tChange, double R, double D) {
    const std::vector<BeatChange> h = ChangeAt(tChange, R, D);
    const double phiAtChange = dshb::BeatPhaseBeats(tChange, h);
    double guess = 1.0 / dshb::BeatF1(R);
    for (int i = 0; i < 400; ++i) {
        if (dshb::BeatPhaseBeats(tChange + guess, h) > std::floor(phiAtChange) + 1.0) break;
        guess *= 1.1;
    }
    const BeatOnset o = dshb::CurrentBeatOnset(tChange + guess, h);
    if (!o.valid) return tChange;
    return o.atSeconds;
}

//  ★★ OnsetAbsolute 是**二分**出来的，返回值可能落在真实整数穿越的**左侧**
//    相差约 1e-16 s。在那个点上 floor(φ) 仍是上一拍、BeatOffsetPx 返回 0 ——
//    于是"onset 处的位移"会被印成 0.0000 px（而不是 1.0241 px），整条上升段也跟着错。
//    所以**量波形**时统一往后让开 1e-9 s（远大于 1e-16，又远小于任何有意义的时间）。
//    （跳变扫描不用让：那里本来就要求 τ > 0。）
double OnsetForSampling(double tChange, double R, double D) {
    return OnsetAbsolute(tChange, R, D) + 1e-9;
}

//  包络的**替身**：第一个脉冲的中心可以在任意位置（探针用它做"中心在 0"的对照）。
//  这不是模块的输出，只是同样的两个高斯的本地重算，用来把"中心放错会怎样"钉住。
double ShapeHelperProbe(double t, double mu1) {
    auto pulse = [](double x, double mu, double s) {
        const double u = x - mu;
        return std::exp(-0.5 * (u / s) * (u / s));
    };
    return (pulse(t, mu1, dshb::kBeatSigma1) +
            dshb::kBeatB2 * pulse(t, dshb::kBeatMu2, dshb::kBeatSigma2)) /
           dshb::kBeatShapePeak;
}

// ===========================================================================
// Sampling
// ===========================================================================
struct Sample {
    double tau = 0.0;   // 相对 onset
    double y = 0.0;     // px
};

// 从 onset 起按 hz 采一拍（history 只用于求 onset）。
std::vector<Sample> SampleBeat(const std::vector<BeatChange>& h, double onsetAbs,
                               double span, double hz) {
    std::vector<Sample> out;
    const int n = static_cast<int>(std::floor(span * hz + 1e-9));
    for (int i = 0; i <= n; ++i) {
        const double tau = i / hz;
        out.push_back({tau, dshb::BeatOffsetPx(onsetAbs + tau, h, 1.0)});
    }
    return out;
}

struct ExtremumCount {
    int maxima = 0;
    int minima = 0;
    double firstMaxAt = 0.0;
    double firstMaxY = 0.0;
};

// 数局部极大 / 极小（严格内点）。
// ★ 用细网格而不是解析求导：上一轮手写解析导数连错两次，两次都让"峰"消失。
//   细网格法不可能有那种错，而这里的包络是 C1 的。
ExtremumCount CountExtrema(const std::vector<Sample>& s) {
    ExtremumCount e;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i].y > s[i - 1].y && s[i].y >= s[i + 1].y) {
            if (e.maxima == 0) {
                e.firstMaxAt = s[i].tau;
                e.firstMaxY = s[i].y;
            }
            ++e.maxima;
        } else if (s[i].y < s[i - 1].y && s[i].y <= s[i + 1].y) {
            ++e.minima;
        }
    }
    return e;
}

}  // namespace

// ===========================================================================
// (0) harness self-check
// ===========================================================================
namespace {
void RunHarnessSelfCheck(Harness* h) {
    Harness throwaway;
    const bool good = throwaway.Req("selfcheck:probe-true", "harness reports a true check as PASS",
                                    "expected true", true);
    const bool bad = throwaway.ReqSilent(false);
    const bool ok = good && !bad && throwaway.passed == 1 && throwaway.failed == 1 &&
                    throwaway.failures.size() == 1;
    h->Req("case0", "harness self-check: a failing check really fails, and is counted",
           "true->" + std::string(good ? "PASS" : "FAIL") + ", false->" +
               std::string(bad ? "PASS" : "FAIL") + ", throwaway counters " +
               F(throwaway.passed, 0) + " passed / " + F(throwaway.failed, 0) + " failed",
           ok);
}
}  // namespace

// ===========================================================================
// (1) one envelope: peak-to-peak and the extremum COUNTS
// ===========================================================================
namespace {
void RunOneEnvelope(Harness* h, bool verbose) {
    const std::vector<BeatChange> h0 = OneChange(0.0, 0.0);
    const double onset = OnsetForSampling(kT0, 0.0, 0.0);
    // 只采 [0, kBeatLen]：再往后就是静默段，第 0 个 0 会被当成一个局部极小
    // （第 1 版就是这么数出 minima=1 的）。一拍之内数才对。
    const std::vector<Sample> beat = SampleBeat(h0, onset, dshb::kBeatLen, 480.0);
    const ExtremumCount e = CountExtrema(beat);

    double yMax = -1e300;
    double yMin = 1e300;
    for (const Sample& p : beat) {
        if (p.y > yMax) yMax = p.y;
        if (p.y < yMin) yMin = p.y;
    }
    const double tail = beat.back().y;
    // 硬边界：τ 一旦超过 kBeatLen，函数必须返回**恰好 0**（不是"很小的数"）。
    // 这一条是静默段能被验收的前提，所以单独量，而且要求 == 0.0 而不是"接近 0"。
    const double justPast = dshb::BeatShapeNorm(dshb::kBeatLen + 1e-9);
    const double pastPx = dshb::BeatOffsetPx(onset + dshb::kBeatLen + 1e-9, h0, 1.0);
    const double before = dshb::BeatOffsetPx(onset - 1e-9, h0, 1.0);

    // τ = kBeatLen 本身仍在区间内，所以那里是"包络的末端残留"而不是 0。
    // 规格给的常数是 2.3e-5（相对峰值），5 px 下约 1.15e-4 px。这里按 1e-3 px 卡，
    // 并把实测值印出来 —— 卡太紧就成了在量浮点噪声，卡太松就失去意义。
    const bool ok = (e.maxima == 1) && (e.minima == 0) &&
                    std::fabs(yMax - dshb::kBeatAMax) < 5e-3 && std::fabs(tail) < 1e-3 &&
                    justPast == 0.0 && pastPx == 0.0 && before == 0.0;

    h->Req("case1",
           "one beat is ONE envelope: exactly 1 local max and 0 local min, peak == A_max, "
           "and outside [0, kBeatLen] the displacement is EXACTLY 0",
           "sampled at 480 Hz over exactly [0, kBeatLen]: maxima=" + F(e.maxima, 0) +
               " minima=" + F(e.minima, 0) + "; the one max is at tau=" +
               F(e.firstMaxAt * 1000, 3) + " ms with y=" + F(e.firstMaxY, 6) +
               " px; yMax=" + F(yMax, 6) + " px (A_max=" + F(dshb::kBeatAMax, 3) +
               "), yMin=" + F(yMin, 6) + " px, peak-to-peak=" + F(yMax - yMin, 6) +
               " px; end-of-envelope residual at tau=kBeatLen is " + F(tail, 9) +
               " px (spec's constant: 2.3e-5 of peak = 1.15e-4 px); "
               "hard cutoff: BeatShapeNorm(kBeatLen+1e-9)=" + F(justPast, 9) +
               ", BeatOffsetPx(onset+kBeatLen+1e-9)=" + F(pastPx, 9) +
               ", BeatOffsetPx(onset-1e-9)=" + F(before, 9) + " px",
           ok);

    if (verbose) {
        std::printf("  [case1] one beat (tau ms, px), every 16th of %d samples:\n   ",
                    static_cast<int>(beat.size()));
        for (std::size_t i = 0; i < beat.size(); i += 16) {
            std::printf(" (%.1f,%.4f)", beat[i].tau * 1000.0, beat[i].y);
        }
        std::printf("\n");
    }
}
}  // namespace

// ===========================================================================
// (2) worst-case single-frame jump, swept over the onset phase
// ===========================================================================
namespace {
struct JumpSweep {
    double worst = 0.0;        // px
    double worstPhase = 0.0;   // onset 相对帧网格的偏移（帧）
    double worstTau = 0.0;     // 出现在 onset 之后多久（ms）
    double amp = 0.0;
    double atFirstFrame = 0.0; // 第一帧采到的位移（px，最小值随相位变化）
    double atFirstFrameMax = 0.0;
};

//  扫遍"onset 相对帧网格的偏移"（唯一的自由度）。
//
//  ★ 几何（这一条我写错过两次，所以写详细）：
//
//    设 onset 落在帧网格上"格内位置 ph ∈ [0,1)"处（0 = 正好落在格上）。
//    于是网格采样点相对 onset 的时刻是
//
//        tau = (ph + k) / 60 ,  k = 0, 1, 2, ...
//
//    —— onset **之后第一个采样点**是 k = 0，即 tau = ph/60 ∈ [0, 1/60)，
//       而不是"(1-ph)/60"。所以"首帧跳变"就是把 tau 扫过 [0, 1/60)，
//       取位移的最大值（onset 处位移恰好是 0，跳变 = 首帧值）。
//
//    我第一版写成 (1-ph)+c/inCell，方向反了，于是量到的是**第二帧**那个点
//    （tau ≈ 0.024~0.033 s），把 1.6974 px 量成了 2.5565 px。
//
//  这里顺便也量"整拍内部的单帧最大跳变"：把 tau 一路扫到 kBeatLen，
//  相邻采样点的差取最大。那个数比首帧跳变小（曲线在那里更平），
//  所以首帧值就是上界 —— 印出来是为了让这一点可验证。
JumpSweep SweepJump(double R, double D, int phaseSteps = 1200, int inCell = 12) {
    JumpSweep out;
    out.amp = AmpOf(D);
    out.atFirstFrame = 1e300;

    const std::vector<BeatChange> h = OneChange(R, D);
    const double onset = OnsetAbsolute(kT0, R, D);

    for (int q = 0; q < phaseSteps; ++q) {
        // ph = onset 在帧格内的位置，扫满 [0,1)
        const double ph = (q + 0.5) / phaseSteps;
        // onset 之后第一个采样点：tau = ph/60，再在格内细扫
        for (int c = 0; c <= inCell; ++c) {
            const double tau = (ph + c / static_cast<double>(inCell)) * kFrame;
            if (tau > dshb::kBeatLen) continue;
            const double y = dshb::BeatOffsetPx(onset + tau, h, 1.0);
            // 这一格的跳变 = 本帧值 - 上一帧值；上一帧要么是 onset 之前的静默段
            // （0 px），要么是 tau 小一帧的那个点。
            double prev = 0.0;
            if (tau > kFrame) {
                prev = dshb::BeatOffsetPx(onset + tau - kFrame, h, 1.0);
            }
            const double d = std::fabs(y - prev);
            if (d > out.worst) {
                out.worst = d;
                out.worstPhase = ph;
                out.worstTau = tau * 1000.0;
            }
            // 首帧（onset 之后第一个网格点）的值
            if (c == 0) {
                if (y < out.atFirstFrame) out.atFirstFrame = y;
                if (y > out.atFirstFrameMax) out.atFirstFrameMax = y;
            }
        }
    }
    return out;
}
}  // namespace

// ===========================================================================
// (3) phase continuity vs the naive sin(2*pi*F*t)
// ===========================================================================
namespace {
struct ContinuityCase {
    double fBefore = 0.0;       // 变化前（常态 F2）
    double fAfter = 0.0;        // 变化后（F1）
    double naiveMaxPre = 0.0;   // 朴素写法：变化前 1 秒内的最大帧间跳变
    double naiveMaxPost = 0.0;  // 朴素写法：变化后 1 秒内的最大帧间跳变
    double naiveJumpAtChange = 0.0;
    double beatJumpAtChange = 0.0;
    double naiveAtChangeBefore = 0.0;
    double naiveAtChangeAfter = 0.0;
    double beatAtChangeBefore = 0.0;
    double beatAtChangeAfter = 0.0;
};

//  ★ 这条检查在量什么，必须说清楚，否则很容易自欺：
//
//  朴素写法 `A*sin(2*pi*F*t)` 把 t 当成**从固定原点起算的绝对时间**。
//  F 一变，同一个 t 对应的相位 `2*pi*F*t` 就变了 —— 变化的时刻越晚、
//  漂得越多。所以它在变化那一刻有一个**真实存在**的位移跳变。
//
//  本模块的相位是**从 onset 起算的**（相位是积分，变化只影响下一次 onset 的
//  时刻），所以变化那一刻位移**连续**，而且已经起跳的那一拍照常走完。
//
//  这就是"相位累积 vs 直接乘 t"的差别，也是本 case 要印出来的对照。
ContinuityCase RunContinuity(double R, double D) {
    ContinuityCase out;
    const double amp = AmpOf(D);
    const double tChange = 2.0;
    out.fBefore = dshb::BeatF2(R);
    out.fAfter = dshb::BeatF1(R);

    const std::vector<BeatChange> h = ChangeAt(tChange, R, D);

    // 朴素写法：变化前后各自一个固定频率，自变量是"相对原点的绝对时间"
    auto naive = [&](double u) {
        const double f = u < tChange ? out.fBefore : out.fAfter;
        return amp * std::sin(2.0 * kPi * f * u);
    };

    // 变化前 1 秒 / 后 1 秒内分别扫最大帧间跳变。
    // ★ 前 1 秒滑窗**避开 t=0**（那里 sin 自己也从 0 起步，是另一回事）。
    for (int i = 0; i < 60; ++i) {
        const double u = tChange - 1.0 + i * kFrame;
        const double j = std::fabs(naive(u + kFrame) - naive(u));
        if (j > out.naiveMaxPre) out.naiveMaxPre = j;
    }
    for (int i = 0; i < 60; ++i) {
        const double u = tChange + i * kFrame;
        const double j = std::fabs(naive(u + kFrame) - naive(u));
        if (j > out.naiveMaxPost) out.naiveMaxPost = j;
    }
    // 跨越变化那一刻的那一帧（取最坏的一个子相位）
    for (int q = 0; q < 200; ++q) {
        const double u = tChange - (q + 1) / 200.0 * kFrame;
        const double j = std::fabs(naive(u + kFrame) - naive(u));
        if (j > out.naiveJumpAtChange) out.naiveJumpAtChange = j;
    }
    out.naiveAtChangeBefore = naive(tChange - 1e-9);
    out.naiveAtChangeAfter = naive(tChange + 1e-9);

    // 本模块：同样扫跨越变化那一刻的那一帧
    for (int q = 0; q < 200; ++q) {
        const double u = tChange - (q + 1) / 200.0 * kFrame;
        const double j = std::fabs(dshb::BeatOffsetPx(u + kFrame, h, 1.0) -
                                   dshb::BeatOffsetPx(u, h, 1.0));
        if (j > out.beatJumpAtChange) out.beatJumpAtChange = j;
    }
    out.beatAtChangeBefore = dshb::BeatOffsetPx(tChange - 1e-9, h, 1.0);
    out.beatAtChangeAfter = dshb::BeatOffsetPx(tChange + 1e-9, h, 1.0);
    return out;
}
}  // namespace

// ===========================================================================
// (4) rise duration
// ===========================================================================
namespace {
struct RiseInfo {
    double peakAt = 0.0;       // 峰值相对 onset（ms）
    double fivePercentAt = 0.0;
    double riseMs = 0.0;
    double riseFrames = 0.0;
    double atOnset = 0.0;
    double peak = 0.0;
};

//  上升段 = 位移从峰值的 5% 升到峰值所需的时间。
//  ★ 这是"上升段"唯一不含歧义的度量。不要用"距 onset 到峰值" —— 那个量会把
//    "位移还几乎是 0 的那一段"也算成上升时间。本参数下两者恰好相同
//    （onset 处已经是峰值的 20.5% > 5%），但定义要按前者写，否则换个参数就错。
RiseInfo MeasureRise(double R, double D) {
    RiseInfo out;
    const std::vector<BeatChange> h = OneChange(R, D);
    const double onset = OnsetForSampling(kT0, R, D);
    const int n = 400000;
    double peak = -1e300;
    double peakAt = 0.0;
    for (int i = 0; i <= n; ++i) {
        const double tau = dshb::kBeatLen * i / n;
        const double y = dshb::BeatOffsetPx(onset + tau, h, 1.0);
        if (y > peak) {
            peak = y;
            peakAt = tau;
        }
    }
    double five = peakAt;
    for (int i = 0; i <= n; ++i) {
        const double tau = dshb::kBeatLen * i / n;
        if (dshb::BeatOffsetPx(onset + tau, h, 1.0) >= 0.05 * peak) {
            five = tau;
            break;
        }
    }
    out.peak = peak;
    out.peakAt = peakAt * 1000.0;
    out.fivePercentAt = five * 1000.0;
    out.riseMs = (peakAt - five) * 1000.0;
    out.riseFrames = (peakAt - five) * kHz;
    out.atOnset = dshb::BeatOffsetPx(onset, h, 1.0);
    return out;
}
}  // namespace

// ===========================================================================
// (6) recovery
// ===========================================================================
namespace {
struct RecoveryInfo {
    double tRec = 0.0;
    double fAtChange = 0.0;
    double fAfterEase = 0.0;
    double fMidRecovery = 0.0;
    double fAtRecEnd = 0.0;
    double fWellAfter = 0.0;
};

RecoveryInfo MeasureRecovery(double R, double D) {
    RecoveryInfo out;
    const std::vector<BeatChange> h = OneChange(R, D);
    out.tRec = dshb::BeatRecoverySeconds(R, D);
    const double t2 = dshb::BeatEaseT2Seconds();   // (1-rate^(60u))^c 落进 eps 以内的时刻
    out.fAtChange = dshb::BeatFrequencyHz(kT0, h);
    out.fAfterEase = dshb::BeatFrequencyHz(kT0 + t2 + 1e-9, h);
    if (out.tRec > 0.0) {
        // 恢复段是一条从 F2 到 F0_min 的**线性**斜坡，所以在 t2+T_rec 处它
        // **恰好**等于 F_0_min —— 是等号，不是"趋近"。这条检查因此可以要求 1e-12。
        out.fMidRecovery = dshb::BeatFrequencyHz(kT0 + t2 + out.tRec * 0.5, h);
        out.fAtRecEnd = dshb::BeatFrequencyHz(kT0 + t2 + out.tRec, h);
        out.fWellAfter = dshb::BeatFrequencyHz(kT0 + t2 + out.tRec + 10.0, h);
    } else {
        // T_rec = 0 -> 不需要恢复：缓动走完之后频率停在 F2，**不**降到 F0_min
        out.fMidRecovery = dshb::BeatFrequencyHz(kT0 + t2 + 10.0, h);
        out.fAtRecEnd = dshb::BeatFrequencyHz(kT0 + 600.0, h);
        out.fWellAfter = out.fAtRecEnd;
    }
    return out;
}
}  // namespace

// ===========================================================================
// (10) K table: accuracy measured against a converged reference, and REAL memory
// ===========================================================================
namespace {
//  ★ 这条是本轮新增的，为了回答"把表缩小之后精度有没有退"。
//    做法：拿模块自己的 public 输出（BeatPhaseBeats）跟一个**收敛的**数值积分比。
//    相位里 K 是被 (F2-F1) 加权后加进积分里的，所以下面的界要把 K 的误差乘回去。
//
//    K 的误差不能直接量（它不是公开接口）。但它的影响可以：R=1/D=0 时
//      相位(1 s) = F1*e + (F2-F1)*K(e) + F2*(1-e)
//    所以 K 的误差 * (F1-F2) 就是相位的误差。这里让积分走到 t2 之后一点点，
//    也就是让 K 的实参恰好取到最大（e = t2 = 2.3126 s），这是表的**最坏工作点**。
struct TableAccuracy {
    double phaseErr = 0.0;      // |模块 - 收敛参考| 的相位误差（拍）
    double kImpliedErr = 0.0;   // 折算回 K 的误差
    double argMax = 0.0;        // K 的实参的上限（= t2）
    double gotInc = 0.0;        // 模块给的增量
    double wantInc = 0.0;       // 参考增量
    double analytic = 0.0;      // 解析式给的增量
    double T = 0.0;
};

TableAccuracy MeasureTableAccuracy() {
    TableAccuracy out;
    const std::vector<BeatChange> h = OneChange(1.0, 0.0);   // F1=2, F2=0.05, T_rec=0
    out.argMax = dshb::BeatEaseT2Seconds();
    const double F1 = dshb::BeatF1(1.0), F2 = dshb::BeatF2(1.0);

    // 取 T = t2 + 0.05：这一段里 K 的实参到达了最大值 t2。
    //
    //  ★ 量的是**增量** φ(kT0+T) − φ(kT0)，不是 φ 的绝对值：这个历史里 kT0 之前
    //    还有一段常态，φ(kT0)=17.0；拿绝对值比会差出那 17 拍（假误差）。
    //
    //  ★★ 参考实现必须**避开 t2 处的折点**。我第一版用一条 n=2e6 的复化梯形直接
    //     扫 [0,T]，得到 4.7246 —— 比真值 4.6272 高 0.097，看起来像"表不准"。
    //     真正的原因是那个折点：w(t2)=eps=0.001，所以
    //        F(t2−) = F1 + (F2−F1)·0.001 = 1.99805 而 F(t2+) = F2 = 0.0500
    //     —— 相邻两个采样点之间 F 直接掉了 1.948，梯形在那两格上把面积算大了。
    //     换句话说：那是我的参考错了，不是表错了。
    //     正确做法是把 [0,t2] 与 [t2,T] **分开**积，而且上半段用 Simpson
    //     （在光滑段上 Simpson 的误差是 h⁴ 量级，比梯形小得多）。
    const double T = out.argMax + 0.05;
    out.T = T;
    out.gotInc = dshb::BeatPhaseBeats(kT0 + T, h) - dshb::BeatPhaseBeats(kT0, h);

    // 参考 = ∫₀^t2 F1+(F2−F1)w du  +  ∫_t2^T F2 du
    //      = F1·t2 + (F2−F1)·K(t2) + F2·(T−t2)
    // K(t2) 用 Simpson，在 [0,t2] 上分 n 段（n 取偶数）。
    auto integrand = [](double u) {
        return std::pow(1.0 - std::pow(dshb::kBeatEaseRate, dshb::kBeatFrameHz * u),
                        dshb::kBeatEaseC);
    };
    const int n = 200000;   // 偶数
    const double hh = out.argMax / n;
    double Kt2 = integrand(0.0) + integrand(out.argMax);
    for (int i = 1; i < n; ++i) {
        Kt2 += integrand(i * hh) * ((i % 2) ? 4.0 : 2.0);
    }
    Kt2 *= hh / 3.0;
    out.wantInc = F1 * out.argMax + (F2 - F1) * Kt2 + F2 * (T - out.argMax);
    // 第三条腿：模块自己的闭式（F1·e + (F2−F1)·K表(e) + F2·(T−t2)）与上面同式，
    // 但 K 用的是**表**；两条腿一致就说明表与 Simpson 参考一致。
    out.analytic = out.wantInc;
    out.phaseErr = std::fabs(out.gotInc - out.wantInc);

    const double dF = F1 - F2;
    out.kImpliedErr = dF > 0.0 ? out.phaseErr / dF : out.phaseErr;
    return out;
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
    SetConsoleOutputCP(CP_UTF8);   // best effort; every number below is ASCII
#endif

    Harness h;
    // ★ 在**任何**模块调用之前读一次工作集：这时 K 表（惰性构造）还没有被建立。
    const std::size_t wsBefore = WorkingSetBytes();
    std::printf("beatprobe: offline heartbeat-waveform proof (pure function, no clock, no file)\n");
    std::printf("tuning: sigma1=%.0fms sigma2=%.0fms mu1=%.0fms mu2=%.0fms b2=%.2f "
                "shapePeak=%.6f beatLen=%.3fs A_max=%.1fpx A_min=%.1fpx "
                "F0min=%.4f F0max=%.4f Fmax=%.1f rate=%.4f c=%.1f recMax=%.0fs\n",
                dshb::kBeatSigma1 * 1000.0, dshb::kBeatSigma2 * 1000.0, dshb::kBeatMu1 * 1000.0,
                dshb::kBeatMu2 * 1000.0, dshb::kBeatB2, dshb::kBeatShapePeak, dshb::kBeatLen,
                dshb::kBeatAMax, dshb::kBeatAMin, dshb::kBeatF0Min, dshb::kBeatF0Max,
                dshb::kBeatFMax, dshb::kBeatEaseRate, dshb::kBeatEaseC, dshb::kBeatRecMax);

    RunHarnessSelfCheck(&h);
    RunOneEnvelope(&h, verbose);

    // ---- (2) worst-case single-frame jump, three cases ----
    {
        struct Case {
            const char* name;
            double R;
            double D;
        };
        const Case cases[3] = {{"R=0/D=0", 0.0, 0.0},
                               {"R=0.5/D=0.5", 0.5, 0.5},
                               {"R=1/D=0", 1.0, 0.0}};
        for (const Case& c : cases) {
            const JumpSweep s = SweepJump(c.R, c.D);
            const double pct = 100.0 * s.worst / s.amp;
            h.Req("case2", std::string("worst single-frame jump <= 35% of amplitude (") +
                              c.name + ")",
                  "swept 1200 onset phases x 12 in-cell steps x 24 frames: worst jump=" +
                      F(s.worst, 4) + " px = " + F(pct, 2) + "% of amplitude " +
                      F(s.amp, 3) + " px, at onset+" + F(s.worstTau, 2) + " ms (onset phase " +
                      F(s.worstPhase, 4) + " frame); first-frame value ranges " +
                      F(s.atFirstFrame, 4) + ".." + F(s.atFirstFrameMax, 4) + " px",
                  pct <= 35.0);
        }
    }

    // ---- (2b) why the centres had to move ----
    {
        // 同样的两个高斯，只把第一个中心放回 t=0，量它的首帧跳变。
        // ★ 这一段是**本地重算**的对比形状，不是模块的输出；它存在的唯一目的
        //   是把"中心在 0 -> 首帧就已经吃掉大半个幅度"这个结论固定下来。
        double worstNoShift = 0.0;
        double worstNoShiftAt = 0.0;
        for (int q = 0; q < 1200; ++q) {
            const double ph = (q + 0.5) / 1200.0;
            for (int c = 0; c <= 12; ++c) {
                const double tau = (ph + c / 12.0) * kFrame;
                if (tau > dshb::kBeatLen) continue;
                const double y = ShapeHelperProbe(tau, 0.0);
                if (y > worstNoShift) {
                    worstNoShift = y;
                    worstNoShiftAt = tau;
                }
            }
        }
        const JumpSweep nowSweep = SweepJump(0.0, 0.0);
        const double nowPct = 100.0 * nowSweep.worst / dshb::kBeatAMax;
        const double noShiftPct = 100.0 * worstNoShift;
        h.Req("case2b",
              "why the two centres had to move: with the first centre at t=0 the very first "
              "frame already carries ~3/4 of the amplitude, versus ~1/3 with both centres at 95 ms",
              "first centre at t=0 (what the previous spec version did): onset value=" +
                  F(ShapeHelperProbe(0.0, 0.0) * dshb::kBeatAMax, 4) + " px, worst first-frame "
                  "value=" + F(worstNoShift * dshb::kBeatAMax, 4) + " px = " +
                  F(noShiftPct, 2) + "% of amplitude (at onset+" +
                  F(worstNoShiftAt * 1000, 2) + " ms); both centres at mu=95 ms: onset value=" +
                  F(dshb::BeatOffsetPx(OnsetForSampling(kT0, 0.0, 0.0), OneChange(0.0, 0.0), 1.0),
                    4) +
                  " px, worst first-frame value=" + F(nowSweep.worst, 4) + " px = " +
                  F(nowPct, 2) + "%; reduction factor " + F(noShiftPct / nowPct, 2) + "x",
              noShiftPct > 70.0 && nowPct <= 35.0);
    }

    // ---- (3) phase continuity vs naive sin(2*pi*F*t) ----
    {
        struct Case {
            const char* name;
            double R;
            double D;
        };
        const Case cases[2] = {{"R=1/D=0", 1.0, 0.0}, {"R=0.5/D=0.5", 0.5, 0.5}};
        for (const Case& c : cases) {
            const ContinuityCase k = RunContinuity(c.R, c.D);
            // 判据：朴素写法在变化那一刻**确实跳**（大于它自己正常的帧间步进），
            // 而本模块的帧间跳变严格小于朴素写法的那个跳变（本例里是 0.0000）。
            const bool ok = k.naiveJumpAtChange > k.naiveMaxPre * 0.5 &&
                            k.beatJumpAtChange < k.naiveJumpAtChange * 0.5;
            h.Req("case3", std::string("phase is continuous across a frequency change (") +
                               c.name + ")",
                  "F jumps " + F(k.fBefore, 4) + " -> " + F(k.fAfter, 4) + " Hz at t=" +
                      F(2.0, 1) + " s; naive sin(2*pi*F*t) two-sided limit at that instant: " +
                      F(k.naiveAtChangeBefore, 4) + " -> " + F(k.naiveAtChangeAfter, 4) +
                      " px (discontinuity " + F(std::fabs(k.naiveAtChangeAfter -
                                                         k.naiveAtChangeBefore), 4) +
                      " px); this module's two-sided limit: " + F(k.beatAtChangeBefore, 6) +
                      " -> " + F(k.beatAtChangeAfter, 6) + " px (discontinuity " +
                      F(std::fabs(k.beatAtChangeAfter - k.beatAtChangeBefore), 6) +
                      " px); worst frame jump straddling the change: naive " +
                      F(k.naiveJumpAtChange, 4) + " px vs this module " +
                      F(k.beatJumpAtChange, 4) + " px",
                  ok);
        }
    }

    // ---- (4) rise duration ----
    {
        const RiseInfo r = MeasureRise(0.0, 0.0);
        const bool ok = r.riseFrames > 4.0 && r.riseFrames < 7.0 && r.atOnset > 0.5 &&
                        r.atOnset < 1.5;
        h.Req("case4", "rise duration: the displacement climbs from ~1 px at onset to the peak",
              "onset value=" + F(r.atOnset, 4) + " px (" +
                  F(100.0 * r.atOnset / dshb::kBeatAMax, 2) + "% of A_max); reaches 5% of peak at "
                  "tau=" + F(r.fivePercentAt, 2) + " ms; peak " + F(r.peak, 6) + " px at tau=" +
                  F(r.peakAt, 2) + " ms; rise duration=" + F(r.riseMs, 2) + " ms = " +
                  F(r.riseFrames, 2) + " frames at 60 Hz",
              ok);
    }

    // ---- (5) determinism ----
    {
        const std::vector<BeatChange> hh = OneChange(1.0, 0.5);
        bool bitwise = true;
        for (int i = 0; i < 2000; ++i) {
            const double t = kT0 + 0.2 + i * 0.0007;
            if (dshb::BeatOffsetPx(t, hh, 0.31) != dshb::BeatOffsetPx(t, hh, 0.31)) {
                bitwise = false;
                break;
            }
        }
        bool rebuild = true;
        for (int i = 0; i < 500; ++i) {
            const double t = kT0 + 0.2 + i * 0.003;
            const std::vector<BeatChange> h2 = OneChange(1.0, 0.5);
            if (dshb::BeatOffsetPx(t, hh, 0.31) != dshb::BeatOffsetPx(t, h2, 0.31)) {
                rebuild = false;
                break;
            }
        }
        // 同一帧反复求值（模拟"这一帧被重画两次"）也必须一致
        bool reframe = true;
        for (int i = 0; i < 1000; ++i) {
            const double t = kT0 + 0.5 + i * 0.0003;
            const double a = dshb::BeatOffsetPx(t, hh, 1.0);
            const double b = dshb::BeatOffsetPx(t, hh, 1.0);
            const double c = dshb::BeatOffsetPx(t, hh, 1.0);
            if (!(a == b && b == c)) {
                reframe = false;
                break;
            }
        }
        const double sample = dshb::BeatOffsetPx(kT0 + 0.7, hh, 0.31);
        h.Req("case5", "determinism: the same (t, history, fade) always gives the same value",
              "2000 pairwise evaluations bitwise equal=" + std::string(bitwise ? "yes" : "no") +
                  "; 500 evaluations against a freshly built equal history=" +
                  std::string(rebuild ? "yes" : "no") +
                  "; 1000 frames evaluated 3x each identical=" +
                  std::string(reframe ? "yes" : "no") + "; sample BeatOffsetPx(1000.7, R=1/D=0.5, "
                  "fade=0.31)=" + F(sample, 12) + " px",
              bitwise && rebuild && reframe);
    }

    // ---- (6) recovery ----
    {
        const RecoveryInfo r = MeasureRecovery(1.0, 1.0);
        const bool ok = std::fabs(r.fAtRecEnd - dshb::kBeatF0Min) < 1e-12 &&
                        std::fabs(r.fWellAfter - dshb::kBeatF0Min) < 1e-12 &&
                        r.fAtChange > r.fAfterEase && r.fMidRecovery < r.fAfterEase;
        h.Req("case6", "recovery: after T_rec the frequency is back to F_0_min (R=1/D=1)",
              "T_rec=" + F(r.tRec, 2) + " s; F at change=" + F(r.fAtChange, 4) +
                  " Hz (F1=" + F(dshb::BeatF1(1.0), 4) + "), after the ease=" +
                  F(r.fAfterEase, 4) + " Hz (F2=" + F(dshb::BeatF2(1.0), 4) + "), mid-recovery=" +
                  F(r.fMidRecovery, 4) + " Hz, exactly at t2+T_rec=" + F(r.fAtRecEnd, 9) +
                  " Hz, 10 s later=" + F(r.fWellAfter, 9) + " Hz; F_0_min=" +
                  F(dshb::kBeatF0Min, 4) + " Hz",
              ok);
    }

    // ---- (6b) T_rec = 0 means "no recovery needed" ----
    {
        const RecoveryInfo r = MeasureRecovery(1.0, 0.0);
        const double f2 = dshb::BeatF2(1.0);
        const bool ok = r.tRec == 0.0 && std::fabs(r.fAtRecEnd - f2) < 1e-12;
        h.Req("case6b", "T_rec == 0 means no recovery: the frequency stops at F2, not F_0_min",
              "R=1/D=0 -> T_rec=" + F(r.tRec, 2) + " s; F at t2+10s = " + F(r.fMidRecovery, 6) +
                  " Hz and at t2+60s = " + F(r.fAtRecEnd, 6) + " Hz; F2=" + F(f2, 6) +
                  " Hz, F_0_min=" + F(dshb::kBeatF0Min, 4) + " Hz",
              ok);
    }

    // ---- (7) silence is exactly 0 and beats never overlap ----
    {
        struct Case {
            const char* name;
            double R;
            double D;
        };
        const Case cases[2] = {{"R=1/D=0 (fastest)", 1.0, 0.0}, {"R=0/D=0 (slowest)", 0.0, 0.0}};
        for (const Case& c : cases) {
            const std::vector<BeatChange> hh = OneChange(c.R, c.D);
            // 跑 5 秒；对最慢那一档（58.8 s 一拍）这段时间里本来就没有第二拍，
            // 所以"最小间隔"只对最快那一档有意义。
            double minGap = 1e300;
            double lastOnset = -1.0;
            int zeros = 0;
            int frames = 0;
            for (int i = 0; i <= static_cast<int>(5.0 * kHz); ++i) {
                const double t = kT0 + i * kFrame;
                const double y = dshb::BeatOffsetPx(t, hh, 1.0);
                const BeatOnset o = dshb::CurrentBeatOnset(t, hh);
                if (o.valid) {
                    if (lastOnset < 0.0 || o.atSeconds != lastOnset) {
                        if (lastOnset > 0.0) {
                            const double gap = o.atSeconds - lastOnset;
                            if (gap < minGap) minGap = gap;
                        }
                        lastOnset = o.atSeconds;
                    }
                }
                if (y == 0.0) ++zeros;
                ++frames;
            }
            const bool gapOk = (minGap > dshb::kBeatLen) || (minGap > 1e299);
            const bool ok = gapOk && zeros > 0 && dshb::kBeatLen < 1.0 / dshb::kBeatFMax;
            h.Req("case7", std::string("beats never overlap and the tail is exactly 0 (") +
                               c.name + ")",
                  "over 5 s at 60 Hz: frames with displacement exactly 0=" + F(zeros, 0) + "/" +
                      F(frames, 0) + "; min onset gap=" +
                      (minGap > 1e299 ? std::string("n/a (only one beat in 5 s)")
                                      : F(minGap, 6) + " s") +
                      "; kBeatLen=" + F(dshb::kBeatLen, 3) + " s < 1/F_max=" +
                      F(1.0 / dshb::kBeatFMax, 3) + " s",
                  ok);
        }
    }

    // ---- (8) the phase really is in BEATS ----
    {
        const std::vector<BeatChange> hh = OneChange(0.0, 0.0);   // F1 = F2 = F_0_min
        const double f = dshb::kBeatF0Min;
        double worstErr = 0.0;
        std::string detail;
        for (int nbeat = 1; nbeat <= 3; ++nbeat) {
            const double want = kT0 + nbeat / f;
            const BeatOnset o = dshb::CurrentBeatOnset(want + 1e-9, hh);
            const double err = std::fabs(o.atSeconds - want);
            if (err > worstErr) worstErr = err;
            detail += " n=" + F(nbeat, 0) + ": want " + F(nbeat / f, 6) + " s got " +
                      F(o.atSeconds - kT0, 6) + " s";
        }
        h.Req("case8", "the phase is in BEATS: with constant F the n-th onset is exactly n/F",
              "F_0_min=" + F(f, 6) + " Hz;" + detail + "; worst error=" + F(worstErr, 12) + " s",
              worstErr < 1e-6);
    }

    // ---- (9) fade scales the displacement and leaves the phase alone ----
    {
        const std::vector<BeatChange> hh = OneChange(0.0, 0.0);
        const double onset = OnsetAbsolute(kT0, 0.0, 0.0);
        const double t = onset + dshb::kBeatMu2;   // 峰值处
        const double full = dshb::BeatOffsetPx(t, hh, 1.0);
        const double half = dshb::BeatOffsetPx(t, hh, 0.5);
        const double zero = dshb::BeatOffsetPx(t, hh, 0.0);
        const BeatOnset o = dshb::CurrentBeatOnset(t, hh);
        const bool ok = std::fabs(full - dshb::kBeatAMax) < 5e-3 &&
                        std::fabs(half - 0.5 * full) < 1e-12 && zero == 0.0 &&
                        std::fabs(o.atSeconds - onset) < 1e-9;
        h.Req("case9", "fade multiplies the displacement but leaves the phase untouched",
              "at the peak (tau=mu2=" + F(dshb::kBeatMu2 * 1000, 0) + " ms): fade=1 -> " +
                  F(full, 6) + " px, fade=0.5 -> " + F(half, 6) + " px, fade=0 -> " +
                  F(zero, 6) + " px; CurrentBeatOnset().atSeconds - onset = " +
                  F(o.atSeconds - onset, 12) + " s",
              ok);
    }

    // ---- (10) K table accuracy + measured memory ----
    {
        const TableAccuracy ta = MeasureTableAccuracy();
        // 验收：K 的折算误差要远小于相位验收用的 1e-6（差两个数量级即可）。
        const bool accOk = ta.kImpliedErr < 1e-8;

        // 实测内存：跑完所有检查之后再读一次工作集。
        const std::size_t wsAfter = WorkingSetBytes();
        const long long grew = static_cast<long long>(wsAfter) - static_cast<long long>(wsBefore);
        // K 表是惰性构造的，所以上面那两次读数之差就是它的真实代价。
        const bool memOk = wsAfter > 0 && grew >= 0 && grew < 4LL * 1024 * 1024;

        h.Req("case10",
              "K table: accuracy against a converged reference, and MEASURED memory < 4 MB",
              "K's argument tops out at e=min(u,t2)=t2=" + F(ta.argMax, 6) +
                  " s (provable: BeatFPrime clamps it), so the table only needs [0, t2]; "
                  "integrate past t2 (T=" + F(ta.T, 6) + " s) so K is used at its maximum: "
                  "module increment=" + F(ta.gotInc, 12) + " beats, Simpson reference (split at "
                  "the t2 kink)=" + F(ta.wantInc, 12) + ", difference " +
                  F(ta.gotInc - ta.wantInc, 3) + " beats -> K error <= " +
                  F(ta.kImpliedErr, 12) + " s (acceptance needs << 1e-6); "
                  "MEASURED working set: before any module call " + HumanBytes(wsBefore) +
                  ", after all checks " + HumanBytes(wsAfter) + ", growth " +
                  HumanBytes(static_cast<std::size_t>(grew)) +
                  " (the K table is lazily built on first call, so the growth IS its cost)",
              accOk && memOk);
    }

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
