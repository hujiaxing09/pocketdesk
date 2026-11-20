#include "input.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace pd {

namespace {

// 按住不动的触点每隔这么久重新注入一次，否则 Windows 会认为触点丢失（Sunshine 也这样做）
constexpr auto kTouchRepeat = std::chrono::milliseconds(50);
// 有按键/触点按住时，超过这么久没有任何消息就全部松开（手机断网、切后台）
constexpr auto kWatchdog = std::chrono::milliseconds(1500);

bool isExtendedKey(WORD vk) {
	switch (vk) {
	case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
	case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
	case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_NUMLOCK: case VK_SNAPSHOT:
		return true;
	default:
		return false;
	}
}

void warnRejected(const char *what) {
	static auto lastWarn = std::chrono::steady_clock::time_point{};
	auto now = std::chrono::steady_clock::now();
	if (now - lastWarn > std::chrono::seconds(5)) {
		log("{} 被拒绝（错误 {}）：前台可能是管理员权限的窗口，或者处于锁屏/UAC 界面", what,
		    GetLastError());
		lastWarn = now;
	}
}

void sendInputs(std::vector<INPUT> &inputs) {
	if (inputs.empty())
		return;
	if (SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size())
		warnRejected("SendInput");
	inputs.clear();
}

std::wstring widen(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

} // namespace

InputInjector::InputInjector() {
	mTouchDevice = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts, POINTER_FEEDBACK_DEFAULT);
	if (!mTouchDevice)
		log("创建合成触摸设备失败（错误 {}），触控模式不可用", GetLastError());
	for (int i = 0; i < kMaxContacts; ++i)
		mContacts[i].pointerId = UINT32(i);
}

InputInjector::~InputInjector() {
	releaseAll();
	if (mTouchDevice)
		DestroySyntheticPointerDevice(mTouchDevice);
}

void InputInjector::setTarget(const RECT &desktopRect) {
	std::lock_guard lock(mMutex);
	mTarget = desktopRect;
}

POINT InputInjector::toDesktop(double nx, double ny) const {
	nx = std::clamp(nx, 0.0, 1.0);
	ny = std::clamp(ny, 0.0, 1.0);
	const LONG w = mTarget.right - mTarget.left;
	const LONG h = mTarget.bottom - mTarget.top;
	return POINT{mTarget.left + std::min(w - 1, LONG(nx * w)),
	             mTarget.top + std::min(h - 1, LONG(ny * h))};
}

void InputInjector::handle(const nlohmann::json &msg) {
	const std::string t = msg.value("t", "");
	std::lock_guard lock(mMutex);
	mLastActivity = std::chrono::steady_clock::now();
	if (t == "touch") {
		touch(msg.value("pts", nlohmann::json::array()));
	} else if (t == "mabs") {
		mouseAbs(msg.value("x", 0.0), msg.value("y", 0.0));
	} else if (t == "mrel") {
		mouseRel(msg.value("dx", 0), msg.value("dy", 0));
	} else if (t == "btn") {
		button(msg.value("b", 1), msg.value("d", false));
	} else if (t == "wheel") {
		wheel(msg.value("dy", 0), msg.value("dx", 0));
	} else if (t == "key") {
		int vk = msg.value("vk", 0);
		if (vk > 0 && vk < 256)
			key(WORD(vk), msg.value("d", false));
	} else if (t == "combo") {
		std::vector<WORD> keys;
		for (auto &k : msg.value("vks", nlohmann::json::array()))
			if (k.is_number_integer() && k.get<int>() > 0 && k.get<int>() < 256)
				keys.push_back(WORD(k.get<int>()));
		for (WORD k : keys)
			key(k, true);
		for (auto it = keys.rbegin(); it != keys.rend(); ++it)
			key(*it, false);
	} else if (t == "text") {
		text(widen(msg.value("s", "")));
	}
}

// ---- 触摸（参考 Sunshine 的 touch_update / perform_touch_injection）----

void InputInjector::touch(const nlohmann::json &pts) {
	if (!mTouchDevice)
		return;
	bool changed = false;
	for (auto &p : pts) {
		const std::string id = p.contains("id") ? p["id"].dump() : "";
		const std::string s = p.value("s", "");
		const POINT pt = toDesktop(p.value("x", 0.0), p.value("y", 0.0));
		Contact *c = nullptr;
		for (auto &slot : mContacts)
			if (slot.active && slot.clientId == id)
				c = &slot;
		if (s == "down") {
			if (c)
				continue; // 重复的 down
			for (auto &slot : mContacts)
				if (!slot.active) {
					c = &slot;
					break;
				}
			if (!c)
				continue; // 超过 10 个触点
			c->active = true;
			c->clientId = id;
			c->pt = pt;
			log("触摸按下 桌面坐标 ({},{})", pt.x, pt.y);
			c->flags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
			changed = true;
		} else if (c && s == "move") {
			c->pt = pt;
			// 同一帧里刚按下的触点保持 DOWN，注入后再变成 UPDATE
			if (!(c->flags & POINTER_FLAG_DOWN))
				c->flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
			changed = true;
		} else if (c && (s == "up" || s == "cancel")) {
			if (c->flags & POINTER_FLAG_DOWN)
				injectTouchFrame(); // 先把 DOWN 注入，再抬起
			c->pt = pt;
			c->flags = POINTER_FLAG_UP | (s == "cancel" ? POINTER_FLAG_CANCELED : 0);
			changed = true;
		}
	}
	if (changed)
		injectTouchFrame();
}

