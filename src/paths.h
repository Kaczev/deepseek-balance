// 路径集中定义（A12b）
//
// 三条规矩，都是踩过或用血换来的：
//   1. **绝不相对当前工作目录**。双击 exe 时工作目录是别的地方（常常是 System32），
//      日志和数据会落到用户找不到的位置。一律拼绝对路径。
//   2. 用户数据放用户目录（%LOCALAPPDATA%），**不放 exe 旁边**：exe 可能被放在
//      Program Files 之类只读目录里，那里写不进去。
//   3. 目录建不出来或写不进去时**降级**，不要崩：数据退化成仅内存，日志退化成
//      什么都不写，并在能写的地方说明原因。

#pragma once

#include <windows.h>

#include <string>

namespace dshb {

struct AppPaths {
    std::wstring dataDir;      // %LOCALAPPDATA%\deepseek-balance
    std::wstring samples;      // dataDir\samples.jsonl   —— 采样记录
    std::wstring log;          // dataDir\widget.log      —— 运行日志（轮转 ≤5MB×3）
    std::wstring config;       // dataDir\config.json     —— 窗口位置等设置
    bool writable = false;     // 目录是否真的能建、能写
    std::wstring unwritableReason;   // 不能写时的原因（进日志用）
};

// 解析并试探可写性。多次调用返回同一份结果。
const AppPaths& Paths();

// 目录不可写时，把降级原因记到**别的地方**（事件日志或 stderr 无从下手，
// 所以记到 %TEMP%），否则用户永远不知道数据没落盘。
void RecordUnwritableFallback(const std::wstring& reason);

}  // namespace dshb
