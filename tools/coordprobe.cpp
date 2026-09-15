// coordprobe —— 打印纵实际坐标表，用来核对 roll_axis.h 的定义。
//
// 只做一件事：给一个金额，把它每一个数字位的 ActualY / 数字 / 小数部分打出来。
// 不画任何东西、不接动画——这样"坐标系对不对"可以脱离渲染单独核对。
//
//   coordprobe 100.00
//   coordprobe 99.50 7.03 0.00

#include "../src/amount.h"
#include "../src/roll_axis.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void DumpOne(const std::string& text, double h) {
    dshb::Amount a{};
    if (!dshb::ParseAmount(text.c_str(), &a)) {
        std::printf("  %s  <- 解析失败，跳过\n", text.c_str());
        return;
    }
    const std::string body = a.ToString2();
    std::printf("\n金额 %s  (raw=%lld, 单位 1/10000 元)\n", body.c_str(),
                static_cast<long long>(a.raw));
    std::printf("  槽位  字符  位次    纵实际坐标(格)      显示数字  两格之间\n");
    for (int slot = 0; slot < static_cast<int>(body.size()); ++slot) {
        const int place = dshb::axis::PlaceOfSlot(body, slot);
        if (place == dshb::axis::kNoPlace) {
            std::printf("   %2d    %c    --      （小数点，不参与滚动）\n", slot, body[slot]);
            continue;
        }
        const double y = dshb::axis::ActualY(a, place);
        const int d = dshb::axis::DigitAt(a, place);
        const double f = dshb::axis::FracAt(a, place);
        std::printf("   %2d    %c   %+3d    %16.4f          %d      %.4f\n", slot, body[slot], place,
                    y, d, f);
    }

    // ---- 按"h × 纵实际坐标"换算成屏幕位置：这一位实际要画哪些数字、画在哪 ----
    //
    // 规则（所有者给的）：数字 d 的参考点 O 画在 y(d) = O_y + (d - 纵实际坐标) × h。
    // 所以"正好落在 O 上"的那一位是 floor(坐标)，另一个要一起画的是它上面那一个
    // （坐标往正方向走 = 数字往上移，所以上面那个是 floor+1）。
    std::printf("  按 h=%.4f 换算：每个数字的参考点 O 相对 O_y 的偏移（负 = 在 O 上方）\n", h);
    for (int slot = 0; slot < static_cast<int>(body.size()); ++slot) {
        const int place = dshb::axis::PlaceOfSlot(body, slot);
        if (place == dshb::axis::kNoPlace) continue;
        const double y = dshb::axis::ActualY(a, place);
        const double f = dshb::axis::FracAt(a, place);
        const int lo = dshb::axis::DigitAt(a, place);          // floor(坐标) mod 10
        const int hi = (lo + 1) % 10;
        std::printf("    位次 %+3d : 数字 %d 画在 %+7.2f px ；数字 %d 画在 %+7.2f px%s\n", place,
                    lo, -f * h, hi, (1.0 - f) * h,
                    (f == 0.0) ? "   <- 正好落在整数上，只需画这一个" : "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    // h 从字体实测得来：--layout-probe 打出的 line advance。
    // 当前字体（数字）下是 52.0708 DIP；换字号要重新量，不要沿用。
    const double h = 52.0708;
    std::printf("纵实际坐标 = DisplayAmount / 10^位次 ；0.00 时每一位都是 0\n");
    if (argc < 2) {
        std::printf("\n用法: coordprobe <金额> [更多金额...]\n");
        for (const char* s : {"0.00", "7.03", "99.50", "100.00", "1234.56"}) DumpOne(s, h);
        return 0;
    }
    for (int i = 1; i < argc; ++i) DumpOne(argv[i], h);
    return 0;
}
