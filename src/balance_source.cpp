#include "balance_source.h"

#include "amount.h"
#include "api_client.h"
#include "fx_rate.h"
#include "tuning.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace dshb {

namespace {

int64_t NowWallMsLocal() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// 把 api_client 的结果变成一条 Sample。**成功与失败都要变成 Sample**：
// 状态机靠"有没有成功样本"判断连接状态；界面靠 amountsOk 决定要不要动显示值——
// 失败时 amountsOk=false，界面就保持原样（所有者："姑且先当作没变显示"）。
//
// ★ 汇率就在这一层用掉（`rate` 参数）。为什么是这里、而不是显示层或 OnSample：
//   1. 这是**唯一**把 api::BalanceEntry 变成 CurrencyAmount 的地方 —— 换算是"再产生
//      一个 CurrencyAmount"，和接口自己给的条目走同一条路，下游一个分支都不用加。
//   2. 它在后台线程里跑，UI 线程每帧只做 Poll；显示层那一层没有网络、也不该有网络。
//   3. 曲线记录（FeedCurve）吃的是 Sample::entries，所以补在这里 = 曲线自动有 USD 键。
Sample MakeSample(const api::BalanceResult& r, const fx::Rate& rate) {
    Sample s{};
    s.wallMs = NowWallMsLocal();
    s.monotonicMs = static_cast<int64_t>(GetTickCount64());
    s.bootId = 0;
    s.httpStatus = r.httpStatus;
    s.transportOk = (r.httpStatus > 0);
    s.isAvailable = r.isAvailable;

    const int idx = api::PreferredEntryIndex(r.entries);
    if (r.status == api::Status::Ok && idx >= 0) {
        const api::BalanceEntry& e = r.entries[static_cast<size_t>(idx)];
        s.currency = e.currency;
        // ★ 金额按十进制字符串解析（设计 §3.1），绝不过二进制浮点。
        Amount a{};
        if (ParseAmount(e.totalBalance, &a)) {
            s.total = a;
            s.amountsOk = true;
        }
        ParseAmount(e.grantedBalance, &s.granted);
        ParseAmount(e.toppedUpBalance, &s.toppedUp);
    }
    // 全部条目都带上：点击币种符号时要在它们之间切换（所以不能只留优先的那条）
    for (const api::BalanceEntry& e : r.entries) {
        CurrencyAmount ca{};
        ca.currency = e.currency;
        Amount a{};
        if (ParseAmount(e.totalBalance, &a)) {
            ca.total = a;
            ca.ok = true;
        }
        s.entries.push_back(ca);
    }
    // ---- 汇率原样带上（**折成元那个方向**：危险度 D 与曲线颜色唯一的换算口）----
    // ★ 这是 `rate` 的第二个用处，而且方向相反：InjectUsdCounterpart 用它把元折成美元
    //   （显示用，截断到分）；这里把"1 美元 = 多少元"逐字带给样本，供 fx::UsdAmountToYuan
    //   把美元折回元。
    // ★ 两者是**同一个响应**里的同一个数（base=USD&symbols=CNY 读出来就是美元->元），
    //   所以不需要第二次请求；时机也没变 —— 仍然是启动时那一次（Run 的最前面）。
    // ★ ok 逐字跟着 rate.ok：没有汇率 = "美元折不成元"，下游据此决定
    //   （D 取什么值见 widget_display.cpp 的 AdvanceAmbience）。
    s.usdToCnyOk = rate.ok;
    s.usdToCnyText = rate.ok ? rate.rateText : std::string();

    // 只有 CNY 的账号（大陆账号）在这里多出一个 USD 条目，形状和接口给的一模一样。
    // 海外账号（已经有 USD）这条调用什么都不做，逐字段不变。
    fx::InjectUsdCounterpart(&s, rate);
    s.note = r.detail;
    return s;
}

// 两条样本的取值是否相同：币种与金额逐条比。
// 用它决定间隔是缩短还是延长（所有者：有变化就缩短，没变化就延长）。
bool SameAmounts(const Sample& a, const Sample& b) {
    if (a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        if (a.entries[i].currency != b.entries[i].currency) return false;
        if (a.entries[i].total.raw != b.entries[i].total.raw) return false;
        if (a.entries[i].ok != b.entries[i].ok) return false;
    }
    if (a.entries.empty()) {
        return a.total.raw == b.total.raw && a.currency == b.currency;
    }
    return true;
}

}  // namespace

BalanceSource::~BalanceSource() { Stop(); }

void BalanceSource::Start(const BalanceSourceConfig& cfg) {
    if (started_) return;
    stop_.store(false);
    failures_.store(0);
    intervalMs_.store(cfg.intervalMs);
    started_ = true;
    worker_ = std::thread(&BalanceSource::Run, this, cfg);
}

void BalanceSource::Pause() {
    paused_.store(true);
    cv_.notify_all();   // 把正在睡的循环叫醒，让它进暂停等待
}

void BalanceSource::ResumeNow() {
    paused_.store(false);
    nextDueMs_.store(0);   // 立刻到期 -> 马上取一次
    cv_.notify_all();
}

void BalanceSource::Stop() {
    if (!started_) return;
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_ = false;
}

void BalanceSource::Note(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    logs_.push_back(line);
}