void InputInjector::injectTouchFrame() {
	// Windows 要求每次注入都包含所有仍在接触的触点
	std::vector<POINTER_TYPE_INFO> frame;
	for (auto &c : mContacts) {
		if (!c.active)
			continue;
		POINTER_TYPE_INFO info{};
		info.type = PT_TOUCH;
		POINTER_TOUCH_INFO &ti = info.touchInfo;
		ti.pointerInfo.pointerType = PT_TOUCH;
		ti.pointerInfo.pointerId = c.pointerId;
		ti.pointerInfo.ptPixelLocation = c.pt;
		ti.pointerInfo.pointerFlags = c.flags;
		ti.touchFlags = TOUCH_FLAG_NONE;
		ti.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
		ti.rcContact = RECT{c.pt.x - 4, c.pt.y - 4, c.pt.x + 4, c.pt.y + 4};
		ti.orientation = 90;
		ti.pressure = 512;
		frame.push_back(info);
	}
	if (frame.empty())
		return;
	if (!InjectSyntheticPointerInput(mTouchDevice, frame.data(), UINT32(frame.size())))
		warnRejected("触摸注入");
	mLastTouchInject = std::chrono::steady_clock::now();
	for (auto &c : mContacts) {
		if (!c.active)
			continue;
		if (c.flags & POINTER_FLAG_UP) {
			c.active = false;
			c.clientId.clear();
		} else {
			c.flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
		}
	}
}

// ---- 鼠标和键盘 ----

void InputInjector::mouseAbs(double nx, double ny) {
	const POINT p = toDesktop(nx, ny);
	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
	const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
	INPUT in{};
	in.type = INPUT_MOUSE;
	in.mi.dx = LONG(std::clamp((p.x - vx) * 65535.0 / vw, 0.0, 65535.0));
	in.mi.dy = LONG(std::clamp((p.y - vy) * 65535.0 / vh, 0.0, 65535.0));
	in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
	std::vector<INPUT> v{in};
	sendInputs(v);
}

void InputInjector::mouseRel(int dx, int dy) {
	if (!dx && !dy)
		return;
	INPUT in{};
	in.type = INPUT_MOUSE;
	in.mi.dx = dx;
	in.mi.dy = dy;
	in.mi.dwFlags = MOUSEEVENTF_MOVE;
	std::vector<INPUT> v{in};
	sendInputs(v);
}

void InputInjector::button(int which, bool down) {
	DWORD flag = 0;
	switch (which) {
	case 1: flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
	case 2: flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
	case 3: flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
	default: return;
	}
	if (down)
		mButtons.insert(which);
	else
		mButtons.erase(which);
	INPUT in{};
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = flag;
	std::vector<INPUT> v{in};
	sendInputs(v);
}

void InputInjector::wheel(int dy, int dx) {
	std::vector<INPUT> v;
	if (dy) {
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_WHEEL;
		in.mi.mouseData = DWORD(dy);
		v.push_back(in);
	}
	if (dx) {
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		in.mi.mouseData = DWORD(dx);
		v.push_back(in);
	}
	sendInputs(v);
}

void InputInjector::key(WORD vk, bool down) {
	if (down)
		mKeys.insert(vk);
	else
		mKeys.erase(vk);
	INPUT in{};
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
	in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (isExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
	std::vector<INPUT> v{in};
	sendInputs(v);
}

void InputInjector::text(const std::wstring &s) {
	std::vector<INPUT> inputs;
	for (wchar_t c : s) {
		if (c == L'\r')
			continue;
		if (c == L'\n') {
			sendInputs(inputs);
			key(VK_RETURN, true);
			key(VK_RETURN, false);
			continue;
		}
		INPUT in{};
		in.type = INPUT_KEYBOARD;
		in.ki.wScan = c; // UTF-16 码元；代理对按两个码元发送，Windows 会拼回去
		in.ki.dwFlags = KEYEVENTF_UNICODE;
		inputs.push_back(in);
		in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		inputs.push_back(in);
	}
	sendInputs(inputs);
}

// ---- 释放和保护 ----

void InputInjector::releaseLocked() {
	bool anyContact = false;
	for (auto &c : mContacts)
		if (c.active) {
			c.flags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
			anyContact = true;
		}
	if (anyContact && mTouchDevice)
		injectTouchFrame();
	for (int b : std::set<int>(mButtons))
		button(b, false);
	for (WORD k : std::set<WORD>(mKeys))
		key(k, false);
}

void InputInjector::releaseAll() {
	std::lock_guard lock(mMutex);
	releaseLocked();
}

void InputInjector::tick() {
	std::lock_guard lock(mMutex);
	const auto now = std::chrono::steady_clock::now();
	bool anyContact = std::any_of(mContacts.begin(), mContacts.end(), [](auto &c) { return c.active; });
	bool anyHeld = anyContact || !mButtons.empty() || !mKeys.empty();
	if (anyHeld && now - mLastActivity > kWatchdog) {
		log("手机 {} ms 没有消息，松开所有按键和触点", kWatchdog.count());
		releaseLocked();
		return;
	}
	if (anyContact && now - mLastTouchInject > kTouchRepeat)
		injectTouchFrame();
}

} // namespace pd
