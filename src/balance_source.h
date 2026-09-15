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
    int intervalMs = 10000;          // 设计 §4.1：固定 10 秒
    std::string apiKey;
    bool once = false;               // 只取一次就停（自检用）
};

class BalanceSource {
public:
    ~BalanceSource();
    void Start(const BalanceSourceConfig& cfg);
    void Stop();

    // UI 线程：取走一条待处理样本。有新样本返回 true。
    bool Poll(Sample* out);
    // UI 线程：取走一行日志（J6）。行内不含 Key。
    bool PollLog(std::string* out);
    // 连续失败次数。所有者定的规则：失败时先"当作没变"，连续 5 次才显示 --.--
    int consecutiveFailures() const { return failures_.load(); }
    bool started() const { return started_; }

private:
    void Run(BalanceSourceConfig cfg);

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Sample> pending_;
    std::vector<std::string> logs_;
    std::atomic<int> failures_{0};
    std::atomic<bool> stop_{false};
    bool started_ = false;
};

}  // namespace dshb