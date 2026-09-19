// moneyprobe -- 危险度 D 与曲线颜色必须按"钱"算：折成元之后再比 10 元阈值。
//
//   moneyprobe            跑全部用例（默认）
//   moneyprobe --keep     留下临时文件并打印路径
//
// 它守的是一个**单位**错误，而单位错误只有把两个币种放进同一条链里才看得见。
// 所有者逐字报的："那个 10 元就算余额危急，D 开始 >1，换成美元之后就变成 10 美元了。"
// 修的是两处输入（D 与曲线点的颜色），它们以前吃的是"屏幕上那个币种的数"：
// 显示 USD 时 "$2.81" 被当成 "¥2.81" 去比 10 元 —— 同一笔钱，两个结论。
//
// 为什么它必须是一个单独的探针：
//   * 这两处输入都在显示层（widget_display.cpp），而 fxprobe 与 storeprobe 都不链接它；
//   * 曲线点的颜色**只能从文件里读回来**（FeedCurve 只在追加时落盘），所以这个探针走的是
//     真实那条路：Sample -> DisplayedAmount::OnSample -> FeedCurve -> curve.json；
//   * 每一条都要能失败：m8 把"旧口径会给什么颜色"一起印出来，读数的人一眼就能看出
//     这条用例不是恒真的。
//
// ★ 硬约束：所有写出来的文件都在 %TEMP%\dshb-moneyprobe-<pid>-<场景>.json，
//   并且每个场景用一个**新的**文件名（SetCurveStorePath 会顺手 Load，新名字 = 空存储 =
//   场景之间互相隔离）。所有者真实的 curve.json / config.json / fx.json 一个字节都不会被碰。
#include "widget_display.h"

#include "amount.h"
#include "curve_store.h"
#include "fx_rate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using dshb::Amount;
using dshb::CurrencyAmount;
using dshb::DisplayedAmount;
using dshb::ParseAmount;
using dshb::Sample;

namespace {

// ===========================================================================
// 和 storeprobe / fxprobe / panelprobe 同一个极小的夹具：一行一条，只有全过才退 0。
// ===========================================================================
struct Harness {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    bool Req(const char* id, const std::string& what, const std::string& evidence, bool ok) {
        std::string line = std::string(ok ? "PASS: " : "FAIL: ") + id + " | " + what;
        if (!evidence.empty()) line += " | " + evidence;
        std::printf("%s\n", line.c_str());
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(line);
        }
        return ok;
    }
};

// 逐位比较要用的写法：%.17g 是 double 能往返的最短十进制位数，
// 于是"切换前后 D 是不是同一个数"在输出里就是逐字可判的。
std::string Bits(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

std::string F2(double value) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ", ";
        out += parts[i];
    }
    return out.empty() ? std::string("<none>") : out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::string TempPath(const char* name) {
    wchar_t dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, dir) == 0) return std::string();
    const std::wstring path = std::wstring(dir) + L"dshb-moneyprobe-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" + Wide(name) + L".json";
    return WideToUtf8(path);
}

bool ReadFile(const std::string& path, std::string* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    out->clear();
    char buf[4096];
    std::size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, got);
    fclose(f);
    return true;
}

// JSON 里按顺序出现的每一个 "color": "#xxxxxx"。
// 写成"扫描已知的那一个键"而不是引一个 JSON 解析器：这个文件是本程序自己写的，
// 而这里要判的是**颜色的个数、顺序与取值**，不是通用的 JSON 形状（形状由 storeprobe 管）。
std::vector<std::string> ColorsIn(const std::string& json) {
    std::vector<std::string> out;
    const std::string key = "\"color\": \"";
    std::size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        const std::size_t begin = pos + key.size();
        const std::size_t end = json.find('"', begin);
        if (end == std::string::npos) break;
        out.push_back(json.substr(begin, end - begin));
        pos = end + 1;
    }
    return out;
}

// 文件里第一个点的原文（给读数的人看：这个点带着哪些币种、什么颜色）。
std::string FirstPoint(const std::string& json) {
    const std::size_t array = json.find("\"points\": [");
    if (array == std::string::npos) return std::string();
    const std::size_t begin = json.find('{', array);
    if (begin == std::string::npos) return std::string();
    const std::size_t end = json.find('}', begin);
    if (end == std::string::npos) return std::string();
    return json.substr(begin, end - begin + 1);
}

