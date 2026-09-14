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

#include <cstdint>
#include <string>

namespace dshb {

// 余额数字的显示状态
class DisplayedAmount {
public:
    // 滚动时间常数（秒）。C4 会调它——数值本身是可变的，要点是它**不与采样间隔挂钩**。
    double tau = 0.18;

    // 吸附阈值（元）：差距小于这个就直接贴上去，不再慢慢逼近。
    double snapYuan = 0.005;

    // 收到新样本。做一次确认，避免"瞬间 0"把界面闪成灰色。
    void OnSample(const Sample& s);

    // 每帧推进。返回当前应当显示的余额（元）。
    double Update(double dtSeconds);

    bool hasValue() const { return hasValue_; }
    double value() const { return value_; }
    double target() const { return target_; }

    // 是否处于"连续两次采样都是 0"的确认态
    bool zeroConfirmed() const { return zeroConfirmed_; }

private:
    bool hasValue_ = false;
    double value_ = 0.0;
    double target_ = 0.0;
    bool zeroPending_ = false;
    bool zeroConfirmed_ = false;
    double latest_ = 0.0;
};

// 一帧要显示的东西。**渲染层只认这个结构**，它不关心余额是怎么来的。
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
