// ===========================================================================
//  heartbeat.h —— 窗口"心跳"的位移波形（纯数学，可离线复算）
// ===========================================================================
//
//  ★ 本模块是**纯函数**：不读时钟、不碰文件、不含 `Windows.h`、不碰 D2D、
//    **没有全局可变状态**（连积分表都没有了，见下）。
//    输入是"现在几点 + 计时器状态 + 这一拍的 (R, D)"，输出是一个像素位移。
//    正因为没有隐藏状态，`tools/beatprobe.cpp` 能用合成状态把每条性质证明一遍，
//    也能"导出单帧、离线重算"（这是本项目唯一的验证方式）。
//
// ---------------------------------------------------------------------------
// 模型（所有者 2026-09-18 定）：**一次跳动 = 一个包络；节拍 = 一个计时器**
// ---------------------------------------------------------------------------
//  位移：
//      Δ = A(R, D) · Shape(τ)          τ = 现在 - 这一拍的触发时刻
//  包络（两个高斯之和，形状与 R/D 无关）：
//      Shape(τ) = [ G(τ; μ1, σ1) + b2 · G(τ; μ2, σ2) ] / (1 + b2)
//      G(τ; μ, σ) = exp( -½ ((τ - μ)/σ)² )
//  幅度与周期（都在**触发那一刻**采样，整拍不变）：
//      A(R, D) = A_base + (A_min - A_base)·D + (A_max - A_base)·R
//      T(R, D) = T_base + (T_max - T_base)·D + (T_min - T_base)·R
//  计时（显示层每帧做，本模块只提供 BucketFor 与 Shape）：
//      每帧 t = now - tTrigger；若 t >= TTrigger，则触发一次跳动、tTrigger = now、
//      重新采样这一拍的 T 与 A。
//
//  ★ 为什么"周期"而不是"频率"（所有者点名的理由）：真正被计的是**秒**。
//    上一版用频率 F 与相位累积 φ = ∫F ds，那是为了"F 变化时相位不跳"付的代价：
//    一张 0.19 MB 的 K 积分表、缓动 (1-rate^(60u))^c、恢复段 T_rec = 60·D·R、
//    以及"拍号 = floor(φ)"的二分反解 —— 全部为那个目标服务。
//    改成直接计时之后，那些东西一个都不需要了。
//
//  ★ 周期与幅度**在触发时采样**（不是每帧重算）：R 现在每帧都在衰减
//    （R(t) = kAmbienceDecayA^t），若每帧重算 T，t 与 T 会同时变、参照系自己会动。
//
//  ★ 两拍不会叠：最短周期 T_min = 0.5 s > 一拍长度 kBeatLen = 0.345 s，
//    所以任何时刻最多只有一拍在跳，位移就是**单拍**的位移（不是求和）。
//    这条是窗口区域要留 kBeatAMax 像素行程的前提（renderer.cpp 的 SetWindowRgn）。
//
//  ★ 位移加在**画布内部内容的偏移**上，**不是**加在窗口位置上：
//      reference（窗口真实位置 P）  ← 只有拖动能改它；心跳**从不**改写它
//      Δ                            ← 作为绘制偏移，平移整个画布内容（renderer.cpp 的 SetTransform）
//    于是窗口真实位置一帧都不变 ->"程序移动窗口触发系统贴边吸附"在结构上不可能发生。
//    ⚠ 两个容易漏的后果：
//      1. **命中测试要跟着 Δ 平移**（币种符号矩形、拖动判定），否则点击差最多 5 px。
//      2. 持久化只存 reference（现在这是字面成立的：P 根本不含 Δ）。
#pragma once

#include <cstddef>