// ---- 造样本 -----------------------------------------------------------------
// ★ 手写而不是走 MakeSample：本探针要的是"**某一个形状**的样本喂进显示层"，
//   而 MakeSample 在后台线程那一侧（balance_source.cpp），它要 api::BalanceResult。
//   样本里的字段与 MakeSample 的产物逐字段同形：total/currency/entries + 汇率两件。
CurrencyAmount Entry(const std::string& currency, const std::string& text) {
    CurrencyAmount e{};
    e.currency = currency;
    e.ok = ParseAmount(text, &e.total);
    return e;
}

// 大陆账号：主币种 CNY，条目顺序 = [CNY, USD]（USD 是补出来的那一条，追加在末尾）。
Sample MainlandSample(const std::string& cnyText, const std::string& usdText, int64_t atSec,
                      const std::string& usdToCny = "6.6976", bool rateOk = true) {
    Sample s{};
    s.wallMs = atSec * 1000;
    s.transportOk = true;
    s.httpStatus = 200;
    s.isAvailable = true;
    s.amountsOk = true;
    s.currency = "CNY";
    s.entries.push_back(Entry("CNY", cnyText));
    s.total = s.entries.front().total;
    if (!usdText.empty()) s.entries.push_back(Entry("USD", usdText));
    s.usdToCnyOk = rateOk;
    s.usdToCnyText = rateOk ? usdToCny : std::string();
    return s;
}

// 海外账号：接口只给 USD，**没有** CNY 条目（所以"补 USD 条目"那条路根本不参与）。
Sample OverseasSample(const std::string& usdText, int64_t atSec, const std::string& usdToCny,
                      bool rateOk) {
    Sample s{};
    s.wallMs = atSec * 1000;
    s.transportOk = true;
    s.httpStatus = 200;
    s.isAvailable = true;
    s.amountsOk = true;
    s.currency = "USD";
    s.entries.push_back(Entry("USD", usdText));
    s.total = s.entries.front().total;
    s.usdToCnyOk = rateOk;
    s.usdToCnyText = rateOk ? usdToCny : std::string();
    return s;
}

// 跑 240 帧真实的显示层推进（1/60 秒一帧，共 4 秒）。为什么要跑：
// 切换币种之后屏幕上的数字要**滚**过去（行程约 2 秒），而"切换后的危险度"只有在滚完
// 之后才是稳定读数 —— 旧实现（D 吃屏幕上的数）正是在滚完之后才饱和到 1 的。
// 只推进一帧会让两边都读到"还停在旧币种上"的数，那样这一条既通不过也失败不了。
void Settle(DisplayedAmount* display) {
    for (int i = 0; i < 240; ++i) display->Update(1.0 / 60.0);
}

// 一个色卡场景 = 一串样本喂进一个干净存储，然后把落盘的颜色读回来。
// `displays[i]` 非空 = 喂第 i 个样本**之前**把显示币种切成它（颜色是追加那个点的那一刻
// 回填的，所以要问的正是"那一刻屏幕上显示的是哪个币种"）。
struct CurveRun {
    std::vector<std::string> colors;
    std::string firstPoint;
    bool ok = false;
};

CurveRun RunCurve(const char* name, const std::vector<Sample>& samples,
                  const std::vector<std::string>& displays) {
    CurveRun run;
    const std::string path = TempPath(name);
    if (path.empty()) return run;
    DeleteFileA(path.c_str());
    dshb::SetCurveStorePath(Wide(path));   // 新名字 -> Load 找不到它 -> 存储被清空

    DisplayedAmount display;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i < displays.size() && !displays[i].empty()) display.SelectCurrency(displays[i]);
        display.OnSample(samples[i]);
    }
    std::string raw;
    run.ok = ReadFile(path, &raw);
    if (run.ok) {
        run.colors = ColorsIn(raw);
        run.firstPoint = FirstPoint(raw);
    }
    return run;
}

