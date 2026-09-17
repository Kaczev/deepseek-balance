// 单实例（A11）
//
// 为什么必须有：两个实例会互相抢拖动、并同时写同一个历史文件。
//
// ★ 一个容易写错的地方：**"把已有实例提到前台"在这里没有落点**。
//   这个窗口是 WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW 的——它既不能成为前台窗口，
//   也不出现在 Alt+Tab 里。所以"激活"只能由我们自己给一次可见反馈。
//   本实现的做法：向已有实例发一条自定义消息，让它闪一次描边。

#pragma once

#include <windows.h>

namespace dshb {

// 第二个实例向已有实例发的消息。wParam/lParam 留作以后传参。
constexpr UINT kMsgActivate = WM_APP + 1;

// 命名互斥体：跨会话可见（Global\ 前缀），所以多用户/多会话下也不会重复。
inline constexpr wchar_t kInstanceMutexName[] = L"Global\\deepseek-balance-v0.2";

// 尝试成为唯一实例。
//   返回 true  = 我是第一个，可以继续启动。
//   返回 false = 已经有实例在跑，本进程应当退出（已经尝试通知它闪一下）。
bool AcquireSingleInstance();

}  // namespace dshb
