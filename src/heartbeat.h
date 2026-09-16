// ===========================================================================
//  heartbeat.h —— 窗口"心跳"的位移波形（纯数学，可离线复算）
// ===========================================================================
//
//  规格：`不入库文件/E 心跳波形.md`（权威）。本文件是那份规格的实现，
//  数字全部取自它，没有在这里新引入任何常量。
//
//  ★ 本模块是**纯函数**：不读时钟、不碰文件、不含 `Windows.h`、不碰 D2D、
//    **没有全局可变状态**（唯一的文件级状态是一张只读的积分表，见 .cpp 的说明）。
//    输入是"仿真时间 + 那段时间里的余额变化历史 + R/D"，输出是一个像素位移。
//    正因为没有隐藏状态，`tools/beatprobe.cpp` 能用合成历史把每条性质证明一遍，
//    也能"导出单帧、离线重算"（这是本项目唯一的验证方式）。
//
// ---------------------------------------------------------------------------
// 一次心跳长什么样（一句话）
// ---------------------------------------------------------------------------
//  两个高斯脉冲之和：σ1 = 55 ms、σ2 = 50 ms、**两个中心都在 onset 之后 95 ms**、
//  第二个脉冲的幅度比 b2 = 0.50。位移从 onset 处的 ≈1.02 px **平滑升到** 5 px，
//  再单调降回 0 —— **一个包络、一个峰**，不是"两个峰"。
//
//  ★ 两个脉冲都用**中心**参数化（不是"起点"）。这一条是整个模块最容易写错的地方：
//    把第一个中心放在 t = 0 时，onset 处位移就已经是 3.61 px（幅度的 72%），
//    首帧落点最坏到 3.68 px —— 也就是**一层之首就吃掉 73.5% 的幅度**，
//    上升段根本没被 60 Hz 采到。两个中心都放到 95 ms 之后，同一指标降到 **33.94%**
//    （探针 case2 扫 1200 个起始相位实测；规格 §8 给的是同一件事的解析值）。
//
//  ★ 不要给第二个脉冲加 `(t >= mu2)` 这样的时间门闸：中心重合之后，
//    门闸会把 0～95 ms 那整段上升**直接删掉**，首帧跳变立刻退回 100% 那一档。
//
//  ★ `fma` 陷阱：`(t - mu)` 必须用 `std::fma(t, 1.0, -mu)` 算，而且**不能**
//    加 `if (t < mu) return 0`。原因：`fma(0.0, 1.0, -0.095)` 返回 −1.4e-17
//    而不是 0，于是 t=0 会被判成"小于 mu"、那个脉冲返回 0，
//    而正确值是 exp(-1.805) = 0.165 —— 只差 1.4e-17 的符号，整条波形就错了。
//
// ---------------------------------------------------------------------------
// ★ 渲染循环怎么调本模块（接线时照这里做）
// ---------------------------------------------------------------------------
//  保留什么状态（都在窗口/显示层手里，本模块一个都不留）：
//
//      double  simSeconds;                  // 单调高精度计时（QueryPerformanceCounter 差分）
//      std::vector<BeatChange> history;     // 每次"余额变化"追加一条，只留最近 8 条
//      double  fade01;                      // 拖动/销毁/归零时的淡出系数，调用方自己平滑
//
//  初始化一次：
//
//      history.push_back(MakeBeatChange(simSeconds, R, D));   // 启动时先记一条
//
//  每次余额变化（采样到新值、算出新的 R 与 D）：
//
//      AppendBeatChange(&history, MakeBeatChange(simSeconds, R, D),
//                       kBeatHistoryKeep);
//
//  每帧（画面更新前）：
//
//      const double dx = BeatOffsetPx(nowSeconds(), history, fade01);
//      // dx 单位 DIP；> 0 = 往下
//
//  ★★ 这个 dx 加在**画布内部内容的偏移**上，**不是**加在窗口位置上：
//
//      reference（窗口真实位置 P）  ← 只有拖动能改它；心跳**从不**改写它
//      dx（心跳的唯一产物）        ← 作为绘制偏移，平移整个画布内容
//
//      于是窗口真实位置一帧都不变 -> "程序移动窗口触发系统贴边吸附"
//      在结构上不可能发生（设计 §9.4 的 E9 不需要做，不需要任何守卫）。
//
//      画布 475×289 DIP、实体区只占中间 315×129 DIP，四周各有 80 DIP 余量，
//      5 px 的偏移远小于 80 DIP，内容永远不会被裁切 —— 屏幕上看到的就是
//      "整个面板动了 5 px"，与移动窗口视觉等价。
//
//  ⚠ 两个接线时容易漏的后果：
//    1. **命中测试要跟着 dx 平移**（币种符号矩形、拖动判定），否则点击差 5 px。
//    2. 持久化只存 reference。现在这是**字面成立**的（P 根本不含 dx），
//       不再是"选一个不抖的时刻存"。
//
//  ★ 接受多个历史条目，但**正常只会用到最后一条**：R=1 时最快 2 Hz（0.5 s 一拍），
//    而采样固定 10 s 一次（设计 §4.1），所以两次变化之间必然跨过好几个整数相位，
//    老条目对当前这一拍没有影响。留 8 条是为了"迟到的历史"和将来的可复算，
//    相位积分是逐段求和，老段只贡献一个常数。
//
// ---------------------------------------------------------------------------
// 相位为什么以「拍」为单位、为什么不能用 sin(2πFt)
// ---------------------------------------------------------------------------
//  写成 `sin(2*pi*F*t)` 时，F 一变，那个时刻的相位 `2*pi*F*t` 就跟着变，
//  窗口当场跳一下。跳变幅度与"t 是绝对时间"成正比：F 从 F2 跳到 F1、
//  变化发生在 t = 2 s 时，实测两帧之间跳 0.90～2.92 px（R=0.5 与 R=1 两档，
//  见探针 case3）；变化发生得越晚，跳得越大。
//
//  本模块的做法：相位是一个**累积量**，单位是"拍"（不是弧度）：
//
//      φ(t) = ∫₀ᵗ F(s) ds
//
//  "距离上一次搏动够久了吗"于是等价于"φ 跨过下一个整数了吗"。
//  F 变化时 φ **连续**（它是积分），只影响**下一次 onset 的时刻**；
//  已经起跳的那一拍永远走完（形状由它的 onset 决定，与 F 无关）。
//
//  ★ 实现上 φ(t) 是**解析分段积分 + 逐段求和**，不是逐帧累加：
//    每个变化段有自己的 F 曲线，段内积分有闭式解（见 .cpp 的 BeatFPrime）。
//    这样单帧可离线复算、掉帧不漂移、换帧率不改变波形。
#pragma once

