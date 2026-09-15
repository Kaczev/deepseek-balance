// ===========================================================================
//  roll_axis.h —— 数字滚动的坐标系（**只算坐标，不涉及动画**）
// ===========================================================================
//
//  所有者给的定义，原文照抄：
//
//    · 「先将每一个数字为 0.00 时的纵坐标记为 0」—— 也就是原点定在
//      DisplayAmount == 0.00：此时**每一位**的纵实际坐标都是 0。
//    · 「任何一个位数，比如说 10 位，DisplayAmount / 10 就是这个位数的坐标；
//       如果是个位，DisplayAmount / 1 就是个位数的坐标。」
//
//  于是（下面一律叫 ActualY，中文叫「纵实际坐标」）：
//
//      ActualY(位次 p) = DisplayAmount / 10^p
//
//      位次 p：个位 = 0，十位 = 1，百位 = 2，千位 = 3 …
//              十分位 = -1，百分位 = -2 …
//
//  单位是「格」：坐标每 +1，那一位的带子正好走过一个数字。
//  所以小数部分 frac = ActualY - floor(ActualY) 就是"这一位停在两个数字之间的位置"，
//  整数部分 mod 10 就是"此刻这一位显示哪个数字"。两者合起来足以画出滚动。
//
//  ★ 一条必须知道的性质（不是毛病，是里程表本身的定义）：
//    位次越大，坐标走得越慢；位次越小越快。同一个金额变化 Δ 里，
//    百分位的坐标变化是十位的 1000 倍。这不是"谁卡住了"——每位的坐标都是
//    按自己的位权推进的，这正是里程表的样子。动画如何让它们"同时结束"
//    是后面做动画时的事，坐标系本身不做任何妥协。
//
//  ★ 为什么用整数金额而不是 double 去算：
//    金额在项目里是 Amount{int64 raw}，单位 1/10000 元（kUnitsPerYuan）。
//    用 raw 做整数除法就没有浮点漂移，例如 100.00 的百分位坐标精确是 10000，
//    而不是 9999.999999。只有要"停在两格之间的比例"时才转成 double。
//
//  这一版**不接任何渲染/动画**：只有下面这些纯函数 + tools/coordprobe.cpp
//  用来打印一张表核对。

#pragma once

