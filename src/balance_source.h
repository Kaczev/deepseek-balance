// 后台采样源（J2/J4/J6）：把"真实余额"变成一条条 Sample 交给 UI 线程。
//
// ★ 为什么必须单独一个线程（设计 §11.1）：HTTP 请求绝不在 UI 线程上做。
//   哪怕用异步 HTTP 客户端，JSON 解析与金额转换也可能落在 UI 线程上——
//   一次 100ms 的解析就会让这个窗口掉好几帧，肉眼可见。
//   所以这里自己起一个线程，UI 线程只做两件事：Poll 取样本、PollLog 取日志。
//
// ★ 日志绝不包含 Key（设计 §10.5 / J6）。日志行由 api_client 的 LogLine 生成，
//   它只输出状态、币种、金额与间隔决策。

#pragma once

#include "sampling.h"
#include "tuning.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dshb {

struct BalanceSourceConfig {
    std::wstring host = L"api.deepseek.com";
    int port = 443;
    bool plainHttp = false;          // 本地测试服务器用
    std::wstring path = L"/user/balance";
    int timeoutMs = 5000;            // 设计 §4.1：请求必须在一个采样周期内完成
    int intervalMs = static_cast<int>(kApiIntervalMs);   // 默认 = 上限（自适应会自己缩短）
    std::string apiKey;
    bool once = false;               // 只取一次就停（自检用）

    // ---- 汇率（所有者 2026-09-19：只在启动时取一次）----
    // 地址可换，理由与上面的 host/port 一样：失败路径必须能在不重编译的前提下复发
    // （指向一个故意连不上的本地端口）。默认 = frankfurter/ECB，理由与实测地址见 fx_rate.h。
    bool fxEnabled = true;
    std::wstring fxHost = L"api.frankfurter.dev";
    int fxPort = 443;
    bool fxPlainHttp = false;
    std::wstring fxPath = L"/v1/latest?base=USD&symbols=CNY";
    int fxTimeoutMs = 5000;
    // 汇率缓存文件（fx.json）。空 = 不读也不写缓存，直接走"取不到就没有"。
    // 所有者追加的要求：取到了要落盘，本次取不到就用上一次存下来的。
    std::wstring fxCachePath;
};

class BalanceSource {
public:
    ~BalanceSource();
    void Start(const BalanceSourceConfig& cfg);
    void Stop();

    // ---- 暂停与唤醒（J4）----
    // 暂停期间**不发任何请求**（睡眠/锁屏时没人看，白烧请求没有意义）。
    // 唤醒时 ResumeNow() -> 立刻补一次，不等下一个周期。
    void Pause();
    void ResumeNow();
    bool paused() const { return paused_.load(); }

    // UI 线程：取走一条待处理样本。有新样本返回 true。
    bool Poll(Sample* out);
    // UI 线程：取走一行日志（J6）。行内不含 Key。
    bool PollLog(std::string* out);
    // 在**同一个日志队列**里排一行（供后台线程/夹具使用，行内不含 Key）。
    // 为什么要有它：汇率那一行是在后台线程里、第一个样本之前产生的，而日志队列是
    // UI 线程唯一读日志的地方；排进同一条队列，就能保证它和样本按同一顺序出现在
    // 主日志里（而不是被写到一个没人读的地方）。
    void Note(const std::string& line);
    // 连续失败次数。所有者定的规则：失败时先"当作没变"，连续 5 次才显示 --.--
    int consecutiveFailures() const { return failures_.load(); }
    // 距离下一次请求还有多少毫秒（给右上角倒计时用）。没有排定则 0。
    int msUntilNextFetch() const;
    bool started() const { return started_; }

private:
    void Run(BalanceSourceConfig cfg);
    int currentIntervalMs() const { return intervalMs_.load(); }


    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Sample> pending_;
    std::vector<std::string> logs_;
    std::atomic<int> failures_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};   // 暂停中：不发请求
    std::atomic<int> intervalMs_{static_cast<int>(kApiIntervalMs)};   // 当前生效的间隔（自适应）
    std::atomic<long long> nextDueMs_{0};    // 下一次请求的到期时刻（steady 毫秒）
    Sample prev_{};                          // 上一次成功样本（比"有没有变化"用）
    bool havePrev_ = false;
    bool started_ = false;
};

}  // namespace dshb