namespace dshb {

// ---------------------------------------------------------------------------
// 波形常量（形状）
// ---------------------------------------------------------------------------
//  ★ 两个脉冲都按**中心**给参数，而且**两个中心相等**。中心 = 触发之后多久达到峰值。
//    这一条是整个模块最容易写错的地方：把第一个中心放在 τ = 0 时，触发那一刻位移就已经
//    是 3.61 px（A_max 的 72%），首帧落点最坏到 73.5% —— 上升段根本没被 60 Hz 采到。
//    两个中心都放到 95 ms 之后，同一指标降到 33.94%（探针 case2 扫 1200 个起始相位实测）。
inline constexpr double kBeatSigma1 = 0.055;   // s   第一个高斯脉冲的 sigma
inline constexpr double kBeatSigma2 = 0.050;   // s   第二个高斯脉冲的 sigma
inline constexpr double kBeatMu1 = 0.095;      // s   第一个高斯脉冲的**中心**
inline constexpr double kBeatMu2 = 0.095;      // s   第二个高斯脉冲的**中心**
inline constexpr double kBeatB2 = 0.50;        //     第二个脉冲的幅度比

// 两个中心重合 -> 连续域峰值恰好是 1 + b2。归一化后"幅度 = 几 px"这句话才成立。
inline constexpr double kBeatShapePeak = 1.0 + kBeatB2;   // = 1.5

// 一次跳动的时长：末端取第二个中心之后 5 个 sigma（那里残留约 2.3e-5，按 0 处理）。
// ★ 必须 < T_min = 0.5 s，否则最短周期下两拍会叠在一起。
inline constexpr double kBeatLen = kBeatMu2 + 5.0 * kBeatSigma2;   // = 0.345 s

// ---------------------------------------------------------------------------
// 幅度（px）：A = A_base + (A_min - A_base)·D + (A_max - A_base)·R
// ---------------------------------------------------------------------------
inline constexpr double kBeatAMax = 5.0;       // 最剧烈（R = 1）时的幅度
inline constexpr double kBeatAMin = 1.0;       // 最枯竭（D = 1、R = 0）时的幅度
inline constexpr double kBeatABase = 3.0;      // 常态（R = 0、D = 0）时的幅度
static_assert(kBeatAMin <= kBeatABase && kBeatABase <= kBeatAMax,
              "sanity: A_min <= A_base <= A_max, otherwise the two terms fight");

// ---------------------------------------------------------------------------
// 周期（s）：T = T_base + (T_max - T_base)·D + (T_min - T_base)·R
// ---------------------------------------------------------------------------
inline constexpr double kBeatTMax = 30.0;      // 最慢：D = 1、R = 0（余额枯竭，几乎不动）
inline constexpr double kBeatTMin = 0.5;       // 最快：R = 1（刚发生剧烈消耗）
inline constexpr double kBeatTBase = 15.0;     // 常态：R = 0、D = 0
static_assert(kBeatTMin > kBeatLen,
              "sanity: the shortest period must exceed one beat, or two beats overlap");

// 60 帧/秒：`--beat-frame=k` 的换算（k 帧 = k/60 秒），与 kCurveFrameHz 同源。
inline constexpr double kBeatFrameHz = 60.0;

// ---------------------------------------------------------------------------
// 幅度律与周期律（纯函数，R、D 会先夹到 [0,1]）
// ---------------------------------------------------------------------------
double BeatAmplitudePx(double R, double D);
double BeatPeriodSeconds(double R, double D);

// ---------------------------------------------------------------------------
// 包络
// ---------------------------------------------------------------------------
//  τ 秒处的**归一化**包络：连续域峰值恰好是 1（= A px 对应 A）。
//  τ < 0 或 τ > kBeatLen 时为**恰好 0**（不是"很小的数"）—— 静默段必须严格是 0，
//  否则"两次跳动之间回到 0"这条验收项就没法量。
double BeatShapeNorm(double tauSeconds);

// ---------------------------------------------------------------------------
// 一拍的运行状态（调用方持有；本模块一个静态量都没有）
// ---------------------------------------------------------------------------
//  显示层每帧照着做（顺序就是这里写的顺序）：
//      1. 推进仿真时钟（冻结时不推进）
//      2. elapsed = simSeconds - triggerSeconds
//      3. if (elapsed >= periodSeconds) { triggerSeconds = simSeconds;
//                                          periodSeconds = BeatPeriodSeconds(R, D);
//                                          amplitudePx   = BeatAmplitudePx(R, D); }
//      4. offsetDip = BeatOffsetFromBucket(simSeconds, bucket)
//  初始化：triggerSeconds = 0（所以第一帧就跳一次）、periodSeconds / amplitudePx 用当帧的 R/D 采样。
struct BeatBucket {
    double triggerSeconds = 0.0;   // 上次跳动触发的时刻（与 nowSeconds 同一计时基准）
    double periodSeconds = 0.0;    // 这一拍的周期（触发时采样，整拍不变）
    double amplitudePx = 0.0;      // 这一拍的幅度（触发时采样，整拍不变）
};

// 这一拍在 τ = nowSeconds - triggerSeconds 处的位移（DIP，> 0 = 往下）。
// 纯函数：同一个 (nowSeconds, bucket) 永远给同一个值，不含任何"记住上一帧"的状态。
// 静默期（τ 落在 [0, kBeatLen] 之外）返回**恰好 0**。
double BeatOffsetFromBucket(double nowSeconds, const BeatBucket& bucket);

// 这一拍从哪一刻开始、已经走了多久（探针/自检用；渲染循环不需要）。
struct BeatOnset {
    bool valid = false;       // false = 还没有触发过任何一拍
    double atSeconds = 0.0;   // 触发时刻
    double tauSeconds = 0.0;  // nowSeconds - atSeconds
};

BeatOnset CurrentBeatOnset(double nowSeconds, const BeatBucket& bucket);

}  // namespace dshb