#include "amount.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace dshb {
namespace axis {

// 不是数字字符的槽位（小数点）用这个值表示"没有位次"。
inline constexpr int kNoPlace = 99;

// 一个位次的当前纵坐标，交给渲染层用。
// coord 是**连续**的：静止时等于整数（读数清晰），滚动中落在两格之间（有滚动感）。
struct PlaceCoord {
    int place = kNoPlace;
    double coord = 0.0;
};


// 小数点在文本里的下标（"100.00" 是 3）。找不到时返回 -1。
inline int DotSlotOf(const std::string& amountText) {
    const size_t dot = amountText.find('.');
    return (dot == std::string::npos) ? -1 : static_cast<int>(dot);
}

// 文本第 slot 个字符属于哪个十进制位次。小数点返回 kNoPlace。
//
//   "100.00"：slot 0='1' -> 百位(2)，1='0' -> 十位(1)，2='0' -> 个位(0)，
//             3='.' -> kNoPlace，4='0' -> 十分位(-1)，5='0' -> 百分位(-2)
inline int PlaceOfSlot(const std::string& amountText, int slot) {
    const int dot = DotSlotOf(amountText);
    if (dot < 0) return kNoPlace;          // 没有小数点就不猜
    if (slot < 0 || slot >= static_cast<int>(amountText.size())) return kNoPlace;
    if (slot == dot) return kNoPlace;
    if (amountText[slot] < '0' || amountText[slot] > '9') return kNoPlace;
    return (slot < dot) ? (dot - 1 - slot) : -(slot - dot);
}

// 该位次在 1 元里有多少"格"。个位 = 1，十位 = 10，十分位 = 0.1 …（仅用于打印说明）
inline double GridPerYuan(int place) { return std::pow(10.0, static_cast<double>(place)); }

// 纵实际坐标 = DisplayAmount / 10^place，单位「格」。
//
// 精确做法：raw 的单位是 1/10000 元，所以
//     ActualY = raw / (10^(place + 4))
// place >= -4 时分母是 10 的整数次幂；这里是整数除法的**商**（带小数），
// 用 double 只为表达"两格之间"的比例，整数部分是精确的。
inline double ActualY(AmountRaw raw, int place) {
    if (place == kNoPlace) return 0.0;
    const double denom = std::pow(10.0, static_cast<double>(place) + 4.0);
    return static_cast<double>(raw) / denom;
}

inline double ActualY(const Amount& value, int place) { return ActualY(value.raw, place); }

// 该位此刻显示的数字：floor(ActualY) mod 10。
// 余额为正时就是整数除法；写成 floor 是为了负数（欠费）也不跳。
inline int DigitAt(AmountRaw raw, int place) {
    if (place == kNoPlace) return 0;
    const double y = ActualY(raw, place);
    const double f = std::floor(y);
    int d = static_cast<int>(std::fmod(f, 10.0));
    if (d < 0) d += 10;
    return d;
}

inline int DigitAt(const Amount& value, int place) { return DigitAt(value.raw, place); }

// 该位停在两格之间的比例，0 = 正好落在某个数字上，0.5 = 正好在中间。
inline double FracAt(AmountRaw raw, int place) {
    if (place == kNoPlace) return 0.0;
    const double y = ActualY(raw, place);
    return y - std::floor(y);
}

inline double FracAt(const Amount& value, int place) { return FracAt(value.raw, place); }

// ---------------------------------------------------------------------------
// 一位的完整状态：静止位置上是哪个数字、还差多少、要一起画的下一位是谁
// ---------------------------------------------------------------------------

struct PlaceState {
    int place = kNoPlace;
    double actualY = 0.0;   // 纵实际坐标 = S / 10^place（连续）
    int base = 0;           // B = floor(actualY)：落在参考点上的那位数字的序号
    double frac = 0.0;      // actualY − B：停在两格之间的比例
    int shown = 0;          // B mod 10：此刻该位显示的数字
    int next = 0;           // (B+1) mod 10：从下方补上来的那一位
    bool settled = false;   // frac == 0：正落在整数上，只需画一个数字
};

inline PlaceState StateAt(const Amount& value, int place) {
    PlaceState st;
    st.place = place;
    if (place == kNoPlace) return st;
    st.actualY = ActualY(value, place);
    // floor 而不是截断：余额为负（欠费）时也不会跳。
    st.base = static_cast<int>(std::floor(st.actualY));
    st.frac = st.actualY - static_cast<double>(st.base);
    st.shown = static_cast<int>(std::fmod(static_cast<double>(st.base), 10.0));
    if (st.shown < 0) st.shown += 10;
    st.next = (st.shown + 1) % 10;
    st.settled = (st.frac == 0.0);
    return st;
}

// 数字 digit 相对「静止位置」的偏移（单位与 h 相同，y 向下为正）。
//
//   offset(d) = ((d − B) mod 10) × h + frac × h
//
//   · d = B   ->  +frac × h        ：正在走的那位，往下走（所有者给的偏移：99.50 的个位 +0.50h）
//   · d = B+1 ->  −(1−frac) × h    ：从**上面**补进来的那位（99.50 的个位 −0.50h）
//   · 其余数字的偏移至少 h，一行高的窗口里不可能出现，不必画。
inline double OffsetOf(const PlaceState& st, int digit, double h) {
    int k = (digit - st.base) % 10;
    if (k < 0) k += 10;
    return static_cast<double>(k) * h + st.frac * h;
}

// ★ 所有者最初的写法：((B + d) mod 10) × h。**保留在这里只为了能把它渲染出来对照**，
//   它不是正确公式：它把数字 d 钉在"由 B 决定的那一行"上，与"这一位此刻该显示谁"无关。
//   反例（已渲染出来）：100.00 的百位 B=1 -> 落在窗口里的数字是 9，画面显示 900.00；
//   7.03 的个位 B=7 -> 窗口里是 3。正确写法见 OffsetOf。
inline double OffsetOfUserFormula(const PlaceState& st, int digit, double h) {
    int k = (st.base + digit) % 10;
    if (k < 0) k += 10;
    return static_cast<double>(k) * h;
}

// 静止口径：坐标取 S/n 的**整数部分**，frac 恒为 0。
//
// 为什么必须有这一条（已渲染确认）：直接用 S/n 当静止坐标时，只有"金额是 10 的整数倍"
// 那种情况下每一位才同时落在整数上。99.50 的十位 S/n = 9.95 -> frac 0.95，于是十位
// 几乎整格滚到了 0，屏幕上显示成 "09.00" 而不是 "99.50"（实测图）。
// 取整之后：99.50 的十位 floor(9.95)=9 -> frac 0 -> 正好显示 9。读数清晰。
//
// 滚动时不用这一条：那时坐标在两个整数之间连续走，frac 自然出现，两个数字一起画。
inline PlaceState StateAtRest(const Amount& value, int place) {
    PlaceState st = StateAt(value, place);
    st.actualY = std::floor(st.actualY);
    st.frac = 0.0;
    st.settled = true;
    return st;
}

}  // namespace axis
}  // namespace dshb
