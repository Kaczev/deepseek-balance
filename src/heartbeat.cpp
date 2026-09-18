// ===========================================================================
//  heartbeat.cpp —— 心跳位移波形的实现（纯数学）
// ===========================================================================
//
//  模型见 heartbeat.h 顶部。本文件只有四件小事：一个高斯脉冲、包络、两条律、位移。
//  上一版那一整套相位机器（K 积分表 / 缓动 / 恢复段 / 拍号二分）已经删掉 —— 改成
//  直接计时之后它们一个都不需要（所有者 2026-09-18 的模型）。
//
//  本文件里最要紧的一件事：
//    **脉冲用中心参数化**，且 (τ-μ) 用 fma 且**不加** `(τ < μ)` 门闸。
//    写错 = 单帧跳变从 33.94% 退到 73.5%（触发那一刻就吃掉半个幅度）。
#include "heartbeat.h"

#include <algorithm>
#include <cmath>

namespace dshb {
namespace {

// 一个高斯脉冲，τ 是"距触发经过的秒数"。
//  只能用 std::fma 算 (τ - μ)：写 τ - μ 时，τ≈1e-5、μ=0.095 会把有效数字丢掉 13 位，
//  (τ-μ)² 就变成纯舍入噪声。fma 把减法并进乘加、只舍入一次，这一处要的就是它。
//
//  ★ 更要紧的是**不许加 `(τ < μ)` 这道门闸**，理由与 fma 无关：在 τ = 0 处
//    `0.0 < 0.095` 为**真**，门闸会在触发那一刻把函数打成 0，于是 0 → 95 ms 的整段上升
//    被删掉（正确值是 exp(-1.805) = 0.165）。实测代价：首帧跳变从 33.94% 涨到 73.5%。
//    （曾经这里写着"fma(0.0,1.0,-0.095) 返回 −1.4e-17 所以门闸会误判"——那句话是错的：
//      按 IEEE-754，那个表达式精确返回 −0.095，与 0.0 - μ 逐位相同。错的是门闸，不是比较。）
//  所以这里**不分段**，只有一个 exp；z 很大时 exp 自己下溢到 0。
double Pulse(double tau, double mu, double sigma) {
    const double u = std::fma(tau, 1.0, -mu);
    const double z = u / sigma;
    const double e = -0.5 * z * z;
    return e > -700.0 ? std::exp(e) : 0.0;
}

double Clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

}  // namespace

// ---------------------------------------------------------------------------
// 响应曲线与两条律（触发那一刻采样）
// ---------------------------------------------------------------------------
double BeatResponseCurve(double R) {
    const double r = Clamp01(R);
    return 1.0 - std::pow(1.0 - r, kBeatRPow);
}

double BeatAmplitudePx(double R, double D) {
    const double e = BeatResponseCurve(R);
    const double d = Clamp01(D);
    // ★ A_max 配 R、A_min 配 D（与周期相反）：剧烈 = 跳得又快又猛，枯竭 = 又慢又弱。
    return kBeatABase + (kBeatAMax - kBeatABase) * e * (1.0 - d) + (kBeatAMin - kBeatABase) * d;
}

double BeatPeriodSeconds(double R, double D) {
    const double e = BeatResponseCurve(R);
    const double d = Clamp01(D);
    return kBeatTBase + (kBeatTMin - kBeatTBase) * e * (1.0 - d) + (kBeatTMax - kBeatTBase) * d;
}

// ---------------------------------------------------------------------------
// 包络
// ---------------------------------------------------------------------------
double BeatShapeNorm(double tauSeconds) {
    // ★ 边界严格：τ < 0 或 τ > kBeatLen 时**恰好 0**（不是"很小的数"）。
    if (tauSeconds < 0.0 || tauSeconds > kBeatLen) return 0.0;
    const double p = Pulse(tauSeconds, kBeatMu1, kBeatSigma1) +
                     kBeatB2 * Pulse(tauSeconds, kBeatMu2, kBeatSigma2);
    return p / kBeatShapePeak;
}

// ---------------------------------------------------------------------------
// 位移
// ---------------------------------------------------------------------------
BeatOnset CurrentBeatOnset(double nowSeconds, const BeatBucket& bucket) {
    BeatOnset out;
    out.valid = bucket.amplitudePx > 0.0;      // 幅度为 0 的拍等于没跳（理论到不了，防呆）
    out.atSeconds = bucket.triggerSeconds;
    out.tauSeconds = nowSeconds - bucket.triggerSeconds;
    return out;
}

double BeatOffsetFromBucket(double nowSeconds, const BeatBucket& bucket) {
    const double tau = nowSeconds - bucket.triggerSeconds;
    // 静默段恰好 0。T_min = 0.5 s > kBeatLen = 0.345 s 保证这条是常规路径而不是兜底：
    // 下一次触发一定在上一拍走完之后（所有者给的参数，static_assert 守着）。
    if (tau < 0.0 || tau > kBeatLen) return 0.0;
    return bucket.amplitudePx * BeatShapeNorm(tau);
}

}  // namespace dshb
