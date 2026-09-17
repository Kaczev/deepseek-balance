#include "single_instance.h"

namespace dshb {

namespace {

HANDLE g_mutex = nullptr;

}  // namespace

bool AcquireSingleInstance() {
    // CreateMutexW：第一个调用者拿到 ERROR_SUCCESS，后续调用者拿到 ERROR_ALREADY_EXISTS。
    // 句柄故意不关：它要活到进程结束，互斥体才会在最后一个持有者退出时释放。
    g_mutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (g_mutex == nullptr) {
        // 连互斥体都建不出来（权限/内核对象配额）：宁可让它跑起来，也不要在这里拒绝启动
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例：通知它闪一次，然后本进程退出。
        // 注意 PostMessage 的窗口必须是**线程消息队列可达**的；这里用类名找窗口。
        // ★ 类名必须与 main.cpp 的 kClassName 一致：原来写的是 "DshbWidgetWnd"，
//   永远找不到窗口 -> "通知旧窗口闪一下"这条静默失效（所有者点了第二次看不到任何反应）。
        HWND existing = FindWindowW(L"DshbWnd", nullptr);
        if (existing) PostMessageW(existing, kMsgActivate, 0, 0);
        return false;
    }
    return true;
}

}  // namespace dshb
