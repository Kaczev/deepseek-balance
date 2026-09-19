// deepseek-balance v0.2 —— 关闭粒子的运动学（设计的理由写在 particles.h 顶部）
//
// 这一份代码里有四处是**故意**这么写、而不是顺手那么写的，先记在这里：
//
//  1. **没有 std::random_device、没有 rand()、没有时钟。** 种子是常量，取样是 PCG 式的
//     整数散列。理由不是"洁癖"，是验收：量像素必须可复现 —— 同一帧号导出两次要逐像素
//     相同，否则"这一帧有 56 个粒子"这句话每次量出来都不一样，也就不是证据。
//  2. **拖动用指数衰减，不用"每步乘一个系数"。** 每步乘系数会让轨迹随帧率变化：60 fps
//     与 30 fps 播出来的粒子位置不同。指数衰减是 (1 - e^(-dt/tau)) 的形式，与步长无关。
//     重力则相反，就是 v += g*dt（真的与步长无关的只有它这一阶）。
//  3. **大 dt 要切小步。** 窗口被遮挡、系统忙、调试器停顿都会给一个几百毫秒的 dt；
//     一步走完的话粒子会直接从面板里"跳"到画面外，并且越过拖尾采样。所以内部按
//     kShutdownParticleStepSeconds 切片，切片上限定成一个常数（见 kMaxSliceSeconds）。
//  4. **"切向"扰动直接乘在垂直分量上，不绕 atan2 一圈。** 见 kPerturb 的注释：
//     与 atan2 版本在数学上是同一件事，但少一次三角函数、也不会在方向接近 ±pi 时
//     出现"同一颗粒子两个角度"这种只能靠调试才发现的分叉。

#include "particles.h"

#include "tuning.h"   // kEntityWidthDip / kEntityHeightDip：起爆点铺的就是面板矩形

#include <cmath>
#include <cstdio>

