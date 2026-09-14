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
#include <vector>

namespace dshb {

// 余额数字的显示状态
//
// ★ 为什么是"定时长"而不是"指数逼近"（被实测逼出来的结论）：
//   指数逼近永远到不了目标，只能无限接近。它的后果不只是"数字慢"——
//   滚动期间渲染层必须显示"起点文本"，于是**数字看起来一直停在旧值上**，
//   直到逼近到吸附阈值为止（实测约 1 秒）。视觉上就是"数字根本没变"。
//   改成固定时长 + 缓动：滚动期明确开始、明确结束，两头都能对上。
class DisplayedAmount {
public:
    // 滚动时长（秒）。C4 会调它——要点是它**不与采样间隔挂钩**（采样固定 10 秒）。
    double rollSeconds = 0.40;

    // 吸附阈值（元）：差距小于这个就直接贴上去，不做无意义的滚动。
    double snapYuan = 0.005;

    // 收到新样本。做一次确认，避免"瞬间 0"把界面闪成灰色。
    void OnSample(const Sample& s);

    // 每帧推进。返回当前应当显示的余额（元）。
    double Update(double dtSeconds);

    bool hasValue() const { return hasValue_; }
    double value() const { return value_; }
    double target() const { return target_; }

    // 滚动进度：0 = 停在旧值，1 = 已落在新值上。逐位滚动的竖直偏移由它驱动。
    double rollFraction() const { return rollFraction_; }
    const std::string& rollOldText() const { return rollOldText_; }
    const std::string& rollNewText() const { return rollNewText_; }
    bool rolling() const { return rollFraction_ < 1.0 && !rollOldText_.empty(); }

    // 滚动期间应当显示哪一段文本。
    // ★ 数字必须**冻结**成"起点文本"或"目标文本"两者之一，不能每帧按插值后的
    //   数值重算——那样每帧都换一套数字，看起来就是一闪一闪（实测确认：
    //   每帧 text 从 20.30 变到 21.47 再变到 23.32，闪的来源就在这里）。
    //   竖直偏移负责"动"，文本负责"内容"，两者不能同时变。
    std::string TextToShow() const;
    double AmountToShow() const;

    // 是否处于"连续两次采样都是 0"的确认态
    bool zeroConfirmed() const { return zeroConfirmed_; }

private:
    bool hasValue_ = false;
    double value_ = 0.0;
    double target_ = 0.0;
    bool zeroPending_ = false;
    bool zeroConfirmed_ = false;
    double latest_ = 0.0;

    // 逐位滚动用的量：起点值、目标值、以及已经滚了多久。
    // rollFraction_ 由 rollElapsed_/rollSeconds 算出，**不自己衰减**——
    // 让进度和数值各走一套是上一版的错误来源。
    std::string rollOldText_;
    std::string rollNewText_;
    double rollFromValue_ = 0.0;
    double rollElapsed_ = 0.0;
    double rollFraction_ = 1.0;
};

// 一帧要显示的东西。**渲染层只认这个结构**，它不关心余额是怎么来的。
//
// ★ 数字滚动的正确理解（所有者明确纠正过一次）：
//   "滚动"不是指数值平滑逼近，而是**每一位数字上下滑动**——旧数字往上滑出去、
//   新数字从下面滑进来，像里程表。第一版只做了数值插值，数字在原地变脸，
//   看起来只是"数字变了"，没有滚动的痕迹。
struct NumberRoll {
    bool active = false;
    // 变化的那一段：旧文本与新文本。长度相同（同一个格式，等宽数字），
    // 所以可以逐位配对。
    std::string oldText;
    std::string newText;
    int changeFrom = 0;      // 第一个不同的位置
    int changeTo = 0;        // 最后一个不同的位置（含）
    double fraction = 1.0;   // 0 = 还在旧位置，1 = 已落位
};

struct WidgetFrame {
    ConnState state = ConnState::ColdStart;
    bool showAmount = false;        // 数字该不该显示（无数据时显示占位符）
    std::string amountText;         // 已格式化的两位小数
    const wchar_t* currencySymbol = L"";   // 空串 = 币种未知，**不默认 ¥**
    const wchar_t* statusText = L"";       // 标题行/状态文案
    std::string zeroTimeText;       // 清零预估（C9 填；现在留占位）
    NumberRoll roll;
};

// 把状态 + 显示值组装成一帧。放在这里而不是渲染层，是为了让"显示什么"
// 和"怎么画"分开——渲染层不该知道余额为 0 和查不到有什么区别。
WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol);

// 状态文案行（C6）。标题兼状态行：紧急状态除了颜色变化，文字也要跟着换，
// 否则只靠颜色编码状态——色觉障碍、屏幕反光、截图转述三种情况下都会失效。
const wchar_t* StatusTextFor(ConnState state);

}  // namespace dshb
