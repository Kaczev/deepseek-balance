#include "wheel.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dshb {

bool g_wheelTrace = false;
std::string g_wheelTraceDir;

namespace {

void Trace(const char* fmt, ...) {
    if (!g_wheelTrace) return;
    const std::string path = g_wheelTraceDir.empty()
                                 ? std::string("wheel-trace.log")
                                 : (g_wheelTraceDir + "\\wheel-trace.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// 找出小数点在文本里的位置；没有小数点则返回 text.size()。
size_t DotPos(const std::string& text) {
    const size_t d = text.find('.');
    return (d == std::string::npos) ? text.size() : d;
}

// 小数点之前的数字字符总数
int IntegerDigits(const std::string& text) {
    const size_t dot = DotPos(text);
    int n = 0;
    for (size_t i = 0; i < dot; ++i) {
        if (text[i] >= '0' && text[i] <= '9') ++n;
    }
    return n;
}

// 某一位的位权。slot 必须指向数字字符。
//   小数点前的第 p 个（从 0 数）-> 10^(整数位总数 - 1 - p)
//   小数点后的第 q 个（从 0 数）-> 10^-(q + 1)
bool WeightAt(const std::string& text, int slot, double* out) {
    if (slot < 0 || slot >= static_cast<int>(text.size())) return false;
    const char c = text[static_cast<size_t>(slot)];
    if (c < '0' || c > '9') return false;

    const size_t dot = DotPos(text);
    if (static_cast<size_t>(slot) < dot) {
        // 小数点前：数它前面还有几个数字字符，得到它是第几个
        int p = 0;
        for (int i = 0; i < slot; ++i) {
            if (text[static_cast<size_t>(i)] >= '0' && text[static_cast<size_t>(i)] <= '9') ++p;
        }
        const int exponent = IntegerDigits(text) - 1 - p;
        *out = std::pow(10.0, static_cast<double>(exponent));
    } else {
        // 小数点后：数它前面（小数点之后）还有几个数字字符
        int q = 0;
        for (size_t i = dot + 1; i < static_cast<size_t>(slot); ++i) {
            if (text[i] >= '0' && text[i] <= '9') ++q;
        }
        *out = std::pow(10.0, -static_cast<double>(q + 1));
    }
    return true;
}

}  // namespace

bool WheelValueAt(const std::string& text, int slot, double amount, double* out) {
    double weight = 0.0;
    if (!WeightAt(text, slot, &weight) || weight == 0.0) return false;
    if (out) *out = amount / weight;
    return true;
}

std::vector<WheelDraw> ComputeWheel(const std::string& text, double amount) {
    std::vector<WheelDraw> out;
    if (text.empty()) return out;

    if (g_wheelTrace) {
        Trace("[wheel] text=%s amount=%.6f\n", text.c_str(), amount);
    }

    const int n = static_cast<int>(text.size());
    for (int i = 0; i < n; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') continue;   // 小数点、逗号等不参与滚动

        double weight = 0.0;
        if (!WeightAt(text, i, &weight) || weight == 0.0) continue;
        const double wheelValue = amount / weight;
        const double base = std::floor(wheelValue);

        if (g_wheelTrace) {
            Trace("  [slot=%d weight=%.4f wheelValue=%.4f base=%.0f]\n", i, weight, wheelValue,
                  base);
        }

        // 窗口只露一行，所以最多两个数字有可能落在里面
        for (int k = 0; k <= 1; ++k) {
            const long long raw = static_cast<long long>(base) + k;
            WheelDraw d{};
            d.slot = i;
            d.row = k;                                            // 0 = 落位那一行
            d.digit = static_cast<int>(((raw % 10) + 10) % 10);   // 9 -> 0 要能绕回
            out.push_back(d);
        }
    }
    return out;
}

}  // namespace dshb
