#pragma once
#include <windows.h>

#include <array>
#include <chrono>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace pd {

// 手机发来的输入 → Windows。做法参考 Sunshine（src/platform/windows/input.cpp）：
// 触摸用合成指针设备注入真正的 Windows 触摸，由 Windows 自己识别点按、长按、拖动、缩放；
// 鼠标和键盘用 SendInput，文字用 KEYEVENTF_UNICODE。
//
// 消息（JSON，经 WebRTC DataChannel）：
//   {t:"touch", pts:[{id, x, y, s:"down"|"move"|"up"|"cancel"}]}  x/y 是画面上的 0..1 比例
//   {t:"mabs", x, y}            鼠标移到画面 0..1 位置
//   {t:"mrel", dx, dy}          鼠标相对移动（桌面像素）
//   {t:"btn", b:1|2|3, d}       左/中/右键按下(d=true)或抬起
//   {t:"wheel", dy, dx}         滚轮，单位同 Windows（120 一格；dy 正数向上，dx 正数向右）
//   {t:"key", vk, d}            虚拟键码按下/抬起
//   {t:"combo", vks:[...]}      组合键：依次按下再倒序抬起
//   {t:"text", s}               直接输入文字（中文等）
//   {t:"hb"}                    按住期间的心跳
// 普通权限的进程不能向管理员权限的窗口注入输入（Windows 的 UIPI 限制）。
class InputInjector {
public:
	InputInjector();
	~InputInjector();

	// 当前被截取的显示器在虚拟桌面上的位置（物理像素）
	void setTarget(const RECT &desktopRect);
	void handle(const nlohmann::json &msg);
	// 连接断开时调用：松开所有按键、抬起所有手指
	void releaseAll();
	// 在截屏线程里定期调用：重复注入按住不动的触点（Windows 需要），以及心跳超时保护
	void tick();

private:
	struct Contact {
		bool active = false;
		UINT32 pointerId = 0;
		std::string clientId;
		POINT pt{};
		POINTER_FLAGS flags = 0;
	};

	POINT toDesktop(double nx, double ny) const;
	void touch(const nlohmann::json &pts);
	void injectTouchFrame();
	void mouseAbs(double nx, double ny);
	void mouseRel(int dx, int dy);
	void button(int which, bool down);
	void wheel(int dy, int dx);
	void key(WORD vk, bool down);
	void text(const std::wstring &s);
	void releaseLocked();

	static constexpr int kMaxContacts = 10;
	std::mutex mMutex;
	RECT mTarget{0, 0, 1920, 1080};
	HSYNTHETICPOINTERDEVICE mTouchDevice = nullptr;
	std::array<Contact, kMaxContacts> mContacts{};
	UINT32 mNextPointerId = 0;
	std::chrono::steady_clock::time_point mLastTouchInject{};
	std::set<int> mButtons;
	std::set<WORD> mKeys;
	std::chrono::steady_clock::time_point mLastActivity = std::chrono::steady_clock::now();
};

} // namespace pd
