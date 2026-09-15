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

void DumpOne(const std::string& text) {
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
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("纵实际坐标 = DisplayAmount / 10^位次 ；0.00 时每一位都是 0\n");
    if (argc < 2) {
        std::printf("\n用法: coordprobe <金额> [更多金额...]\n");
        for (const char* s : {"0.00", "7.03", "99.50", "100.00", "1234.56"}) DumpOne(s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) DumpOne(argv[i]);
    return 0;
}
