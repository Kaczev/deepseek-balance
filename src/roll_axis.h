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

}  // namespace axis
}  // namespace dshb
