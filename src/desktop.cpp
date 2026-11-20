#include "desktop.h"
#include "log.h"

#include <windows.h>
#include <shellapi.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <mutex>
#include <vector>

namespace pd {

namespace {
std::string narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::string desktopName(HDESK d) {
	wchar_t name[256] = {};
	DWORD needed = 0;
	if (!GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &needed))
		return {};
	return narrow(name);
}
} // namespace

bool isLocalSystem() {
	static const bool result = [] {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
			return false;
		alignas(TOKEN_USER) BYTE buf[256];
		DWORD len = 0;
		bool system = false;
		if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len))
			system = IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buf)->User.Sid, WinLocalSystemSid);
		CloseHandle(token);
		return system;
	}();
	return result;
}

std::string syncThreadDesktop() {
	HDESK input = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
	if (!input)
		return {};
	std::string name = desktopName(input);
	const std::string current = desktopName(GetThreadDesktop(GetCurrentThreadId()));
	if (name != current) {
		if (!SetThreadDesktop(input)) {
			CloseDesktop(input);
			return {};
		}
		// 各线程都会切，只在桌面变了的时候记一次
		static std::mutex m;
		static std::string lastLogged;
		std::lock_guard lock(m);
		if (lastLogged != name) {
			log("输入桌面切换到 {}", name);
			lastLogged = name;
		}
	}
	// 线程还在用这个桌面时 CloseDesktop 会失败，不影响（Sunshine 也是这样）
	CloseDesktop(input);
	return name;
}

void openAsUser(const std::wstring &target) {
	if (!isLocalSystem()) {
		ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	// 用程序自己所在的会话（可能是控制台，也可能是远程桌面会话）
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	HANDLE user = nullptr;
	if (!WTSQueryUserToken(session, &user)) {
		log("没有已登录的用户，不打开 {}", narrow(target.c_str()));
		return;
	}
	void *env = nullptr;
	CreateEnvironmentBlock(&env, user, FALSE);
	wchar_t windows[MAX_PATH];
	GetWindowsDirectoryW(windows, MAX_PATH);
	const std::wstring exe = std::wstring(windows) + L"\\explorer.exe";
	// explorer.exe 打开网址时交给默认浏览器，打开文件夹时显示文件夹
	std::wstring cmd = L"\"" + exe + L"\" \"" + target + L"\"";
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	STARTUPINFOW si{sizeof(si)};
	si.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
	PROCESS_INFORMATION pi{};
	if (CreateProcessAsUserW(user, exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
	                         CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		log("以用户身份打开 {} 失败（错误 {}）", narrow(target.c_str()), GetLastError());
	}
	if (env)
		DestroyEnvironmentBlock(env);
	CloseHandle(user);
}

} // namespace pd
