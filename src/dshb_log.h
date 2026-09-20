// dshb_log.h —— 本程序**唯一**的日志落点与写入逻辑。
//
// ===========================================================================
// 为什么要有这个头（三个写入者、一次真实的坑）
// ===========================================================================
// 2026-09-19 所有者报："运行程序的时候会在目录下生成 selftest.log。不应该这样，
// 而是将这个文件放在 Appdata 下，跟曲线那些数据放在一起才对。"
//
// 查下去发现问题是**三个写入者各写各的**：
//   ① main.cpp 的 SelfTestLog      —— exe 同目录 + selftest.log
//   ② tray.cpp 的 TrayLog          —— 又自己算了一遍 exe 同目录 + selftest.log
//   ③ renderer.cpp 的 DumpLayoutProbe —— exe 同目录 + layout.log
// 只改其中一个的症状是"一半日志在数据目录、一半还在 exe 旁边"——**比原来更难查**。
// 所以这里把它们收口：**路径只有一处定义、写入只有一处实现**。
//
// ★ 为什么是 header-only（inline）而不是一个新 .cpp：
//   写入者分散在 main.cpp / tray.cpp / renderer.cpp，而它们在**不同的可执行目标**里
//   （dshb 链全部；trayprobe 链 tray.cpp 但**不链 paths.cpp**）。新加一个 .cpp 就要给
//   每个目标补源文件 —— 那是改 CMakeLists.txt。inline 函数随头文件进各自的 TU，
//   一个目标的链接关系都不用动。
//
// ★ 绝**不**回退到 exe 旁边写。数据目录不可写时（只读介质、被策略挡住）就**什么都不写**：
//   回退会把所有者报的这个 bug 原封不动地带回来，而且从此没人再报它。
//
// ===========================================================================
// 上限：只保留一个文件，超过就截断重开
// ===========================================================================
// ★ 为什么不按 paths.h 原来那句"轮转 ≤5MB×3"做多份：所有者的抱怨正是"目录里冒出文件"。
//   为修这个 bug 而改成"攒三个 5MB 的文件"，方向是反的。而且实现是**一行一开一关**
//   （见下），多份轮转要维护"当前写到第几份"的状态，代价与收益不成比例。
//   所以：**一个文件、1 MB 上限、超了截断重开**。paths.h 里那句注释已按实际改写。

#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace dshb {

// ---------------------------------------------------------------------------
// 落点：%LOCALAPPDATA%\deepseek-balance\dshb.log（与 curve.json / config.json 并列）
// ---------------------------------------------------------------------------
// 进程内只算一次（函数内 static）。算不出来时返回空串 = "不要写日志"。
//
// ★ `--log-file=<路径>` 可以把它改到别处，存在的理由是**探针**：日志只有一个落点之后，
//   探针拉起的子进程会与所有者正在跑的挂件写**同一个文件**，而 dragprobe 是按字节偏移
//   读那个文件的（它记录启动前的大小，再读新增部分）——两个进程同时写就会互相干扰，
//   而且探针的噪声会进所有者会去看的那份日志。所以探针显式把子进程的日志指到临时目录。
inline std::wstring& LogFilePathOverride() {
    static std::wstring path;
    return path;
}
inline void SetLogFilePathOverride(std::wstring path) { LogFilePathOverride() = std::move(path); }

inline const std::wstring& LogFilePath() {
    static const std::wstring path = [] {
        if (!LogFilePathOverride().empty()) return LogFilePathOverride();
        wchar_t* base = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || !base) {
            return std::wstring();   // 取不到已知文件夹：宁可不写，也不写到 exe 旁边
        }
        std::wstring dir = base;
        CoTaskMemFree(base);
        if (!dir.empty() && dir.back() != L'\\') dir.push_back(L'\\');
        dir += L"deepseek-balance";
        CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时返回 FALSE + ERROR_ALREADY_EXISTS
        dir.push_back(L'\\');
        dir += L"dshb.log";
        return dir;
    }();
    return path;
}

// 上限。1 MB ≈ 上万行，足够定位"昨天那次为什么没显示余额"，又不至于让人不敢开着。
inline constexpr unsigned __int64 kLogMaxBytes = 1024ull * 1024ull;

// ---------------------------------------------------------------------------
// 写不写：开发者/测试调用才写详细日志
// ---------------------------------------------------------------------------
// ★ 判据是**命令行里有没有 `--` 开头的开关**（例外见下）。理由：所有者双击 exe 时
//   命令行是空的，所以"有开关"就等于"这是开发/测试调用"。
//   好处是**不需要维护一张"哪些开关算测试"的清单** —— 那种清单一定会漏，而漏掉的
//   症状是"某个探针要的日志没了、它自己红"，排查成本远高于这条规则本身的粗糙。
// ★ 两个例外：`--config=` 与 `--curve-store=` 只换数据位置，可能是普通用户在用，
//   不单独算"开发调用"。
inline bool& LogVerboseFlag() {
    static bool on = false;
    return on;
}
inline void SetLogVerbose(bool on) { LogVerboseFlag() = on; }
inline bool LogVerboseEnabled() { return LogVerboseFlag(); }

// 进程内已写字节数。★ 不在每行去 GetFileSize：那是每行一次系统调用，而日志是热路径。
inline unsigned __int64& LogBytesWritten() {
    static unsigned __int64 n = 0;
    return n;
}
inline bool& LogSizeSeeded() {
    static bool seeded = false;
    return seeded;
}

inline void LogWriteV(const wchar_t* fmt, va_list args) {
    const std::wstring& path = LogFilePath();
    if (path.empty()) return;
    const wchar_t* p = path.c_str();

    // 首次写：把现有文件的大小当作起点，否则"上限"要等写到 1 MB 之后才开始算。
    if (!LogSizeSeeded()) {
        LogSizeSeeded() = true;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &info)) {
            LogBytesWritten() =
                (static_cast<unsigned __int64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
    }

    // 到顶了就**这一次**截断重开；写完记住新的字节数，于是下一行又回到追加模式。
    const bool truncate = LogBytesWritten() >= kLogMaxBytes;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, truncate ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") != 0 || !f) return;
    vfwprintf(f, fmt, args);
    fwprintf(f, L"\n");
    // 写完问一次位置：这就是精确的当前字节数（省掉一次 GetFileSize）。
    LogBytesWritten() = static_cast<unsigned __int64>(_ftelli64(f));
    fclose(f);
}

// 无条件写：用户报症状时留着这几行才定得了位。
inline void LogLine(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWriteV(fmt, args);
    va_end(args);
}

// 只在开发/测试调用里写：帧率、每帧状态、探针脚本的逐步输出。
inline void LogVerbose(const wchar_t* fmt, ...) {
    if (!LogVerboseEnabled()) return;
    va_list args;
    va_start(args, fmt);
    LogWriteV(fmt, args);
    va_end(args);
}

}  // namespace dshb
