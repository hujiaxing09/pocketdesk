#include "tray.h"

#include <shellapi.h>

#include <memory>

namespace pd {

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kQuitMessage = WM_APP + 2;
constexpr UINT kTooltipMessage = WM_APP + 3;
constexpr UINT kIconId = 1;
enum MenuId : UINT { kMenuPair = 1, kMenuLogs, kMenuExit };
} // namespace

bool Tray::create(HINSTANCE instance, Callbacks callbacks) {
	mCallbacks = std::move(callbacks);
	mIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	if (!mIcon)
		mIcon = LoadIconW(nullptr, IDI_APPLICATION);

	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc = wndProc;
	wc.hInstance = instance;
	wc.lpszClassName = L"PocketDeskTray";
	wc.hIcon = mIcon;
	RegisterClassExW(&wc);
	mWnd = CreateWindowExW(0, wc.lpszClassName, L"PocketDesk", 0, 0, 0, 0, 0, nullptr, nullptr,
	                       instance, this);
	if (!mWnd)
		return false;
	mTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	// 由系统服务启动时进程是 SYSTEM，权限比资源管理器高，Windows 默认挡掉资源管理器发来的消息（UIPI），
	// 托盘图标就点不动、资源管理器重启后也加不回来。放行这两种消息。
	ChangeWindowMessageFilterEx(mWnd, kTrayMessage, MSGFLT_ALLOW, nullptr);
	ChangeWindowMessageFilterEx(mWnd, mTaskbarCreated, MSGFLT_ALLOW, nullptr);
	addIcon();
	return true;
}

void Tray::addIcon() {
	NOTIFYICONDATAW nid{sizeof(nid)};
	nid.hWnd = mWnd;
	nid.uID = kIconId;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
	nid.uCallbackMessage = kTrayMessage;
	nid.hIcon = mIcon;
	wcsncpy_s(nid.szTip, mTooltip.c_str(), _TRUNCATE);
	Shell_NotifyIconW(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void Tray::run() {
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

void Tray::quit() {
	if (mWnd)
		PostMessageW(mWnd, kQuitMessage, 0, 0);
}

void Tray::setTooltip(const std::wstring &text) {
	if (!mWnd)
		return;
	// 交给窗口线程处理
	auto *copy = new std::wstring(text);
	if (!PostMessageW(mWnd, kTooltipMessage, 0, reinterpret_cast<LPARAM>(copy)))
		delete copy;
}

void Tray::notify(const std::wstring &title, const std::wstring &text) {
	NOTIFYICONDATAW nid{sizeof(nid)};
	nid.hWnd = mWnd;
	nid.uID = kIconId;
	nid.uFlags = NIF_INFO;
	nid.dwInfoFlags = NIIF_INFO;
	wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
	wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::showMenu() {
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, kMenuPair, L"手机扫码连接…");
	AppendMenuW(menu, MF_STRING, kMenuLogs, L"打开日志文件夹");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 PocketDesk");
	SetMenuDefaultItem(menu, kMenuPair, FALSE);
	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(mWnd); // 否则点别处菜单不会消失
	UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0,
	                          mWnd, nullptr);
	DestroyMenu(menu);
	if (cmd == kMenuPair && mCallbacks.onPair)
		mCallbacks.onPair();
	else if (cmd == kMenuLogs && mCallbacks.onOpenLogs)
		mCallbacks.onOpenLogs();
	else if (cmd == kMenuExit) {
		if (mCallbacks.onExit)
			mCallbacks.onExit();
		quit();
	}
}

LRESULT CALLBACK Tray::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_NCCREATE) {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto *self = reinterpret_cast<Tray *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (self && self->mWnd == hwnd)
		return self->handle(msg, wp, lp);
	return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Tray::handle(UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == mTaskbarCreated && mTaskbarCreated) {
		addIcon(); // 资源管理器重启后图标会丢，重新加
		return 0;
	}
	switch (msg) {
	case kTrayMessage:
		switch (LOWORD(lp)) {
		case WM_CONTEXTMENU:
		case WM_RBUTTONUP:
			showMenu();
			break;
		case NIN_SELECT:
		case NIN_KEYSELECT:
		case WM_LBUTTONDBLCLK:
			if (mCallbacks.onPair)
				mCallbacks.onPair();
			break;
		}
		return 0;
	case kTooltipMessage: {
		std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
		mTooltip = *text;
		NOTIFYICONDATAW nid{sizeof(nid)};
		nid.hWnd = mWnd;
		nid.uID = kIconId;
		nid.uFlags = NIF_TIP | NIF_SHOWTIP;
		wcsncpy_s(nid.szTip, mTooltip.c_str(), _TRUNCATE);
		Shell_NotifyIconW(NIM_MODIFY, &nid);
		return 0;
	}
	case kQuitMessage: {
		NOTIFYICONDATAW nid{sizeof(nid)};
		nid.hWnd = mWnd;
		nid.uID = kIconId;
		Shell_NotifyIconW(NIM_DELETE, &nid);
		DestroyWindow(mWnd);
		mWnd = nullptr;
		PostQuitMessage(0);
		return 0;
	}
	}
	return DefWindowProcW(mWnd, msg, wp, lp);
}

} // namespace pd
