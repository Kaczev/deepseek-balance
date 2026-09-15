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

#include "roll_axis.h"
#include "state_machine.h"
#include "tuning.h"

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

    // ★ 每一位当前的纵坐标（连续）。渲染层按 place 取值来画数字。
    //   静止时每一位正好落在自己的数字上（整数坐标）；滚动中落在两格之间，
    //   于是显示位与它旁边那位数字一起画出来。
    const std::vector<axis::PlaceCoord>& places() const { return places_; }

    // ★ 手动设定**显示值**并冻结：不做指数平滑。
    //   用途是排查与调参——必须能停在一个状态上看清楚，否则分不清是机制错还是过程错。
    //   注意：**只设显示值，不动 target_**。target_ 是"实际数字"，是每一位行程的终点；
    //   早先这里把 target_ 也设成 yuan，结果行程起点=终点，轮子永远不动（实测踩过）。
    void ForceDisplay(double yuan) {
        value_ = yuan;
        rollFromValue_ = yuan;
        rollStartValue_ = yuan;
        hasValue_ = true;
        frozen_ = true;
        trips_.clear();
        places_.clear();
    }

    bool frozen() const { return frozen_; }
    void SetCrisp(bool on) { crisp_ = on; }
    // 强行指定相位 0..1（>=0 生效）：用来停在行程的任意一刻看效果。
    void SetPhaseOverride(double p) { phaseOverride_ = p; }

    // 每位坐标的读数表。单位是"格"，乘 h 就是像素。每行：
    //   位次  纵实际坐标(S÷10^位次)  纵显示坐标  显示数字  两格之间  数字0画在何处
    // 数字0那一列是所有者问的：它相对参考点的偏移，能直接看出"这一位偏了多少"。
    std::string PlaceReport() const;
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

    // 每一位的纵坐标（见 places()）
    // 每一位自己的行程：从 from 走到 to。轮子只在自己这一位要变的时候才动。
    struct Trip {
        int place = axis::kNoPlace;
        double from = 0.0;   // 起点坐标（整数）
        double to = 0.0;     // 终点坐标（整数）
    };
    std::vector<Trip> trips_;
    // 每一位当前的纵坐标（见 places()），渲染层按它画
    std::vector<axis::PlaceCoord> places_;
    double rollStartValue_ = 0.0;   // 本次行程的起点金额（用于算进度）
    double phaseOverride_ = -1.0;   // >=0 时强行指定相位（调参/排查用）
    double phaseNow_ = 0.0;         // 本帧实际用的相位（读数用）

    // 手动冻结：为真时不推进显示值
    bool frozen_ = false;
    // true = 坐标取 S/n 的整数部分（静止读数清晰）；false = 用连续坐标 S/n（有"两格之间"）
    bool crisp_ = false;

    // 按当前显示值刷新每位坐标：目标是 floor(显示值 / 10^位次)，本帧朝它追赶 dt 秒。
    // "有哪些位次"由文本决定——高位是 0 时文本里没有这一位，于是自动隐藏。
    void AdvancePlaces(double dtSeconds, const std::string& amountText);
    double UpdateValue(double dtSeconds);   // Update 的内核（只推进显示值）
};


struct WidgetFrame {
    ConnState state = ConnState::ColdStart;
    bool showAmount = false;        // 数字该不该显示（无数据时显示占位符）
    std::string amountText;         // 已格式化的两位小数
    const wchar_t* currencySymbol = L"";   // 空串 = 币种未知，**不默认 ¥**
    const wchar_t* statusText = L"";       // 标题行/状态文案
    std::string zeroTimeText;       // 清零预估（C9 填；现在留占位）
    // 每一位当前的纵坐标（渲染层按它画数字）。空 = 渲染层退回整串绘制。
    std::vector<axis::PlaceCoord> places;
};

// 把状态 + 显示值组装成一帧。放在这里而不是渲染层，是为了让"显示什么"
// 和"怎么画"分开——渲染层不该知道余额为 0 和查不到有什么区别。
WidgetFrame BuildWidgetFrame(ConnState state, const DisplayedAmount& amount, bool currencyKnown,
                             const wchar_t* currencySymbol);

// 状态文案行（C6）。标题兼状态行：紧急状态除了颜色变化，文字也要跟着换，
// 否则只靠颜色编码状态——色觉障碍、屏幕反光、截图转述三种情况下都会失效。
const wchar_t* StatusTextFor(ConnState state);

}  // namespace dshb
