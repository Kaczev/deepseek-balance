// deepseek-balance v0.2 —— 关闭时的破碎粒子（设计 §11.6）
//
// 这一层**故意什么都不知道**：不知道窗口、不知道 Direct2D、不知道时钟、不知道随机数。
// 它只是"一群点各自的运动学 + 一条拖尾的采样"，输入是 (R, D, 画布)，输出是位置。
// 于是同一条代码有两个驱动者：
//   · 生产：Renderer 每帧推进一次（StartShutdownParticles -> AdvanceShutdownParticles）
//   · 验收：探针在没有鼠标、没有消息循环的情况下，直接把它推到任意时刻再导帧量像素
// 探针量的就是屏幕上画的那一份 —— 这正是本项目唯一的验证方式。
//
// ★ 谁在什么时候调它（与"关闭流程"那一步的接缝，只有这一个入口）：
//   widget_display 的**第三次**关闭点击确认、窗口即将销毁的那一刻调
//   Renderer::StartShutdownParticles()（无参数）。那一步只发"该放粒子了"这一个信号，
//   不传颜色、不传时间、不管数量：颜色由 Renderer 从**本帧的 WidgetFrame** 里取
//   （见 renderer.cpp 的注释），时间由 Renderer 的帧循环累计，数量与物理全在这个模块里。
//   粒子期间再次调用是**空操作**（返回 false），所以"再次触发销毁不产生第二个实例"
//   （设计 G5）是这一层的性质，不依赖调用方记得先判断。
//
// ---------------------------------------------------------------------------
// ★★ 2026-09-19 改版：从"面板亮着往上喷"改成"面板先消失、再四散爆开"
// ---------------------------------------------------------------------------
//  所有者原话："窗口先消失（也可以想成窗口可见部分隐藏），然后窗口爆开，变成飞溅粒子，
//  向四处飞溅。这就是为什么窗口要传达出'紧张'的情绪，因为它要爆开了。"
//  于是三件事同时改了，而且它们是**一件事**的三个面，不要拆开看：
//   1. 起爆点铺满**整个面板矩形**（原来是下半部一条带）—— 爆开的是那块板子本身；
//   2. 初速度**径向向外**（原来向上为主）—— 从面板中心朝四周飞，不是往上喷；
//   3. 重力降到 1/4（见 particles.cpp 的 kGravity 注释）—— 否则"四散"会被拉成
//      一条向下的尾巴，而那不是"爆开"。
//  "面板先消失"不在这一层：粒子一个像素都不负责面板。它是渲染层的一件事
//  （renderer.cpp 的 g_panelGone：第 3 击那一帧面板整层不再画，不淡出、不收缩）。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "widget_display.h"   // AmbienceColor

namespace dshb {

// ---------------------------------------------------------------------------
// 数量与时长（设计 §11.6 给的区间；这里同时是**降级顺序第 1 档**的定义）
// ---------------------------------------------------------------------------
//  §11.6：完整档 = 飞溅 + 拖尾 + 淡出，30–40 个粒子，总时长 ≤ 500 ms。
//  ★ 这些常量属于"看一眼觉得不对就改"的东西，所以按 tuning.h 的约定也放这里 ——
//    但它们的**唯一出处是这个模块**：粒子几何只有一处实现，探针也读同一份常量。
//  ★ ★ 数量为什么从 36 抬到 56（超出 §11.6 的 30–40）：§11.6 那个区间是按**旧形状**
//    （下半部一条带、往上喷）定的。改成"铺满整个面板矩形"之后，同样 36 颗要摊在
//    315x129 的面积上，实测（.dsh 里的离线模型 + 导帧量像素，同一份散列）面板内
//    16 px 的格子只有 27% 被碰到、面板面积里的墨迹只有 894 px²（2.2%）——
//    看起来是"稀疏的几点"，不是"这块板子碎开了"。抬到 56 之后同一指标是 64/180
//    （35.6%）、1442 px²（3.5%），而帧耗时与 36 颗**分不开**（见本回合 [framestats]）。
//    再往上（64 颗）只多 8% 覆盖率，边际收益已经很小，所以停在 56。
//  ★ ★ 降级顺序（§11.6）怎么落在这三个常量上，以及**量出来的结论**：
//    第 1 档 = 现在这一组（56 颗 + 拖尾，拖尾打开时用 count 那一档）；
//    第 2 档 = 把 kShutdownParticleTrails 改成 false（自动换成 count/2 = 28 颗、不画拖尾）；
//    第 3/4/5 档不在粒子里（粒子只在实体区内生成、或退化为内容收缩 + 淡出）。
//    2026-09-19 实测：完整档的帧耗时与**同一台机器的基线**没有可测差异
//    （--no-present：基线 p50 22.99 / p99 29.86 ms，完整档 p50 23.55 / p99 28.53 ms），
//    而且粒子那一段自己的 p99 与整体 p99 也分不开。所以**停在**第 1 档：不需要降级。
//    （§11.3 的 p99 < 8 ms 是另一件事：这台机器光画 475×289 的面板本身就已经 ~22 ms，
//      那是既有代价，与粒子无关，见本回合的 [framestats] 四组对照。）
inline constexpr int kShutdownParticleCount = 56;          // 见上面"为什么不是 36"
inline constexpr int kShutdownParticleCountHalf = kShutdownParticleCount / 2;   // 28（第 2 档）
inline constexpr bool kShutdownParticleTrails = true;      // 第 2 档要关掉的就是它
inline constexpr double kShutdownParticleSeconds = 0.50;   // 总时长上限，一个粒子都不许活过它
// 起爆错开：横跨这段时间依次起爆（最后一颗粒子在这段时间的末尾出生）。
// ★ 与寿命的关系是**加法**：总时长 = 本值 + 最长寿命，必须 ≤ kShutdownParticleSeconds。
//   static_assert 在 particles.cpp 里守着这条（写错方向会静默越过 500 ms）。
// ★ 为什么最早那一颗不是"第 0 帧就到"：散列给的第一颗出生时刻是 0.0027 s（不是恰好 0），
//   所以导出第 0 帧时画面上还没有粒子——面板已经没了，紧跟着（第 1 帧，16.7 ms 后）
//   才看到碎屑从这个矩形的位置炸开。这正是所有者要的顺序，一个像素的淡出都没有。
inline constexpr double kShutdownSpawnWindowSeconds = 0.06;
inline constexpr double kShutdownParticleStepSeconds = 1.0 / 60.0;   // 最小仿真步（= 导帧的 k/60）

// 一个粒子。
// ★ trail 是**环形**的：每 kShutdownTrailSampleEverySteps 步写一格，绕回开头继续写。
//   用环形而不是 deque/vector 搬移，是因为绘制要按"从旧到新"取，而下标一算就出来；
//   每帧搬一次 56x10 个点也不会崩，但那属于白付的代价。
inline constexpr int kShutdownTrailPoints = 10;            // 每颗粒子留几个拖尾采样点
inline constexpr int kShutdownTrailSampleEverySteps = 1;   // 每几个仿真步采一次

struct ShutdownParticle {
    double x = 0.0, y = 0.0;         // 当前位置（DIP，画布坐标系，原点在画布左上角）
    double vx = 0.0, vy = 0.0;       // 速度（DIP/秒）
    double rDip = 3.0;               // 半径（DIP）；画成圆头线段时它就是线宽的一半
    double bornSeconds = 0.0;        // 出生时刻（相对粒子系统自己的 0 点）
    double lifeSeconds = 0.35;       // 寿命：到点即淡到 0 并被移除

