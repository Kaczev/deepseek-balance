// 显示层：把"跳变的测量值"变成"连续变化的显示值"（C2/C3/C4）
//
// 设计里有一条规则必须保留：**测量值可以突变，显示值必须渐进跟随**。
// 充值会让余额瞬间从 3 元跳到 100 元；如果直接显示，数字会闪一下。
//
// ★ 两个容易做错的地方，这里都按实测定死了：
//   1. 时间常数与采样间隔**解耦**。采样是固定 10 秒，但滚动时长是 0.6 秒级别的
//      固定值——绑在一起会让数字永远滚不完就被下一个值打断。
//   2. 必须**吸附**。纯指数逼近永远到不了目标，于是数字会永远停在
//      "99.9997"这种地方，显示成 100.00 又和目标差一点，看起来像在抖。

#pragma once

#include "state_machine.h"

#include <cmath>     // std::fabs（rolling() 用）
#include <cstdint>
#include <string>
#include <vector>

namespace dshb {

// 余额数字的显示状态
//
// ★ 为什么是"速率 + 连续函数"而不是"定时长"（所有者纠正过一次，这里记清楚）：
//
//   设计里写的是"各状态的变化都用连续的方法去控制；变量可以突变，但显示变量
//   必须逐渐变化、跟着那个突变变量"。那就是**速率参数**驱动的连续函数。
//
//   我一度改成"固定时长 0.40 秒 + 缓动"，那是为了绕开指数逼近的尾巴而打的补丁，
//   副作用有两个：
//     1. 引入了设计里没有的东西（一次跳跃有自己的时长）——于是"0.4 秒是什么时长"
//        这个问题本身就说明它不对；
//     2. 它和采样间隔（10 秒）脱钩：数字 0.4 秒走完，然后 9.6 秒纹丝不动。
//        这和"绑在采样间隔上"是同一类毛病，只是换了个方向。
//
//   正确的形态：**目标值每 10 秒换一次**，所以"永远降不到 0"根本不发生——
//   显示值一直在追一个一直在动的目标。唯一的危害是"最后半格磨蹭"，
//   那由截断（吸附阈值）解决：差到看不见就直接贴上去。
//
//   于是只有一个参数 τ（速率），没有时长。
class DisplayedAmount {
public:
    // 速率：显示值每秒钟走掉剩余差距的 (1 - e^(-dt/τ))。
    //
    // ★ 默认 0.10 秒是**量出来的**，不是拍的（tools/tauprobe.cpp 可复现）：
    //     τ     截断(走完)   99% 到位
    //     0.30    2.90 s      1.37 s    太慢，对 10 秒采样明显滞后
    //     0.20    1.93 s      0.92 s    偏慢
    //     0.10    0.97 s      0.45 s    ← 正对初稿写的"1 秒恰好滚动完成"
    //     0.06    0.57 s      0.27 s    偏急
    //     0.04    0.38 s      0.18 s    像瞬跳，失去滚动感
    //   τ 仍远小于采样间隔（10 秒），所以每次采样后数字都早早到位、看起来始终新鲜。
    double tau = 0.10;

    // 截断阈值（元，= 半个最小显示单位）：差距小于这个就直接吸附。
    // 指数逼近的尾巴在数值上永远不为 0，但在视觉上早就没有移动了；
    // 这条截断把那截"看不见的尾巴"切掉，最后一位数字才能干脆落定。
    double snapYuan = 0.005;

    // 收到新样本。做一次确认，避免"瞬间 0"把界面闪成灰色。
    void OnSample(const Sample& s);

    // 每帧推进。返回当前应当显示的余额（元）。
    double Update(double dtSeconds);

    bool hasValue() const { return hasValue_; }
    double value() const { return value_; }
    double target() const { return target_; }

    // 是否正在追一个还没到位的目标（渲染层据此决定轮子要不要转）
    bool rolling() const { return hasValue_ && std::fabs(target_ - value_) > 0.0; }

    // 距离目标的剩余比例，仅用于日志与自检（渲染不再需要）
    double remaining() const {
        const double span = rollFromValue_ - target_;
        return (span == 0.0) ? 0.0 : (value_ - target_) / span;
    }

    // 滚动期间应当显示哪一段文本。
    // ★ 数字文本只由**目标值**决定，不按每帧插值后的数值重算——那样每帧换一套
    //   数字，看起来就是一闪一闪（实测确认过）。竖直偏移（由当前值驱动）负责"动"，
    //   文本负责"内容"，两者不能同时变。
    std::string TextToShow() const;

    // 是否处于"连续两次采样都是 0"的确认态
    bool zeroConfirmed() const { return zeroConfirmed_; }

private:
    bool hasValue_ = false;
    double value_ = 0.0;      // 当前显示值：连续函数追着 target_ 走
    double target_ = 0.0;     // 目标值：每次采样直接改写（可以突变）
    bool zeroPending_ = false;
    bool zeroConfirmed_ = false;
    double latest_ = 0.0;

    // 这一段的起点值，用于自检报告"走了多少比例"
    double rollFromValue_ = 0.0;
};


struct WidgetFrame {
    ConnState state = ConnState::ColdStart;
    bool showAmount = false;        // 数字该不该显示（无数据时显示占位符）
    std::string amountText;         // 已格式化的两位小数
    const wchar_t* currencySymbol = L"";   // 空串 = 币种未知，**不默认 ¥**
    const wchar_t* statusText = L"";       // 标题行/状态文案
    std::string zeroTimeText;       // 清零预估（C9 填；现在留占位）
};

// 把状态 + 显示值组装成一帧。放在这里而不是渲染层，是为了让"显示什么"
// 和"怎么画"分开——渲染层不该知道余额为 0 和查不到有什么区别。
WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol);

// 状态文案行（C6）。标题兼状态行：紧急状态除了颜色变化，文字也要跟着换，
// 否则只靠颜色编码状态——色觉障碍、屏幕反光、截图转述三种情况下都会失效。
const wchar_t* StatusTextFor(ConnState state);

}  // namespace dshb
