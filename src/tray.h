#pragma once
#include <windows.h>

#include <functional>
#include <string>

namespace pd {

// 托盘图标和右键菜单。在主线程里创建并运行消息循环。
class Tray {
public:
	struct Callbacks {
		std::function<void()> onPair;     // 手机扫码连接
		std::function<void()> onOpenLogs; // 打开日志文件夹
		std::function<void()> onExit;
	};

	bool create(HINSTANCE instance, Callbacks callbacks);
	// 运行消息循环，直到 quit() 或菜单里点退出
	void run();
	// 可以在任何线程调用
	void quit();
	void setTooltip(const std::wstring &text);
	void notify(const std::wstring &title, const std::wstring &text);

private:
	static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
	LRESULT handle(UINT, WPARAM, LPARAM);
	void addIcon();
	void showMenu();

	HWND mWnd = nullptr;
	HICON mIcon = nullptr;
	UINT mTaskbarCreated = 0;
	Callbacks mCallbacks;
	std::wstring mTooltip = L"PocketDesk";
};

} // namespace pd
