// 独立的运动曲线探针：直接打印 tau 与实际收敛曲线。
//
// ★ 为什么单独一个程序，而不是写进 --selftest-b：
//   自检块里满是"先喂样本再断言"的状态操作，我连着两次在那里把中间量算错，
//   白查两轮。把要观察的东西放进一个**只做一件事**的小程序，就没有这种余地了。
//
// 用法：tauprobe.exe [tau] [startYuan] [targetYuan] [frames]

#include "../src/widget_display.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    dshb::DisplayedAmount d;
    if (argc > 1) d.tau = std::atof(argv[1]);
    const double start = (argc > 2) ? std::atof(argv[2]) : 19.90;
    const double target = (argc > 3) ? std::atof(argv[3]) : 99.50;
    const int frames = (argc > 4) ? std::atoi(argv[4]) : 130;

    printf("tau=%.4f snapYuan=%.4f start=%.2f target=%.2f frames=%d\n", d.tau, d.snapYuan, start,
           target, frames);

    dshb::Sample s0{};
    s0.amountsOk = true;
    s0.total = dshb::Amount{static_cast<dshb::AmountRaw>(std::llround(start * 10000.0))};
    d.OnSample(s0);

    dshb::Sample s1{};
    s1.amountsOk = true;
    s1.total = dshb::Amount{static_cast<dshb::AmountRaw>(std::llround(target * 10000.0))};
    d.OnSample(s1);

    const double span = start - target;
    printf("%6s %12s %10s %10s %8s\n", "frame", "value", "gap", "remaining", "rolling");
    int snapFrame = -1;
    for (int i = 0; i < frames; ++i) {
        d.Update(1.0 / 60.0);
        const double gap = target - d.value();
        const double remaining = (span == 0.0) ? 0.0 : (d.value() - target) / span;
        if (i < 6 || (i % 40 == 0)) {
            printf("%6d %12.4f %10.4f %10.6f %8d\n", i, d.value(), gap, remaining,
                   d.rolling() ? 1 : 0);
        }
        // 第一次不再 rolling 的那一帧就是截断发生的时刻
        if (snapFrame < 0 && !d.rolling()) snapFrame = i;
    }
    printf("--- tau=%.3f：截断发生在第 %d 帧 = %.3f 秒 ---\n", d.tau, snapFrame,
           snapFrame / 60.0);
    // "肉眼基本到位"的时间：走到目标的 99% 处
    {
        dshb::DisplayedAmount e;
        e.tau = d.tau;
        e.OnSample(s0);
        e.OnSample(s1);
        int f99 = -1;
        for (int i = 0; i < 2000; ++i) {
            e.Update(1.0 / 60.0);
            const double remain = (span == 0.0) ? 0.0 : (e.value() - target) / span;
            if (f99 < 0 && remain < 0.01) f99 = i;
        }
        printf("    走到 99%% 处用了 %d 帧 = %.3f 秒\n", f99, f99 / 60.0);
    }
    printf("结束: value=%.4f gap=%.4f rolling=%d\n", d.value(), target - d.value(),
           d.rolling() ? 1 : 0);
    printf("一步的理论衰减系数 = 1 - exp(-(1/60)/tau) = %.6f\n",
           1.0 - std::exp(-(1.0 / 60.0) / d.tau));
    return 0;
}