#include <cstddef>
#include <vector>

namespace dshb {

// ---------------------------------------------------------------------------
// 波形常量（数字全部来自 `不入库文件/E 心跳波形.md`，改这里就是改规格）
// ---------------------------------------------------------------------------
//  ★ 两个脉冲都按**中心**给参数。中心 = onset 之后多久达到峰值。
inline constexpr double kBeatSigma1 = 0.055;   // s   第一个高斯脉冲的 sigma
inline constexpr double kBeatSigma2 = 0.050;   // s   第二个高斯脉冲的 sigma
inline constexpr double kBeatMu1 = 0.095;      // s   第一个高斯脉冲的**中心**
inline constexpr double kBeatMu2 = 0.095;      // s   第二个高斯脉冲的**中心**
inline constexpr double kBeatB2 = 0.50;        //     第二个脉冲的幅度比（所有者：一半）

// 两个中心重合 -> 连续域峰值恰好是 1 + b2。归一化后"幅度 = 几 px"这句话才成立。
inline constexpr double kBeatShapePeak = 1.0 + kBeatB2;   // = 1.5
inline constexpr double kBeatShapeNorm = 1.0 / kBeatShapePeak;

// 一次搏动的时长：末端取第二个中心之后 5 个 sigma（那里残留约 2.3e-5，按 0 处理）。
// ★ 必须 < 1/kBeatFMax = 0.5 s，否则 2 Hz 时两次搏动会叠在一起
//   （叠加的表现是"位移回不到 0"，也就是地板被抬高，不是抖动）。
inline constexpr double kBeatLen = kBeatMu2 + 5.0 * kBeatSigma2;   // = 0.345 s

// 幅度（px）：A = A_max + (A_min - A_max) * D
inline constexpr double kBeatAMax = 5.0;       // px  最大幅度（D = 0）
inline constexpr double kBeatAMin = 1.0;       // px  最小幅度（D = 1）

// 频率（Hz）：R 决定"多快"，D 不参与频率
inline constexpr double kBeatF0Min = 0.017;    // Hz  常态最小频率（60 s 一次）
inline constexpr double kBeatF0Max = 0.050;    // Hz  常态最大频率（20 s 一次）
inline constexpr double kBeatFMax = 2.0;       // Hz  变化一开始的最大频率

// 恢复：T_rec = kBeatRecMax * D * R；T_rec == 0 视为"不需要恢复"
inline constexpr double kBeatRecMax = 60.0;    // s

// 频率缓动 (1 - rate^(60u))^c
// ★ rate 是 0.995，**不是**项目里数字滚动用的 0.975：0.975 下缓动几秒就走完，
//   "刚变化时跳得快"根本看不见（实测 5 秒时的累积相位：0.975 → 4.003 拍，
//   0.995 → 9.866 拍，见规格 §9.4）。
inline constexpr double kBeatEaseRate = 0.995;  // 每帧系数
inline constexpr double kBeatEaseC = 10.0;      // 指数 c
inline constexpr double kBeatFrameHz = 60.0;    // k = 秒数 × 60（与 kCurveFrameHz 同源）

// "F_t 已经落到 F2" 的截断值（规格 §4.1 的 t2）。它只决定**恢复段从何时开始**，
// 不影响 F_t 的连续性 —— 因为缓动那一段写成 F1 + (F2-F1)*ease，
// ease=0 时恰好是 F1、ease=1 时恰好是 F2，两个端点都是精确的。
inline constexpr double kBeatF2Eps = 0.001;

// 建议的历史长度：够用即可，老条目只贡献一个常数项
inline constexpr std::size_t kBeatHistoryKeep = 8;

// ---------------------------------------------------------------------------
// 一次"余额变化"
// ---------------------------------------------------------------------------
//  R = 剧烈程度 ∈ [0,1]、D = 死态程度 ∈ [0,1]，两者都由调用方（显示层）算好；
//  本模块不认识余额、不认识速率，也不认识"读不到"——那是状态机的事。
//
//  ★ F1/F2 是从 R 现算的，不存：存了就会出现"R 与 F1 不一致"这种没法查的错。
struct BeatChange {
    double at = 0.0;   // 事件时刻（秒，与 BeatOffsetPx 的 nowSeconds 同一计时基准）
    double R = 0.0;    // 剧烈程度 ∈ [0,1]
    double D = 0.0;    // 死态程度 ∈ [0,1]
};

inline constexpr double BeatF1(double R) { return kBeatF0Min + (kBeatFMax - kBeatF0Min) * R; }
inline constexpr double BeatF2(double R) { return kBeatF0Min + (kBeatF0Max - kBeatF0Min) * R; }
inline constexpr double BeatRecoverySeconds(double R, double D) { return kBeatRecMax * D * R; }

// 由 R/D 造一条历史条目；R、D 会被夹到 [0,1]。
BeatChange MakeBeatChange(double atSeconds, double R, double D);

// 追加一条（并保持按时间有序、只留最多 keep 条）。
// ★ 正常只会在**时间更晚**的时刻追加；若给了更早的时刻，这里按时间插进去并裁掉最老的，
//   这样"迟到的历史"也不会让 φ 变成非单调（那会让整拍号错乱）。
void AppendBeatChange(std::vector<BeatChange>* history, const BeatChange& change,
                      std::size_t keep = kBeatHistoryKeep);

// ---------------------------------------------------------------------------
// 位移
// ---------------------------------------------------------------------------
//  τ 秒处的**归一化**包络：连续域峰值恰好是 1（= 5 px 对应 A = 5）。
//  τ < 0 或 τ > kBeatLen 时为**恰好 0**（不是"很小的数"）。
double BeatShapeNorm(double tauSeconds);

//  某一刻的实时频率 F_t（Hz）。分段：
//    第一段（缓动，从变化起）: F1 + (F2 - F1) * (1 - rate^(60u))^c
//    第二段（恢复，u > t2 且 T_rec > 0）: F2 + (F0_min - F2) * min((u-t2)/T_rec, 1)
//  其中 t2 = (1-rate^(60u))^c 第一次落进 kBeatF2Eps 以内的时刻（= 2.312614 s，见 BeatEaseT2Seconds()）。
//  没有历史条目时返回 kBeatF0Min（常态最慢那一档）。
double BeatFrequencyHz(double nowSeconds, const std::vector<BeatChange>& history);

//  上面那个 t2（秒）。暴露出来是为了让探针能在**精确的** t2 与 t2+T_rec 处取频率；
//  用截断值近似会让"恢复段末尾恰好等于 F_0_min"这条检查带上 1e-4 s 的假误差。
double BeatEaseT2Seconds();

//  相位 φ(t)，单位是**拍**。纯函数：只由 (nowSeconds, history) 决定。
//  ★ 这是本模块的核心量：搏动的号数 = floor(φ)，onset 时刻 = φ 取到那个整数的时刻。
double BeatPhaseBeats(double nowSeconds, const std::vector<BeatChange>& history);

//  ★ 唯一的对外位移接口。必须是纯函数：同一个 (nowSeconds, history, fade01)
//    永远给同一个值，不含任何逐帧累加的状态。
//
//    返回**像素**（与 kBeatAMax/kBeatAMin 同单位，DIP）。> 0 = 往屏幕下方。
//    淡出系数 fade01 ∈ [0,1] **乘在最后**、不进相位：淡出不能让相位停下，
//    否则松手之后会从一个相位错乱的地方继续。
//
//    ★ 调用方拿到的这个值要加在**画布内部内容的偏移**上（见文件顶部的接线说明）。
double BeatOffsetPx(double nowSeconds, const std::vector<BeatChange>& history,
                    double fade01 = 1.0);

// ---------------------------------------------------------------------------
// 给探针/自检用的诊断（正常渲染循环不需要）
// ---------------------------------------------------------------------------
struct BeatOnset {
    bool valid = false;       // false = 这段历史里还没有过一次 onset
    long long index = 0;      // 第几拍（φ 跨过的整数），从 1 开始
    double atSeconds = 0.0;   // 这一拍的 onset 时刻
    double tauSeconds = 0.0;  // nowSeconds - atSeconds
};

//  当前这一拍是从哪一刻开始的。内部用"解析求 φ 的逆"而不是逐帧找跨界，
//  所以它和 BeatOffsetPx 用同一个 φ，两者不可能对不上。
BeatOnset CurrentBeatOnset(double nowSeconds, const std::vector<BeatChange>& history);

}  // namespace dshb
