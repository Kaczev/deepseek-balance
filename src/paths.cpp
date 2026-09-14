#include "paths.h"

#include <shlobj.h>

#include <cstdio>

namespace dshb {

namespace {

std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += b;
    return out;
}

// 真去做一次"建目录 + 写一个临时文件再删掉"，才敢说 writable。
// 只看目录是否存在是骗人的：存在但只读、或被重定向到不存在的盘，都会被漏掉。
bool ProbeWritable(const std::wstring& dir, std::wstring* reason) {
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            wchar_t buf[128];
            swprintf_s(buf, L"CreateDirectory 失败 err=%lu", err);
            if (reason) *reason = buf;
            return false;
        }
    }
    const std::wstring probe = Join(dir, L".write-probe.tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        wchar_t buf[128];
        swprintf_s(buf, L"写入试探文件失败 err=%lu", GetLastError());
        if (reason) *reason = buf;
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

AppPaths g_paths;
bool g_resolved = false;

}  // namespace

const AppPaths& Paths() {
    if (g_resolved) return g_paths;

    wchar_t* localAppData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
        localAppData) {
        base = localAppData;
        CoTaskMemFree(localAppData);
    }

    if (base.empty()) {
        // 取不到已知文件夹时的兜底：exe 旁边（可能不可写，后面会探）
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = L'\0';
        base = exe;
    }

    g_paths.dataDir = Join(base, L"deepseek-balance");
    g_paths.samples = Join(g_paths.dataDir, L"samples.jsonl");
    g_paths.log = Join(g_paths.dataDir, L"widget.log");
    g_paths.config = Join(g_paths.dataDir, L"config.json");

    std::wstring reason;
    g_paths.writable = ProbeWritable(g_paths.dataDir, &reason);
    g_paths.unwritableReason = reason;
    g_resolved = true;

    if (!g_paths.writable) RecordUnwritableFallback(reason);
    return g_paths;
}

void RecordUnwritableFallback(const std::wstring& reason) {
    // 落盘不了的时候，唯一还能写的大概只剩 %TEMP%。写在这里，
    // 并在文件里写清"数据不会保留"，否则用户会以为历史在正常记录。
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) return;
    const std::wstring path = Join(temp, L"deepseek-balance-unwritable.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"数据目录不可写：%ls\n原因：%ls\n后果：采样不会落盘，重启后没有历史。\n\n",
                 Paths().dataDir.c_str(), reason.c_str());
        fclose(f);
    }
}

}  // namespace dshb