    float cr = 1.0f, cg = 1.0f, cb = 1.0f;   // 直通颜色（0..1）——**出生那一刻**的氛围色

    // 拖尾采样（环形）。head 是"下一格要写哪里"，所以最新那一点是 head-1。
    double trailX[kShutdownTrailPoints]{};
    double trailY[kShutdownTrailPoints]{};
    int head = 0;
    int trailCount = 0;

    // 拖尾点从**最旧**到**最新**的下标（写进 out 的是数组下标，不是坐标）。
    // 返回写了几个。这个函数是"环怎么读"的唯一出处，绘制与探针都用它。
    int TrailIndicesOldestFirst(int* out, int cap) const;
};

// 粒子系统。**没有时钟**：时间由调用方喂（dt 或"直接推到某个年龄"）。
class ShutdownParticles {
public:
    // 发一次粒子：清掉上一轮的，用给定颜色按确定性种子撒点。
    // 返回实际撒出来的粒子数（= 这一档的数量，见 kShutdownParticleCount/Half）。
    // ★ 颜色是**调用方给的**、不是这里算的：生产给的是这一帧 WidgetFrame 里的
    //   ambientColor，也就是屏幕上那一刻真实用的那个颜色（理由写在 renderer.cpp）。
    int Start(const AmbienceColor& colour, double canvasWidthDip, double canvasHeightDip);

    // 推进 dt 秒。dt <= 0 是空操作；dt 很大时内部按最小步切片（见实现里的"为什么"）。
    void Advance(double dtSeconds);

    // 直接推到某个年龄（秒）：从 0 起按最小步往复，所以**与逐帧播放同一条路径**。
    // 只给探针/导帧用；生产路径不调它（生产是 Advance）。
    void AdvanceToAge(double ageSeconds);

    bool active() const { return !particles_.empty(); }
    double ageSeconds() const { return age_; }
    const std::vector<ShutdownParticle>& particles() const { return particles_; }

    // 这一轮用了几个粒子、拖尾开没开（降级档位）。日志与验收要引用它，不许自己抄常量。
    struct Stats {
        int count = 0;          // 撒出来的粒子数
        int drawn = 0;          // 上一次 AdvanceToAge/Advance 之后还活着的（供日志比对）
        bool trails = false;
        double lifetime = 0.0;  // 这一轮的**总时长**（起爆窗口 + 最长寿命）
        unsigned seed = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    std::vector<ShutdownParticle> particles_;
    double age_ = 0.0;
    int stepNo_ = 0;   // 仿真步序号（拖尾采样相位）；Start 时归零，见 particles.cpp
    Stats stats_{};
};

// 一个粒子在 age 秒时的**可见度**（0..1），只含淡出曲线，不含颜色。
// ★ 为什么单独抽出来：绘制与探针都要这条曲线，两处各写一遍必然会分叉；而且它是纯函数，
//   可以被离线逐点核对（"淡出是连续的、末端到 0"这句话因此有凭据，不靠眼睛）。
float ShutdownParticleAlpha(double age, double lifeSeconds);

// 日志行（UTF-8）：数量、拖尾、种子、总时长、当前还活着几个。生产与探针打同一行。
std::string ShutdownParticlesLogLine(const ShutdownParticles& p);

}  // namespace dshb
