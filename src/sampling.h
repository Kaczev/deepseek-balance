// 采样记录与模拟数据源（B1 / B3）
//
// 模拟数据源存在的理由：真实的余额只在有人用 API 时才动，几小时不动是常态。
// 要验"快速消耗时颜色变红、心跳加快"，靠等真实数据是不现实的——所以要有假数据。

#pragma once

#include "amount.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dshb {

// ---------------------------------------------------------------------------
// 一条采样
// ---------------------------------------------------------------------------
// 一个币种条目（响应的 balance_infos 里的一项）。
// ★ 切换币种要用到**全部**条目，所以 Sample 不能只带"被选中的那一条"。
struct CurrencyAmount {
    std::string currency;
    Amount total{};
    bool ok = false;
};

struct Sample {
    // 墙钟（Unix 毫秒）。用于显示和落盘。
    int64_t wallMs = 0;

    // 单调时钟（毫秒，不计休眠）+ 本次开机标识。
    // ★ bootId 目前**只被写入、没有任何读方**（sampling.cpp 与 balance_source.cpp 各写一处）：
    //   设计 §5.4 的 H4 要用它把"QPC 计数跨进程重启后不可比"和"墙钟被改"分开，而那条
    //   断层判据在树里**没有实现**，所以注释不能再宣称这里有一个"缺一不可"的不变量——
    //   实际发生的是**没有读方去强制它**。monotonicMs 除了算自己这个 bootId，
    //   也没有别的读方。是否保留这个字段由所有者单独决定。
    int64_t monotonicMs = 0;
    int64_t bootId = 0;

    // 金额。ok = false 表示这次没解析出来（**不是 0**，这个区分是设计里第一个要防的错）
    Amount total{};
    Amount granted{};
    Amount toppedUp{};
    bool amountsOk = false;

    std::string currency;        // "CNY" / "USD" / "" 未知
    // 响应里的全部币种条目（切换币种用）。空 = 只有 currency/total 这一条。
    std::vector<CurrencyAmount> entries;

    bool isAvailable = false;
    int httpStatus = 0;          // 0 = 还没发出去（本地错误）
    bool transportOk = false;    // 请求本身是否成功到达并拿到响应
    std::string note;            // 错误说明/日志用

    // 币种符号。未知币种返回空串——**绝不能默认成 ¥**（USD 账户上显示 ¥ 是
    // "数值正确、单位错误"，最危险的一类错）
    const char* CurrencySymbol() const {
        if (currency == "CNY") return "\xC2\xA5";   // UTF-8 的 ¥
        if (currency == "USD") return "$";
        return "";
    }

    // 同上，宽字符版本（界面用宽字符，省一次编码转换）
    const wchar_t* CurrencySymbolW() const {
        if (currency == "CNY") return L"\u00A5";
        if (currency == "USD") return L"$";
        return L"";
    }

    bool CurrencyKnown() const { return currency == "CNY" || currency == "USD"; }
};

// ---------------------------------------------------------------------------
// 模拟情形（B6：九个状态 + 一个复位）
// ---------------------------------------------------------------------------
enum class Scenario {
    Steady = 0,     // 平稳消耗：慢速下降
    FastDrain,      // 快速消耗：曲线抖、颜色往上冲
    LowBalance,     // 余额偏低：从高处一路降到低余额
    Recharge,       // 充值跳变：先降，再做一次正跳变
    Zero,           // 余额归零
    Unavailable,    // is_available=false（查得到，但账户不可用）
    NoNetwork,      // 连不上
    StaleData,      // 旧数据：先把样本往前推 8 小时，再连续失败
    ClockJump,      // 时钟跳变：样本时间整体前进 8 小时
    Count
};

const wchar_t* ScenarioName(Scenario s);

// ---------------------------------------------------------------------------
// 模拟数据源：按固定节奏（10 秒）产出采样
// ---------------------------------------------------------------------------
class FakeSource {
public:
    void Select(Scenario s);
    Scenario scenario() const { return scenario_; }
    const wchar_t* scenarioName() const { return ScenarioName(scenario_); }

    // 触发一次性事件（B7）。目前只有"充值跳变"与"时钟跳变"需要。
    // jumpToYuan 可指定跳到多少（默认 100）。用来构造"位数相同/不同"两种跳变，
    // 因为逐位滚动只在位数相同时成立。
    void TriggerRecharge(double jumpToYuan = 100.0);
    void TriggerClockJump();

    // 推进到 nowSeconds（单调秒），返回这一拍应当产生的采样。
    // 内部用"第几拍"而不是浮点时间判断，所以速度倍率变化不会让节奏抖动。
    Sample NextIfDue(double nowSeconds);

    // 已产出多少条
    int produced() const { return produced_; }

    // 最近一次产出的采样（给状态机/渲染用）
    const Sample& last() const { return last_; }

    // --speed=N：把内部时间放大，快速看长期行为（设计 §0 的调试开关）
    void SetSpeed(double speed) { speed_ = speed > 0.0 ? speed : 1.0; }
    double speed() const { return speed_; }

    // 是否已产生过第一条（用来区分"冷启动"）
    bool HasAny() const { return produced_ > 0; }

private:
    double StepSeconds() const { return 10.0; }   // 固定 10 秒采样（设计 §4.2）

    Scenario scenario_ = Scenario::Steady;
    double speed_ = 1.0;
    double nextAt_ = 0.0;
    int produced_ = 0;
    int stepIndex_ = 0;
    Sample last_{};
    Amount balance_ = Amount::FromYuan(100);
    bool rechargePending_ = false;
    double rechargeTo_ = 100.0;
    bool clockJumped_ = false;
};

}  // namespace dshb