// 探针自己算一遍"如果按旧口径（把美元数当成元）上色会是什么颜色"。
// ★ 它**不是**判据 —— 判据是两个场景的颜色逐字相同。它只是让读数的人一眼看出
//   "这条用例能失败"：旧口径给的是完全另一个颜色。
std::string HexOfColor(const dshb::AmbienceColor& c) {
    auto byte = [](float x) {
        const float v = (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
        return static_cast<int>(v * 255.0f + 0.5f);
    };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--keep") {
            keep = true;
        } else {
            std::printf("moneyprobe: unknown argument \"%s\"\n", arg.c_str());
            std::printf("usage: moneyprobe [--keep]\n");
            return 64;
        }
    }
    SetConsoleOutputCP(CP_UTF8);

    Harness h;
    std::printf("moneyprobe: 危险度 D 与曲线颜色按“钱”算（主币种折元 -> 比 10 元阈值）\n");
    std::printf("threshold G=%.4f 元 (kLowBalanceThresholdYuan)\n", dshb::kLowBalanceThresholdYuan);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    std::vector<std::string> written;

    // =====================================================================
    // m1：**切换币种不再改变危险度** —— 所有者报的那一笔钱（大陆账号 18.81 元）
    // =====================================================================
    // 旧实现：显示 USD 时 D = BalanceDepth($2.80) = 0.72；显示 CNY 时 D = 0。
    // 同一个账户、同一笔钱、同一个时刻 —— 两个结论，就是所有者看到的那件事。
    {
        DisplayedAmount display;
        display.OnSample(MainlandSample("18.81", "2.80", now));
        Settle(&display);
        const double cny = display.ambienceDepthShown();
        const std::string before = display.shownCurrency();

        display.SelectCurrency("USD");
        Settle(&display);
        const double usd = display.ambienceDepthShown();

        const double asCny = dshb::BalanceDepth(18.81);   // 正确口径（元）
        const double asUsd = dshb::BalanceDepth(2.80);    // 旧口径（屏幕上的数）
        const bool same = (cny == usd);
        h.Req("m1", "同一笔 18.81 元：切到 USD 显示前后 D 逐位相同",
              "显示 " + before + " -> " + display.shownCurrency() + " target=" +
                  F2(display.target()) + " text=" + Quote(display.TextToShow()) +
                  "; D(before)=" + Bits(cny) + " D(after)=" + Bits(usd) + " 逐位相同=" +
                  std::to_string(same ? 1 : 0) + "; 正确口径 BalanceDepth(18.81)=" + Bits(asCny) +
                  "，旧口径 BalanceDepth(2.80)=" + Bits(asUsd),
              same && cny == asCny && usd != asUsd && display.shownCurrency() == "USD");
    }

    // =====================================================================
    // m1b：同一件事在**低余额**上（那里 D 不是 0，所以新旧两个数差得更远）
    // =====================================================================
    // 9.00 元：正确 D = 0.1；旧口径按 $1.34 算 = 0.866。这一条是 m1 的加强版 ——
    // m1 的正确值是 0，可能被"两边都是 0"蒙过去（例如 D 恒为 0 的实现）。
    {
        DisplayedAmount display;
        display.OnSample(MainlandSample("9.00", "1.34", now));
        Settle(&display);
        const double cny = display.ambienceDepthShown();
        display.SelectCurrency("USD");
        Settle(&display);
        const double usd = display.ambienceDepthShown();
        h.Req("m1b", "同一笔 9.00 元：切到 USD 显示前后 D 逐位相同（且非 0，排除“恒为 0”）",
              "D(before)=" + Bits(cny) + " D(after)=" + Bits(usd) + " 期望 " +
                  Bits(dshb::BalanceDepth(9.00)) + "（旧口径按 $1.34 会给 " +
                  Bits(dshb::BalanceDepth(1.34)) + "）",
              cny == usd && cny == dshb::BalanceDepth(9.00) && cny > 0.0 &&
                  usd != dshb::BalanceDepth(1.34));
    }

    // =====================================================================
    // m2：**阈值仍然在 10 元**（边界各一侧）
    // =====================================================================
    {
        const double d10 = dshb::BalanceDepth(10.00);
        const double d999 = dshb::BalanceDepth(9.99);
        const double d1001 = dshb::BalanceDepth(10.01);
        const double d5 = dshb::BalanceDepth(5.00);
        const double d0 = dshb::BalanceDepth(0.00);
        h.Req("m2", "余额 10.00 / 9.99 / 10.01 / 5.00 / 0.00 元 -> D = 0 / 0.001 / 0 / 0.5 / 1",
              "D(10.00)=" + Bits(d10) + " D(9.99)=" + Bits(d999) + " D(10.01)=" + Bits(d1001) +
                  " D(5.00)=" + Bits(d5) + " D(0.00)=" + Bits(d0),
              d10 == 0.0 && std::fabs(d999 - 0.001) < 1e-12 && d1001 == 0.0 &&
                  std::fabs(d5 - 0.5) < 1e-12 && d0 == 1.0);
    }

    // =====================================================================
    // m3：**海外账号**（只有 USD、没有 CNY）—— 美元折元之后与"元余额 10 元"同一条线
    // =====================================================================
    // 1 USD = 6.6976 CNY，所以"10 元"这条线落在 10 / 6.6976 = 1.4930… 美元上：
    //   $1.49 -> 9.9794 元 -> D > 0（线下方）；$1.50 -> 10.0464 元 -> D = 0（线上方）。
    // 判据不止"大于/等于 0"，还有更硬的一条：**同一笔钱的 D 逐位相同** ——
    //   $1.49 的海外样本与 ¥9.9794 的大陆样本必须给出同一个 D。
    {
        DisplayedAmount belowSide;
        belowSide.OnSample(OverseasSample("1.49", now, "6.6976", true));
        Settle(&belowSide);
        const double below = belowSide.ambienceDepthShown();

        DisplayedAmount aboveSide;
        aboveSide.OnSample(OverseasSample("1.50", now, "6.6976", true));
        Settle(&aboveSide);
        const double above = aboveSide.ambienceDepthShown();

        DisplayedAmount mainland;
        mainland.OnSample(MainlandSample("9.9794", "1.49", now));
        Settle(&mainland);
        const double sameMoney = mainland.ambienceDepthShown();

        Amount yuan{};
        const bool converted =
            dshb::fx::UsdAmountToYuan(Entry("USD", "1.49").total, "6.6976", &yuan);
        h.Req("m3", "海外账号 $1.49（=9.9794 元）-> D>0、$1.50（=10.0464 元）-> D=0，"
                    "且与同一笔钱的大陆样本逐位相同",
              "USD 1.49 -> 元 raw=" + std::to_string(yuan.raw) + " (" + yuan.ToString2() +
                  ") D=" + Bits(below) + "; USD 1.50 -> D=" + Bits(above) +
                  "; 大陆 9.9794 元 -> D=" + Bits(sameMoney) + "; 期望 BalanceDepth(9.9794)=" +
                  Bits(dshb::BalanceDepth(9.9794)),
              converted && below > 0.0 && above == 0.0 && below == sameMoney &&
                  below == dshb::BalanceDepth(9.9794));
    }

    // =====================================================================
    // m4：**没有汇率**（海外账号 + 取不到汇）—— 不崩、D 取 0、日志写明后果
    // =====================================================================
    // 选 0 的理由写在 widget_display.cpp 的 AdvanceAmbience 第 3 步：D 唯一的作用是
    // "宣告低余额"，而我们没有任何依据说这个账户余额低 —— 凭"我们没有汇率"点亮危险信号
    // 会让真正的危险信号贬值。它也不是"安全"的结论：日志里那一行说清了这笔钱没有来源。
    {
        DisplayedAmount display;
        display.OnSample(OverseasSample("1.49", now, "", false));
        Settle(&display);
        const double d = display.ambienceDepthShown();

        double yuan = -1.0;
        const bool converted =
            dshb::AmountInYuan("USD", Entry("USD", "1.49").total, std::string(), false, &yuan);

        dshb::fx::Rate none;
        none.error = "请求没到（WinHttpConnect failed, error=10061）；缓存也不可用：读不到缓存文件";
        const std::string line = none.LogLine();
        h.Req("m4", "海外账号 + 没有汇率：D = 0（不宣告低余额），且日志写明这个后果",
              "D=" + Bits(d) + " AmountInYuan 折成了吗=" + std::to_string(converted ? 1 : 0) +
                  " 日志=" + line,
              d == 0.0 && !converted && line.find("危险度 D 取 0") != std::string::npos);
    }

    // =====================================================================
    // m5：USD -> 元 这个方向本身（定点，不留二进制浮点）
    // =====================================================================
    {
        struct Case {
            const char* usd;
            const char* rate;
            long long wantRaw;   // 1/10000 元
        };
        const Case cases[] = {
            {"1.49", "6.6976", 99794},      // = 9.9794 元（线上的下一侧）
            {"1.50", "6.6976", 100464},     // = 10.0464 元（线的上一侧）
            {"0.00", "6.6976", 0},          // 0 美元就是 0 元
            {"100.00", "1.0000", 1000000},  // 汇率恰好 1：两边相等
            {"19.76", "6.6976", 1323445},   // 19.76*6.6976 = 132.344576 -> 截到 1/10000 元
        };
        bool allOk = true;
        std::string detail;
        for (const Case& c : cases) {
            Amount got{};
            const bool ok = dshb::fx::UsdAmountToYuan(Entry("USD", c.usd).total, c.rate, &got);
            char line[224];
            std::snprintf(line, sizeof(line), " %s*%s -> raw=%lld (want %lld)", c.usd, c.rate,
                          static_cast<long long>(got.raw), c.wantRaw);
            detail += line;
            if (!ok || got.raw != c.wantRaw) allOk = false;
        }
        h.Req("m5", "美元折元是定点乘法：5 个值逐位吻合",
              std::string(allOk ? "5/5 吻合;" : "有偏差;") + detail, allOk);

        Amount got{};
        const bool zeroRate = dshb::fx::UsdAmountToYuan(Entry("USD", "1.49").total, "0", &got);
        const bool junkRate = dshb::fx::UsdAmountToYuan(Entry("USD", "1.49").total, "abc", &got);
        const bool negative = dshb::fx::UsdAmountToYuan(Entry("USD", "1.49").total, "-6.6976", &got);
        h.Req("m5b", "读不出来的汇率既不是 0 也不是猜：\"0\" / 非数 / 负数一律拒绝",
              "rate=\"0\" ok=" + std::to_string(zeroRate) + " rate=\"abc\" ok=" +
                  std::to_string(junkRate) + " rate=\"-6.6976\" ok=" + std::to_string(negative),
              !zeroRate && !junkRate && !negative);
    }

    // =====================================================================
    // m6：**一个换算口**的口径 —— CNY 恒等（不需要汇率），别的币种不猜
    // =====================================================================
    {
        double yuan = -1.0;
        const bool cny =
            dshb::AmountInYuan("CNY", Entry("CNY", "18.81").total, std::string(), false, &yuan);
        const double cnyValue = yuan;
        yuan = -1.0;
        const bool usdNoRate =
            dshb::AmountInYuan("USD", Entry("USD", "2.80").total, std::string(), false, &yuan);
        yuan = -1.0;
        const bool other =
            dshb::AmountInYuan("EUR", Entry("EUR", "5.00").total, "6.6976", true, &yuan);
        yuan = -1.0;
        const bool usd = dshb::AmountInYuan("USD", Entry("USD", "2.80").total, "6.6976", true, &yuan);
        const double usdValue = yuan;
        h.Req("m6", "CNY 折元是恒等且**不看汇率**；USD 没有汇率就折不成；其他币种不猜",
              "CNY 18.81（无汇率）-> " + Bits(cnyValue) + " ok=" + std::to_string(cny ? 1 : 0) +
                  "; USD 2.80（无汇率）ok=" + std::to_string(usdNoRate ? 1 : 0) +
                  "; USD 2.80 x 6.6976 -> " + Bits(usdValue) + " (raw 187532); EUR ok=" +
                  std::to_string(other ? 1 : 0),
              cny && cnyValue == 18.81 && !usdNoRate && !other && usd &&
                  std::fabs(usdValue - 18.7532) < 1e-9);
    }

    // =====================================================================
    // m7：曲线颜色 —— **显示 USD 时回填的颜色 == 显示 CNY 时回填的颜色**（逐字）
    // =====================================================================
    // 两个点的主币种余额是 20.00 元与 5.00 元（所有者指定的那两个数），再加第三个点
    // （3.00 元）让第二个点也被回填一次 —— 于是 20 元（D=0）与 5 元（D=0.5）两种余额
    // 都被覆盖到。
    {
        const std::vector<Sample> samples = {MainlandSample("20.00", "2.99", now - 20),
                                             MainlandSample("5.00", "0.75", now - 10),
                                             MainlandSample("3.00", "0.45", now)};
        const CurveRun cny = RunCurve("m7-cny", samples, {"CNY", "CNY", "CNY"});
        const CurveRun usd = RunCurve("m7-usd", samples, {"CNY", "USD", "USD"});
        written.push_back(TempPath("m7-cny"));
        written.push_back(TempPath("m7-usd"));
        const bool ok = cny.ok && usd.ok && cny.colors.size() == 2 &&
                        cny.colors.size() == usd.colors.size() && cny.colors == usd.colors;
        h.Req("m7", "同一串点（20 元 / 5 元 / 3 元）：显示 CNY 与显示 USD 回填出的颜色逐字相同",
              "显示 CNY -> [" + Join(cny.colors) + "]，显示 USD -> [" + Join(usd.colors) +
                  "]，颜色个数=" + std::to_string(cny.colors.size()) +
                  "（第三个点还没有颜色：它要等下一个点到达才回填）；文件里第一个点=" +
                  cny.firstPoint,
              ok);
    }

    // =====================================================================
    // m8：曲线颜色的锚是**主币种折元**，不是"第一个条目那个数"
    // =====================================================================
    // 海外账号 $1.00（=6.6976 元）与大陆账号 ¥6.6976 是**同一笔钱**，两个场景的步长都
    // 足够陡（R 都饱和到 1），所以回填出来的颜色必须逐字相同。
    // ★ 这一条能失败：旧口径（把第一个可用条目当元）在海外场景里会用 $1.00 去算 D
    //   -> D = 0.9，颜色是另一个（下面把它印出来）。
    {
        const std::vector<Sample> mainland = {MainlandSample("6.6976", "1.00", now - 10),
                                              MainlandSample("1.3395", "0.20", now)};
        const std::vector<Sample> overseas = {OverseasSample("1.00", now - 10, "6.6976", true),
                                              OverseasSample("0.20", now, "6.6976", true)};
        const CurveRun a = RunCurve("m8-cny", mainland, {"CNY", "CNY"});
        const CurveRun b = RunCurve("m8-usd", overseas, {"USD", "USD"});
        written.push_back(TempPath("m8-cny"));
        written.push_back(TempPath("m8-usd"));

        const std::string oldRule =
            HexOfColor(dshb::AmbienceTargetColor(1.0, dshb::BalanceDepth(1.00)));
        const std::string want =
            HexOfColor(dshb::AmbienceTargetColor(1.0, dshb::BalanceDepth(6.6976)));
        const bool ok = a.ok && b.ok && !a.colors.empty() && a.colors == b.colors &&
                        a.colors.front() == want && a.colors.front() != oldRule;
        h.Req("m8", "海外 $1.00 与大陆 ¥6.6976（同一笔钱）回填出同一个颜色；"
                    "旧口径（把 $1.00 当成元）给的是另一个",
              "大陆 [" + Join(a.colors) + "] 海外 [" + Join(b.colors) + "] 期望 " + want +
                  "（D=" + Bits(dshb::BalanceDepth(6.6976)) + "），旧口径会给 " + oldRule +
                  "（D=" + Bits(dshb::BalanceDepth(1.00)) + "）",
              ok);
    }

    if (!keep) {
        for (const std::string& path : written) DeleteFileA(path.c_str());
    } else {
        for (const std::string& path : written) std::printf("kept: %s\n", path.c_str());
    }
    std::printf("moneyprobe: %d passed, %d failed\n", h.passed, h.failed);
    for (const std::string& line : h.failures) std::printf("  %s\n", line.c_str());
    std::printf("RESULT: %s\n", h.failed == 0 ? "all properties hold" : "FAILED");
    return h.failed == 0 ? 0 : 1;
}
