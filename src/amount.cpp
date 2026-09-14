#include "amount.h"

#include <cmath>
#include <cstdio>

namespace dshb {

std::string Amount::ToString2() const {
    // 手工取整，避开 printf 的浮点路径
    const bool negative = raw < 0;
    const AmountRaw magnitude = negative ? -raw : raw;

    // 1/10000 -> 1/100：四舍五入到"分"
    const AmountRaw cents = (magnitude + 50) / 100;
    const AmountRaw yuan = cents / 100;
    const AmountRaw frac = cents % 100;

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%lld.%02lld", negative ? "-" : "",
                  static_cast<long long>(yuan), static_cast<long long>(frac));
    return buf;
}

bool ParseAmount(const std::string& text, Amount* out) {
    if (!out) return false;

    // 去掉首尾空白（接口一般不会给，但防御一次很便宜）
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    if (begin == end) return false;

    size_t i = begin;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = (text[i] == '-');
        ++i;
    }
    if (i == end) return false;   // 只有符号

    // MSVC 没有 __int128，所以用"先判上限再乘"的方式防溢出。
    // 上限取 1e17：远小于 INT64_MAX（约 9.2e18），但比任何真实余额都大得多。
    constexpr int64_t kLimit = 100000000000000000LL;

    int64_t units = 0;
    int fractionDigits = 0;
    bool seenDigit = false;
    bool seenDot = false;

    for (; i < end; ++i) {
        const char c = text[i];
        if (c == ',') continue;        // 千分位
        if (c == '.') {
            if (seenDot) return false; // 第二个小数点
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        seenDigit = true;

        if (!seenDot) {
            if (units > kLimit) return false;
            units = units * 10 + (c - '0');
            if (units > kLimit) return false;
        } else if (fractionDigits < 4) {
            // 收进 1/10000 的位
            units = units * 10 + (c - '0');
            ++fractionDigits;
        } else {
            // 第 5 位及以后：四舍五入（只看紧跟的那一位，然后忽略其余）
            if (c >= '5') {
                ++units;               // 1 个最小单位 = 1/10000 元
                if (units > kLimit) return false;
            }
            // 剩下的位数不再参与数值，但上面的字符校验已经保证了它们都是数字
        }
    }
    if (!seenDigit) return false;

    // 补齐到 4 位小数
    for (int k = fractionDigits; k < 4; ++k) {
        if (units > kLimit) return false;
        units *= 10;
    }

    if (negative) units = -units;
    out->raw = units;
    return true;
}

}  // namespace dshb