void BalanceSource::Run(BalanceSourceConfig cfg) {
    // ---- 汇率：整条线程生命周期里只取这一次（所有者 2026-09-19）----
    // ★ 放在 Run 的最前面、第一次采样之前，所以后台线程第一次交付样本时汇率已经就位；
    //   UI 线程从头到尾不知道有"取汇"这回事，也不会因此掉一帧。
    // ★ 它确实会推迟第一次采样：最长 = fxTimeoutMs + 读缓存。5 s 的硬超时就是这个代价
    //   的上限，而暂停/唤醒（Pause/ResumeNow）走的是下面那个循环，**不会**再取第二次。
    // ★ 回退顺序（所有者追加）：本次取汇 -> 磁盘缓存 -> 没有汇率。三步都在
    //   fx::ResolveRate 里，这里只负责把地址和缓存路径给它、把结果记进日志。
    fx::Rate fxRate{};
    if (cfg.fxEnabled) {
        fx::RateEndpoint fxEp{};
        fxEp.host = cfg.fxHost;
        fxEp.port = static_cast<unsigned short>(cfg.fxPort);
        fxEp.secure = !cfg.fxPlainHttp;
        fxEp.path = cfg.fxPath;
        fxEp.timeoutMs = cfg.fxTimeoutMs;
        fxRate = fx::ResolveRate(fxEp, cfg.fxCachePath);
    } else {
        fxRate.error = "--fx=off";
    }
    // 启动过渡那一行：成败都记，值/来源/日期/是否来自缓存/为什么回退都在里面。
    Note(fxRate.LogLine());
    if (!fxRate.cacheSaveError.empty()) {
        // 取到了、这个会话能用，但下一次开机不会有它 —— 这一句是给"为什么下次还得再取"
        // 留的解释，不是错误。
        Note("[fx] 本次取到了汇率，但没能写进缓存：" + fxRate.cacheSaveError);
    }

    // 设计 §4.1：启动后立即采一次，不等第一个 10 秒。
    bool first = true;
    while (!stop_.load()) {
        // 暂停（睡眠/锁屏）：不排期、不发请求，等唤醒
        if (paused_.load()) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_.load() || !paused_.load(); });
            if (stop_.load()) break;
            first = true;   // 唤醒后立刻补一次
            continue;
        }
        if (!first) {
            std::unique_lock<std::mutex> lk(mu_);
            // 用条件变量睡：Stop() 能立刻把它叫醒，不会卡在一个周期上。
            // 排定下一次到期时刻（倒计时读它）
            nextDueMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count() +
                             intervalMs_.load());
            // ★ 谓词必须把"暂停"和"唤醒补一次"也算进去，否则 notify 会被忽略：
            //   带谓词的 wait_for 即使被叫醒，只要谓词为假就继续睡到超时——
            //   实测后果是"解锁后 1.3 秒才取"，不是立刻（时间戳看出来的）。
            cv_.wait_for(lk, std::chrono::milliseconds(intervalMs_.load()),
                         [this] {
                             return stop_.load() || paused_.load() || nextDueMs_.load() == 0;
                         });
            if (stop_.load()) break;
            // ★ 睡醒后必须**重新检查暂停**：否则锁屏那一刻正好睡醒，会多取一次
            //   （实测：锁屏 3.0s，3.1s 就冒出一个请求）。
            if (paused_.load()) {
                first = true;   // 恢复后立刻补一次
                continue;
            }
        }
        first = false;
        nextDueMs_.store(0);   // 正在请求：倒计时归零

        api::Endpoint ep{};
        ep.host = cfg.host;
        ep.port = static_cast<unsigned short>(cfg.port);
        ep.secure = !cfg.plainHttp;
        ep.path = cfg.path;
        ep.timeoutMs = cfg.timeoutMs;

        const api::BalanceResult r = api::FetchBalance(Widen(cfg.apiKey), ep);

        const Sample s = MakeSample(r, fxRate);
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(s);
            logs_.push_back(api::LogLine(r));   // J6：行内绝不含 Key
        }
        // ---- 固定节奏（所有者规格 §2.3：10 秒获取一个点，不做自适应）----
        // 日志仍然记录"值有没有变化"，因为它决定有没有新点（曲线滚动由新点触发）。
        if (r.status == api::Status::Ok) {
            const bool changed = !havePrev_ || !SameAmounts(prev_, s);
            {
                std::lock_guard<std::mutex> lk(mu_);
                char ib[160];
                if (!havePrev_) {
                    std::snprintf(ib, sizeof(ib), "interval -> %dms fixed (first sample)", intervalMs_.load());
                } else {
                    std::snprintf(ib, sizeof(ib), "interval -> %dms fixed (%s)", intervalMs_.load(),
                                  changed ? "value changed -> new point" : "unchanged -> no point");
                }
                logs_.push_back(ib);
            }
            prev_ = s;
            havePrev_ = true;
        }
        if (r.status == api::Status::Ok) failures_.store(0);
        else failures_.fetch_add(1);

        if (cfg.once) break;
    }
}

int BalanceSource::msUntilNextFetch() const {
    const long long due = nextDueMs_.load();
    if (due <= 0) return 0;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
    return due > now ? static_cast<int>(due - now) : 0;
}

bool BalanceSource::Poll(Sample* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.empty()) return false;
    *out = pending_.front();
    pending_.erase(pending_.begin());
    return true;
}

bool BalanceSource::PollLog(std::string* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (logs_.empty()) return false;
    *out = logs_.front();
    logs_.erase(logs_.begin());
    return true;
}

}  // namespace dshb