namespace dshb {
namespace {

// ---------------------------------------------------------------------------
// 运动参数。它们只出现在这里：粒子的形状与轨迹只有一处定义。
// ---------------------------------------------------------------------------
//  ★ 重力：原来 900（= 旧形状"向上喷"的配套值：喷上去、再落下来）。现在是**四散爆开**，
//    重力如果还那么大，0.48 s 的行程里会自由落体 0.5*900*0.48^2 = 104 DIP —— 而同一
//    颗粒子朝外只走了 60..80 DIP，于是整群粒子被拉成一条**向下的尾巴**，上下不再对称，
//    那正是"喷水/下雨"，不是所有者要的"爆开"。
//    225 = 原来的 1/4：到最后一帧的下沉量是 0.5*225*0.48^2 = 26 DIP，只有最外那圈
//    粒子朝外行程（约 80 DIP）的三分之一，屏幕上读成"碎屑最后微微沉一下"。这条比值
//    （下沉/朝外 = 0.32）就是"为什么是 225 而不是 0"的全部依据；离线模型与本回合导帧
//    逐帧量到的外接框上下扩张幅度都能核到它。
constexpr double kGravity = 225.0;         // DIP/s^2，向下（原来是 900）
// 阻尼时间常数（秒）：越快越小。0.30 与"最多活 0.48 s"配对：末速衰减到 e^(-1.6)=0.20，
// 于是每个粒子都有一条"先快后慢"的减速尾巴，看起来像被空气咬住，而不是匀速平移。
constexpr double kDampingTau = 0.30;
// 初速区间（DIP/s）。★ 这是"爆开"与"整体平移"的分界：下限不能是 0（慢的那些粒子停在
// 原地＝没爆开），上限也不能太大（0.48 s 内会飞出画布 475x289 之外被裁掉）。
// 取 150..340：乘上 tau*(1-e^(-t/tau)) 的行程积分，0.48 s 时是 65..147 DIP，
// 而面板半宽只有 157 DIP —— 最外圈正好把外接框推到面板矩形之外一点点，逐帧扩张看得见。
constexpr double kSpeedMin = 150.0;
constexpr double kSpeedSpan = 190.0;
// 方向扰动：v = speed * (径向 + k * 切向)，k = ±kPerturb*(1 + u*kPerturbSpan)。
// ★ 为什么需要扰动：纯径向的话，一颗粒子的位置与它的方向严格一一对应，等距的圆环
//   会像"从中心散开的辐条"，一眼看出是算出来的。0.30 的切向分量 = 约 ±17°，
//   足够打散辐条但不足以让粒子明显混到相邻方向上去。
//   （上界 1 + 0.6 = 倍数 0.48 => 约 ±26°。）
constexpr double kPerturb = 0.30;
constexpr double kPerturbSpan = 0.60;
// 起爆点在面板矩形里的铺法：**朝心收 8%**，于是四边留一圈薄边不生成粒子。
// ★ 为什么不是铺满整矩形：粒子自己还有半径（1.9..3.9 DIP），铺到边上会让外接框在
//   第 0 帧就顶到画布外扩余量里去；收 8%（每边约 12.6 DIP）之后，第一帧的外接框
//   正好落在面板矩形之内、随后逐帧向外扩张 —— "从窗口形状出来"这句话因此可量。
constexpr double kSpawnInsetFraction = 0.92;
constexpr double kLifeMin = 0.32;           // 寿命下限（秒）
constexpr double kLifeSpan = 0.12;          // 寿命范围（上限 = Min + Span = 0.44）
constexpr double kRadiusMin = 1.9;          // 半径（DIP）
constexpr double kRadiusSpan = 2.0;
// 总时长账（必须 ≤ kShutdownParticleSeconds）：起爆窗口 + 最长寿命。
// ★ 这里**故意不做"末颗截断"**（旧版有）：截断会让最晚出生的那些粒子寿命不同、死亡时刻
//   挤在同一帧，虽然更保险，但会让"寿命是 0.32..0.44 的散列值"这句话不再成立。
//   改成让 static_assert 守住预算：谁把常数改坏，构建时就报错，而不是静默越过 500 ms。
constexpr double kMaxBirthSeconds = kShutdownSpawnWindowSeconds;
constexpr double kMaxLifeSeconds = kLifeMin + kLifeSpan;
static_assert(kMaxBirthSeconds + kMaxLifeSeconds <= kShutdownParticleSeconds,
              "起爆窗口 + 最长寿命必须 <= kShutdownParticleSeconds（§11.6 的 500 ms 是总时长）");
// 一步最多推进多少秒：再大就切（见文件顶部第 3 条）。
constexpr double kMaxSliceSeconds = 1.0 / 30.0;

// 种子。常量 —— 理由见文件顶部第 1 条。
constexpr uint32_t kSeed = 0x5eed1234u;

// 整数散列（PCG 输出函数那一族）。只用它的高位，够均匀且完全确定性。
uint32_t Hash(uint32_t x) {
    x ^= x >> 17;
    x *= 0xed5ad4bbu;
    x ^= x >> 11;
    x *= 0xac4c1b51u;
    x ^= x >> 15;
    return x;
}

// [0,1) 均匀。用高位而不是低位：低位那几位的周期很短（这是散列，不是真随机源）。
double UnitAt(uint32_t salt) {
    return static_cast<double>(Hash(kSeed + salt * 0x9e3779b9u) >> 8) / 16777216.0;
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

int ShutdownParticle::TrailIndicesOldestFirst(int* out, int cap) const {
    if (!out || cap <= 0) return 0;
    const int n = (trailCount < kShutdownTrailPoints) ? trailCount : kShutdownTrailPoints;
    const int use = (n < cap) ? n : cap;
    // 最旧那一点的下标：环形里"下一格要写"的位置再往回数 n 格。
    int idx = head - n;
    while (idx < 0) idx += kShutdownTrailPoints;
    for (int i = 0; i < use; ++i) {
        out[i] = idx;
        if (++idx == kShutdownTrailPoints) idx = 0;
    }
    return use;
}

float ShutdownParticleAlpha(double age, double lifeSeconds) {
    if (lifeSeconds <= 0.0) return 0.0f;
    double u = age / lifeSeconds;
    if (u <= 0.0) u = 0.0;
    if (u >= 1.0) return 0.0f;
    // 1.6 次幂：前段几乎不透明、末段收得快。要的是"干脆消失"，不是拖一条长尾巴
    // （尾巴越长越容易在最后一帧留下看不见但量得出来的残影）。
    return static_cast<float>(std::pow(1.0 - u, 1.6));
}

int ShutdownParticles::Start(const AmbienceColor& colour, double canvasWidthDip,
                             double canvasHeightDip) {
    particles_.clear();
    age_ = 0.0;
    stepNo_ = 0;

    const int want = (kShutdownParticleTrails ? kShutdownParticleCount : kShutdownParticleCountHalf);
    // ★ 中心取**面板矩形**的中心，不是画布中心。画布四周还有 80 DIP 的透明余量，
    //   用画布中心的话"径向"的起点会偏到面板外，四散就不再关于面板对称。
    //   面板在画布里的位置由 kMarginDip 唯一决定（与 renderer.h 的 kCanvas* 同一个口径）。
    const double cx = canvasWidthDip * 0.5;
    const double cy = canvasHeightDip * 0.5;
    const double halfW = kEntityWidthDip * 0.5 * kSpawnInsetFraction;
    const double halfH = kEntityHeightDip * 0.5 * kSpawnInsetFraction;

    particles_.reserve(static_cast<std::size_t>(want));
    for (int i = 0; i < want; ++i) {
        const uint32_t salt = static_cast<uint32_t>(i) * 7u + 1u;
        const double u1 = UnitAt(salt + 0);
        const double u2 = UnitAt(salt + 1);
        const double u3 = UnitAt(salt + 2);
        const double u4 = UnitAt(salt + 3);
        const double u5 = UnitAt(salt + 4);
        const double u6 = UnitAt(salt + 5);
        const double u7 = UnitAt(salt + 6);
        const double u8 = UnitAt(salt + 7);

        ShutdownParticle p;
        // 起爆点：**均匀铺满面板矩形**（原来的下半部一条带已经删掉）。
        // ★ 均匀而不是"都堆在中心"：堆在中心的话第一帧的外接框只有几个像素，
        //   "面板整个变成碎屑"这个读法就不成立（验收 3 量的正是第 0/3/4 帧的外接框）。
        p.x = cx + (u1 * 2.0 - 1.0) * halfW;
        p.y = cy + (u2 * 2.0 - 1.0) * halfH;

        // 方向：**从面板中心指向该粒子**（径向向外），再叠一点切向扰动。
        // 中心那一点没有方向可言（d 趋 0），退化成"向右"是任意的但确定 —— 与随机选一个
        // 方向相比，它至少不会让同一份种子在两次运行里给出不同的画面。
        double dx = p.x - cx;
        double dy = p.y - cy;
        double d = std::sqrt(dx * dx + dy * dy);
        if (d < 1e-6) {
            dx = 1.0;
            dy = 0.0;
            d = 1.0;
        }
        const double rx = dx / d;
        const double ry = dy / d;
        // 切向 = 径向逆时针转 90°；k 的正负由 u3 定，幅度再被 u4 拉宽（快慢/角度都别太整齐）。
        const double k = (u3 * 2.0 - 1.0) * kPerturb * (1.0 + u4 * kPerturbSpan);
        const double speed = kSpeedMin + u5 * kSpeedSpan;
        p.vx = speed * (rx - k * ry);
        p.vy = speed * (ry + k * rx);

        p.rDip = kRadiusMin + u6 * kRadiusSpan;
        p.bornSeconds = u7 * kShutdownSpawnWindowSeconds;   // 错开起爆
        p.lifeSeconds = kLifeMin + u8 * kLifeSpan;

        p.cr = colour.r;
        p.cg = colour.g;
        p.cb = colour.b;

        // 拖尾的初始状态：环里先放一个"出生点"，否则第一帧没有可画的段。
        p.trailX[0] = p.x;
        p.trailY[0] = p.y;
        p.head = 1 % kShutdownTrailPoints;
        p.trailCount = 1;

        particles_.push_back(p);
    }

    stats_.count = static_cast<int>(particles_.size());
    stats_.drawn = stats_.count;
    stats_.trails = kShutdownParticleTrails;
    // ★ 报的是**总时长**（起爆窗口 + 最长寿命），不是最长寿命：§11.6 的 500 ms 说的是
    //   从第 3 击那一刻到画面干净为止，日志里那一行必须与验收量的东西是同一个量。
    stats_.lifetime = kShutdownSpawnWindowSeconds + kMaxLifeSeconds;
    stats_.seed = kSeed;
    return stats_.count;
}

void ShutdownParticles::Advance(double dtSeconds) {
    if (dtSeconds <= 0.0 || particles_.empty()) return;
    double left = dtSeconds;
    while (left > 1e-9) {
        const double h = (left > kMaxSliceSeconds) ? kMaxSliceSeconds : left;
        left -= h;
        age_ += h;

        const double damp = 1.0 - std::exp(-h / kDampingTau);
        for (std::size_t i = 0; i < particles_.size(); ++i) {
            ShutdownParticle& p = particles_[i];
            if (age_ < p.bornSeconds) continue;   // 还没起爆：位置与拖尾都不动
            p.vy += kGravity * h;
            p.vx -= p.vx * damp;
            p.vy -= p.vy * damp;
            p.x += p.vx * h;
            p.y += p.vy * h;
        }

        // 采样拖尾：**与仿真步对齐**，而不是与"帧"对齐。
        // ★ 用成员计数器而不是函数内 static：static 会把上一轮的步数带到下一轮，
        //   于是导帧（每次从一个新进程、新的一轮开始）与屏幕（连续播）相位不同。
        //   计数在 Start 里归零，所以每一轮都是同一串采样点。
        ++stepNo_;
        if (kShutdownTrailSampleEverySteps <= 1 || stepNo_ % kShutdownTrailSampleEverySteps == 0) {
            for (std::size_t i = 0; i < particles_.size(); ++i) {
                ShutdownParticle& p = particles_[i];
                if (age_ < p.bornSeconds) continue;
                p.trailX[p.head] = p.x;
                p.trailY[p.head] = p.y;
                if (++p.head == kShutdownTrailPoints) p.head = 0;
                if (p.trailCount < kShutdownTrailPoints) ++p.trailCount;
            }
        }
    }

    // 寿命到了就移除。淡出曲线在 u=1 处**恰好**是 0，所以移除时没有可见跳变 ——
    // "最后一帧干净消失"（验收 4）靠的就是这条：透明度先到 0，再被移出列表。
    std::size_t keep = 0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        const ShutdownParticle& p = particles_[i];
        if (age_ >= p.bornSeconds + p.lifeSeconds) continue;
        if (keep != i) particles_[keep] = p;
        ++keep;
    }
    particles_.resize(keep);
    stats_.drawn = static_cast<int>(particles_.size());
}

void ShutdownParticles::AdvanceToAge(double ageSeconds) {
    if (ageSeconds <= 0.0) return;
    // 从 0 起按最小步往复：这条路径与"真的播了 k 帧"完全一样，
    // 所以导出来的第 k 帧就是屏幕上的第 k 帧。
    const double step = kShutdownParticleStepSeconds;
    int steps = static_cast<int>(ageSeconds / step + 0.5);
    for (int i = 0; i < steps; ++i) Advance(step);
}

std::string ShutdownParticlesLogLine(const ShutdownParticles& p) {
    const ShutdownParticles::Stats& s = p.stats();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "count=%d drawn=%d trails=%d seed=0x%08X lifetimeMs=%.0f ageMs=%.1f",
                  s.count, s.drawn, s.trails ? 1 : 0, s.seed, s.lifetime * 1000.0,
                  p.ageSeconds() * 1000.0);
    return std::string(buf);
}

}  // namespace dshb
