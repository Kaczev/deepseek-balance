#include "balance_source.h"

#include "amount.h"
#include "api_client.h"

#include <windows.h>

#include <chrono>

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
Sample MakeSample(const api::BalanceResult& r) {
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
    s.note = r.detail;
    return s;
}

}  // namespace

BalanceSource::~BalanceSource() { Stop(); }

void BalanceSource::Start(const BalanceSourceConfig& cfg) {
    if (started_) return;
    stop_.store(false);
    failures_.store(0);
    started_ = true;
    worker_ = std::thread(&BalanceSource::Run, this, cfg);
}

void BalanceSource::Stop() {
    if (!started_) return;
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_ = false;
}

void BalanceSource::Run(BalanceSourceConfig cfg) {
    // 设计 §4.1：启动后立即采一次，不等第一个 10 秒。
    bool first = true;
    while (!stop_.load()) {
        if (!first) {
            std::unique_lock<std::mutex> lk(mu_);
            // 用条件变量睡：Stop() 能立刻把它叫醒，不会卡在一个周期上。
            cv_.wait_for(lk, std::chrono::milliseconds(cfg.intervalMs),
                         [this] { return stop_.load(); });
            if (stop_.load()) break;
        }
        first = false;

        api::Endpoint ep{};
        ep.host = cfg.host;
        ep.port = static_cast<unsigned short>(cfg.port);
        ep.secure = !cfg.plainHttp;
        ep.path = cfg.path;
        ep.timeoutMs = cfg.timeoutMs;

        const api::BalanceResult r = api::FetchBalance(Widen(cfg.apiKey), ep);

        const Sample s = MakeSample(r);
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(s);
            logs_.push_back(api::LogLine(r));   // J6：行内绝不含 Key
        }
        if (r.status == api::Status::Ok) failures_.store(0);
        else failures_.fetch_add(1);

        if (cfg.once) break;
    }
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