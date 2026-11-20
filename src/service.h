#pragma once
#include <filesystem>

namespace pd::service {

// 系统服务（参考 Sunshine 的 SunshineService）：服务本身以 SYSTEM 运行，不截屏也不联网，
// 只负责在当前显示在屏幕上的会话里启动 pocketdesk.exe --agent（同样是 SYSTEM 账户），
// 这样锁屏、UAC 弹窗、开机还没登录时也能截屏和操作。程序退出或会话切换时重新启动它。

// 程序（--agent）退出码，告诉服务接下来怎么做
constexpr int kExitUserQuit = 3; // 用户在托盘里点了退出：服务也停下，直到下次开机或再次打开 PocketDesk
constexpr int kExitRestart = 4;  // 需要立即重启（例如 IP 变了，要重新签证书）

// --service：由服务管理器调用
int run(const std::filesystem::path &exe);

// --install：注册并启动服务、加防火墙规则、收紧安装目录权限。需要管理员权限，由安装包调用。
int install(const std::filesystem::path &exe, const std::filesystem::path &dataDir);

// --uninstall：停止并删除服务、结束本目录下正在运行的 PocketDesk、删除防火墙规则。由卸载程序调用。
int uninstall(const std::filesystem::path &exe);

enum class State { NotInstalled, Stopped, Running, Pending };
// 服务装的是不是这个 exe，以及它现在的状态（不是这个 exe 时返回 NotInstalled）
State stateFor(const std::filesystem::path &exe);
// 普通用户也能启动（安装时给了交互式用户启动权限）
bool start();

} // namespace pd::service
