// 单调三次插值（Fritsch–Carlson, PCHIP 的一支）
//
// 用途（D3/D6）：曲线只在采样时刻知道余额，两次采样之间的形状是**猜**的。
// 既然要猜，就要保证猜得"不越界"：
//
//   * 普通三次样条（自然样条 / Catmull-Rom）在样本之间会**过冲**——
//     一段缓降（100 -> 99.9 -> 99.8）会在头尾各鼓出一小块，
//     画出账户里从未出现过的余额。计划的 D6 明确要求避免这个。
//   * 单调三次在原数据单调的区间内保持单调，且必不过冲：
//     曲线始终夹在相邻两个样本值之间。
//
// 另外两点：
//   * 支持**非等距**采样 —— 我们的自适应节奏是 3~10 秒，间隔本来就不均匀。
//   * 采样时刻 ≠ 数据变化的时刻（官网在我们两次请求之间变化）。插值只能给出
//     "区间内的合理形状"，无法恢复"变化发生在哪一刻"——这一点写在这里，
//     免得以后误以为曲线上的斜率有什么物理意义。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dshb {

class MonotoneCurve {
public:
    // x 必须严格递增（时间），y 为对应值。点数 < 2 时曲线为空。
    void Build(const std::vector<double>& x, const std::vector<double>& y);

    // 求值；xq 超出范围时返回端点值（不外推——外推就是在编数据）。
    double Eval(double xq) const;

    bool empty() const { return xs_.empty(); }
    size_t size() const { return xs_.size(); }

private:
    std::vector<double> xs_;
    std::vector<double> ys_;
    std::vector<double> ms_;   // 每个样本点的切线斜率
};

// 自检：验证"精确穿过样本点"与"不过冲"两条性质。
// 返回 true = 全部通过；report（可为空）里是逐条结果，便于日志直接打出来。
bool SelfTestMonotoneCurve(std::string* report);

}  // namespace dshb
