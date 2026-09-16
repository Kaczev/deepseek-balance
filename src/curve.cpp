#include "curve.h"

#include <cmath>

namespace dshb {

namespace {
// 符号函数：>0 -> 1，<0 -> -1，=0 -> 0
int Sign(double v) {
    if (v > 0.0) return 1;
    if (v < 0.0) return -1;
    return 0;
}
}  // namespace

void MonotoneCurve::Build(const std::vector<double>& x, const std::vector<double>& y) {
    xs_.clear();
    ys_.clear();
    ms_.clear();
    if (x.size() != y.size() || x.size() < 2) return;

    // x 必须严格递增；不递增的输入直接拒绝（宁可没有曲线，也不要画错的曲线）
    for (size_t i = 1; i < x.size(); ++i) {
        if (!(x[i] > x[i - 1])) return;
    }

    const size_t n = x.size();
    xs_ = x;
    ys_ = y;
    ms_.assign(n, 0.0);

    // 区间斜率（secants）
    std::vector<double> d(n - 1, 0.0);
    for (size_t i = 0; i + 1 < n; ++i) {
        d[i] = (ys_[i + 1] - ys_[i]) / (xs_[i + 1] - xs_[i]);
    }

    // 端点：单侧斜率（不用"自然样条"的二阶导为零，那个正是过冲的来源之一）
    ms_[0] = d[0];
    ms_[n - 1] = d[n - 2];

    // 内点：先取相邻区间斜率的加权平均，再做单调性限制
    for (size_t i = 1; i + 1 < n; ++i) {
        const double h0 = xs_[i] - xs_[i - 1];
        const double h1 = xs_[i + 1] - xs_[i];
        const double w0 = 2.0 * h1 + h0;
        const double w1 = h1 + 2.0 * h0;
        ms_[i] = (w0 + w1) / (w0 / d[i - 1] + w1 / d[i]);   // 加权调和平均
        if (!std::isfinite(ms_[i])) ms_[i] = 0.0;
    }

    // Fritsch–Carlson 限制：保证不过冲
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0) {
            // 平段：两端切线都必须为 0，否则会鼓出去
            ms_[i] = 0.0;
            ms_[i + 1] = 0.0;
            continue;
        }
        const double a = ms_[i] / d[i];
        const double b = ms_[i + 1] / d[i];
        // 单调性检查
        if (Sign(ms_[i]) != Sign(d[i])) ms_[i] = 0.0;
        if (Sign(ms_[i + 1]) != Sign(d[i])) ms_[i + 1] = 0.0;
        if (a < 0.0 || b < 0.0) continue;
        // 圆限制 a^2 + b^2 <= 9（Fritsch–Carlson 的充分条件）
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double t = 3.0 / std::sqrt(s);
            ms_[i] = t * a * d[i];
            ms_[i + 1] = t * b * d[i];
        }
    }
}

double MonotoneCurve::Eval(double xq) const {
    const size_t n = xs_.size();
    if (n == 0) return 0.0;
    if (n == 1 || xq <= xs_[0]) return ys_[0];
    if (xq >= xs_[n - 1]) return ys_[n - 1];

    // 二分找到所在区间
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (xs_[mid] <= xq) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    // 三次 Hermite 基
    const double h = xs_[lo + 1] - xs_[lo];
    const double t = (xq - xs_[lo]) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    return h00 * ys_[lo] + h10 * h * ms_[lo] + h01 * ys_[lo + 1] + h11 * h * ms_[lo + 1];
}

bool SelfTestMonotoneCurve(std::string* report) {
    std::string r;
    bool allOk = true;
    auto check = [&](bool ok, const char* name) {
        r += ok ? "PASS  " : "FAIL  ";
        r += name;
        r += '\n';
        if (!ok) allOk = false;
    };

    // 一组用例；每条都检查两个性质：
    //   1) 曲线必须精确穿过每个样本点
    //   2) 曲线在任意位置都不得越出"相邻两样本值"构成的带子（= 不过冲）
    struct Case {
        const char* name;
        std::vector<double> x;
        std::vector<double> y;
    };
    const std::vector<Case> cases = {
        {"slow drain (the shape a plain spline overshoots on)",
         {0, 10, 20, 30}, {100.00, 99.90, 99.85, 99.85}},
        {"flat then drop", {0, 10, 20, 30}, {100.00, 100.00, 99.00, 99.00}},
        {"uneven spacing (3s / 10s / 1s, our adaptive cadence)",
         {0, 3, 13, 14, 24}, {50.00, 49.99, 49.99, 49.95, 49.90}},
        {"rising (a top-up)", {0, 10, 20}, {10.00, 60.00, 60.00}},
        {"two points only", {0, 10}, {100.00, 90.00}},
    };

    for (const Case& c : cases) {
        MonotoneCurve curve;
        curve.Build(c.x, c.y);

        bool exact = (curve.size() == c.x.size());
        for (size_t i = 0; exact && i < c.x.size(); ++i) {
            if (std::fabs(curve.Eval(c.x[i]) - c.y[i]) > 1e-9) exact = false;
        }

        bool inBand = true;
        for (size_t i = 0; i + 1 < c.x.size(); ++i) {
            const double lo = std::fmin(c.y[i], c.y[i + 1]);
            const double hi = std::fmax(c.y[i], c.y[i + 1]);
            for (int s = 0; s <= 400; ++s) {
                const double t = c.x[i] + (c.x[i + 1] - c.x[i]) * (s / 400.0);
                const double v = curve.Eval(t);
                if (v < lo - 1e-9 || v > hi + 1e-9) inBand = false;
            }
        }

        r += "--- ";
        r += c.name;
        r += '\n';
        check(exact, "passes through every sample exactly");
        check(inBand, "stays inside the neighbouring-sample band (no overshoot)");
    }

    if (report) *report = r;
    return allOk;
}

}  // namespace dshb
