#pragma once
#include <string>

namespace pd {

// 当前进程是不是以 SYSTEM 账户运行（由系统服务启动时是）
bool isLocalSystem();

// 把调用线程切到当前接收输入的桌面（平时是 Default，锁屏和 UAC 时是 Winlogon）。
// 只有 SYSTEM 进程能切到 Winlogon；普通权限下调用也没有坏处，只是切不过去。
// 截屏和注入输入的线程在失败后调用它再重试（参考 Sunshine 的 syncThreadDesktop）。
// 返回切换后线程所在桌面的名字，失败返回空。
std::string syncThreadDesktop();

// 以当前登录用户的身份打开网址或文件夹（SYSTEM 进程直接打开会用 SYSTEM 账户运行浏览器，既不安全也看不到用户的书签）。
// 不是 SYSTEM 时直接用 ShellExecute。没有用户登录时什么也不做。
void openAsUser(const std::wstring &target);

} // namespace pd
