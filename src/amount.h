// 金额：一律用 1/10000 元的整数存储（设计 §3.1）
//
// 为什么不用 double：接口给的是字符串 "110.00"，而真实场景里会出现
// "10 分钟只花了 0.03 元"。double 在这个量级上的误差和信号同量级，
// 会让"余额变了吗"这个判断自己抖起来。
//
// 单位取 1/10000 是刻意留了两位余量：接口目前给两位小数，将来若给四位
// 也不用改存储。显示时才取到两位。

#pragma once

#include <cstdint>
#include <string>

namespace dshb {

// 以万分之一元为单位。10000 == 1.00 元
using AmountRaw = int64_t;
constexpr AmountRaw kUnitsPerYuan = 10000;

struct Amount {
    AmountRaw raw = 0;

    static Amount FromYuan(int64_t yuan) { return Amount{yuan * kUnitsPerYuan}; }

    // ★ 从"元"的浮点数构造，**保留到分**。
    //   FromYuan 只接受整元：调用方写 FromYuan((int64_t)x) 时小数会被截掉
    //   （实测踩过：--seq=100.50 被当成 100.00、30.66 被当成 30.00，
    //    而且因为"静止帧"和"滚完帧"都被同样截断，对比还是通过的——假通过）。
    static Amount FromYuanDouble(double yuan) {
        const double cents = (yuan < 0.0 ? -yuan : yuan) * 100.0 + 0.5;
        const AmountRaw c = static_cast<AmountRaw>(cents);
        const AmountRaw raw = c * 100;   // 1 分 = 100 raw
        return Amount{yuan < 0.0 ? -raw : raw};
    }

    double ToDouble() const { return static_cast<double>(raw) / kUnitsPerYuan; }

    // 保留两位小数的字符串（用于显示与日志）。不做本地化分隔符。
    std::string ToString2() const;
};

inline bool operator==(Amount a, Amount b) { return a.raw == b.raw; }
inline bool operator!=(Amount a, Amount b) { return a.raw != b.raw; }
inline bool operator<(Amount a, Amount b) { return a.raw < b.raw; }
inline bool operator>(Amount a, Amount b) { return a.raw > b.raw; }
inline Amount operator-(Amount a, Amount b) { return Amount{a.raw - b.raw}; }
inline Amount operator+(Amount a, Amount b) { return Amount{a.raw + b.raw}; }

// 解析接口返回的字符串。**不要**用 strtod 再转——那是浮点路径。
//
// 接受：可选正负号、可选千分位逗号、可选小数点、小数位任意（多余位按四舍五入
// 收进 1/10000；接口目前只给两位，但金额精度官方没有承诺，见设计 §2.1）。
// 拒绝：空串、只有符号、非法字符、超出范围。
//
// 返回 false 时 out 保持原值不动——调用方必须把它当成"这次读不到"，
// 而不是"余额是 0"。
bool ParseAmount(const std::string& text, Amount* out);

}  // namespace dshb
